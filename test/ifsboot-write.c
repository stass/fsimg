/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "fsimg.h"
#include "internal.h"
#include "layer.h"
#include "test.h"

/*
 * The QNX boot image (ifsboot) writer: a create of ifs/ifsboot and a wrap
 * of a standalone ImageFS, both through the public API, each written image
 * checked byte by byte by check_boot() below, which decodes the profile's
 * layout from specs/design-layers.md with this file's own header offsets,
 * word sums and LZO1X decoder and shares no code with lib/layers/.
 * The library reads every image back only as a second opinion.
 *
 * The ImageFS bodies come from the library's own ImageFS writer, fed by a
 * provider of deterministic bytes, so a create and a wrap of the same
 * volume can be compared. Refusals are each checked against a sink that
 * records every write and resize, so "before any sink opens" is observed,
 * not assumed. The machine table makes machine 0 writable under both modes
 * and machine 3 under none only; tests of a mode it does not list run
 * under the private test control that lets the plan accept every mode its
 * tables can describe.
 *
 * --native compares rebuilds with the native evidence the environment
 * names (FSIMG_IFS_NATIVE, FSIMG_IFS_IMAGES), printing counts only.
 * --rebuild IN OUT none|lzo writes one rebuild of a native boot image for
 * a boot trial; it is a test hook, not an interface.
 */

#define KIB		UINT64_C(1024)
#define MIB		(KIB * 1024)
#define EPOCH		INT64_C(1700000000)
#define BIOS_PREFIX	1088u
#define LOADER_BYTES	1085u	/* E13's loader, padded to the prefix */
#define HEADER_BYTES	256u
#define UNIT		65536u
#define CODE_BYTES	4001u
#define SIGNATURE	UINT32_C(0x00ff7eeb)

/* The startup header's fields, from B, as the profile's table gives them. */
enum {
	H_SIGNATURE = 0x00,
	H_VERSION = 0x04,
	H_FLAGS1 = 0x06,
	H_FLAGS2 = 0x07,
	H_HEADER_SIZE = 0x08,
	H_MACHINE = 0x0a,
	H_STARTUP_VADDR = 0x0c,
	H_PADDR_BIAS = 0x10,
	H_IMAGE_PADDR = 0x14,
	H_RAM_PADDR = 0x18,
	H_RAM_SIZE = 0x1c,
	H_STARTUP_SIZE = 0x20,
	H_STORED_SIZE = 0x24,
	H_IMAGEFS_PADDR = 0x28,
	H_IMAGEFS_SIZE = 0x2c,
	H_PREBOOT_SIZE = 0x30,
	H_TAIL = 0x32
};

/*
 * A growable object in memory, as an input, a file setting or a sink.
 * writes and resizes record what the library did to it as a sink. As an
 * input it can misbehave: cap limits each transfer, the fail_read-th read
 * call fails with EIO, and once a read has covered the watched offset
 * switch_at times, every later read serves later instead of bytes, an
 * input changing under its reader. size_claim, when nonzero, is the length
 * get_size reports instead of the real one.
 */
typedef struct memory {
	unsigned char	*bytes;
	uint64_t	 length;
	const unsigned char *later;
	uint64_t	 watch;
	unsigned	 watched;
	unsigned	 switch_at;
	uint64_t	 reads;
	uint64_t	 fail_read;
	uint64_t	 size_claim;
	size_t		 cap;
	unsigned	 writes;
	unsigned	 resizes;
} memory_t;

/* One context with a private directory for scratch and host files. */
typedef struct fixture {
	char		 directory[256];
	int		 directory_fd;
	fsimg_ctx_t	*ctx;
	uint64_t	 baseline_memory;
} fixture_t;

/* The ImageFS a test packages: a directory, a short text file and a file
 * of random_bytes seeded bytes that no LZO1X stream can shorten.
 */
typedef struct volume_spec {
	uint64_t	 random_bytes;
	uint32_t	 seed;
	fsimg_byte_order_t order;
} volume_spec_t;

/* What a written image must hold: the settings, the prefix and the startup
 * code as given (before padding), and the disk.
 */
typedef struct boot_expect {
	const fsimg_layer_options_t *layers;
	const unsigned char *prefix;
	size_t		 prefix_bytes;
	const unsigned char *code;
	size_t		 code_bytes;
	const unsigned char *disk;
	size_t		 disk_bytes;
} boot_expect_t;

/* How a host file setting changes between the plan and the copy. */
typedef enum change {
	CHANGE_TRUNCATE,
	CHANGE_GROW,
	CHANGE_REWRITE
} change_t;

/*
 * A native boot image taken apart: the prefix, startup and disk sections
 * unwrap returns and the typed settings info reports, from which a wrap
 * rebuilds it, and three facts read off the native header: the prefix
 * length B, startup_size, and whether its body is raw. raw_body records
 * the native image, while layers.ifsboot_compress is the mode to write,
 * which a caller may change.
 */
typedef struct unwrapped {
	fsimg_layer_options_t layers;
	memory_t	 prefix;
	memory_t	 code;
	memory_t	 disk;
	size_t		 prefix_bytes;
	size_t		 startup_size;
	bool		 raw_body;
} unwrapped_t;

/* What the checker found in a framed body. */
typedef struct boot_check {
	unsigned	 chunks;
	unsigned	 splits;	/* Units stored as two halves */
	bool		 last_split;	/* The final unit was one of them */
} boot_check_t;

static uint16_t get_le16(const unsigned char *);
static uint32_t get_le32(const unsigned char *);
static void put_le32(unsigned char *, uint32_t);
static uint32_t word_sum(const unsigned char *, size_t);
static bool all_zero(const unsigned char *, size_t);
static unsigned char random_byte(uint32_t, uint64_t);
static void fill_random(unsigned char *, size_t, uint32_t);
static uint64_t digest(const unsigned char *, size_t);
static bool lzo_extend(const unsigned char *, size_t, size_t *, size_t,
    size_t *);
static bool lzo_literals(const unsigned char *, size_t, size_t *,
    unsigned char *, size_t, size_t *, size_t);
static size_t lzo_decode(const unsigned char *, size_t, unsigned char *,
    size_t);
static bool read_chunk(const unsigned char *, size_t, size_t *,
    unsigned char *, size_t *);
static void check_framing(const unsigned char *, size_t,
    const unsigned char *, size_t, boot_check_t *);
static boot_check_t check_boot(const unsigned char *, size_t,
    const boot_expect_t *);
static fsimg_status_t memory_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static fsimg_status_t memory_write(void *, uint64_t, const void *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t memory_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t memory_resize(void *, uint64_t, fsimg_error_t *);
static fsimg_status_t open_memory(fixture_t *, memory_t *, fsimg_io_t **,
    fsimg_error_t *);
static fsimg_io_t *memory_io(fixture_t *, memory_t *);
static memory_t memory_of(const unsigned char *, size_t);
static void memory_free(memory_t *);
static void fixture_init(fixture_t *, uint64_t);
static void fixture_free(fixture_t *);
static int raw_file(fixture_t *);
static int host_file(fixture_t *, const char *, const unsigned char *,
    size_t);
static fsimg_io_t *host_io(fixture_t *, const char *);
static void host_remove(fixture_t *, const char *);
static fsimg_status_t provider_validate(void *, uint64_t, fsimg_error_t *);
static fsimg_status_t provider_read(void *, uint64_t, uint64_t, void *,
    size_t, size_t *, fsimg_error_t *);
static void write_options(fsimg_write_options_t *,
    const fsimg_layer_options_t *, const fsimg_layer_inputs_t *,
    fsimg_byte_order_t);
static fsimg_status_t add_entries(fsimg_writer_t *, const volume_spec_t *,
    fsimg_error_t *);
static void build_volume(fixture_t *, const volume_spec_t *, memory_t *);
static fsimg_status_t create_boot(fixture_t *, const volume_spec_t *,
    const fsimg_layer_options_t *, const fsimg_layer_inputs_t *, memory_t *,
    fsimg_error_t *);
static fsimg_status_t wrap_boot(fixture_t *, fsimg_io_t *,
    const fsimg_layer_options_t *, const fsimg_layer_inputs_t *, memory_t *,
    fsimg_error_t *);
static fsimg_status_t wrap_memory(fixture_t *, memory_t *,
    const fsimg_layer_options_t *, const fsimg_layer_inputs_t *, memory_t *,
    fsimg_error_t *);
static void boot_layers(fsimg_layer_options_t *, uint32_t,
    fsimg_ifsboot_compression_t);
static fsimg_image_t *open_image(fixture_t *, memory_t *, fsimg_io_t **);
static uint64_t detail(const fsimg_image_info_t *, const char *);
static void unwrap(fixture_t *, fsimg_image_t *, fsimg_selector_kind_t,
    const char *, fsimg_status_t, memory_t *);
static void expect_error(fsimg_status_t, fsimg_status_t, const fsimg_error_t *,
    const char *, const char *);
static void expect_untouched(const memory_t *);
static uint64_t memory_now(fixture_t *);
static void expect_disk(fixture_t *, memory_t *, const memory_t *);
static fsimg_status_t construct_plan(fixture_t *, uint32_t,
    fsimg_ifsboot_compression_t, memory_t *, memory_t *, fsimg_session_t *,
    fsimg_container_plan_t **, fsimg_error_t *);
static void free_plan(fsimg_container_plan_t *);
static fsimg_status_t plan_prefix(fixture_t *, memory_t *, bool, uint64_t,
    fsimg_session_t *, fsimg_container_plan_t **, fsimg_error_t *);
static void expect_wrap_refused(fixture_t *, memory_t *,
    const fsimg_layer_options_t *, const fsimg_layer_inputs_t *,
    fsimg_status_t, const char *, const char *);
static fsimg_status_t create_changed(fixture_t *, const volume_spec_t *,
    const fsimg_layer_options_t *, const fsimg_layer_inputs_t *, int, change_t,
    memory_t *, fsimg_error_t *);
static void test_round_trips(void);
static void test_startup_lengths(void);
static void test_prefix(void);
static void test_prefix_buffer(void);
static void test_plan_session(void);
static void test_splits(void);
static void test_required_settings(void);
static void test_refusals(void);
static void test_byte_order_and_inputs(void);
static void test_bounds(void);
static void test_sections(void);
static void test_aliases(void);
static void test_borrowed_settings(void);
static void test_changed_settings(void);
static void test_changed_body(void);
static void test_emission_fault(void);
static void test_allocation_failures(void);
static void test_short_io(void);
static void test_reproducible(void);
static void test_altered_address(void);
static unsigned char *read_whole(const char *, size_t *);
static unsigned char *read_gzip_image(const char *, size_t *);
static bool table_writes(uint32_t, fsimg_ifsboot_compression_t);
static bool bare_header(const unsigned char *, size_t, size_t *);
static bool unwrap_native(fixture_t *, const unsigned char *, size_t,
    unwrapped_t *);
static fsimg_status_t wrap_unwrapped(fixture_t *, unwrapped_t *, memory_t *,
    fsimg_error_t *);
static void unwrapped_free(unwrapped_t *);
static void rebuild_native(const char *, const unsigned char *, size_t);
static bool write_new_file(const char *, const unsigned char *, size_t);
static int rebuild_file(const char *, const char *, const char *);
static int native(void);

static uint16_t
get_le16(const unsigned char *p)
{
	return ((uint16_t)(p[0] | p[1] << 8));
}

static uint32_t
get_le32(const unsigned char *p)
{
	return ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	    (uint32_t)p[3] << 24);
}

static void
put_le32(unsigned char *p, uint32_t value)
{
	for (unsigned i = 0; i < 4; i++)
		p[i] = (unsigned char)(value >> (8 * i));
}

/* The little-endian words of a whole number of words, summed mod 2^32. */
static uint32_t
word_sum(const unsigned char *bytes, size_t length)
{
	uint32_t sum = 0;

	CHECK(length % 4 == 0);
	for (size_t i = 0; i < length; i += 4)
		sum += get_le32(bytes + i);
	return (sum);
}

static bool
all_zero(const unsigned char *bytes, size_t length)
{
	for (size_t i = 0; i < length; i++) {
		if (bytes[i] != 0)
			return (false);
	}
	return (true);
}

/* A stateless byte stream: byte position of the stream seed. */
static unsigned char
random_byte(uint32_t seed, uint64_t position)
{
	uint64_t x = position * UINT64_C(0x9e3779b97f4a7c15) +
	    (uint64_t)seed * UINT64_C(0xd1b54a32d192ed03) + 1;

	x ^= x >> 31;
	x *= UINT64_C(0xbf58476d1ce4e5b9);
	x ^= x >> 29;
	x *= UINT64_C(0x94d049bb133111eb);
	return ((unsigned char)(x >> 32));
}

static void
fill_random(unsigned char *bytes, size_t length, uint32_t seed)
{
	for (size_t i = 0; i < length; i++)
		bytes[i] = random_byte(seed, i);
}

/* FNV-1a, 64 bits: a pinned digest of a whole output. */
static uint64_t
digest(const unsigned char *bytes, size_t length)
{
	uint64_t h = UINT64_C(0xcbf29ce484222325);

	for (size_t i = 0; i < length; i++)
		h = (h ^ bytes[i]) * UINT64_C(0x100000001b3);
	return (h);
}

/*
 * An LZO1X length extension: zero bytes each add 255 and the first nonzero
 * byte ends it, all added to base.
 */
static bool
lzo_extend(const unsigned char *in, size_t in_len, size_t *ip, size_t base,
    size_t *out)
{
	size_t added = 0;

	while (*ip < in_len && in[*ip] == 0) {
		added += 255;
		(*ip)++;
	}
	if (*ip == in_len)
		return (false);
	*out = base + added + in[(*ip)++];
	return (true);
}

/* Copy count literal bytes from the stream to the output, within both. */
static bool
lzo_literals(const unsigned char *in, size_t in_len, size_t *ip,
    unsigned char *out, size_t capacity, size_t *op, size_t count)
{
	if (in_len - *ip < count || capacity - *op < count)
		return (false);
	bcopy(in + *ip, out + *op, count);
	*ip += count;
	*op += count;
	return (true);
}

/*
 * An LZO1X decoder written from the format's instruction set: the decoded
 * length, or SIZE_MAX when the stream is malformed, would pass capacity,
 * or does not end exactly at its last byte with the end marker. state is
 * the literals the last instruction copied: 0, 1 to 3 after a match, or 4
 * after a literal run, which chooses what an instruction below 16 means.
 */
static size_t
lzo_decode(const unsigned char *in, size_t in_len, unsigned char *out,
    size_t capacity)
{
	size_t ip = 0, op = 0, length, distance;
	unsigned state = 0, next, t;

	if (in_len == 0)
		return (SIZE_MAX);
	if (in[0] > 17) {
		length = (size_t)in[ip++] - 17;
		if (!lzo_literals(in, in_len, &ip, out, capacity, &op, length))
			return (SIZE_MAX);
		state = length < 4 ? (unsigned)length : 4;
	}
	for (;;) {
		if (ip == in_len)
			return (SIZE_MAX);
		t = in[ip++];
		if (t < 16 && state == 0) {
			length = t;
			if (t == 0 && !lzo_extend(in, in_len, &ip, 15, &length))
				return (SIZE_MAX);
			if (!lzo_literals(in, in_len, &ip, out, capacity, &op,
			    length + 3))
				return (SIZE_MAX);
			state = 4;
			continue;
		}
		if (t < 16) {
			/* A short match: two bytes after a match's literals,
			 * three after a literal run, from further back. */
			if (ip == in_len)
				return (SIZE_MAX);
			distance = 1 + (t >> 2) + ((size_t)in[ip++] << 2) +
			    (state == 4 ? 2048 : 0);
			length = state == 4 ? 3 : 2;
			next = t & 3;
		} else if (t >= 64) {
			if (ip == in_len)
				return (SIZE_MAX);
			distance = 1 + ((t >> 2) & 7) + ((size_t)in[ip++] << 3);
			length = (t >> 5) + 1;
			next = t & 3;
		} else if (t >= 32) {
			length = t & 31;
			if (length == 0 && !lzo_extend(in, in_len, &ip, 31, &length))
				return (SIZE_MAX);
			length += 2;
			if (in_len - ip < 2)
				return (SIZE_MAX);
			distance = 1 + (in[ip] >> 2) + ((size_t)in[ip + 1] << 6);
			next = in[ip] & 3;
			ip += 2;
		} else {
			length = t & 7;
			if (length == 0 && !lzo_extend(in, in_len, &ip, 7, &length))
				return (SIZE_MAX);
			length += 2;
			if (in_len - ip < 2)
				return (SIZE_MAX);
			distance = ((size_t)(t & 8) << 11) + (in[ip] >> 2) +
			    ((size_t)in[ip + 1] << 6);
			next = in[ip] & 3;
			ip += 2;
			/* The end marker, 0x11 0x00 0x00, ends the stream. */
			if (distance == 0)
				return (length == 3 && ip == in_len ? op : SIZE_MAX);
			distance += 16384;
		}
		if (distance > op || capacity - op < length)
			return (SIZE_MAX);
		for (size_t i = 0; i < length; i++, op++)
			out[op] = out[op - distance];
		if (!lzo_literals(in, in_len, &ip, out, capacity, &op, next))
			return (SIZE_MAX);
		state = next;
	}
}

/*
 * The next chunk of a framed body at *at: its big-endian length word and
 * stream, decoded into out, *decoded its length. False at the zero
 * terminator. Every word and stream lies before the trailer.
 */
static bool
read_chunk(const unsigned char *body, size_t body_bytes, size_t *at,
    unsigned char *out, size_t *decoded)
{
	size_t stored;

	CHECK(body_bytes >= 4 && *at + 2 <= body_bytes - 4);
	stored = (size_t)body[*at] << 8 | body[*at + 1];
	*at += 2;
	if (stored == 0)
		return (false);
	CHECK(stored <= body_bytes - 4 - *at);
	*decoded = lzo_decode(body + *at, stored, out, UNIT);
	CHECK(*decoded != SIZE_MAX);
	*at += stored;
	return (true);
}

/*
 * A framed body: chunks of 65536 decoded bytes with a shorter last one,
 * each unit either one chunk or, where its stream would pass a length word,
 * two chunks of ceil(n / 2) and floor(n / 2) bytes in that order; together
 * they decode to the disk. Then the zero terminator, zero padding to a word
 * and the trailer, the body's last word.
 */
static void
check_framing(const unsigned char *body, size_t body_bytes,
    const unsigned char *disk, size_t disk_bytes, boot_check_t *check)
{
	unsigned char *out;
	size_t at = 0, decoded = 0, unit, n;

	out = malloc(UNIT);
	CHECK(out != NULL);
	while (read_chunk(body, body_bytes, &at, out, &n)) {
		unit = disk_bytes - decoded < UNIT ? disk_bytes - decoded : UNIT;
		CHECK(unit > 0 && n <= unit);
		CHECK(bcmp(out, disk + decoded, n) == 0);
		check->chunks++;
		if (n != unit) {
			CHECK(n == unit - unit / 2);
			CHECK(read_chunk(body, body_bytes, &at, out, &n));
			CHECK(n == unit / 2 &&
			    bcmp(out, disk + decoded + unit - unit / 2, n) == 0);
			check->chunks++;
			check->splits++;
			check->last_split = decoded + unit == disk_bytes;
		}
		decoded += unit;
	}
	CHECK(decoded == disk_bytes);
	CHECK(((at + 3) & ~(size_t)3) == body_bytes - 4);
	CHECK(all_zero(body + at, body_bytes - 4 - at));
	free(out);
}

/*
 * Check every byte of a written boot image against what the profile says
 * it holds: the prefix and its zero padding, the header field by field,
 * a zero flags2 and tail, the code and its zero padding, the startup
 * checksum word, the body raw or framed, and both sums.
 */
static boot_check_t
check_boot(const unsigned char *file, size_t length, const boot_expect_t *expect)
{
	const fsimg_layer_options_t *layers = expect->layers;
	bool lzo = layers->ifsboot_compress == FSIMG_IFSBOOT_LZO;
	size_t b = expect->prefix_bytes == 0 ? 0 : BIOS_PREFIX;
	size_t padded = (expect->code_bytes + 3) & ~(size_t)3;
	size_t startup = HEADER_BYTES + padded + 4;
	const unsigned char *h = file + b;
	boot_check_t check;

	bzero(&check, sizeof(check));
	CHECK(length >= b + startup && (length - b) % 4 == 0);
	CHECK(expect->prefix_bytes == 0 ||
	    bcmp(file, expect->prefix, expect->prefix_bytes) == 0);
	CHECK(all_zero(file + expect->prefix_bytes, b - expect->prefix_bytes));

	CHECK(get_le32(h + H_SIGNATURE) == SIGNATURE);
	CHECK(get_le16(h + H_VERSION) == 1);
	CHECK(h[H_FLAGS1] == ((layers->ifsboot_virtual ? 0x01 : 0x00) |
	    (lzo ? 0x08 : 0x00)));
	CHECK(h[H_FLAGS2] == 0);
	CHECK(get_le16(h + H_HEADER_SIZE) == HEADER_BYTES);
	CHECK(get_le16(h + H_MACHINE) == layers->ifsboot_machine);
	CHECK(get_le32(h + H_STARTUP_VADDR) == layers->ifsboot_startup_vaddr);
	CHECK(get_le32(h + H_PADDR_BIAS) == layers->ifsboot_paddr_bias);
	CHECK(get_le32(h + H_IMAGE_PADDR) == layers->ifsboot_image_paddr);
	CHECK(get_le32(h + H_RAM_PADDR) == layers->ifsboot_ram_paddr);
	/* mkifs writes ram_size 0 for machine 0 (E7), the sum for a boot
	 * machine. */
	CHECK(get_le32(h + H_RAM_SIZE) == (layers->ifsboot_machine == 0 ? 0 :
	    startup + expect->disk_bytes));
	CHECK(get_le32(h + H_STARTUP_SIZE) == startup);
	CHECK(get_le32(h + H_STORED_SIZE) == length - b);
	CHECK(get_le32(h + H_IMAGEFS_PADDR) == layers->ifsboot_imagefs_paddr);
	CHECK(get_le32(h + H_IMAGEFS_SIZE) == expect->disk_bytes);
	CHECK(get_le16(h + H_PREBOOT_SIZE) == b);
	CHECK(all_zero(h + H_TAIL, HEADER_BYTES - H_TAIL));

	CHECK(expect->code_bytes == 0 ||
	    bcmp(h + HEADER_BYTES, expect->code, expect->code_bytes) == 0);
	CHECK(all_zero(h + HEADER_BYTES + expect->code_bytes,
	    padded - expect->code_bytes));
	CHECK(word_sum(h, startup) == 0);
	CHECK(word_sum(h, length - b) == 0);

	if (!lzo) {
		CHECK(length - b - startup == expect->disk_bytes);
		CHECK(bcmp(h + startup, expect->disk, expect->disk_bytes) == 0);
		check.chunks = 1;
		return (check);
	}
	check_framing(h + startup, length - b - startup, expect->disk,
	    expect->disk_bytes, &check);
	return (check);
}

static fsimg_status_t
memory_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error)
{
	memory_t *memory = argument;
	const unsigned char *source = memory->bytes;

	memory->reads++;
	if (memory->fail_read != 0 && memory->reads == memory->fail_read) {
		bzero(error, sizeof(*error));
		error->status = FSIMG_EIO;
		error->os_error = EIO;
		error->operation = FSIMG_OP_IO;
		snprintf(error->message, sizeof(error->message),
		    "injected read failure");
		return (FSIMG_EIO);
	}
	if (offset >= memory->length) {
		*transferred = 0;
		return (FSIMG_OK);
	}
	if (length > memory->length - offset)
		length = (size_t)(memory->length - offset);
	if (memory->cap != 0 && length > memory->cap)
		length = memory->cap;
	if (memory->switch_at != 0 && memory->watched >= memory->switch_at)
		source = memory->later;
	else if (memory->switch_at != 0 && offset <= memory->watch &&
	    memory->watch - offset < length)
		memory->watched++;
	bcopy(source + offset, buffer, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_write(void *argument, uint64_t offset, const void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_t *memory = argument;

	CHECK(offset <= memory->length && length <= memory->length - offset);
	bcopy(buffer, memory->bytes + offset, length);
	memory->writes++;
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_size(void *argument, uint64_t *size, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_t *memory = argument;

	*size = memory->size_claim != 0 ? memory->size_claim : memory->length;
	return (FSIMG_OK);
}

/* Growth reads as 0xa5, so only bytes the library writes read as zero. */
static fsimg_status_t
memory_resize(void *argument, uint64_t length, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_t *memory = argument;
	unsigned char *bytes;

	bytes = realloc(memory->bytes, length == 0 ? 1 : (size_t)length);
	CHECK(bytes != NULL);
	if (length > memory->length)
		memset(bytes + memory->length, 0xa5,
		    (size_t)(length - memory->length));
	memory->bytes = bytes;
	memory->length = length;
	memory->resizes++;
	return (FSIMG_OK);
}

/* An object over memory; only an injected allocation failure fails it. */
static fsimg_status_t
open_memory(fixture_t *f, memory_t *memory, fsimg_io_t **io,
    fsimg_error_t *error)
{
	fsimg_io_ops_t ops;

	fsimg_io_ops_init(&ops);
	ops.read_at = memory_read;
	ops.write_at = memory_write;
	ops.get_size = memory_size;
	ops.resize = memory_resize;
	return (fsimg_io_from_callbacks(f->ctx, &ops, memory, io, error));
}

static fsimg_io_t *
memory_io(fixture_t *f, memory_t *memory)
{
	fsimg_io_t *io;

	CHECK(open_memory(f, memory, &io, NULL) == FSIMG_OK);
	return (io);
}

/* A memory object holding a copy of bytes. */
static memory_t
memory_of(const unsigned char *bytes, size_t length)
{
	memory_t memory;

	bzero(&memory, sizeof(memory));
	memory.bytes = malloc(length == 0 ? 1 : length);
	CHECK(memory.bytes != NULL);
	if (length != 0)
		bcopy(bytes, memory.bytes, length);
	memory.length = length;
	return (memory);
}

static void
memory_free(memory_t *memory)
{
	free(memory->bytes);
	bzero(memory, sizeof(memory_t));
}

static void
fixture_init(fixture_t *f, uint64_t memory)
{
	fsimg_options_t options;
	fsimg_stats_t stats;

	bzero(f, sizeof(*f));
	snprintf(f->directory, sizeof(f->directory),
	    "%s/fsimg-ifsboot-write.XXXXXX",
	    getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp");
	CHECK(mkdtemp(f->directory) != NULL);
	f->directory_fd = open(f->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(f->directory_fd != -1);
	fsimg_options_init(&options);
	options.memory_bytes = memory;
	options.scratch_directory_fd = f->directory_fd;
	options.max_open_files = 64;
	CHECK(fsimg_ctx_new(&options, &f->ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	f->baseline_memory = stats.memory_bytes;
}

/* Every charge is returned, and nothing is left in the directory. */
static void
fixture_free(fixture_t *f)
{
	fsimg_stats_t stats;
	struct dirent *entry;
	DIR *directory;

	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == f->baseline_memory);
	fsimg_ctx_free(f->ctx);
	directory = fdopendir(f->directory_fd);
	CHECK(directory != NULL);
	while ((entry = readdir(directory)) != NULL)
		CHECK(strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0);
	CHECK(closedir(directory) == 0);
	CHECK(rmdir(f->directory) == 0);
}

/* A writer's raw intermediate: an exclusively created, unlinked file. */
static int
raw_file(fixture_t *f)
{
	int fd;

	fd = openat(f->directory_fd, "raw", O_RDWR | O_CREAT | O_EXCL |
	    O_CLOEXEC, 0600);
	CHECK(fd != -1);
	CHECK(unlinkat(f->directory_fd, "raw", 0) == 0);
	return (fd);
}

/* Create or replace the host file name with bytes; its descriptor, open
 * for writing, which the caller closes.
 */
static int
host_file(fixture_t *f, const char *name, const unsigned char *bytes,
    size_t length)
{
	int fd;

	fd = openat(f->directory_fd, name, O_RDWR | O_CREAT | O_TRUNC |
	    O_CLOEXEC, 0600);
	CHECK(fd != -1);
	CHECK(length == 0 || pwrite(fd, bytes, length, 0) == (ssize_t)length);
	return (fd);
}

/* A read-only descriptor adapter over the host file name. */
static fsimg_io_t *
host_io(fixture_t *f, const char *name)
{
	fsimg_io_t *io;
	int fd;

	fd = openat(f->directory_fd, name, O_RDONLY | O_CLOEXEC);
	CHECK(fd != -1);
	CHECK(fsimg_io_from_fd(f->ctx, fd, FSIMG_IO_READ, &io, NULL) ==
	    FSIMG_OK);
	CHECK(close(fd) == 0);
	return (io);
}

static void
host_remove(fixture_t *f, const char *name)
{
	CHECK(unlinkat(f->directory_fd, name, 0) == 0);
}

static fsimg_status_t
provider_validate(void *argument FSIMG_UNUSED, uint64_t id FSIMG_UNUSED,
    fsimg_error_t *error FSIMG_UNUSED)
{
	return (FSIMG_OK);
}

/* Source 1 is the text file, source 2 the random one. */
static fsimg_status_t
provider_read(void *argument, uint64_t id, uint64_t offset, void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	static const char text[] = "hello, boot\n";
	const volume_spec_t *spec = argument;
	unsigned char *bytes = buffer;

	for (size_t i = 0; i < length; i++)
		bytes[i] = id == 1 ?
		    (unsigned char)text[(offset + i) % (sizeof(text) - 1)] :
		    random_byte(spec->seed, offset + i);
	*transferred = length;
	return (FSIMG_OK);
}

static void
write_options(fsimg_write_options_t *options,
    const fsimg_layer_options_t *layers, const fsimg_layer_inputs_t *inputs,
    fsimg_byte_order_t order)
{
	fsimg_write_options_init(options);
	options->flags = FSIMG_WRITE_REPRODUCIBLE | FSIMG_WRITE_ALLOW_LOSS;
	options->epoch = EPOCH;
	options->layers = layers;
	options->inputs = inputs;
	options->byte_order = order;
}

/* The volume's entries, through the writer's provider. */
static fsimg_status_t
add_entries(fsimg_writer_t *writer, const volume_spec_t *spec,
    fsimg_error_t *error)
{
	fsimg_source_ops_t ops;
	fsimg_entry_t entry;
	fsimg_source_t source;
	fsimg_status_t status;

	fsimg_source_ops_init(&ops);
	ops.validate = provider_validate;
	ops.read_at = provider_read;
	status = fsimg_writer_set_sources(writer, &ops, (void *)spec, error);
	if (status != FSIMG_OK)
		return (status);
	bzero(&entry, sizeof(entry));
	entry.path = "/etc";
	entry.kind = FSIMG_ENTRY_DIRECTORY;
	status = fsimg_writer_add(writer, &entry, NULL, error);
	if (status != FSIMG_OK)
		return (status);
	entry.path = "/etc/motd";
	entry.kind = FSIMG_ENTRY_FILE;
	entry.size = 12;
	source = (fsimg_source_t){ .kind = FSIMG_SOURCE_PROVIDER, .id = 1 };
	status = fsimg_writer_add(writer, &entry, &source, error);
	if (status != FSIMG_OK || spec->random_bytes == 0)
		return (status);
	entry.path = "/data.bin";
	entry.size = spec->random_bytes;
	source.id = 2;
	return (fsimg_writer_add(writer, &entry, &source, error));
}

/* The bare ImageFS of a spec, as the ifs writer emits it. */
static void
build_volume(fixture_t *f, const volume_spec_t *spec, memory_t *out)
{
	fsimg_write_options_t options;
	fsimg_writer_t *writer;
	fsimg_io_t *io;

	bzero(out, sizeof(*out));
	write_options(&options, NULL, NULL, spec->order);
	CHECK(fsimg_writer_new(f->ctx, "ifs", &options, &writer, NULL) ==
	    FSIMG_OK);
	CHECK(add_entries(writer, spec, NULL) == FSIMG_OK);
	io = memory_io(f, out);
	CHECK(fsimg_writer_emit(writer, io, NULL) == FSIMG_OK);
	fsimg_io_free(io);
	fsimg_writer_free(writer);
	CHECK(out->length % 4 == 0);
}

/* A create of ifs/ifsboot into out through the writer's raw intermediate. */
static fsimg_status_t
create_boot(fixture_t *f, const volume_spec_t *spec,
    const fsimg_layer_options_t *layers, const fsimg_layer_inputs_t *inputs,
    memory_t *out, fsimg_error_t *error)
{
	fsimg_write_options_t options;
	fsimg_writer_t *writer;
	fsimg_sink_t sink;
	fsimg_status_t status;

	bzero(out, sizeof(*out));
	write_options(&options, layers, inputs, spec->order);
	status = fsimg_writer_new(f->ctx, "ifs/ifsboot", &options, &writer,
	    error);
	if (status != FSIMG_OK)
		return (status);
	status = add_entries(writer, spec, error);
	fsimg_sink_init(&sink, FSIMG_SINK_IO);
	if (status == FSIMG_OK)
		status = open_memory(f, out, &sink.io, error);
	if (status == FSIMG_OK) {
		sink.raw_fd = raw_file(f);
		status = fsimg_writer_output(writer, &sink, error);
		fsimg_io_free(sink.io);
	}
	fsimg_writer_free(writer);
	return (status);
}

/* A wrap of input into ifs/ifsboot, written into out. */
static fsimg_status_t
wrap_boot(fixture_t *f, fsimg_io_t *input, const fsimg_layer_options_t *layers,
    const fsimg_layer_inputs_t *inputs, memory_t *out, fsimg_error_t *error)
{
	fsimg_write_options_t options;
	fsimg_sink_t sink;
	fsimg_status_t status;

	write_options(&options, layers, inputs, FSIMG_BYTE_ORDER_LITTLE);
	fsimg_sink_init(&sink, FSIMG_SINK_IO);
	status = open_memory(f, out, &sink.io, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_image_wrap(f->ctx, input, "ifs/ifsboot", &options, &sink,
	    error);
	fsimg_io_free(sink.io);
	return (status);
}

static fsimg_status_t
wrap_memory(fixture_t *f, memory_t *volume, const fsimg_layer_options_t *layers,
    const fsimg_layer_inputs_t *inputs, memory_t *out, fsimg_error_t *error)
{
	fsimg_io_t *input;
	fsimg_status_t status;

	status = open_memory(f, volume, &input, error);
	if (status != FSIMG_OK)
		return (status);
	status = wrap_boot(f, input, layers, inputs, out, error);
	fsimg_io_free(input);
	return (status);
}

/* Every required setting, with a value of each kind. */
static void
boot_layers(fsimg_layer_options_t *layers, uint32_t machine,
    fsimg_ifsboot_compression_t mode)
{
	fsimg_layer_options_init(layers);
	layers->ifsboot_machine = machine;
	layers->ifsboot_machine_set = true;
	layers->ifsboot_virtual = true;
	layers->ifsboot_virtual_set = true;
	layers->ifsboot_startup_vaddr = 0x00401234;
	layers->ifsboot_startup_vaddr_set = true;
	layers->ifsboot_paddr_bias = 0;
	layers->ifsboot_paddr_bias_set = true;
	layers->ifsboot_image_paddr = 0x00400440;
	layers->ifsboot_image_paddr_set = true;
	layers->ifsboot_ram_paddr = 0x00400000;
	layers->ifsboot_ram_paddr_set = true;
	layers->ifsboot_imagefs_paddr = 0xfffffffc;
	layers->ifsboot_imagefs_paddr_set = true;
	layers->ifsboot_compress = mode;
}

/* Open an image of memory automatically, with a spool so that a spooled
 * container such as hbcifs opens too; its input stays alive in *io.
 */
static fsimg_image_t *
open_image(fixture_t *f, memory_t *memory, fsimg_io_t **io)
{
	fsimg_image_options_t options;
	fsimg_image_t *image;

	*io = memory_io(f, memory);
	fsimg_image_options_init(&options);
	options.spool_fd = openat(f->directory_fd, "spool", O_RDWR | O_CREAT |
	    O_EXCL | O_CLOEXEC, 0600);
	CHECK(options.spool_fd != -1);
	CHECK(unlinkat(f->directory_fd, "spool", 0) == 0);
	CHECK(fsimg_image_open(f->ctx, *io, &options, &image, NULL) == FSIMG_OK);
	return (image);
}

static uint64_t
detail(const fsimg_image_info_t *info, const char *name)
{
	for (uint32_t i = 0; i < info->detail_count; i++) {
		if (strcmp(info->details[i].name, name) == 0)
			return (info->details[i].value);
	}
	CHECK(!"detail reported");
	return (0);
}

/* Unwrap a selector of image into out, which the caller frees. */
static void
unwrap(fixture_t *f, fsimg_image_t *image, fsimg_selector_kind_t kind,
    const char *text, fsimg_status_t expected, memory_t *out)
{
	fsimg_selector_t selector = { .kind = kind, .text = text };
	fsimg_write_options_t options;
	fsimg_sink_t sink;
	fsimg_io_t *io;
	fsimg_error_t error;

	bzero(out, sizeof(*out));
	fsimg_write_options_init(&options);
	fsimg_sink_init(&sink, FSIMG_SINK_IO);
	io = memory_io(f, out);
	sink.io = io;
	CHECK(fsimg_image_unwrap(image, &selector, &options, &sink, &error) ==
	    expected);
	fsimg_io_free(io);
	if (expected == FSIMG_EINVAL && kind == FSIMG_SELECT_SECTION)
		CHECK(strcmp(error.path, text) == 0);
}

/* A call failed with status, naming path, its message holding words. */
static void
expect_error(fsimg_status_t got, fsimg_status_t status,
    const fsimg_error_t *error, const char *path, const char *words)
{
	if (got != status || (path != NULL && strcmp(error->path, path) != 0) ||
	    (words != NULL && strstr(error->message, words) == NULL)) {
		fprintf(stderr, "status %d (wanted %d), path \"%s\", message "
		    "\"%s\"\n", got, status, error->path, error->message);
		CHECK(!"the expected refusal");
	}
}

/* A refused plan wrote nothing and resized nothing. */
static void
expect_untouched(const memory_t *sink)
{
	CHECK(sink->writes == 0 && sink->resizes == 0 && sink->length == 0);
}

/* The context's charged memory now. */
static uint64_t
memory_now(fixture_t *f)
{
	fsimg_stats_t stats;

	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	return (stats.memory_bytes);
}

/* The disk an image peels to equals expected. */
static void
expect_disk(fixture_t *f, memory_t *file, const memory_t *expected)
{
	fsimg_selector_t selector = { .kind = FSIMG_SELECT_DISK };
	fsimg_image_t *image;
	fsimg_io_t *io, *disk;
	unsigned char *bytes;
	uint64_t size;
	size_t transferred;

	image = open_image(f, file, &io);
	CHECK(fsimg_image_select(image, &selector, &disk, NULL) == FSIMG_OK);
	CHECK(fsimg_io_get_size(disk, &size, NULL) == FSIMG_OK);
	CHECK(size == expected->length);
	bytes = malloc((size_t)size + 1);
	CHECK(bytes != NULL);
	CHECK(fsimg_io_read_exact(disk, 0, bytes, (size_t)size, &transferred,
	    NULL) == FSIMG_OK);
	CHECK(bcmp(bytes, expected->bytes, (size_t)size) == 0);
	free(bytes);
	fsimg_io_free(disk);
	fsimg_image_free(image);
	fsimg_io_free(io);
}

/*
 * A create of ifs/ifsboot and a wrap of the same standalone ImageFS write
 * the same bytes, under lzo and under none, each byte as check_boot()
 * says. The image peels back to the volume, verifies with both sums and
 * ram_size, reports its settings' values and zero flags2 and tail_nonzero
 * in info, and unwraps to its sections, from which a wrap with the values
 * info reports rebuilds it byte for byte.
 */
static void
test_round_trips(void)
{
	static const fsimg_ifsboot_compression_t modes[] = { FSIMG_IFSBOOT_LZO,
	    FSIMG_IFSBOOT_NONE };
	volume_spec_t spec = { .random_bytes = 3000, .seed = 1 };
	unsigned char code[CODE_BYTES];
	fsimg_layer_options_t layers, rebuilt;
	fsimg_layer_inputs_t inputs;
	memory_t volume, startup, created, wrapped, prefix, section, disk, again;
	fsimg_layer_report_t report;
	fsimg_image_info_t info;
	fsimg_image_t *image;
	fsimg_io_t *io, *startup_io;
	fixture_t f;

	fill_random(code, sizeof(code), 7);
	for (unsigned m = 0; m < 2; m++) {
		fixture_init(&f, 64 * MIB);
		build_volume(&f, &spec, &volume);
		boot_layers(&layers, 0, modes[m]);
		startup = memory_of(code, sizeof(code));
		startup_io = memory_io(&f, &startup);
		inputs = (fsimg_layer_inputs_t){ .ifsboot_startup = startup_io };
		CHECK(create_boot(&f, &spec, &layers, &inputs, &created, NULL) ==
		    FSIMG_OK);
		check_boot(created.bytes, (size_t)created.length,
		    &(boot_expect_t){ .layers = &layers, .code = code,
		    .code_bytes = sizeof(code), .disk = volume.bytes,
		    .disk_bytes = (size_t)volume.length });
		bzero(&wrapped, sizeof(wrapped));
		CHECK(wrap_memory(&f, &volume, &layers, &inputs, &wrapped, NULL) ==
		    FSIMG_OK);
		CHECK(wrapped.length == created.length &&
		    bcmp(wrapped.bytes, created.bytes, (size_t)created.length) == 0);
		expect_disk(&f, &created, &volume);

		image = open_image(&f, &created, &io);
		CHECK(fsimg_image_get_info(image, &info, NULL) == FSIMG_OK);
		CHECK(strcmp(info.stack, "ifs/ifsboot") == 0);
		CHECK(strcmp(info.codecs, m == 0 ? "lzo1x" : "raw") == 0);
		CHECK(detail(&info, "prefix_bytes") == 0 &&
		    detail(&info, "preboot_size") == 0);
		CHECK(detail(&info, "machine") == 0 && detail(&info, "virtual") == 1);
		CHECK(detail(&info, "compression") == (m == 0 ? 0x08 : 0x00));
		CHECK(detail(&info, "startup_vaddr") == 0x00401234 &&
		    detail(&info, "paddr_bias") == 0 &&
		    detail(&info, "image_paddr") == 0x00400440 &&
		    detail(&info, "ram_paddr") == 0x00400000 &&
		    detail(&info, "imagefs_paddr") == 0xfffffffc);
		CHECK(detail(&info, "startup_size") == HEADER_BYTES + 4004 + 4);
		CHECK(detail(&info, "imagefs_size") == volume.length);
		CHECK(detail(&info, "flags2") == 0 &&
		    detail(&info, "tail_nonzero") == 0);
		CHECK(fsimg_image_verify(image, FSIMG_VERIFY_PAYLOAD, &report,
		    NULL) == FSIMG_OK);

		unwrap(&f, image, FSIMG_SELECT_SECTION, "prefix", FSIMG_OK, &prefix);
		CHECK(prefix.length == 0);
		unwrap(&f, image, FSIMG_SELECT_SECTION, "startup", FSIMG_OK,
		    &section);
		CHECK(section.length == 4004 &&
		    bcmp(section.bytes, code, sizeof(code)) == 0 &&
		    all_zero(section.bytes + sizeof(code), 3));
		unwrap(&f, image, FSIMG_SELECT_DISK, NULL, FSIMG_OK, &disk);
		CHECK(disk.length == volume.length &&
		    bcmp(disk.bytes, volume.bytes, (size_t)volume.length) == 0);

		/* The rebuild takes every typed value from info. */
		boot_layers(&rebuilt, (uint32_t)detail(&info, "machine"),
		    detail(&info, "compression") == 0 ? FSIMG_IFSBOOT_NONE :
		    FSIMG_IFSBOOT_LZO);
		rebuilt.ifsboot_virtual = detail(&info, "virtual") != 0;
		rebuilt.ifsboot_startup_vaddr =
		    (uint32_t)detail(&info, "startup_vaddr");
		rebuilt.ifsboot_paddr_bias = (uint32_t)detail(&info, "paddr_bias");
		rebuilt.ifsboot_image_paddr = (uint32_t)detail(&info, "image_paddr");
		rebuilt.ifsboot_ram_paddr = (uint32_t)detail(&info, "ram_paddr");
		rebuilt.ifsboot_imagefs_paddr =
		    (uint32_t)detail(&info, "imagefs_paddr");
		fsimg_image_free(image);
		fsimg_io_free(io);
		fsimg_io_free(startup_io);
		startup_io = memory_io(&f, &section);
		inputs = (fsimg_layer_inputs_t){ .ifsboot_startup = startup_io };
		bzero(&again, sizeof(again));
		CHECK(wrap_memory(&f, &disk, &rebuilt, &inputs, &again, NULL) ==
		    FSIMG_OK);
		CHECK(again.length == created.length &&
		    bcmp(again.bytes, created.bytes, (size_t)created.length) == 0);
		fsimg_io_free(startup_io);
		memory_free(&again);
		memory_free(&disk);
		memory_free(&section);
		memory_free(&prefix);
		memory_free(&wrapped);
		memory_free(&created);
		memory_free(&startup);
		memory_free(&volume);
		fixture_free(&f);
	}
}

/*
 * Absent startup code, or an empty file, gives the 260-byte section of
 * the header and its checksum word; code already a whole number of words
 * takes no padding.
 */
static void
test_startup_lengths(void)
{
	volume_spec_t spec = { .random_bytes = 100, .seed = 2 };
	unsigned char code[4004];
	fsimg_layer_options_t layers;
	fsimg_layer_inputs_t inputs;
	memory_t volume, startup, out;
	fsimg_io_t *startup_io;
	fixture_t f;

	fill_random(code, sizeof(code), 9);
	fixture_init(&f, 64 * MIB);
	build_volume(&f, &spec, &volume);
	boot_layers(&layers, 0, FSIMG_IFSBOOT_NONE);
	for (unsigned c = 0; c < 3; c++) {
		size_t length = c == 0 ? 0 : c == 1 ? 0 : sizeof(code);

		startup = memory_of(code, length);
		startup_io = memory_io(&f, &startup);
		inputs = (fsimg_layer_inputs_t){
			.ifsboot_startup = c == 0 ? NULL : startup_io };
		CHECK(create_boot(&f, &spec, &layers, c == 0 ? NULL : &inputs,
		    &out, NULL) == FSIMG_OK);
		check_boot(out.bytes, (size_t)out.length, &(boot_expect_t){
		    .layers = &layers, .code = code, .code_bytes = length,
		    .disk = volume.bytes, .disk_bytes = (size_t)volume.length });
		CHECK(get_le32(out.bytes + H_STARTUP_SIZE) ==
		    (c == 2 ? HEADER_BYTES + sizeof(code) + 4 : 260));
		fsimg_io_free(startup_io);
		memory_free(&startup);
		memory_free(&out);
	}
	memory_free(&volume);
	fixture_free(&f);
}

/*
 * The x86 prefix, under both modes: E13's 1085-byte loader becomes the
 * 1088-byte prefix with three zero bytes, a 1088-byte one is taken as it
 * is, the header ties to it, and the prefix section unwraps padded. The
 * table lists only none for machine 3, so lzo runs under the test control
 * that lets every mode be written, and without it lzo is refused.
 */
static void
test_prefix(void)
{
	static const fsimg_ifsboot_compression_t modes[] = { FSIMG_IFSBOOT_LZO,
	    FSIMG_IFSBOOT_NONE };
	volume_spec_t spec = { .random_bytes = 5000, .seed = 4 };
	unsigned char loader[BIOS_PREFIX], code[CODE_BYTES];
	fsimg_layer_options_t layers;
	fsimg_layer_inputs_t inputs;
	memory_t volume, prefix, startup, out, section;
	fsimg_image_info_t info;
	fsimg_layer_report_t report;
	fsimg_image_t *image;
	fsimg_io_t *io, *prefix_io, *startup_io;
	fsimg_error_t error;
	fixture_t f;

	fill_random(loader, sizeof(loader), 11);
	loader[0] = 0xfa;
	fill_random(code, sizeof(code), 12);
	for (unsigned m = 0; m < 2; m++) {
		for (unsigned p = 0; p < 2; p++) {
			size_t length = p == 0 ? LOADER_BYTES : BIOS_PREFIX;

			fixture_init(&f, 64 * MIB);
			fsimg_test_unproven_writes(f.ctx,
			    modes[m] == FSIMG_IFSBOOT_LZO);
			build_volume(&f, &spec, &volume);
			boot_layers(&layers, 3, modes[m]);
			prefix = memory_of(loader, length);
			startup = memory_of(code, sizeof(code));
			prefix_io = memory_io(&f, &prefix);
			startup_io = memory_io(&f, &startup);
			inputs = (fsimg_layer_inputs_t){
				.ifsboot_startup = startup_io,
				.ifsboot_prefix = prefix_io };
			CHECK(create_boot(&f, &spec, &layers, &inputs, &out, NULL) ==
			    FSIMG_OK);
			check_boot(out.bytes, (size_t)out.length, &(boot_expect_t){
			    .layers = &layers, .prefix = loader,
			    .prefix_bytes = length, .code = code,
			    .code_bytes = sizeof(code), .disk = volume.bytes,
			    .disk_bytes = (size_t)volume.length });
			image = open_image(&f, &out, &io);
			CHECK(fsimg_image_get_info(image, &info, NULL) == FSIMG_OK);
			CHECK(detail(&info, "prefix_bytes") == BIOS_PREFIX &&
			    detail(&info, "preboot_size") == BIOS_PREFIX &&
			    detail(&info, "machine") == 3);
			CHECK(fsimg_image_verify(image, FSIMG_VERIFY_PAYLOAD, &report,
			    NULL) == FSIMG_OK);
			unwrap(&f, image, FSIMG_SELECT_SECTION, "prefix", FSIMG_OK,
			    &section);
			CHECK(section.length == BIOS_PREFIX &&
			    bcmp(section.bytes, loader, length) == 0 &&
			    all_zero(section.bytes + length, BIOS_PREFIX - length));
			fsimg_image_free(image);
			fsimg_io_free(io);
			memory_free(&section);
			memory_free(&out);

			/*
			 * The table, not the control, decides: a wrap of the same
			 * volume is written under none and refused under lzo.
			 */
			if (modes[m] == FSIMG_IFSBOOT_NONE) {
				CHECK(wrap_memory(&f, &volume, &layers, &inputs, &out,
				    NULL) == FSIMG_OK);
				check_boot(out.bytes, (size_t)out.length,
				    &(boot_expect_t){ .layers = &layers,
				    .prefix = loader, .prefix_bytes = length,
				    .code = code, .code_bytes = sizeof(code),
				    .disk = volume.bytes,
				    .disk_bytes = (size_t)volume.length });
				memory_free(&out);
			}
			if (modes[m] == FSIMG_IFSBOOT_LZO) {
				fsimg_test_unproven_writes(f.ctx, false);
				bzero(&out, sizeof(out));
				expect_error(wrap_memory(&f, &volume, &layers, &inputs,
				    &out, &error), FSIMG_EUNSUPPORTED, &error,
				    "ifsboot:machine",
				    "machine 3 is not writable under compress=lzo");
				expect_untouched(&out);
				memory_free(&out);
			}
			fsimg_io_free(prefix_io);
			fsimg_io_free(startup_io);
			memory_free(&prefix);
			memory_free(&startup);
			memory_free(&volume);
			fixture_free(&f);
		}
	}
}

/*
 * A direct construction of the container's plan for machine under mode,
 * with the prefix and startup code (NULL for none) recorded as the disk
 * plan records them, at the length their objects report, and charged to
 * session. *plan receives what the constructor attached, NULL or a plan,
 * partial on failure, for free_plan.
 */
static fsimg_status_t
construct_plan(fixture_t *f, uint32_t machine,
    fsimg_ifsboot_compression_t mode, memory_t *prefix, memory_t *startup,
    fsimg_session_t *session, fsimg_container_plan_t **plan,
    fsimg_error_t *error)
{
	fsimg_planned_file_t inputs[FSIMG_FILE_SETTINGS];
	fsimg_container_plan_args_t input;
	fsimg_layer_options_t layers;
	fsimg_status_t status;

	boot_layers(&layers, machine, mode);
	bzero(inputs, sizeof(inputs));
	if (prefix != NULL)
		inputs[FSIMG_FILE_IFSBOOT_PREFIX].io = memory_io(f, prefix);
	if (startup != NULL)
		inputs[FSIMG_FILE_IFSBOOT_STARTUP].io = memory_io(f, startup);
	for (unsigned i = 0; i < FSIMG_FILE_SETTINGS; i++) {
		if (inputs[i].io != NULL)
			CHECK(fsimg_io_get_size(inputs[i].io, &inputs[i].length,
			    NULL) == FSIMG_OK);
	}
	input = (fsimg_container_plan_args_t){ .ctx = f->ctx,
	    .session = session, .layers = &layers, .files = inputs,
	    .epoch = EPOCH, .flags = FSIMG_WRITE_REPRODUCIBLE,
	    .volume_order = FSIMG_BYTE_ORDER_LITTLE };
	*plan = NULL;
	status = fsimg_ifsboot_container.writer->plan(&input, plan, error);
	for (unsigned i = 0; i < FSIMG_FILE_SETTINGS; i++)
		fsimg_io_free(inputs[i].io);
	return (status);
}

/* Release a constructed plan as the disk plan does: NULL is no plan. */
static void
free_plan(fsimg_container_plan_t *plan)
{
	if (plan != NULL)
		fsimg_ifsboot_container.writer->plan_free(plan);
}

/*
 * The plan for machine 3 with prefix, its allocations poisoned or the
 * fail_alloc-th of them failed (0 none); the input object is made before
 * either applies.
 */
static fsimg_status_t
plan_prefix(fixture_t *f, memory_t *prefix, bool poison, uint64_t fail_alloc,
    fsimg_session_t *session, fsimg_container_plan_t **plan,
    fsimg_error_t *error)
{
	fsimg_layer_options_t layers;
	fsimg_planned_file_t inputs[FSIMG_FILE_SETTINGS];
	fsimg_container_plan_args_t input;
	fsimg_status_t status;

	boot_layers(&layers, 3, FSIMG_IFSBOOT_LZO);
	bzero(inputs, sizeof(inputs));
	inputs[FSIMG_FILE_IFSBOOT_PREFIX].io = memory_io(f, prefix);
	inputs[FSIMG_FILE_IFSBOOT_PREFIX].length = LOADER_BYTES;
	input = (fsimg_container_plan_args_t){ .ctx = f->ctx,
	    .session = session, .layers = &layers, .files = inputs,
	    .epoch = EPOCH, .flags = FSIMG_WRITE_REPRODUCIBLE,
	    .volume_order = FSIMG_BYTE_ORDER_LITTLE };
	*plan = NULL;
	fsimg_test_poison_allocations(f->ctx, poison);
	fsimg_test_fail_alloc_after(f->ctx, fail_alloc);
	status = fsimg_ifsboot_container.writer->plan(&input, plan, error);
	fsimg_test_fail_alloc_after(f->ctx, 0);
	fsimg_test_poison_allocations(f->ctx, false);
	fsimg_io_free(inputs[FSIMG_FILE_IFSBOOT_PREFIX].io);
	return (status);
}

/*
 * The plan's prefix buffer: copied through an allocation the test control
 * fills with 0xa5, the 1085-byte loader still gains three zero bytes, so
 * the padding is written, not inherited. Every failure leaves what the
 * constructor attached, nothing when the plan's own allocation fails and a
 * partial plan once it is made (the prefix buffer's allocation, the
 * signature check, a short read, a read failure), and the one release
 * returns every charge.
 */
static void
test_prefix_buffer(void)
{
	unsigned char loader[LOADER_BYTES];
	const unsigned char *bytes;
	fsimg_container_plan_t *plan;
	fsimg_session_t session;
	fsimg_error_t error;
	memory_t prefix;
	uint64_t baseline;
	size_t length;
	fixture_t f;

	fill_random(loader, sizeof(loader), 13);
	loader[0] = 0xfa;
	fixture_init(&f, 64 * MIB);
	fsimg_test_unproven_writes(f.ctx, true);
	fsimg_session_init(&session, f.ctx, "test", 0);
	baseline = memory_now(&f);
	prefix = memory_of(loader, sizeof(loader));
	CHECK(plan_prefix(&f, &prefix, true, 0, &session, &plan, NULL) ==
	    FSIMG_OK);
	fsimg_test_ifsboot_prefix(plan, &bytes, &length);
	CHECK(length == BIOS_PREFIX);
	CHECK(bcmp(bytes, loader, sizeof(loader)) == 0);
	CHECK(all_zero(bytes + LOADER_BYTES, 3));
	CHECK(plan->passes == 2 && plan->chunk_bytes == UNIT &&
	    plan->granularity == 1 && !plan->uses_library &&
	    all_zero(plan->identifier, sizeof(plan->identifier)));
	free_plan(plan);
	CHECK(memory_now(&f) == baseline);

	put_le32(prefix.bytes, SIGNATURE);
	expect_error(plan_prefix(&f, &prefix, false, 0, &session, &plan,
	    &error), FSIMG_EINVAL, &error, "ifsboot:prefix",
	    "startup signature");
	CHECK(plan != NULL);
	free_plan(plan);
	CHECK(memory_now(&f) == baseline);
	prefix.bytes[0] = 0xfa;

	prefix.length = 1000;
	prefix.size_claim = LOADER_BYTES;
	expect_error(plan_prefix(&f, &prefix, false, 0, &session, &plan,
	    &error), FSIMG_ESOURCE_CHANGED, &error, "ifsboot:prefix",
	    "planned length");
	CHECK(plan != NULL);
	free_plan(plan);
	CHECK(memory_now(&f) == baseline);
	prefix.length = LOADER_BYTES;
	prefix.size_claim = 0;

	prefix.fail_read = prefix.reads + 1;
	expect_error(plan_prefix(&f, &prefix, false, 0, &session, &plan,
	    &error), FSIMG_EIO, &error, "ifsboot:prefix", NULL);
	CHECK(plan != NULL);
	free_plan(plan);
	CHECK(memory_now(&f) == baseline);
	prefix.fail_read = 0;

	/* The plan, then its prefix buffer, are its two allocations. */
	CHECK(plan_prefix(&f, &prefix, false, 1, &session, &plan, &error) ==
	    FSIMG_ENOMEM);
	CHECK(plan == NULL && memory_now(&f) == baseline);
	CHECK(plan_prefix(&f, &prefix, false, 2, &session, &plan, &error) ==
	    FSIMG_ENOMEM);
	CHECK(plan != NULL);
	fsimg_test_ifsboot_prefix(plan, &bytes, &length);
	CHECK(bytes == NULL);
	free_plan(plan);
	CHECK(plan_prefix(&f, &prefix, false, 3, &session, &plan, &error) ==
	    FSIMG_OK);
	free_plan(plan);
	CHECK(memory_now(&f) == baseline);
	memory_free(&prefix);
	fixture_free(&f);
}

/* The largest work an "image output" session reported. */
static void
record_output_work(void *argument, const fsimg_progress_t *progress)
{
	uint64_t *work = argument;

	if (progress->phase != NULL &&
	    strcmp(progress->phase, "image output") == 0 &&
	    progress->work > *work)
		*work = progress->work;
}

/*
 * The plan reads its prefix through the operation's session, whatever
 * session the caller's object has (a callback object has none): each
 * transfer, short ones included, and each byte is charged to it, so a work
 * limit the reads cross fails the plan naming the prefix, and a wrap's
 * output continues the counter the plan began. A prefix read one byte per
 * transfer costs a wrap exactly 1084 more units of work than one read in
 * a single transfer, for the same output.
 */
static void
test_plan_session(void)
{
	volume_spec_t spec = { .random_bytes = 100, .seed = 19 };
	unsigned char loader[LOADER_BYTES];
	fsimg_layer_options_t layers;
	fsimg_layer_inputs_t inputs;
	fsimg_container_plan_t *plan;
	fsimg_session_t session;
	fsimg_options_t options;
	fsimg_error_t error;
	memory_t prefix, volume, out[2];
	uint64_t work[2], reads;
	fsimg_io_t *io;
	fixture_t f;

	fill_random(loader, sizeof(loader), 20);
	loader[0] = 0xfa;
	fixture_init(&f, 64 * MIB);
	prefix = memory_of(loader, sizeof(loader));
	prefix.cap = 100;
	fsimg_session_init(&session, f.ctx, "test", 0);
	reads = prefix.reads;
	CHECK(construct_plan(&f, 3, FSIMG_IFSBOOT_NONE, &prefix, NULL,
	    &session, &plan, NULL) == FSIMG_OK);
	CHECK(prefix.reads - reads == 11);
	CHECK(session.progress.work == 11 + LOADER_BYTES);
	free_plan(plan);
	fixture_free(&f);

	/* A limit between the transfers and their bytes stops the plan. */
	fixture_init(&f, 64 * MIB);
	fsimg_ctx_free(f.ctx);
	fsimg_options_init(&options);
	options.memory_bytes = 64 * MIB;
	options.scratch_directory_fd = f.directory_fd;
	options.max_open_files = 64;
	options.max_work = 500;
	CHECK(fsimg_ctx_new(&options, &f.ctx, NULL) == FSIMG_OK);
	f.baseline_memory = memory_now(&f);
	fsimg_session_init(&session, f.ctx, "test", 0);
	expect_error(construct_plan(&f, 3, FSIMG_IFSBOOT_NONE, &prefix, NULL,
	    &session, &plan, &error), FSIMG_ELIMIT, &error, "ifsboot:prefix",
	    NULL);
	free_plan(plan);
	fixture_free(&f);

	for (unsigned i = 0; i < 2; i++) {
		fixture_init(&f, 64 * MIB);
		fsimg_ctx_free(f.ctx);
		fsimg_options_init(&options);
		options.memory_bytes = 64 * MIB;
		options.scratch_directory_fd = f.directory_fd;
		options.max_open_files = 64;
		options.progress = record_output_work;
		options.progress_argument = &work[i];
		CHECK(fsimg_ctx_new(&options, &f.ctx, NULL) == FSIMG_OK);
		f.baseline_memory = memory_now(&f);
		work[i] = 0;
		build_volume(&f, &spec, &volume);
		boot_layers(&layers, 3, FSIMG_IFSBOOT_NONE);
		/* One transfer of the whole prefix, then one per byte. */
		prefix.cap = i == 0 ? 0 : 1;
		io = memory_io(&f, &prefix);
		inputs = (fsimg_layer_inputs_t){ .ifsboot_prefix = io };
		bzero(&out[i], sizeof(out[i]));
		CHECK(wrap_memory(&f, &volume, &layers, &inputs, &out[i],
		    NULL) == FSIMG_OK);
		fsimg_io_free(io);
		memory_free(&volume);
		fixture_free(&f);
	}
	CHECK(out[0].length == out[1].length &&
	    bcmp(out[0].bytes, out[1].bytes, (size_t)out[0].length) == 0);
	CHECK(work[0] != 0 && work[1] == work[0] + LOADER_BYTES - 1);
	memory_free(&out[0]);
	memory_free(&out[1]);
	memory_free(&prefix);
}

/*
 * Units of random bytes encode longer than a length word holds, so each
 * is stored as two halves; so is a final unit of 65400 bytes, whose second
 * half stays final. Every half reads back.
 */
static void
test_splits(void)
{
	volume_spec_t spec = { .random_bytes = 3 * UNIT, .seed = 3 };
	fsimg_layer_options_t layers;
	memory_t volume, out;
	boot_check_t check;
	uint64_t rest;
	fixture_t f;

	fixture_init(&f, 64 * MIB);
	boot_layers(&layers, 0, FSIMG_IFSBOOT_LZO);
	build_volume(&f, &spec, &volume);
	CHECK(create_boot(&f, &spec, &layers, NULL, &out, NULL) == FSIMG_OK);
	check = check_boot(out.bytes, (size_t)out.length, &(boot_expect_t){
	    .layers = &layers, .disk = volume.bytes,
	    .disk_bytes = (size_t)volume.length });
	CHECK(check.splits >= 2);
	expect_disk(&f, &out, &volume);
	memory_free(&out);

	/* Grow the random file until the disk ends 65400 bytes into a unit. */
	for (unsigned tries = 0; volume.length % UNIT != 65400; tries++) {
		CHECK(tries < 4);
		rest = volume.length % UNIT;
		spec.random_bytes += (65400 + UNIT - rest) % UNIT;
		memory_free(&volume);
		build_volume(&f, &spec, &volume);
	}
	CHECK(create_boot(&f, &spec, &layers, NULL, &out, NULL) == FSIMG_OK);
	check = check_boot(out.bytes, (size_t)out.length, &(boot_expect_t){
	    .layers = &layers, .disk = volume.bytes,
	    .disk_bytes = (size_t)volume.length });
	CHECK(check.last_split);
	expect_disk(&f, &out, &volume);
	memory_free(&out);
	memory_free(&volume);
	fixture_free(&f);
}

/* Each required setting unset, its value and its presence flag clear, is
 * refused naming its key, by a create and by a wrap, whose sink is never
 * touched.
 */
static void
test_required_settings(void)
{
#define REQUIRED(key, value, set) { key, \
	offsetof(fsimg_layer_options_t, value), \
	sizeof(((fsimg_layer_options_t *)0)->value), \
	offsetof(fsimg_layer_options_t, set) }
	static const struct {
		const char	*key;
		size_t		 value;
		size_t		 bytes;
		size_t		 set;
	} required[] = {
		REQUIRED("ifsboot:machine", ifsboot_machine, ifsboot_machine_set),
		REQUIRED("ifsboot:virtual", ifsboot_virtual, ifsboot_virtual_set),
		REQUIRED("ifsboot:startup_vaddr", ifsboot_startup_vaddr,
		    ifsboot_startup_vaddr_set),
		REQUIRED("ifsboot:paddr_bias", ifsboot_paddr_bias,
		    ifsboot_paddr_bias_set),
		REQUIRED("ifsboot:image_paddr", ifsboot_image_paddr,
		    ifsboot_image_paddr_set),
		REQUIRED("ifsboot:ram_paddr", ifsboot_ram_paddr,
		    ifsboot_ram_paddr_set),
		REQUIRED("ifsboot:imagefs_paddr", ifsboot_imagefs_paddr,
		    ifsboot_imagefs_paddr_set)
	};
#undef REQUIRED
	volume_spec_t spec = { .random_bytes = 100, .seed = 5 };
	fsimg_layer_options_t layers;
	fsimg_error_t error;
	memory_t volume, out;
	fixture_t f;

	fixture_init(&f, 64 * MIB);
	build_volume(&f, &spec, &volume);
	for (unsigned i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
		boot_layers(&layers, 0, FSIMG_IFSBOOT_LZO);
		bzero((unsigned char *)&layers + required[i].value,
		    required[i].bytes);
		bzero((unsigned char *)&layers + required[i].set, sizeof(bool));
		expect_error(create_boot(&f, &spec, &layers, NULL, &out, &error),
		    FSIMG_EINVAL, &error, required[i].key, "required");
		bzero(&out, sizeof(out));
		expect_error(wrap_memory(&f, &volume, &layers, NULL, &out, &error),
		    FSIMG_EINVAL, &error, required[i].key, "required");
		expect_untouched(&out);
		memory_free(&out);
	}
	memory_free(&volume);
	fixture_free(&f);
}

/* A wrap refused with status naming path, its sink untouched. */
static void
expect_wrap_refused(fixture_t *f, memory_t *volume,
    const fsimg_layer_options_t *layers, const fsimg_layer_inputs_t *inputs,
    fsimg_status_t status, const char *path, const char *words)
{
	fsimg_error_t error;
	memory_t out;

	bzero(&out, sizeof(out));
	expect_error(wrap_memory(f, volume, layers, inputs, &out, &error),
	    status, &error, path, words);
	expect_untouched(&out);
	memory_free(&out);
}

/*
 * The table refusals, each the plan's, before any sink opens: a machine
 * and mode the writable-modes column does not list: machine 42 under
 * either mode and machine 3 under lzo; a machine or compression outside
 * its table; a machine without the prefix it was observed behind or a
 * prefix without its machine; a prefix whose padded length is not 0 or
 * 1088 or that begins with the startup signature; and startup code too
 * long for startup_size. None of them needs the test control.
 */
static void
test_refusals(void)
{
	static const struct {
		uint32_t machine;
		fsimg_ifsboot_compression_t mode;
		const char *words;
	} unwritable[] = {
		{ 42, FSIMG_IFSBOOT_NONE, "machine 42 is not writable under compress=none" },
		{ 42, FSIMG_IFSBOOT_LZO, "machine 42 is not writable under compress=lzo" },
		{ 3, FSIMG_IFSBOOT_LZO, "machine 3 is not writable under compress=lzo" }
	};
	static const size_t bad_prefix[] = { 4, 1084, 1089 };
	volume_spec_t spec = { .random_bytes = 100, .seed = 6 };
	unsigned char loader[1089];
	fsimg_layer_options_t layers;
	fsimg_layer_inputs_t inputs;
	memory_t volume, prefix, startup;
	fixture_t f;

	fixture_init(&f, 64 * MIB);
	build_volume(&f, &spec, &volume);
	for (unsigned i = 0; i < sizeof(unwritable) / sizeof(unwritable[0]); i++) {
		boot_layers(&layers, unwritable[i].machine, unwritable[i].mode);
		expect_wrap_refused(&f, &volume, &layers, NULL, FSIMG_EUNSUPPORTED,
		    "ifsboot:machine", unwritable[i].words);
	}
	boot_layers(&layers, 7, FSIMG_IFSBOOT_LZO);
	expect_wrap_refused(&f, &volume, &layers, NULL, FSIMG_EINVAL,
	    "ifsboot:machine", "machine table");
	boot_layers(&layers, 0x10000, FSIMG_IFSBOOT_LZO);
	expect_wrap_refused(&f, &volume, &layers, NULL, FSIMG_EINVAL,
	    "ifsboot:machine", "machine table");
	boot_layers(&layers, 0, (fsimg_ifsboot_compression_t)2);
	expect_wrap_refused(&f, &volume, &layers, NULL, FSIMG_EINVAL,
	    "ifsboot:compress", "lzo or none");

	fill_random(loader, sizeof(loader), 14);
	loader[0] = 0xfa;
	boot_layers(&layers, 3, FSIMG_IFSBOOT_NONE);
	expect_wrap_refused(&f, &volume, &layers, NULL, FSIMG_EINVAL,
	    "ifsboot:machine", "machine 3 is not supported behind this boot prefix");
	prefix = memory_of(loader, LOADER_BYTES);
	inputs = (fsimg_layer_inputs_t){ .ifsboot_prefix = memory_io(&f, &prefix) };
	boot_layers(&layers, 0, FSIMG_IFSBOOT_LZO);
	expect_wrap_refused(&f, &volume, &layers, &inputs, FSIMG_EINVAL,
	    "ifsboot:machine", "machine 0 is not supported behind this boot prefix");
	fsimg_io_free(inputs.ifsboot_prefix);
	memory_free(&prefix);

	boot_layers(&layers, 3, FSIMG_IFSBOOT_NONE);
	for (unsigned i = 0; i < sizeof(bad_prefix) / sizeof(bad_prefix[0]); i++) {
		prefix = memory_of(loader, bad_prefix[i]);
		inputs = (fsimg_layer_inputs_t){
			.ifsboot_prefix = memory_io(&f, &prefix) };
		expect_wrap_refused(&f, &volume, &layers, &inputs, FSIMG_EINVAL,
		    "ifsboot:prefix", "prefix-table length");
		fsimg_io_free(inputs.ifsboot_prefix);
		memory_free(&prefix);
	}
	prefix = memory_of(loader, LOADER_BYTES);
	put_le32(prefix.bytes, SIGNATURE);
	inputs = (fsimg_layer_inputs_t){ .ifsboot_prefix = memory_io(&f, &prefix) };
	expect_wrap_refused(&f, &volume, &layers, &inputs, FSIMG_EINVAL,
	    "ifsboot:prefix", "startup signature");
	fsimg_io_free(inputs.ifsboot_prefix);
	memory_free(&prefix);

	/* Code that cannot fit startup_size is refused before it is read. */
	startup = memory_of(loader, 4);
	startup.size_claim = UINT64_C(1) << 32;
	inputs = (fsimg_layer_inputs_t){
		.ifsboot_startup = memory_io(&f, &startup) };
	expect_wrap_refused(&f, &volume, &layers, &inputs, FSIMG_EINVAL,
	    "ifsboot:startup", "32-bit startup_size");
	CHECK(startup.reads == 0);
	fsimg_io_free(inputs.ifsboot_startup);
	memory_free(&startup);

	/* An empty prefix file is no prefix. */
	prefix = memory_of(loader, 0);
	inputs = (fsimg_layer_inputs_t){ .ifsboot_prefix = memory_io(&f, &prefix) };
	boot_layers(&layers, 0, FSIMG_IFSBOOT_NONE);
	bzero(&startup, sizeof(startup));
	CHECK(wrap_memory(&f, &volume, &layers, &inputs, &startup, NULL) ==
	    FSIMG_OK);
	check_boot(startup.bytes, (size_t)startup.length, &(boot_expect_t){
	    .layers = &layers, .disk = volume.bytes,
	    .disk_bytes = (size_t)volume.length });
	memory_free(&startup);
	fsimg_io_free(inputs.ifsboot_prefix);
	memory_free(&prefix);
	memory_free(&volume);
	fixture_free(&f);
}

/*
 * The body's byte order, a create's endian setting or a wrap input's own,
 * must be little under both modes; a wrap input must draw an ifs claim and
 * pass its structural verification, the ImageFS checksum included; and a
 * file setting of a layer the stack lacks is refused naming its key.
 */
static void
test_byte_order_and_inputs(void)
{
	static const fsimg_ifsboot_compression_t modes[] = { FSIMG_IFSBOOT_LZO,
	    FSIMG_IFSBOOT_NONE };
	volume_spec_t little = { .random_bytes = 100, .seed = 8 };
	volume_spec_t big = { .random_bytes = 100, .seed = 8,
	    .order = FSIMG_BYTE_ORDER_BIG };
	fsimg_write_options_t options;
	fsimg_layer_options_t layers;
	fsimg_layer_inputs_t inputs;
	fsimg_writer_t *writer;
	fsimg_error_t error;
	memory_t volume, wrong, out, startup;
	unsigned char noise[8192];
	fixture_t f;

	fixture_init(&f, 64 * MIB);
	build_volume(&f, &big, &wrong);
	for (unsigned m = 0; m < 2; m++) {
		boot_layers(&layers, 0, modes[m]);
		expect_error(create_boot(&f, &big, &layers, NULL, &out, &error),
		    FSIMG_EUNSUPPORTED, &error, NULL, "big-endian");
		expect_wrap_refused(&f, &wrong, &layers, NULL, FSIMG_EUNSUPPORTED,
		    NULL, "big-endian");
	}
	memory_free(&wrong);

	boot_layers(&layers, 0, FSIMG_IFSBOOT_LZO);
	fill_random(noise, sizeof(noise), 15);
	wrong = memory_of(noise, sizeof(noise));
	expect_wrap_refused(&f, &wrong, &layers, NULL, FSIMG_EFORMAT, "input",
	    NULL);
	memory_free(&wrong);
	build_volume(&f, &little, &volume);
	wrong = memory_of(volume.bytes, (size_t)volume.length);
	wrong.bytes[wrong.length - 1] ^= 0x01;
	for (unsigned m = 0; m < 2; m++) {
		boot_layers(&layers, 0, modes[m]);
		expect_wrap_refused(&f, &wrong, &layers, NULL, FSIMG_ECORRUPT,
		    "input", NULL);
	}
	memory_free(&wrong);

	startup = memory_of(noise, 16);
	inputs = (fsimg_layer_inputs_t){
		.ifsboot_startup = memory_io(&f, &startup) };
	write_options(&options, NULL, &inputs, FSIMG_BYTE_ORDER_LITTLE);
	expect_error(fsimg_writer_new(f.ctx, "ifs", &options, &writer, &error),
	    FSIMG_EINVAL, &error, "ifsboot:startup", NULL);
	fsimg_io_free(inputs.ifsboot_startup);
	memory_free(&startup);
	memory_free(&volume);
	fixture_free(&f);
}

/*
 * The bound, called directly on constructed plans: a disk past 2^32 - 4
 * bytes, or not a whole number of words when stored raw, names
 * imagefs_size; ram_size and stored_size past 32 bits name themselves; and
 * the bound is the prefix, the startup section and the body's worst case,
 * the same however often it is asked.
 */
static void
test_bounds(void)
{
	unsigned char loader[LOADER_BYTES], code[CODE_BYTES];
	const fsimg_container_writer_t *writer =
	    fsimg_ifsboot_container.writer;
	fsimg_container_plan_t *plan;
	fsimg_session_t session;
	fsimg_error_t error;
	memory_t prefix, startup;
	uint64_t out, again;
	fixture_t f;

	fill_random(loader, sizeof(loader), 21);
	loader[0] = 0xfa;
	fill_random(code, sizeof(code), 22);
	fixture_init(&f, 64 * MIB);
	fsimg_test_unproven_writes(f.ctx, true);
	fsimg_session_init(&session, f.ctx, "test", 0);
	CHECK(construct_plan(&f, 0, FSIMG_IFSBOOT_LZO, NULL, NULL, &session,
	    &plan, NULL) == FSIMG_OK);
	expect_error(writer->bound(plan, (UINT64_C(1) << 32) - 3, &out,
	    &error), FSIMG_EUNSUPPORTED, &error, "imagefs_size", NULL);
	expect_error(writer->bound(plan, (UINT64_C(1) << 32) - 4, &out,
	    &error), FSIMG_EUNSUPPORTED, &error, "ram_size", NULL);
	expect_error(writer->bound(plan, UINT64_C(4280000000), &out, &error),
	    FSIMG_EUNSUPPORTED, &error, "stored_size", NULL);
	CHECK(writer->bound(plan, 100000, &out, NULL) == FSIMG_OK);
	CHECK(out == 260 + 100000 + 100000 / 255 + 20 * 2 + 12);
	CHECK(writer->bound(plan, 100000, &again, NULL) == FSIMG_OK &&
	    again == out);
	free_plan(plan);

	prefix = memory_of(loader, sizeof(loader));
	startup = memory_of(code, sizeof(code));
	CHECK(construct_plan(&f, 3, FSIMG_IFSBOOT_LZO, &prefix, &startup,
	    &session, &plan, NULL) == FSIMG_OK);
	CHECK(writer->bound(plan, 100000, &out, NULL) == FSIMG_OK);
	CHECK(out == BIOS_PREFIX + 260 + 4004 + 100000 + 100000 / 255 + 20 * 2 +
	    12);
	free_plan(plan);
	CHECK(construct_plan(&f, 3, FSIMG_IFSBOOT_NONE, &prefix, &startup,
	    &session, &plan, NULL) == FSIMG_OK);
	expect_error(writer->bound(plan, 4097, &out, &error),
	    FSIMG_EUNSUPPORTED, &error, "imagefs_size", NULL);
	CHECK(writer->bound(plan, 4096, &out, NULL) == FSIMG_OK &&
	    out == BIOS_PREFIX + 260 + 4004 + 4096);
	CHECK(plan->passes == 1);
	free_plan(plan);
	memory_free(&startup);
	memory_free(&prefix);
	fixture_free(&f);
}

/*
 * Sections: a name the container lacks, the section selector given to
 * select(), and a section of a DMG, which has only its disk, are each
 * FSIMG_EINVAL.
 */
static void
test_sections(void)
{
	volume_spec_t spec = { .random_bytes = 100, .seed = 16 };
	fsimg_selector_t selector = { .kind = FSIMG_SELECT_SECTION,
	    .text = "startup" };
	fsimg_write_options_t options;
	fsimg_layer_options_t layers;
	fsimg_image_t *image;
	fsimg_io_t *io, *input, *selected;
	fsimg_sink_t sink;
	memory_t volume, out, dmg, section;
	fixture_t f;

	fixture_init(&f, 64 * MIB);
	build_volume(&f, &spec, &volume);
	boot_layers(&layers, 0, FSIMG_IFSBOOT_NONE);
	bzero(&out, sizeof(out));
	CHECK(wrap_memory(&f, &volume, &layers, NULL, &out, NULL) == FSIMG_OK);
	image = open_image(&f, &out, &io);
	CHECK(fsimg_image_select(image, &selector, &selected, NULL) ==
	    FSIMG_EINVAL);
	unwrap(&f, image, FSIMG_SELECT_SECTION, "bogus", FSIMG_EINVAL, &section);
	memory_free(&section);
	unwrap(&f, image, FSIMG_SELECT_SECTION, "startup", FSIMG_OK, &section);
	CHECK(section.length == 0);
	memory_free(&section);
	fsimg_image_free(image);
	fsimg_io_free(io);
	memory_free(&out);

	write_options(&options, NULL, NULL, FSIMG_BYTE_ORDER_LITTLE);
	bzero(&dmg, sizeof(dmg));
	input = memory_io(&f, &volume);
	fsimg_sink_init(&sink, FSIMG_SINK_IO);
	sink.io = memory_io(&f, &dmg);
	CHECK(fsimg_image_wrap(f.ctx, input, "dmg", &options, &sink, NULL) ==
	    FSIMG_OK);
	fsimg_io_free(sink.io);
	fsimg_io_free(input);
	image = open_image(&f, &dmg, &io);
	unwrap(&f, image, FSIMG_SELECT_SECTION, "startup", FSIMG_EINVAL,
	    &section);
	memory_free(&section);
	fsimg_image_free(image);
	fsimg_io_free(io);
	memory_free(&dmg);
	memory_free(&volume);
	fixture_free(&f);
}

/* A wrap, or a create, whose output is the file a file setting reads is
 * refused before anything is written, and the file is unchanged.
 */
static void
test_aliases(void)
{
	volume_spec_t spec = { .random_bytes = 100, .seed = 17 };
	unsigned char code[CODE_BYTES], loader[LOADER_BYTES], after[CODE_BYTES];
	fsimg_write_options_t options;
	fsimg_layer_options_t layers;
	fsimg_layer_inputs_t inputs;
	fsimg_writer_t *writer;
	fsimg_error_t error;
	fsimg_sink_t sink;
	fsimg_io_t *input, *setting, *output;
	memory_t volume;
	fixture_t f;
	int fd;

	fill_random(code, sizeof(code), 18);
	fill_random(loader, sizeof(loader), 19);
	loader[0] = 0xfa;
	fixture_init(&f, 64 * MIB);
	fsimg_test_unproven_writes(f.ctx, true);
	build_volume(&f, &spec, &volume);
	for (unsigned s = 0; s < FSIMG_FILE_SETTINGS; s++) {
		bool is_prefix = s == FSIMG_FILE_IFSBOOT_PREFIX;
		const char *key = is_prefix ? "ifsboot:prefix" : "ifsboot:startup";

		fd = host_file(&f, "setting", is_prefix ? loader : code,
		    is_prefix ? sizeof(loader) : sizeof(code));
		setting = host_io(&f, "setting");
		CHECK(fsimg_io_from_fd(f.ctx, fd, FSIMG_IO_READ | FSIMG_IO_WRITE,
		    &output, NULL) == FSIMG_OK);
		inputs = (fsimg_layer_inputs_t){
			.ifsboot_startup = is_prefix ? NULL : setting,
			.ifsboot_prefix = is_prefix ? setting : NULL };
		boot_layers(&layers, is_prefix ? 3 : 0, FSIMG_IFSBOOT_LZO);
		write_options(&options, &layers, &inputs, FSIMG_BYTE_ORDER_LITTLE);
		input = memory_io(&f, &volume);
		fsimg_sink_init(&sink, FSIMG_SINK_IO);
		sink.io = output;
		expect_error(fsimg_image_wrap(f.ctx, input, "ifs/ifsboot",
		    &options, &sink, &error), FSIMG_EINVAL, &error, key,
		    "aliases a file setting");
		fsimg_io_free(input);

		CHECK(fsimg_writer_new(f.ctx, "ifs/ifsboot", &options, &writer,
		    NULL) == FSIMG_OK);
		CHECK(add_entries(writer, &spec, NULL) == FSIMG_OK);
		fsimg_sink_init(&sink, FSIMG_SINK_IO);
		sink.io = output;
		sink.raw_fd = raw_file(&f);
		expect_error(fsimg_writer_output(writer, &sink, &error),
		    FSIMG_EINVAL, &error, key, "aliases a file setting");
		fsimg_writer_free(writer);
		fsimg_io_free(output);
		fsimg_io_free(setting);
		CHECK(pread(fd, after, sizeof(after), 0) ==
		    (ssize_t)(is_prefix ? sizeof(loader) : sizeof(code)));
		CHECK(bcmp(after, is_prefix ? loader : code,
		    is_prefix ? sizeof(loader) : sizeof(code)) == 0);
		CHECK(close(fd) == 0);
		host_remove(&f, "setting");
	}
	memory_free(&volume);
	fixture_free(&f);
}

/*
 * A borrowed file setting is planned at the length its file has when the
 * plan runs: one whose file grew after its object was made is refused as
 * changed, naming its key, before any output, so the image never carries
 * only the old part of the file. An object of another context is refused
 * like every other borrowed object, also naming its key.
 */
static void
test_borrowed_settings(void)
{
	volume_spec_t spec = { .random_bytes = 100, .seed = 17 };
	unsigned char code[CODE_BYTES];
	fsimg_write_options_t options;
	fsimg_layer_options_t layers;
	fsimg_layer_inputs_t inputs;
	fsimg_writer_t *writer;
	fsimg_error_t error;
	fsimg_io_t *setting;
	memory_t volume, out;
	fixture_t f, other;
	int fd;

	fill_random(code, sizeof(code), 18);
	fixture_init(&f, 64 * MIB);
	fixture_init(&other, 64 * MIB);
	build_volume(&f, &spec, &volume);
	boot_layers(&layers, 0, FSIMG_IFSBOOT_LZO);

	fd = host_file(&f, "startup", code, 1000);
	setting = host_io(&f, "startup");
	CHECK(pwrite(fd, code + 1000, sizeof(code) - 1000, 1000) ==
	    (ssize_t)(sizeof(code) - 1000));
	inputs = (fsimg_layer_inputs_t){ .ifsboot_startup = setting };
	write_options(&options, &layers, &inputs, FSIMG_BYTE_ORDER_LITTLE);
	expect_error(fsimg_writer_new(f.ctx, "ifs/ifsboot", &options, &writer,
	    &error), FSIMG_ESOURCE_CHANGED, &error, "ifsboot:startup", NULL);
	bzero(&out, sizeof(out));
	expect_error(wrap_memory(&f, &volume, &layers, &inputs, &out, &error),
	    FSIMG_ESOURCE_CHANGED, &error, "ifsboot:startup", NULL);
	expect_untouched(&out);
	memory_free(&out);
	fsimg_io_free(setting);

	CHECK(close(fd) == 0);
	fd = openat(f.directory_fd, "startup", O_RDONLY | O_CLOEXEC);
	CHECK(fd != -1);
	CHECK(fsimg_io_from_fd(other.ctx, fd, FSIMG_IO_READ, &setting, NULL) ==
	    FSIMG_OK);
	inputs = (fsimg_layer_inputs_t){ .ifsboot_startup = setting };
	expect_error(fsimg_writer_new(f.ctx, "ifs/ifsboot", &options, &writer,
	    &error), FSIMG_EINVAL, &error, "ifsboot:startup", "context");
	bzero(&out, sizeof(out));
	expect_error(wrap_memory(&f, &volume, &layers, &inputs, &out, &error),
	    FSIMG_EINVAL, &error, "ifsboot:startup", "context");
	expect_untouched(&out);
	memory_free(&out);
	fsimg_io_free(setting);
	CHECK(close(fd) == 0);
	host_remove(&f, "startup");
	memory_free(&volume);
	fixture_free(&other);
	fixture_free(&f);
}

/*
 * A create whose host file setting named name changes, by change, after
 * the writer's construction planned it and before its output copies it.
 */
static fsimg_status_t
create_changed(fixture_t *f, const volume_spec_t *spec,
    const fsimg_layer_options_t *layers, const fsimg_layer_inputs_t *inputs,
    int fd, change_t change, memory_t *out, fsimg_error_t *error)
{
	static const unsigned char more[100] = { 1 };
	struct timespec times[2] = { { .tv_sec = 1000000000 },
	    { .tv_sec = 1000000000 } };
	fsimg_write_options_t options;
	fsimg_writer_t *writer;
	fsimg_sink_t sink;
	unsigned char byte;
	fsimg_status_t status;
	struct stat st;

	bzero(out, sizeof(*out));
	write_options(&options, layers, inputs, FSIMG_BYTE_ORDER_LITTLE);
	CHECK(fsimg_writer_new(f->ctx, "ifs/ifsboot", &options, &writer, NULL) ==
	    FSIMG_OK);
	CHECK(add_entries(writer, spec, NULL) == FSIMG_OK);
	CHECK(fstat(fd, &st) == 0);
	switch (change) {
	case CHANGE_TRUNCATE:
		CHECK(ftruncate(fd, 1000) == 0);
		break;
	case CHANGE_GROW:
		CHECK(pwrite(fd, more, sizeof(more), st.st_size) ==
		    (ssize_t)sizeof(more));
		break;
	case CHANGE_REWRITE:
		/* The same length, other bytes, and an old modification time. */
		CHECK(pread(fd, &byte, 1, 10) == 1);
		byte ^= 0xff;
		CHECK(pwrite(fd, &byte, 1, 10) == 1);
		CHECK(futimens(fd, times) == 0);
		break;
	}
	fsimg_sink_init(&sink, FSIMG_SINK_IO);
	sink.io = memory_io(f, out);
	sink.raw_fd = raw_file(f);
	status = fsimg_writer_output(writer, &sink, error);
	fsimg_io_free(sink.io);
	fsimg_writer_free(writer);
	return (status);
}

/*
 * A startup or prefix file truncated, grown or rewritten in place between
 * the plan and the copy fails the output with FSIMG_ESOURCE_CHANGED naming
 * its key: a startup file that ends early as its copy reaches the end,
 * with no padding written after the bytes it had, every other change by
 * the check after the copy.
 */
static void
test_changed_settings(void)
{
	static const change_t changes[] = { CHANGE_TRUNCATE, CHANGE_GROW,
	    CHANGE_REWRITE };
	volume_spec_t spec = { .random_bytes = 100, .seed = 20 };
	unsigned char code[CODE_BYTES], loader[LOADER_BYTES];
	fsimg_layer_options_t layers;
	fsimg_layer_inputs_t inputs;
	fsimg_error_t error;
	fsimg_io_t *setting;
	memory_t out;
	fixture_t f;
	size_t end;
	int fd;

	fill_random(code, sizeof(code), 21);
	for (size_t i = 996; i < 1004; i++)
		code[i] |= 0x01;
	fill_random(loader, sizeof(loader), 22);
	loader[0] = 0xfa;
	fixture_init(&f, 64 * MIB);
	fsimg_test_unproven_writes(f.ctx, true);
	for (unsigned c = 0; c < 3; c++) {
		for (unsigned m = 0; m < 2; m++) {
			boot_layers(&layers, 0, m == 0 ? FSIMG_IFSBOOT_LZO :
			    FSIMG_IFSBOOT_NONE);
			fd = host_file(&f, "startup", code, sizeof(code));
			setting = host_io(&f, "startup");
			inputs = (fsimg_layer_inputs_t){ .ifsboot_startup = setting };
			expect_error(create_changed(&f, &spec, &layers, &inputs, fd,
			    changes[c], &out, &error), FSIMG_ESOURCE_CHANGED, &error,
			    "ifsboot:startup", NULL);
			/* The copy stopped where the truncated file ended: no
			 * byte, padding included, reached the sink past it. */
			end = HEADER_BYTES + 1000;
			for (size_t i = end; changes[c] == CHANGE_TRUNCATE &&
			    i < out.length; i++)
				CHECK(out.bytes[i] == 0xa5);
			memory_free(&out);
			fsimg_io_free(setting);
			CHECK(close(fd) == 0);
			host_remove(&f, "startup");

			boot_layers(&layers, 3, m == 0 ? FSIMG_IFSBOOT_LZO :
			    FSIMG_IFSBOOT_NONE);
			fd = host_file(&f, "prefix", loader, sizeof(loader));
			setting = host_io(&f, "prefix");
			inputs = (fsimg_layer_inputs_t){ .ifsboot_prefix = setting };
			expect_error(create_changed(&f, &spec, &layers, &inputs, fd,
			    changes[c], &out, &error), FSIMG_ESOURCE_CHANGED, &error,
			    "ifsboot:prefix", NULL);
			memory_free(&out);
			fsimg_io_free(setting);
			CHECK(close(fd) == 0);
			host_remove(&f, "prefix");
		}
	}
	fixture_free(&f);
}

/*
 * The body's checksum, checked by the encoder's plain sum: a create's
 * staged volume whose checksum word a fault flips fails at the end of the
 * copy as corrupt, naming the body, and a wrap's verified input changed
 * the same way after verification as changed, under both modes. Under
 * lzo, an input that changes between the two passes, keeping its sum but
 * not its encoded length, fails the framed-length check.
 */
static void
test_changed_body(void)
{
	static const fsimg_ifsboot_compression_t modes[] = { FSIMG_IFSBOOT_LZO,
	    FSIMG_IFSBOOT_NONE };
	volume_spec_t spec = { .random_bytes = 3 * UNIT, .seed = 23 };
	fsimg_layer_options_t layers;
	fsimg_error_t error;
	memory_t volume, out, input;
	unsigned char *later;
	uint32_t sum;
	size_t region;
	unsigned covered;
	fixture_t f;

	fixture_init(&f, 64 * MIB);
	build_volume(&f, &spec, &volume);
	for (unsigned m = 0; m < 2; m++) {
		boot_layers(&layers, 0, modes[m]);
		fsimg_test_transcode_flip(f.ctx, volume.length - 1);
		expect_error(create_boot(&f, &spec, &layers, NULL, &out, &error),
		    FSIMG_ECORRUPT, &error, "body", "sum to zero");
		memory_free(&out);
		bzero(&out, sizeof(out));
		expect_error(wrap_memory(&f, &volume, &layers, NULL, &out, &error),
		    FSIMG_ESOURCE_CHANGED, &error, "input", "after verification");
		memory_free(&out);
		fsimg_test_transcode_flip(f.ctx, UINT64_MAX);
	}

	/* Count the reads that cover the disk's last byte in a whole wrap:
	 * the verification's, then one per pass. */
	boot_layers(&layers, 0, FSIMG_IFSBOOT_LZO);
	input = memory_of(volume.bytes, (size_t)volume.length);
	input.watch = volume.length - 1;
	input.switch_at = UINT_MAX;
	bzero(&out, sizeof(out));
	CHECK(wrap_memory(&f, &input, &layers, NULL, &out, NULL) == FSIMG_OK);
	covered = input.watched;
	CHECK(covered >= 2);
	memory_free(&out);

	/* The second pass reads a middle region of random bytes as zeros but
	 * for a last word holding their sum: the same sum, a shorter stream. */
	later = malloc((size_t)volume.length);
	CHECK(later != NULL);
	bcopy(volume.bytes, later, (size_t)volume.length);
	region = (size_t)(volume.length / 2) & ~(size_t)3;
	sum = word_sum(later + region, 4096);
	bzero(later + region, 4096);
	put_le32(later + region + 4092, sum);
	input.later = later;
	input.watched = 0;
	input.switch_at = covered - 1;
	bzero(&out, sizeof(out));
	expect_error(wrap_memory(&f, &input, &layers, NULL, &out, &error),
	    FSIMG_ESOURCE_CHANGED, &error, "input", "two passes");
	memory_free(&out);
	free(later);
	memory_free(&input);
	memory_free(&volume);
	fixture_free(&f);
}

/* The emission invariant: a header that ties at 0, which the plan's
 * prefix check exists to prevent and a fault plants, fails the output
 * with FSIMG_ESTATE before its first byte, under both modes.
 */
static void
test_emission_fault(void)
{
	volume_spec_t spec = { .random_bytes = 100, .seed = 24 };
	unsigned char loader[LOADER_BYTES];
	fsimg_layer_options_t layers;
	fsimg_layer_inputs_t inputs;
	fsimg_error_t error;
	memory_t volume, prefix, out;
	fixture_t f;

	fill_random(loader, sizeof(loader), 25);
	loader[0] = 0xfa;
	fixture_init(&f, 64 * MIB);
	fsimg_test_unproven_writes(f.ctx, true);
	fsimg_test_misplace_boot_header(f.ctx, true);
	build_volume(&f, &spec, &volume);
	prefix = memory_of(loader, sizeof(loader));
	inputs = (fsimg_layer_inputs_t){ .ifsboot_prefix = memory_io(&f, &prefix) };
	for (unsigned m = 0; m < 2; m++) {
		boot_layers(&layers, 3, m == 0 ? FSIMG_IFSBOOT_LZO :
		    FSIMG_IFSBOOT_NONE);
		bzero(&out, sizeof(out));
		expect_error(wrap_memory(&f, &volume, &layers, &inputs, &out,
		    &error), FSIMG_ESTATE, &error, NULL, "does not decode");
		CHECK(out.writes == 0);
		memory_free(&out);
	}
	fsimg_io_free(inputs.ifsboot_prefix);
	memory_free(&prefix);
	memory_free(&volume);
	fixture_free(&f);
}

/*
 * Every allocation of a create and of a wrap, under both modes, failed in
 * turn: each is FSIMG_ENOMEM and leaves nothing charged, until the
 * operation succeeds with the same bytes.
 */
static void
test_allocation_failures(void)
{
	volume_spec_t spec = { .random_bytes = 2 * UNIT, .seed = 26 };
	unsigned char code[CODE_BYTES];
	fsimg_layer_options_t layers;
	fsimg_layer_inputs_t inputs;
	fsimg_status_t status;
	memory_t volume, startup, expected, out;
	uint64_t baseline, n;
	fixture_t f;

	fill_random(code, sizeof(code), 27);
	for (unsigned m = 0; m < 2; m++) {
		for (unsigned wrap = 0; wrap < 2; wrap++) {
			fixture_init(&f, 64 * MIB);
			build_volume(&f, &spec, &volume);
			boot_layers(&layers, 0, m == 0 ? FSIMG_IFSBOOT_LZO :
			    FSIMG_IFSBOOT_NONE);
			startup = memory_of(code, sizeof(code));
			inputs = (fsimg_layer_inputs_t){
				.ifsboot_startup = memory_io(&f, &startup) };
			bzero(&expected, sizeof(expected));
			CHECK((wrap ? wrap_memory(&f, &volume, &layers, &inputs,
			    &expected, NULL) : create_boot(&f, &spec, &layers,
			    &inputs, &expected, NULL)) == FSIMG_OK);
			baseline = memory_now(&f);
			for (n = 1;; n++) {
				CHECK(n < 100000);
				bzero(&out, sizeof(out));
				fsimg_test_fail_alloc_after(f.ctx, n);
				status = wrap ? wrap_memory(&f, &volume, &layers,
				    &inputs, &out, NULL) : create_boot(&f, &spec,
				    &layers, &inputs, &out, NULL);
				fsimg_test_fail_alloc_after(f.ctx, 0);
				CHECK(memory_now(&f) == baseline);
				if (status == FSIMG_OK)
					break;
				CHECK(status == FSIMG_ENOMEM);
				memory_free(&out);
			}
			/* The encoder's own allocations come last; dozens fail first. */
			CHECK(n > 20);
			CHECK(out.length == expected.length &&
			    bcmp(out.bytes, expected.bytes,
			    (size_t)out.length) == 0);
			memory_free(&out);
			memory_free(&expected);
			fsimg_io_free(inputs.ifsboot_startup);
			memory_free(&startup);
			memory_free(&volume);
			fixture_free(&f);
		}
	}
}

/*
 * Short transfers from the input and the startup file change nothing; a
 * read failure of either, at each read in turn, fails the wrap with
 * FSIMG_EIO, naming ifsboot:startup when the startup file failed.
 */
static void
test_short_io(void)
{
	volume_spec_t spec = { .random_bytes = 2 * UNIT, .seed = 28 };
	unsigned char code[CODE_BYTES];
	fsimg_layer_options_t layers;
	fsimg_layer_inputs_t inputs;
	fsimg_error_t error;
	fsimg_status_t status;
	memory_t volume, startup, expected, out;
	uint64_t baseline, reads, n;
	fixture_t f;

	fill_random(code, sizeof(code), 29);
	fixture_init(&f, 64 * MIB);
	build_volume(&f, &spec, &volume);
	boot_layers(&layers, 0, FSIMG_IFSBOOT_LZO);
	startup = memory_of(code, sizeof(code));
	inputs = (fsimg_layer_inputs_t){
		.ifsboot_startup = memory_io(&f, &startup) };
	bzero(&expected, sizeof(expected));
	CHECK(wrap_memory(&f, &volume, &layers, &inputs, &expected, NULL) ==
	    FSIMG_OK);
	baseline = memory_now(&f);

	volume.cap = 1000;
	startup.cap = 777;
	bzero(&out, sizeof(out));
	CHECK(wrap_memory(&f, &volume, &layers, &inputs, &out, NULL) == FSIMG_OK);
	CHECK(out.length == expected.length &&
	    bcmp(out.bytes, expected.bytes, (size_t)out.length) == 0);
	memory_free(&out);
	volume.cap = 0;
	startup.cap = 0;

	for (unsigned which = 0; which < 2; which++) {
		memory_t *failing = which == 0 ? &startup : &volume;

		for (n = 1;; n++) {
			CHECK(n < 100000);
			reads = failing->reads;
			failing->fail_read = reads + n;
			bzero(&out, sizeof(out));
			status = wrap_memory(&f, &volume, &layers, &inputs, &out,
			    &error);
			failing->fail_read = 0;
			memory_free(&out);
			CHECK(memory_now(&f) == baseline);
			if (failing->reads < reads + n) {
				CHECK(status == FSIMG_OK);
				break;
			}
			expect_error(status, FSIMG_EIO, &error,
			    which == 0 ? "ifsboot:startup" : NULL, NULL);
		}
		CHECK(n > 1);
	}
	memory_free(&expected);
	fsimg_io_free(inputs.ifsboot_startup);
	memory_free(&startup);
	memory_free(&volume);
	fixture_free(&f);
}

/*
 * The output depends on the inputs alone: the same under a small and a
 * large memory budget and with hole discovery off, and equal to digests
 * pinned here, so every host writes the same bytes.
 */
static void
test_reproducible(void)
{
	static const uint64_t pinned[] = {
		UINT64_C(0xc43979807086c829), UINT64_C(0xa26d16af707350a5)
	};
	volume_spec_t spec = { .random_bytes = 3000, .seed = 1 };
	unsigned char code[CODE_BYTES];
	fsimg_layer_options_t layers;
	fsimg_layer_inputs_t inputs;
	memory_t startup, out;
	uint64_t first = 0;
	fixture_t f;

	fill_random(code, sizeof(code), 7);
	for (unsigned m = 0; m < 2; m++) {
		for (unsigned run = 0; run < 3; run++) {
			fixture_init(&f, run == 1 ? 16 * MIB : 256 * MIB);
			fsimg_test_hole_discovery(f.ctx, run != 2);
			boot_layers(&layers, 0, m == 0 ? FSIMG_IFSBOOT_LZO :
			    FSIMG_IFSBOOT_NONE);
			startup = memory_of(code, sizeof(code));
			inputs = (fsimg_layer_inputs_t){
				.ifsboot_startup = memory_io(&f, &startup) };
			CHECK(create_boot(&f, &spec, &layers, &inputs, &out, NULL) ==
			    FSIMG_OK);
			if (run == 0)
				first = digest(out.bytes, (size_t)out.length);
			CHECK(digest(out.bytes, (size_t)out.length) == first);
			memory_free(&out);
			fsimg_io_free(inputs.ifsboot_startup);
			memory_free(&startup);
			fixture_free(&f);
		}
		if (first != pinned[m])
			printf("digest %u: 0x%016llx\n", m, (unsigned long long)first);
		CHECK(first == pinned[m]);
	}
}

/*
 * One address altered rebuilds an image whose info shows the new value,
 * whose startup checksum still verifies, and which differs from the first
 * only in that field and the startup checksum word.
 */
static void
test_altered_address(void)
{
	volume_spec_t spec = { .random_bytes = 3000, .seed = 30 };
	unsigned char code[CODE_BYTES];
	fsimg_layer_options_t layers;
	fsimg_layer_inputs_t inputs;
	fsimg_layer_report_t report;
	fsimg_image_info_t info;
	fsimg_image_t *image;
	fsimg_io_t *io;
	memory_t startup, first, second;
	size_t checksum;
	unsigned differ = 0;
	fixture_t f;

	fill_random(code, sizeof(code), 31);
	fixture_init(&f, 64 * MIB);
	boot_layers(&layers, 0, FSIMG_IFSBOOT_LZO);
	startup = memory_of(code, sizeof(code));
	inputs = (fsimg_layer_inputs_t){ .ifsboot_startup = memory_io(&f, &startup) };
	CHECK(create_boot(&f, &spec, &layers, &inputs, &first, NULL) == FSIMG_OK);
	layers.ifsboot_image_paddr = 0x12345678;
	CHECK(create_boot(&f, &spec, &layers, &inputs, &second, NULL) ==
	    FSIMG_OK);
	CHECK(first.length == second.length);
	checksum = HEADER_BYTES + 4004;
	for (size_t i = 0; i < first.length; i++) {
		if (first.bytes[i] == second.bytes[i])
			continue;
		CHECK((i >= H_IMAGE_PADDR && i < H_IMAGE_PADDR + 4) ||
		    (i >= checksum && i < checksum + 4));
		differ++;
	}
	CHECK(differ > 0);
	image = open_image(&f, &second, &io);
	CHECK(fsimg_image_get_info(image, &info, NULL) == FSIMG_OK);
	CHECK(detail(&info, "image_paddr") == 0x12345678);
	CHECK(fsimg_image_verify(image, FSIMG_VERIFY_PAYLOAD, &report, NULL) ==
	    FSIMG_OK);
	fsimg_image_free(image);
	fsimg_io_free(io);
	memory_free(&first);
	memory_free(&second);
	fsimg_io_free(inputs.ifsboot_startup);
	memory_free(&startup);
	fixture_free(&f);
}

static unsigned char *
read_whole(const char *path, size_t *length)
{
	unsigned char *bytes;
	struct stat st;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd == -1)
		return (NULL);
	CHECK(fstat(fd, &st) == 0 && st.st_size > 0);
	bytes = malloc((size_t)st.st_size);
	CHECK(bytes != NULL);
	CHECK(pread(fd, bytes, (size_t)st.st_size, 0) == st.st_size);
	CHECK(close(fd) == 0);
	*length = (size_t)st.st_size;
	return (bytes);
}

/*
 * The boot image at the start of a gzip-compressed disk, such as E12's
 * floppy whose slot 0 is cAu.ifs: the 1344 bytes that hold any header,
 * then exactly B + stored_size bytes.
 */
static unsigned char *
read_gzip_image(const char *path, size_t *length)
{
	unsigned char head[BIOS_PREFIX + HEADER_BYTES], *bytes;
	size_t b;
	gzFile file;

	file = gzopen(path, "rb");
	if (file == NULL)
		return (NULL);
	CHECK(gzread(file, head, sizeof(head)) == (int)sizeof(head));
	b = get_le32(head) == SIGNATURE ? 0 : BIOS_PREFIX;
	CHECK(get_le32(head + b) == SIGNATURE);
	*length = b + get_le32(head + b + H_STORED_SIZE);
	CHECK(*length >= sizeof(head));
	bytes = malloc(*length);
	CHECK(bytes != NULL);
	bcopy(head, bytes, sizeof(head));
	CHECK(gzread(file, bytes + sizeof(head),
	    (unsigned)(*length - sizeof(head))) ==
	    (int)(*length - sizeof(head)));
	CHECK(gzclose(file) == Z_OK);
	return (bytes);
}

/*
 * Whether the machine table lists mode as writable for machine: the
 * profile's writable-modes column, which a native boot of a written image
 * decides. Machine 0 needs no boot; machine 3 booted under none only, since
 * the x86 startup rejects an LZO body; no SH-4 boot is recorded.
 */
static bool
table_writes(uint32_t machine, fsimg_ifsboot_compression_t mode)
{
	if (machine == 0)
		return (true);
	return (machine == 3 && mode == FSIMG_IFSBOOT_NONE);
}

/*
 * Whether bytes are a bare boot image by their own header: the first
 * signature at 0 or at 1088 decides, as a prefix never begins with the
 * signature, and B + stored_size must equal their length. *prefix_bytes
 * is B.
 */
static bool
bare_header(const unsigned char *bytes, size_t length, size_t *prefix_bytes)
{
	static const size_t offsets[] = { 0, BIOS_PREFIX };

	for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
		if (length < offsets[i] + HEADER_BYTES)
			continue;
		if (get_le32(bytes + offsets[i]) != SIGNATURE)
			continue;
		*prefix_bytes = offsets[i];
		return (length - offsets[i] ==
		    get_le32(bytes + offsets[i] + H_STORED_SIZE));
	}
	return (false);
}

/*
 * Take a native boot image apart: open it, and when it is an ifsboot
 * image unwrap its prefix, startup and disk sections into parts, checking
 * each against the byte range the profile names, found here from the
 * header alone, and take the typed settings from the values info reports.
 * The mode is none for a raw body and lzo for any framed one, UCL
 * included, since lzo is the writer's only framed mode. Returns false,
 * with nothing to free, unless the bytes themselves are an ifsboot image:
 * another container, or one inside an outer layer such as gzip.
 *
 * The header reads below trust the library's claim once the bytes are
 * tied to it: the header lies at 0 or 1088 within the bytes and its
 * B + stored_size is their length, so the claimed container is these
 * bytes, startup_size is at least 260 and the body lies within them.
 */
static bool
unwrap_native(fixture_t *f, const unsigned char *bytes, size_t length,
    unwrapped_t *parts)
{
	fsimg_image_info_t info;
	fsimg_image_t *image;
	fsimg_io_t *io;
	memory_t file;
	const unsigned char *h;
	bool ifsboot, opaque_zero;

	bzero(parts, sizeof(*parts));
	file = memory_of(bytes, length);
	image = open_image(f, &file, &io);
	CHECK(fsimg_image_get_info(image, &info, NULL) == FSIMG_OK);
	ifsboot = strcmp(info.container, "ifsboot") == 0 &&
	    bare_header(bytes, length, &parts->prefix_bytes);
	if (ifsboot) {
		h = bytes + parts->prefix_bytes;
		CHECK(detail(&info, "prefix_bytes") == parts->prefix_bytes);
		parts->startup_size = get_le32(h + H_STARTUP_SIZE);
		parts->raw_body = (h[H_FLAGS1] & 0x1c) == 0;
		/* Info's tail count must agree with the header's bytes. */
		opaque_zero = h[H_FLAGS2] == 0 &&
		    all_zero(h + H_TAIL, HEADER_BYTES - H_TAIL);
		CHECK(detail(&info, "tail_nonzero") == 0 || !opaque_zero);

		unwrap(f, image, FSIMG_SELECT_SECTION, "prefix", FSIMG_OK,
		    &parts->prefix);
		CHECK(parts->prefix.length == parts->prefix_bytes &&
		    bcmp(parts->prefix.bytes, bytes, parts->prefix_bytes) == 0);
		unwrap(f, image, FSIMG_SELECT_SECTION, "startup", FSIMG_OK,
		    &parts->code);
		CHECK(parts->code.length == parts->startup_size - 260 &&
		    bcmp(parts->code.bytes, h + HEADER_BYTES,
		    parts->startup_size - 260) == 0);
		unwrap(f, image, FSIMG_SELECT_DISK, NULL, FSIMG_OK, &parts->disk);
		CHECK(parts->disk.length == get_le32(h + H_IMAGEFS_SIZE));
		CHECK(!parts->raw_body || bcmp(parts->disk.bytes,
		    h + parts->startup_size, (size_t)parts->disk.length) == 0);

		boot_layers(&parts->layers, (uint32_t)detail(&info, "machine"),
		    parts->raw_body ? FSIMG_IFSBOOT_NONE : FSIMG_IFSBOOT_LZO);
		parts->layers.ifsboot_virtual = detail(&info, "virtual") != 0;
		parts->layers.ifsboot_startup_vaddr =
		    (uint32_t)detail(&info, "startup_vaddr");
		parts->layers.ifsboot_paddr_bias =
		    (uint32_t)detail(&info, "paddr_bias");
		parts->layers.ifsboot_image_paddr =
		    (uint32_t)detail(&info, "image_paddr");
		parts->layers.ifsboot_ram_paddr =
		    (uint32_t)detail(&info, "ram_paddr");
		parts->layers.ifsboot_imagefs_paddr =
		    (uint32_t)detail(&info, "imagefs_paddr");
	}
	fsimg_image_free(image);
	fsimg_io_free(io);
	memory_free(&file);
	return (ifsboot);
}

/* Wrap the unwrapped disk again with its sections and settings into out. */
static fsimg_status_t
wrap_unwrapped(fixture_t *f, unwrapped_t *parts, memory_t *out,
    fsimg_error_t *error)
{
	fsimg_layer_inputs_t inputs;
	fsimg_status_t status;

	inputs = (fsimg_layer_inputs_t){
		.ifsboot_prefix = memory_io(f, &parts->prefix),
		.ifsboot_startup = memory_io(f, &parts->code) };
	bzero(out, sizeof(*out));
	status = wrap_memory(f, &parts->disk, &parts->layers, &inputs, out,
	    error);
	fsimg_io_free(inputs.ifsboot_prefix);
	fsimg_io_free(inputs.ifsboot_startup);
	return (status);
}

static void
unwrapped_free(unwrapped_t *parts)
{
	memory_free(&parts->disk);
	memory_free(&parts->code);
	memory_free(&parts->prefix);
}

/*
 * Unwrap a native boot image and rebuild it under the mode unwrap_native()
 * chooses: none for a raw body, lzo for a framed one, so a UCL image is
 * rebuilt under lzo. The machine table decides, without the test control:
 * a machine and mode it does not list is refused naming the machine. A raw
 * body with a zero flags2 and tail rebuilds byte for byte; otherwise the
 * rebuild matches in the prefix, the code and every header field but
 * stored_size, the compression bits and a nonzero flags2 or tail, and its
 * disk reads back identical.
 */
static void
rebuild_native(const char *name, const unsigned char *bytes, size_t length)
{
	static const unsigned fields[] = { H_VERSION, H_HEADER_SIZE, H_MACHINE,
	    H_STARTUP_VADDR, H_PADDR_BIAS, H_IMAGE_PADDR, H_RAM_PADDR,
	    H_STARTUP_SIZE, H_IMAGEFS_PADDR, H_IMAGEFS_SIZE, H_PREBOOT_SIZE };
	unwrapped_t parts;
	fsimg_error_t error;
	memory_t again;
	const unsigned char *h, *g;
	bool opaque_zero;
	fsimg_status_t status;
	fixture_t f;

	fixture_init(&f, 512 * MIB);
	if (!unwrap_native(&f, bytes, length, &parts)) {
		fixture_free(&f);
		return;
	}
	h = bytes + parts.prefix_bytes;
	opaque_zero = h[H_FLAGS2] == 0 &&
	    all_zero(h + H_TAIL, HEADER_BYTES - H_TAIL);

	status = wrap_unwrapped(&f, &parts, &again, &error);
	if (!table_writes(parts.layers.ifsboot_machine,
	    parts.layers.ifsboot_compress)) {
		expect_error(status, FSIMG_EUNSUPPORTED, &error, "ifsboot:machine",
		    "not writable");
		printf("%s: machine %u, sections unwrapped, rebuild refused naming "
		    "the machine\n", name, (unsigned)parts.layers.ifsboot_machine);
	} else if (parts.raw_body && opaque_zero) {
		CHECK(status == FSIMG_OK);
		CHECK(again.length == length &&
		    bcmp(again.bytes, bytes, length) == 0);
		printf("%s: rebuilt byte for byte\n", name);
	} else {
		CHECK(status == FSIMG_OK);
		g = again.bytes + parts.prefix_bytes;
		CHECK(bcmp(again.bytes, bytes, parts.prefix_bytes) == 0);
		for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
			CHECK(bcmp(g + fields[i], h + fields[i],
			    fields[i] == H_VERSION || fields[i] == H_HEADER_SIZE ||
			    fields[i] == H_MACHINE || fields[i] == H_PREBOOT_SIZE ?
			    2 : 4) == 0);
		CHECK(get_le32(g + H_RAM_SIZE) == get_le32(h + H_RAM_SIZE));
		CHECK((g[H_FLAGS1] & ~0x1c) == (h[H_FLAGS1] & ~0x1c));
		CHECK(!opaque_zero || (g[H_FLAGS2] == h[H_FLAGS2] &&
		    bcmp(g + H_TAIL, h + H_TAIL, HEADER_BYTES - H_TAIL) == 0));
		CHECK(bcmp(g + HEADER_BYTES, h + HEADER_BYTES,
		    parts.startup_size - 260) == 0);
		expect_disk(&f, &again, &parts.disk);
		printf("%s: rebuilt, identical outside the body-dependent "
		    "fields, disk identical\n", name);
	}
	memory_free(&again);
	unwrapped_free(&parts);
	fixture_free(&f);
}

/*
 * Write length bytes to path, a new file. On failure, report it, remove
 * the partial file and return false, so a failed rebuild leaves nothing
 * behind and the same command can run again.
 */
static bool
write_new_file(const char *path, const unsigned char *bytes, size_t length)
{
	size_t written;
	ssize_t n;
	int fd;

	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (fd == -1) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return (false);
	}
	written = 0;
	while (written < length) {
		n = write(fd, bytes + written, length - written);
		if (n == -1 && errno == EINTR)
			continue;
		if (n <= 0) {
			fprintf(stderr, "%s: %s\n", path,
			    n == 0 ? "short write" : strerror(errno));
			close(fd);
			unlink(path);
			return (false);
		}
		written += (size_t)n;
	}
	if (close(fd) != 0) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		unlink(path);
		return (false);
	}
	return (true);
}

/*
 * --rebuild IN OUT none|lzo, the native boot gate's hook: take the boot
 * image IN apart and wrap its disk again under the named mode with its own
 * sections and header values, check every byte of the result with
 * check_boot(), and write it to OUT, a new file. It runs under the test
 * control, so that a mode the machine table does not list can be
 * written for a boot trial; such an image qualifies nothing until it boots.
 */
static int
rebuild_file(const char *in, const char *out, const char *mode_name)
{
	unwrapped_t parts;
	fsimg_error_t error;
	unsigned char *bytes;
	memory_t again;
	size_t length;
	bool identical;
	int result;
	fixture_t f;

	if (strcmp(mode_name, "none") != 0 && strcmp(mode_name, "lzo") != 0) {
		fprintf(stderr, "usage: test-ifsboot-write --rebuild IN OUT "
		    "none|lzo\n");
		return (2);
	}
	bytes = read_whole(in, &length);
	if (bytes == NULL) {
		fprintf(stderr, "%s: %s\n", in, strerror(errno));
		return (1);
	}
	fixture_init(&f, 512 * MIB);
	fsimg_test_unproven_writes(f.ctx, true);
	if (!unwrap_native(&f, bytes, length, &parts)) {
		fprintf(stderr, "%s: not a bare ifsboot image\n", in);
		free(bytes);
		fixture_free(&f);
		return (1);
	}
	parts.layers.ifsboot_compress = strcmp(mode_name, "lzo") == 0 ?
	    FSIMG_IFSBOOT_LZO : FSIMG_IFSBOOT_NONE;

	result = 1;
	if (wrap_unwrapped(&f, &parts, &again, &error) != FSIMG_OK) {
		fprintf(stderr, "%s: rebuild failed: %s: %s\n", in, error.path,
		    error.message);
	} else {
		check_boot(again.bytes, (size_t)again.length, &(boot_expect_t){
		    .layers = &parts.layers, .prefix = parts.prefix.bytes,
		    .prefix_bytes = (size_t)parts.prefix.length,
		    .code = parts.code.bytes,
		    .code_bytes = (size_t)parts.code.length,
		    .disk = parts.disk.bytes,
		    .disk_bytes = (size_t)parts.disk.length });
		identical = again.length == length &&
		    bcmp(again.bytes, bytes, length) == 0;
		if (write_new_file(out, again.bytes, (size_t)again.length)) {
			printf("%s: machine %u, compress=%s, %zu bytes, %s the "
			    "input\n", out, (unsigned)parts.layers.ifsboot_machine,
			    mode_name, (size_t)again.length,
			    identical ? "identical to" : "differs from");
			result = 0;
		}
	}

	memory_free(&again);
	unwrapped_free(&parts);
	free(bytes);
	fixture_free(&f);
	return (result);
}

/*
 * The native evidence. Under FSIMG_IFS_NATIVE: the E7 nonbootable images,
 * machine 0, rebuilt under lzo; the uncompressed x86 images, E12's cAu.ifs
 * and the x86 boot gate's gu.ifs, machine 3, rebuilt byte for byte; and
 * the gate's gl.ifs, mkifs's LZO x86 image, which the x86 startup refuses
 * to boot, unwrapped and its rebuild under lzo refused naming the machine.
 * Under FSIMG_IFS_IMAGES as well: every native boot image there unwrapped,
 * and its rebuild refused naming its machine.
 */
static int
native(void)
{
	static const char *const e7[] = { "e7-compress-2.ifs",
	    "e7-compress-3.ifs", "e7-compress-plus.ifs" };
	static const char *const gate[] = { "gu.ifs", "gl.ifs" };
	const char *captures = getenv("FSIMG_IFS_NATIVE");
	const char *images = getenv("FSIMG_IFS_IMAGES");
	struct dirent *dirent;
	unsigned char *bytes;
	char path[2048];
	size_t length;
	DIR *directory;

	if (captures == NULL || captures[0] == '\0') {
		printf("SKIP: FSIMG_IFS_NATIVE is not set\n");
		return (77);
	}
	for (size_t i = 0; i < sizeof(e7) / sizeof(e7[0]); i++) {
		snprintf(path, sizeof(path), "%s/cases/ifs-extra/%s", captures,
		    e7[i]);
		bytes = read_whole(path, &length);
		CHECK(bytes != NULL);
		rebuild_native(e7[i], bytes, length);
		free(bytes);
	}
	snprintf(path, sizeof(path), "%s/e12-links/e12-au.img.gz", captures);
	bytes = read_gzip_image(path, &length);
	CHECK(bytes != NULL);
	rebuild_native("cAu.ifs", bytes, length);
	free(bytes);
	for (size_t i = 0; i < sizeof(gate) / sizeof(gate[0]); i++) {
		snprintf(path, sizeof(path), "%s/e14-gate/%s", captures, gate[i]);
		bytes = read_whole(path, &length);
		CHECK(bytes != NULL);
		rebuild_native(gate[i], bytes, length);
		free(bytes);
	}

	if (images == NULL || images[0] == '\0') {
		printf("SKIP: FSIMG_IFS_IMAGES is not set\n");
		return (0);
	}
	directory = opendir(images);
	CHECK(directory != NULL);
	while ((dirent = readdir(directory)) != NULL) {
		size_t name_length = strlen(dirent->d_name);

		if (name_length < 5 ||
		    strcmp(dirent->d_name + name_length - 4, ".ifs") != 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s", images, dirent->d_name);
		bytes = read_whole(path, &length);
		CHECK(bytes != NULL);
		rebuild_native(dirent->d_name, bytes, length);
		free(bytes);
	}
	CHECK(closedir(directory) == 0);
	return (0);
}

int
main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--native") == 0)
		return (native());
	if (argc == 5 && strcmp(argv[1], "--rebuild") == 0)
		return (rebuild_file(argv[2], argv[3], argv[4]));
	CHECK(argc == 1);
	test_round_trips();
	test_startup_lengths();
	test_prefix();
	test_prefix_buffer();
	test_plan_session();
	test_splits();
	test_required_settings();
	test_refusals();
	test_byte_order_and_inputs();
	test_bounds();
	test_sections();
	test_aliases();
	test_borrowed_settings();
	test_changed_settings();
	test_changed_body();
	test_emission_fault();
	test_allocation_failures();
	test_short_io();
	test_reproducible();
	test_altered_address();
	return (0);
}
