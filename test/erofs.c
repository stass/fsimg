/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/stat.h>

#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "erofs-gen.h"
#include "erofs.h"
#include "format.h"
#include "test.h"

/*
 * Tests of the EROFS on-disk codecs, and of the probe, open and the metadata
 * reader. Structures are built here with small helpers that write fields at
 * the byte offsets of the format's on-disk tables, independently of codec.c.
 * Real mkfs.erofs output is embedded as sparse runs of its non-zero bytes.
 * The probe, open and metadata reader tests run against images from the
 * independent generator in test/erofs-gen.c. Every check is recorded, not
 * fatal, so one run lists every failing case.
 */

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

enum {
	REGION_BYTES = 3072,		/* the superblock region */
	SUPER_BYTES = 128,
	SENTINEL = 0xa5
};

static int failures;

static const char *
status_name(fsimg_status_t status)
{
	switch (status) {
	case FSIMG_OK:
		return ("OK");
	case FSIMG_ECORRUPT:
		return ("ECORRUPT");
	case FSIMG_EUNSUPPORTED:
		return ("EUNSUPPORTED");
	case FSIMG_EINVAL:
		return ("EINVAL");
	case FSIMG_ENOMEM:
		return ("ENOMEM");
	default:
		return ("other status");
	}
}

/* Record one failed expectation: where it is, which case and what it said. */
static void
expectf(bool ok, const char *function, int line, const char *expression,
    const char *format, ...)
{
	va_list ap;

	if (ok)
		return;
	failures++;
	fprintf(stderr, "test/erofs.c:%d: %s: case ", line, function);
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	fprintf(stderr, ": expected %s\n", expression);
}

#define EXPECT(ok, ...) \
	expectf((ok), __func__, __LINE__, #ok, __VA_ARGS__)

/* A status mismatch reports both statuses and the decoder's phrase. */
static void
expect_status(fsimg_status_t got, fsimg_status_t want, const char *what,
    const char *function, int line, const char *name)
{
	if (got == want)
		return;
	failures++;
	fprintf(stderr, "test/erofs.c:%d: %s: case %s: status %s, expected "
	    "%s (%s)\n", line, function, name, status_name(got),
	    status_name(want), what != NULL ? what : "no phrase");
}

#define EXPECT_STATUS(got, want, what, name) \
	expect_status((got), (want), (what), __func__, __LINE__, (name))

static void
put_le(unsigned char *p, uint64_t value, size_t width)
{
	size_t i;

	for (i = 0; i < width; i++)
		p[i] = (unsigned char)(value >> (8 * i));
}

/* One field written into a structure: byte offset, width and value. */
typedef struct patch {
	uint16_t	 offset;
	uint8_t		 width;
	uint64_t	 value;
} patch_t;

enum { MAX_PATCHES = 6 };

/* A patch list ends at the first entry of width zero. */
static void
patch_apply(unsigned char *buf, const patch_t *patches)
{
	size_t i;

	for (i = 0; i < MAX_PATCHES && patches[i].width != 0; i++)
		put_le(buf + patches[i].offset, patches[i].value,
		    patches[i].width);
}

/* ---- Fixtures: sparse images of real mkfs.erofs output ---- */

/*
 * A run of non-zero bytes: the image offset, the position in the fixture's
 * byte array and the length. Bytes outside every run are zero.
 */
typedef struct fixture_run {
	uint32_t	 offset;
	uint32_t	 source;
	uint32_t	 length;
} fixture_run_t;

/* base is the image offset that buffer byte 0 stands for. */
typedef struct fixture {
	const unsigned char *bytes;
	const fixture_run_t *runs;
	size_t		 run_count;
	size_t		 base;
} fixture_t;

#define FIXTURE(name, base) \
	{ name##_bytes, name##_runs, ARRAY_SIZE(name##_runs), (base) }

static void
fixture_load(const fixture_t *fixture, unsigned char *buf, size_t size)
{
	size_t i;

	bzero(buf, size);
	for (i = 0; i < fixture->run_count; i++) {
		const fixture_run_t *run = &fixture->runs[i];

		CHECK(run->offset >= fixture->base);
		CHECK(run->offset - fixture->base + run->length <= size);
		bcopy(fixture->bytes + run->source,
		    buf + (run->offset - fixture->base), run->length);
	}
}

/* ---- Independent oracles ---- */

/*
 * CRC-32C from its definition: the reflected Castagnoli polynomial
 * 0x82F63B78, bit by bit, with the register seeded with all ones. The
 * result is the register, which is the form the superblock stores; the
 * published check value is the register inverted.
 */
static uint32_t
crc32c_register(const unsigned char *p, size_t n)
{
	uint32_t reg = UINT32_C(0xffffffff);
	size_t i;
	int bit;

	for (i = 0; i < n; i++) {
		reg ^= p[i];
		for (bit = 0; bit < 8; bit++)
			reg = (reg & 1) != 0 ?
			    (reg >> 1) ^ UINT32_C(0x82f63b78) : reg >> 1;
	}
	return (reg);
}

/* The bytes the checksum covers, counted from volume byte 1024. */
static size_t
covered_bytes(unsigned blkszbits)
{
	size_t block = (size_t)1 << blkszbits;

	return (block > 1024 ? block - 1024 : block);
}

/* The checksum the design defines for region: its range, bytes 4..7 zero. */
static uint32_t
expected_checksum(const unsigned char *region, unsigned blkszbits)
{
	unsigned char copy[REGION_BYTES];
	size_t covered = covered_bytes(blkszbits);

	bcopy(region, copy, covered);
	bzero(copy + 4, 4);
	return (crc32c_register(copy, covered));
}

/* The Linux 32-bit device number encoding, from the design's bit layout. */
static uint32_t
device_word(uint32_t major, uint32_t minor)
{
	return ((minor & 0xffu) | (major << 8) |
	    ((minor & ~UINT32_C(0xff)) << 12));
}

/* ---- Superblock ---- */

/* Superblock field offsets from the design's table, relative to byte 1024. */
enum {
	SB_MAGIC = 0x00,
	SB_CHECKSUM = 0x04,
	SB_COMPAT = 0x08,
	SB_BLKSZBITS = 0x0c,
	SB_SLOTS = 0x0d,
	SB_ROOT16 = 0x0e,
	SB_INODES = 0x10,
	SB_EPOCH = 0x18,
	SB_NSEC = 0x20,
	SB_BLOCKS = 0x24,
	SB_META_BLOCK = 0x28,
	SB_XATTR_BLOCK = 0x2c,
	SB_UUID = 0x30,
	SB_NAME = 0x40,
	SB_INCOMPAT = 0x50,
	SB_WORD54 = 0x54,
	SB_DEVICES = 0x56,
	SB_DIRBLKBITS = 0x5a,
	SB_PREFIX_COUNT = 0x5b,
	SB_PREFIX_START = 0x5c,
	SB_PACKED_NID = 0x60,
	SB_FILTER_BYTE = 0x68,
	SB_ISHARE = 0x69,
	SB_RESERVED_6A = 0x6a,
	SB_BUILD_TIME = 0x6c,
	SB_ROOT64 = 0x70,
	SB_RESERVED_78 = 0x78,
	SB_BOX_NID = 0x80,
	SB_RESERVED_88 = 0x88
};

/* A valid 4096-byte-block superblock region with no features. */
static void
region_init(unsigned char *region)
{
	bzero(region, REGION_BYTES);
	put_le(region + SB_MAGIC, EROFS_MAGIC, 4);
	region[SB_BLKSZBITS] = 12;
	put_le(region + SB_ROOT16, 36, 2);
}

typedef struct region_result {
	fsimg_status_t	 status;
	const char	*what;
	erofs_super_t	 sb;
	erofs_config_t	 config;
} region_result_t;

/* Decode, and check that a failing decoder left both outputs untouched. */
static region_result_t
region_decode_checked(const char *name, const unsigned char *bytes,
    size_t length)
{
	region_result_t r;
	erofs_super_t sb_before;
	erofs_config_t config_before;

	r.what = NULL;
	memset(&r.sb, SENTINEL, sizeof(r.sb));
	memset(&r.config, SENTINEL, sizeof(r.config));
	sb_before = r.sb;
	config_before = r.config;
	r.status = erofs_region_decode(bytes, length, &r.sb, &r.config,
	    &r.what);
	if (r.status != FSIMG_OK) {
		EXPECT(bcmp(&r.sb, &sb_before, sizeof(sb_before)) == 0,
		    "%s: failure leaves the superblock untouched", name);
		EXPECT(bcmp(&r.config, &config_before,
		    sizeof(config_before)) == 0,
		    "%s: failure leaves the configuration untouched", name);
		/* The sentinel is not a valid bool; no caller may read it. */
		bzero(&r.sb, sizeof(r.sb));
		bzero(&r.config, sizeof(r.config));
	}
	return (r);
}

typedef struct super_case {
	const char	*name;
	size_t		 length;	/* 0, or left out: SUPER_BYTES */
	patch_t		 patches[MAX_PATCHES];
	fsimg_status_t	 status;
	/*
	 * A rule the probe does not check: erofs_region_decode() refuses the
	 * row with its own class, which the region cases pin.
	 */
	bool		 region_only;
} super_case_t;

#define FEATURE(offset, value)	{ (offset), 4, (value) }

static const super_case_t super_cases[] = {
	{ .name = "length_127_is_corrupt",
	    .length = 127, .status = FSIMG_ECORRUPT },
	{ .name = "length_1_is_corrupt",
	    .length = 1, .status = FSIMG_ECORRUPT },
	{ .name = "length_128_is_accepted", .length = 128, .status = FSIMG_OK },
	{ .name = "blkszbits_0_is_corrupt",
	    .patches = {{ SB_BLKSZBITS, 1, 0 }}, .status = FSIMG_ECORRUPT },
	{ .name = "blkszbits_8_is_corrupt",
	    .patches = {{ SB_BLKSZBITS, 1, 8 }}, .status = FSIMG_ECORRUPT },
	{ .name = "blkszbits_9_is_accepted",
	    .patches = {{ SB_BLKSZBITS, 1, 9 }}, .status = FSIMG_OK },
	{ .name = "blkszbits_12_is_accepted",
	    .patches = {{ SB_BLKSZBITS, 1, 12 }}, .status = FSIMG_OK },
	{ .name = "blkszbits_13_is_unsupported",
	    .patches = {{ SB_BLKSZBITS, 1, 13 }},
	    .status = FSIMG_EUNSUPPORTED },
	{ .name = "blkszbits_255_is_unsupported",
	    .patches = {{ SB_BLKSZBITS, 1, 255 }},
	    .status = FSIMG_EUNSUPPORTED },
	{ .name = "dirblkbits_1_is_unsupported",
	    .patches = {{ SB_DIRBLKBITS, 1, 1 }},
	    .status = FSIMG_EUNSUPPORTED },
	{ .name = "dirblkbits_255_is_unsupported",
	    .patches = {{ SB_DIRBLKBITS, 1, 255 }},
	    .status = FSIMG_EUNSUPPORTED },
	{ .name = "0_extension_slots_are_accepted",
	    .patches = {{ SB_SLOTS, 1, 0 }}, .status = FSIMG_OK },
	{ .name = "184_extension_slots_are_accepted",
	    .patches = {{ SB_SLOTS, 1, 184 }}, .status = FSIMG_OK },
	{ .name = "185_extension_slots_are_unsupported",
	    .patches = {{ SB_SLOTS, 1, 185 }}, .status = FSIMG_EUNSUPPORTED },
	{ .name = "255_extension_slots_are_unsupported",
	    .patches = {{ SB_SLOTS, 1, 255 }}, .status = FSIMG_EUNSUPPORTED },
	{ .name = "incompat_0x200_is_unsupported",
	    .patches = {FEATURE(SB_INCOMPAT, 0x200)},
	    .status = FSIMG_EUNSUPPORTED },
	{ .name = "incompat_0x80000000_is_unsupported",
	    .patches = {FEATURE(SB_INCOMPAT, 0x80000000)},
	    .status = FSIMG_EUNSUPPORTED },
	{ .name = "incompat_all_known_bits_with_0x200_is_unsupported",
	    .patches = {FEATURE(SB_INCOMPAT, 0x3ff), { SB_SLOTS, 1, 1 }},
	    .status = FSIMG_EUNSUPPORTED },
	{ .name = "incompat_lz4_0padding_is_accepted",
	    .patches = {FEATURE(SB_INCOMPAT, 0x001)}, .status = FSIMG_OK },
	{ .name = "incompat_compr_cfgs_is_accepted",
	    .patches = {FEATURE(SB_INCOMPAT, 0x002)}, .status = FSIMG_OK },
	{ .name = "incompat_chunked_file_is_accepted",
	    .patches = {FEATURE(SB_INCOMPAT, 0x004)}, .status = FSIMG_OK },
	{ .name = "incompat_device_table_is_accepted",
	    .patches = {FEATURE(SB_INCOMPAT, 0x008)}, .status = FSIMG_OK },
	{ .name = "incompat_ztailpacking_is_accepted",
	    .patches = {FEATURE(SB_INCOMPAT, 0x010)}, .status = FSIMG_OK },
	{ .name = "incompat_fragments_is_accepted",
	    .patches = {FEATURE(SB_INCOMPAT, 0x020)}, .status = FSIMG_OK },
	{ .name = "incompat_xattr_prefixes_is_accepted",
	    .patches = {FEATURE(SB_INCOMPAT, 0x040)}, .status = FSIMG_OK },
	{ .name = "incompat_48bit_is_accepted",
	    .patches = {FEATURE(SB_INCOMPAT, 0x080)}, .status = FSIMG_OK },
	{ .name = "incompat_metabox_with_a_slot_is_accepted",
	    .patches = {FEATURE(SB_INCOMPAT, 0x100), { SB_SLOTS, 1, 1 }},
	    .status = FSIMG_OK },
	{ .name = "incompat_all_known_bits_with_a_slot_are_accepted",
	    .patches = {FEATURE(SB_INCOMPAT, 0x1ff), { SB_SLOTS, 1, 1 }},
	    .status = FSIMG_OK },
	{ .name = "shared_ea_in_metabox_with_metabox_is_accepted",
	    .patches = {FEATURE(SB_COMPAT, 0x08), FEATURE(SB_INCOMPAT, 0x100),
	    { SB_SLOTS, 1, 1 }},
	    .status = FSIMG_OK },
	{ .name = "unknown_compat_bits_are_accepted",
	    .patches = {FEATURE(SB_COMPAT, 0xffffffc0)}, .status = FSIMG_OK },
	/* The rules erofs_region_decode() adds are not the probe's. */
	{ .name = "extra_devices_are_not_a_probe_check",
	    .patches = {{ SB_DEVICES, 2, 1 }}, .status = FSIMG_OK,
	    .region_only = true },
	{ .name = "epoch_nanoseconds_are_not_a_probe_check",
	    .patches = {FEATURE(SB_NSEC, 1000000000)}, .status = FSIMG_OK,
	    .region_only = true },
	{ .name = "shared_ea_without_metabox_is_not_a_probe_check",
	    .patches = {FEATURE(SB_COMPAT, 0x08)}, .status = FSIMG_OK,
	    .region_only = true },
	{ .name = "metabox_without_a_slot_is_not_a_probe_check",
	    .patches = {FEATURE(SB_INCOMPAT, 0x100)}, .status = FSIMG_OK,
	    .region_only = true },
};

/*
 * Every row is classified by the probe, and the region decoder applies
 * the same classification. The probe reads a heap copy of exactly the
 * given length, so a read past the length leaves the allocation.
 */
static void
test_super_probe_classifies_each_case(void)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(super_cases); i++) {
		const super_case_t *c = &super_cases[i];
		unsigned char region[REGION_BYTES];
		size_t length = c->length != 0 ? c->length : SUPER_BYTES;
		unsigned char *exact;
		const char *what = NULL;
		fsimg_status_t status;
		region_result_t r;

		region_init(region);
		patch_apply(region, c->patches);

		exact = malloc(length);
		EXPECT(exact != NULL, "%s: allocate %zu bytes", c->name,
		    length);
		if (exact == NULL)
			continue;
		bcopy(region, exact, length);
		status = erofs_super_probe(exact, length, &what);
		free(exact);
		EXPECT_STATUS(status, c->status, what, c->name);

		if (c->region_only)
			continue;
		/* The full region leaves room for any extension slots. */
		r = region_decode_checked(c->name, region,
		    c->length != 0 ? c->length : REGION_BYTES);
		EXPECT_STATUS(r.status, c->status, r.what, c->name);
	}
}

static void
test_magic_matches_only_the_magic_bytes(void)
{
	unsigned char region[REGION_BYTES];
	size_t i;

	region_init(region);
	EXPECT(erofs_magic_matches(region, 4), "four bytes of magic");
	EXPECT(erofs_magic_matches(region, SUPER_BYTES), "full superblock");
	EXPECT(!erofs_magic_matches(region, 3), "three bytes are too few");
	EXPECT(!erofs_magic_matches(region, 0), "no bytes");
	for (i = 0; i < 4; i++) {
		region[i] ^= 0x01;
		EXPECT(!erofs_magic_matches(region, SUPER_BYTES),
		    "magic byte %zu flipped", i);
		region[i] ^= 0x01;
	}
}

static void
test_super_fields_are_decoded_from_their_offsets(void)
{
	unsigned char region[REGION_BYTES];
	region_result_t r;
	size_t i;

	region_init(region);
	put_le(region + SB_CHECKSUM, 0x11223344, 4);
	put_le(region + SB_COMPAT, 0x80000022, 4);
	region[SB_BLKSZBITS] = 10;
	region[SB_SLOTS] = 2;
	put_le(region + SB_ROOT16, 0x1234, 2);
	put_le(region + SB_INODES, UINT64_C(0x0123456789), 8);
	put_le(region + SB_EPOCH, 1700000000, 8);
	put_le(region + SB_NSEC, 5, 4);
	put_le(region + SB_BLOCKS, 0x00abcdef, 4);
	put_le(region + SB_META_BLOCK, 7, 4);
	put_le(region + SB_XATTR_BLOCK, 9, 4);
	for (i = 0; i < 16; i++)
		region[SB_UUID + i] = (unsigned char)(0xc0 + i);
	bcopy("volume-label", region + SB_NAME, 12);
	put_le(region + SB_INCOMPAT, 0x044, 4);
	put_le(region + SB_WORD54, 0x1234, 2);
	region[SB_PREFIX_COUNT] = 3;
	put_le(region + SB_PREFIX_START, 0x1000, 4);
	put_le(region + SB_PACKED_NID, UINT64_C(0x0000000200000003), 8);
	region[SB_ISHARE] = 0x42;
	put_le(region + SB_BUILD_TIME, 100000000, 4);

	r = region_decode_checked("all_fields", region, REGION_BYTES);

	EXPECT_STATUS(r.status, FSIMG_OK, r.what, "all_fields");
	EXPECT(r.sb.checksum == 0x11223344, "checksum as stored");
	EXPECT(r.sb.compat == 0x80000022, "compat as stored");
	EXPECT(r.sb.incompat == 0x044, "incompat as stored");
	EXPECT(r.sb.blkszbits == 10, "blkszbits");
	EXPECT(r.sb.extension_slots == 2, "extension slots");
	EXPECT(r.sb.root_nid == 0x1234, "root nid from the 16-bit field");
	EXPECT(r.sb.inode_count == UINT64_C(0x0123456789), "inode count");
	EXPECT(r.sb.epoch == 1700000000, "epoch");
	EXPECT(r.sb.epoch_nsec == 5, "epoch nanoseconds");
	EXPECT(r.sb.block_count == 0x00abcdef, "block count");
	EXPECT(r.sb.meta_blkaddr == 7, "metadata block");
	EXPECT(r.sb.xattr_blkaddr == 9, "shared attribute block");
	EXPECT(bcmp(r.sb.uuid, region + SB_UUID, 16) == 0, "uuid");
	EXPECT(bcmp(r.sb.name, region + SB_NAME, 16) == 0, "volume name");
	EXPECT(r.sb.name_length == 12, "volume name length");
	EXPECT(r.sb.compression_word == 0x1234, "compression word");
	EXPECT(r.sb.xattr_prefix_count == 3, "long-prefix count");
	EXPECT(r.sb.xattr_prefix_start == 0x1000, "long-prefix start");
	EXPECT(r.sb.packed_nid == UINT64_C(0x0000000200000003), "packed nid");
	EXPECT(r.sb.ishare_prefix == 0x42, "image-share prefix index");
	EXPECT(r.sb.build_time == 100000000, "build time");
	EXPECT(!r.sb.filter_disabled, "filter byte zero keeps the filter");
	EXPECT(!r.sb.reserved_nonzero, "no reserved byte set");
	EXPECT(r.sb.box_nid == 0, "box nid is zero without the metabox");
}

static void
test_filter_byte_nonzero_disables_the_name_filter(void)
{
	unsigned char region[REGION_BYTES];
	region_result_t r;

	region_init(region);
	region[SB_FILTER_BYTE] = 1;
	r = region_decode_checked("filter_byte_1", region, REGION_BYTES);
	EXPECT(r.status == FSIMG_OK && r.sb.filter_disabled,
	    "filter byte 1 disables the filter");

	region[SB_FILTER_BYTE] = 0x80;
	r = region_decode_checked("filter_byte_0x80", region, REGION_BYTES);
	EXPECT(r.status == FSIMG_OK && r.sb.filter_disabled,
	    "filter byte 0x80 disables the filter");
}

static void
test_name_length_ends_at_the_first_nul_or_the_field_end(void)
{
	static const struct {
		const char	*name;
		unsigned char	 bytes[16];
		unsigned	 length;
	} cases[] = {
		{ "empty_name", { 0 }, 0 },
		{ "abc_then_nul_then_garbage",
		    { 'a', 'b', 'c', 0, 'x', 'y', 'z' }, 3 },
		{ "fifteen_bytes_then_nul",
		    { 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a', 'a',
		    'a', 'a', 'a', 'a', 0 }, 15 },
		{ "sixteen_bytes_without_nul",
		    { 'b', 'b', 'b', 'b', 'b', 'b', 'b', 'b', 'b', 'b', 'b',
		    'b', 'b', 'b', 'b', 'b' }, 16 },
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		unsigned char region[REGION_BYTES];
		region_result_t r;

		region_init(region);
		bcopy(cases[i].bytes, region + SB_NAME, 16);
		r = region_decode_checked(cases[i].name, region, REGION_BYTES);
		EXPECT(r.status == FSIMG_OK &&
		    r.sb.name_length == cases[i].length, "%s", cases[i].name);
	}
}

static void
test_negative_epochs_decode_as_signed(void)
{
	static const int64_t epochs[] = { -1, -86400, INT64_MIN, INT64_MAX };
	size_t i;

	for (i = 0; i < ARRAY_SIZE(epochs); i++) {
		unsigned char region[REGION_BYTES];
		region_result_t r;

		region_init(region);
		put_le(region + SB_EPOCH, (uint64_t)epochs[i], 8);
		r = region_decode_checked("epoch", region, REGION_BYTES);
		EXPECT(r.status == FSIMG_OK && r.sb.epoch == epochs[i],
		    "epoch %lld", (long long)epochs[i]);
	}
}

static void
test_root_nid_without_48bit_is_the_16_bit_field(void)
{
	unsigned char region[REGION_BYTES];
	region_result_t r;

	region_init(region);
	put_le(region + SB_ROOT16, 0xfffe, 2);
	put_le(region + SB_BLOCKS, 0x89abcdef, 4);
	r = region_decode_checked("root16", region, REGION_BYTES);
	EXPECT(r.status == FSIMG_OK && r.sb.root_nid == 0xfffe,
	    "root nid is the 16-bit field");
	EXPECT(r.sb.block_count == 0x89abcdef,
	    "block count is the 32-bit field alone");
	EXPECT(!r.sb.reserved_nonzero, "wide root field zero");
}

static void
test_48bit_widens_root_nid_and_block_count(void)
{
	unsigned char region[REGION_BYTES];
	region_result_t r;

	region_init(region);
	put_le(region + SB_INCOMPAT, EROFS_INCOMPAT_48BIT, 4);
	put_le(region + SB_ROOT16, 0x1234, 2);
	put_le(region + SB_BLOCKS, 0x89abcdef, 4);
	put_le(region + SB_ROOT64, UINT64_C(0x0123456789abcdef), 8);

	r = region_decode_checked("48bit_widths", region, REGION_BYTES);

	EXPECT_STATUS(r.status, FSIMG_OK, r.what, "48bit_widths");
	EXPECT(r.sb.root_nid == UINT64_C(0x0123456789abcdef),
	    "root nid is the 64-bit field");
	EXPECT(r.sb.block_count == UINT64_C(0x123489abcdef),
	    "block count gains the 0x0E word as bits 32 to 47");
	EXPECT(!r.sb.reserved_nonzero, "wide root field is not reserved");
}

static void
test_reserved_superblock_bytes_are_reported_not_refused(void)
{
	static const struct {
		uint16_t	 offset;
		bool		 reserved_without_48bit;
		bool		 reserved_with_48bit;
	} bytes[] = {
		{ SB_RESERVED_6A, true, true },
		{ SB_RESERVED_6A + 1, true, true },
		{ SB_ROOT64, true, false },
		{ SB_ROOT64 + 7, true, false },
		{ SB_RESERVED_78, true, true },
		{ SB_RESERVED_78 + 7, true, true },
	};
	size_t i;
	int wide;

	for (wide = 0; wide < 2; wide++) {
		for (i = 0; i < ARRAY_SIZE(bytes); i++) {
			unsigned char region[REGION_BYTES];
			region_result_t r;
			bool want = wide != 0 ? bytes[i].reserved_with_48bit :
			    bytes[i].reserved_without_48bit;

			region_init(region);
			if (wide != 0)
				put_le(region + SB_INCOMPAT,
				    EROFS_INCOMPAT_48BIT, 4);
			region[bytes[i].offset] = 1;
			r = region_decode_checked("reserved", region,
			    REGION_BYTES);
			EXPECT(r.status == FSIMG_OK &&
			    r.sb.reserved_nonzero == want,
			    "byte 0x%x set, 48bit=%d, reserved_nonzero=%d",
			    (unsigned)bytes[i].offset, wide, (int)want);
		}
	}
}

/* ---- Superblock region, checksum and configuration ---- */

static void
test_extension_slots_past_the_given_length_are_corrupt(void)
{
	unsigned char region[REGION_BYTES];
	region_result_t r;

	region_init(region);
	region[SB_SLOTS] = 2;
	r = region_decode_checked("2_slots_length_159", region, 159);
	EXPECT_STATUS(r.status, FSIMG_ECORRUPT, r.what, "2_slots_length_159");
	r = region_decode_checked("2_slots_length_160", region, 160);
	EXPECT_STATUS(r.status, FSIMG_OK, r.what, "2_slots_length_160");

	region[SB_SLOTS] = 184;
	r = region_decode_checked("184_slots_length_3071", region, 3071);
	EXPECT_STATUS(r.status, FSIMG_ECORRUPT, r.what,
	    "184_slots_length_3071");
	r = region_decode_checked("184_slots_length_3072", region, 3072);
	EXPECT_STATUS(r.status, FSIMG_OK, r.what, "184_slots_length_3072");

	r = region_decode_checked("length_127", region, 127);
	EXPECT_STATUS(r.status, FSIMG_ECORRUPT, r.what, "length_127");
}

/* Under METABOX the first slot holds the box NID and a reserved word. */
static void
metabox_region_init(unsigned char *region)
{
	region_init(region);
	region[SB_SLOTS] = 1;
	put_le(region + SB_INCOMPAT, EROFS_INCOMPAT_METABOX, 4);
}

static void
test_metabox_nid_is_read_from_the_first_slot(void)
{
	unsigned char region[REGION_BYTES];
	region_result_t r;

	metabox_region_init(region);
	put_le(region + SB_BOX_NID, UINT64_C(0x1122334455667788), 8);
	r = region_decode_checked("box_nid", region, REGION_BYTES);
	EXPECT_STATUS(r.status, FSIMG_OK, r.what, "box_nid");
	EXPECT(r.sb.box_nid == UINT64_C(0x1122334455667788), "box nid");
	EXPECT(!r.sb.reserved_nonzero, "slot reserved word zero");

	put_le(region + SB_BOX_NID, UINT64_C(0x7fffffffffffffff), 8);
	r = region_decode_checked("box_nid_max", region, REGION_BYTES);
	EXPECT(r.status == FSIMG_OK &&
	    r.sb.box_nid == UINT64_C(0x7fffffffffffffff),
	    "box nid with bit 63 clear is accepted");
}

static void
test_metabox_nid_with_bit_63_is_corrupt(void)
{
	unsigned char region[REGION_BYTES];
	region_result_t r;

	metabox_region_init(region);
	put_le(region + SB_BOX_NID, UINT64_C(1) << 63, 8);
	r = region_decode_checked("box_nid_bit63", region, REGION_BYTES);
	EXPECT_STATUS(r.status, FSIMG_ECORRUPT, r.what, "box_nid_bit63");
}

static void
test_metabox_slot_reserved_word_is_reported(void)
{
	unsigned char region[REGION_BYTES];
	region_result_t r;

	metabox_region_init(region);
	region[SB_RESERVED_88] = 1;
	r = region_decode_checked("slot_reserved_first", region,
	    REGION_BYTES);
	EXPECT(r.status == FSIMG_OK && r.sb.reserved_nonzero,
	    "byte 0x88 sets reserved_nonzero");

	metabox_region_init(region);
	region[SB_RESERVED_88 + 7] = 0x80;
	r = region_decode_checked("slot_reserved_last", region, REGION_BYTES);
	EXPECT(r.status == FSIMG_OK && r.sb.reserved_nonzero,
	    "byte 0x8f sets reserved_nonzero");
}

static void
test_slot_bytes_are_ignored_without_metabox(void)
{
	unsigned char region[REGION_BYTES];
	region_result_t r;

	region_init(region);
	region[SB_SLOTS] = 1;
	bcopy("\xff\xff\xff\xff\xff\xff\xff\xff\x01\x02\x03\x04\x05\x06\x07"
	    "\x08", region + SB_BOX_NID, 16);
	r = region_decode_checked("slot_without_metabox", region,
	    REGION_BYTES);
	EXPECT_STATUS(r.status, FSIMG_OK, r.what, "slot_without_metabox");
	EXPECT(r.sb.box_nid == 0, "box nid stays zero");
	EXPECT(!r.sb.reserved_nonzero, "slot bytes are not reserved bytes");
}

typedef struct region_case {
	const char	*name;
	size_t		 length;	/* 0: the 3072-byte region */
	patch_t		 patches[MAX_PATCHES];
	fsimg_status_t	 status;
} region_case_t;

/*
 * The rules erofs_region_decode() adds to the base superblock's, in the
 * order it applies them. The row with a wrong checksum pins that the device
 * refusal precedes the checksum.
 */
static const region_case_t region_cases[] = {
	{ .name = "extra_device_count_1_is_unsupported",
	    .patches = {{ SB_DEVICES, 2, 1 }}, .status = FSIMG_EUNSUPPORTED },
	{ .name = "extra_device_count_65535_is_unsupported",
	    .patches = {{ SB_DEVICES, 2, 0xffff }},
	    .status = FSIMG_EUNSUPPORTED },
	{ .name = "extra_devices_are_refused_before_a_wrong_checksum",
	    .patches = {{ SB_DEVICES, 2, 1 },
	    { SB_COMPAT, 4, EROFS_COMPAT_SB_CHKSUM },
	    { SB_CHECKSUM, 4, 0xdeadbeef }},
	    .status = FSIMG_EUNSUPPORTED },
	{ .name = "truncated_slots_are_corrupt_before_extra_devices",
	    .length = 159,
	    .patches = {{ SB_SLOTS, 1, 2 }, { SB_DEVICES, 2, 1 }},
	    .status = FSIMG_ECORRUPT },
	{ .name = "epoch_nanoseconds_999999999_are_accepted",
	    .patches = {{ SB_NSEC, 4, 999999999 }}, .status = FSIMG_OK },
	{ .name = "epoch_nanoseconds_1000000000_are_corrupt",
	    .patches = {{ SB_NSEC, 4, 1000000000 }}, .status = FSIMG_ECORRUPT },
	{ .name = "epoch_nanoseconds_4294967295_are_corrupt",
	    .patches = {{ SB_NSEC, 4, 0xffffffff }}, .status = FSIMG_ECORRUPT },
	{ .name = "metabox_without_a_slot_is_corrupt",
	    .patches = {{ SB_INCOMPAT, 4, EROFS_INCOMPAT_METABOX }},
	    .status = FSIMG_ECORRUPT },
	{ .name = "metabox_with_a_slot_is_accepted",
	    .patches = {{ SB_INCOMPAT, 4, EROFS_INCOMPAT_METABOX },
	    { SB_SLOTS, 1, 1 }}, .status = FSIMG_OK },
	{ .name = "shared_ea_in_metabox_without_metabox_is_corrupt",
	    .patches = {{ SB_COMPAT, 4, EROFS_COMPAT_SHARED_EA_IN_METABOX }},
	    .status = FSIMG_ECORRUPT },
	{ .name = "shared_ea_in_metabox_with_metabox_is_accepted",
	    .patches = {{ SB_COMPAT, 4, EROFS_COMPAT_SHARED_EA_IN_METABOX },
	    { SB_INCOMPAT, 4, EROFS_INCOMPAT_METABOX }, { SB_SLOTS, 1, 1 }},
	    .status = FSIMG_OK },
};

static void
test_region_decode_applies_the_rules_beyond_the_base_superblock(void)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(region_cases); i++) {
		const region_case_t *c = &region_cases[i];
		unsigned char region[REGION_BYTES];
		region_result_t r;

		region_init(region);
		patch_apply(region, c->patches);
		r = region_decode_checked(c->name, region,
		    c->length != 0 ? c->length : REGION_BYTES);
		EXPECT_STATUS(r.status, c->status, r.what, c->name);
	}
}

/* ---- Checksum, over the real regions of e1-chk-b512 .. b4096 ---- */

/* e1-chk-b512: image bytes [1024, 1536), the checksummed range */
static const unsigned char chk512_bytes[] = {
	/* image offset 1024, 17 bytes */
	0xe2, 0xe1, 0xf5, 0xe0, 0x3d, 0xdd, 0xc0, 0x28, 0x03, 0x00,
	0x00, 0x00, 0x09, 0x00, 0x24, 0x00, 0x07,
	/* image offset 1060, 28 bytes */
	0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x33, 0x33,
	0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
	/* image offset 1152, 140 bytes */
	0x04, 0x00, 0x00, 0x00, 0xed, 0x41, 0x03, 0x00, 0x6c, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x01, 0x00, 0x00, 0x00, 0xf5, 0x01, 0x14, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x54, 0x00, 0x02, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x55, 0x00, 0x02, 0x00, 0x29, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x57, 0x00, 0x01, 0x00, 0x2b, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5c, 0x00, 0x01, 0x00,
	0x2d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x63, 0x00,
	0x02, 0x00, 0xf0, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x64, 0x00, 0x07, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x65, 0x00, 0x01, 0x00, 0x2e, 0x2e, 0x2e, 0x61,
	0x2e, 0x74, 0x78, 0x74, 0x62, 0x69, 0x67, 0x2e, 0x62, 0x69,
	0x6e, 0x64, 0x6c, 0x6d, 0x69, 0x64, 0x2e, 0x62, 0x69, 0x6e,
	/* image offset 1312, 38 bytes */
	0x04, 0x00, 0x00, 0x00, 0xa4, 0x81, 0x01, 0x00, 0x06, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x02, 0x00, 0x00, 0x00, 0xf5, 0x01, 0x14, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x0a,
	/* image offset 1376, 140 bytes */
	0x04, 0x00, 0x00, 0x00, 0xa4, 0x81, 0x01, 0x00, 0x20, 0x4e,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
	0x03, 0x00, 0x00, 0x00, 0xf5, 0x01, 0x14, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x12, 0xf8, 0x58, 0x8d, 0xad, 0x6c, 0x5b, 0x4a,
	0xc1, 0x9a, 0x71, 0x91, 0x70, 0x84, 0x5f, 0x18, 0x1b, 0x61,
	0xea, 0x57, 0xe0, 0x36, 0x87, 0xa5, 0x53, 0x0c, 0xc2, 0x59,
	0xba, 0x13, 0xe9, 0x8b, 0x04, 0x00, 0x00, 0x00, 0xed, 0x41,
	0x02, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0x04, 0x00, 0x00, 0x00, 0xf5, 0x01,
	0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2d, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x24, 0x00, 0x02, 0x00, 0x24, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x25, 0x00, 0x02, 0x00,
	0xf2, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x27, 0x00,
	0x01, 0x00, 0x2e, 0x2e, 0x2e, 0x62, 0x2e, 0x74, 0x78, 0x74,
};
static const fixture_run_t chk512_runs[] = {
	{ 1024, 0, 17 },
	{ 1060, 17, 28 },
	{ 1152, 45, 140 },
	{ 1312, 185, 38 },
	{ 1376, 223, 140 },
};

/* e1-chk-b1024: image bytes [1024, 2048), the checksummed range */
static const unsigned char chk1024_bytes[] = {
	/* image offset 1024, 17 bytes */
	0xe2, 0xe1, 0xf5, 0xe0, 0xd1, 0x67, 0x5f, 0x8c, 0x03, 0x00,
	0x00, 0x00, 0x0a, 0x00, 0x24, 0x00, 0x07,
	/* image offset 1060, 28 bytes */
	0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x33, 0x33,
	0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
	/* image offset 1152, 140 bytes */
	0x04, 0x00, 0x00, 0x00, 0xed, 0x41, 0x03, 0x00, 0x6c, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x01, 0x00, 0x00, 0x00, 0xf5, 0x01, 0x14, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x54, 0x00, 0x02, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x55, 0x00, 0x02, 0x00, 0x29, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x57, 0x00, 0x01, 0x00, 0x2b, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5c, 0x00, 0x01, 0x00,
	0x3d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x63, 0x00,
	0x02, 0x00, 0xe0, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x64, 0x00, 0x07, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x65, 0x00, 0x01, 0x00, 0x2e, 0x2e, 0x2e, 0x61,
	0x2e, 0x74, 0x78, 0x74, 0x62, 0x69, 0x67, 0x2e, 0x62, 0x69,
	0x6e, 0x64, 0x6c, 0x6d, 0x69, 0x64, 0x2e, 0x62, 0x69, 0x6e,
	/* image offset 1312, 38 bytes */
	0x04, 0x00, 0x00, 0x00, 0xa4, 0x81, 0x01, 0x00, 0x06, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x02, 0x00, 0x00, 0x00, 0xf5, 0x01, 0x14, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x0a,
	/* image offset 1376, 652 bytes */
	0x04, 0x00, 0x00, 0x00, 0xa4, 0x81, 0x01, 0x00, 0x20, 0x4e,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x03, 0x00, 0x00, 0x00, 0xf5, 0x01, 0x14, 0x00, 0x00, 0x00,
	0x00, 0x00, 0xbf, 0xde, 0x83, 0x1d, 0x0d, 0x53, 0x0b, 0x9f,
	0x23, 0xdf, 0x45, 0x35, 0x3d, 0x4c, 0x9f, 0x92, 0x9b, 0xfa,
	0xd8, 0xbe, 0x85, 0xa4, 0x49, 0x2e, 0x30, 0xa8, 0x45, 0x67,
	0xa4, 0xac, 0x7a, 0x1d, 0xd2, 0x3a, 0x06, 0x7c, 0x43, 0xd8,
	0x51, 0xdd, 0x86, 0x41, 0x30, 0xe3, 0x26, 0x93, 0xee, 0x47,
	0x12, 0x80, 0x2f, 0x23, 0x47, 0x32, 0x75, 0xf5, 0xb5, 0x22,
	0xe1, 0xf6, 0x0f, 0x3d, 0x67, 0xcf, 0x6c, 0xc7, 0xe9, 0x0a,
	0x34, 0xd8, 0x3d, 0x09, 0xae, 0xea, 0x74, 0x19, 0xe1, 0x42,
	0x86, 0x66, 0x72, 0x27, 0x26, 0x39, 0xca, 0xe5, 0xcb, 0x00,
	0x35, 0x5c, 0x7c, 0xce, 0x03, 0xc6, 0x00, 0x2e, 0x98, 0x14,
	0xb4, 0x74, 0xab, 0x40, 0x4d, 0x76, 0x99, 0x77, 0x3b, 0xa7,
	0x9a, 0x32, 0xb2, 0x20, 0x78, 0x4f, 0xa0, 0x2c, 0x01, 0x9a,
	0x31, 0x05, 0xe5, 0x49, 0xae, 0x2a, 0xfe, 0x10, 0x90, 0x0b,
	0x06, 0x51, 0x56, 0x85, 0x12, 0xcd, 0xd5, 0xe6, 0x22, 0xe2,
	0x83, 0xbd, 0x1f, 0x5a, 0xb9, 0xc7, 0xe0, 0xb9, 0x64, 0x76,
	0xe8, 0x49, 0xab, 0xfa, 0x50, 0x07, 0x9f, 0xde, 0x62, 0x9d,
	0xe3, 0xbd, 0xab, 0xc1, 0x10, 0xa9, 0x33, 0x4e, 0x63, 0x6a,
	0x24, 0x5e, 0xc4, 0x3f, 0x7f, 0x38, 0x45, 0xbd, 0x52, 0x8d,
	0x22, 0x3c, 0xfe, 0x8e, 0xd2, 0x60, 0x88, 0x86, 0xc1, 0x5f,
	0x02, 0x3e, 0xf6, 0x80, 0x13, 0x97, 0xb1, 0xc1, 0x3e, 0xb1,
	0x5d, 0x0b, 0xde, 0xae, 0x1c, 0xf0, 0x4b, 0x58, 0x2d, 0x4f,
	0x19, 0xdf, 0xa5, 0xdc, 0xa3, 0x99, 0x67, 0xfa, 0xcf, 0xec,
	0xf8, 0x42, 0x74, 0x8d, 0xd4, 0x36, 0xbe, 0x1c, 0x8b, 0x69,
	0x79, 0xb1, 0xfb, 0x03, 0x68, 0x27, 0x8e, 0x9c, 0xcc, 0x72,
	0x9b, 0x99, 0xcc, 0x57, 0xfd, 0x8b, 0x4a, 0x57, 0x70, 0x40,
	0xc6, 0x85, 0xa7, 0x63, 0x2f, 0x34, 0x78, 0xbd, 0x4e, 0xb6,
	0xf6, 0x19, 0x1f, 0x50, 0xb8, 0x3e, 0xfb, 0xac, 0x55, 0xb6,
	0x21, 0x9a, 0x0e, 0x6d, 0xab, 0x06, 0x90, 0x16, 0xfe, 0xaa,
	0x7f, 0x75, 0x09, 0x40, 0xb7, 0xdd, 0x7f, 0x04, 0xb6, 0xd7,
	0xef, 0xf4, 0x0a, 0xdd, 0xb8, 0x44, 0x16, 0xe8, 0xe6, 0x07,
	0x5d, 0xb5, 0xd6, 0x1a, 0x12, 0xa3, 0x2e, 0x52, 0x86, 0x7c,
	0xf8, 0x49, 0xcc, 0x23, 0xfa, 0x44, 0xfb, 0xb2, 0xa5, 0x3e,
	0x79, 0x8c, 0x24, 0xdf, 0x1d, 0x65, 0x8a, 0x96, 0xd7, 0xcb,
	0x67, 0xb2, 0x28, 0xcc, 0x32, 0xa0, 0xd9, 0x19, 0x4d, 0xa1,
	0x52, 0xc9, 0x93, 0xf1, 0x42, 0x0b, 0xad, 0xb8, 0xd0, 0x27,
	0xfe, 0xa7, 0xa4, 0x2a, 0xa2, 0x7d, 0x5c, 0x51, 0x23, 0x22,
	0xce, 0x86, 0x07, 0x48, 0x0f, 0x58, 0xf2, 0x30, 0x5c, 0x75,
	0xc7, 0x43, 0x3a, 0x5d, 0x81, 0xae, 0x21, 0x17, 0xe6, 0xae,
	0x0f, 0x91, 0x03, 0x98, 0x2f, 0x33, 0x2e, 0x07, 0xac, 0x0a,
	0xbd, 0x66, 0x3a, 0xd2, 0x86, 0xc6, 0xf3, 0xa8, 0x83, 0xed,
	0x66, 0x3a, 0x88, 0x23, 0x9e, 0x33, 0x24, 0x96, 0xdd, 0x97,
	0x54, 0x4a, 0x9d, 0x6a, 0x30, 0x2f, 0x2d, 0x16, 0x7c, 0xb3,
	0x6d, 0x73, 0xea, 0xa2, 0x6a, 0xed, 0x51, 0x28, 0x46, 0x59,
	0x8e, 0x2f, 0x93, 0xb3, 0x8f, 0xa8, 0x1e, 0x62, 0xe3, 0xba,
	0x9a, 0x69, 0xb0, 0x89, 0x28, 0x2e, 0xab, 0xc9, 0xe4, 0xd4,
	0xab, 0xf8, 0x70, 0x11, 0x39, 0xa6, 0x0e, 0x83, 0xd5, 0x27,
	0x1f, 0x8d, 0xeb, 0x68, 0x0e, 0xf7, 0x7d, 0xbc, 0x8a, 0x98,
	0xfa, 0x04, 0x87, 0xa1, 0x7f, 0x79, 0x3f, 0x25, 0xc3, 0xce,
	0x41, 0x22, 0xee, 0xfe, 0x1e, 0x3b, 0x2e, 0x08, 0x35, 0x64,
	0xf2, 0x02, 0x50, 0x49, 0xb2, 0xe9, 0x33, 0xf2, 0x02, 0x1f,
	0xac, 0xd4, 0x00, 0xc7, 0xb1, 0xff, 0x51, 0x98, 0x46, 0x27,
	0xba, 0x47, 0x83, 0x26, 0x12, 0xf8, 0x58, 0x8d, 0xad, 0x6c,
	0x5b, 0x4a, 0xc1, 0x9a, 0x71, 0x91, 0x70, 0x84, 0x5f, 0x18,
	0x1b, 0x61, 0xea, 0x57, 0xe0, 0x36, 0x87, 0xa5, 0x53, 0x0c,
	0xc2, 0x59, 0xba, 0x13, 0xe9, 0x8b, 0x04, 0x00, 0x00, 0x00,
	0xed, 0x41, 0x02, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x04, 0x00, 0x00, 0x00,
	0xf5, 0x01, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3d, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x00, 0x02, 0x00,
	0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x25, 0x00,
	0x02, 0x00, 0xe2, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x27, 0x00, 0x01, 0x00, 0x2e, 0x2e, 0x2e, 0x62, 0x2e, 0x74,
	0x78, 0x74,
};
static const fixture_run_t chk1024_runs[] = {
	{ 1024, 0, 17 },
	{ 1060, 17, 28 },
	{ 1152, 45, 140 },
	{ 1312, 185, 38 },
	{ 1376, 223, 652 },
};

/* e1-chk-b2048: image bytes [1024, 2048), the checksummed range */
static const unsigned char chk2048_bytes[] = {
	/* image offset 1024, 17 bytes */
	0xe2, 0xe1, 0xf5, 0xe0, 0x53, 0xcc, 0xb3, 0xb0, 0x03, 0x00,
	0x00, 0x00, 0x0b, 0x00, 0x24, 0x00, 0x07,
	/* image offset 1060, 28 bytes */
	0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x33, 0x33,
	0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
	/* image offset 1152, 140 bytes */
	0x04, 0x00, 0x00, 0x00, 0xed, 0x41, 0x03, 0x00, 0x6c, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x01, 0x00, 0x00, 0x00, 0xf5, 0x01, 0x14, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x54, 0x00, 0x02, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x55, 0x00, 0x02, 0x00, 0x29, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x57, 0x00, 0x01, 0x00, 0xc0, 0x02,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5c, 0x00, 0x01, 0x00,
	0xf2, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x63, 0x00,
	0x02, 0x00, 0xf5, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x64, 0x00, 0x07, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x65, 0x00, 0x01, 0x00, 0x2e, 0x2e, 0x2e, 0x61,
	0x2e, 0x74, 0x78, 0x74, 0x62, 0x69, 0x67, 0x2e, 0x62, 0x69,
	0x6e, 0x64, 0x6c, 0x6d, 0x69, 0x64, 0x2e, 0x62, 0x69, 0x6e,
	/* image offset 1312, 38 bytes */
	0x04, 0x00, 0x00, 0x00, 0xa4, 0x81, 0x01, 0x00, 0x06, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x02, 0x00, 0x00, 0x00, 0xf5, 0x01, 0x14, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x0a,
};
static const fixture_run_t chk2048_runs[] = {
	{ 1024, 0, 17 },
	{ 1060, 17, 28 },
	{ 1152, 45, 140 },
	{ 1312, 185, 38 },
};

/* e1-chk-b4096: image bytes [1024, 4096), the checksummed range */
static const unsigned char chk4096_bytes[] = {
	/* image offset 1024, 17 bytes */
	0xe2, 0xe1, 0xf5, 0xe0, 0xe3, 0x76, 0x4f, 0x0d, 0x03, 0x00,
	0x00, 0x00, 0x0c, 0x00, 0x24, 0x00, 0x07,
	/* image offset 1060, 28 bytes */
	0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x33, 0x33,
	0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
	/* image offset 1152, 140 bytes */
	0x04, 0x00, 0x00, 0x00, 0xed, 0x41, 0x03, 0x00, 0x6c, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x01, 0x00, 0x00, 0x00, 0xf5, 0x01, 0x14, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x54, 0x00, 0x02, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x55, 0x00, 0x02, 0x00, 0x29, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x57, 0x00, 0x01, 0x00, 0x80, 0x02,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5c, 0x00, 0x01, 0x00,
	0xf2, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x63, 0x00,
	0x02, 0x00, 0xf5, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x64, 0x00, 0x07, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x65, 0x00, 0x01, 0x00, 0x2e, 0x2e, 0x2e, 0x61,
	0x2e, 0x74, 0x78, 0x74, 0x62, 0x69, 0x67, 0x2e, 0x62, 0x69,
	0x6e, 0x64, 0x6c, 0x6d, 0x69, 0x64, 0x2e, 0x62, 0x69, 0x6e,
	/* image offset 1312, 38 bytes */
	0x04, 0x00, 0x00, 0x00, 0xa4, 0x81, 0x01, 0x00, 0x06, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x02, 0x00, 0x00, 0x00, 0xf5, 0x01, 0x14, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x0a,
};
static const fixture_run_t chk4096_runs[] = {
	{ 1024, 0, 17 },
	{ 1060, 17, 28 },
	{ 1152, 45, 140 },
	{ 1312, 185, 38 },
};


typedef struct checksum_fixture {
	const char	*name;
	unsigned	 blkszbits;
	fixture_t	 fixture;
} checksum_fixture_t;

static const checksum_fixture_t checksum_fixtures[] = {
	{ "e1-chk-b512", 9, FIXTURE(chk512, 1024) },
	{ "e1-chk-b1024", 10, FIXTURE(chk1024, 1024) },
	{ "e1-chk-b2048", 11, FIXTURE(chk2048, 1024) },
	{ "e1-chk-b4096", 12, FIXTURE(chk4096, 1024) },
};

/*
 * The region of one fixture: its covered range from the real image and zero
 * beyond, which the checksum does not see.
 */
static void
checksum_region_load(const checksum_fixture_t *f, unsigned char *region)
{
	fixture_load(&f->fixture, region, covered_bytes(f->blkszbits));
	bzero(region + covered_bytes(f->blkszbits),
	    REGION_BYTES - covered_bytes(f->blkszbits));
}

static void
test_crc32c_oracle_matches_the_published_check_value(void)
{
	EXPECT((crc32c_register((const unsigned char *)"123456789", 9) ^
	    UINT32_C(0xffffffff)) == UINT32_C(0xe3069283),
	    "inverted register of 123456789");
	EXPECT(crc32c_register((const unsigned char *)"123456789", 9) ==
	    UINT32_C(0x1cf96d7c), "stored form of 123456789");
}

static void
test_real_checksummed_regions_are_accepted(void)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(checksum_fixtures); i++) {
		const checksum_fixture_t *f = &checksum_fixtures[i];
		unsigned char region[REGION_BYTES];
		region_result_t r;

		checksum_region_load(f, region);
		r = region_decode_checked(f->name, region, REGION_BYTES);
		EXPECT_STATUS(r.status, FSIMG_OK, r.what, f->name);
		EXPECT(r.status != FSIMG_OK ||
		    (r.sb.compat & EROFS_COMPAT_SB_CHKSUM) != 0,
		    "%s: fixture sets SB_CHKSUM", f->name);
		EXPECT(r.status != FSIMG_OK ||
		    r.sb.checksum == expected_checksum(region, f->blkszbits),
		    "%s: stored checksum equals the independent CRC", f->name);
	}
}

static void
test_flipped_bit_inside_the_covered_range_is_corrupt(void)
{
	size_t i, j;

	for (i = 0; i < ARRAY_SIZE(checksum_fixtures); i++) {
		const checksum_fixture_t *f = &checksum_fixtures[i];
		size_t covered = covered_bytes(f->blkszbits);
		size_t positions[] = { 0, 9, covered / 2, covered - 1 };

		for (j = 0; j < ARRAY_SIZE(positions); j++) {
			unsigned char region[REGION_BYTES];
			region_result_t r;

			checksum_region_load(f, region);
			region[positions[j]] ^= 0x01;
			r = region_decode_checked(f->name, region,
			    REGION_BYTES);
			EXPECT(r.status == FSIMG_ECORRUPT,
			    "%s: bit flipped in byte %zu: %s", f->name,
			    positions[j], status_name(r.status));
		}
	}
}

static void
test_flipped_checksum_field_is_corrupt(void)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(checksum_fixtures); i++) {
		const checksum_fixture_t *f = &checksum_fixtures[i];
		unsigned char region[REGION_BYTES];
		region_result_t r;

		checksum_region_load(f, region);
		region[SB_CHECKSUM + 3] ^= 0x80;
		r = region_decode_checked(f->name, region, REGION_BYTES);
		EXPECT_STATUS(r.status, FSIMG_ECORRUPT, r.what, f->name);
	}
}

static void
test_flipped_bit_just_past_the_covered_range_is_accepted(void)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(checksum_fixtures); i++) {
		const checksum_fixture_t *f = &checksum_fixtures[i];
		size_t covered = covered_bytes(f->blkszbits);
		unsigned char region[REGION_BYTES];
		region_result_t r;

		if (covered == REGION_BYTES)
			continue;
		checksum_region_load(f, region);
		region[covered] ^= 0x01;
		r = region_decode_checked(f->name, region, REGION_BYTES);
		EXPECT_STATUS(r.status, FSIMG_OK, r.what, f->name);

		region[covered] ^= 0x01;
		region[REGION_BYTES - 1] ^= 0x80;
		r = region_decode_checked(f->name, region, REGION_BYTES);
		EXPECT_STATUS(r.status, FSIMG_OK, r.what, f->name);
	}
}

static void
test_region_shorter_than_the_covered_range_is_corrupt(void)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(checksum_fixtures); i++) {
		const checksum_fixture_t *f = &checksum_fixtures[i];
		size_t covered = covered_bytes(f->blkszbits);
		unsigned char region[REGION_BYTES];
		region_result_t r;

		checksum_region_load(f, region);
		r = region_decode_checked(f->name, region, covered - 1);
		EXPECT_STATUS(r.status, FSIMG_ECORRUPT, r.what, f->name);
		r = region_decode_checked(f->name, region, covered);
		EXPECT_STATUS(r.status, FSIMG_OK, r.what, f->name);
	}
}

static void
test_wrong_checksum_without_sb_chksum_is_accepted(void)
{
	unsigned char region[REGION_BYTES];
	region_result_t r;

	region_init(region);
	put_le(region + SB_CHECKSUM, 0xdeadbeef, 4);
	r = region_decode_checked("wrong_checksum_no_feature", region,
	    REGION_BYTES);
	EXPECT_STATUS(r.status, FSIMG_OK, r.what, "wrong_checksum_no_feature");
	EXPECT(r.sb.checksum == 0xdeadbeef, "checksum reported as stored");

	put_le(region + SB_COMPAT, EROFS_COMPAT_SB_CHKSUM, 4);
	r = region_decode_checked("wrong_checksum_with_feature", region,
	    REGION_BYTES);
	EXPECT_STATUS(r.status, FSIMG_ECORRUPT, r.what,
	    "wrong_checksum_with_feature");
}

static void
test_super_checksum_equals_the_independent_crc(void)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(checksum_fixtures); i++) {
		const checksum_fixture_t *f = &checksum_fixtures[i];
		size_t covered = covered_bytes(f->blkszbits);
		unsigned char region[REGION_BYTES];
		uint32_t value = 0;

		checksum_region_load(f, region);
		EXPECT(erofs_super_checksum(region, REGION_BYTES,
		    f->blkszbits, &value) &&
		    value == expected_checksum(region, f->blkszbits),
		    "%s: whole region", f->name);

		value = 0;
		EXPECT(erofs_super_checksum(region, covered, f->blkszbits,
		    &value) && value == expected_checksum(region, f->blkszbits),
		    "%s: region of exactly the covered length", f->name);

		value = 0x12345678;
		EXPECT(!erofs_super_checksum(region, covered - 1,
		    f->blkszbits, &value) && value == 0x12345678,
		    "%s: one byte short is false and leaves the output",
		    f->name);

		put_le(region + SB_CHECKSUM, 0xa5a5a5a5, 4);
		EXPECT(erofs_super_checksum(region, REGION_BYTES,
		    f->blkszbits, &value) &&
		    value == expected_checksum(region, f->blkszbits),
		    "%s: the stored field counts as zero", f->name);
	}
}

static void
test_super_checksum_covers_the_range_of_every_block_size(void)
{
	unsigned char region[REGION_BYTES];
	unsigned bits;
	size_t i;

	for (i = 0; i < sizeof(region); i++)
		region[i] = (unsigned char)(i * 7 + (i >> 8));
	for (bits = 9; bits <= 12; bits++) {
		uint32_t value = 0;

		EXPECT(erofs_super_checksum(region, REGION_BYTES, bits,
		    &value) && value == expected_checksum(region, bits),
		    "blkszbits %u", bits);
	}
	EXPECT(!erofs_super_checksum(region, 0, 9, &(uint32_t){ 0 }),
	    "empty region");
}

static void
test_super_checksum_refuses_block_sizes_outside_9_to_12(void)
{
	static const unsigned bits[] = { 0, 8, 13, 255 };
	unsigned char region[REGION_BYTES];
	size_t i;

	bzero(region, sizeof(region));
	for (i = 0; i < ARRAY_SIZE(bits); i++) {
		uint32_t value = 0x12345678;

		EXPECT(!erofs_super_checksum(region, REGION_BYTES, bits[i],
		    &value) && value == 0x12345678,
		    "blkszbits %u is false and leaves the output", bits[i]);
	}
}

/* ---- Compression configuration ---- */

#define LE32(v)	(uint8_t)(v), (uint8_t)((v) >> 8), (uint8_t)((v) >> 16), \
		    (uint8_t)((v) >> 24)

/* A configured volume: COMPR_CFGS set, the mask in the word at 0x54. */
static void
config_region_init(unsigned char *region, unsigned slots, unsigned mask)
{
	region_init(region);
	region[SB_SLOTS] = (unsigned char)slots;
	put_le(region + SB_INCOMPAT, EROFS_INCOMPAT_COMPR_CFGS, 4);
	put_le(region + SB_WORD54, mask, 2);
}

static bool
config_equal(const erofs_config_t *a, const erofs_config_t *b)
{
	return (a->lzma_dict_size == b->lzma_dict_size &&
	    a->lz4_max_distance == b->lz4_max_distance &&
	    a->lz4_max_pcluster_blocks == b->lz4_max_pcluster_blocks &&
	    a->algorithms == b->algorithms &&
	    a->deflate_window_bits == b->deflate_window_bits &&
	    a->zstd_window_log == b->zstd_window_log &&
	    a->reserved_nonzero == b->reserved_nonzero);
}

/*
 * One algorithm's record alone, at the first position after a bare
 * superblock: length word, payload and the decoded result when accepted.
 */
typedef struct payload_case {
	const char	*name;
	unsigned	 algorithm;
	uint16_t	 length_word;	/* 0, or left out: the natural length */
	unsigned char	 payload[14];
	fsimg_status_t	 status;
	erofs_config_t	 expect;
} payload_case_t;

static const payload_case_t payload_cases[] = {
	{ .name = "lz4_distance_0xffff_and_one_block",
	    .algorithm = EROFS_ALGORITHM_LZ4,
	    .payload = { 0xff, 0xff, 0x01, 0x00 }, .status = FSIMG_OK,
	    .expect = { .algorithms = 1, .lz4_max_distance = 0xffff,
	    .lz4_max_pcluster_blocks = 1 } },
	{ .name = "lz4_reserved_byte_nonzero_is_reported",
	    .algorithm = EROFS_ALGORITHM_LZ4,
	    .payload = { 0x00, 0x20, 0x04, 0x00, 0x01 }, .status = FSIMG_OK,
	    .expect = { .algorithms = 1, .lz4_max_distance = 0x2000,
	    .lz4_max_pcluster_blocks = 4, .reserved_nonzero = true } },
	{ .name = "lz4_last_reserved_byte_nonzero_is_reported",
	    .algorithm = EROFS_ALGORITHM_LZ4,
	    .payload = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x80 },
	    .status = FSIMG_OK,
	    .expect = { .algorithms = 1, .reserved_nonzero = true } },
	{ .name = "lz4_payload_length_13_is_unsupported",
	    .algorithm = EROFS_ALGORITHM_LZ4, .length_word = 13,
	    .payload = { 0 }, .status = FSIMG_EUNSUPPORTED },
	{ .name = "lz4_payload_length_15_is_unsupported",
	    .algorithm = EROFS_ALGORITHM_LZ4, .length_word = 15,
	    .payload = { 0 }, .status = FSIMG_EUNSUPPORTED },
	{ .name = "lzma_dictionary_4095_is_unsupported",
	    .algorithm = EROFS_ALGORITHM_LZMA, .payload = { LE32(4095) },
	    .status = FSIMG_EUNSUPPORTED },
	{ .name = "lzma_dictionary_4096_is_accepted",
	    .algorithm = EROFS_ALGORITHM_LZMA, .payload = { LE32(4096) },
	    .status = FSIMG_OK,
	    .expect = { .algorithms = 2, .lzma_dict_size = 4096 } },
	{ .name = "lzma_dictionary_8388608_is_accepted",
	    .algorithm = EROFS_ALGORITHM_LZMA, .payload = { LE32(8388608) },
	    .status = FSIMG_OK,
	    .expect = { .algorithms = 2, .lzma_dict_size = 8388608 } },
	{ .name = "lzma_dictionary_8388609_is_unsupported",
	    .algorithm = EROFS_ALGORITHM_LZMA, .payload = { LE32(8388609) },
	    .status = FSIMG_EUNSUPPORTED },
	{ .name = "lzma_dictionary_0_is_unsupported",
	    .algorithm = EROFS_ALGORITHM_LZMA, .payload = { LE32(0) },
	    .status = FSIMG_EUNSUPPORTED },
	{ .name = "lzma_dictionary_4294967295_is_unsupported",
	    .algorithm = EROFS_ALGORITHM_LZMA, .payload = { LE32(0xffffffffu) },
	    .status = FSIMG_EUNSUPPORTED },
	{ .name = "lzma_format_1_is_unsupported",
	    .algorithm = EROFS_ALGORITHM_LZMA,
	    .payload = { LE32(65536), 0x01, 0x00 },
	    .status = FSIMG_EUNSUPPORTED },
	{ .name = "lzma_reserved_byte_nonzero_is_reported",
	    .algorithm = EROFS_ALGORITHM_LZMA,
	    .payload = { LE32(65536), 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x00, 0x40 },
	    .status = FSIMG_OK,
	    .expect = { .algorithms = 2, .lzma_dict_size = 65536,
	    .reserved_nonzero = true } },
	{ .name = "lzma_payload_length_6_is_unsupported",
	    .algorithm = EROFS_ALGORITHM_LZMA, .length_word = 6,
	    .payload = { LE32(65536) }, .status = FSIMG_EUNSUPPORTED },
	{ .name = "deflate_window_bits_15_are_accepted",
	    .algorithm = EROFS_ALGORITHM_DEFLATE, .payload = { 15 },
	    .status = FSIMG_OK,
	    .expect = { .algorithms = 4, .deflate_window_bits = 15 } },
	{ .name = "deflate_window_bits_7_are_accepted",
	    .algorithm = EROFS_ALGORITHM_DEFLATE, .payload = { 7 },
	    .status = FSIMG_OK,
	    .expect = { .algorithms = 4, .deflate_window_bits = 7 } },
	{ .name = "deflate_window_bits_16_are_unsupported",
	    .algorithm = EROFS_ALGORITHM_DEFLATE, .payload = { 16 },
	    .status = FSIMG_EUNSUPPORTED },
	{ .name = "deflate_window_bits_255_are_unsupported",
	    .algorithm = EROFS_ALGORITHM_DEFLATE, .payload = { 255 },
	    .status = FSIMG_EUNSUPPORTED },
	{ .name = "deflate_reserved_byte_nonzero_is_reported",
	    .algorithm = EROFS_ALGORITHM_DEFLATE,
	    .payload = { 15, 0, 0, 0, 0, 0x01 }, .status = FSIMG_OK,
	    .expect = { .algorithms = 4, .deflate_window_bits = 15,
	    .reserved_nonzero = true } },
	{ .name = "deflate_payload_length_14_is_unsupported",
	    .algorithm = EROFS_ALGORITHM_DEFLATE, .length_word = 14,
	    .payload = { 15 }, .status = FSIMG_EUNSUPPORTED },
	{ .name = "deflate_payload_length_5_is_unsupported",
	    .algorithm = EROFS_ALGORITHM_DEFLATE, .length_word = 5,
	    .payload = { 15 }, .status = FSIMG_EUNSUPPORTED },
	{ .name = "zstd_window_field_10_gives_log_20",
	    .algorithm = EROFS_ALGORITHM_ZSTD, .payload = { 0x00, 10 },
	    .status = FSIMG_OK,
	    .expect = { .algorithms = 8, .zstd_window_log = 20 } },
	{ .name = "zstd_window_field_0_gives_log_10",
	    .algorithm = EROFS_ALGORITHM_ZSTD, .payload = { 0x00, 0 },
	    .status = FSIMG_OK,
	    .expect = { .algorithms = 8, .zstd_window_log = 10 } },
	{ .name = "zstd_window_field_11_is_unsupported",
	    .algorithm = EROFS_ALGORITHM_ZSTD, .payload = { 0x00, 11 },
	    .status = FSIMG_EUNSUPPORTED },
	{ .name = "zstd_window_field_255_is_unsupported",
	    .algorithm = EROFS_ALGORITHM_ZSTD, .payload = { 0x00, 255 },
	    .status = FSIMG_EUNSUPPORTED },
	{ .name = "zstd_format_1_is_unsupported",
	    .algorithm = EROFS_ALGORITHM_ZSTD, .payload = { 0x01, 5 },
	    .status = FSIMG_EUNSUPPORTED },
	{ .name = "zstd_reserved_byte_nonzero_is_reported",
	    .algorithm = EROFS_ALGORITHM_ZSTD,
	    .payload = { 0x00, 5, 0, 0, 0, 0x02 }, .status = FSIMG_OK,
	    .expect = { .algorithms = 8, .zstd_window_log = 15,
	    .reserved_nonzero = true } },
	{ .name = "zstd_payload_length_7_is_unsupported",
	    .algorithm = EROFS_ALGORITHM_ZSTD, .length_word = 7,
	    .payload = { 0x00, 5 }, .status = FSIMG_EUNSUPPORTED },
};

static void
test_each_configuration_payload_is_validated_and_decoded(void)
{
	static const uint16_t natural_length[EROFS_ALGORITHM_COUNT] =
	    { 14, 14, 6, 6 };
	size_t i;

	for (i = 0; i < ARRAY_SIZE(payload_cases); i++) {
		const payload_case_t *c = &payload_cases[i];
		unsigned char region[REGION_BYTES];
		region_result_t r;

		config_region_init(region, 0, 1u << c->algorithm);
		put_le(region + SUPER_BYTES, c->length_word != 0 ?
		    c->length_word : natural_length[c->algorithm], 2);
		bcopy(c->payload, region + SUPER_BYTES + 2,
		    sizeof(c->payload));

		r = region_decode_checked(c->name, region, REGION_BYTES);

		EXPECT_STATUS(r.status, c->status, r.what, c->name);
		EXPECT(r.status != FSIMG_OK || c->status != FSIMG_OK ||
		    config_equal(&r.config, &c->expect),
		    "%s: decoded fields", c->name);
	}
}

/* Distinct values per algorithm, written in the layout of the design. */
static size_t
config_records_put(unsigned char *region, size_t offset, unsigned mask,
    erofs_config_t *expect)
{
	bzero(expect, sizeof(*expect));
	expect->algorithms = (uint8_t)mask;
	if ((mask & 1u) != 0) {
		put_le(region + offset, 14, 2);
		put_le(region + offset + 2, 4096, 2);
		put_le(region + offset + 4, 16, 2);
		expect->lz4_max_distance = 4096;
		expect->lz4_max_pcluster_blocks = 16;
		offset += 16;
	}
	if ((mask & 2u) != 0) {
		put_le(region + offset, 14, 2);
		put_le(region + offset + 2, 65536, 4);
		expect->lzma_dict_size = 65536;
		offset += 16;
	}
	if ((mask & 4u) != 0) {
		put_le(region + offset, 6, 2);
		region[offset + 2] = 12;
		expect->deflate_window_bits = 12;
		offset += 8;
	}
	if ((mask & 8u) != 0) {
		put_le(region + offset, 6, 2);
		region[offset + 3] = 7;
		expect->zstd_window_log = 17;
		offset += 8;
	}
	return (offset);
}

static void
test_records_follow_the_slots_in_increasing_algorithm_order(void)
{
	static const unsigned masks[] = { 0x1, 0x2, 0x4, 0x8, 0x5, 0xa, 0x6,
	    0xc, 0xf };
	static const unsigned slot_counts[] = { 0, 1, 2, 100 };
	size_t i, j;

	for (i = 0; i < ARRAY_SIZE(masks); i++) {
		for (j = 0; j < ARRAY_SIZE(slot_counts); j++) {
			unsigned char region[REGION_BYTES];
			erofs_config_t expect;
			region_result_t r;
			size_t end;

			config_region_init(region, slot_counts[j], masks[i]);
			end = config_records_put(region, SUPER_BYTES +
			    16 * slot_counts[j], masks[i], &expect);
			r = region_decode_checked("records", region, end);
			EXPECT(r.status == FSIMG_OK &&
			    config_equal(&r.config, &expect),
			    "mask 0x%x after %u slots, length %zu", masks[i],
			    slot_counts[j], end);
		}
	}
}

static void
test_without_compr_cfgs_the_word_is_lz4_max_distance(void)
{
	unsigned char region[REGION_BYTES];
	erofs_config_t expect;
	region_result_t r;

	bzero(&expect, sizeof(expect));
	expect.algorithms = 1;
	expect.lz4_max_distance = 0x1234;
	region_init(region);
	put_le(region + SB_WORD54, 0x1234, 2);
	memset(region + SUPER_BYTES, 0xee, 32);
	r = region_decode_checked("no_compr_cfgs", region, REGION_BYTES);
	EXPECT(r.status == FSIMG_OK && config_equal(&r.config, &expect),
	    "LZ4 alone with the superblock distance; bytes after are not "
	    "records");

	put_le(region + SB_WORD54, 0, 2);
	expect.lz4_max_distance = 0;
	r = region_decode_checked("no_compr_cfgs_zero", region, REGION_BYTES);
	EXPECT(r.status == FSIMG_OK && config_equal(&r.config, &expect),
	    "distance zero");
}

static void
test_algorithm_mask_bits_above_3_are_unsupported(void)
{
	static const unsigned masks[] = { 0x10, 0x11, 0x1f, 0x100, 0x8000,
	    0xfff0 };
	size_t i;

	for (i = 0; i < ARRAY_SIZE(masks); i++) {
		unsigned char region[REGION_BYTES];
		region_result_t r;

		config_region_init(region, 0, masks[i]);
		put_le(region + SUPER_BYTES, 14, 2);
		r = region_decode_checked("mask", region, REGION_BYTES);
		EXPECT(r.status == FSIMG_EUNSUPPORTED, "mask 0x%x: %s",
		    masks[i], status_name(r.status));
	}
}

static void
test_record_past_byte_3072_is_unsupported(void)
{
	unsigned char region[REGION_BYTES];
	region_result_t r;

	config_region_init(region, 184, 0x1);
	r = region_decode_checked("lz4_after_184_slots", region, REGION_BYTES);
	EXPECT_STATUS(r.status, FSIMG_EUNSUPPORTED, r.what,
	    "lz4_after_184_slots");

	config_region_init(region, 183, 0x1);
	put_le(region + SUPER_BYTES + 16 * 183, 14, 2);
	r = region_decode_checked("lz4_ending_at_3072", region, REGION_BYTES);
	EXPECT_STATUS(r.status, FSIMG_OK, r.what, "lz4_ending_at_3072");

	config_region_init(region, 183, 0x3);
	put_le(region + SUPER_BYTES + 16 * 183, 14, 2);
	r = region_decode_checked("second_record_at_3072", region,
	    REGION_BYTES);
	EXPECT_STATUS(r.status, FSIMG_EUNSUPPORTED, r.what,
	    "second_record_at_3072");

	config_region_init(region, 183, 0x1);
	put_le(region + SUPER_BYTES + 16 * 183, 14, 2);
	r = region_decode_checked("record_fits_region_not_length", region,
	    REGION_BYTES - 1);
	EXPECT_STATUS(r.status, FSIMG_ECORRUPT, r.what,
	    "record_fits_region_not_length");
}

static void
test_record_past_a_shorter_length_is_corrupt(void)
{
	unsigned char region[REGION_BYTES];
	region_result_t r;

	config_region_init(region, 2, 0x1);
	put_le(region + SUPER_BYTES + 32, 14, 2);
	r = region_decode_checked("length_word_cut", region, 161);
	EXPECT_STATUS(r.status, FSIMG_ECORRUPT, r.what, "length_word_cut");
	r = region_decode_checked("payload_cut", region, 175);
	EXPECT_STATUS(r.status, FSIMG_ECORRUPT, r.what, "payload_cut");
	r = region_decode_checked("record_exact", region, 176);
	EXPECT_STATUS(r.status, FSIMG_OK, r.what, "record_exact");
}

/* e9-config-four: image bytes [1024, 1216), superblock and its four records */
static const unsigned char config_four_bytes[] = {
	/* image offset 1024, 17 bytes */
	0xe2, 0xe1, 0xf5, 0xe0, 0x28, 0xc9, 0xd5, 0x2e, 0x03, 0x00,
	0x00, 0x00, 0x0c, 0x00, 0x26, 0x00, 0x51,
	/* image offset 1060, 28 bytes */
	0x6e, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x33, 0x33,
	0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
	/* image offset 1104, 5 bytes */
	0x03, 0x00, 0x00, 0x00, 0x0f,
	/* image offset 1152, 20 bytes */
	0x0e, 0x00, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x80,
	/* image offset 1184, 12 bytes */
	0x06, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00,
	0x00, 0x05,
};
static const fixture_run_t config_four_runs[] = {
	{ 1024, 0, 17 },
	{ 1060, 17, 28 },
	{ 1104, 45, 5 },
	{ 1152, 50, 20 },
	{ 1184, 70, 12 },
};

/* The four records of mkfs.erofs 1.9.4 as written by e9-config-four. */
static void
test_real_four_record_configuration_decodes(void)
{
	static const fixture_t fixture = FIXTURE(config_four, 1024);
	unsigned char region[REGION_BYTES];
	erofs_config_t expect = { .algorithms = 0x0f,
	    .lz4_max_distance = 0xffff, .lz4_max_pcluster_blocks = 1,
	    .lzma_dict_size = 32768, .deflate_window_bits = 15,
	    .zstd_window_log = 15 };
	region_result_t r;

	fixture_load(&fixture, region, REGION_BYTES);
	/*
	 * The fragment carries the superblock and its records, not the whole
	 * 3072-byte region its checksum covers, so the feature is cleared.
	 */
	region[SB_COMPAT] &= (unsigned char)~EROFS_COMPAT_SB_CHKSUM;

	r = region_decode_checked("e9-config-four", region, REGION_BYTES);

	EXPECT_STATUS(r.status, FSIMG_OK, r.what, "e9-config-four");
	EXPECT(r.sb.compression_word == 0x0f, "algorithm mask 0x0f");
	EXPECT(config_equal(&r.config, &expect), "four records' fields");
	EXPECT(!r.config.reserved_nonzero, "records' reserved bytes zero");
}

/* ---- Inodes ---- */

/* Inode field offsets from the design's tables. */
enum {
	I_FORMAT = 0x00,
	I_XATTR_COUNT = 0x02,
	I_MODE = 0x04,
	I_LAYOUT_WORD = 0x10,
	I_SERIAL = 0x14,
	IC_NLINK = 0x06,
	IC_SIZE = 0x08,
	IC_TIME = 0x0c,
	IC_UID = 0x18,
	IC_GID = 0x1a,
	IC_RESERVED = 0x1c,
	IX_HIGH = 0x06,
	IX_SIZE = 0x08,
	IX_UID = 0x18,
	IX_GID = 0x1c,
	IX_TIME = 0x20,
	IX_NSEC = 0x28,
	IX_NLINK = 0x2c,
	IX_RESERVED = 0x30
};

/* Format word: bit 0 extended, bits 1 to 3 layout, bit 4 48-bit only. */
#define FORMAT_EXTENDED		0x0001u
#define FORMAT_BIT4		0x0010u
#define FORMAT(layout)		((unsigned)(layout) << 1)

/* Mode type bits, as in POSIX, plus rw-r--r--. */
#define MODE_FIFO		0x1000u
#define MODE_CHAR		0x2000u
#define MODE_DIRECTORY		0x4000u
#define MODE_BLOCK		0x6000u
#define MODE_REGULAR		0x8000u
#define MODE_SYMLINK		0xa000u
#define MODE_SOCKET		0xc000u
#define PERMISSIONS_644		0x1a4u

enum { INODE_BUFFER_BYTES = 128 };

/*
 * A regular file, flat, 10 bytes, layout word 7, link count 1, with the
 * time word, owner and serial number set to recognisable values.
 */
static void
inode_init(unsigned char *raw, bool extended)
{
	bzero(raw, INODE_BUFFER_BYTES);
	put_le(raw + I_FORMAT, extended ? FORMAT_EXTENDED : 0, 2);
	put_le(raw + I_MODE, MODE_REGULAR | PERMISSIONS_644, 2);
	put_le(raw + I_LAYOUT_WORD, 7, 4);
	if (extended) {
		put_le(raw + IX_SIZE, 10, 8);
		put_le(raw + IX_NLINK, 1, 4);
	} else {
		put_le(raw + IC_SIZE, 10, 4);
		put_le(raw + IC_NLINK, 1, 2);
	}
}

static erofs_super_t
super_for_inodes(unsigned blkszbits, uint32_t incompat, int64_t epoch,
    uint32_t nsec)
{
	erofs_super_t sb;

	bzero(&sb, sizeof(sb));
	sb.blkszbits = (uint8_t)blkszbits;
	sb.incompat = incompat;
	sb.epoch = epoch;
	sb.epoch_nsec = nsec;
	return (sb);
}

typedef struct inode_result {
	fsimg_status_t	 status;
	const char	*what;
	erofs_inode_t	 inode;
} inode_result_t;

static inode_result_t
inode_decode_checked(const char *name, const unsigned char *raw,
    size_t length, const erofs_super_t *sb)
{
	inode_result_t r;
	erofs_inode_t before;

	r.what = NULL;
	memset(&r.inode, SENTINEL, sizeof(r.inode));
	before = r.inode;
	r.status = erofs_inode_decode(raw, length, sb, &r.inode, &r.what);
	if (r.status != FSIMG_OK) {
		EXPECT(bcmp(&r.inode, &before, sizeof(before)) == 0,
		    "%s: failure leaves the output untouched", name);
		/* A sentinel is not a valid bool/enum; nobody may read it. */
		bzero(&r.inode, sizeof(r.inode));
	}
	return (r);
}

typedef struct inode_case {
	const char	*name;
	bool		 extended;
	unsigned	 blkszbits;	/* 0: 12 */
	uint32_t	 incompat;
	int64_t		 epoch;
	size_t		 length;	/* 0: the fixed part */
	patch_t		 patches[MAX_PATCHES];
	fsimg_status_t	 status;
	uint64_t	 body_bytes;	/* 0: not checked */
	uint64_t	 layout_offset;	/* 0: not checked */
} inode_case_t;

#define CHUNKED		EROFS_INCOMPAT_CHUNKED_FILE
#define WIDE		EROFS_INCOMPAT_48BIT
#define FORMAT_FIELD(value)	{ I_FORMAT, 2, (value) }
#define MODE_FIELD(value)	{ I_MODE, 2, (value) }
#define XATTR_FIELD(value)	{ I_XATTR_COUNT, 2, (value) }
#define LAYOUT_FIELD(value)	{ I_LAYOUT_WORD, 4, (value) }
#define C_SIZE(value)		{ IC_SIZE, 4, (value) }
#define X_SIZE(value)		{ IX_SIZE, 8, (value) }

static const inode_case_t inode_cases[] = {
	/* Truncation */
	{ .name = "compact_length_31_is_corrupt", .length = 31,
	    .status = FSIMG_ECORRUPT },
	{ .name = "compact_length_32_is_accepted", .length = 32,
	    .status = FSIMG_OK, .body_bytes = 32, .layout_offset = 32 },
	{ .name = "extended_length_63_is_corrupt", .extended = true,
	    .length = 63, .status = FSIMG_ECORRUPT },
	{ .name = "extended_length_64_is_accepted", .extended = true,
	    .length = 64, .status = FSIMG_OK, .body_bytes = 64,
	    .layout_offset = 64 },
	{ .name = "length_1_is_corrupt", .length = 1,
	    .status = FSIMG_ECORRUPT },

	/* Format word */
	{ .name = "compact_bit_4_without_48bit_is_corrupt",
	    .patches = {FORMAT_FIELD(FORMAT_BIT4)}, .status = FSIMG_ECORRUPT },
	{ .name = "extended_bit_4_without_48bit_is_corrupt",
	    .extended = true,
	    .patches = {FORMAT_FIELD(FORMAT_EXTENDED | FORMAT_BIT4)},
	    .status = FSIMG_ECORRUPT },
	{ .name = "layout_5_is_corrupt",
	    .patches = {FORMAT_FIELD(FORMAT(5))}, .status = FSIMG_ECORRUPT },
	{ .name = "layout_6_is_corrupt",
	    .patches = {FORMAT_FIELD(FORMAT(6))}, .status = FSIMG_ECORRUPT },
	{ .name = "layout_7_is_corrupt",
	    .patches = {FORMAT_FIELD(FORMAT(7))}, .status = FSIMG_ECORRUPT },
	{ .name = "layout_1_is_unsupported",
	    .patches = {FORMAT_FIELD(FORMAT(1))},
	    .status = FSIMG_EUNSUPPORTED },
	{ .name = "layout_3_is_unsupported",
	    .patches = {FORMAT_FIELD(FORMAT(3))},
	    .status = FSIMG_EUNSUPPORTED },
	{ .name = "extended_layout_1_is_unsupported", .extended = true,
	    .patches = {FORMAT_FIELD(FORMAT(1) | FORMAT_EXTENDED)},
	    .status = FSIMG_EUNSUPPORTED },

	/* Attribute region sizes */
	{ .name = "attribute_count_0_is_no_region",
	    .patches = {XATTR_FIELD(0)}, .status = FSIMG_OK,
	    .body_bytes = 32, .layout_offset = 32 },
	{ .name = "attribute_count_1_is_a_12_byte_region",
	    .patches = {XATTR_FIELD(1)}, .status = FSIMG_OK,
	    .body_bytes = 32 + 12, .layout_offset = 32 + 12 },
	{ .name = "attribute_count_2_is_a_16_byte_region",
	    .patches = {XATTR_FIELD(2)}, .status = FSIMG_OK,
	    .body_bytes = 32 + 16, .layout_offset = 32 + 16 },
	{ .name = "attribute_count_65535_is_the_largest_region",
	    .extended = true, .patches = {XATTR_FIELD(65535)},
	    .status = FSIMG_OK, .body_bytes = 64 + 12 + 4 * 65534,
	    .layout_offset = 64 + 12 + 4 * 65534 },

	/* Kinds and their rules */
	{ .name = "directory_of_size_0_is_corrupt",
	    .patches = {MODE_FIELD(MODE_DIRECTORY | 0755), C_SIZE(0)},
	    .status = FSIMG_ECORRUPT },
	{ .name = "directory_of_size_1_is_accepted",
	    .patches = {MODE_FIELD(MODE_DIRECTORY | 0755), C_SIZE(1)},
	    .status = FSIMG_OK, .body_bytes = 32, .layout_offset = 32 },
	{ .name = "symlink_of_size_0_is_corrupt",
	    .patches = {MODE_FIELD(MODE_SYMLINK | 0777), C_SIZE(0)},
	    .status = FSIMG_ECORRUPT },
	{ .name = "symlink_of_size_1024_is_accepted",
	    .patches = {MODE_FIELD(MODE_SYMLINK | 0777), C_SIZE(1024)},
	    .status = FSIMG_OK, .body_bytes = 32, .layout_offset = 32 },
	{ .name = "symlink_of_size_1025_is_unsupported",
	    .patches = {MODE_FIELD(MODE_SYMLINK | 0777), C_SIZE(1025)},
	    .status = FSIMG_EUNSUPPORTED },
	{ .name = "extended_symlink_of_size_1025_is_unsupported",
	    .extended = true,
	    .patches = {MODE_FIELD(MODE_SYMLINK | 0777), X_SIZE(1025)},
	    .status = FSIMG_EUNSUPPORTED },
	{ .name = "fifo_of_size_0_is_accepted",
	    .patches = {MODE_FIELD(MODE_FIFO | 0644), C_SIZE(0),
	    LAYOUT_FIELD(0)},
	    .status = FSIMG_OK, .body_bytes = 32, .layout_offset = 32 },
	{ .name = "fifo_with_layout_word_is_corrupt",
	    .patches = {MODE_FIELD(MODE_FIFO | 0644), C_SIZE(0),
	    LAYOUT_FIELD(1)},
	    .status = FSIMG_ECORRUPT },
	{ .name = "fifo_with_inline_layout_is_corrupt",
	    .patches = {MODE_FIELD(MODE_FIFO | 0644), C_SIZE(0),
	    LAYOUT_FIELD(0),
	    FORMAT_FIELD(FORMAT(2))},
	    .status = FSIMG_ECORRUPT },
	{ .name = "fifo_with_size_is_corrupt",
	    .patches = {MODE_FIELD(MODE_FIFO | 0644), C_SIZE(1),
	    LAYOUT_FIELD(0)},
	    .status = FSIMG_ECORRUPT },
	{ .name = "socket_with_layout_word_is_corrupt", .extended = true,
	    .patches = {MODE_FIELD(MODE_SOCKET | 0644), X_SIZE(0),
	    LAYOUT_FIELD(0xffffffff)},
	    .status = FSIMG_ECORRUPT },
	{ .name = "socket_of_size_0_is_accepted", .extended = true,
	    .patches = {MODE_FIELD(MODE_SOCKET | 0644), X_SIZE(0),
	    LAYOUT_FIELD(0)},
	    .status = FSIMG_OK, .body_bytes = 64, .layout_offset = 64 },
	{ .name = "socket_with_size_is_corrupt", .extended = true,
	    .patches = {MODE_FIELD(MODE_SOCKET | 0644), X_SIZE(5),
	    LAYOUT_FIELD(0)},
	    .status = FSIMG_ECORRUPT },
	{ .name = "char_device_with_any_layout_word_is_accepted",
	    .patches = {MODE_FIELD(MODE_CHAR | 0600), C_SIZE(0),
	    LAYOUT_FIELD(0xffffffff)},
	    .status = FSIMG_OK, .body_bytes = 32, .layout_offset = 32 },
	{ .name = "block_device_with_layout_word_0_is_accepted",
	    .patches = {MODE_FIELD(MODE_BLOCK | 0600), C_SIZE(0),
	    LAYOUT_FIELD(0)},
	    .status = FSIMG_OK, .body_bytes = 32, .layout_offset = 32 },
	{ .name = "char_device_with_size_is_corrupt",
	    .patches = {MODE_FIELD(MODE_CHAR | 0600), C_SIZE(1),
	    LAYOUT_FIELD(0x0801)},
	    .status = FSIMG_ECORRUPT },
	{ .name = "block_device_with_inline_layout_is_corrupt",
	    .patches = {MODE_FIELD(MODE_BLOCK | 0600), C_SIZE(0),
	    LAYOUT_FIELD(0x0801),
	    FORMAT_FIELD(FORMAT(2))},
	    .status = FSIMG_ECORRUPT },

	/* Times */
	{ .name = "compact_time_overflow_is_corrupt",
	    .epoch = INT64_MAX - 0xfffffffe,
	    .patches = {{ IC_TIME, 4, 0xffffffff }}, .status = FSIMG_ECORRUPT },
	{ .name = "compact_time_reaching_int64_max_is_accepted",
	    .epoch = INT64_MAX - 0xffffffff,
	    .patches = {{ IC_TIME, 4, 0xffffffff }}, .status = FSIMG_OK,
	    .body_bytes = 32, .layout_offset = 32 },
	{ .name = "compact_reserved_word_nonzero_is_corrupt",
	    .patches = {{ IC_RESERVED, 4, 1 }}, .status = FSIMG_ECORRUPT },
	{ .name = "compact_reserved_high_byte_nonzero_is_corrupt",
	    .patches = {{ IC_RESERVED + 3, 1, 0x80 }},
	    .status = FSIMG_ECORRUPT },
	{ .name = "extended_nanoseconds_999999999_are_accepted",
	    .extended = true, .patches = {{ IX_NSEC, 4, 999999999 }},
	    .status = FSIMG_OK, .body_bytes = 64, .layout_offset = 64 },
	{ .name = "extended_nanoseconds_1000000000_are_corrupt",
	    .extended = true, .patches = {{ IX_NSEC, 4, 1000000000 }},
	    .status = FSIMG_ECORRUPT },
	{ .name = "extended_high_word_without_48bit_is_corrupt",
	    .extended = true, .patches = {{ IX_HIGH, 2, 1 }},
	    .status = FSIMG_ECORRUPT },
	{ .name = "extended_high_word_under_48bit_is_accepted",
	    .extended = true, .incompat = WIDE,
	    .patches = {{ IX_HIGH, 2, 1 }}, .status = FSIMG_OK,
	    .body_bytes = 64, .layout_offset = 64 },

	/* 48-bit format bit 4 */
	{ .name = "compact_file_with_bit_4_under_48bit_is_accepted",
	    .incompat = WIDE, .patches = {FORMAT_FIELD(FORMAT_BIT4)},
	    .status = FSIMG_OK, .body_bytes = 32, .layout_offset = 32 },
	{ .name = "compact_directory_with_bit_4_under_48bit_is_accepted",
	    .incompat = WIDE,
	    .patches = {FORMAT_FIELD(FORMAT_BIT4),
	    MODE_FIELD(MODE_DIRECTORY | 0755), C_SIZE(24)},
	    .status = FSIMG_OK, .body_bytes = 32, .layout_offset = 32 },
	{ .name = "extended_file_with_bit_4_is_unsupported",
	    .extended = true, .incompat = WIDE,
	    .patches = {FORMAT_FIELD(FORMAT_EXTENDED | FORMAT_BIT4)},
	    .status = FSIMG_EUNSUPPORTED },
	{ .name = "extended_directory_with_bit_4_under_48bit_is_accepted",
	    .extended = true, .incompat = WIDE,
	    .patches = {FORMAT_FIELD(FORMAT_EXTENDED | FORMAT_BIT4),
	    MODE_FIELD(MODE_DIRECTORY | 0755), X_SIZE(24)},
	    .status = FSIMG_OK, .body_bytes = 64, .layout_offset = 64 },

	/* Flat-inline tails */
	{ .name = "inline_size_4096_at_4096_is_corrupt", .blkszbits = 12,
	    .patches = {FORMAT_FIELD(FORMAT(2)), C_SIZE(4096)},
	    .status = FSIMG_ECORRUPT },
	{ .name = "inline_size_8192_at_4096_is_corrupt", .blkszbits = 12,
	    .patches = {FORMAT_FIELD(FORMAT(2)), C_SIZE(8192)},
	    .status = FSIMG_ECORRUPT },
	{ .name = "extended_inline_size_512_at_512_is_corrupt",
	    .extended = true, .blkszbits = 9,
	    .patches = {FORMAT_FIELD(FORMAT(2) | FORMAT_EXTENDED), X_SIZE(512)},
	    .status = FSIMG_ECORRUPT },
	{ .name = "inline_size_0_is_accepted", .blkszbits = 12,
	    .patches = {FORMAT_FIELD(FORMAT(2)), C_SIZE(0)},
	    .status = FSIMG_OK, .body_bytes = 32, .layout_offset = 32 },
	{ .name = "inline_size_5000_at_4096_has_a_904_byte_tail",
	    .blkszbits = 12,
	    .patches = {FORMAT_FIELD(FORMAT(2)), C_SIZE(5000)},
	    .status = FSIMG_OK, .body_bytes = 32 + 904, .layout_offset = 32 },
	{ .name = "inline_size_513_at_512_has_a_1_byte_tail", .blkszbits = 9,
	    .patches = {FORMAT_FIELD(FORMAT(2)), C_SIZE(513)},
	    .status = FSIMG_OK, .body_bytes = 32 + 1, .layout_offset = 32 },
	{ .name = "extended_inline_tail_follows_the_attribute_region",
	    .extended = true, .blkszbits = 12,
	    .patches = {FORMAT_FIELD(FORMAT(2) | FORMAT_EXTENDED),
	    XATTR_FIELD(3),
	    X_SIZE(100)},
	    .status = FSIMG_OK, .body_bytes = 64 + 20 + 100,
	    .layout_offset = 84 },
	{ .name = "flat_size_4096_is_accepted_without_a_tail",
	    .blkszbits = 12, .patches = {C_SIZE(4096)}, .status = FSIMG_OK,
	    .body_bytes = 32, .layout_offset = 32 },
	{ .name = "flat_body_is_fixed_part_and_attribute_region",
	    .extended = true, .blkszbits = 12,
	    .patches = {XATTR_FIELD(3), X_SIZE(100000)}, .status = FSIMG_OK,
	    .body_bytes = 64 + 20, .layout_offset = 64 + 20 },

	/* Chunk-based bodies */
	{ .name = "chunk_based_without_chunked_file_is_corrupt",
	    .blkszbits = 12,
	    .patches = {FORMAT_FIELD(FORMAT(4)), LAYOUT_FIELD(0), C_SIZE(4096)},
	    .status = FSIMG_ECORRUPT },
	{ .name = "chunk_4_byte_entries_one_per_block_chunk",
	    .blkszbits = 12, .incompat = CHUNKED,
	    .patches = {FORMAT_FIELD(FORMAT(4)), LAYOUT_FIELD(0),
	    C_SIZE(3 * 4096 + 1)},
	    .status = FSIMG_OK, .body_bytes = 32 + 4 * 4, .layout_offset = 32 },
	{ .name = "chunk_exact_multiple_has_no_partial_chunk",
	    .blkszbits = 12, .incompat = CHUNKED,
	    .patches = {FORMAT_FIELD(FORMAT(4)), LAYOUT_FIELD(0),
	    C_SIZE(2 * 4096)},
	    .status = FSIMG_OK, .body_bytes = 32 + 4 * 2, .layout_offset = 32 },
	{ .name = "chunk_bits_1_doubles_the_chunk", .blkszbits = 12,
	    .incompat = CHUNKED,
	    .patches = {FORMAT_FIELD(FORMAT(4)), LAYOUT_FIELD(1),
	    C_SIZE(3 * 4096 + 1)},
	    .status = FSIMG_OK, .body_bytes = 32 + 4 * 2, .layout_offset = 32 },
	{ .name = "chunk_layout_word_high_half_is_ignored", .blkszbits = 12,
	    .incompat = CHUNKED,
	    .patches = {FORMAT_FIELD(FORMAT(4)), LAYOUT_FIELD(0xabcd0001),
	    C_SIZE(3 * 4096 + 1)},
	    .status = FSIMG_OK, .body_bytes = 32 + 4 * 2, .layout_offset = 32 },
	{ .name = "chunk_8_byte_entries_after_a_16_byte_region_are_aligned",
	    .blkszbits = 12, .incompat = CHUNKED,
	    .patches = {FORMAT_FIELD(FORMAT(4)), XATTR_FIELD(2),
	    LAYOUT_FIELD(0x20), C_SIZE(4097)}, .status = FSIMG_OK,
	    .body_bytes = 48 + 8 * 2, .layout_offset = 48 },
	{ .name = "chunk_8_byte_entries_after_a_20_byte_region_round_up",
	    .blkszbits = 12, .incompat = CHUNKED,
	    .patches = {FORMAT_FIELD(FORMAT(4)), XATTR_FIELD(3),
	    LAYOUT_FIELD(0x20), C_SIZE(4097)}, .status = FSIMG_OK,
	    .body_bytes = 56 + 8 * 2, .layout_offset = 56 },
	{ .name = "chunk_4_byte_entries_after_a_20_byte_region_do_not_round",
	    .blkszbits = 12, .incompat = CHUNKED,
	    .patches = {FORMAT_FIELD(FORMAT(4)), XATTR_FIELD(3),
	    LAYOUT_FIELD(0), C_SIZE(4097)}, .status = FSIMG_OK,
	    .body_bytes = 52 + 4 * 2, .layout_offset = 52 },
	{ .name = "chunk_size_0_has_no_entries", .blkszbits = 12,
	    .incompat = CHUNKED,
	    .patches = {FORMAT_FIELD(FORMAT(4)), LAYOUT_FIELD(0), C_SIZE(0)},
	    .status = FSIMG_OK, .body_bytes = 32, .layout_offset = 32 },
	{ .name = "chunk_8_byte_entries_align_the_index_to_8",
	    .blkszbits = 12, .incompat = CHUNKED,
	    .patches = {FORMAT_FIELD(FORMAT(4)), XATTR_FIELD(1),
	    LAYOUT_FIELD(0x20),
	    C_SIZE(4097)},
	    .status = FSIMG_OK, .body_bytes = 48 + 8 * 2, .layout_offset = 48 },
	{ .name = "extended_chunk_8_byte_entries_align_the_index_to_8",
	    .extended = true, .blkszbits = 12, .incompat = CHUNKED,
	    .patches = {FORMAT_FIELD(FORMAT(4) | FORMAT_EXTENDED),
	    XATTR_FIELD(1), LAYOUT_FIELD(0x20), X_SIZE(4096)},
	    .status = FSIMG_OK, .body_bytes = 80 + 8, .layout_offset = 80 },
	{ .name = "extended_chunk_4_byte_entries_follow_the_region",
	    .extended = true, .blkszbits = 12, .incompat = CHUNKED,
	    .patches = {FORMAT_FIELD(FORMAT(4) | FORMAT_EXTENDED),
	    XATTR_FIELD(2),
	    LAYOUT_FIELD(0), X_SIZE(3 * 4096)},
	    .status = FSIMG_OK, .body_bytes = 80 + 4 * 3, .layout_offset = 80 },
	{ .name = "chunk_index_of_the_largest_file_does_not_overflow",
	    .extended = true, .blkszbits = 9, .incompat = CHUNKED,
	    .patches = {FORMAT_FIELD(FORMAT(4) | FORMAT_EXTENDED),
	    LAYOUT_FIELD(0),
	    X_SIZE((UINT64_C(1) << 47) - 1)},
	    .status = FSIMG_OK, .body_bytes = 64 + 4 * (UINT64_C(1) << 38),
	    .layout_offset = 64 },
	{ .name = "chunk_format_bit_7_is_corrupt", .blkszbits = 12,
	    .incompat = CHUNKED,
	    .patches = {FORMAT_FIELD(FORMAT(4)), LAYOUT_FIELD(0x80)},
	    .status = FSIMG_ECORRUPT },
	{ .name = "chunk_format_bit_6_without_48bit_is_corrupt",
	    .blkszbits = 12, .incompat = CHUNKED,
	    .patches = {FORMAT_FIELD(FORMAT(4)), LAYOUT_FIELD(0x60)},
	    .status = FSIMG_ECORRUPT },
};

static void
test_inode_decode_classifies_each_case(void)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(inode_cases); i++) {
		const inode_case_t *c = &inode_cases[i];
		unsigned char raw[INODE_BUFFER_BYTES];
		erofs_super_t sb = super_for_inodes(c->blkszbits != 0 ?
		    c->blkszbits : 12, c->incompat, c->epoch, 0);
		size_t fixed = c->extended ? 64 : 32;
		inode_result_t r;

		inode_init(raw, c->extended);
		patch_apply(raw, c->patches);
		r = inode_decode_checked(c->name, raw,
		    c->length != 0 ? c->length : fixed, &sb);

		EXPECT_STATUS(r.status, c->status, r.what, c->name);
		if (r.status != FSIMG_OK)
			continue;
		EXPECT(c->body_bytes == 0 ||
		    r.inode.body_bytes == c->body_bytes,
		    "%s: body_bytes %llu, expected %llu", c->name,
		    (unsigned long long)r.inode.body_bytes,
		    (unsigned long long)c->body_bytes);
		EXPECT(c->layout_offset == 0 ||
		    r.inode.layout_offset == c->layout_offset,
		    "%s: layout_offset %llu, expected %llu", c->name,
		    (unsigned long long)r.inode.layout_offset,
		    (unsigned long long)c->layout_offset);
	}
}

/*
 * FIFOs, sockets and devices have no start block, so the start-block bits 32
 * to 47 the 48-bit layout adds must be zero: compact inodes keep them in the
 * link-count word under format bit 4, extended inodes in the word at 0x06.
 */
static void
test_special_files_refuse_start_block_high_bits(void)
{
	static const uint16_t modes[] = { MODE_FIFO, MODE_SOCKET, MODE_CHAR,
	    MODE_BLOCK };
	erofs_super_t sb = super_for_inodes(12, WIDE, 0, 0);

	for (size_t i = 0; i < ARRAY_SIZE(modes); i++) {
		for (int extended = 0; extended < 2; extended++) {
			for (uint64_t high = 0; high < 2; high++) {
				unsigned char raw[INODE_BUFFER_BYTES];
				char name[80];
				inode_result_t r;

				inode_init(raw, extended != 0);
				put_le(raw + I_MODE, modes[i] | 0600, 2);
				put_le(raw + I_LAYOUT_WORD, 0, 4);
				if (extended != 0) {
					put_le(raw + I_FORMAT, FORMAT_EXTENDED, 2);
					put_le(raw + IX_SIZE, 0, 8);
					put_le(raw + IX_HIGH, high, 2);
				} else {
					put_le(raw + I_FORMAT, FORMAT_BIT4, 2);
					put_le(raw + IC_SIZE, 0, 4);
					put_le(raw + IC_NLINK, high, 2);
				}
				snprintf(name, sizeof(name),
				    "mode %o, %s, high bits %llu", modes[i],
				    extended != 0 ? "extended" : "compact",
				    (unsigned long long)high);
				r = inode_decode_checked(name, raw,
				    extended != 0 ? 64 : 32, &sb);
				EXPECT_STATUS(r.status, high != 0 ?
				    FSIMG_ECORRUPT : FSIMG_OK, r.what, name);
			}
		}
	}
}

static void
test_inode_of_length_0_is_corrupt(void)
{
	unsigned char raw[INODE_BUFFER_BYTES];
	erofs_super_t sb = super_for_inodes(12, 0, 0, 0);
	inode_result_t r;

	inode_init(raw, false);
	r = inode_decode_checked("length_0", raw, 0, &sb);
	EXPECT_STATUS(r.status, FSIMG_ECORRUPT, r.what, "length_0");
}

static void
test_fixed_bytes_follow_format_bit_0_only(void)
{
	static const uint16_t formats[] = { 0x0000, 0x0002, 0x0008, 0x0010,
	    0x0e00, 0xfffe, 0x0001, 0x0003, 0x0009, 0xffff };
	size_t i;

	for (i = 0; i < ARRAY_SIZE(formats); i++) {
		unsigned char raw[2];

		put_le(raw, formats[i], 2);
		EXPECT(erofs_inode_fixed_bytes(raw) ==
		    ((formats[i] & 1) != 0 ? 64u : 32u), "format 0x%04x",
		    (unsigned)formats[i]);
	}
}

static void
test_format_bits_5_to_15_are_each_corrupt(void)
{
	unsigned bit;

	for (bit = 5; bit < 16; bit++) {
		unsigned char raw[INODE_BUFFER_BYTES];
		erofs_super_t sb = super_for_inodes(12, WIDE | CHUNKED, 0, 0);
		inode_result_t r;
		int extended;

		for (extended = 0; extended < 2; extended++) {
			inode_init(raw, extended != 0);
			put_le(raw + I_FORMAT, (uint64_t)extended |
			    (UINT64_C(1) << bit), 2);
			r = inode_decode_checked("format_bit", raw,
			    extended != 0 ? 64 : 32, &sb);
			EXPECT(r.status == FSIMG_ECORRUPT,
			    "format bit %u, extended=%d: %s", bit, extended,
			    status_name(r.status));
		}
	}
}

static void
test_each_mode_type_selects_its_kind(void)
{
	static const struct {
		unsigned		 type;
		fsimg_entry_kind_t	 kind;
		uint64_t		 size;
	} kinds[] = {
		{ MODE_REGULAR, FSIMG_ENTRY_FILE, 10 },
		{ MODE_DIRECTORY, FSIMG_ENTRY_DIRECTORY, 10 },
		{ MODE_SYMLINK, FSIMG_ENTRY_SYMLINK, 10 },
		{ MODE_FIFO, FSIMG_ENTRY_FIFO, 0 },
		{ MODE_CHAR, FSIMG_ENTRY_CHAR_DEVICE, 0 },
		{ MODE_BLOCK, FSIMG_ENTRY_BLOCK_DEVICE, 0 },
		{ MODE_SOCKET, FSIMG_ENTRY_SOCKET, 0 },
	};
	size_t i;
	int extended;

	for (extended = 0; extended < 2; extended++) {
		for (i = 0; i < ARRAY_SIZE(kinds); i++) {
			unsigned char raw[INODE_BUFFER_BYTES];
			erofs_super_t sb = super_for_inodes(12, 0, 0, 0);
			inode_result_t r;

			inode_init(raw, extended != 0);
			put_le(raw + I_MODE, kinds[i].type | 0644, 2);
			put_le(raw + (extended != 0 ? IX_SIZE : IC_SIZE),
			    kinds[i].size, extended != 0 ? 8 : 4);
			put_le(raw + I_LAYOUT_WORD, 0, 4);
			r = inode_decode_checked("kind", raw,
			    extended != 0 ? 64 : 32, &sb);
			EXPECT(r.status == FSIMG_OK && r.inode.kind ==
			    kinds[i].kind, "mode type 0x%x, extended=%d",
			    kinds[i].type, extended);
		}
	}
}

static void
test_other_mode_types_are_corrupt(void)
{
	static const unsigned types[] = { 0x0000, 0x3000, 0x5000, 0x7000,
	    0x9000, 0xb000, 0xd000, 0xe000, 0xf000 };
	size_t i;

	for (i = 0; i < ARRAY_SIZE(types); i++) {
		unsigned char raw[INODE_BUFFER_BYTES];
		erofs_super_t sb = super_for_inodes(12, 0, 0, 0);
		inode_result_t r;

		inode_init(raw, false);
		put_le(raw + I_MODE, types[i] | 0644, 2);
		r = inode_decode_checked("bad_type", raw, 32, &sb);
		EXPECT(r.status == FSIMG_ECORRUPT, "mode type 0x%x: %s",
		    types[i], status_name(r.status));
	}
}

static void
test_permissions_are_the_low_12_bits_of_the_mode(void)
{
	unsigned char raw[INODE_BUFFER_BYTES];
	erofs_super_t sb = super_for_inodes(12, 0, 0, 0);
	inode_result_t r;

	inode_init(raw, false);
	put_le(raw + I_MODE, MODE_REGULAR | 0xfff, 2);
	r = inode_decode_checked("perm_fff", raw, 32, &sb);
	EXPECT(r.status == FSIMG_OK && r.inode.permissions == 0xfff,
	    "setuid, setgid, sticky and rwx for all");

	put_le(raw + I_MODE, MODE_REGULAR | 0755, 2);
	r = inode_decode_checked("perm_755", raw, 32, &sb);
	EXPECT(r.status == FSIMG_OK && r.inode.permissions == 0755,
	    "rwxr-xr-x");
}

static void
test_compact_inode_fields_are_decoded(void)
{
	unsigned char raw[INODE_BUFFER_BYTES];
	erofs_super_t sb = super_for_inodes(12, 0, 1000, 123456789);
	inode_result_t r;

	inode_init(raw, false);
	put_le(raw + IC_NLINK, 0xfffe, 2);
	put_le(raw + IC_SIZE, 0xfedcba98, 4);
	put_le(raw + IC_TIME, 5, 4);
	put_le(raw + I_LAYOUT_WORD, 0x01020304, 4);
	put_le(raw + I_SERIAL, 0x11223344, 4);
	put_le(raw + IC_UID, 0xabcd, 2);
	put_le(raw + IC_GID, 0xef01, 2);

	r = inode_decode_checked("compact_fields", raw, 32, &sb);

	EXPECT_STATUS(r.status, FSIMG_OK, r.what, "compact_fields");
	EXPECT(!r.inode.extended && r.inode.fixed_bytes == 32,
	    "compact, 32 bytes");
	EXPECT(r.inode.layout == EROFS_LAYOUT_FLAT, "flat layout");
	EXPECT(r.inode.nlink == 0xfffe, "16-bit link count");
	EXPECT(r.inode.size == 0xfedcba98, "32-bit size");
	EXPECT(r.inode.time == 1005, "time is the epoch plus the word");
	EXPECT(r.inode.time_nsec == 123456789, "superblock nanoseconds");
	EXPECT(r.inode.layout_word == 0x01020304, "layout word");
	EXPECT(r.inode.serial == 0x11223344, "serial number");
	EXPECT(r.inode.uid == 0xabcd, "16-bit uid");
	EXPECT(r.inode.gid == 0xef01, "16-bit gid");
	EXPECT(r.inode.xattr_bytes == 0, "no attribute region");
	EXPECT(r.inode.layout_high == 0, "no high start-block bits");
	EXPECT(!r.inode.omits_dot, "dot not omitted");
}

static void
test_compact_time_is_epoch_plus_the_unsigned_word(void)
{
	unsigned char raw[INODE_BUFFER_BYTES];
	inode_result_t r;
	erofs_super_t sb;

	inode_init(raw, false);
	put_le(raw + IC_TIME, 0xffffffff, 4);

	sb = super_for_inodes(12, 0, -1, 0);
	r = inode_decode_checked("negative_epoch", raw, 32, &sb);
	EXPECT(r.status == FSIMG_OK && r.inode.time == 0xfffffffe,
	    "epoch -1 plus 0xffffffff");

	sb = super_for_inodes(12, 0, -INT64_C(5000000000), 0);
	r = inode_decode_checked("very_negative_epoch", raw, 32, &sb);
	EXPECT(r.status == FSIMG_OK &&
	    r.inode.time == -INT64_C(5000000000) + 0xffffffff,
	    "epoch -5e9 plus 0xffffffff");

	sb = super_for_inodes(12, WIDE, 0, 0);
	r = inode_decode_checked("48bit_time", raw, 32, &sb);
	EXPECT(r.status == FSIMG_OK && r.inode.time == 0xffffffff,
	    "the word is a time under the 48-bit layout too");

	sb = super_for_inodes(12, 0, INT64_MIN, 0);
	put_le(raw + IC_TIME, 0, 4);
	r = inode_decode_checked("min_epoch", raw, 32, &sb);
	EXPECT(r.status == FSIMG_OK && r.inode.time == INT64_MIN,
	    "epoch INT64_MIN plus 0");
}

static void
test_extended_inode_fields_are_decoded(void)
{
	unsigned char raw[INODE_BUFFER_BYTES];
	erofs_super_t sb = super_for_inodes(12, 0, 1000, 999);
	inode_result_t r;

	inode_init(raw, true);
	put_le(raw + I_XATTR_COUNT, 3, 2);
	put_le(raw + IX_SIZE, UINT64_C(0x00001234567890ab), 8);
	put_le(raw + I_LAYOUT_WORD, 0x01020304, 4);
	put_le(raw + I_SERIAL, 0x55667788, 4);
	put_le(raw + IX_UID, 0xdeadbeef, 4);
	put_le(raw + IX_GID, 0xcafef00d, 4);
	put_le(raw + IX_TIME, (uint64_t)-123456789, 8);
	put_le(raw + IX_NSEC, 987654321, 4);
	put_le(raw + IX_NLINK, 0x01000001, 4);

	r = inode_decode_checked("extended_fields", raw, 64, &sb);

	EXPECT_STATUS(r.status, FSIMG_OK, r.what, "extended_fields");
	EXPECT(r.inode.extended && r.inode.fixed_bytes == 64,
	    "extended, 64 bytes");
	EXPECT(r.inode.xattr_bytes == 20, "attribute count 3 is 20 bytes");
	EXPECT(r.inode.size == UINT64_C(0x00001234567890ab), "64-bit size");
	EXPECT(r.inode.layout_word == 0x01020304, "layout word");
	EXPECT(r.inode.serial == 0x55667788, "serial number");
	EXPECT(r.inode.uid == 0xdeadbeef, "32-bit uid");
	EXPECT(r.inode.gid == 0xcafef00d, "32-bit gid");
	EXPECT(r.inode.time == -123456789, "signed time, not the epoch's");
	EXPECT(r.inode.time_nsec == 987654321, "its own nanoseconds");
	EXPECT(r.inode.nlink == 0x01000001, "32-bit link count");
	EXPECT(r.inode.layout_high == 0, "no high start-block bits");
	EXPECT(!r.inode.omits_dot, "dot not omitted");
}

static void
test_each_extended_reserved_byte_nonzero_is_corrupt(void)
{
	unsigned offset;

	for (offset = IX_RESERVED; offset < IX_RESERVED + 16; offset++) {
		unsigned char raw[INODE_BUFFER_BYTES];
		erofs_super_t sb = super_for_inodes(12, WIDE, 0, 0);
		inode_result_t r;

		inode_init(raw, true);
		raw[offset] = 1;
		r = inode_decode_checked("reserved", raw, 64, &sb);
		EXPECT(r.status == FSIMG_ECORRUPT, "byte 0x%x: %s", offset,
		    status_name(r.status));
	}
}

static void
test_48bit_inode_start_block_bits_are_reported(void)
{
	unsigned char raw[INODE_BUFFER_BYTES];
	erofs_super_t sb = super_for_inodes(12, WIDE, 0, 0);
	inode_result_t r;

	inode_init(raw, true);
	put_le(raw + IX_HIGH, 0xbeef, 2);
	r = inode_decode_checked("extended_high", raw, 64, &sb);
	EXPECT(r.status == FSIMG_OK && r.inode.layout_high == 0xbeef &&
	    r.inode.nlink == 1, "extended: the 0x06 word is layout_high");

	inode_init(raw, false);
	put_le(raw + I_FORMAT, FORMAT_BIT4, 2);
	put_le(raw + IC_NLINK, 0x1234, 2);
	r = inode_decode_checked("compact_implicit_nlink", raw, 32, &sb);
	EXPECT(r.status == FSIMG_OK && r.inode.nlink == 1 &&
	    r.inode.layout_high == 0x1234 && !r.inode.omits_dot,
	    "compact file with bit 4: link count 1, word is layout_high");

	inode_init(raw, false);
	put_le(raw + IC_NLINK, 0x1234, 2);
	r = inode_decode_checked("compact_explicit_nlink", raw, 32, &sb);
	EXPECT(r.status == FSIMG_OK && r.inode.nlink == 0x1234 &&
	    r.inode.layout_high == 0, "compact file without bit 4 keeps nlink");
}

static void
test_directory_with_bit_4_omits_dot(void)
{
	unsigned char raw[INODE_BUFFER_BYTES];
	erofs_super_t sb = super_for_inodes(12, WIDE, 0, 0);
	inode_result_t r;
	int extended;

	for (extended = 0; extended < 2; extended++) {
		inode_init(raw, extended != 0);
		put_le(raw + I_FORMAT, extended | FORMAT_BIT4, 2);
		put_le(raw + I_MODE, MODE_DIRECTORY | 0755, 2);
		put_le(raw + (extended != 0 ? IX_SIZE : IC_SIZE), 24,
		    extended != 0 ? 8 : 4);
		r = inode_decode_checked("omits_dot", raw,
		    extended != 0 ? 64 : 32, &sb);
		EXPECT(r.status == FSIMG_OK && r.inode.omits_dot &&
		    r.inode.kind == FSIMG_ENTRY_DIRECTORY,
		    "extended=%d directory with bit 4", extended);
	}

	inode_init(raw, false);
	put_le(raw + I_MODE, MODE_DIRECTORY | 0755, 2);
	put_le(raw + IC_SIZE, 24, 4);
	r = inode_decode_checked("keeps_dot", raw, 32, &sb);
	EXPECT(r.status == FSIMG_OK && !r.inode.omits_dot,
	    "directory without bit 4 keeps its dot entry");
}

static void
test_device_numbers_decode_by_the_linux_bit_layout(void)
{
	static const struct {
		uint32_t	 word;
		uint32_t	 major;
		uint32_t	 minor;
	} cases[] = {
		{ 0xffffffff, 4095, 1048575 },
		{ 0x00000000, 0, 0 },
		{ 0x00000801, 8, 1 },
		{ 0x00000102, 1, 2 },
		{ 0x000fff00, 4095, 0 },
		{ 0xfff000ff, 0, 0xfffff },
		{ 0x0011032c, 0x103, 0x12c },
		{ 0x00100000, 0, 0x100 },
		{ 0x000000ff, 0, 255 },
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		uint32_t major = 0xdead, minor = 0xbeef;

		erofs_device_decode(cases[i].word, &major, &minor);
		EXPECT(major == cases[i].major && minor == cases[i].minor,
		    "word 0x%08x gave %u:%u, expected %u:%u",
		    (unsigned)cases[i].word, (unsigned)major,
		    (unsigned)minor, (unsigned)cases[i].major,
		    (unsigned)cases[i].minor);
		EXPECT(device_word(cases[i].major, cases[i].minor) ==
		    cases[i].word, "case table agrees with the encoding, "
		    "word 0x%08x", (unsigned)cases[i].word);
	}
}

/* ---- Chunk formats and entries ---- */

static void
test_chunk_format_reserved_bits_7_to_15_are_each_corrupt(void)
{
	erofs_super_t sb = super_for_inodes(12, WIDE, 0, 0);
	unsigned bit;

	for (bit = 7; bit < 16; bit++) {
		erofs_chunk_format_t before, format;
		const char *what = NULL;
		fsimg_status_t status;

		memset(&format, SENTINEL, sizeof(format));
		before = format;
		status = erofs_chunk_format_decode(UINT32_C(1) << bit, &sb,
		    &format, &what);
		EXPECT(status == FSIMG_ECORRUPT, "format bit %u: %s", bit,
		    status_name(status));
		EXPECT(bcmp(&format, &before, sizeof(before)) == 0,
		    "format bit %u: output untouched", bit);
	}
}

static void
test_chunk_format_48bit_bit_needs_the_48bit_layout(void)
{
	erofs_super_t plain = super_for_inodes(12, 0, 0, 0);
	erofs_super_t wide = super_for_inodes(12, WIDE, 0, 0);
	erofs_chunk_format_t format;
	const char *what = NULL;

	EXPECT(erofs_chunk_format_decode(0x40, &plain, &format, &what) ==
	    FSIMG_ECORRUPT, "bit 6 without 48BIT");
	EXPECT(erofs_chunk_format_decode(0x60, &wide, &format, &what) ==
	    FSIMG_OK && format.wide && format.entry_bytes == 8,
	    "bit 6 with 48BIT is wide");
	EXPECT(erofs_chunk_format_decode(0x20, &wide, &format, &what) ==
	    FSIMG_OK && !format.wide && format.entry_bytes == 8,
	    "bit 6 clear is not wide, even under 48BIT");
}

/* Bit 6 widens a block number, which a 4-byte entry has no high part for. */
static void
test_chunk_format_48bit_bit_needs_8_byte_entries(void)
{
	erofs_super_t wide = super_for_inodes(12, WIDE, 0, 0);
	erofs_chunk_format_t format, before;
	const char *what = NULL;

	memset(&format, SENTINEL, sizeof(format));
	before = format;
	EXPECT(erofs_chunk_format_decode(0x40, &wide, &format, &what) ==
	    FSIMG_ECORRUPT, "bit 6 with 4-byte entries under 48BIT");
	EXPECT(bcmp(&format, &before, sizeof(before)) == 0,
	    "the refusal leaves the output untouched");

	EXPECT(erofs_chunk_format_decode(0x60, &wide, &format, &what) ==
	    FSIMG_OK && format.wide && format.entry_bytes == 8,
	    "bit 6 with 8-byte entries under 48BIT is wide");
}

static void
test_chunk_format_sets_size_and_entry_width(void)
{
	static const struct {
		unsigned	 blkszbits;
		uint32_t	 word;
		uint8_t		 chunkbits;
		uint64_t	 chunk_bytes;
		uint8_t		 entry_bytes;
	} cases[] = {
		{ .blkszbits = 12, .word = 0x0000, .chunkbits = 0,
		    .chunk_bytes = 4096, .entry_bytes = 4 },
		{ .blkszbits = 12, .word = 0x0005, .chunkbits = 5,
		    .chunk_bytes = 4096 << 5, .entry_bytes = 4 },
		{ .blkszbits = 12, .word = 0x0020, .chunkbits = 0,
		    .chunk_bytes = 4096, .entry_bytes = 8 },
		{ .blkszbits = 12, .word = 0x0025, .chunkbits = 5,
		    .chunk_bytes = 4096 << 5, .entry_bytes = 8 },
		{ .blkszbits = 9, .word = 0x001f, .chunkbits = 31,
		    .chunk_bytes = UINT64_C(512) << 31, .entry_bytes = 4 },
		{ .blkszbits = 12, .word = 0x001f, .chunkbits = 31,
		    .chunk_bytes = UINT64_C(4096) << 31, .entry_bytes = 4 },
		{ .blkszbits = 10, .word = 0xabcd0003, .chunkbits = 3,
		    .chunk_bytes = 1024 << 3, .entry_bytes = 4 },
		{ .blkszbits = 11, .word = 0xffff0000, .chunkbits = 0,
		    .chunk_bytes = 2048, .entry_bytes = 4 },
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		erofs_super_t sb = super_for_inodes(cases[i].blkszbits, 0, 0,
		    0);
		erofs_chunk_format_t format;
		const char *what = NULL;
		fsimg_status_t status;

		status = erofs_chunk_format_decode(cases[i].word, &sb, &format,
		    &what);
		EXPECT(status == FSIMG_OK &&
		    format.chunkbits == cases[i].chunkbits &&
		    format.chunk_bytes == cases[i].chunk_bytes &&
		    format.entry_bytes == cases[i].entry_bytes &&
		    !format.wide, "blkszbits %u word 0x%08x",
		    cases[i].blkszbits, (unsigned)cases[i].word);
	}
}

typedef struct chunk_case {
	const char	*name;
	bool		 wide;
	uint8_t		 entry_bytes;
	size_t		 length;
	unsigned char	 entry[8];
	fsimg_status_t	 status;
	bool		 hole;
	uint64_t	 block;
} chunk_case_t;

/* An 8-byte entry: high 16 bits, device 16 bits, low 32 bits. */
#define ENTRY8(high, device, low) \
	{ (uint8_t)(high), (uint8_t)((high) >> 8), (uint8_t)(device), \
	    (uint8_t)((device) >> 8), LE32(low) }

static const chunk_case_t chunk_cases[] = {
	{ .name = "four_byte_block", .entry_bytes = 4, .length = 4,
	    .entry = { LE32(0x12345678) }, .status = FSIMG_OK,
	    .block = 0x12345678 },
	{ .name = "four_byte_block_0", .entry_bytes = 4, .length = 4,
	    .entry = { LE32(0) }, .status = FSIMG_OK },
	{ .name = "four_byte_hole", .entry_bytes = 4, .length = 4,
	    .entry = { LE32(0xffffffff) }, .status = FSIMG_OK, .hole = true },
	{ .name = "four_byte_entry_of_3_bytes_is_corrupt", .entry_bytes = 4,
	    .length = 3, .entry = { LE32(0x12345678) },
	    .status = FSIMG_ECORRUPT },
	{ .name = "four_byte_entry_with_longer_input_is_accepted",
	    .entry_bytes = 4, .length = 100, .entry = { LE32(7) },
	    .status = FSIMG_OK, .block = 7 },
	{ .name = "eight_byte_block", .entry_bytes = 8, .length = 8,
	    .entry = ENTRY8(0, 0, 0x1234), .status = FSIMG_OK,
	    .block = 0x1234 },
	{ .name = "eight_byte_hole_is_zero_high_device_and_all_ones",
	    .entry_bytes = 8, .length = 8, .entry = ENTRY8(0, 0, 0xffffffff),
	    .status = FSIMG_OK, .hole = true },
	{ .name = "eight_byte_high_part_without_wide_is_corrupt",
	    .entry_bytes = 8, .length = 8, .entry = ENTRY8(1, 0, 5),
	    .status = FSIMG_ECORRUPT },
	{ .name = "eight_byte_all_ones_high_part_without_wide_is_corrupt",
	    .entry_bytes = 8, .length = 8,
	    .entry = ENTRY8(0xffff, 0, 0xffffffff), .status = FSIMG_ECORRUPT },
	{ .name = "eight_byte_device_identifier_is_corrupt",
	    .entry_bytes = 8, .length = 8, .entry = ENTRY8(0, 1, 5),
	    .status = FSIMG_ECORRUPT },
	{ .name = "eight_byte_hole_with_device_identifier_is_corrupt",
	    .entry_bytes = 8, .length = 8, .entry = ENTRY8(0, 1, 0xffffffff),
	    .status = FSIMG_ECORRUPT },
	{ .name = "eight_byte_entry_of_7_bytes_is_corrupt", .entry_bytes = 8,
	    .length = 7, .entry = ENTRY8(0, 0, 5), .status = FSIMG_ECORRUPT },
	{ .name = "wide_block_has_48_bits", .wide = true, .entry_bytes = 8,
	    .length = 8, .entry = ENTRY8(0x0001, 0, 2), .status = FSIMG_OK,
	    .block = UINT64_C(0x100000002) },
	{ .name = "wide_block_with_high_part_ffff_and_low_word_not_all_ones",
	    .wide = true, .entry_bytes = 8, .length = 8,
	    .entry = ENTRY8(0xffff, 0, 0xfffffffe), .status = FSIMG_OK,
	    .block = UINT64_C(0xfffffffffffe) },
	{ .name = "wide_low_word_all_ones_with_another_high_part_is_a_block",
	    .wide = true, .entry_bytes = 8, .length = 8,
	    .entry = ENTRY8(0x0001, 0, 0xffffffff), .status = FSIMG_OK,
	    .block = UINT64_C(0x1ffffffff) },
	{ .name = "wide_all_48_bits_ones_is_a_hole", .wide = true,
	    .entry_bytes = 8, .length = 8,
	    .entry = ENTRY8(0xffff, 0, 0xffffffff), .status = FSIMG_OK,
	    .hole = true },
	{ .name = "wide_zero_high_part_all_ones_low_word_is_a_block",
	    .wide = true, .entry_bytes = 8, .length = 8,
	    .entry = ENTRY8(0, 0, 0xffffffff), .status = FSIMG_OK,
	    .block = 0xffffffff },
	{ .name = "wide_device_identifier_is_corrupt", .wide = true,
	    .entry_bytes = 8, .length = 8, .entry = ENTRY8(1, 1, 5),
	    .status = FSIMG_ECORRUPT },
	{ .name = "wide_hole_with_device_identifier_is_corrupt",
	    .wide = true, .entry_bytes = 8, .length = 8,
	    .entry = ENTRY8(0xffff, 1, 0xffffffff), .status = FSIMG_ECORRUPT },
	{ .name = "wide_entry_of_7_bytes_is_corrupt", .wide = true,
	    .entry_bytes = 8, .length = 7, .entry = ENTRY8(1, 0, 5),
	    .status = FSIMG_ECORRUPT },
};

static void
test_chunk_entries_decode_to_blocks_and_holes(void)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(chunk_cases); i++) {
		const chunk_case_t *c = &chunk_cases[i];
		erofs_chunk_format_t format = { .chunk_bytes = 4096,
		    .chunkbits = 0, .entry_bytes = c->entry_bytes,
		    .wide = c->wide };
		unsigned char buf[100];
		erofs_chunk_t chunk, before;
		const char *what = NULL;
		fsimg_status_t status;

		bzero(buf, sizeof(buf));
		bcopy(c->entry, buf, sizeof(c->entry));
		memset(&chunk, SENTINEL, sizeof(chunk));
		before = chunk;
		status = erofs_chunk_decode(buf, c->length, &format, &chunk,
		    &what);

		EXPECT_STATUS(status, c->status, what, c->name);
		if (status == FSIMG_OK) {
			EXPECT(chunk.hole == c->hole && chunk.block == c->block,
			    "%s: hole=%d block=0x%llx", c->name,
			    (int)chunk.hole, (unsigned long long)chunk.block);
		} else {
			EXPECT(bcmp(&chunk, &before, sizeof(before)) == 0,
			    "%s: failure leaves the output untouched", c->name);
		}
	}
}
/* e1-geom-b512: image bytes [1184, 1254), the root directory's inline tail */
static const unsigned char geom_root_bytes[] = {
	/* image offset 1184, 70 bytes */
	0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x00,
	0x02, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x3d, 0x00, 0x02, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x3f, 0x00, 0x01, 0x00, 0x2a, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x44, 0x00, 0x02, 0x00, 0x2d, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x45, 0x00, 0x07, 0x00,
	0x2e, 0x2e, 0x2e, 0x61, 0x2e, 0x74, 0x78, 0x74, 0x64, 0x6c,
};
static const fixture_run_t geom_root_runs[] = {
	{ 1184, 0, 70 },
};

/*
 * e6-xattr-default: superblock, shared pool [1152, 1188) and the fixed
 * part and attribute region of inodes (NIDs) 56, 74, 99, 115 and 118.
 */
static const unsigned char xattr_default_bytes[] = {
	/* image offset 1024, 17 bytes */
	0xe2, 0xe1, 0xf5, 0xe0, 0x05, 0xb9, 0xc8, 0xe4, 0x07, 0x00,
	0x00, 0x00, 0x0c, 0x00, 0x26, 0x00, 0x1b,
	/* image offset 1060, 28 bytes */
	0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x33, 0x33,
	0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
	/* image offset 1152, 34 bytes */
	0x06, 0x01, 0x09, 0x00, 0x63, 0x6f, 0x6d, 0x6d, 0x6f, 0x6e,
	0x73, 0x61, 0x6d, 0x65, 0x76, 0x61, 0x6c, 0x75, 0x65, 0x00,
	0x08, 0x01, 0x02, 0x00, 0x70, 0x61, 0x69, 0x72, 0x61, 0x74,
	0x74, 0x72, 0x70, 0x76,
	/* image offset 1792, 56 bytes */
	0x04, 0x00, 0x04, 0x00, 0xa4, 0x81, 0x01, 0x00, 0x08, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0xff, 0xdf, 0xff, 0xfd, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x20, 0x01, 0x00, 0x00, 0x03, 0x01,
	0x01, 0x00, 0x6f, 0x77, 0x6e, 0x31,
	/* image offset 2368, 156 bytes */
	0x04, 0x00, 0x1d, 0x00, 0xfd, 0x41, 0x02, 0x00, 0x1b, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0xff, 0xff, 0xdf, 0xbf, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x34, 0x00, 0x02, 0x00,
	0x00, 0x00, 0x01, 0x00, 0x07, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x02, 0x00, 0x07, 0x00, 0xe8, 0x03, 0x00, 0x00, 0x04, 0x00,
	0x05, 0x00, 0xff, 0xff, 0xff, 0xff, 0x08, 0x00, 0x05, 0x00,
	0xe8, 0x03, 0x00, 0x00, 0x10, 0x00, 0x07, 0x00, 0xff, 0xff,
	0xff, 0xff, 0x20, 0x00, 0x05, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x00, 0x03, 0x34, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00,
	0x07, 0x00, 0xff, 0xff, 0xff, 0xff, 0x02, 0x00, 0x07, 0x00,
	0xe8, 0x03, 0x00, 0x00, 0x04, 0x00, 0x05, 0x00, 0xff, 0xff,
	0xff, 0xff, 0x08, 0x00, 0x05, 0x00, 0xe8, 0x03, 0x00, 0x00,
	0x10, 0x00, 0x07, 0x00, 0xff, 0xff, 0xff, 0xff, 0x20, 0x00,
	0x05, 0x00, 0xff, 0xff, 0xff, 0xff,
	/* image offset 3168, 93 bytes */
	0x04, 0x00, 0x0e, 0x00, 0xa4, 0x81, 0x01, 0x00, 0x03, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0xff, 0xf7, 0xff, 0xef, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x03, 0x06, 0x02, 0x00, 0x73, 0x65,
	0x63, 0x53, 0x53, 0x00, 0x00, 0x00, 0x07, 0x06, 0x1a, 0x00,
	0x73, 0x65, 0x6c, 0x69, 0x6e, 0x75, 0x78, 0x73, 0x79, 0x73,
	0x74, 0x65, 0x6d, 0x5f, 0x75, 0x3a, 0x6f, 0x62, 0x6a, 0x65,
	0x63, 0x74, 0x5f, 0x72, 0x3a, 0x65, 0x74, 0x63, 0x5f, 0x74,
	0x3a, 0x73, 0x30,
	/* image offset 3680, 91 bytes */
	0x04, 0x00, 0x0d, 0x00, 0xa4, 0x81, 0x01, 0x00, 0x04, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0xff, 0x7f, 0xff, 0xbe, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x08, 0x04, 0x01, 0x00, 0x76, 0x65,
	0x6e, 0x64, 0x6f, 0x72, 0x2e, 0x7a, 0x36, 0x00, 0x00, 0x00,
	0x07, 0x01, 0x01, 0x00, 0x6f, 0x74, 0x68, 0x65, 0x72, 0x2e,
	0x71, 0x35, 0x0e, 0x01, 0x01, 0x00, 0x76, 0x65, 0x6e, 0x64,
	0x6f, 0x72, 0x2e, 0x61, 0x6c, 0x70, 0x68, 0x61, 0x2e, 0x78,
	0x34,
	/* image offset 3776, 248 bytes */
	0x04, 0x00, 0x34, 0x00, 0xa4, 0x81, 0x01, 0x00, 0x05, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0xf7, 0x36, 0x38, 0xbc, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x02, 0x01, 0x02, 0x00, 0x6b, 0x31,
	0x76, 0x31, 0x03, 0x01, 0x03, 0x00, 0x6b, 0x31, 0x30, 0x76,
	0x31, 0x30, 0x00, 0x00, 0x03, 0x01, 0x03, 0x00, 0x6b, 0x31,
	0x31, 0x76, 0x31, 0x31, 0x00, 0x00, 0x03, 0x01, 0x03, 0x00,
	0x6b, 0x31, 0x32, 0x76, 0x31, 0x32, 0x00, 0x00, 0x03, 0x01,
	0x03, 0x00, 0x6b, 0x31, 0x33, 0x76, 0x31, 0x33, 0x00, 0x00,
	0x03, 0x01, 0x03, 0x00, 0x6b, 0x31, 0x34, 0x76, 0x31, 0x34,
	0x00, 0x00, 0x03, 0x01, 0x03, 0x00, 0x6b, 0x31, 0x35, 0x76,
	0x31, 0x35, 0x00, 0x00, 0x03, 0x01, 0x03, 0x00, 0x6b, 0x31,
	0x36, 0x76, 0x31, 0x36, 0x00, 0x00, 0x03, 0x01, 0x03, 0x00,
	0x6b, 0x31, 0x37, 0x76, 0x31, 0x37, 0x00, 0x00, 0x03, 0x01,
	0x03, 0x00, 0x6b, 0x31, 0x38, 0x76, 0x31, 0x38, 0x00, 0x00,
	0x03, 0x01, 0x03, 0x00, 0x6b, 0x31, 0x39, 0x76, 0x31, 0x39,
	0x00, 0x00, 0x02, 0x01, 0x02, 0x00, 0x6b, 0x32, 0x76, 0x32,
	0x03, 0x01, 0x03, 0x00, 0x6b, 0x32, 0x30, 0x76, 0x32, 0x30,
	0x00, 0x00, 0x02, 0x01, 0x02, 0x00, 0x6b, 0x33, 0x76, 0x33,
	0x02, 0x01, 0x02, 0x00, 0x6b, 0x34, 0x76, 0x34, 0x02, 0x01,
	0x02, 0x00, 0x6b, 0x35, 0x76, 0x35, 0x02, 0x01, 0x02, 0x00,
	0x6b, 0x36, 0x76, 0x36, 0x02, 0x01, 0x02, 0x00, 0x6b, 0x37,
	0x76, 0x37, 0x02, 0x01, 0x02, 0x00, 0x6b, 0x38, 0x76, 0x38,
	0x02, 0x01, 0x02, 0x00, 0x6b, 0x39, 0x76, 0x39,
};
static const fixture_run_t xattr_default_runs[] = {
	{ 1024, 0, 17 },
	{ 1060, 17, 28 },
	{ 1152, 45, 34 },
	{ 1792, 79, 56 },
	{ 2368, 135, 156 },
	{ 3168, 291, 93 },
	{ 3680, 384, 91 },
	{ 3776, 475, 248 },
};

/*
 * e6-xattr-pfx-plain: superblock, shared pool [1152, 1188), prefix table
 * [1188, 1216) and the fixed part and attribute region of inodes (NIDs)
 * 107, 111, 113, 124 and 128.
 */
static const unsigned char xattr_prefixes_bytes[] = {
	/* image offset 1024, 17 bytes */
	0xe2, 0xe1, 0xf5, 0xe0, 0x97, 0x40, 0x95, 0x6f, 0x17, 0x00,
	0x00, 0x00, 0x0c, 0x00, 0x26, 0x00, 0x1b,
	/* image offset 1060, 28 bytes */
	0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x33, 0x33,
	0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
	/* image offset 1104, 14 bytes */
	0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x02, 0x29, 0x01,
	/* image offset 1152, 34 bytes */
	0x06, 0x01, 0x09, 0x00, 0x63, 0x6f, 0x6d, 0x6d, 0x6f, 0x6e,
	0x73, 0x61, 0x6d, 0x65, 0x76, 0x61, 0x6c, 0x75, 0x65, 0x00,
	0x08, 0x01, 0x02, 0x00, 0x70, 0x61, 0x69, 0x72, 0x61, 0x74,
	0x74, 0x72, 0x70, 0x76,
	/* image offset 1188, 26 bytes */
	0x0e, 0x00, 0x01, 0x76, 0x65, 0x6e, 0x64, 0x6f, 0x72, 0x2e,
	0x61, 0x6c, 0x70, 0x68, 0x61, 0x2e, 0x08, 0x00, 0x04, 0x76,
	0x65, 0x6e, 0x64, 0x6f, 0x72, 0x2e,
	/* image offset 3424, 50 bytes */
	0x04, 0x00, 0x03, 0x00, 0xa4, 0x81, 0x01, 0x00, 0x04, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0xff, 0x7f, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x80, 0x01, 0x00, 0x78, 0x31,
	/* image offset 3552, 50 bytes */
	0x04, 0x00, 0x03, 0x00, 0xa4, 0x81, 0x01, 0x00, 0x04, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0xff, 0xff, 0xff, 0xfe, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x81, 0x01, 0x00, 0x7a, 0x33,
	/* image offset 3616, 70 bytes */
	0x04, 0x00, 0x08, 0x00, 0xa4, 0x81, 0x01, 0x00, 0x04, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0xff, 0x7f, 0xff, 0xbe, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x81, 0x01, 0x00, 0x7a, 0x36,
	0x00, 0x00, 0x07, 0x01, 0x01, 0x00, 0x6f, 0x74, 0x68, 0x65,
	0x72, 0x2e, 0x71, 0x35, 0x01, 0x80, 0x01, 0x00, 0x78, 0x34,
	/* image offset 3968, 114 bytes */
	0x04, 0x00, 0x13, 0x00, 0xa4, 0x81, 0x01, 0x00, 0x06, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0xff, 0xdf, 0xdf, 0xbf, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x20, 0x01, 0x00, 0x00, 0x00, 0x02,
	0x2c, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0x00,
	0xff, 0xff, 0xff, 0xff, 0x02, 0x00, 0x04, 0x00, 0xe8, 0x03,
	0x00, 0x00, 0x04, 0x00, 0x04, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x10, 0x00, 0x04, 0x00, 0xff, 0xff, 0xff, 0xff, 0x20, 0x00,
	0x04, 0x00, 0xff, 0xff, 0xff, 0xff, 0x01, 0x04, 0x02, 0x00,
	0x6d, 0x6d, 0x6d, 0x00, 0x01, 0x01, 0x05, 0x00, 0x61, 0x6d,
	0x69, 0x78, 0x65, 0x64,
	/* image offset 4096, 46 bytes */
	0x04, 0x00, 0x02, 0x00, 0xa4, 0x81, 0x01, 0x00, 0x06, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0xff, 0xff, 0xbf, 0xff, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x25, 0x01,
};
static const fixture_run_t xattr_prefixes_runs[] = {
	{ 1024, 0, 17 },
	{ 1060, 17, 28 },
	{ 1104, 45, 14 },
	{ 1152, 59, 34 },
	{ 1188, 93, 26 },
	{ 3424, 119, 50 },
	{ 3552, 169, 50 },
	{ 3616, 219, 70 },
	{ 3968, 289, 114 },
	{ 4096, 403, 46 },
};


/* ---- Directory blocks ---- */

typedef struct dname {
	uint64_t	 nid;
	const char	*name;
	size_t		 length;
	unsigned	 type;
} dname_t;

/* A literal may hold NUL bytes: its length is the array's, not strlen's. */
#define DNAME(nid, literal, type) { (nid), (literal), sizeof(literal) - 1, \
	    (type) }

enum { DIR_BLOCK_BYTES = 4096 };

/*
 * Lay out a directory block of size bytes: the 12-byte entries, then the
 * names back to back, then zeros. Returns the offset just past the last name.
 */
static size_t
dir_block_build(unsigned char *blk, size_t size, const dname_t *names,
    size_t count)
{
	size_t name_offset = count * EROFS_DIRENT_BYTES;
	size_t i;

	bzero(blk, size);
	for (i = 0; i < count; i++) {
		unsigned char *entry = blk + i * EROFS_DIRENT_BYTES;

		CHECK(name_offset + names[i].length <= size);
		put_le(entry, names[i].nid, 8);
		put_le(entry + 8, name_offset, 2);
		entry[10] = (unsigned char)names[i].type;
		bcopy(names[i].name, blk + name_offset, names[i].length);
		name_offset += names[i].length;
	}
	return (name_offset);
}

/* Entry i of a block starts at 12 * i; name offset at +8, type at +10. */
static void
dir_set_name_offset(unsigned char *blk, size_t entry, uint64_t offset)
{
	put_le(blk + entry * EROFS_DIRENT_BYTES + 8, offset, 2);
}

static const dname_t abc_names[] = {
	DNAME(11, "a", EROFS_FT_REGULAR),
	DNAME(22, "bb", EROFS_FT_DIRECTORY),
	DNAME(33, "ccc", EROFS_FT_SYMLINK),
};

/* The block above: entries at 0..35, names "a" 36, "bb" 37, "ccc" 39..41. */
enum { ABC_END = 42 };

static fsimg_status_t
dirblock_decode_checked(const char *name, const unsigned char *bytes,
    size_t length, erofs_dirblock_t *block, const char **what)
{
	erofs_dirblock_t before;
	fsimg_status_t status;

	memset(block, SENTINEL, sizeof(*block));
	before = *block;
	*what = NULL;
	status = erofs_dirblock_decode(bytes, length, block, what);
	if (status != FSIMG_OK) {
		EXPECT(bcmp(block, &before, sizeof(before)) == 0,
		    "%s: failure leaves the output untouched", name);
		/* The sentinel is not a valid bool; no caller may read it. */
		bzero(block, sizeof(*block));
	}
	return (status);
}

/* The status of decoding a block, which validates every entry. */
static fsimg_status_t
dir_status(const char *name, const unsigned char *blk, size_t length)
{
	erofs_dirblock_t block;
	const char *what;

	return (dirblock_decode_checked(name, blk, length, &block, &what));
}

static void
test_directory_block_shorter_than_one_entry_is_corrupt(void)
{
	unsigned char blk[DIR_BLOCK_BYTES];

	bzero(blk, sizeof(blk));
	EXPECT(dir_status("length_0", blk, 0) == FSIMG_ECORRUPT, "length 0");
	EXPECT(dir_status("length_11", blk, 11) == FSIMG_ECORRUPT, "length 11");
}

static void
test_first_name_offset_defines_the_entry_count(void)
{
	unsigned char blk[DIR_BLOCK_BYTES];
	erofs_dirblock_t block;
	const char *what;
	static const dname_t ten[] = {
		DNAME(1, "a", 1), DNAME(2, "b", 1), DNAME(3, "c", 1),
		DNAME(4, "d", 1), DNAME(5, "e", 1), DNAME(6, "f", 1),
		DNAME(7, "g", 1), DNAME(8, "h", 1), DNAME(9, "i", 1),
		DNAME(10, "j", 1)
	};

	dir_block_build(blk, sizeof(blk), abc_names, 3);
	EXPECT(dirblock_decode_checked("three", blk, sizeof(blk), &block,
	    &what) == FSIMG_OK && block.count == 3 &&
	    block.length == sizeof(blk) && block.bytes == blk,
	    "three entries: first name offset 36");

	dir_block_build(blk, sizeof(blk), ten, 10);
	EXPECT(dirblock_decode_checked("ten", blk, sizeof(blk), &block,
	    &what) == FSIMG_OK && block.count == 10,
	    "ten entries: first name offset 120");

	dir_block_build(blk, sizeof(blk), abc_names, 1);
	EXPECT(dirblock_decode_checked("one", blk, ABC_END, &block, &what) ==
	    FSIMG_OK && block.count == 1 && block.length == ABC_END,
	    "one entry, length as given");
}

static void
test_first_name_offset_must_be_a_nonzero_multiple_of_12_below_the_length(void)
{
	unsigned char blk[DIR_BLOCK_BYTES];

	dir_block_build(blk, sizeof(blk), abc_names, 3);
	dir_set_name_offset(blk, 0, 0);
	EXPECT(dir_status("offset_0", blk, sizeof(blk)) == FSIMG_ECORRUPT,
	    "first name offset 0");

	dir_block_build(blk, sizeof(blk), abc_names, 3);
	dir_set_name_offset(blk, 0, 37);
	EXPECT(dir_status("offset_37", blk, sizeof(blk)) == FSIMG_ECORRUPT,
	    "first name offset 37 is not a multiple of 12");
	dir_set_name_offset(blk, 0, 13);
	EXPECT(dir_status("offset_13", blk, sizeof(blk)) == FSIMG_ECORRUPT,
	    "first name offset 13");

	dir_block_build(blk, sizeof(blk), abc_names, 1);
	dir_set_name_offset(blk, 0, 12);
	EXPECT(dir_status("offset_12_length_12", blk, 12) == FSIMG_ECORRUPT,
	    "first name offset 12 is not below length 12");
	EXPECT(dir_status("offset_12_length_13", blk, 13) == FSIMG_OK,
	    "first name offset 12 is below length 13 (name \"a\")");

	dir_block_build(blk, sizeof(blk), abc_names, 3);
	EXPECT(dir_status("offset_36_length_36", blk, 36) == FSIMG_ECORRUPT,
	    "first name offset 36 is not below length 36");
	EXPECT(dir_status("offset_36_length_37", blk, 37) == FSIMG_ECORRUPT,
	    "the other entries' offsets 37 and 39 are past length 37");
}

static void
test_entry_fields_and_name_spans_are_decoded(void)
{
	unsigned char blk[DIR_BLOCK_BYTES];
	erofs_dirblock_t block;
	erofs_dirent_t ent;
	const char *what;
	size_t i;
	static const struct {
		uint64_t nid;
		uint16_t offset;
		uint16_t length;
		uint8_t type;
	} want[3] = {
		{ .nid = 11, .offset = 36, .length = 1,
		    .type = EROFS_FT_REGULAR },
		{ .nid = 22, .offset = 37, .length = 2,
		    .type = EROFS_FT_DIRECTORY },
		{ .nid = 33, .offset = 39, .length = 3,
		    .type = EROFS_FT_SYMLINK },
	};
	static const size_t valid_lengths[] = { ABC_END, DIR_BLOCK_BYTES };

	for (size_t v = 0; v < ARRAY_SIZE(valid_lengths); v++) {
		dir_block_build(blk, sizeof(blk), abc_names, 3);
		CHECK(dirblock_decode_checked("abc", blk, valid_lengths[v],
		    &block, &what) == FSIMG_OK);
		for (i = 0; i < 3; i++) {
			memset(&ent, SENTINEL, sizeof(ent));
			EXPECT(erofs_dirent_decode(&block, i, &ent, &what) ==
			    FSIMG_OK && ent.nid == want[i].nid &&
			    ent.name_offset == want[i].offset &&
			    ent.name_length == want[i].length &&
			    ent.file_type == want[i].type,
			    "valid length %zu, entry %zu", valid_lengths[v], i);
		}
	}
}

static void
test_last_name_ends_at_the_valid_length_or_the_first_nul(void)
{
	unsigned char blk[DIR_BLOCK_BYTES];
	erofs_dirblock_t block;
	erofs_dirent_t ent;
	const char *what;

	/* Valid length 41 cuts "ccc" to "cc": the name ends with the block. */
	dir_block_build(blk, sizeof(blk), abc_names, 3);
	CHECK(dirblock_decode_checked("cut", blk, ABC_END - 1, &block,
	    &what) == FSIMG_OK);
	EXPECT(erofs_dirent_decode(&block, 2, &ent, &what) == FSIMG_OK &&
	    ent.name_length == 2, "valid length 41: last name is 2 bytes");

	/* "a", NUL, "b" as the last name: it ends at the NUL. */
	{
		static const dname_t names[] = {
			DNAME(1, "x", EROFS_FT_REGULAR),
			DNAME(2, "a\0b", EROFS_FT_REGULAR),
		};

		dir_block_build(blk, sizeof(blk), names, 2);
		CHECK(dirblock_decode_checked("nul", blk, sizeof(blk), &block,
		    &what) == FSIMG_OK);
		EXPECT(erofs_dirent_decode(&block, 1, &ent, &what) ==
		    FSIMG_OK && ent.name_length == 1 &&
		    ent.name_offset == 25, "last name stops at its NUL");
		EXPECT(erofs_dirent_decode(&block, 0, &ent, &what) ==
		    FSIMG_OK && ent.name_length == 1,
		    "the middle name is bounded by the next offset");
	}
}

static void
test_name_offsets_must_increase_strictly(void)
{
	unsigned char blk[DIR_BLOCK_BYTES];

	dir_block_build(blk, sizeof(blk), abc_names, 3);
	dir_set_name_offset(blk, 1, 36);
	EXPECT(dir_status("equal_0_1", blk, sizeof(blk)) == FSIMG_ECORRUPT,
	    "entry 1 offset equals entry 0's");

	dir_block_build(blk, sizeof(blk), abc_names, 3);
	dir_set_name_offset(blk, 2, 37);
	EXPECT(dir_status("equal_1_2", blk, sizeof(blk)) == FSIMG_ECORRUPT,
	    "entry 2 offset equals entry 1's");

	dir_block_build(blk, sizeof(blk), abc_names, 3);
	dir_set_name_offset(blk, 2, 36);
	EXPECT(dir_status("decreasing_last", blk, sizeof(blk)) ==
	    FSIMG_ECORRUPT, "entry 2 offset 36 is below entry 1's 37");

	dir_block_build(blk, sizeof(blk), abc_names, 3);
	dir_set_name_offset(blk, 1, 40);
	EXPECT(dir_status("decreasing_middle", blk, sizeof(blk)) ==
	    FSIMG_ECORRUPT, "entry 2 offset 39 is below entry 1's 40");
}

static void
test_name_offset_inside_the_entry_array_is_corrupt(void)
{
	unsigned char blk[DIR_BLOCK_BYTES];

	dir_block_build(blk, sizeof(blk), abc_names, 3);
	dir_set_name_offset(blk, 1, 24);
	EXPECT(dir_status("inside_array", blk, sizeof(blk)) == FSIMG_ECORRUPT,
	    "entry 1 name offset 24 lies in the entry array");

	dir_block_build(blk, sizeof(blk), abc_names, 3);
	dir_set_name_offset(blk, 2, 4);
	EXPECT(dir_status("inside_array_last", blk, sizeof(blk)) ==
	    FSIMG_ECORRUPT, "entry 2 name offset 4 lies in the entry array");
}

static void
test_name_offset_past_the_valid_length_is_corrupt(void)
{
	unsigned char blk[DIR_BLOCK_BYTES];

	dir_block_build(blk, sizeof(blk), abc_names, 3);
	dir_set_name_offset(blk, 2, ABC_END + 1);
	EXPECT(dir_status("past_end", blk, ABC_END) == FSIMG_ECORRUPT,
	    "last name offset 43 with valid length 42");

	dir_block_build(blk, sizeof(blk), abc_names, 3);
	dir_set_name_offset(blk, 1, 100);
	EXPECT(dir_status("middle_past_end", blk, ABC_END) == FSIMG_ECORRUPT,
	    "middle name offset 100 with valid length 42");
}

static void
test_empty_last_name_is_corrupt(void)
{
	unsigned char blk[DIR_BLOCK_BYTES];

	dir_block_build(blk, sizeof(blk), abc_names, 3);
	dir_set_name_offset(blk, 2, ABC_END);
	EXPECT(dir_status("empty_at_end", blk, ABC_END) == FSIMG_ECORRUPT,
	    "last name offset equals the valid length");
	EXPECT(dir_status("empty_at_nul", blk, sizeof(blk)) == FSIMG_ECORRUPT,
	    "last name starts at a NUL byte");

	dir_set_name_offset(blk, 2, 2000);
	EXPECT(dir_status("empty_in_zeros", blk, sizeof(blk)) ==
	    FSIMG_ECORRUPT, "last name starts inside the zero padding");
}

/* A two-entry block: a name of the given length, then "z". */
static fsimg_status_t
long_name_status(const char *name, size_t length, bool last)
{
	unsigned char blk[DIR_BLOCK_BYTES];
	unsigned char text[300];
	dname_t z = DNAME(1, "z", EROFS_FT_REGULAR);
	dname_t x = { 2, (const char *)text, length, EROFS_FT_REGULAR };
	dname_t names[2];

	memset(text, 'x', sizeof(text));
	names[0] = last ? z : x;
	names[1] = last ? x : z;
	dir_block_build(blk, sizeof(blk), names, 2);
	return (dir_status(name, blk, sizeof(blk)));
}

static void
test_names_longer_than_255_bytes_are_corrupt(void)
{
	EXPECT(long_name_status("middle_255", 255, false) == FSIMG_OK,
	    "255-byte middle name");
	EXPECT(long_name_status("middle_256", 256, false) == FSIMG_ECORRUPT,
	    "256-byte middle name");
	EXPECT(long_name_status("last_255", 255, true) == FSIMG_OK,
	    "255-byte last name");
	EXPECT(long_name_status("last_256", 256, true) == FSIMG_ECORRUPT,
	    "256-byte last name");
	EXPECT(long_name_status("last_300", 300, true) == FSIMG_ECORRUPT,
	    "300-byte last name");
}

static void
test_slash_or_nul_in_a_name_is_corrupt(void)
{
	unsigned char blk[DIR_BLOCK_BYTES];
	static const dname_t slash_middle[] = {
		DNAME(1, "a/b", 1), DNAME(2, "z", 1) };
	static const dname_t slash_last[] = {
		DNAME(1, "z", 1), DNAME(2, "a/b", 1) };
	static const dname_t slash_only[] = {
		DNAME(1, "z", 1), DNAME(2, "/", 1) };
	static const dname_t nul_middle[] = {
		DNAME(1, "a\0b", 1), DNAME(2, "z", 1) };
	static const dname_t nul_before_slash[] = {
		DNAME(1, "z", 1), DNAME(2, "a\0/", 1) };

	dir_block_build(blk, sizeof(blk), slash_middle, 2);
	EXPECT(dir_status("slash_middle", blk, sizeof(blk)) == FSIMG_ECORRUPT,
	    "slash in a middle name");
	dir_block_build(blk, sizeof(blk), slash_last, 2);
	EXPECT(dir_status("slash_last", blk, sizeof(blk)) == FSIMG_ECORRUPT,
	    "slash in the last name");
	dir_block_build(blk, sizeof(blk), slash_only, 2);
	EXPECT(dir_status("slash_only", blk, sizeof(blk)) == FSIMG_ECORRUPT,
	    "a name that is a slash");
	dir_block_build(blk, sizeof(blk), nul_middle, 2);
	EXPECT(dir_status("nul_middle", blk, sizeof(blk)) == FSIMG_ECORRUPT,
	    "NUL inside a middle name");
	dir_block_build(blk, sizeof(blk), nul_before_slash, 2);
	EXPECT(dir_status("nul_before_slash", blk, sizeof(blk)) == FSIMG_OK,
	    "the last name ends at its NUL, before the slash");
}

static void
test_file_types_0_to_7_are_accepted_and_8_is_corrupt(void)
{
	static const unsigned types[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 15,
	    255 };
	size_t i, entry;

	for (i = 0; i < ARRAY_SIZE(types); i++) {
		for (entry = 0; entry < 3; entry++) {
			unsigned char blk[DIR_BLOCK_BYTES];
			bool valid = types[i] <= 7;

			dir_block_build(blk, sizeof(blk), abc_names, 3);
			blk[entry * EROFS_DIRENT_BYTES + 10] =
			    (unsigned char)types[i];
			EXPECT(dir_status("type", blk, sizeof(blk)) ==
			    (valid ? FSIMG_OK : FSIMG_ECORRUPT),
			    "file type %u in entry %zu", types[i], entry);
		}
	}
}

static void
test_entry_index_past_the_count_is_invalid(void)
{
	unsigned char blk[DIR_BLOCK_BYTES];
	erofs_dirblock_t block;
	erofs_dirent_t ent, before;
	const char *what;
	size_t indexes[] = { 3, 4, 1000, SIZE_MAX };
	size_t i;

	dir_block_build(blk, sizeof(blk), abc_names, 3);
	CHECK(dirblock_decode_checked("abc", blk, sizeof(blk), &block, &what) ==
	    FSIMG_OK);
	for (i = 0; i < ARRAY_SIZE(indexes); i++) {
		memset(&ent, SENTINEL, sizeof(ent));
		before = ent;
		EXPECT(erofs_dirent_decode(&block, indexes[i], &ent, &what) ==
		    FSIMG_EINVAL && bcmp(&ent, &before, sizeof(ent)) == 0,
		    "index %zu is the caller's error and leaves the output",
		    indexes[i]);
	}
}

/* The root of e1-geom-b512, an inline tail of 70 bytes. */
static void
test_real_root_directory_block_decodes_to_the_dumped_names(void)
{
	static const fixture_t fixture = FIXTURE(geom_root, 1184);
	static const struct {
		uint64_t	 nid;
		const char	*name;
		uint8_t		 type;
	} want[] = {
		{ 36, ".", EROFS_FT_DIRECTORY },
		{ 36, "..", EROFS_FT_DIRECTORY },
		{ 40, "a.txt", EROFS_FT_REGULAR },
		{ 42, "d", EROFS_FT_DIRECTORY },
		{ 45, "l", EROFS_FT_SYMLINK },
	};
	unsigned char tail[70];
	erofs_dirblock_t block;
	const char *what;
	size_t i;

	fixture_load(&fixture, tail, sizeof(tail));
	CHECK(dirblock_decode_checked("e1-geom-b512", tail, sizeof(tail),
	    &block, &what) == FSIMG_OK);
	EXPECT(block.count == 5, "five entries");
	for (i = 0; i < ARRAY_SIZE(want) && i < block.count; i++) {
		erofs_dirent_t ent;

		CHECK(erofs_dirent_decode(&block, i, &ent, &what) == FSIMG_OK);
		EXPECT(ent.nid == want[i].nid &&
		    ent.file_type == want[i].type &&
		    ent.name_length == strlen(want[i].name) &&
		    bcmp(tail + ent.name_offset, want[i].name,
		    ent.name_length) == 0, "entry %zu is %s", i, want[i].name);
	}
}

/* ---- Extended attributes ---- */

static void
test_attribute_header_must_fit_the_region(void)
{
	unsigned char region[1100];
	erofs_xattr_header_t header, before;
	const char *what = NULL;
	static const struct {
		const char	*name;
		unsigned	 shared;
		size_t		 length;
		fsimg_status_t	 status;
	} cases[] = {
		{ "length_0", 0, 0, FSIMG_ECORRUPT },
		{ "length_11", 0, 11, FSIMG_ECORRUPT },
		{ "length_12_no_shared", 0, 12, FSIMG_OK },
		{ "length_12_one_shared", 1, 12, FSIMG_ECORRUPT },
		{ "one_shared_exact", 1, 16, FSIMG_OK },
		{ "three_shared_one_byte_short", 3, 23, FSIMG_ECORRUPT },
		{ "three_shared_exact", 3, 24, FSIMG_OK },
		{ "three_shared_with_inline_entries", 3, 100, FSIMG_OK },
		{ "255_shared_one_byte_short", 255, 1031, FSIMG_ECORRUPT },
		{ "255_shared_exact", 255, 1032, FSIMG_OK },
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		fsimg_status_t status;

		bzero(region, sizeof(region));
		put_le(region, 0xdeadbeef, 4);
		region[4] = (unsigned char)cases[i].shared;
		memset(&header, SENTINEL, sizeof(header));
		before = header;
		status = erofs_xattr_header_decode(region, cases[i].length,
		    &header, &what);
		EXPECT_STATUS(status, cases[i].status, what, cases[i].name);
		EXPECT(status != FSIMG_OK ||
		    (header.filter == 0xdeadbeef &&
		    header.shared_count == cases[i].shared &&
		    header.entries_offset == 12 + 4 * cases[i].shared),
		    "%s: filter, shared count and entries offset",
		    cases[i].name);
		EXPECT(status == FSIMG_OK ||
		    bcmp(&header, &before, sizeof(before)) == 0,
		    "%s: failure leaves the output untouched", cases[i].name);
	}
}

static void
test_attribute_header_reserved_bytes_are_not_required_zero(void)
{
	unsigned char region[12];
	erofs_xattr_header_t header;
	const char *what = NULL;

	memset(region, 0xff, sizeof(region));
	region[4] = 0;
	EXPECT(erofs_xattr_header_decode(region, sizeof(region), &header,
	    &what) == FSIMG_OK && header.filter == 0xffffffff &&
	    header.shared_count == 0 && header.entries_offset == 12,
	    "bytes 5..11 hold 0xff");
}

static void
test_shared_ids_are_read_from_offset_12(void)
{
	unsigned char region[32];
	erofs_xattr_header_t header;
	const char *what = NULL;
	uint32_t id;

	bzero(region, sizeof(region));
	region[4] = 3;
	put_le(region + 12, 288, 4);
	put_le(region + 16, 0xffffffff, 4);
	put_le(region + 20, 1, 4);
	CHECK(erofs_xattr_header_decode(region, sizeof(region), &header,
	    &what) == FSIMG_OK);

	id = 0;
	EXPECT(erofs_xattr_shared_id(region, &header, 0, &id) && id == 288,
	    "index 0");
	EXPECT(erofs_xattr_shared_id(region, &header, 1, &id) &&
	    id == 0xffffffff, "index 1");
	EXPECT(erofs_xattr_shared_id(region, &header, 2, &id) && id == 1,
	    "index 2");
}

static void
test_shared_id_past_the_shared_count_is_false_and_leaves_the_output(void)
{
	static const unsigned indexes[] = { 3, 4, 255, UINT_MAX };
	unsigned char region[32];
	erofs_xattr_header_t header;
	const char *what = NULL;
	uint32_t id;
	size_t i;

	/* Bytes sit past the count's ids; they are not shared ids. */
	memset(region, 0x11, sizeof(region));
	region[4] = 3;
	CHECK(erofs_xattr_header_decode(region, sizeof(region), &header,
	    &what) == FSIMG_OK);
	for (i = 0; i < ARRAY_SIZE(indexes); i++) {
		id = 0x5a5a5a5a;
		EXPECT(!erofs_xattr_shared_id(region, &header, indexes[i],
		    &id) && id == 0x5a5a5a5a,
		    "index %u of 3 is false, id untouched", indexes[i]);
	}

	region[4] = 0;
	CHECK(erofs_xattr_header_decode(region, sizeof(region), &header,
	    &what) == FSIMG_OK);
	id = 0x5a5a5a5a;
	EXPECT(!erofs_xattr_shared_id(region, &header, 0, &id) &&
	    id == 0x5a5a5a5a, "index 0 of 0 is false, id untouched");
}

static void
test_shared_entry_offset_is_the_block_plus_4_per_id(void)
{
	static const struct {
		unsigned	 blkszbits;
		uint32_t	 block;
		uint32_t	 id;
		uint64_t	 offset;
	} cases[] = {
		{ 12, 0, 0, 0 },
		{ 12, 3, 5, 3 * 4096 + 20 },
		{ 9, 7, 288, 7 * 512 + 1152 },
		{ 12, 0xffffffff, 0xffffffff,
		    UINT64_C(0xffffffff) * 4096 + UINT64_C(4) * 0xffffffff },
		{ 9, 0xffffffff, 0xffffffff,
		    UINT64_C(0xffffffff) * 512 + UINT64_C(4) * 0xffffffff },
		{ 11, 1, 0xffffffff, 2048 + UINT64_C(4) * 0xffffffff },
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		erofs_super_t sb = super_for_inodes(cases[i].blkszbits, 0, 0,
		    0);

		sb.xattr_blkaddr = cases[i].block;
		EXPECT(erofs_xattr_shared_offset(&sb, cases[i].id) ==
		    cases[i].offset, "blkszbits %u block %u id %u",
		    cases[i].blkszbits, (unsigned)cases[i].block,
		    (unsigned)cases[i].id);
	}
}

typedef struct xattr_case {
	const char	*name;
	uint8_t		 index;
	size_t		 suffix;	/* suffix length */
	size_t		 value;		/* value length */
	long		 slack;		/* length given less padded size */
	unsigned	 prefixes;	/* the volume's long-prefix count */
	bool		 prefix_feature;
	fsimg_status_t	 status;
} xattr_case_t;

static const xattr_case_t xattr_cases[] = {
	{ .name = "padded_entry_fits_exactly", .index = 1, .suffix = 3,
	    .value = 2, .status = FSIMG_OK },
	{ .name = "entry_one_byte_short_of_padding_is_corrupt", .index = 1,
	    .suffix = 3, .value = 2, .slack = -1, .status = FSIMG_ECORRUPT },
	{ .name = "entry_with_slack_is_accepted", .index = 1, .suffix = 3,
	    .value = 2, .slack = 50, .status = FSIMG_OK },
	{ .name = "unpadded_size_multiple_of_4_fits_exactly", .index = 1,
	    .suffix = 4, .value = 4, .status = FSIMG_OK },
	{ .name = "unpadded_size_multiple_of_4_one_short_is_corrupt",
	    .index = 1, .suffix = 4, .value = 4, .slack = -1,
	    .status = FSIMG_ECORRUPT },
	{ .name = "empty_suffix_and_value_is_a_4_byte_entry", .index = 1,
	    .suffix = 0, .value = 0, .status = FSIMG_OK },
	{ .name = "largest_entry_is_accepted", .index = 1, .suffix = 255,
	    .value = 65535, .status = FSIMG_OK },
	{ .name = "largest_entry_one_byte_short_is_corrupt", .index = 1,
	    .suffix = 255, .value = 65535, .slack = -1,
	    .status = FSIMG_ECORRUPT },
	{ .name = "user_index_1_is_accepted", .index = 1, .suffix = 5,
	    .value = 7, .status = FSIMG_OK },
	{ .name = "trusted_index_4_is_accepted", .index = 4, .suffix = 5,
	    .value = 7, .status = FSIMG_OK },
	{ .name = "security_index_6_is_accepted", .index = 6, .suffix = 5,
	    .value = 7, .status = FSIMG_OK },
	{ .name = "acl_access_index_2_with_empty_suffix_is_accepted",
	    .index = 2, .suffix = 0, .value = 28, .status = FSIMG_OK },
	{ .name = "acl_default_index_3_with_empty_suffix_is_accepted",
	    .index = 3, .suffix = 0, .value = 28, .status = FSIMG_OK },
	{ .name = "acl_access_index_2_with_a_suffix_is_corrupt", .index = 2,
	    .suffix = 1, .value = 28, .status = FSIMG_ECORRUPT },
	{ .name = "acl_default_index_3_with_a_suffix_is_corrupt", .index = 3,
	    .suffix = 1, .value = 28, .status = FSIMG_ECORRUPT },
	{ .name = "index_0_is_unsupported", .index = 0, .suffix = 2,
	    .value = 2, .status = FSIMG_EUNSUPPORTED },
	{ .name = "index_5_is_unsupported", .index = 5, .suffix = 2,
	    .value = 2, .status = FSIMG_EUNSUPPORTED },
	{ .name = "index_7_is_unsupported", .index = 7, .suffix = 2,
	    .value = 2, .status = FSIMG_EUNSUPPORTED },
	{ .name = "index_0x7f_is_unsupported", .index = 0x7f, .suffix = 2,
	    .value = 2, .status = FSIMG_EUNSUPPORTED },
	{ .name = "index_0x40_is_unsupported", .index = 0x40, .suffix = 2,
	    .value = 2, .status = FSIMG_EUNSUPPORTED },
	{ .name = "long_prefix_without_the_feature_is_corrupt",
	    .index = 0x80, .suffix = 2, .value = 2, .prefixes = 2,
	    .status = FSIMG_ECORRUPT },
	{ .name = "long_prefix_with_no_prefixes_is_corrupt", .index = 0x80,
	    .suffix = 2, .value = 2, .prefix_feature = true,
	    .status = FSIMG_ECORRUPT },
	{ .name = "long_prefix_0_of_2_is_accepted", .index = 0x80,
	    .suffix = 2, .value = 2, .prefixes = 2, .prefix_feature = true,
	    .status = FSIMG_OK },
	{ .name = "long_prefix_1_of_2_is_accepted", .index = 0x81,
	    .suffix = 2, .value = 2, .prefixes = 2, .prefix_feature = true,
	    .status = FSIMG_OK },
	{ .name = "long_prefix_2_of_2_is_corrupt", .index = 0x82,
	    .suffix = 2, .value = 2, .prefixes = 2, .prefix_feature = true,
	    .status = FSIMG_ECORRUPT },
	{ .name = "long_prefix_127_of_2_is_corrupt", .index = 0xff,
	    .suffix = 2, .value = 2, .prefixes = 2, .prefix_feature = true,
	    .status = FSIMG_ECORRUPT },
	{ .name = "long_prefix_126_of_127_is_accepted", .index = 0xfe,
	    .suffix = 2, .value = 2, .prefixes = 127, .prefix_feature = true,
	    .status = FSIMG_OK },
};

static void
test_attribute_entries_are_validated_and_decoded(void)
{
	static unsigned char buf[70000];
	size_t i;

	for (i = 0; i < ARRAY_SIZE(xattr_cases); i++) {
		const xattr_case_t *c = &xattr_cases[i];
		erofs_super_t sb = super_for_inodes(12, c->prefix_feature ?
		    EROFS_INCOMPAT_XATTR_PREFIXES : 0, 0, 0);
		erofs_xattr_t x, before;
		size_t padded = (4 + c->suffix + c->value + 3) & ~(size_t)3;
		const char *what = NULL;
		fsimg_status_t status;

		sb.xattr_prefix_count = (uint8_t)c->prefixes;
		bzero(buf, 4 + c->suffix + c->value + 8);
		buf[0] = (unsigned char)c->suffix;
		buf[1] = c->index;
		put_le(buf + 2, c->value, 2);
		memset(buf + 4, 's', c->suffix);
		memset(buf + 4 + c->suffix, 'v', c->value);
		memset(&x, SENTINEL, sizeof(x));
		before = x;

		status = erofs_xattr_decode(buf,
		    (size_t)((long)padded + c->slack), &sb, &x, &what);

		EXPECT_STATUS(status, c->status, what, c->name);
		EXPECT(status != FSIMG_OK || (x.entry_bytes == padded &&
		    x.name_index == c->index &&
		    x.suffix_length == c->suffix &&
		    x.value_length == c->value),
		    "%s: entry_bytes %u (padded %zu), index 0x%x, suffix %u, "
		    "value %u", c->name, (unsigned)x.entry_bytes, padded,
		    (unsigned)x.name_index, (unsigned)x.suffix_length,
		    (unsigned)x.value_length);
		EXPECT(status == FSIMG_OK ||
		    bcmp(&x, &before, sizeof(before)) == 0,
		    "%s: failure leaves the output untouched", c->name);
	}
}

static void
test_attribute_entry_shorter_than_4_bytes_is_corrupt(void)
{
	unsigned char buf[8] = { 0, 1, 0, 0, 0, 0, 0, 0 };
	erofs_super_t sb = super_for_inodes(12, 0, 0, 0);
	erofs_xattr_t x;
	const char *what = NULL;
	size_t length;

	for (length = 0; length < 4; length++)
		EXPECT(erofs_xattr_decode(buf, length, &sb, &x, &what) ==
		    FSIMG_ECORRUPT, "length %zu", length);
	EXPECT(erofs_xattr_decode(buf, 4, &sb, &x, &what) == FSIMG_OK &&
	    x.entry_bytes == 4, "length 4 holds an empty entry");
}

static void
test_prefix_record_validation(void)
{
	/* Record L=8: base 4, infix "abcdefg" (7 bytes); 10 bytes pad to 12 */
	static const unsigned char record8[16] = { 8, 0, 4, 'a', 'b', 'c', 'd',
	    'e', 'f', 'g', 0, 0, 0xee, 0xee, 0xee, 0xee };
	unsigned char buf[16];
	erofs_prefix_t p, before;
	const char *what = NULL;
	size_t length;

	for (length = 0; length < 2; length++)
		EXPECT(erofs_prefix_decode(record8, length, &p, &what) ==
		    FSIMG_ECORRUPT, "length %zu is below the length word",
		    length);

	bcopy(record8, buf, sizeof(buf));
	put_le(buf, 0, 2);
	EXPECT(erofs_prefix_decode(buf, sizeof(buf), &p, &what) ==
	    FSIMG_ECORRUPT, "record length 0");

	for (length = 2; length < 10; length++)
		EXPECT(erofs_prefix_decode(record8, length, &p, &what) ==
		    FSIMG_ECORRUPT, "record of 10 bytes cut at %zu", length);

	memset(&p, SENTINEL, sizeof(p));
	EXPECT(erofs_prefix_decode(record8, 12, &p, &what) == FSIMG_OK &&
	    p.base_index == 4 && p.infix_length == 7 && p.record_bytes == 12,
	    "record padded to 12");

	bcopy(record8, buf, sizeof(buf));
	put_le(buf, 1, 2);
	memset(&p, SENTINEL, sizeof(p));
	EXPECT(erofs_prefix_decode(buf, 4, &p, &what) == FSIMG_OK &&
	    p.infix_length == 0 && p.record_bytes == 4,
	    "record length 1 is a base index alone");

	{
		static const unsigned char bases[] = { 1, 4, 6, 0, 5, 7, 0x80,
		    0x81, 0xff };
		size_t i;

		for (i = 0; i < ARRAY_SIZE(bases); i++) {
			bool builtin = bases[i] == 1 || bases[i] == 4 ||
			    bases[i] == 6;

			bcopy(record8, buf, sizeof(buf));
			buf[2] = bases[i];
			memset(&p, SENTINEL, sizeof(p));
			before = p;
			EXPECT((erofs_prefix_decode(buf, 12, &p, &what) ==
			    FSIMG_OK) == builtin, "base index 0x%x",
			    (unsigned)bases[i]);
			EXPECT(builtin || bcmp(&p, &before, sizeof(p)) == 0,
			    "base index 0x%x: failure leaves the output",
			    (unsigned)bases[i]);
		}
	}
}

/* ---- XXH32 and the name filter ---- */

/* The test message: byte i is (31 * i + 7) mod 256. */
static void
message_fill(unsigned char *msg, size_t length)
{
	size_t i;

	for (i = 0; i < length; i++)
		msg[i] = (unsigned char)(31 * i + 7);
}

/* Reference values: `xxhsum -H0 -s<seed> FILE` on the generated message. */
static const struct {
	size_t		 length;
	uint32_t	 seed;
	uint32_t	 hash;
} xxh_cases[] = {
	{ 0, 0, 0x02cc5d05 }, { 0, 1, 0x0b2cb792 },
	{ 0, 631893647, 0x7a17d3b4 }, { 0, 4294967295u, 0x9061da9d },
	{ 0, 633069711, 0x59ff5d54 }, { 1, 0, 0x002e0d32 },
	{ 1, 1, 0x628e4e70 }, { 1, 631893647, 0x88692909 },
	{ 4, 0, 0x073faa82 }, { 4, 1, 0x7c94ab13 },
	{ 4, 631893647, 0x624be495 }, { 4, 4294967295u, 0x82063e1a },
	{ 4, 633069711, 0x2ff521d5 }, { 15, 0, 0x9f29f87b },
	{ 15, 1, 0xdddd4dc2 }, { 15, 631893647, 0x9231563c },
	{ 16, 0, 0x3f6c9665 }, { 16, 1, 0x0e5c90ba },
	{ 16, 631893647, 0x1b3fa419 }, { 17, 0, 0xe048ecdb },
	{ 17, 1, 0xf6b42fec }, { 17, 631893647, 0xb358841c },
	{ 17, 4294967295u, 0xb0de87c9 }, { 17, 633069711, 0xe3afc88c },
	{ 31, 0, 0x00f1525c }, { 31, 1, 0x43fa4523 },
	{ 31, 631893647, 0xa0e5fda5 }, { 32, 0, 0xee42e668 },
	{ 32, 1, 0xd1cd5a82 }, { 32, 631893647, 0xc4c23cc6 },
	{ 33, 0, 0x770b5126 }, { 33, 1, 0xb19d0f8a },
	{ 33, 631893647, 0xa61926ce }, { 33, 4294967295u, 0x9d009823 },
	{ 33, 633069711, 0x6bc8b3ea }, { 100, 0, 0x75936eb8 },
	{ 100, 1, 0x0b9a0a5e }, { 100, 631893647, 0xb0d348fa },
	{ 100, 4294967295u, 0x8c2af6d6 }, { 100, 633069711, 0xd54445f5 },
};

static void
test_xxh32_matches_xxhsum_at_stripe_boundary_lengths(void)
{
	unsigned char msg[100];
	size_t i;

	message_fill(msg, sizeof(msg));
	for (i = 0; i < ARRAY_SIZE(xxh_cases); i++) {
		EXPECT(erofs_xxh32(msg, xxh_cases[i].length, NULL, 0,
		    xxh_cases[i].seed) == xxh_cases[i].hash,
		    "length %zu seed %u, all in the first piece",
		    xxh_cases[i].length, (unsigned)xxh_cases[i].seed);
		EXPECT(erofs_xxh32(NULL, 0, msg, xxh_cases[i].length,
		    xxh_cases[i].seed) == xxh_cases[i].hash,
		    "length %zu seed %u, all in the second piece",
		    xxh_cases[i].length, (unsigned)xxh_cases[i].seed);
	}
}

static void
test_xxh32_matches_the_published_string_values(void)
{
	static const struct {
		const char	*text;
		uint32_t	 hash;
	} strings[] = {
		{ "", 0x02cc5d05 },
		{ "a", 0x550d7456 },
		{ "abc", 0x32d153ff },
		{ "0123456789abcdef", 0xc2c45b69 },
		{ "0123456789abcdefghijklmnopqrstuvwxyz", 0x9aa38e7e },
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(strings); i++) {
		const unsigned char *text =
		    (const unsigned char *)strings[i].text;

		EXPECT(erofs_xxh32(text, strlen(strings[i].text), NULL, 0, 0) ==
		    strings[i].hash, "\"%s\"", strings[i].text);
	}
}

static void
test_xxh32_gives_one_value_for_every_split_of_a_message(void)
{
	unsigned char msg[100];
	size_t i, k;

	message_fill(msg, sizeof(msg));
	for (i = 0; i < ARRAY_SIZE(xxh_cases); i++) {
		for (k = 0; k <= xxh_cases[i].length; k++)
			EXPECT(erofs_xxh32(msg, k, msg + k,
			    xxh_cases[i].length - k, xxh_cases[i].seed) ==
			    xxh_cases[i].hash,
			    "length %zu seed %u split at %zu",
			    xxh_cases[i].length, (unsigned)xxh_cases[i].seed,
			    k);
	}
}

/* The filter bit of the name infix then suffix, through the name hash. */
static unsigned
filter_bit_of(uint8_t base_index, const unsigned char *infix,
    size_t infix_length, const unsigned char *suffix, size_t suffix_length)
{
	erofs_name_hash_t hash;

	erofs_name_hash_init(&hash, base_index);
	erofs_name_hash_absorb(&hash, infix, infix_length);
	erofs_name_hash_absorb(&hash, suffix, suffix_length);
	return (erofs_name_hash_bit(&hash));
}

static void
test_filter_bit_hashes_infix_and_suffix_with_the_prefix_seed(void)
{
	unsigned char msg[33];

	message_fill(msg, sizeof(msg));
	/* Seed 0x25BBE08F plus the base index; the bit is the hash mod 32. */
	EXPECT(filter_bit_of(1, NULL, 0, msg, 33) == 26,
	    "base 1, suffix only: 0xdb804b7a");
	EXPECT(filter_bit_of(1, msg, 10, msg + 10, 23) == 26,
	    "base 1, infix and suffix of one message");
	EXPECT(filter_bit_of(4, msg, 4, NULL, 0) == 11,
	    "base 4, infix only: 0x83c6616b");
	EXPECT(filter_bit_of(6, NULL, 0, msg, 33) == 31,
	    "base 6, suffix only: 0x116a82df");
	EXPECT(filter_bit_of(6, msg, 2, msg + 2, 2) == 13,
	    "base 6, split 4-byte message: 0xe969cbed");
	EXPECT(filter_bit_of(0, msg, 4, NULL, 0) == 21,
	    "base 0: 0x2ff521d5");
}

/*
 * The name filter of real inodes. For each, the filter word the producer
 * stored must equal the complement of the OR of one bit per attribute, shared
 * or inline, found by walking the inode's region with the decoders.
 */
typedef struct filter_inode {
	const char	*name;
	uint32_t	 nid;
	uint32_t	 filter;	/* the fixture's, as e6-xattrs.rb */
} filter_inode_t;

static const filter_inode_t default_inodes[] = {
	{ "/common1", 56, 0xfdffdfff },
	{ "/dacl", 74, 0xbfdfffff },
	{ "/fs", 99, 0xeffff7ff },
	{ "/lp4", 115, 0xbeff7fff },
	{ "/many", 118, 0xbc3836f7 },
};

static const filter_inode_t prefix_inodes[] = {
	{ "/lp1", 107, 0xffff7fff },
	{ "/lp3", 111, 0xfeffffff },
	{ "/lp4", 113, 0xbeff7fff },
	{ "/mixed", 124, 0xbfdfdfff },
	{ "/pair1", 128, 0xffbfffff },
};

enum { E6_IMAGE_BYTES = 8192 };

/* One attribute's filter bit, resolving a long prefix in the image's table. */
static unsigned
attribute_filter_bit(const unsigned char *image, const erofs_super_t *sb,
    const unsigned char *entry, const erofs_xattr_t *x)
{
	const unsigned char *suffix = entry + EROFS_XATTR_ENTRY_BYTES;
	const unsigned char *record;
	erofs_prefix_t prefix;
	const char *what = NULL;
	unsigned k;

	if (x->name_index < EROFS_XATTR_LONG_PREFIX)
		return (filter_bit_of(x->name_index, NULL, 0, suffix,
		    x->suffix_length));

	record = image + 4 * (size_t)sb->xattr_prefix_start;
	for (k = 0; k < (unsigned)(x->name_index & 0x7f); k++) {
		CHECK(erofs_prefix_decode(record, 64, &prefix, &what) ==
		    FSIMG_OK);
		record += prefix.record_bytes;
	}
	CHECK(erofs_prefix_decode(record, 64, &prefix, &what) == FSIMG_OK);
	return (filter_bit_of(prefix.base_index, record + 3,
	    prefix.infix_length, suffix, x->suffix_length));
}

static void
filter_check_inode(const char *image_name, const unsigned char *image,
    const erofs_super_t *sb, const filter_inode_t *inode)
{
	erofs_inode_t decoded;
	erofs_xattr_header_t header;
	erofs_xattr_t x;
	const unsigned char *region;
	const char *what = NULL;
	size_t position = 32 * (size_t)inode->nid;
	uint32_t seen = 0;
	unsigned i;

	CHECK(erofs_inode_decode(image + position, E6_IMAGE_BYTES - position,
	    sb, &decoded, &what) == FSIMG_OK);
	region = image + position + decoded.fixed_bytes;
	CHECK(erofs_xattr_header_decode(region, decoded.xattr_bytes, &header,
	    &what) == FSIMG_OK);

	for (i = 0; i < header.shared_count; i++) {
		uint64_t at;
		uint32_t id;

		CHECK(erofs_xattr_shared_id(region, &header, i, &id));
		at = erofs_xattr_shared_offset(sb, id);
		CHECK(at < E6_IMAGE_BYTES);
		CHECK(erofs_xattr_decode(image + at, E6_IMAGE_BYTES - at, sb,
		    &x, &what) == FSIMG_OK);
		seen |= UINT32_C(1) << attribute_filter_bit(image, sb,
		    image + at, &x);
	}
	for (position = header.entries_offset; position < decoded.xattr_bytes;
	    position += x.entry_bytes) {
		CHECK(erofs_xattr_decode(region + position,
		    decoded.xattr_bytes - position, sb, &x, &what) == FSIMG_OK);
		seen |= UINT32_C(1) << attribute_filter_bit(image, sb,
		    region + position, &x);
	}

	EXPECT(header.filter == inode->filter, "%s %s: stored filter 0x%08x",
	    image_name, inode->name, (unsigned)header.filter);
	EXPECT(~seen == inode->filter, "%s %s: complement of the bits is "
	    "0x%08x, fixture has 0x%08x", image_name, inode->name,
	    (unsigned)~seen, (unsigned)inode->filter);
}

/*
 * Decode the superblock of a fixture image. A fixture holds only the bytes
 * its tests read, so the checksum, which covers the rest of the block, is
 * taken out of the decoded region rather than verified.
 */
static fsimg_status_t
fixture_super_decode(const unsigned char *image, erofs_super_t *sb)
{
	unsigned char region[REGION_BYTES];
	erofs_config_t config;
	const char *what;

	bcopy(image + EROFS_SUPER_OFFSET, region, sizeof(region));
	region[SB_COMPAT] &= (unsigned char)~EROFS_COMPAT_SB_CHKSUM;
	return (erofs_region_decode(region, sizeof(region), sb, &config,
	    &what));
}

static void
filter_check_image(const char *image_name, const fixture_t *fixture,
    const filter_inode_t *inodes, size_t count, unsigned prefix_count)
{
	static unsigned char image[E6_IMAGE_BYTES];
	erofs_super_t sb;
	size_t i;

	fixture_load(fixture, image, sizeof(image));
	CHECK(fixture_super_decode(image, &sb) == FSIMG_OK);
	CHECK(sb.xattr_prefix_count == prefix_count);
	CHECK(!sb.filter_disabled);
	for (i = 0; i < count; i++)
		filter_check_inode(image_name, image, &sb, &inodes[i]);
}

static void
test_name_filter_of_real_inodes_is_the_complement_of_the_attribute_bits(void)
{
	static const fixture_t plain = FIXTURE(xattr_default, 0);
	static const fixture_t prefixes = FIXTURE(xattr_prefixes, 0);

	filter_check_image("e6-xattr-default", &plain, default_inodes,
	    ARRAY_SIZE(default_inodes), 0);
	filter_check_image("e6-xattr-pfx-plain", &prefixes, prefix_inodes,
	    ARRAY_SIZE(prefix_inodes), 2);
}

static void
test_real_long_prefix_records_decode(void)
{
	static const fixture_t fixture = FIXTURE(xattr_prefixes, 0);
	static unsigned char image[E6_IMAGE_BYTES];
	erofs_super_t sb;
	erofs_prefix_t first, second;
	const char *what = NULL;
	const unsigned char *record;

	fixture_load(&fixture, image, sizeof(image));
	CHECK(fixture_super_decode(image, &sb) == FSIMG_OK);
	EXPECT(sb.xattr_prefix_count == 2 && sb.xattr_prefix_start == 297,
	    "two prefixes starting at word 297 (byte 1188)");

	record = image + 1188;
	EXPECT(erofs_prefix_decode(record, 28, &first, &what) == FSIMG_OK &&
	    first.base_index == 1 && first.infix_length == 13 &&
	    first.record_bytes == 16 &&
	    bcmp(record + 3, "vendor.alpha.", 13) == 0,
	    "record at 1188: user. + vendor.alpha.");
	record += first.record_bytes;
	EXPECT(erofs_prefix_decode(record, 12, &second, &what) == FSIMG_OK &&
	    second.base_index == 4 && second.infix_length == 7 &&
	    second.record_bytes == 12 && bcmp(record + 3, "vendor.", 7) == 0,
	    "record at 1204: trusted. + vendor.");
	EXPECT(1188 + first.record_bytes + second.record_bytes == 1216,
	    "the table ends where the first inode starts");
}

/* ---- Probe, open and the metadata reader ---- */

/*
 * These tests open images from test/erofs-gen.c through the driver's own
 * open callback, the way the hfsplus tests do, so every refusal is the
 * driver's and not the registry's. A minimal volume has its metadata at
 * byte 4096, past the superblock region at every block size, and two blocks
 * of metadata space from there to the end of its address domain.
 */

enum {
	META_BYTES = 4096,		/* where a minimal volume's metadata starts */
	INODE_ROOT_TAIL = 24		/* the minimal root directory's inline size */
};

/*
 * Feature bits and data layouts as the design's tables number them, kept
 * apart from the driver's constants so a wrong value there cannot make a
 * test agree with it.
 */
enum {
	COMPAT_SB_CHKSUM = 0x01,
	COMPAT_MTIME = 0x02,
	COMPAT_SHARED_EA_IN_METABOX = 0x08,
	COMPAT_PLAIN_XATTR_PFX = 0x10,
	COMPAT_UNNAMED_BIT = 0x40,
	INCOMPAT_CHUNKED_FILE = 0x004,
	INCOMPAT_FRAGMENTS = 0x020,
	INCOMPAT_XATTR_PREFIXES = 0x040,
	INCOMPAT_METABOX = 0x100,
	LAYOUT_COMPRESSED_FULL = 1,
	LAYOUT_FLAT_INLINE = 2,
	LAYOUT_COMPRESSED_COMPACT = 3,
	LAYOUT_CHUNK_BASED = 4
};

/* The blocks of a minimal volume: the blocks before the metadata, then two. */
static uint32_t
minimal_blocks(unsigned blkszbits)
{
	return ((uint32_t)META_BYTES >> blkszbits) + 2;
}

/*
 * Build a volume of length bytes from sb and root, the minimal root when
 * root is NULL; the checksum is stored last when the superblock asks for it.
 */
static void
image_build_sized(gen_image_t *image, const gen_super_t *sb,
    const gen_inode_t *root, size_t length)
{
	gen_inode_t minimal = gen_root_default();

	gen_image_init(image, length);
	gen_super_write(image, sb);
	gen_inode_write(image, root != NULL ? root : &minimal);
	if ((sb->compat & COMPAT_SB_CHKSUM) != 0)
		gen_seal(image);
}

static void
image_build(gen_image_t *image, const gen_super_t *sb, const gen_inode_t *root)
{
	image_build_sized(image, sb, root, (size_t)sb->blocks << sb->blkszbits);
}

static void
minimal_image(gen_image_t *image, unsigned blkszbits)
{
	gen_super_t sb = gen_super_default(blkszbits, minimal_blocks(blkszbits));

	image_build(image, &sb, NULL);
}

/* A reader over an in-memory image, built the way a driver test opens one. */
typedef struct volume {
	fsimg_reader_t	 reader;
	fsimg_ctx_t	*ctx;
	fsimg_io_t	*io;
	const unsigned char *bytes;
	uint64_t	 length;
	uint64_t	 baseline;
} volume_t;

static fsimg_status_t
volume_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	volume_t *v = argument;

	CHECK(offset <= v->length && length <= v->length - offset);
	bcopy(v->bytes + offset, buffer, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
volume_size(void *argument, uint64_t *size, fsimg_error_t *error FSIMG_UNUSED)
{
	*size = ((volume_t *)argument)->length;
	return (FSIMG_OK);
}

static void
volume_start_with(volume_t *v, const unsigned char *bytes, uint64_t length,
    uint64_t memory_bytes)
{
	fsimg_options_t options;
	fsimg_io_ops_t ops;
	fsimg_stats_t stats;

	bzero(v, sizeof(*v));
	v->bytes = bytes;
	v->length = length;
	fsimg_options_init(&options);
	options.memory_bytes = memory_bytes;
	CHECK(fsimg_ctx_new(&options, &v->ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(v->ctx, &stats, NULL) == FSIMG_OK);
	v->baseline = stats.memory_bytes;
	fsimg_io_ops_init(&ops);
	ops.read_at = volume_read;
	ops.get_size = volume_size;
	CHECK(fsimg_io_from_callbacks(v->ctx, &ops, v, &v->io, NULL) ==
	    FSIMG_OK);
	v->reader.ctx = v->ctx;
	v->reader.input = v->io;
	v->reader.ops = &fsimg_erofs_reader_ops;
	v->reader.slice_bytes = length;
	v->reader.state = FSIMG_READER_OPENING;
	fsimg_read_options_init(&v->reader.options);
	fsimg_session_init(&v->reader.session, v->ctx, "test", 0);
}

static void
volume_start(volume_t *v, const unsigned char *bytes, uint64_t length)
{
	volume_start_with(v, bytes, length, 16 * 1024 * 1024);
}

static fsimg_status_t
volume_open(volume_t *v, unsigned flags, fsimg_error_t *error)
{
	v->reader.options.flags |= flags;
	return (fsimg_erofs_reader_ops.open(&v->reader, NULL, error));
}

/* Close twice, then require that every byte the open took came back. */
static void
volume_stop(volume_t *v, const char *name)
{
	fsimg_stats_t stats;

	fsimg_erofs_reader_ops.close(&v->reader);
	fsimg_erofs_reader_ops.close(&v->reader);
	fsimg_io_free(v->io);
	CHECK(fsimg_ctx_get_stats(v->ctx, &stats, NULL) == FSIMG_OK);
	EXPECT(stats.memory_bytes == v->baseline,
	    "%s: close returns memory to the baseline (%llu != %llu)", name,
	    (unsigned long long)stats.memory_bytes,
	    (unsigned long long)v->baseline);
	fsimg_ctx_free(v->ctx);
}

static void
check_status(const char *name, fsimg_status_t got, fsimg_status_t want,
    const fsimg_error_t *error)
{
	expect_status(got, want, error->message, __func__, __LINE__, name);
}

static bool
has_phrase(const fsimg_error_t *error, const char *phrase)
{
	return (phrase == NULL || strstr(error->message, phrase) != NULL);
}

/* Open image of length bytes and close it again; the open's error is kept. */
static fsimg_status_t
open_close(const char *name, const unsigned char *bytes, size_t length,
    unsigned flags, fsimg_error_t *error)
{
	volume_t v;
	fsimg_status_t status;

	bzero(error, sizeof(*error));
	volume_start(&v, bytes, length);
	status = volume_open(&v, flags, error);
	volume_stop(&v, name);
	return (status);
}

/* Require status want and, if given, a phrase in the message. */
static void
check_refusal(const char *name, fsimg_status_t got, fsimg_status_t want,
    const fsimg_error_t *error, const char *phrase)
{
	check_status(name, got, want, error);
	EXPECT(has_phrase(error, phrase), "%s: message \"%s\" names \"%s\"",
	    name, error->message, phrase);
}

/* Open the whole image with flags and require status want and a phrase. */
static void
check_open_flags(const char *name, const gen_image_t *image, unsigned flags,
    fsimg_status_t want, const char *phrase)
{
	fsimg_error_t error;
	fsimg_status_t status;

	status = open_close(name, image->bytes, image->length, flags, &error);
	check_refusal(name, status, want, &error, phrase);
}

static void
check_open(const char *name, const gen_image_t *image, fsimg_status_t want,
    const char *phrase)
{
	check_open_flags(name, image, 0, want, phrase);
}

static const fsimg_volume_detail_t *
find_detail(const fsimg_volume_info_t *info, const char *name)
{
	for (unsigned i = 0; i < info->detail_count; i++) {
		if (strcmp(info->details[i].name, name) == 0)
			return (&info->details[i]);
	}
	return (NULL);
}

static const char *
find_text(const fsimg_volume_info_t *info, const char *name)
{
	for (unsigned i = 0; i < info->text_count; i++) {
		if (strcmp(info->texts[i].name, name) == 0)
			return (info->texts[i].value);
	}
	return (NULL);
}

static bool
detail_is(const fsimg_volume_info_t *info, const char *name, uint64_t value)
{
	const fsimg_volume_detail_t *detail = find_detail(info, name);

	return (detail != NULL && detail->value == value);
}

static bool
text_is(const fsimg_volume_info_t *info, const char *name, const char *value)
{
	const char *text = find_text(info, name);

	return (text != NULL && strcmp(text, value) == 0);
}

/* ---- The probe ---- */

static void
test_generator_crc32c_matches_the_published_check_value(void)
{
	/* The stored form is the register without the final inversion. */
	EXPECT(gen_crc32c_register((const unsigned char *)"123456789", 9) ==
	    UINT32_C(0x1cf96d7c), "stored form of 123456789");
}

static fsimg_status_t
probe_prefix(const unsigned char *prefix, size_t length, const char **name)
{
	const fsimg_format_variant_t *variant = NULL;
	fsimg_error_t error;
	fsimg_status_t status;

	bzero(&error, sizeof(error));
	status = fsimg_erofs_format.probe(prefix, length, length, &variant,
	    &error);
	*name = variant != NULL ? variant->info.name : NULL;
	return (status);
}

static void
check_probe(const char *name, const unsigned char *prefix, size_t length,
    fsimg_status_t want)
{
	const char *variant;
	fsimg_status_t status = probe_prefix(prefix, length, &variant);

	expect_status(status, want, NULL, __func__, __LINE__, name);
}

static void
test_probe_needs_the_magic_inside_the_prefix(void)
{
	gen_image_t image;
	unsigned char other[1152];

	minimal_image(&image, 12);
	check_probe("no prefix", NULL, 0, FSIMG_EFORMAT);
	check_probe("prefix ends before the magic ends", image.bytes, 1027,
	    FSIMG_EFORMAT);
	bcopy(image.bytes, other, sizeof(other));
	other[1024] ^= 0xff;
	check_probe("a different magic", other, sizeof(other), FSIMG_EFORMAT);
	gen_image_free(&image);
}

static void
test_probe_claims_a_full_prefix_and_names_erofs(void)
{
	gen_image_t image;
	const char *variant;

	minimal_image(&image, 12);
	EXPECT(probe_prefix(image.bytes, 1152, &variant) == FSIMG_OK,
	    "1152-byte prefix of a valid image");
	EXPECT(variant != NULL && strcmp(variant, "erofs") == 0,
	    "the variant is named erofs");
	gen_image_free(&image);
}

static void
test_probe_with_a_short_base_superblock_is_corrupt(void)
{
	gen_image_t image;

	minimal_image(&image, 12);
	check_probe("prefix of 1028 bytes", image.bytes, 1028, FSIMG_ECORRUPT);
	check_probe("prefix of 1151 bytes", image.bytes, 1151, FSIMG_ECORRUPT);
	gen_image_free(&image);
}

typedef struct probe_case {
	const char	*name;
	patch_t		 patches[MAX_PATCHES];
	fsimg_status_t	 want;
} probe_case_t;

static void
test_probe_classifies_the_base_superblock(void)
{
	static const probe_case_t cases[] = {
		{ "blkszbits 8", { { SB_BLKSZBITS, 1, 8 } }, FSIMG_ECORRUPT },
		{ "blkszbits 13", { { SB_BLKSZBITS, 1, 13 } },
		    FSIMG_EUNSUPPORTED },
		{ "blkszbits 9", { { SB_BLKSZBITS, 1, 9 } }, FSIMG_OK },
		{ "dirblkbits 1", { { SB_DIRBLKBITS, 1, 1 } },
		    FSIMG_EUNSUPPORTED },
		{ "185 extension slots", { { SB_SLOTS, 1, 185 } },
		    FSIMG_EUNSUPPORTED },
		{ "184 extension slots", { { SB_SLOTS, 1, 184 } }, FSIMG_OK },
		{ "incompat bit 0x200", { { SB_INCOMPAT, 4, 0x200 } },
		    FSIMG_EUNSUPPORTED },
		{ "wrong checksum is not the probe's concern",
		    { { SB_CHECKSUM, 4, 0xdeadbeef } }, FSIMG_OK }
	};
	gen_image_t image;

	minimal_image(&image, 12);
	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		unsigned char prefix[1152];
		const char *variant;

		bcopy(image.bytes, prefix, sizeof(prefix));
		patch_apply(prefix + GEN_SUPER_OFFSET, cases[i].patches);
		expect_status(probe_prefix(prefix, sizeof(prefix), &variant),
		    cases[i].want, NULL, __func__, __LINE__, cases[i].name);
	}
	gen_image_free(&image);
}

/* ---- Open ---- */

static void
test_open_accepts_the_minimal_volume(void)
{
	gen_image_t image;

	minimal_image(&image, 12);
	check_open("minimal volume", &image, FSIMG_OK, NULL);
	gen_image_free(&image);
}

static void
test_open_reports_geometry_uuid_and_the_format_values(void)
{
	gen_super_t sb = gen_super_default(12, 3);
	gen_image_t image;
	volume_t v;
	fsimg_error_t error;
	const fsimg_volume_info_t *info;

	for (unsigned i = 0; i < 16; i++)
		sb.uuid[i] = (uint8_t)(0x10 + i);
	sb.inode_count = 77;
	sb.compat = COMPAT_SB_CHKSUM | COMPAT_MTIME;
	sb.build_time = 12345;
	sb.epoch = 1700000000;
	sb.epoch_nsec = 123456789;
	image_build(&image, &sb, NULL);
	gen_put(&image, GEN_SUPER_OFFSET + SB_ISHARE, 9, 1);
	gen_put(&image, GEN_SUPER_OFFSET + SB_RESERVED_6A, 1, 1);
	gen_seal(&image);

	volume_start(&v, image.bytes, image.length);
	check_status("open", volume_open(&v, 0, &error), FSIMG_OK, &error);
	info = &v.reader.info;
	EXPECT(strcmp(info->format, "erofs") == 0 &&
	    strcmp(info->profile, "core") == 0, "format erofs, profile core");
	EXPECT((info->valid & FSIMG_VOLUME_GEOMETRY) != 0 &&
	    (info->valid & FSIMG_VOLUME_UUID) != 0, "geometry and uuid valid");
	EXPECT(info->volume_bytes == 3 * 4096 &&
	    info->allocation_unit_bytes == 4096 && info->allocation_units == 3,
	    "volume_bytes %llu, unit %llu, units %llu",
	    (unsigned long long)info->volume_bytes,
	    (unsigned long long)info->allocation_unit_bytes,
	    (unsigned long long)info->allocation_units);
	EXPECT(bcmp(info->uuid, sb.uuid, 16) == 0, "uuid as stored");
	EXPECT(info->detail_count > 0 &&
	    strcmp(info->details[0].name, "block_count") == 0,
	    "block_count is the first detail");
	EXPECT(detail_is(info, "block_count", 3), "block_count detail");
	EXPECT(detail_is(info, "inode_count", 77), "inode_count detail");
	EXPECT(detail_is(info, "epoch_nsec", 123456789), "epoch_nsec detail");
	EXPECT(detail_is(info, "compat", 3), "compat detail");
	EXPECT(detail_is(info, "incompat", 0), "incompat detail");
	EXPECT(detail_is(info, "build_time", 12345), "build_time detail");
	EXPECT(detail_is(info, "ishare_prefix", 9), "ishare_prefix detail");
	EXPECT(detail_is(info, "reserved_nonzero", 1),
	    "reserved_nonzero detail");
	EXPECT(text_is(info, "epoch", "1700000000"), "epoch text is \"%s\"",
	    find_text(info, "epoch") != NULL ? find_text(info, "epoch") : "");
	EXPECT(text_is(info, "compat_features", "SB_CHKSUM MTIME"),
	    "compat_features text");
	EXPECT(erofs_state(&v.reader)->features.mtime, "MTIME feature decided");
	volume_stop(&v, "geometry");
	gen_image_free(&image);
}

static void
test_open_reports_a_negative_epoch_as_signed_seconds(void)
{
	gen_super_t sb = gen_super_default(12, 3);
	gen_image_t image;
	volume_t v;
	fsimg_error_t error;

	sb.epoch = -5;
	sb.epoch_nsec = 999999999;
	image_build(&image, &sb, NULL);
	volume_start(&v, image.bytes, image.length);
	check_status("open", volume_open(&v, 0, &error), FSIMG_OK, &error);
	EXPECT(text_is(&v.reader.info, "epoch", "-5"), "epoch text is -5");
	EXPECT(detail_is(&v.reader.info, "epoch_nsec", 999999999),
	    "epoch_nsec detail");
	EXPECT(detail_is(&v.reader.info, "reserved_nonzero", 0),
	    "reserved bytes are zero");
	EXPECT(!erofs_state(&v.reader)->features.mtime, "no MTIME feature");
	volume_stop(&v, "negative epoch");
	gen_image_free(&image);
}

static void
test_open_accepts_every_block_size_with_a_correct_checksum(void)
{
	for (unsigned bits = 9; bits <= 12; bits++) {
		char name[32];
		gen_image_t image;

		snprintf(name, sizeof(name), "block size %u", 1u << bits);
		minimal_image(&image, bits);
		check_open(name, &image, FSIMG_OK, NULL);
		gen_image_free(&image);
	}
}

static void
test_open_refuses_a_flipped_bit_inside_the_checksum_range(void)
{
	for (unsigned bits = 9; bits <= 12; bits++) {
		size_t last = GEN_SUPER_OFFSET + covered_bytes(bits) - 1;
		const size_t flips[] = { GEN_SUPER_OFFSET + SB_UUID, last };
		char name[48];
		gen_image_t image;

		for (size_t i = 0; i < ARRAY_SIZE(flips); i++) {
			minimal_image(&image, bits);
			image.bytes[flips[i]] ^= 0x10;
			snprintf(name, sizeof(name), "B=%u flip at %zu",
			    1u << bits, flips[i]);
			check_open(name, &image, FSIMG_ECORRUPT, "checksum");
			gen_image_free(&image);
		}
	}
}

static void
test_open_accepts_a_flipped_bit_just_past_the_checksum_range(void)
{
	for (unsigned bits = 9; bits <= 12; bits++) {
		/* At 4096 the range ends where the root inode starts; its
		 * serial number is a free field. */
		size_t past = bits == 12 ? META_BYTES + 0x14 :
		    GEN_SUPER_OFFSET + covered_bytes(bits);
		char name[48];
		gen_image_t image;

		minimal_image(&image, bits);
		image.bytes[past] ^= 0x10;
		snprintf(name, sizeof(name), "B=%u flip at %zu", 1u << bits,
		    past);
		check_open(name, &image, FSIMG_OK, NULL);
		gen_image_free(&image);
	}
}

static void
test_open_does_not_check_the_checksum_without_sb_chksum(void)
{
	gen_super_t sb = gen_super_default(12, 3);
	gen_image_t image;

	sb.compat = 0;
	sb.checksum = 0xdeadbeef;
	image_build(&image, &sb, NULL);
	check_open("garbage checksum, no SB_CHKSUM", &image, FSIMG_OK, NULL);
	gen_image_free(&image);
}

/* Fields patched into a minimal 4096-byte-block volume before open. */
typedef struct open_case {
	const char	*name;
	patch_t		 patches[MAX_PATCHES];
	bool		 reseal;	/* store the checksum after patching */
	fsimg_status_t	 want;
	const char	*phrase;	/* the message names it, or NULL */
} open_case_t;

static void
run_open_cases(const open_case_t *cases, size_t count, unsigned flags)
{
	for (size_t i = 0; i < count; i++) {
		gen_image_t image;

		minimal_image(&image, 12);
		patch_apply(image.bytes + GEN_SUPER_OFFSET, cases[i].patches);
		if (cases[i].reseal)
			gen_seal(&image);
		check_open_flags(cases[i].name, &image, flags,
		    cases[i].want, cases[i].phrase);
		gen_image_free(&image);
	}
}

static void
test_open_refuses_a_checksum_range_cut_short_by_the_input(void)
{
	gen_image_t image;
	fsimg_error_t error;
	fsimg_status_t status;

	minimal_image(&image, 12);
	put_le(image.bytes + GEN_SUPER_OFFSET + SB_COMPAT, COMPAT_SB_CHKSUM,
	    4);
	/*
	 * The 2000-byte input holds the magic and the base superblock, so
	 * the identity check and the base rules pass, and no extension slot
	 * or extra device is declared. The checksum covers 3072 bytes from
	 * byte 1024 but only 976 are there, and the checksum rule is the
	 * first to look at them.
	 */
	status = open_close("checksum range past the input", image.bytes,
	    2000, 0, &error);
	check_refusal("checksum range past the input", status, FSIMG_ECORRUPT,
	    &error, "checksum range truncated");
	gen_image_free(&image);
}

static void
test_open_refuses_an_unknown_incompatible_bit(void)
{
	static const open_case_t cases[] = {
		{ "bit 9", { { SB_INCOMPAT, 4, 0x200 } }, true,
		    FSIMG_EUNSUPPORTED, "incompatible feature" }
	};

	run_open_cases(cases, ARRAY_SIZE(cases), 0);
}

static void
test_open_refuses_nonzero_extra_devices_before_the_checksum(void)
{
	static const open_case_t cases[] = {
		{ "extra devices, checksum correct",
		    { { SB_DEVICES, 2, 1 } }, true, FSIMG_EUNSUPPORTED,
		    "device" },
		{ "extra devices, checksum wrong, CHUNKED_FILE set",
		    { { SB_DEVICES, 2, 1 },
		    { SB_INCOMPAT, 4, INCOMPAT_CHUNKED_FILE } }, false,
		    FSIMG_EUNSUPPORTED, "device" }
	};

	run_open_cases(cases, ARRAY_SIZE(cases), 0);
}

static void
test_open_refuses_inconsistent_superblock_fields(void)
{
	static const open_case_t cases[] = {
		{ "epoch nanoseconds 1000000000",
		    { { SB_NSEC, 4, 1000000000 } }, true, FSIMG_ECORRUPT,
		    NULL },
		{ "SHARED_EA_IN_METABOX without METABOX",
		    { { SB_COMPAT, 4, COMPAT_SB_CHKSUM |
		    COMPAT_SHARED_EA_IN_METABOX } }, true,
		    FSIMG_ECORRUPT, NULL },
		{ "METABOX with no extension slot",
		    { { SB_INCOMPAT, 4, INCOMPAT_METABOX } }, true,
		    FSIMG_ECORRUPT, NULL },
		{ "METABOX with one extension slot",
		    { { SB_INCOMPAT, 4, INCOMPAT_METABOX },
		    { SB_SLOTS, 1, 1 } }, true, FSIMG_EUNSUPPORTED,
		    "METABOX" }
	};

	run_open_cases(cases, ARRAY_SIZE(cases), 0);
}

static void
test_open_refuses_each_unimplemented_incompatible_bit_by_name(void)
{
	static const open_case_t cases[] = {
		{ "LZ4_0PADDING", { { SB_INCOMPAT, 4, 0x001 } }, true,
		    FSIMG_EUNSUPPORTED, "LZ4_0PADDING" },
		{ "COMPR_CFGS", { { SB_INCOMPAT, 4, 0x002 },
		    { SB_WORD54, 2, 0 } }, true, FSIMG_EUNSUPPORTED,
		    "COMPR_CFGS" },
		{ "DEVICE_TABLE", { { SB_INCOMPAT, 4, 0x008 },
		    { SB_DEVICES, 2, 0 } }, true, FSIMG_EUNSUPPORTED,
		    "DEVICE_TABLE" },
		{ "ZTAILPACKING", { { SB_INCOMPAT, 4, 0x010 } }, true,
		    FSIMG_EUNSUPPORTED, "ZTAILPACKING" },
		{ "FRAGMENTS", { { SB_INCOMPAT, 4, 0x020 } }, true,
		    FSIMG_EUNSUPPORTED, "FRAGMENTS" },
		{ "48BIT", { { SB_INCOMPAT, 4, 0x080 } }, true,
		    FSIMG_EUNSUPPORTED, "48BIT" },
		{ "METABOX", { { SB_INCOMPAT, 4, 0x100 },
		    { SB_SLOTS, 1, 1 } }, true, FSIMG_EUNSUPPORTED,
		    "METABOX" }
	};

	run_open_cases(cases, ARRAY_SIZE(cases), 0);
}

static void
test_open_pairs_long_prefixes_with_a_plain_table_or_a_packed_inode(void)
{
	static const open_case_t cases[] = {
		{ "XATTR_PREFIXES with PLAIN_XATTR_PFX",
		    { { SB_INCOMPAT, 4, INCOMPAT_XATTR_PREFIXES },
		    { SB_COMPAT, 4, COMPAT_SB_CHKSUM |
		    COMPAT_PLAIN_XATTR_PFX } }, true, FSIMG_OK, NULL },
		{ "XATTR_PREFIXES alone",
		    { { SB_INCOMPAT, 4, INCOMPAT_XATTR_PREFIXES } },
		    true, FSIMG_ECORRUPT, NULL },
		{ "XATTR_PREFIXES with FRAGMENTS and no plain table",
		    { { SB_INCOMPAT, 4, INCOMPAT_XATTR_PREFIXES |
		    INCOMPAT_FRAGMENTS } }, true, FSIMG_EUNSUPPORTED,
		    "FRAGMENTS" }
	};

	run_open_cases(cases, ARRAY_SIZE(cases), 0);
}

static void
test_open_tolerates_unknown_compatible_bits_and_reserved_bytes(void)
{
	gen_image_t image;
	volume_t v;
	fsimg_error_t error;

	minimal_image(&image, 12);
	gen_super_put(&image, SB_COMPAT, COMPAT_SB_CHKSUM | COMPAT_UNNAMED_BIT,
	    4);
	gen_seal(&image);
	volume_start(&v, image.bytes, image.length);
	check_status("unknown compatible bit", volume_open(&v, 0, &error),
	    FSIMG_OK, &error);
	EXPECT(erofs_state(&v.reader)->features.unknown_compat,
	    "unknown_compat is set");
	EXPECT(text_is(&v.reader.info, "compat_features", "SB_CHKSUM 0x40"),
	    "compat_features shows \"0x40\"");
	volume_stop(&v, "unknown compatible bit");

	gen_super_put(&image, SB_COMPAT, COMPAT_SB_CHKSUM, 4);
	gen_super_put(&image, SB_RESERVED_6A, 1, 1);
	gen_seal(&image);
	volume_start(&v, image.bytes, image.length);
	check_status("reserved byte", volume_open(&v, 0, &error), FSIMG_OK,
	    &error);
	EXPECT(detail_is(&v.reader.info, "reserved_nonzero", 1),
	    "reserved_nonzero is 1");
	volume_stop(&v, "reserved byte");
	gen_image_free(&image);
}

static void
test_open_refuses_a_wrong_magic_unless_identity_is_assumed(void)
{
	gen_image_t image;
	fsimg_error_t error;
	volume_t v;

	minimal_image(&image, 12);
	gen_super_put(&image, SB_MAGIC, 0x12345678, 4);
	gen_seal(&image);
	check_open("wrong magic", &image, FSIMG_ECORRUPT, "magic");

	volume_start(&v, image.bytes, image.length);
	check_status("wrong magic, assumed",
	    volume_open(&v, FSIMG_READ_ASSUME_IDENTITY, &error), FSIMG_OK,
	    &error);
	EXPECT(v.reader.assumed_identity != NULL, "the waived check is recorded");
	volume_stop(&v, "wrong magic, assumed");
	gen_image_free(&image);
}

/* A forced open waives the magic and nothing else. */
static void
test_forced_open_still_applies_every_other_superblock_rule(void)
{
	/*
	 * The blkszbits case clears SB_CHKSUM, so a missing block-size rule
	 * cannot hide behind a checksum refusal.
	 */
	static const open_case_t cases[] = {
		{ "blkszbits 8", { { SB_MAGIC, 4, 0x12345678 },
		    { SB_COMPAT, 4, 0 },
		    { SB_BLKSZBITS, 1, 8 } }, false, FSIMG_ECORRUPT,
		    "block size" },
		{ "dirblkbits 1", { { SB_MAGIC, 4, 0x12345678 },
		    { SB_DIRBLKBITS, 1, 1 } }, true, FSIMG_EUNSUPPORTED,
		    "directory block size" },
		{ "extra devices", { { SB_MAGIC, 4, 0x12345678 },
		    { SB_DEVICES, 2, 1 } }, true, FSIMG_EUNSUPPORTED,
		    "device" },
		{ "bad checksum", { { SB_MAGIC, 4, 0x12345678 },
		    { SB_CHECKSUM, 4, 0xdeadbeef } }, false, FSIMG_ECORRUPT,
		    "checksum" }
	};

	run_open_cases(cases, ARRAY_SIZE(cases), FSIMG_READ_ASSUME_IDENTITY);
}

static void
test_open_reports_an_allocation_failure_and_keeps_no_memory(void)
{
	gen_image_t image;
	volume_t v;
	fsimg_error_t error;

	minimal_image(&image, 12);
	volume_start(&v, image.bytes, image.length);
	bzero(&error, sizeof(error));
	/* The driver's state is the first allocation open makes. */
	fsimg_test_fail_alloc_after(v.ctx, 1);
	check_refusal("state allocation fails", volume_open(&v, 0, &error),
	    FSIMG_ENOMEM, &error, "allocation");
	fsimg_test_fail_alloc_after(v.ctx, 0);
	EXPECT(v.reader.driver == NULL, "no driver state is left behind");
	volume_stop(&v, "allocation failure");
	gen_image_free(&image);
}

static void
test_open_refuses_a_volume_shorter_than_the_magic_either_way(void)
{
	static const size_t lengths[] = { 1000, GEN_SUPER_OFFSET };
	unsigned char bytes[GEN_SUPER_OFFSET] = { 0 };

	for (size_t i = 0; i < ARRAY_SIZE(lengths); i++) {
		fsimg_error_t error;
		fsimg_status_t status;
		char name[48];

		/* The identity check finds no magic in an empty region. */
		snprintf(name, sizeof(name), "plain open, %zu bytes",
		    lengths[i]);
		status = open_close(name, bytes, lengths[i], 0, &error);
		check_refusal(name, status, FSIMG_ECORRUPT, &error,
		    "no EROFS superblock magic");
		/* Waiving it leaves the decoder, which needs the superblock. */
		snprintf(name, sizeof(name), "assumed identity, %zu bytes",
		    lengths[i]);
		status = open_close(name, bytes, lengths[i],
		    FSIMG_READ_ASSUME_IDENTITY, &error);
		check_refusal(name, status, FSIMG_ECORRUPT, &error,
		    "superblock truncated");
	}
}

static void
test_open_refuses_extension_slots_past_a_short_volume(void)
{
	gen_super_t sb = gen_super_default(9, 4);
	gen_image_t image;

	sb.meta_blkaddr = 3;
	sb.slots = 100;
	image_build(&image, &sb, NULL);
	check_open("100 slots in 2048 bytes", &image, FSIMG_ECORRUPT, NULL);
	gen_image_free(&image);
}

static void
test_open_reports_a_truncated_volume_at_its_declared_length(void)
{
	gen_super_t sb = gen_super_default(12, 6);
	gen_image_t image;
	volume_t v;
	fsimg_error_t error;

	/* Build the whole volume, then hand open only its first two blocks. */
	image_build(&image, &sb, NULL);
	image.length = 2 * 4096;
	volume_start(&v, image.bytes, image.length);
	check_status("root inside the input", volume_open(&v, 0, &error),
	    FSIMG_OK, &error);
	EXPECT(v.reader.info.volume_bytes == 6 * 4096,
	    "volume_bytes %llu is blocks * B, not the input",
	    (unsigned long long)v.reader.info.volume_bytes);
	volume_stop(&v, "truncated, root present");
	gen_image_free(&image);

	/* The root lies inside blocks * B but past the input's end. */
	sb.meta_blkaddr = 3;
	image_build(&image, &sb, NULL);
	image.length = 2 * 4096;
	check_open("root past the input", &image, FSIMG_ECORRUPT, NULL);
	gen_image_free(&image);
}

static void
test_open_takes_a_zero_block_count_as_the_input_length(void)
{
	gen_super_t sb = gen_super_default(12, 0);
	gen_image_t image;
	volume_t v;
	fsimg_error_t error;

	image_build_sized(&image, &sb, NULL, 3 * 4096);
	volume_start(&v, image.bytes, image.length);
	check_status("open", volume_open(&v, 0, &error), FSIMG_OK, &error);
	EXPECT(v.reader.info.volume_bytes == 3 * 4096,
	    "volume_bytes %llu equals the input",
	    (unsigned long long)v.reader.info.volume_bytes);
	EXPECT(detail_is(&v.reader.info, "block_count", 0),
	    "block_count detail is the stored zero");
	volume_stop(&v, "zero block count");
	gen_image_free(&image);
}

static void
test_open_refuses_a_block_count_ending_inside_the_superblock(void)
{
	gen_super_t sb = gen_super_default(9, 2);
	gen_image_t image;

	/*
	 * The root inode at volume byte 0 lies inside the 1024-byte domain,
	 * so only the superblock itself is outside it.
	 */
	sb.meta_blkaddr = 0;
	image_build_sized(&image, &sb, NULL, 10 * 512);
	check_open("512-byte blocks, 2 blocks", &image, FSIMG_ECORRUPT,
	    "superblock");
	gen_image_free(&image);

	/* Three blocks end at byte 1536, past the 128-byte base superblock. */
	sb.blocks = 3;
	image_build_sized(&image, &sb, NULL, 10 * 512);
	check_open("512-byte blocks, 3 blocks", &image, FSIMG_OK, NULL);
	gen_image_free(&image);
}

/*
 * The input holds a valid root at the byte meta_blkaddr names, one block past
 * the block count, so only the address domain refuses it.
 */
static void
test_open_refuses_a_root_outside_the_address_domain(void)
{
	gen_super_t sb = gen_super_default(12, 3);
	gen_image_t image;

	sb.meta_blkaddr = 3;
	image_build_sized(&image, &sb, NULL, 4 * 4096);
	check_open("meta_blkaddr past the block count", &image, FSIMG_ECORRUPT,
	    "metadata outside the address domain");
	gen_image_free(&image);
}

static void
test_open_refuses_a_root_that_is_a_regular_file(void)
{
	gen_super_t sb = gen_super_default(12, 3);
	gen_inode_t root = gen_root_default();
	gen_image_t image;

	root.mode = MODE_REGULAR | 0644;
	image_build(&image, &sb, &root);
	check_open("root is a regular file", &image, FSIMG_ECORRUPT,
	    "directory");
	gen_image_free(&image);
}

static void
test_open_refuses_a_root_with_a_reserved_format_bit(void)
{
	gen_super_t sb = gen_super_default(12, 3);
	gen_inode_t root = gen_root_default();
	gen_image_t image;

	root.format_extra = 0x20;	/* format bit 5 is reserved */
	image_build(&image, &sb, &root);
	check_open("root with a reserved format bit", &image, FSIMG_ECORRUPT,
	    "inode format");
	gen_image_free(&image);
}

typedef struct label_case {
	const char	*name;
	unsigned char	 bytes[sizeof(((gen_super_t *)NULL)->name) + 1];
	size_t		 length;
	const char	*label;		/* NULL: no label */
	const char	*escaped;	/* the "name" text, or NULL: none */
} label_case_t;

static void
test_open_delivers_the_name_as_a_label_only_when_it_is_printable(void)
{
	static const label_case_t cases[] = {
		{ "short name", "vol", 3, "vol", NULL },
		{ "16 bytes without a NUL", "ABCDEFGHIJKLMNOP", 16,
		    "ABCDEFGHIJKLMNOP", NULL },
		{ "control byte", { 'a', 0x01, 'b' }, 3, NULL, "a\\x01b" },
		{ "backslash", { 'a', '\\', 'b' }, 3, "a\\b", NULL },
		{ "DEL byte", { 'a', 0x7f, 'b' }, 3, NULL, "a\\x7fb" },
		{ "empty name", { 0 }, 0, NULL, NULL },
		{ "invalid UTF-8", { 0xff }, 1, NULL, "\\xff" },
		{ "16-byte name truncated inside a UTF-8 sequence",
		    "ABCDEFGHIJKLMNO\xc2", 16, NULL, "ABCDEFGHIJKLMNO\\xc2" },
		{ "C1 control character", { 'a', 0xc2, 0x85, 'b' }, 4, NULL,
		    "a\\xc2\\x85b" },
		{ "printable two-byte character", { 0xc3, 0xa9 }, 2,
		    "\xc3\xa9", NULL }
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		const label_case_t *c = &cases[i];
		gen_super_t sb = gen_super_default(12, 3);
		gen_image_t image;
		volume_t v;
		fsimg_error_t error;
		const fsimg_volume_info_t *info;

		bcopy(c->bytes, sb.name, c->length);
		image_build(&image, &sb, NULL);
		volume_start(&v, image.bytes, image.length);
		check_status(c->name, volume_open(&v, 0, &error), FSIMG_OK,
		    &error);
		info = &v.reader.info;
		EXPECT(((info->valid & FSIMG_VOLUME_LABEL) != 0) ==
		    (c->label != NULL), "%s: label flag", c->name);
		EXPECT(c->label == NULL || strcmp(info->label, c->label) == 0,
		    "%s: label is \"%s\"", c->name, info->label);
		EXPECT(c->escaped == NULL ?
		    find_text(info, "name") == NULL :
		    text_is(info, "name", c->escaped),
		    "%s: name text is \"%s\"", c->name,
		    find_text(info, "name") != NULL ?
		    find_text(info, "name") : "(none)");
		volume_stop(&v, c->name);
		gen_image_free(&image);
	}
}

static void
test_open_failures_return_all_memory(void)
{
	static const struct {
		const char	*name;
		unsigned	 offset;
		uint64_t	 value;
		unsigned	 width;
	} cases[] = {
		{ "feature refusal", SB_INCOMPAT, 0x001, 4 },
		{ "superblock rule", SB_NSEC, 1000000000, 4 },
		{ "root outside the domain", SB_META_BLOCK, 3, 4 }
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		gen_image_t image;
		fsimg_error_t error;

		minimal_image(&image, 12);
		gen_super_put(&image, cases[i].offset, cases[i].value,
		    cases[i].width);
		gen_seal(&image);
		/* open_close() requires the baseline after the failed open. */
		EXPECT(open_close(cases[i].name, image.bytes, image.length, 0,
		    &error) != FSIMG_OK, "%s fails", cases[i].name);
		gen_image_free(&image);
	}
}

/* ---- The metadata reader ---- */

/* Space for extended inodes at the end of the domain: a page past it. */
enum { DOMAIN_BYTES = 3 * 4096, TRAILING_BYTES = 4096 };

/* A minimal 4096-byte-block volume of 3 blocks and room past its end. */
static void
meta_image(gen_image_t *image)
{
	gen_super_t sb = gen_super_default(12, 3);

	image_build_sized(image, &sb, NULL, DOMAIN_BYTES + TRAILING_BYTES);
}

static void
meta_open(volume_t *v, const gen_image_t *image)
{
	fsimg_error_t error;

	volume_start(v, image->bytes, image->length);
	CHECK(volume_open(v, 0, &error) == FSIMG_OK);
}

static fsimg_status_t
load(volume_t *v, uint64_t nid, erofs_inode_t *inode, fsimg_error_t *error)
{
	return (erofs_inode_load(&v->reader, nid, inode, error));
}

/* The last slot of the 3-block domain, and the slot after it. */
enum {
	LAST_NID = (DOMAIN_BYTES - META_BYTES) / 32 - 1,
	PAST_DOMAIN_NID = LAST_NID + 1
};

static void
test_inode_load_refuses_unaddressable_nids(void)
{
	/*
	 * The phrase pins each case to its own check: with one check
	 * removed, the case either loads or fails with a different
	 * phrase. The last NID lies inside the input, one slot past the
	 * domain, where the test writes a valid inode.
	 */
	static const struct {
		const char	*name;
		uint64_t	 nid;
		const char	*phrase;
	} cases[] = {
		{ "bit 63 set", UINT64_C(1) << 63, "metadata box" },
		{ "slot product overflows 64 bits", UINT64_C(1) << 62, "NID" },
		{ "slot product fits, adding the metadata base overflows",
		    (UINT64_C(1) << 59) - 1, "NID" },
		{ "past the domain, inside the input", PAST_DOMAIN_NID,
		    "metadata outside the address domain" }
	};
	gen_inode_t beyond = { .nid = PAST_DOMAIN_NID,
	    .mode = MODE_REGULAR | 0644, .nlink = 1 };
	gen_image_t image;
	volume_t v;

	meta_image(&image);
	gen_inode_write(&image, &beyond);
	meta_open(&v, &image);
	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		erofs_inode_t inode;
		fsimg_error_t error;

		check_refusal(cases[i].name, load(&v, cases[i].nid, &inode,
		    &error), FSIMG_ECORRUPT, &error, cases[i].phrase);
	}
	volume_stop(&v, "unaddressable nids");
	gen_image_free(&image);
}

/*
 * The last slot leaves 32 bytes of the domain: a compact inode loads, and an
 * extended one, which needs 64, is refused.
 */
static void
test_inode_load_in_the_last_slot_is_compact_only(void)
{
	gen_inode_t inode = { .nid = LAST_NID, .mode = MODE_REGULAR | 0644,
	    .nlink = 1 };
	gen_image_t image;
	erofs_inode_t loaded;
	volume_t v;
	fsimg_error_t error;

	meta_image(&image);
	gen_inode_write(&image, &inode);
	meta_open(&v, &image);
	check_status("compact inode in the last 32 bytes",
	    load(&v, LAST_NID, &loaded, &error), FSIMG_OK, &error);

	inode.extended = 1;
	gen_inode_write(&image, &inode);
	check_status("extended inode needing 64 bytes",
	    load(&v, LAST_NID, &loaded, &error), FSIMG_ECORRUPT, &error);
	volume_stop(&v, "end of the domain");
	gen_image_free(&image);
}

static void
test_inode_load_allows_a_fixed_part_across_a_block_boundary(void)
{
	/* 64 bytes starting 32 bytes before the end of block 1. */
	enum { CROSSING_NID = (2 * 4096 - 32 - META_BYTES) / 32 };
	gen_inode_t inode = { .nid = CROSSING_NID, .extended = 1,
	    .mode = MODE_REGULAR | 0644, .nlink = 1 };
	gen_image_t image;
	erofs_inode_t loaded;
	volume_t v;
	fsimg_error_t error;

	meta_image(&image);
	gen_inode_write(&image, &inode);
	meta_open(&v, &image);
	check_status("extended inode across a block boundary",
	    load(&v, CROSSING_NID, &loaded, &error), FSIMG_OK, &error);
	EXPECT(loaded.extended && loaded.fixed_bytes == 64,
	    "the extended form was decoded");
	volume_stop(&v, "crossing fixed part");
	gen_image_free(&image);
}

static void
test_inode_load_reports_where_the_inode_is_and_how_long(void)
{
	gen_inode_t inode = { .nid = 1, .mode = MODE_REGULAR | 0644,
	    .nlink = 1, .layout = LAYOUT_FLAT_INLINE, .size = 100,
	    .xattr_count = 2 };
	gen_image_t image;
	erofs_inode_t loaded;
	volume_t v;
	fsimg_error_t error;

	meta_image(&image);
	gen_inode_write(&image, &inode);
	meta_open(&v, &image);
	check_status("flat-inline inode", load(&v, 1, &loaded, &error),
	    FSIMG_OK, &error);
	EXPECT(loaded.nid == 1 && loaded.space == EROFS_SPACE_PRIMARY &&
	    loaded.offset == META_BYTES + 32, "nid, space and offset");
	EXPECT(loaded.fixed_bytes == 32 && loaded.xattr_bytes == 16 &&
	    loaded.layout_offset == 48, "fixed %u, attributes %u, layout at %llu",
	    loaded.fixed_bytes, loaded.xattr_bytes,
	    (unsigned long long)loaded.layout_offset);
	EXPECT(loaded.body_bytes == 32 + 12 + 4 + 100, "body_bytes %llu",
	    (unsigned long long)loaded.body_bytes);
	volume_stop(&v, "loaded fields");
	gen_image_free(&image);
}

/*
 * A flat-inline inode at NID 100 whose tail starts at byte 4096 + 3200 + 32
 * of the volume and runs for size bytes; block 1 ends at byte 8192, so a
 * size of 864 ends exactly there.
 */
static void
test_inode_load_accepts_a_tail_ending_at_the_block_end_only(void)
{
	gen_inode_t inode = { .nid = 100, .mode = MODE_REGULAR | 0644,
	    .nlink = 1, .layout = LAYOUT_FLAT_INLINE, .size = 864 };
	gen_image_t image;
	erofs_inode_t loaded;
	volume_t v;
	fsimg_error_t error;

	meta_image(&image);
	gen_inode_write(&image, &inode);
	meta_open(&v, &image);
	check_status("tail ends at the block end",
	    load(&v, 100, &loaded, &error), FSIMG_OK, &error);
	inode.size = 865;
	gen_inode_write(&image, &inode);
	check_status("tail one byte past the block end",
	    load(&v, 100, &loaded, &error), FSIMG_ECORRUPT, &error);
	volume_stop(&v, "tail at the block end");
	gen_image_free(&image);
}

/*
 * Inodes whose header (the fixed part and its attribute region) ends at byte
 * 8192, the first byte of block 2, so the inline tail lies wholly in block 2.
 */
static void
test_inode_load_accepts_a_tail_starting_at_the_next_block(void)
{
	static const struct {
		const char	*name;
		uint64_t	 nid;
		bool		 extended;
		uint16_t	 xattr_count;
		uint64_t	 header_bytes;
	} cases[] = {
		{ "compact, no attributes", 127, false, 0, 32 },
		{ "extended, no attributes", 126, true, 0, 64 },
		{ "compact, 32-byte attribute region", 126, false, 6, 64 }
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		gen_inode_t inode = { .nid = cases[i].nid,
		    .extended = cases[i].extended, .mode = MODE_REGULAR | 0644,
		    .nlink = 1, .layout = LAYOUT_FLAT_INLINE, .size = 100,
		    .xattr_count = cases[i].xattr_count };
		gen_image_t image;
		erofs_inode_t loaded = { 0 };
		volume_t v;
		fsimg_error_t error;
		fsimg_status_t status;

		meta_image(&image);
		gen_inode_write(&image, &inode);
		CHECK(gen_inode_offset(&image, inode.nid) +
		    gen_inode_header_bytes(&inode) == 2 * 4096);
		meta_open(&v, &image);
		status = load(&v, inode.nid, &loaded, &error);
		check_status(cases[i].name, status, FSIMG_OK, &error);
		/* A failed load leaves nothing to compare. */
		EXPECT(status != FSIMG_OK ||
		    (loaded.layout_offset == cases[i].header_bytes &&
		    loaded.body_bytes == cases[i].header_bytes + 100),
		    "%s: layout at %llu, body %llu bytes", cases[i].name,
		    (unsigned long long)loaded.layout_offset,
		    (unsigned long long)loaded.body_bytes);
		volume_stop(&v, cases[i].name);
		gen_image_free(&image);
	}
}

static void
test_inode_load_refuses_a_body_past_the_domain(void)
{
	gen_inode_t inode = { .nid = LAST_NID, .mode = MODE_REGULAR | 0644,
	    .nlink = 1, .layout = LAYOUT_FLAT_INLINE, .size = 10 };
	gen_image_t image;
	erofs_inode_t loaded;
	volume_t v;
	fsimg_error_t error;

	meta_image(&image);
	gen_inode_write(&image, &inode);
	meta_open(&v, &image);
	check_status("10-byte tail after the last slot",
	    load(&v, LAST_NID, &loaded, &error), FSIMG_ECORRUPT, &error);
	volume_stop(&v, "body past the domain");
	gen_image_free(&image);
}

static void
test_body_read_returns_the_tail_and_bounds_its_range(void)
{
	gen_inode_t inode = { .nid = 2, .mode = MODE_REGULAR | 0644,
	    .nlink = 1, .layout = LAYOUT_FLAT_INLINE, .size = 20 };
	const unsigned char tail[] = "0123456789abcdefghij";
	const size_t tail_bytes = sizeof(tail) - 1;
	unsigned char out[32];
	gen_image_t image;
	erofs_inode_t loaded;
	volume_t v;
	fsimg_error_t error;

	meta_image(&image);
	gen_inode_write(&image, &inode);
	gen_put_bytes(&image, gen_inode_offset(&image, 2) +
	    gen_inode_header_bytes(&inode), tail, tail_bytes);
	meta_open(&v, &image);
	CHECK(load(&v, 2, &loaded, &error) == FSIMG_OK);

	bzero(out, sizeof(out));
	check_status("read the tail", erofs_body_read(&v.reader, &loaded,
	    loaded.layout_offset, tail_bytes, out, &error), FSIMG_OK,
	    &error);
	EXPECT(bcmp(out, tail, tail_bytes) == 0, "the tail bytes");
	check_refusal("range past body_bytes", erofs_body_read(&v.reader,
	    &loaded, 0, loaded.body_bytes + 1, out, &error), FSIMG_ECORRUPT,
	    &error, "outside the inode's body");
	check_refusal("offset past body_bytes", erofs_body_read(&v.reader,
	    &loaded, loaded.body_bytes + 1, 0, out, &error), FSIMG_ECORRUPT,
	    &error, "outside the inode's body");
	check_status("empty read at body_bytes", erofs_body_read(&v.reader,
	    &loaded, loaded.body_bytes, 0, out, &error), FSIMG_OK, &error);
	volume_stop(&v, "body read");
	gen_image_free(&image);
}

static void
test_meta_read_bounds_the_space_and_the_input(void)
{
	gen_super_t sb = gen_super_default(12, 6);
	gen_image_t image;
	unsigned char out[32];
	volume_t v;
	fsimg_error_t error;

	/* Six blocks declared, and two more blocks of input past them. */
	image_build_sized(&image, &sb, NULL, 8 * 4096);
	meta_open(&v, &image);
	check_refusal("metadata box space", erofs_meta_read(&v.reader,
	    EROFS_SPACE_BOX, 0, 8, out, &error), FSIMG_EUNSUPPORTED, &error,
	    "metadata box");
	check_refusal("range past the domain, inside the input",
	    erofs_meta_read(&v.reader, EROFS_SPACE_PRIMARY, 6 * 4096 - 8, 16,
	    out, &error), FSIMG_ECORRUPT, &error,
	    "metadata outside the address domain");
	check_status("range ending at the domain's end", erofs_meta_read(
	    &v.reader, EROFS_SPACE_PRIMARY, 6 * 4096 - 16, 16, out, &error),
	    FSIMG_OK, &error);
	volume_stop(&v, "meta read, long input");
	gen_image_free(&image);

	/* Six blocks declared, three present. */
	image_build(&image, &sb, NULL);
	image.length = 3 * 4096;
	meta_open(&v, &image);
	check_status("range inside the domain, past the input",
	    erofs_meta_read(&v.reader, EROFS_SPACE_PRIMARY, 4 * 4096, 16, out,
	    &error), FSIMG_ECORRUPT, &error);
	EXPECT(strstr(error.message, "address domain") == NULL,
	    "bounded I/O refuses it, not the domain: \"%s\"", error.message);
	check_status("range inside the input", erofs_meta_read(&v.reader,
	    EROFS_SPACE_PRIMARY, 2 * 4096, 16, out, &error), FSIMG_OK, &error);
	volume_stop(&v, "meta read, short input");
	gen_image_free(&image);
}

static void
test_inode_load_checks_the_layout_against_the_volume_features(void)
{
	static const struct {
		const char	*name;
		uint8_t		 layout;
		fsimg_status_t	 want;
	} cases[] = {
		{ "compressed full layout", LAYOUT_COMPRESSED_FULL,
		    FSIMG_EUNSUPPORTED },
		{ "compressed compact layout", LAYOUT_COMPRESSED_COMPACT,
		    FSIMG_EUNSUPPORTED },
		{ "chunk-based layout without CHUNKED_FILE",
		    LAYOUT_CHUNK_BASED, FSIMG_ECORRUPT }
	};
	gen_image_t image;
	volume_t v;

	meta_image(&image);
	meta_open(&v, &image);
	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		gen_inode_t inode = { .nid = 1, .mode = MODE_REGULAR | 0644,
		    .nlink = 1, .layout = cases[i].layout, .size = 10 };
		erofs_inode_t loaded;
		fsimg_error_t error;

		gen_inode_write(&image, &inode);
		check_status(cases[i].name, load(&v, 1, &loaded, &error),
		    cases[i].want, &error);
	}
	volume_stop(&v, "layouts");
	gen_image_free(&image);
}

/* ---- Entries and content through the public reader ---- */

/*
 * These tests build whole trees with the generator, open them with
 * fsimg_reader_open() and walk them to the end, so the status of the first
 * failing step is the status the caller sees. Each listed file and link is
 * read completely through fsimg_reader_read_at().
 */

enum {
	WALK_MAX = 512,
	PATH_BYTES = 320,
	LAYOUT_FLAT = 0
};

#define MODE_FILE	0100644
#define MODE_DIR	040755
#define MODE_LINK	0120777

enum {
	T_UNKNOWN = 0,
	T_FILE = 1,
	T_DIR = 2,
	T_CHAR = 3,
	T_BLOCK = 4,
	T_FIFO = 5,
	T_SOCKET = 6,
	T_LINK = 7
};

typedef struct node {
	char		 path[PATH_BYTES];
	fsimg_entry_kind_t kind;
	uint64_t	 size;
	fsimg_metadata_t meta;
	unsigned char	*content;	/* size bytes of a file or link */
} node_t;

typedef struct listing {
	node_t		 nodes[WALK_MAX];
	size_t		 count;
} listing_t;

static listing_t *
listing_new(void)
{
	listing_t *list = calloc(1, sizeof(*list));

	CHECK(list != NULL);
	return (list);
}

static void
listing_free(listing_t *list)
{
	for (size_t i = 0; i < list->count; i++)
		free(list->nodes[i].content);
	free(list);
}

static const node_t *
find_node(const listing_t *list, const char *path)
{
	for (size_t i = 0; i < list->count; i++) {
		if (strcmp(list->nodes[i].path, path) == 0)
			return (&list->nodes[i]);
	}
	return (NULL);
}

/* A byte of the test content: it differs by seed, offset and block. */
static unsigned char
pattern_byte(unsigned seed, size_t i)
{
	return ((unsigned char)(seed + i * 31 + (i >> 8) * 7));
}

static void
fill_pattern(unsigned char *bytes, size_t length, unsigned seed)
{
	for (size_t i = 0; i < length; i++)
		bytes[i] = pattern_byte(seed, i);
}

/* The first offset where the node's bytes leave the pattern, or its size. */
static size_t
pattern_mismatch(const node_t *node, unsigned seed)
{
	size_t i;

	for (i = 0; i < node->size; i++) {
		if (node->content[i] != pattern_byte(seed, i))
			break;
	}
	return (i);
}

static fsimg_status_t
read_content(fsimg_reader_t *reader, node_t *node, fsimg_error_t *error)
{
	size_t done = 0;

	if (node->size == 0)
		return (FSIMG_OK);
	node->content = malloc(node->size);
	CHECK(node->content != NULL);
	while (done < node->size) {
		size_t count = 0;
		fsimg_status_t status;

		status = fsimg_reader_read_at(reader, done, node->content + done,
		    node->size - done, &count, error);
		if (status != FSIMG_OK)
			return (status);
		if (count == 0)
			return (FSIMG_ESTATE);
		done += count;
	}
	return (FSIMG_OK);
}

static fsimg_status_t
list_entry(fsimg_reader_t *reader, const fsimg_entry_t *entry,
    listing_t *list, fsimg_error_t *error)
{
	node_t *node;

	CHECK(list->count < WALK_MAX && strlen(entry->path) < PATH_BYTES);
	node = &list->nodes[list->count++];
	strcpy(node->path, entry->path);
	node->kind = entry->kind;
	node->size = entry->size;
	node->meta = entry->metadata;
	if (entry->kind != FSIMG_ENTRY_FILE &&
	    entry->kind != FSIMG_ENTRY_SYMLINK)
		return (FSIMG_OK);
	return (read_content(reader, node, error));
}

/*
 * Open the image, list every entry with its content and close it again,
 * requiring that the memory the walk took comes back. The result is the
 * status of the first step that failed, or FSIMG_OK at the end of the walk.
 */
static fsimg_status_t
walk_image(const gen_image_t *image, listing_t *list, fsimg_error_t *error)
{
	const fsimg_entry_t *entry;
	fsimg_reader_t *reader = NULL;
	fsimg_stats_t stats;
	fsimg_status_t status;
	volume_t v;

	volume_start(&v, image->bytes, image->length);
	status = fsimg_reader_open(v.ctx, v.io, NULL, &reader, error);
	while (status == FSIMG_OK) {
		status = fsimg_reader_next(reader, &entry, error);
		if (status == FSIMG_OK)
			status = list_entry(reader, entry, list, error);
	}
	fsimg_reader_free(reader);
	fsimg_io_free(v.io);
	CHECK(fsimg_ctx_get_stats(v.ctx, &stats, NULL) == FSIMG_OK);
	EXPECT(stats.memory_bytes == v.baseline,
	    "a walk returns its memory to the baseline (%llu != %llu)",
	    (unsigned long long)stats.memory_bytes,
	    (unsigned long long)v.baseline);
	fsimg_ctx_free(v.ctx);
	return (status == FSIMG_DONE ? FSIMG_OK : status);
}

/*
 * Seal the tree, walk it and require the status want and, if phrase is not
 * NULL, that phrase in the error message, so a case passes only through the
 * rule it names. The listing is returned.
 */
static listing_t *
walk_tree(const char *name, gen_tree_t *tree, fsimg_status_t want,
    const char *phrase)
{
	listing_t *list = listing_new();
	fsimg_error_t error;
	fsimg_status_t status;

	bzero(&error, sizeof(error));
	gen_tree_seal(tree);
	status = walk_image(&tree->image, list, &error);
	check_refusal(name, status, want, &error, phrase);
	return (list);
}

/* A walk that must reach the end of the image. */
static listing_t *
walk_ok(const char *name, gen_tree_t *tree)
{
	return (walk_tree(name, tree, FSIMG_OK, NULL));
}

/* A walk that must stop with status want, through the rule phrase names. */
static void
check_refused(const char *name, gen_tree_t *tree, fsimg_status_t want,
    const char *phrase)
{
	listing_free(walk_tree(name, tree, want, phrase));
}

/* A tree with room for the given number of data blocks. */
static void
tree_new(gen_tree_t *tree, unsigned blkszbits, unsigned data_blocks)
{
	gen_tree_init(tree, blkszbits, gen_tree_first_block(blkszbits) +
	    data_blocks);
}

static gen_inode_t
inode_proto(uint16_t mode, unsigned layout)
{
	gen_inode_t inode;

	bzero(&inode, sizeof(inode));
	inode.mode = mode;
	inode.nlink = 1;
	inode.layout = (uint8_t)layout;
	inode.uid = 1000;
	inode.gid = 100;
	return (inode);
}

static gen_inode_t
file_proto(unsigned layout)
{
	return (inode_proto(MODE_FILE, layout));
}

/* A file of size pattern bytes added to the tree; returns its NID. */
static uint64_t
add_file(gen_tree_t *tree, unsigned layout, size_t size, unsigned seed)
{
	gen_inode_t proto = file_proto(layout);
	unsigned char *bytes = calloc(size != 0 ? size : 1, 1);
	uint64_t nid;

	CHECK(bytes != NULL);
	fill_pattern(bytes, size, seed);
	nid = gen_tree_add(tree, &proto, bytes, size);
	free(bytes);
	return (nid);
}

/* The root directory naming just one child. */
static void
root_names(gen_tree_t *tree, const char *name, uint64_t nid, uint8_t type)
{
	gen_dirent_t child = { name, nid, type };

	gen_tree_dir(tree, 0, 0, &child, 1);
}

/* The records packed as given: the directory's bytes and its size. */
static unsigned char *
pack_records(const gen_tree_t *tree, const gen_dirent_t *records, size_t count,
    size_t *size)
{
	size_t block = (size_t)1 << tree->blkszbits;
	size_t capacity = count * block;	/* a record fits one block */
	unsigned char *bytes = calloc(capacity, 1);

	CHECK(bytes != NULL);
	*size = gen_dir_pack(bytes, capacity, block, records, count);
	return (bytes);
}

/* A flat directory with the records exactly as given, in the order given. */
static void
dir_raw(gen_tree_t *tree, uint64_t nid, const gen_dirent_t *records,
    size_t count)
{
	gen_inode_t dir = inode_proto(MODE_DIR, LAYOUT_FLAT);
	unsigned char *bytes;
	size_t size;

	bytes = pack_records(tree, records, count, &size);
	gen_tree_fill(tree, nid, &dir, bytes, size);
	free(bytes);
}

/*
 * The root as a flat-inline directory with the records as given, whose
 * "." and ".." name it. Its inode is placed last, with room for its tail,
 * and the superblock points at it.
 */
static void
inline_root_raw(gen_tree_t *tree, const gen_dirent_t *records, size_t count)
{
	gen_inode_t dir = inode_proto(MODE_DIR, LAYOUT_FLAT_INLINE);
	size_t block = (size_t)1 << tree->blkszbits;
	gen_dirent_t *named = calloc(count, sizeof(*named));
	unsigned char *bytes;
	uint64_t nid;
	size_t size;

	CHECK(named != NULL);
	bytes = pack_records(tree, records, count, &size);
	free(bytes);
	nid = gen_tree_slot(tree, GEN_COMPACT_BYTES, size & (block - 1));
	gen_tree_set_root(tree, nid);
	bcopy(records, named, count * sizeof(*named));
	for (size_t i = 0; i < count; i++) {
		if (strcmp(named[i].name, ".") == 0 ||
		    strcmp(named[i].name, "..") == 0)
			named[i].nid = nid;
	}
	bytes = pack_records(tree, named, count, &size);
	gen_tree_fill(tree, nid, &dir, bytes, size);
	free(bytes);
	free(named);
}

/*
 * How many records the first k blocks of the records, packed as dir_raw
 * packs them, hold: the index of the first record of block k.
 */
static size_t
records_before_block(const gen_tree_t *tree, const gen_dirent_t *records,
    size_t count, size_t k)
{
	size_t block = (size_t)1 << tree->blkszbits;
	unsigned char *bytes;
	size_t size, total = 0;

	bytes = pack_records(tree, records, count, &size);
	CHECK(size > k * block);
	for (size_t i = 0; i < k; i++) {
		/* The first record's name offset is 12 bytes per record. */
		total += (size_t)(bytes[i * block + 8] |
		    bytes[i * block + 9] << 8) / 12;
	}
	free(bytes);
	return (total);
}

/* Exchange the last record before block k with the first record of it. */
static void
swap_across_block(const gen_tree_t *tree, gen_dirent_t *records, size_t count,
    size_t k)
{
	size_t first = records_before_block(tree, records, count, k);
	gen_dirent_t held;

	CHECK(first > 2 && first < count);
	held = records[first - 1];
	records[first - 1] = records[first];
	records[first] = held;
}

/* ---- Mapping at every boundary ---- */

typedef struct size_case {
	unsigned	 blkszbits;
	size_t		 size;
	unsigned	 layout;
} size_case_t;

/* Flat-inline content that is a nonzero multiple of B is not readable. */
static bool
size_is_refused(const size_case_t *c)
{
	size_t block = (size_t)1 << c->blkszbits;

	return (c->layout == LAYOUT_FLAT_INLINE && c->size != 0 &&
	    c->size % block == 0);
}

static void
test_every_boundary_size_is_read_byte_for_byte(void)
{
	static const unsigned bits[] = { 9, 12 };
	static const unsigned layouts[] = { LAYOUT_FLAT, LAYOUT_FLAT_INLINE };

	for (size_t b = 0; b < ARRAY_SIZE(bits); b++) {
		size_t block = (size_t)1 << bits[b];
		const size_t sizes[] = { 0, 1, block - 1, block, block + 1,
		    2 * block };

		for (size_t l = 0; l < ARRAY_SIZE(layouts); l++) {
			for (size_t s = 0; s < ARRAY_SIZE(sizes); s++) {
				size_case_t c = { bits[b], sizes[s], layouts[l] };
				char name[64];
				gen_tree_t tree;
				listing_t *list;
				const node_t *node;
				uint64_t nid;

				snprintf(name, sizeof(name),
				    "layout %u B=%zu size=%zu", c.layout,
				    block, c.size);
				tree_new(&tree, c.blkszbits, 8);
				nid = add_file(&tree, c.layout, c.size, 5);
				root_names(&tree, "f", nid, T_FILE);
				if (size_is_refused(&c)) {
					check_refused(name, &tree, FSIMG_ECORRUPT,
					    "inline tail of a whole block");
					gen_tree_free(&tree);
					continue;
				}
				list = walk_ok(name, &tree);
				node = find_node(list, "/f");
				EXPECT(node != NULL && node->size == c.size &&
				    pattern_mismatch(node, 5) == c.size,
				    "%s: every byte read back", name);
				listing_free(list);
				gen_tree_free(&tree);
			}
		}
	}
}

/* A flat file of blocks blocks placed last in a volume of exactly its end. */
static void
flat_file_at_volume_end(gen_tree_t *tree, unsigned blkszbits, unsigned blocks)
{
	size_t block = (size_t)1 << blkszbits;
	gen_inode_t proto = file_proto(LAYOUT_FLAT);
	unsigned char *bytes = calloc(blocks * block, 1);
	uint64_t nid;

	CHECK(bytes != NULL);
	fill_pattern(bytes, blocks * block, 3);
	tree_new(tree, blkszbits, 1 + blocks);

	/* The root takes the first data block, so the file takes the rest. */
	nid = gen_tree_slot(tree, GEN_COMPACT_BYTES, 0);
	root_names(tree, "f", nid, T_FILE);
	gen_tree_fill(tree, nid, &proto, bytes, blocks * block);
	free(bytes);
}

static void
test_flat_extent_ending_at_the_volume_end_is_read(void)
{
	static const unsigned bits[] = { 9, 12 };

	for (size_t b = 0; b < ARRAY_SIZE(bits); b++) {
		char name[32];
		gen_tree_t tree;
		listing_t *list;
		const node_t *node;

		snprintf(name, sizeof(name), "B=%u", 1u << bits[b]);
		flat_file_at_volume_end(&tree, bits[b], 2);
		list = walk_ok(name, &tree);
		node = find_node(list, "/f");
		EXPECT(node != NULL && pattern_mismatch(node, 3) == node->size,
		    "%s: the last block of the volume is read", name);
		listing_free(list);
		gen_tree_free(&tree);
	}
}

static void
test_flat_extent_one_block_past_the_volume_end_is_corrupt(void)
{
	static const unsigned bits[] = { 9, 12 };

	for (size_t b = 0; b < ARRAY_SIZE(bits); b++) {
		char name[32];
		gen_tree_t tree;

		snprintf(name, sizeof(name), "B=%u", 1u << bits[b]);
		flat_file_at_volume_end(&tree, bits[b], 2);
		gen_tree_set_blocks(&tree, tree.blocks - 1);
		check_refused(name, &tree, FSIMG_ECORRUPT,
		    "data blocks outside the volume");
		gen_tree_free(&tree);
	}
}

/* A one-file tree whose file has the layout, size and start word given. */
static void
word_case(gen_tree_t *tree, unsigned layout, size_t size, uint32_t word)
{
	gen_inode_t proto = file_proto(layout);
	unsigned char *bytes = calloc(size != 0 ? size : 1, 1);
	uint64_t nid;

	CHECK(bytes != NULL);
	fill_pattern(bytes, size, 9);
	tree_new(tree, 9, 8);
	proto.layout_word = word;
	nid = gen_tree_add(tree, &proto, bytes, size);
	root_names(tree, "f", nid, T_FILE);
	free(bytes);
}

/* Overwrite the layout word of the inode at nid. */
static void
set_word(gen_tree_t *tree, uint64_t nid, uint32_t word)
{
	gen_put(&tree->image, gen_inode_offset(&tree->image, nid) + 0x10,
	    word, 4);
}

static void
test_flat_inline_file_under_a_block_ignores_its_start_word(void)
{
	gen_tree_t tree;
	listing_t *list;
	const node_t *node;

	word_case(&tree, LAYOUT_FLAT_INLINE, 511, UINT32_C(0xffffffff));
	list = walk_ok("flat-inline 511 bytes, word 0xffffffff", &tree);
	node = find_node(list, "/f");
	EXPECT(node != NULL && pattern_mismatch(node, 9) == 511,
	    "the inline tail is the whole content");
	listing_free(list);
	gen_tree_free(&tree);
}

static void
test_flat_file_with_content_and_start_word_ffffffff_is_corrupt(void)
{
	gen_tree_t tree;

	word_case(&tree, LAYOUT_FLAT, 1, 0);
	set_word(&tree, 1, UINT32_C(0xffffffff));
	check_refused("flat 1 byte, word 0xffffffff", &tree, FSIMG_ECORRUPT,
	    "data start block 0xFFFFFFFF");
	gen_tree_free(&tree);

	/* Any other word outside the volume is refused by the domain rule. */
	word_case(&tree, LAYOUT_FLAT, 1, 0);
	set_word(&tree, 1, UINT32_C(0xfffffffe));
	check_refused("flat 1 byte, word 0xfffffffe", &tree, FSIMG_ECORRUPT,
	    "data blocks outside the volume");
	gen_tree_free(&tree);
}

static void
test_flat_inline_with_a_full_block_reads_its_start_word(void)
{
	gen_tree_t tree;

	word_case(&tree, LAYOUT_FLAT_INLINE, 600, 0);
	set_word(&tree, 1, UINT32_C(0xffffffff));
	check_refused("flat-inline 600 bytes, word 0xffffffff", &tree,
	    FSIMG_ECORRUPT, "data start block 0xFFFFFFFF");
	gen_tree_free(&tree);
}

static void
test_empty_file_ignores_its_start_word_in_either_layout(void)
{
	static const uint32_t words[] = { 0, 12345, UINT32_C(0xffffffff) };

	for (size_t l = 0; l < 2; l++) {
		unsigned layout = l == 0 ? LAYOUT_FLAT : LAYOUT_FLAT_INLINE;

		for (size_t w = 0; w < ARRAY_SIZE(words); w++) {
			char name[48];
			gen_tree_t tree;
			listing_t *list;
			const node_t *node;

			snprintf(name, sizeof(name), "layout %u word 0x%x",
			    layout, (unsigned)words[w]);
			word_case(&tree, layout, 0, words[w]);
			list = walk_ok(name, &tree);
			node = find_node(list, "/f");
			EXPECT(node != NULL && node->size == 0, "%s: empty file",
			    name);
			listing_free(list);
			gen_tree_free(&tree);
		}
	}
}

/*
 * A flat-inline file with no full block whose inode starts a metadata block
 * and whose tail is the given length: the inode takes 32 bytes, so a tail of
 * B - 32 ends at the block's end and a longer one crosses it.
 */
static void
tail_case(gen_tree_t *tree, size_t tail)
{
	gen_inode_t proto = file_proto(LAYOUT_FLAT_INLINE);
	size_t block = 512;
	unsigned char *bytes = calloc(tail, 1);
	uint64_t nid;

	CHECK(bytes != NULL);
	fill_pattern(bytes, tail, 11);
	tree_new(tree, 9, 4);
	nid = gen_nid_at(&tree->image, tree->image.meta_base + block);
	proto.layout_word = UINT32_C(0xffffffff);
	gen_tree_fill(tree, nid, &proto, bytes, tail);
	root_names(tree, "f", nid, T_FILE);
	free(bytes);
}

static void
test_inline_tail_ending_at_the_block_end_is_read(void)
{
	gen_tree_t tree;
	listing_t *list;
	const node_t *node;

	tail_case(&tree, 512 - 32);
	list = walk_ok("tail ends at the block end", &tree);
	node = find_node(list, "/f");
	EXPECT(node != NULL && node->size == 480 &&
	    pattern_mismatch(node, 11) == 480, "the tail is read whole");
	listing_free(list);
	gen_tree_free(&tree);
}

static void
test_inline_tail_crossing_the_block_end_is_corrupt(void)
{
	gen_tree_t tree;

	tail_case(&tree, 512 - 31);
	check_refused("tail crosses the block end", &tree, FSIMG_ECORRUPT,
	    "inline tail crosses");
	gen_tree_free(&tree);
}

/* ---- Directories ---- */

/* A tree whose root holds the records given, and one small file "a". */
static void
raw_root_tree(gen_tree_t *tree, const gen_dirent_t *records, size_t count)
{
	uint64_t file;

	tree_new(tree, 9, 8);
	file = add_file(tree, LAYOUT_FLAT_INLINE, 3, 1);
	CHECK(file == 1);
	dir_raw(tree, 0, records, count);
}

#define DOT	{ ".", 0, T_DIR }
#define DOTDOT	{ "..", 0, T_DIR }

static void
test_directory_over_several_blocks_lists_every_name_in_order(void)
{
	static const unsigned bits[] = { 9, 12 };

	for (size_t b = 0; b < ARRAY_SIZE(bits); b++) {
		size_t block = (size_t)1 << bits[b];
		/* A name record is 29 bytes. */
		size_t count = 2 * block / 30 + 5;
		gen_dirent_t *kids = calloc(count, sizeof(*kids));
		char (*names)[24] = calloc(count, sizeof(*names));
		gen_tree_t tree;
		listing_t *list;
		uint64_t nid;

		CHECK(kids != NULL && names != NULL);
		tree_new(&tree, bits[b], 8);
		nid = add_file(&tree, LAYOUT_FLAT_INLINE, 3, 1);
		for (size_t i = 0; i < count; i++) {
			snprintf(names[i], sizeof(names[i]), "n%03zu-padding-name",
			    i);
			kids[i] = (gen_dirent_t){ names[i], nid, T_FILE };
		}
		gen_tree_dir(&tree, 0, 0, kids, count);
		list = walk_ok("multi-block directory", &tree);
		EXPECT(list->count == count, "B=%zu: %zu names listed, not %zu",
		    block, list->count, count);
		for (size_t i = 0; i < list->count && i < count; i++) {
			char path[32];

			snprintf(path, sizeof(path), "/%s", names[i]);
			EXPECT(strcmp(list->nodes[i].path, path) == 0,
			    "B=%zu: entry %zu is %s", block, i, path);
		}
		listing_free(list);
		gen_tree_free(&tree);
		free(names);
		free(kids);
	}
}

/* Many same-length names at B = 512, so a record fits wherever its swap does. */
enum {
	MANY_NAMES = 40,
	MANY_BYTES = 24
};

static void
many_names(gen_dirent_t *records, char names[][MANY_BYTES], uint64_t nid)
{
	records[0] = (gen_dirent_t){ ".", 0, T_DIR };
	records[1] = (gen_dirent_t){ "..", 0, T_DIR };
	for (size_t i = 0; i < MANY_NAMES; i++) {
		snprintf(names[i], MANY_BYTES, "n%03zu-padding-name", i);
		records[2 + i] = (gen_dirent_t){ names[i], nid, T_FILE };
	}
}

static void
test_names_out_of_order_across_a_block_boundary_are_corrupt(void)
{
	gen_dirent_t all[MANY_NAMES + 2];
	char names[MANY_NAMES][MANY_BYTES];
	gen_tree_t tree;
	uint64_t file;

	tree_new(&tree, 9, 8);
	file = add_file(&tree, LAYOUT_FLAT_INLINE, 3, 1);
	many_names(all, names, file);
	swap_across_block(&tree, all, MANY_NAMES + 2, 1);
	dir_raw(&tree, 0, all, MANY_NAMES + 2);
	check_refused("last of block 0 above first of block 1", &tree,
	    FSIMG_ECORRUPT, "directory names out of order");
	gen_tree_free(&tree);
}

/*
 * A flat-inline root: a one-block directory held wholly in the inline tail,
 * and a 2B + k one whose last block is the tail, so the name order is
 * checked from the second stored block into the inline one.
 */
static void
test_flat_inline_directories_list_their_stored_and_inline_blocks(void)
{
	static const size_t counts[] = { 3, MANY_NAMES };

	for (size_t c = 0; c < ARRAY_SIZE(counts); c++) {
		gen_dirent_t all[MANY_NAMES + 2];
		char names[MANY_NAMES][MANY_BYTES];
		gen_tree_t tree;
		listing_t *list;
		uint64_t file;
		size_t size;

		tree_new(&tree, 9, 8);
		file = add_file(&tree, LAYOUT_FLAT_INLINE, 3, 1);
		many_names(all, names, file);
		free(pack_records(&tree, all, counts[c] + 2, &size));
		CHECK(c == 0 ? size < 512 : size > 1024 && size % 512 != 0);
		inline_root_raw(&tree, all, counts[c] + 2);
		list = walk_ok("flat-inline root", &tree);
		EXPECT(list->count == counts[c], "directory of %zu bytes: "
		    "%zu names listed, not %zu", size, list->count, counts[c]);
		for (size_t i = 0; i < list->count && i < counts[c]; i++) {
			char path[MANY_BYTES + 1];

			snprintf(path, sizeof(path), "/%s", names[i]);
			EXPECT(strcmp(list->nodes[i].path, path) == 0,
			    "directory of %zu bytes: entry %zu is %s", size, i,
			    path);
		}
		listing_free(list);
		gen_tree_free(&tree);
	}
}

static void
test_names_out_of_order_into_the_inline_tail_are_corrupt(void)
{
	gen_dirent_t all[MANY_NAMES + 2];
	char names[MANY_NAMES][MANY_BYTES];
	gen_tree_t tree;
	uint64_t file;

	tree_new(&tree, 9, 8);
	file = add_file(&tree, LAYOUT_FLAT_INLINE, 3, 1);
	many_names(all, names, file);
	swap_across_block(&tree, all, MANY_NAMES + 2, 2);
	inline_root_raw(&tree, all, MANY_NAMES + 2);
	check_refused("last stored block above the inline block", &tree,
	    FSIMG_ECORRUPT, "directory names out of order");
	gen_tree_free(&tree);
}

/*
 * The last name of a full block that is not the last block ends exactly at
 * the block's end, with no NUL after it: its length is the block's valid
 * length, not a search for a terminator.
 */
static void
test_name_ending_at_the_end_of_a_full_block_is_read_whole(void)
{
	char a[151], b[151], c[150];
	gen_dirent_t records[6];
	gen_tree_t tree;
	listing_t *list;
	char path[160];
	uint64_t file;

	memset(a, 'a', 150);
	memset(b, 'b', 150);
	memset(c, 'c', 149);
	a[150] = b[150] = c[149] = '\0';
	tree_new(&tree, 9, 8);
	file = add_file(&tree, LAYOUT_FLAT_INLINE, 3, 1);
	records[0] = (gen_dirent_t){ ".", 0, T_DIR };
	records[1] = (gen_dirent_t){ "..", 0, T_DIR };
	records[2] = (gen_dirent_t){ a, file, T_FILE };
	records[3] = (gen_dirent_t){ b, file, T_FILE };
	records[4] = (gen_dirent_t){ c, file, T_FILE };
	records[5] = (gen_dirent_t){ "d", file, T_FILE };
	/*
	 * Block 0 holds five records, 60 bytes, and the names "." "..", a, b
	 * and c, 1 + 2 + 150 + 150 + 149 = 452 bytes: 512 exactly.
	 */
	CHECK(records_before_block(&tree, records, 6, 1) == 5);
	dir_raw(&tree, 0, records, 6);
	list = walk_ok("name ends at the end of block 0", &tree);
	snprintf(path, sizeof(path), "/%s", c);
	EXPECT(list->count == 4 && find_node(list, path) != NULL &&
	    find_node(list, "/d") != NULL, "all four names, the long ones whole");
	listing_free(list);
	gen_tree_free(&tree);
}

/*
 * Names of 255 bytes that differ only in their last byte, one to a block at
 * B = 512, so the previous name carried across each block boundary is
 * compared at full width.
 */
enum {
	LONG_NAMES = 4,
	LONG_BYTES = 256
};

static void
long_names(gen_dirent_t *records, char names[][LONG_BYTES], uint64_t nid)
{
	records[0] = (gen_dirent_t){ ".", 0, T_DIR };
	records[1] = (gen_dirent_t){ "..", 0, T_DIR };
	for (size_t i = 0; i < LONG_NAMES; i++) {
		memset(names[i], 'x', 254);
		names[i][254] = (char)('a' + i);
		names[i][255] = '\0';
		records[2 + i] = (gen_dirent_t){ names[i], nid, T_FILE };
	}
}

static void
test_directory_of_255_byte_names_lists_across_its_block_boundaries(void)
{
	gen_dirent_t records[LONG_NAMES + 2];
	char names[LONG_NAMES][LONG_BYTES];
	gen_tree_t tree;
	listing_t *list;
	uint64_t file;

	tree_new(&tree, 9, 8);
	file = add_file(&tree, LAYOUT_FLAT_INLINE, 3, 1);
	long_names(records, names, file);
	CHECK(records_before_block(&tree, records, LONG_NAMES + 2, 2) == 4);
	dir_raw(&tree, 0, records, LONG_NAMES + 2);
	list = walk_ok("four 255-byte names", &tree);
	EXPECT(list->count == LONG_NAMES, "%zu names listed", list->count);
	for (size_t i = 0; i < list->count && i < LONG_NAMES; i++) {
		char path[LONG_BYTES + 1];

		snprintf(path, sizeof(path), "/%s", names[i]);
		EXPECT(strlen(list->nodes[i].path) == 256 &&
		    strcmp(list->nodes[i].path, path) == 0,
		    "entry %zu is the full 255-byte name", i);
	}
	listing_free(list);
	gen_tree_free(&tree);
}

static void
test_255_byte_names_out_of_order_only_at_a_block_boundary_are_corrupt(void)
{
	gen_dirent_t records[LONG_NAMES + 2];
	char names[LONG_NAMES][LONG_BYTES];
	gen_tree_t tree;
	uint64_t file;

	tree_new(&tree, 9, 8);
	file = add_file(&tree, LAYOUT_FLAT_INLINE, 3, 1);
	long_names(records, names, file);
	swap_across_block(&tree, records, LONG_NAMES + 2, 1);
	dir_raw(&tree, 0, records, LONG_NAMES + 2);
	check_refused("255-byte names swapped across block 0 and 1", &tree,
	    FSIMG_ECORRUPT, "directory names out of order");
	gen_tree_free(&tree);
}

typedef struct root_case {
	const char	*name;
	gen_dirent_t	 records[6];
	size_t		 count;
	fsimg_status_t	 want;
	const char	*phrase;	/* the rule that refuses it */
} root_case_t;

static void
test_root_directory_records_are_checked(void)
{
	/* "" is the last record of a full block, followed by a name that
	 * starts the next block, so zeros follow the empty name inside the
	 * block's valid length.
	 */
	static const root_case_t cases[] = {
		{ "name out of order within a block",
		    { DOT, DOTDOT, { "b", 1, T_FILE }, { "a", 1, T_FILE } }, 4,
		    FSIMG_ECORRUPT, "directory names out of order" },
		{ "repeated name",
		    { DOT, DOTDOT, { "a", 1, T_FILE }, { "a", 1, T_FILE } }, 4,
		    FSIMG_ECORRUPT, "directory names out of order" },
		{ "dot entries in order and nothing else",
		    { DOT, DOTDOT }, 2, FSIMG_OK, NULL },
		{ "missing dot",
		    { DOTDOT, { "a", 1, T_FILE } }, 2, FSIMG_ECORRUPT,
		    "directory without \".\"" },
		{ "missing dot dot",
		    { DOT, { "a", 1, T_FILE } }, 2, FSIMG_ECORRUPT,
		    "directory without \"..\"" },
		{ "name with a slash",
		    { DOT, DOTDOT, { "a/b", 1, T_FILE } }, 3, FSIMG_ECORRUPT,
		    "directory name byte" },
		{ "empty name ending a full block",
		    { DOT, DOTDOT, { "a", 1, T_FILE }, { "", 1, T_FILE },
		    { "b", 1, T_FILE | GEN_STARTS_BLOCK } }, 5, FSIMG_ECORRUPT,
		    "empty directory name" },
		{ "file type 0 defers to the mode",
		    { DOT, DOTDOT, { "a", 1, 0 } }, 3, FSIMG_OK, NULL },
		{ "file type disagrees with the mode",
		    { DOT, DOTDOT, { "a", 1, T_DIR } }, 3, FSIMG_ECORRUPT,
		    "file type disagrees" },
		{ "link type on a regular file",
		    { DOT, DOTDOT, { "a", 1, T_LINK } }, 3, FSIMG_ECORRUPT,
		    "file type disagrees" },
		{ "dot with a file type of 1",
		    { { ".", 0, T_FILE }, DOTDOT }, 2, FSIMG_ECORRUPT,
		    "dot entry file type" },
		{ "dot entries with file type 0",
		    { { ".", 0, 0 }, { "..", 0, 0 } }, 2, FSIMG_OK, NULL },
		{ "dot dot naming another inode than the root",
		    { DOT, { "..", 1, 0 } }, 2, FSIMG_ECORRUPT,
		    "dot entry breaks the parent rule" },
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		gen_tree_t tree;

		raw_root_tree(&tree, cases[i].records, cases[i].count);
		listing_free(walk_tree(cases[i].name, &tree, cases[i].want,
		    cases[i].phrase));
		gen_tree_free(&tree);
	}
}

static void
test_dot_after_a_name_that_sorts_before_it_is_accepted(void)
{
	gen_tree_t tree;
	listing_t *list;

	tree_new(&tree, 9, 8);
	root_names(&tree, "-a", add_file(&tree, LAYOUT_FLAT_INLINE, 3, 1),
	    T_FILE);
	list = walk_ok("-a sorts before the dot entries", &tree);
	EXPECT(list->count == 1 && find_node(list, "/-a") != NULL,
	    "the only entry is /-a, not the dot entries");
	listing_free(list);
	gen_tree_free(&tree);
}

/* NIDs in subdir_case: the directory "d" is the first slot, the file next. */
enum {
	SUBDIR_NID = 1,
	SUBFILE_NID = 2
};

/* The root holds directory "d" whose records are given. */
static void
subdir_case(const char *name, const gen_dirent_t *records, size_t count,
    fsimg_status_t want, const char *phrase)
{
	gen_tree_t tree;
	uint64_t file, dir;

	tree_new(&tree, 9, 8);
	dir = gen_tree_slot(&tree, GEN_COMPACT_BYTES, 0);
	file = add_file(&tree, LAYOUT_FLAT_INLINE, 3, 1);
	CHECK(dir == SUBDIR_NID && file == SUBFILE_NID);
	root_names(&tree, "d", dir, T_DIR);
	dir_raw(&tree, dir, records, count);
	listing_free(walk_tree(name, &tree, want, phrase));
	gen_tree_free(&tree);
}

static void
test_subdirectory_dot_entries_must_name_it_and_its_parent(void)
{
	static const gen_dirent_t good[] = {
		{ ".", SUBDIR_NID, T_DIR }, { "..", 0, T_DIR } };
	static const gen_dirent_t dotdot_names_itself[] = {
		{ ".", SUBDIR_NID, T_DIR }, { "..", SUBDIR_NID, T_DIR } };
	static const gen_dirent_t dot_names_parent[] = {
		{ ".", 0, T_DIR }, { "..", 0, T_DIR } };
	static const gen_dirent_t names_itself[] = {
		{ ".", SUBDIR_NID, T_DIR }, { "..", 0, T_DIR },
		{ "self", SUBDIR_NID, T_DIR } };
	static const gen_dirent_t names_parent[] = {
		{ ".", SUBDIR_NID, T_DIR }, { "..", 0, T_DIR },
		{ "up", 0, T_DIR } };

	subdir_case("dot entries correct", good, 2, FSIMG_OK, NULL);
	subdir_case("dot dot names the directory itself", dotdot_names_itself,
	    2, FSIMG_ECORRUPT, "dot entry breaks the parent rule");
	subdir_case("dot names the parent", dot_names_parent, 2,
	    FSIMG_ECORRUPT, "dot entry breaks the parent rule");
	subdir_case("a child entry names the directory", names_itself, 3,
	    FSIMG_ECORRUPT, "directory entry naming the directory or its parent");
	subdir_case("a child entry names the parent", names_parent, 3,
	    FSIMG_ECORRUPT, "directory entry naming the directory or its parent");
}

static void
test_a_directory_named_from_two_parents_fails_the_second_descent(void)
{
	gen_tree_t tree;
	uint64_t a, b, shared;
	gen_dirent_t root_kids[2], under[1];

	tree_new(&tree, 9, 16);
	a = gen_tree_slot(&tree, GEN_COMPACT_BYTES, 0);
	b = gen_tree_slot(&tree, GEN_COMPACT_BYTES, 0);
	shared = gen_tree_slot(&tree, GEN_COMPACT_BYTES, 0);
	root_kids[0] = (gen_dirent_t){ "a", a, T_DIR };
	root_kids[1] = (gen_dirent_t){ "b", b, T_DIR };
	under[0] = (gen_dirent_t){ "shared", shared, T_DIR };
	gen_tree_dir(&tree, 0, 0, root_kids, 2);
	gen_tree_dir(&tree, a, 0, under, 1);
	gen_tree_dir(&tree, b, 0, under, 1);
	gen_tree_dir(&tree, shared, a, NULL, 0);
	check_refused("shared directory below a and b", &tree, FSIMG_ECORRUPT,
	    "dot entry breaks the parent rule");
	gen_tree_free(&tree);
}

static void
test_two_names_for_one_directory_in_one_parent_are_both_listed(void)
{
	gen_tree_t tree;
	uint64_t dir, file;
	gen_dirent_t root_kids[2], inside[1];
	listing_t *list;

	tree_new(&tree, 9, 16);
	file = add_file(&tree, LAYOUT_FLAT_INLINE, 3, 1);
	dir = gen_tree_slot(&tree, GEN_COMPACT_BYTES, 0);
	root_kids[0] = (gen_dirent_t){ "x", dir, T_DIR };
	root_kids[1] = (gen_dirent_t){ "y", dir, T_DIR };
	inside[0] = (gen_dirent_t){ "f", file, T_FILE };
	gen_tree_dir(&tree, 0, 0, root_kids, 2);
	gen_tree_dir(&tree, dir, 0, inside, 1);
	list = walk_ok("one directory named twice by the root", &tree);
	EXPECT(find_node(list, "/x/f") != NULL && find_node(list, "/y/f") !=
	    NULL, "the shared subtree is listed under both names");
	listing_free(list);
	gen_tree_free(&tree);
}

/*
 * root -> a -> b, where b also lists the inode `up` as a child: a is b's
 * parent, and the root is b's grandparent, which the parent rule meets when
 * it descends into the root and finds its ".." naming the root, not b.
 */
static void
cycle_case(const char *name, bool up_is_root, const char *phrase)
{
	gen_dirent_t root_kids[1], a_kids[1], b_kids[1];
	gen_tree_t tree;
	uint64_t a, b;

	tree_new(&tree, 9, 8);
	a = gen_tree_slot(&tree, GEN_COMPACT_BYTES, 0);
	b = gen_tree_slot(&tree, GEN_COMPACT_BYTES, 0);
	root_kids[0] = (gen_dirent_t){ "a", a, T_DIR };
	a_kids[0] = (gen_dirent_t){ "b", b, T_DIR };
	b_kids[0] = (gen_dirent_t){ "up", up_is_root ? 0 : a, T_DIR };
	gen_tree_dir(&tree, 0, 0, root_kids, 1);
	gen_tree_dir(&tree, a, 0, a_kids, 1);
	gen_tree_dir(&tree, b, a, b_kids, 1);
	check_refused(name, &tree, FSIMG_ECORRUPT, phrase);
	gen_tree_free(&tree);
}

static void
test_a_cycle_back_through_an_ancestor_at_depth_2_is_corrupt(void)
{
	cycle_case("b lists its parent a", false,
	    "directory entry naming the directory or its parent");
	cycle_case("b lists the root", true,
	    "dot entry breaks the parent rule");
}

static void
test_names_that_are_not_portable_utf8_are_omitted_not_refused(void)
{
	static const char *const kept[] = { "/ok", "/caf\xc3\xa9" };
	gen_tree_t tree;
	uint64_t file;
	gen_dirent_t kids[5];
	listing_t *list;

	tree_new(&tree, 9, 8);
	file = add_file(&tree, LAYOUT_FLAT_INLINE, 3, 1);
	kids[0] = (gen_dirent_t){ "ok", file, T_FILE };
	kids[1] = (gen_dirent_t){ "bad\xff", file, T_FILE };
	kids[2] = (gen_dirent_t){ "ctl\x01", file, T_FILE };
	kids[3] = (gen_dirent_t){ "caf\xc3\xa9", file, T_FILE };
	kids[4] = (gen_dirent_t){ "\xc3", file, T_FILE };	/* truncated */
	gen_tree_dir(&tree, 0, 0, kids, 5);
	list = walk_ok("names outside the portable profile", &tree);
	EXPECT(list->count == ARRAY_SIZE(kept), "%zu entries delivered, not %zu",
	    list->count, ARRAY_SIZE(kept));
	for (size_t i = 0; i < ARRAY_SIZE(kept); i++)
		EXPECT(find_node(list, kept[i]) != NULL, "%s is delivered",
		    kept[i]);
	listing_free(list);
	gen_tree_free(&tree);
}

/* ---- Content through common delivery ---- */

static void
test_stored_and_inline_content_and_hard_links_read_back(void)
{
	gen_tree_t tree;
	gen_dirent_t kids[4];
	uint64_t flat, mixed, small;
	gen_inode_t linked = file_proto(LAYOUT_FLAT_INLINE);
	unsigned char bytes[40];
	listing_t *list;
	const node_t *node;

	tree_new(&tree, 9, 16);
	flat = add_file(&tree, LAYOUT_FLAT, 2 * 512 + 7, 21);
	mixed = add_file(&tree, LAYOUT_FLAT_INLINE, 512 + 5, 22);

	/* The link count of "small" says two names; the reader lists both. */
	linked.nlink = 2;
	fill_pattern(bytes, sizeof(bytes), 23);
	small = gen_tree_add(&tree, &linked, bytes, sizeof(bytes));
	kids[0] = (gen_dirent_t){ "flat", flat, T_FILE };
	kids[1] = (gen_dirent_t){ "mixed", mixed, T_FILE };
	kids[2] = (gen_dirent_t){ "small", small, T_FILE };
	kids[3] = (gen_dirent_t){ "small-link", small, T_FILE };
	gen_tree_dir(&tree, 0, 0, kids, 4);

	list = walk_ok("stored, inline and linked files", &tree);
	EXPECT(list->count == 4, "%zu entries", list->count);
	node = find_node(list, "/flat");
	EXPECT(node != NULL && node->size == 1031 &&
	    pattern_mismatch(node, 21) == 1031, "stored blocks and a tail");
	node = find_node(list, "/mixed");
	EXPECT(node != NULL && node->size == 517 &&
	    pattern_mismatch(node, 22) == 517, "a block and an inline tail");
	node = find_node(list, "/small");
	EXPECT(node != NULL && pattern_mismatch(node, 23) == 40,
	    "inline content");
	node = find_node(list, "/small-link");
	EXPECT(node != NULL && node->size == 40 &&
	    pattern_mismatch(node, 23) == 40,
	    "a second name for the same inode has the same bytes");
	listing_free(list);
	gen_tree_free(&tree);
}

/*
 * Reads that start inside the inline tail, and that cross from the last
 * stored block into it, return the same bytes as a whole-file read.
 */
static void
test_reads_at_offsets_cross_from_stored_blocks_into_the_inline_tail(void)
{
	enum { SIZE = 512 + 100 };
	static const struct {
		size_t		 offset;
		size_t		 length;
	} reads[] = {
		{ 0, SIZE }, { 511, 2 }, { 512, 10 }, { 530, 70 },
		{ SIZE - 1, 1 }, { 400, 200 }
	};
	const fsimg_entry_t *entry = NULL;
	fsimg_reader_t *reader = NULL;
	fsimg_error_t error;
	gen_tree_t tree;
	volume_t v;

	tree_new(&tree, 9, 8);
	root_names(&tree, "f", add_file(&tree, LAYOUT_FLAT_INLINE, SIZE, 5),
	    T_FILE);
	gen_tree_seal(&tree);
	volume_start(&v, tree.image.bytes, tree.image.length);
	CHECK(fsimg_reader_open(v.ctx, v.io, NULL, &reader, &error) == FSIMG_OK);
	CHECK(fsimg_reader_next(reader, &entry, &error) == FSIMG_OK);
	CHECK(strcmp(entry->path, "/f") == 0 && entry->size == SIZE);
	for (size_t r = 0; r < ARRAY_SIZE(reads); r++) {
		unsigned char got[SIZE];
		size_t done = 0, count = 1;

		while (done < reads[r].length && count != 0) {
			CHECK(fsimg_reader_read_at(reader, reads[r].offset + done,
			    got + done, reads[r].length - done, &count,
			    &error) == FSIMG_OK);
			done += count;
		}
		EXPECT(done == reads[r].length, "read at %zu for %zu bytes "
		    "returned %zu", reads[r].offset, reads[r].length, done);
		for (size_t i = 0; i < done; i++)
			if (got[i] != pattern_byte(5, reads[r].offset + i)) {
				EXPECT(false, "read at %zu for %zu bytes: byte %zu",
				    reads[r].offset, reads[r].length, i);
				break;
			}
	}
	fsimg_reader_free(reader);
	fsimg_io_free(v.io);
	fsimg_ctx_free(v.ctx);
	gen_tree_free(&tree);
}

/* ---- Symbolic links ---- */

/* A tree whose root names one link of the layout, size and bytes given. */
static void
link_case(gen_tree_t *tree, unsigned layout, size_t size, int nul_at)
{
	gen_inode_t proto = inode_proto(MODE_LINK, layout);
	unsigned char *target = calloc(size != 0 ? size : 1, 1);
	uint64_t nid;

	CHECK(target != NULL);
	for (size_t i = 0; i < size; i++)
		target[i] = (unsigned char)('a' + i % 26);
	if (nul_at >= 0)
		target[nul_at] = 0;
	tree_new(tree, 9, 8);
	proto.layout_word = UINT32_C(0xffffffff);
	nid = gen_tree_add(tree, &proto, target, size);
	root_names(tree, "l", nid, T_LINK);
	free(target);
}

typedef struct link_case_def {
	const char	*name;
	unsigned	 layout;
	size_t		 size;
	int		 nul_at;
	fsimg_status_t	 want;
	const char	*phrase;
} link_case_def_t;

static void
test_symbolic_link_targets_are_checked_and_delivered(void)
{
	static const link_case_def_t cases[] = {
		{ "one byte, inline", LAYOUT_FLAT_INLINE, 1, -1, FSIMG_OK, NULL },
		{ "one byte, flat", LAYOUT_FLAT, 1, -1, FSIMG_OK, NULL },
		{ "1024 bytes across two blocks", LAYOUT_FLAT, 1024, -1,
		    FSIMG_OK, NULL },
		{ "block and inline tail", LAYOUT_FLAT_INLINE, 600, -1,
		    FSIMG_OK, NULL },
		{ "empty target", LAYOUT_FLAT_INLINE, 0, -1, FSIMG_ECORRUPT,
		    "symbolic link size" },
		{ "1025 bytes", LAYOUT_FLAT, 1025, -1, FSIMG_EUNSUPPORTED,
		    "symbolic link size" },
		{ "NUL inside the target", LAYOUT_FLAT_INLINE, 20, 7,
		    FSIMG_ECORRUPT, "NUL byte" },
		{ "NUL as the last byte", LAYOUT_FLAT_INLINE, 20, 19,
		    FSIMG_ECORRUPT, "NUL byte" },
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		const link_case_def_t *c = &cases[i];
		gen_tree_t tree;
		listing_t *list;
		const node_t *node;

		link_case(&tree, c->layout, c->size, c->nul_at);
		list = walk_tree(c->name, &tree, c->want, c->phrase);
		node = find_node(list, "/l");
		if (c->want == FSIMG_OK) {
			EXPECT(node != NULL && node->kind == FSIMG_ENTRY_SYMLINK &&
			    node->size == c->size, "%s: a link of its size",
			    c->name);
			for (size_t k = 0; node != NULL && k < c->size; k++)
				if (node->content[k] != 'a' + k % 26) {
					EXPECT(false, "%s: target byte %zu",
					    c->name, k);
					break;
				}
		}
		listing_free(list);
		gen_tree_free(&tree);
	}
}

/* ---- Extended attributes ---- */

/* A tree with one inline file "f" whose attribute count is count. */
static uint64_t
xattr_file(gen_tree_t *tree, unsigned count)
{
	gen_inode_t proto = file_proto(LAYOUT_FLAT_INLINE);
	unsigned char bytes[8];
	uint64_t nid;

	fill_pattern(bytes, sizeof(bytes), 31);
	tree_new(tree, 9, 4);
	proto.xattr_count = (uint16_t)count;
	proto.layout_word = UINT32_C(0xffffffff);
	nid = gen_tree_add(tree, &proto, bytes, sizeof(bytes));
	root_names(tree, "f", nid, T_FILE);
	return (nid);
}

/* Where the attribute region of an inode with a compact fixed part starts. */
static uint64_t
xattr_at(const gen_tree_t *tree, uint64_t nid)
{
	return (gen_inode_offset(&tree->image, nid) + GEN_COMPACT_BYTES);
}

static bool
has_extra_data(const listing_t *list)
{
	const node_t *node = find_node(list, "/f");

	return (node != NULL && (node->meta.valid & FSIMG_META_EXTRA_DATA) != 0);
}

typedef struct attr_case {
	const char	*name;
	unsigned	 count;		/* the inode's attribute count */
	unsigned	 shared;	/* shared ids, one block id 0 each */
	unsigned	 index;		/* the one inline entry's name index */
	const char	*suffix;	/* ... its suffix, NULL for no entry */
	unsigned	 suffix_length;	/* the length byte, if it lies */
	fsimg_status_t	 want;
	const char	*phrase;	/* the rule that refuses it */
	bool		 extra_data;
} attr_case_t;

static void
test_attribute_regions_set_the_extra_data_mark_or_are_refused(void)
{
	static const attr_case_t cases[] = {
		{ .name = "no attributes", .want = FSIMG_OK },
		{ .name = "header only", .count = 1, .want = FSIMG_OK },
		{ .name = "one inline entry", .count = 3, .index = 1,
		    .suffix = "k", .suffix_length = 1, .want = FSIMG_OK,
		    .extra_data = true },
		{ .name = "one shared id", .count = 2, .shared = 1,
		    .want = FSIMG_OK, .extra_data = true },
		{ .name = "entry longer than the region", .count = 2,
		    .index = 1, .suffix = "", .suffix_length = 8,
		    .want = FSIMG_ECORRUPT, .phrase = "attribute entry length" },
		{ .name = "unknown name index 5", .count = 2, .index = 5,
		    .suffix = "", .want = FSIMG_EUNSUPPORTED,
		    .phrase = "attribute name index" },
		{ .name = "ACL index 2 with a suffix", .count = 3, .index = 2,
		    .suffix = "x", .suffix_length = 1, .want = FSIMG_ECORRUPT,
		    .phrase = "ACL attribute suffix" },
		{ .name = "ACL index 3 with a suffix", .count = 3, .index = 3,
		    .suffix = "x", .suffix_length = 1, .want = FSIMG_ECORRUPT,
		    .phrase = "ACL attribute suffix" },
		{ .name = "ACL index 2 without a suffix", .count = 2,
		    .index = 2, .suffix = "", .want = FSIMG_OK,
		    .extra_data = true },
		{ .name = "ACL index 3 without a suffix", .count = 2,
		    .index = 3, .suffix = "", .want = FSIMG_OK,
		    .extra_data = true },
		{ .name = "index 4", .count = 2, .index = 4, .suffix = "",
		    .want = FSIMG_OK, .extra_data = true },
		{ .name = "index 6", .count = 2, .index = 6, .suffix = "",
		    .want = FSIMG_OK, .extra_data = true },
		{ .name = "index 7", .count = 2, .index = 7, .suffix = "",
		    .want = FSIMG_EUNSUPPORTED,
		    .phrase = "attribute name index" },
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		const attr_case_t *c = &cases[i];
		gen_tree_t tree;
		uint64_t nid = xattr_file(&tree, c->count);
		uint64_t region = xattr_at(&tree, nid);
		listing_t *list;

		if (c->shared != 0) {
			gen_put(&tree.image, region + 4, c->shared, 1);
			gen_put(&tree.image, region + 12, 0, 4);
		}
		if (c->suffix != NULL) {
			size_t used = gen_xattr_entry(&tree.image, region + 12,
			    c->index, c->suffix, "v", 0);

			gen_put(&tree.image, region + 12, c->suffix_length, 1);
			CHECK(used <= 4 * (c->count - 1));
		}
		list = walk_tree(c->name, &tree, c->want, c->phrase);
		if (c->want == FSIMG_OK)
			EXPECT(has_extra_data(list) == c->extra_data,
			    "%s: extra-data mark is %d", c->name,
			    (int)c->extra_data);
		listing_free(list);
		gen_tree_free(&tree);
	}
}

/* ---- Kinds, owners and times ---- */

/* A tree whose root names one inode, of the form and fields given. */
static void
meta_case(gen_tree_t *tree, gen_inode_t proto, uint32_t compat)
{
	uint64_t nid;

	tree_new(tree, 9, 4);
	nid = gen_tree_add(tree, &proto, NULL, 0);
	root_names(tree, "n", nid, T_UNKNOWN);
	gen_tree_set_compat(tree, compat | 1);
}

static void
test_device_numbers_decode_the_linux_32_bit_encoding(void)
{
	static const struct {
		const char	*name;
		uint16_t	 mode;
		uint32_t	 word;
		uint32_t	 want_major, want_minor;
		fsimg_entry_kind_t kind;
	} cases[] = {
		{ "largest numbers", 020644, UINT32_C(0xffffffff), 4095,
		    1048575, FSIMG_ENTRY_CHAR_DEVICE },
		{ "small numbers", 060644, UINT32_C(0x801), 8, 1,
		    FSIMG_ENTRY_BLOCK_DEVICE },
		{ "minor above 8 bits", 020644, UINT32_C(0x12301245), 0x12,
		    0x12345, FSIMG_ENTRY_CHAR_DEVICE },
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		gen_inode_t proto = inode_proto(cases[i].mode, LAYOUT_FLAT);
		gen_tree_t tree;
		listing_t *list;
		const node_t *node;

		proto.layout_word = cases[i].word;
		meta_case(&tree, proto, 0);
		list = walk_ok(cases[i].name, &tree);
		node = find_node(list, "/n");
		EXPECT(node != NULL && node->kind == cases[i].kind &&
		    node->size == 0 &&
		    (node->meta.valid & FSIMG_META_DEVICE) != 0 &&
		    node->meta.dev_major == cases[i].want_major &&
		    node->meta.dev_minor == cases[i].want_minor,
		    "%s: %u:%u", cases[i].name, cases[i].want_major,
		    cases[i].want_minor);
		listing_free(list);
		gen_tree_free(&tree);
	}
}

static void
test_fifos_and_sockets_have_no_size_and_no_device_number(void)
{
	static const struct {
		uint16_t	 mode;
		fsimg_entry_kind_t kind;
	} cases[] = {
		{ 010644, FSIMG_ENTRY_FIFO },
		{ 0140755, FSIMG_ENTRY_SOCKET },
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		gen_tree_t tree;
		listing_t *list;
		const node_t *node;

		meta_case(&tree, inode_proto(cases[i].mode, LAYOUT_FLAT), 0);
		list = walk_ok("fifo or socket", &tree);
		node = find_node(list, "/n");
		EXPECT(node != NULL && node->kind == cases[i].kind &&
		    node->size == 0 && (node->meta.valid & FSIMG_META_DEVICE) == 0,
		    "mode %o is a kind %d without a device", cases[i].mode,
		    (int)cases[i].kind);
		listing_free(list);
		gen_tree_free(&tree);
	}
}

static void
test_owner_and_permission_bits_are_delivered(void)
{
	gen_inode_t proto = inode_proto(0104755, LAYOUT_FLAT);
	gen_tree_t tree;
	listing_t *list;
	const node_t *node;

	proto.uid = 1234;
	proto.gid = 4321;
	meta_case(&tree, proto, 0);
	list = walk_ok("setuid file", &tree);
	node = find_node(list, "/n");
	EXPECT(node != NULL && node->meta.uid == 1234 && node->meta.gid == 4321 &&
	    node->meta.mode == 04755 && (node->meta.valid & FSIMG_META_OWNER) &&
	    (node->meta.valid & FSIMG_META_MODE), "uid, gid and the low 12 bits");
	listing_free(list);
	gen_tree_free(&tree);
}

static void
test_the_stored_time_is_modified_with_mtime_and_changed_without(void)
{
	static const uint32_t compats[] = { 0, 2 };

	for (size_t i = 0; i < ARRAY_SIZE(compats); i++) {
		gen_inode_t proto = inode_proto(MODE_FILE, LAYOUT_FLAT);
		bool mtime = (compats[i] & 2) != 0;
		gen_tree_t tree;
		listing_t *list;
		const node_t *node;
		const fsimg_timestamp_t *got;
		uint32_t other;

		proto.extended = true;
		proto.time = 1700000000;
		proto.time_nsec = 123456789;
		meta_case(&tree, proto, compats[i]);
		list = walk_ok(mtime ? "MTIME set" : "MTIME clear", &tree);
		node = find_node(list, "/n");
		CHECK(node != NULL);
		got = mtime ? &node->meta.modified : &node->meta.changed;
		other = mtime ? FSIMG_META_CHANGED : FSIMG_META_MODIFIED;
		EXPECT(got->seconds == 1700000000 &&
		    got->nanoseconds == 123456789 &&
		    got->basis == FSIMG_TIME_UTC && got->precision_ns == 1 &&
		    (node->meta.valid & (mtime ? FSIMG_META_MODIFIED :
		    FSIMG_META_CHANGED)) != 0 && (node->meta.valid & other) == 0,
		    "mtime=%d: the one time is delivered under one name", mtime);
		listing_free(list);
		gen_tree_free(&tree);
	}
}

static void
test_a_compact_inode_time_is_the_stored_word_after_the_epoch(void)
{
	gen_inode_t proto = inode_proto(MODE_FILE, LAYOUT_FLAT);
	gen_tree_t tree;
	listing_t *list;
	const node_t *node;

	proto.time = 86400;
	meta_case(&tree, proto, 2);
	list = walk_ok("compact time with MTIME", &tree);
	node = find_node(list, "/n");
	EXPECT(node != NULL && node->meta.modified.seconds == 86400 &&
	    node->meta.modified.nanoseconds == 0 &&
	    node->meta.modified.precision_ns == 1000000000,
	    "epoch 0 plus the word, to the second");
	listing_free(list);
	gen_tree_free(&tree);
}

/* ---- Chunk-based files ---- */

/*
 * A chunk-based file described by a recipe, one letter per chunk: 'd' is a
 * chunk of its own data blocks and '.' a hole. The helper stores the data,
 * writes the index and builds the bytes the file must read back as, with
 * zeros for every hole. A chunk is B << chunkbits bytes and the recipe has
 * ceil(size / chunk) letters, so the last chunk is short when the size is
 * not a multiple of it. 48-bit block numbers are not read, so no recipe sets
 * chunk format bit 6.
 */
typedef struct chunk_file {
	unsigned	 blkszbits;
	unsigned	 chunkbits;
	unsigned	 entry_bytes;	/* 4 or 8 */
	uint16_t	 layout_high;	/* the layout word's unspecified half */
	uint64_t	 size;
	const char	*recipe;
	bool		 extended;
} chunk_file_t;

static uint16_t
chunk_format(const chunk_file_t *spec)
{
	return ((uint16_t)(spec->chunkbits |
	    (spec->entry_bytes == 8 ? GEN_CHUNK_INDEXES : 0)));
}

static gen_inode_t
chunk_proto(const chunk_file_t *spec)
{
	gen_inode_t proto = file_proto(4);

	proto.extended = spec->extended;
	proto.size = spec->size;
	return (proto);
}

/*
 * A tree whose root names the file as "f". *expect is the file's size bytes
 * as they must read back, which the caller frees. Returns the file's NID.
 */
static uint64_t
chunk_tree(gen_tree_t *tree, const chunk_file_t *spec, unsigned char **expect)
{
	size_t block = (size_t)1 << spec->blkszbits;
	size_t chunk_bytes = block << spec->chunkbits;
	size_t count = strlen(spec->recipe);
	size_t size = (size_t)spec->size;
	gen_inode_t proto = chunk_proto(spec);
	gen_chunk_t entries[16];
	uint64_t nid;

	CHECK(count == (size + chunk_bytes - 1) / chunk_bytes &&
	    count <= ARRAY_SIZE(entries));
	tree_new(tree, spec->blkszbits, (size + block - 1) / block + count + 2);
	gen_tree_set_incompat(tree, EROFS_INCOMPAT_CHUNKED_FILE);
	*expect = calloc(size != 0 ? size : 1, 1);
	CHECK(*expect != NULL);

	for (size_t i = 0; i < count; i++) {
		size_t start = i * chunk_bytes;
		size_t length = size - start < chunk_bytes ? size - start :
		    chunk_bytes;
		size_t stored = (length + block - 1) / block * block;
		unsigned char *data;

		if (spec->recipe[i] == '.') {
			entries[i] = gen_chunk_hole(chunk_format(spec));
			continue;
		}
		CHECK(spec->recipe[i] == 'd');
		fill_pattern(*expect + start, length, 20 + (unsigned)i);
		data = calloc(stored, 1);
		CHECK(data != NULL);
		bcopy(*expect + start, data, length);
		entries[i] = gen_chunk_block(gen_tree_data(tree, data, stored));
		free(data);
	}
	nid = gen_tree_add_chunked(tree, &proto,
	    (uint32_t)spec->layout_high << 16 | chunk_format(spec), entries,
	    count);
	root_names(tree, "f", nid, T_FILE);
	return (nid);
}

/* The walk of the tree must deliver "/f" as exactly the bytes expected. */
static void
check_chunk_file_reads(const char *name, gen_tree_t *tree,
    const unsigned char *expect, size_t size)
{
	listing_t *list = walk_ok(name, tree);
	const node_t *node = find_node(list, "/f");

	EXPECT(node != NULL && node->size == size &&
	    (size == 0 || bcmp(node->content, expect, size) == 0),
	    "%s: the file reads back byte for byte", name);
	listing_free(list);
}

static void
test_chunk_files_read_byte_for_byte_at_every_size_and_shape(void)
{
	static const chunk_file_t shapes[] = {
		/* A hole in the middle, and chunks that are one block. */
		{ .blkszbits = 12, .size = 3 * 4096, .recipe = "d.d" },
		{ .blkszbits = 12, .size = 3 * 4096, .recipe = "ddd" },
		{ .blkszbits = 12, .size = 3 * 4096, .recipe = ".d." },
		/* The last chunk is short. */
		{ .blkszbits = 12, .size = 2 * 4096 + 1, .recipe = "d.d" },
		{ .blkszbits = 12, .size = 4096 + 100, .recipe = "d." },
		{ .blkszbits = 12, .size = 4095, .recipe = "d" },
		{ .blkszbits = 9, .size = 513, .recipe = "dd" },
		/* The size is an exact multiple of the chunk. */
		{ .blkszbits = 12, .size = 2 * 4096, .recipe = "d." },
		{ .blkszbits = 12, .chunkbits = 1, .size = 2 * 8192,
		    .recipe = ".d" },
		/* A chunk is larger than a block. */
		{ .blkszbits = 12, .chunkbits = 1, .size = 3 * 8192 + 1,
		    .recipe = "d.dd" },
		{ .blkszbits = 9, .chunkbits = 3, .size = 5000, .recipe = "d." },
		/* A file smaller than its one chunk, whole or all hole. */
		{ .blkszbits = 12, .chunkbits = 2, .size = 100, .recipe = "d" },
		{ .blkszbits = 12, .chunkbits = 2, .size = 100, .recipe = "." },
		{ .blkszbits = 12, .chunkbits = 5, .size = 1, .recipe = "." },
		/* Extended inodes and the layout word's unspecified half. */
		{ .blkszbits = 12, .size = 3 * 4096 + 7, .recipe = "d.dd",
		    .extended = true },
		{ .blkszbits = 12, .layout_high = 0xbeef, .size = 3 * 4096,
		    .recipe = "d.d" },
		{ .blkszbits = 12, .layout_high = 0xffff, .size = 4096 + 1,
		    .recipe = ".d" },
	};

	for (size_t i = 0; i < ARRAY_SIZE(shapes); i++) {
		for (unsigned width = 4; width <= 8; width += 4) {
			chunk_file_t spec = shapes[i];
			unsigned char *expect;
			char name[96];
			gen_tree_t tree;

			spec.entry_bytes = width;
			snprintf(name, sizeof(name),
			    "shape %zu, B=%u chunkbits=%u size=%llu \"%s\", "
			    "%u-byte entries", i, 1u << spec.blkszbits,
			    spec.chunkbits, (unsigned long long)spec.size,
			    spec.recipe, width);
			chunk_tree(&tree, &spec, &expect);
			check_chunk_file_reads(name, &tree, expect,
			    (size_t)spec.size);
			free(expect);
			gen_tree_free(&tree);
		}
	}
}

/* ---- Chunk extents through resolve ---- */

/* The reader of the tree's image, positioned on the entry named path. */
static fsimg_reader_t *
reader_on_entry(volume_t *v, const gen_image_t *image, const char *path)
{
	const fsimg_entry_t *entry;
	fsimg_reader_t *reader = NULL;
	fsimg_error_t error;

	volume_start(v, image->bytes, image->length);
	CHECK(fsimg_reader_open(v->ctx, v->io, NULL, &reader, &error) ==
	    FSIMG_OK);
	do {
		CHECK(fsimg_reader_next(reader, &entry, &error) == FSIMG_OK);
	} while (strcmp(entry->path, path) != 0);
	return (reader);
}

static void
reader_close(volume_t *v, fsimg_reader_t *reader)
{
	fsimg_reader_free(reader);
	fsimg_io_free(v->io);
	fsimg_ctx_free(v->ctx);
}

/* The extent the reader resolves at offset, which must succeed unheld. */
static fsimg_extent_t
resolve_ok(fsimg_reader_t *reader, uint64_t offset)
{
	const unsigned char *held = NULL;
	fsimg_extent_t extent;
	fsimg_error_t error;

	CHECK(fsimg_reader_resolve(reader, offset, &extent, &held, &error) ==
	    FSIMG_OK);
	CHECK(held == NULL);
	return (extent);
}

static void
test_chunk_hole_resolves_to_a_zero_extent_of_the_chunks_length(void)
{
	static const struct {
		const char	*recipe;
		uint64_t	 size;
	} cases[] = {
		{ "d.d", 2 * 4096 + 100 },
		{ "d.", 4096 + 100 },
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		chunk_file_t spec = { .blkszbits = 12, .entry_bytes = 4,
		    .size = cases[i].size, .recipe = cases[i].recipe };
		uint64_t first = gen_tree_first_block(12);
		unsigned char *expect;
		fsimg_reader_t *reader;
		fsimg_extent_t extent;
		gen_tree_t tree;
		volume_t v;
		uint64_t last = cases[i].size - 1;

		chunk_tree(&tree, &spec, &expect);
		gen_tree_seal(&tree);
		reader = reader_on_entry(&v, &tree.image, "/f");

		extent = resolve_ok(reader, 10);
		EXPECT(extent.logical_offset == 0 && extent.length == 4096 &&
		    extent.image_offset == first * 4096,
		    "case %zu: a data chunk is a stored extent of its block", i);
		extent = resolve_ok(reader, 4096 + 5);
		EXPECT(extent.image_offset == FSIMG_EXTENT_ZERO &&
		    extent.logical_offset == 4096 &&
		    extent.length == (cases[i].recipe[2] == 'd' ? 4096 : 100),
		    "case %zu: the hole is a zero extent of its chunk (%llu, "
		    "%llu bytes)", i, (unsigned long long)extent.logical_offset,
		    (unsigned long long)extent.length);
		extent = resolve_ok(reader, last);
		EXPECT(extent.logical_offset + extent.length == cases[i].size,
		    "case %zu: the last chunk ends at the size", i);
		reader_close(&v, reader);
		free(expect);
		gen_tree_free(&tree);
	}
}

/* ---- Shared chunks ---- */

static void
test_chunks_naming_one_block_read_correctly_in_both_files(void)
{
	static const unsigned width[] = { 4, 8 };

	for (size_t w = 0; w < ARRAY_SIZE(width); w++) {
		uint16_t format = width[w] == 8 ? GEN_CHUNK_INDEXES : 0;
		gen_inode_t proto = file_proto(4);
		unsigned char shared[4096], own[4096];
		gen_chunk_t entries[2];
		gen_dirent_t names[2];
		gen_tree_t tree;
		listing_t *list;
		const node_t *a, *b;
		uint32_t block;

		fill_pattern(shared, sizeof(shared), 40);
		fill_pattern(own, sizeof(own), 41);
		tree_new(&tree, 12, 6);
		gen_tree_set_incompat(&tree, EROFS_INCOMPAT_CHUNKED_FILE);
		block = gen_tree_data(&tree, shared, sizeof(shared));
		entries[0] = gen_chunk_block(block);
		entries[1] = gen_chunk_block(block);
		proto.size = 2 * 4096;
		names[0] = (gen_dirent_t){ "a", gen_tree_add_chunked(&tree,
		    &proto, format, entries, 2), T_FILE };

		/* The second file shares the block with the first's chunks,
		 * after a block of its own. */
		entries[0] = gen_chunk_block(gen_tree_data(&tree, own,
		    sizeof(own)));
		entries[1] = gen_chunk_block(block);
		names[1] = (gen_dirent_t){ "b", gen_tree_add_chunked(&tree,
		    &proto, format, entries, 2), T_FILE };
		gen_tree_dir(&tree, 0, 0, names, 2);

		list = walk_ok("shared chunks", &tree);
		a = find_node(list, "/a");
		b = find_node(list, "/b");
		EXPECT(a != NULL && a->size == 8192 &&
		    bcmp(a->content, shared, 4096) == 0 &&
		    bcmp(a->content + 4096, shared, 4096) == 0,
		    "%u-byte entries: both chunks of one file read the block",
		    width[w]);
		EXPECT(b != NULL && b->size == 8192 &&
		    bcmp(b->content, own, 4096) == 0 &&
		    bcmp(b->content + 4096, shared, 4096) == 0,
		    "%u-byte entries: another file reads the same block",
		    width[w]);
		listing_free(list);
		gen_tree_free(&tree);
	}
}

/* ---- Chunk rules refused through the reader ---- */

typedef struct chunk_mutation {
	const char	*name;
	unsigned	 entry_bytes;
	unsigned	 chunkbits;
	uint64_t	 size;
	const char	*recipe;
	size_t		 index;		/* the entry replaced, if replace */
	bool		 replace;
	gen_chunk_t	 entry;
	uint32_t	 from_end;	/* nonzero: block is blocks - from_end */
	uint32_t	 word_or;	/* ORed into the layout word */
	const char	*phrase;
} chunk_mutation_t;

static void
test_chunk_files_that_break_a_rule_are_corrupt_naming_it(void)
{
	static const chunk_mutation_t cases[] = {
		{ .name = "a nonzero device identifier", .entry_bytes = 8,
		    .size = 3 * 4096, .recipe = "d.d", .index = 2,
		    .replace = true, .entry = { .block = 40, .device = 1 },
		    .phrase = "chunk device identifier" },
		{ .name = "a nonzero high part without bit 6",
		    .entry_bytes = 8, .size = 3 * 4096, .recipe = "d.d",
		    .index = 0, .replace = true,
		    .entry = { .block = 40, .high = 1 },
		    .phrase = "chunk block high bits" },
		{ .name = "a hole entry with a high part is no hole",
		    .entry_bytes = 8, .size = 3 * 4096, .recipe = "d.d",
		    .index = 1, .replace = true,
		    .entry = { .block = UINT32_MAX, .high = 0xffff },
		    .phrase = "chunk block high bits" },
		{ .name = "a chunk block past the domain", .entry_bytes = 4,
		    .size = 3 * 4096, .recipe = "d.d", .index = 2,
		    .replace = true, .entry = { .block = 0x7ffffffe },
		    .phrase = "data blocks outside the volume" },
		{ .name = "a chunk whose last whole block crosses the end",
		    .entry_bytes = 4, .chunkbits = 1, .size = 8192,
		    .recipe = "d", .index = 0, .replace = true, .from_end = 1,
		    .phrase = "data blocks outside the volume" },
		{ .name = "format bit 7", .entry_bytes = 4,
		    .size = 3 * 4096, .recipe = "d.d", .word_or = 0x80,
		    .phrase = "chunk format" },
		{ .name = "format bit 15", .entry_bytes = 8,
		    .size = 3 * 4096, .recipe = "d.d", .word_or = 0x8000,
		    .phrase = "chunk format" },
		{ .name = "format bit 6 without 48BIT", .entry_bytes = 8,
		    .size = 3 * 4096, .recipe = "d.d", .word_or = 0x40,
		    .phrase = "chunk format 48-bit bit" },
		/* 48BIT is refused at open, so this volume lacks it too and
		 * cannot isolate the 4-byte rule; the codec test
		 * test_chunk_format_48bit_bit_needs_8_byte_entries does. */
		{ .name = "format bit 6 with 4-byte entries", .entry_bytes = 4,
		    .size = 3 * 4096, .recipe = "d.d", .word_or = 0x40,
		    .phrase = "chunk format 48-bit bit" },
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		const chunk_mutation_t *m = &cases[i];
		chunk_file_t spec = { .blkszbits = 12, .chunkbits = m->chunkbits,
		    .entry_bytes = m->entry_bytes, .size = m->size,
		    .recipe = m->recipe };
		gen_inode_t proto = chunk_proto(&spec);
		unsigned char *expect;
		gen_tree_t tree;
		uint64_t nid;

		nid = chunk_tree(&tree, &spec, &expect);
		if (m->replace) {
			gen_chunk_t entry = m->entry;

			if (m->from_end != 0)
				entry.block = tree.blocks - m->from_end;
			gen_chunk_put(&tree, nid, &proto, chunk_format(&spec),
			    m->index, entry);
		}
		if (m->word_or != 0)
			set_word(&tree, nid, (uint32_t)chunk_format(&spec) |
			    m->word_or);
		check_refused(m->name, &tree, FSIMG_ECORRUPT, m->phrase);
		free(expect);
		gen_tree_free(&tree);
	}
}

static void
test_chunk_index_running_past_the_domain_is_refused_when_the_inode_loads(void)
{
	gen_inode_t proto = file_proto(4);
	gen_chunk_t hole = gen_chunk_hole(0);
	gen_tree_t tree;
	uint64_t nid;

	/* 2^40 bytes in 4096-byte chunks need a 1 GiB index. */
	proto.extended = true;
	proto.size = UINT64_C(1) << 40;
	tree_new(&tree, 12, 4);
	gen_tree_set_incompat(&tree, EROFS_INCOMPAT_CHUNKED_FILE);
	nid = gen_tree_add_chunked(&tree, &proto, 0, &hole, 1);
	root_names(&tree, "f", nid, T_FILE);
	check_refused("index past the domain", &tree, FSIMG_ECORRUPT,
	    "inode body outside the address domain");
	gen_tree_free(&tree);
}

/* ---- A large sparse file ---- */

static void
test_a_file_of_one_hole_chunk_reads_its_last_byte_as_zero(void)
{
	gen_inode_t proto = file_proto(4);
	gen_chunk_t hole = gen_chunk_hole(0);
	unsigned char window[4096], last = 0xaa;
	fsimg_reader_t *reader;
	fsimg_error_t error;
	fsimg_extent_t extent;
	gen_tree_t tree;
	volume_t v;
	size_t count = 0;
	uint64_t nid;

	/* One chunk of 4096 << 31 = 2^43 bytes holds the whole 2^40. */
	proto.extended = true;
	proto.size = UINT64_C(1) << 40;
	tree_new(&tree, 12, 4);
	gen_tree_set_incompat(&tree, EROFS_INCOMPAT_CHUNKED_FILE);
	nid = gen_tree_add_chunked(&tree, &proto, 31, &hole, 1);
	root_names(&tree, "f", nid, T_FILE);
	gen_tree_seal(&tree);

	reader = reader_on_entry(&v, &tree.image, "/f");
	extent = resolve_ok(reader, proto.size - 1);
	EXPECT(extent.image_offset == FSIMG_EXTENT_ZERO &&
	    extent.logical_offset == 0 && extent.length == proto.size,
	    "the hole chunk is one zero extent of the file's size");
	CHECK(fsimg_reader_read_at(reader, proto.size - 1, &last, 1, &count,
	    &error) == FSIMG_OK);
	EXPECT(count == 1 && last == 0, "the last byte reads as zero");
	memset(window, 0xaa, sizeof(window));
	CHECK(fsimg_reader_read_at(reader, UINT64_C(1) << 39, window,
	    sizeof(window), &count, &error) == FSIMG_OK);
	EXPECT(count == sizeof(window), "a window reads whole");
	for (size_t i = 0; i < sizeof(window); i++) {
		if (window[i] != 0) {
			EXPECT(false, "window byte %zu is %u", i, window[i]);
			break;
		}
	}
	reader_close(&v, reader);
	gen_tree_free(&tree);
}

/* ---- Salvage of a chunked file ---- */

/*
 * A chunked file whose data chunk and hole come before a chunk naming a block
 * past the volume. Extraction under salvage keeps the readable prefix, the
 * data and the zeros of the hole, as "f.partial" and the sibling whole;
 * without salvage the damaged chunk fails the extraction.
 */
static void
test_salvage_keeps_a_chunked_files_prefix_through_its_hole(void)
{
	enum { BLOCK = 4096 };
	unsigned char data[BLOCK], prefix[2 * BLOCK], got[3 * BLOCK];
	gen_inode_t proto = file_proto(4);
	gen_chunk_t entries[3];
	gen_dirent_t names[2];
	fsimg_extract_options_t options;
	fsimg_reader_t *reader = NULL;
	fsimg_error_t error;
	fsimg_status_t status;
	gen_tree_t tree;
	volume_t v;
	char directory[] = "erofs-salvage-XXXXXX";
	ssize_t got_bytes;
	int parent, output, fd;

	tree_new(&tree, 12, 8);
	gen_tree_set_incompat(&tree, EROFS_INCOMPAT_CHUNKED_FILE);
	fill_pattern(data, sizeof(data), 20);
	entries[0] = gen_chunk_block(gen_tree_data(&tree, data, sizeof(data)));
	entries[1] = gen_chunk_hole(0);
	entries[2] = gen_chunk_block(0x7ffffffe);
	proto.size = 3 * BLOCK;
	names[0] = (gen_dirent_t){ "f",
	    gen_tree_add_chunked(&tree, &proto, 0, entries, 3), T_FILE };
	names[1] = (gen_dirent_t){ "g", add_file(&tree, LAYOUT_FLAT, 600, 9),
	    T_FILE };
	gen_tree_dir(&tree, 0, 0, names, 2);
	gen_tree_seal(&tree);
	bcopy(data, prefix, BLOCK);
	bzero(prefix + BLOCK, BLOCK);

	CHECK(mkdtemp(directory) != NULL);
	parent = open(directory, O_RDONLY | O_DIRECTORY);
	CHECK(parent != -1);
	for (unsigned salvage = 0; salvage < 2; salvage++) {
		volume_start(&v, tree.image.bytes, tree.image.length);
		CHECK(fsimg_reader_open(v.ctx, v.io, NULL, &reader, &error) ==
		    FSIMG_OK);
		fsimg_extract_options_init(&options);
		if (salvage != 0)
			options.flags |= FSIMG_EXTRACT_SALVAGE;
		status = fsimg_extract_at(reader, parent,
		    salvage != 0 ? "salvage" : "strict", &options, &error);
		EXPECT_STATUS(status, salvage != 0 ? FSIMG_EPARTIAL :
		    FSIMG_ECORRUPT, error.message, salvage != 0 ? "salvage" :
		    "strict");
		reader_close(&v, reader);
	}

	output = openat(parent, "salvage", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(output != -1);
	fd = openat(output, "f.partial", O_RDONLY | O_NOFOLLOW);
	CHECK(fd != -1);
	got_bytes = read(fd, got, sizeof(got));
	EXPECT(got_bytes == (ssize_t)sizeof(prefix) &&
	    bcmp(got, prefix, sizeof(prefix)) == 0,
	    "f.partial is the data chunk then the hole's zeros (%zd bytes)",
	    got_bytes);
	CHECK(close(fd) == 0);
	EXPECT(faccessat(output, "f", F_OK, AT_SYMLINK_NOFOLLOW) == -1,
	    "the damaged file is not delivered whole");
	EXPECT(faccessat(output, "g", F_OK, AT_SYMLINK_NOFOLLOW) == 0,
	    "the sibling is delivered");
	CHECK(unlinkat(output, "f.partial", 0) == 0);
	CHECK(unlinkat(output, "g", 0) == 0);
	CHECK(close(output) == 0);
	CHECK(unlinkat(parent, "salvage", AT_REMOVEDIR) == 0);

	/* A strict extraction leaves its private output behind, incomplete. */
	output = openat(parent, "strict", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if (output != -1) {
		unlinkat(output, "f", 0);
		unlinkat(output, "g", 0);
		CHECK(close(output) == 0);
		unlinkat(parent, "strict", AT_REMOVEDIR);
	}
	CHECK(close(parent) == 0);
	CHECK(rmdir(directory) == 0);
	gen_tree_free(&tree);
}

/* ---- Chunk-based directories and symbolic links ---- */

static void
test_a_chunk_based_directory_lists_its_entries(void)
{
	static const unsigned width[] = { 4, 8 };

	for (size_t w = 0; w < ARRAY_SIZE(width); w++) {
		uint16_t format = width[w] == 8 ? GEN_CHUNK_INDEXES : 0;
		gen_inode_t proto = inode_proto(MODE_DIR, 4);
		unsigned char block[4096] = { 0 };
		gen_dirent_t records[3];
		gen_dirent_t root_child;
		gen_chunk_t entry;
		gen_tree_t tree;
		listing_t *list;
		const node_t *dir, *file;
		uint64_t file_nid, dir_nid;

		tree_new(&tree, 12, 6);
		gen_tree_set_incompat(&tree, EROFS_INCOMPAT_CHUNKED_FILE);
		file_nid = add_file(&tree, LAYOUT_FLAT, 600, 7);
		dir_nid = gen_nid_at(&tree.image, tree.meta_next);

		/* Records are sorted by name bytes: ".", "..", "f". */
		records[0] = (gen_dirent_t){ ".", dir_nid, T_DIR };
		records[1] = (gen_dirent_t){ "..", 0, T_DIR };
		records[2] = (gen_dirent_t){ "f", file_nid, T_FILE };
		proto.nlink = 2;
		proto.size = gen_dir_pack(block, sizeof(block), 4096, records,
		    3);
		entry = gen_chunk_block(gen_tree_data(&tree, block, 4096));
		CHECK(gen_tree_add_chunked(&tree, &proto, format, &entry,
		    1) == dir_nid);
		root_child = (gen_dirent_t){ "d", dir_nid, T_DIR };
		gen_tree_dir(&tree, 0, 0, &root_child, 1);

		list = walk_ok("chunk-based directory", &tree);
		dir = find_node(list, "/d");
		file = find_node(list, "/d/f");
		EXPECT(dir != NULL && dir->kind == FSIMG_ENTRY_DIRECTORY,
		    "%u-byte entries: the directory is listed", width[w]);
		EXPECT(file != NULL && file->size == 600 &&
		    pattern_mismatch(file, 7) == 600,
		    "%u-byte entries: its child is listed and read", width[w]);
		listing_free(list);
		gen_tree_free(&tree);
	}
}

static void
test_a_chunk_based_symbolic_link_delivers_its_target(void)
{
	static const char target[] = "some/target/path";
	gen_inode_t proto = inode_proto(MODE_LINK, 4);
	gen_chunk_t entry;
	gen_tree_t tree;
	listing_t *list;
	const node_t *node;
	uint64_t nid;

	proto.size = sizeof(target) - 1;
	tree_new(&tree, 12, 4);
	gen_tree_set_incompat(&tree, EROFS_INCOMPAT_CHUNKED_FILE);
	entry = gen_chunk_block(gen_tree_data(&tree, target,
	    sizeof(target) - 1));
	nid = gen_tree_add_chunked(&tree, &proto, 0, &entry, 1);
	root_names(&tree, "l", nid, T_LINK);

	list = walk_ok("chunk-based symbolic link", &tree);
	node = find_node(list, "/l");
	EXPECT(node != NULL && node->kind == FSIMG_ENTRY_SYMLINK &&
	    node->size == sizeof(target) - 1 &&
	    bcmp(node->content, target, sizeof(target) - 1) == 0,
	    "the link's target is the chunk's bytes");
	listing_free(list);
	gen_tree_free(&tree);
}

/* ---- Mutation matrix: geometry and features ---- */

/*
 * Each compatible bit the reader reads on its own is named and decides its
 * own feature. SHARED_EA_IN_METABOX (0x08) is not among them: alone it is
 * refused, because the shared attributes need the metadata box.
 */
static void
test_open_accepts_each_compatible_bit_alone(void)
{
	static const struct {
		uint32_t	 bit;
		const char	*text;
	} bits[] = {
		{ 0x02, "SB_CHKSUM MTIME" },
		{ 0x04, "SB_CHKSUM XATTR_FILTER" },
		{ 0x10, "SB_CHKSUM PLAIN_XATTR_PFX" },
		{ 0x20, "SB_CHKSUM ISHARE_XATTRS" },
	};

	for (size_t i = 0; i < ARRAY_SIZE(bits); i++) {
		gen_image_t image;
		fsimg_error_t error;
		volume_t v;
		const erofs_features_t *features;

		minimal_image(&image, 12);
		gen_super_put(&image, SB_COMPAT, COMPAT_SB_CHKSUM | bits[i].bit,
		    4);
		gen_seal(&image);
		volume_start(&v, image.bytes, image.length);
		check_status("compatible bit", volume_open(&v, 0, &error),
		    FSIMG_OK, &error);
		features = &erofs_state(&v.reader)->features;
		EXPECT(text_is(&v.reader.info, "compat_features", bits[i].text),
		    "bit 0x%x: compat_features is \"%s\"", (unsigned)bits[i].bit,
		    find_text(&v.reader.info, "compat_features") != NULL ?
		    find_text(&v.reader.info, "compat_features") : "");
		EXPECT(features->checksum && !features->unknown_compat &&
		    features->mtime == (bits[i].bit == 0x02) &&
		    features->xattr_filter == (bits[i].bit == 0x04) &&
		    features->plain_prefixes == (bits[i].bit == 0x10),
		    "bit 0x%x decides exactly its own feature",
		    (unsigned)bits[i].bit);
		volume_stop(&v, "compatible bit");
		gen_image_free(&image);
	}
}

/* Every incompatible bit from 9 to 31 is outside the table and refused. */
static void
test_open_refuses_every_unknown_incompatible_bit(void)
{
	for (unsigned bit = 9; bit < 32; bit++) {
		open_case_t c = { .reseal = true, .want = FSIMG_EUNSUPPORTED,
		    .phrase = "incompatible feature" };
		char name[24];

		snprintf(name, sizeof(name), "incompatible bit %u", bit);
		c.name = name;
		c.patches[0] = (patch_t){ SB_INCOMPAT, 4, UINT32_C(1) << bit };
		run_open_cases(&c, 1, 0);
	}
}

/*
 * Only 9 to 12 are block sizes: below is corrupt, above the reader's bound
 * is unsupported. The checksum is off so it cannot refuse first.
 */
static void
test_open_refuses_each_illegal_block_size(void)
{
	static const unsigned bits[] = { 0, 1, 7, 8, 13, 16, 31, 255 };

	for (size_t i = 0; i < ARRAY_SIZE(bits); i++) {
		bool small = bits[i] < 9;
		open_case_t c = { .want = small ? FSIMG_ECORRUPT :
		    FSIMG_EUNSUPPORTED, .phrase = "block size" };
		char name[24];

		snprintf(name, sizeof(name), "blkszbits %u", bits[i]);
		c.name = name;
		c.patches[0] = (patch_t){ SB_COMPAT, 4, 0 };
		c.patches[1] = (patch_t){ SB_BLKSZBITS, 1, bits[i] };
		run_open_cases(&c, 1, 0);
	}
}

/* Extension slots are skipped, however they are filled, without a box. */
static void
test_open_accepts_extension_slots_without_a_metadata_box(void)
{
	for (unsigned bits = 9; bits <= 12; bits++) {
		for (unsigned slots = 1; slots <= 4; slots++) {
			gen_super_t sb = gen_super_default(bits,
			    minimal_blocks(bits));
			gen_image_t image;
			char name[32];

			sb.slots = (uint8_t)slots;
			sb.box_nid = UINT64_C(0x1122334455667788);
			image_build(&image, &sb, NULL);
			snprintf(name, sizeof(name), "B=%u slots=%u", 1u << bits,
			    slots);
			check_open(name, &image, FSIMG_OK, NULL);
			gen_image_free(&image);
		}
	}
}

/*
 * A block count past the input opens, and the walk lists everything that
 * lies inside the input. Verification of the same image fails as truncated
 * before it walks anything.
 */
static void
test_a_block_count_past_the_input_lists_what_lies_in_the_input(void)
{
	fsimg_verify_report_t report;
	fsimg_reader_t *reader = NULL;
	fsimg_error_t error;
	fsimg_status_t status;
	gen_tree_t tree;
	listing_t *list;
	volume_t v;

	tree_new(&tree, 9, 8);
	root_names(&tree, "f", add_file(&tree, LAYOUT_FLAT_INLINE, 40, 3),
	    T_FILE);
	gen_tree_set_blocks(&tree, tree.blocks + 1000);
	list = walk_ok("block count 1000 past the input", &tree);
	EXPECT(list->count == 1 && find_node(list, "/f") != NULL &&
	    find_node(list, "/f")->size == 40, "the file in the input is listed");
	listing_free(list);

	bzero(&error, sizeof(error));
	bzero(&report, sizeof(report));
	volume_start(&v, tree.image.bytes, tree.image.length);
	status = fsimg_reader_open(v.ctx, v.io, NULL, &reader, &error);
	EXPECT(status == FSIMG_OK, "a fresh reader opens (%s)", error.message);
	if (status == FSIMG_OK)
		status = fsimg_verify(reader, 0, &report, &error);
	EXPECT(status == FSIMG_ECORRUPT && !report.complete &&
	    strstr(error.message, "truncated") != NULL,
	    "verification fails as truncated (status %d, \"%s\")", (int)status,
	    error.message);
	fsimg_reader_free(reader);
	fsimg_io_free(v.io);
	fsimg_ctx_free(v.ctx);
	gen_tree_free(&tree);
}

/* ---- Mutation matrix: inodes ---- */

/* A compact and two extended inodes in one volume, MTIME set. */
static void
test_compact_and_extended_inodes_deliver_owners_and_times(void)
{
	static const struct {
		const char	*path;
		bool		 extended;
		uint32_t	 uid, gid;
		int64_t		 time;		/* seconds, or the word */
		uint32_t	 nsec;
	} want[] = {
		{ "/c", false, 65535, 65535, 86400, 0 },
		{ "/e1", true, 65536, 4000000000u, -86401, 999999999 },
		{ "/e2", true, 4294967295u, 70000, 4102444800, 1 },
		{ "/e3", true, 0, 0, -INT64_C(5000000000), 0 },
	};
	gen_dirent_t kids[ARRAY_SIZE(want)];
	gen_tree_t tree;
	listing_t *list;

	tree_new(&tree, 9, 4);
	for (size_t i = 0; i < ARRAY_SIZE(want); i++) {
		gen_inode_t proto = file_proto(LAYOUT_FLAT);

		proto.extended = want[i].extended;
		proto.uid = want[i].uid;
		proto.gid = want[i].gid;
		proto.time = want[i].time;
		proto.time_nsec = want[i].nsec;
		kids[i] = (gen_dirent_t){ want[i].path + 1,
		    gen_tree_add(&tree, &proto, NULL, 0), T_FILE };
	}
	gen_tree_dir(&tree, 0, 0, kids, ARRAY_SIZE(kids));
	gen_tree_set_compat(&tree, 1 | 2);
	list = walk_ok("compact and extended inodes together", &tree);
	for (size_t i = 0; i < ARRAY_SIZE(want); i++) {
		const node_t *node = find_node(list, want[i].path);

		EXPECT(node != NULL && node->meta.uid == want[i].uid &&
		    node->meta.gid == want[i].gid &&
		    node->meta.modified.seconds == want[i].time &&
		    node->meta.modified.nanoseconds == want[i].nsec,
		    "%s: owners %u:%u, time %lld.%09u", want[i].path,
		    node != NULL ? node->meta.uid : 0,
		    node != NULL ? node->meta.gid : 0,
		    node != NULL ? (long long)node->meta.modified.seconds : 0,
		    node != NULL ? node->meta.modified.nanoseconds : 0);
	}
	listing_free(list);
	gen_tree_free(&tree);
}

/* The sizes between the header size and a block, in both layouts. */
static void
test_sizes_31_and_32_are_read_byte_for_byte(void)
{
	static const size_t sizes[] = { 31, 32 };
	static const unsigned layouts[] = { LAYOUT_FLAT, LAYOUT_FLAT_INLINE };

	for (size_t s = 0; s < ARRAY_SIZE(sizes); s++) {
		for (size_t l = 0; l < ARRAY_SIZE(layouts); l++) {
			gen_tree_t tree;
			listing_t *list;
			const node_t *node;

			tree_new(&tree, 9, 4);
			root_names(&tree, "f", add_file(&tree, layouts[l],
			    sizes[s], 9), T_FILE);
			list = walk_ok("size 31 or 32", &tree);
			node = find_node(list, "/f");
			EXPECT(node != NULL && node->size == sizes[s] &&
			    pattern_mismatch(node, 9) == sizes[s],
			    "layout %u size %zu: every byte read back",
			    layouts[l], sizes[s]);
			listing_free(list);
			gen_tree_free(&tree);
		}
	}
}

/* A child inode with one invalid field stops the walk at that child. */
static void
test_a_child_inode_with_an_invalid_field_is_refused_at_the_walk(void)
{
	static const struct {
		const char	*name;
		uint16_t	 mode;
		bool		 extended;
		uint32_t	 time_nsec;
		unsigned	 layout_bits;	/* patched format bits 1 to 3 */
		uint16_t	 format_extra;
		const char	*phrase;
	} cases[] = {
		{ "layout 5", MODE_FILE, false, 0, 5, 0, "inode data layout" },
		{ "layout 6", MODE_FILE, false, 0, 6, 0, "inode data layout" },
		{ "layout 7", MODE_FILE, false, 0, 7, 0, "inode data layout" },
		{ "format bit 5", MODE_FILE, false, 0, 0, 0x20, "inode format" },
		{ "format bit 15", MODE_FILE, true, 0, 0, 0x8000,
		    "inode format" },
		{ "nanoseconds 10^9", MODE_FILE, true, 1000000000, 0, 0,
		    "inode nanoseconds" },
		{ "mode type 030000", 030644, false, 0, 0, 0,
		    "inode file type" },
		{ "mode type 0", 0644, false, 0, 0, 0, "inode file type" },
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		gen_inode_t proto = inode_proto(cases[i].mode, LAYOUT_FLAT);
		gen_tree_t tree;
		uint64_t nid;
		uint64_t at;

		proto.extended = cases[i].extended;
		proto.time_nsec = cases[i].time_nsec;
		proto.format_extra = cases[i].format_extra;
		tree_new(&tree, 9, 4);
		nid = gen_tree_add(&tree, &proto, NULL, 0);
		root_names(&tree, "n", nid, T_UNKNOWN);
		if (cases[i].layout_bits != 0) {
			at = gen_inode_offset(&tree.image, nid);
			gen_put(&tree.image, at, (cases[i].layout_bits << 1) |
			    (cases[i].extended ? 1 : 0), 2);
		}
		check_refused(cases[i].name, &tree, FSIMG_ECORRUPT,
		    cases[i].phrase);
		gen_tree_free(&tree);
	}
}

/* ---- Mutation matrix: directories ---- */

/* A root naming "a_b" and "c" in a stored block, whose bytes the caller edits. */
static uint64_t
patched_root(gen_tree_t *tree)
{
	uint64_t file = add_file(tree, LAYOUT_FLAT_INLINE, 3, 1);
	uint64_t at = (uint64_t)tree->data_next << tree->blkszbits;
	gen_dirent_t kids[2] = { { "a_b", file, T_FILE },
	    { "c", file, T_FILE } };

	gen_tree_dir(tree, 0, 0, kids, 2);
	return (at);
}

/*
 * Records are ".", "..", "a_b", "c": 12 bytes each, names after 48, packed
 * one after the other, so "a_b" starts at 51 and "c" at 54.
 */
enum {
	RECORD_BYTES = 12,
	NAMES_AT = 48,
	A_B_NAME_AT = NAMES_AT + 1 + 2
};

static void
test_a_stored_directory_block_with_a_bad_field_is_refused(void)
{
	static const struct {
		const char	*name;
		unsigned	 record;
		unsigned	 field_at;	/* offset in the record */
		unsigned	 width;
		uint64_t	 value;
		const char	*phrase;
	} cases[] = {
		{ "first name offset 0", 0, 8, 2, 0, "directory entry count" },
		{ "first name offset 13", 0, 8, 2, 13, "directory entry count" },
		{ "first name offset past the block", 0, 8, 2, 12 * 400,
		    "directory entry count" },
		{ "second name offset below the first", 1, 8, 2, 12,
		    "not increasing" },
		{ "name offsets repeat", 3, 8, 2, A_B_NAME_AT,
		    "not increasing" },
		{ "last name offset past the block", 3, 8, 2, 4000,
		    "directory name offset" },
		{ "name type 8", 2, 10, 1, 8, "directory file type" },
		{ "name type 255", 3, 10, 1, 255, "directory file type" },
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		gen_tree_t tree;
		uint64_t at;

		tree_new(&tree, 9, 4);
		at = patched_root(&tree);
		gen_put(&tree.image, at + cases[i].record * RECORD_BYTES +
		    cases[i].field_at, cases[i].value, cases[i].width);
		check_refused(cases[i].name, &tree, FSIMG_ECORRUPT,
		    cases[i].phrase);
		gen_tree_free(&tree);
	}
}

static void
test_a_nul_or_slash_inside_a_stored_name_is_refused(void)
{
	static const unsigned char bytes[] = { 0, '/' };

	for (size_t i = 0; i < ARRAY_SIZE(bytes); i++) {
		gen_tree_t tree;
		uint64_t at;

		tree_new(&tree, 9, 4);
		at = patched_root(&tree);
		/* The "_" of "a_b", which is not the last name. */
		gen_put(&tree.image, at + A_B_NAME_AT + 1, bytes[i], 1);
		check_refused(bytes[i] == 0 ? "NUL in a name" :
		    "slash in a name", &tree, FSIMG_ECORRUPT,
		    "directory name byte");
		gen_tree_free(&tree);
	}
}

/* A chain of directories named "d", each holding the next. */
static void
test_a_deep_tree_is_listed_to_its_leaf(void)
{
	enum { DEPTH = 100 };
	char path[2 * DEPTH + 3];
	uint64_t nids[DEPTH + 1];
	gen_tree_t tree;
	listing_t *list;
	uint64_t leaf;

	tree_new(&tree, 9, DEPTH + 8);
	nids[0] = 0;
	for (size_t i = 1; i <= DEPTH; i++)
		nids[i] = gen_tree_slot(&tree, GEN_COMPACT_BYTES, 0);
	leaf = add_file(&tree, LAYOUT_FLAT_INLINE, 5, 2);
	for (size_t i = 0; i <= DEPTH; i++) {
		gen_dirent_t child = i < DEPTH ?
		    (gen_dirent_t){ "d", nids[i + 1], T_DIR } :
		    (gen_dirent_t){ "f", leaf, T_FILE };

		gen_tree_dir(&tree, nids[i], i == 0 ? 0 : nids[i - 1], &child, 1);
	}
	list = walk_ok("directories 100 deep", &tree);
	path[0] = '\0';
	for (size_t i = 0; i < DEPTH; i++)
		strcat(path, "/d");
	strcat(path, "/f");
	EXPECT(list->count == DEPTH + 1 && find_node(list, path) != NULL &&
	    find_node(list, path)->size == 5, "the leaf file is at depth %d",
	    DEPTH);
	listing_free(list);
	gen_tree_free(&tree);
}

/* Walk the image counting entries only; last receives the final path. */
static fsimg_status_t
count_entries(const gen_image_t *image, uint64_t memory_bytes, size_t *count,
    char *last, size_t last_bytes)
{
	const fsimg_entry_t *entry;
	fsimg_error_t error;
	fsimg_reader_t *reader = NULL;
	fsimg_status_t status;
	volume_t v;

	volume_start_with(&v, image->bytes, image->length, memory_bytes);
	status = fsimg_reader_open(v.ctx, v.io, NULL, &reader, &error);
	*count = 0;
	while (status == FSIMG_OK &&
	    (status = fsimg_reader_next(reader, &entry, &error)) == FSIMG_OK) {
		(*count)++;
		snprintf(last, last_bytes, "%s", entry->path);
	}
	fsimg_reader_free(reader);
	fsimg_io_free(v.io);
	fsimg_ctx_free(v.ctx);
	return (status);
}

/* One directory of 100000 names over many blocks, all one hard-linked file. */
static void
test_a_directory_of_100000_entries_lists_every_name(void)
{
	enum { ENTRIES = 100000, BLOCKS = 700, BITS = 12 };
	size_t block = (size_t)1 << BITS;
	gen_dirent_t *records = calloc(ENTRIES + 2, sizeof(*records));
	char (*names)[8] = calloc(ENTRIES, sizeof(*names));
	unsigned char *bytes = calloc(BLOCKS, block);
	gen_inode_t dir = inode_proto(MODE_DIR, LAYOUT_FLAT);
	char last[PATH_BYTES];
	fsimg_status_t status;
	gen_tree_t tree;
	size_t count, size;
	uint64_t file;

	CHECK(records != NULL && names != NULL && bytes != NULL);
	tree_new(&tree, BITS, BLOCKS + 8);
	file = add_file(&tree, LAYOUT_FLAT, 0, 0);
	records[0] = (gen_dirent_t){ ".", 0, T_DIR };
	records[1] = (gen_dirent_t){ "..", 0, T_DIR };
	for (size_t i = 0; i < ENTRIES; i++) {
		snprintf(names[i], sizeof(names[i]), "n%06zu", i);
		records[i + 2] = (gen_dirent_t){ names[i], file, T_FILE };
	}
	size = gen_dir_pack(bytes, BLOCKS * block, block, records, ENTRIES + 2);
	gen_tree_fill(&tree, 0, &dir, bytes, size);
	gen_tree_seal(&tree);
	EXPECT(count_entries(&tree.image, 256 * 1024 * 1024, &count, last,
	    sizeof(last)) ==
	    FSIMG_DONE, "the walk of 100000 entries reaches its end");
	EXPECT(count == ENTRIES && strcmp(last, "/n099999") == 0,
	    "%zu entries, the last \"%s\"", count, last);
	/*
	 * Known defect (TODO.md, "EROFS directory listing exceeds the
	 * minimum memory budget"): the design budget of 16 MiB is too small
	 * for this directory. When the listing moves to metadata scratch
	 * this expectation flips to FSIMG_DONE with all ENTRIES names; change
	 * it then and drop the larger budget above.
	 */
	status = count_entries(&tree.image, 16 * 1024 * 1024, &count, last,
	    sizeof(last));
	EXPECT(status == FSIMG_ELIMIT,
	    "at 16 MiB the walk fails with the memory limit (status %d)",
	    (int)status);
	gen_tree_free(&tree);
	free(bytes);
	free(names);
	free(records);
}

/*
 * root -> a -> b -> c, where c lists a, its grandparent. Unlike a listing of
 * the root or of the parent, c's entry names neither c nor b, so it is
 * admitted; the descent into a from c then finds a's ".." naming the root
 * instead of c, and the parent rule refuses it at depth 3 with no root
 * involved.
 */
static void
test_a_cycle_to_a_grandparent_below_the_root_is_corrupt(void)
{
	gen_dirent_t root_kids[1], a_kids[1], b_kids[1], c_kids[1];
	gen_tree_t tree;
	uint64_t a, b, c;

	tree_new(&tree, 9, 8);
	a = gen_tree_slot(&tree, GEN_COMPACT_BYTES, 0);
	b = gen_tree_slot(&tree, GEN_COMPACT_BYTES, 0);
	c = gen_tree_slot(&tree, GEN_COMPACT_BYTES, 0);
	root_kids[0] = (gen_dirent_t){ "a", a, T_DIR };
	a_kids[0] = (gen_dirent_t){ "b", b, T_DIR };
	b_kids[0] = (gen_dirent_t){ "c", c, T_DIR };
	c_kids[0] = (gen_dirent_t){ "up", a, T_DIR };
	gen_tree_dir(&tree, 0, 0, root_kids, 1);
	gen_tree_dir(&tree, a, 0, a_kids, 1);
	gen_tree_dir(&tree, b, a, b_kids, 1);
	gen_tree_dir(&tree, c, b, c_kids, 1);
	check_refused("c lists the grandparent a", &tree, FSIMG_ECORRUPT,
	    "dot entry breaks the parent rule");
	gen_tree_free(&tree);
}

/* ---- Fuzz seeds ---- */

/* A volume with a directory, stored and inline files and a link. */
static void
seed_tree(gen_tree_t *tree, unsigned blkszbits)
{
	gen_inode_t link = inode_proto(MODE_LINK, LAYOUT_FLAT_INLINE);
	gen_dirent_t root_kids[4], inside[1];
	uint64_t sub, inner, stored, inlined, symlink;
	size_t block = (size_t)1 << blkszbits;

	tree_new(tree, blkszbits, 8);
	sub = gen_tree_slot(tree, GEN_COMPACT_BYTES, 0);
	inner = add_file(tree, LAYOUT_FLAT_INLINE, 40, 3);
	stored = add_file(tree, LAYOUT_FLAT, block + 100, 1);
	inlined = add_file(tree, LAYOUT_FLAT_INLINE, 70, 2);
	link.layout_word = UINT32_C(0xffffffff);
	symlink = gen_tree_add(tree, &link, "target/of/the/link", 18);
	inside[0] = (gen_dirent_t){ "inner", inner, T_FILE };
	root_kids[0] = (gen_dirent_t){ "dir", sub, T_DIR };
	root_kids[1] = (gen_dirent_t){ "inline", inlined, T_FILE };
	root_kids[2] = (gen_dirent_t){ "link", symlink, T_LINK };
	root_kids[3] = (gen_dirent_t){ "stored", stored, T_FILE };
	gen_tree_dir(tree, 0, 0, root_kids, 4);
	gen_tree_dir(tree, sub, 0, inside, 1);
}

static void
write_seed(const char *directory, const char *name, gen_tree_t *tree)
{
	char path[1024];
	FILE *file;

	gen_tree_seal(tree);
	CHECK(snprintf(path, sizeof(path), "%s/%s", directory, name) <
	    (int)sizeof(path));
	file = fopen(path, "wb");
	CHECK(file != NULL);
	CHECK(fwrite(tree->image.bytes, 1, tree->image.length, file) ==
	    tree->image.length);
	CHECK(fclose(file) == 0);
}

/* --seed DIRECTORY: small volumes for the fuzz corpus. */
static void
emit_seeds(const char *directory)
{
	gen_tree_t tree;

	seed_tree(&tree, 9);
	write_seed(directory, "tree-512.seed", &tree);
	gen_tree_free(&tree);
	seed_tree(&tree, 12);
	write_seed(directory, "tree-4096.seed", &tree);
	gen_tree_free(&tree);
	tree_new(&tree, 9, 2);
	gen_tree_dir(&tree, 0, 0, NULL, 0);
	write_seed(directory, "empty-root.seed", &tree);
	gen_tree_free(&tree);
}

int
main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--seed") == 0) {
		emit_seeds(argv[2]);
		return (failures == 0 ? 0 : 1);
	}
	test_crc32c_oracle_matches_the_published_check_value();

	test_super_probe_classifies_each_case();
	test_magic_matches_only_the_magic_bytes();
	test_super_fields_are_decoded_from_their_offsets();
	test_filter_byte_nonzero_disables_the_name_filter();
	test_name_length_ends_at_the_first_nul_or_the_field_end();
	test_negative_epochs_decode_as_signed();
	test_root_nid_without_48bit_is_the_16_bit_field();
	test_48bit_widens_root_nid_and_block_count();
	test_reserved_superblock_bytes_are_reported_not_refused();

	test_extension_slots_past_the_given_length_are_corrupt();
	test_region_decode_applies_the_rules_beyond_the_base_superblock();
	test_metabox_nid_is_read_from_the_first_slot();
	test_metabox_nid_with_bit_63_is_corrupt();
	test_metabox_slot_reserved_word_is_reported();
	test_slot_bytes_are_ignored_without_metabox();

	test_real_checksummed_regions_are_accepted();
	test_flipped_bit_inside_the_covered_range_is_corrupt();
	test_flipped_checksum_field_is_corrupt();
	test_flipped_bit_just_past_the_covered_range_is_accepted();
	test_region_shorter_than_the_covered_range_is_corrupt();
	test_wrong_checksum_without_sb_chksum_is_accepted();
	test_super_checksum_equals_the_independent_crc();
	test_super_checksum_covers_the_range_of_every_block_size();
	test_super_checksum_refuses_block_sizes_outside_9_to_12();

	test_each_configuration_payload_is_validated_and_decoded();
	test_records_follow_the_slots_in_increasing_algorithm_order();
	test_without_compr_cfgs_the_word_is_lz4_max_distance();
	test_algorithm_mask_bits_above_3_are_unsupported();
	test_record_past_byte_3072_is_unsupported();
	test_record_past_a_shorter_length_is_corrupt();
	test_real_four_record_configuration_decodes();

	test_inode_decode_classifies_each_case();
	test_special_files_refuse_start_block_high_bits();
	test_inode_of_length_0_is_corrupt();
	test_fixed_bytes_follow_format_bit_0_only();
	test_format_bits_5_to_15_are_each_corrupt();
	test_each_mode_type_selects_its_kind();
	test_other_mode_types_are_corrupt();
	test_permissions_are_the_low_12_bits_of_the_mode();
	test_compact_inode_fields_are_decoded();
	test_compact_time_is_epoch_plus_the_unsigned_word();
	test_extended_inode_fields_are_decoded();
	test_each_extended_reserved_byte_nonzero_is_corrupt();
	test_48bit_inode_start_block_bits_are_reported();
	test_directory_with_bit_4_omits_dot();
	test_device_numbers_decode_by_the_linux_bit_layout();

	test_chunk_format_reserved_bits_7_to_15_are_each_corrupt();
	test_chunk_format_48bit_bit_needs_the_48bit_layout();
	test_chunk_format_48bit_bit_needs_8_byte_entries();
	test_chunk_format_sets_size_and_entry_width();
	test_chunk_entries_decode_to_blocks_and_holes();

	test_directory_block_shorter_than_one_entry_is_corrupt();
	test_first_name_offset_defines_the_entry_count();
	test_first_name_offset_must_be_a_nonzero_multiple_of_12_below_the_length();
	test_entry_fields_and_name_spans_are_decoded();
	test_last_name_ends_at_the_valid_length_or_the_first_nul();
	test_name_offsets_must_increase_strictly();
	test_name_offset_inside_the_entry_array_is_corrupt();
	test_name_offset_past_the_valid_length_is_corrupt();
	test_empty_last_name_is_corrupt();
	test_names_longer_than_255_bytes_are_corrupt();
	test_slash_or_nul_in_a_name_is_corrupt();
	test_file_types_0_to_7_are_accepted_and_8_is_corrupt();
	test_entry_index_past_the_count_is_invalid();
	test_real_root_directory_block_decodes_to_the_dumped_names();

	test_attribute_header_must_fit_the_region();
	test_attribute_header_reserved_bytes_are_not_required_zero();
	test_shared_ids_are_read_from_offset_12();
	test_shared_id_past_the_shared_count_is_false_and_leaves_the_output();
	test_shared_entry_offset_is_the_block_plus_4_per_id();
	test_attribute_entries_are_validated_and_decoded();
	test_attribute_entry_shorter_than_4_bytes_is_corrupt();
	test_prefix_record_validation();

	test_xxh32_matches_xxhsum_at_stripe_boundary_lengths();
	test_xxh32_matches_the_published_string_values();
	test_xxh32_gives_one_value_for_every_split_of_a_message();
	test_filter_bit_hashes_infix_and_suffix_with_the_prefix_seed();
	test_name_filter_of_real_inodes_is_the_complement_of_the_attribute_bits();
	test_real_long_prefix_records_decode();

	test_generator_crc32c_matches_the_published_check_value();
	test_probe_needs_the_magic_inside_the_prefix();
	test_probe_claims_a_full_prefix_and_names_erofs();
	test_probe_with_a_short_base_superblock_is_corrupt();
	test_probe_classifies_the_base_superblock();

	test_open_accepts_the_minimal_volume();
	test_open_reports_geometry_uuid_and_the_format_values();
	test_open_reports_a_negative_epoch_as_signed_seconds();
	test_open_accepts_every_block_size_with_a_correct_checksum();
	test_open_refuses_a_flipped_bit_inside_the_checksum_range();
	test_open_accepts_a_flipped_bit_just_past_the_checksum_range();
	test_open_does_not_check_the_checksum_without_sb_chksum();
	test_open_refuses_a_checksum_range_cut_short_by_the_input();
	test_open_refuses_an_unknown_incompatible_bit();
	test_open_refuses_nonzero_extra_devices_before_the_checksum();
	test_open_refuses_inconsistent_superblock_fields();
	test_open_refuses_each_unimplemented_incompatible_bit_by_name();
	test_open_pairs_long_prefixes_with_a_plain_table_or_a_packed_inode();
	test_open_tolerates_unknown_compatible_bits_and_reserved_bytes();
	test_open_refuses_a_wrong_magic_unless_identity_is_assumed();
	test_forced_open_still_applies_every_other_superblock_rule();
	test_open_reports_an_allocation_failure_and_keeps_no_memory();
	test_open_refuses_a_volume_shorter_than_the_magic_either_way();
	test_open_refuses_extension_slots_past_a_short_volume();
	test_open_reports_a_truncated_volume_at_its_declared_length();
	test_open_takes_a_zero_block_count_as_the_input_length();
	test_open_refuses_a_block_count_ending_inside_the_superblock();
	test_open_refuses_a_root_outside_the_address_domain();
	test_open_refuses_a_root_that_is_a_regular_file();
	test_open_refuses_a_root_with_a_reserved_format_bit();
	test_open_delivers_the_name_as_a_label_only_when_it_is_printable();
	test_open_failures_return_all_memory();

	test_inode_load_refuses_unaddressable_nids();
	test_inode_load_in_the_last_slot_is_compact_only();
	test_inode_load_allows_a_fixed_part_across_a_block_boundary();
	test_inode_load_reports_where_the_inode_is_and_how_long();
	test_inode_load_accepts_a_tail_ending_at_the_block_end_only();
	test_inode_load_accepts_a_tail_starting_at_the_next_block();
	test_inode_load_refuses_a_body_past_the_domain();
	test_body_read_returns_the_tail_and_bounds_its_range();
	test_meta_read_bounds_the_space_and_the_input();
	test_inode_load_checks_the_layout_against_the_volume_features();

	test_every_boundary_size_is_read_byte_for_byte();
	test_flat_extent_ending_at_the_volume_end_is_read();
	test_flat_extent_one_block_past_the_volume_end_is_corrupt();
	test_flat_inline_file_under_a_block_ignores_its_start_word();
	test_flat_file_with_content_and_start_word_ffffffff_is_corrupt();
	test_flat_inline_with_a_full_block_reads_its_start_word();
	test_empty_file_ignores_its_start_word_in_either_layout();
	test_inline_tail_ending_at_the_block_end_is_read();
	test_inline_tail_crossing_the_block_end_is_corrupt();
	test_directory_over_several_blocks_lists_every_name_in_order();
	test_names_out_of_order_across_a_block_boundary_are_corrupt();
	test_flat_inline_directories_list_their_stored_and_inline_blocks();
	test_names_out_of_order_into_the_inline_tail_are_corrupt();
	test_name_ending_at_the_end_of_a_full_block_is_read_whole();
	test_directory_of_255_byte_names_lists_across_its_block_boundaries();
	test_255_byte_names_out_of_order_only_at_a_block_boundary_are_corrupt();
	test_root_directory_records_are_checked();
	test_dot_after_a_name_that_sorts_before_it_is_accepted();
	test_subdirectory_dot_entries_must_name_it_and_its_parent();
	test_a_directory_named_from_two_parents_fails_the_second_descent();
	test_a_cycle_back_through_an_ancestor_at_depth_2_is_corrupt();
	test_two_names_for_one_directory_in_one_parent_are_both_listed();
	test_names_that_are_not_portable_utf8_are_omitted_not_refused();
	test_stored_and_inline_content_and_hard_links_read_back();
	test_reads_at_offsets_cross_from_stored_blocks_into_the_inline_tail();
	test_symbolic_link_targets_are_checked_and_delivered();
	test_chunk_files_read_byte_for_byte_at_every_size_and_shape();
	test_chunk_hole_resolves_to_a_zero_extent_of_the_chunks_length();
	test_chunks_naming_one_block_read_correctly_in_both_files();
	test_chunk_files_that_break_a_rule_are_corrupt_naming_it();
	test_chunk_index_running_past_the_domain_is_refused_when_the_inode_loads();
	test_a_file_of_one_hole_chunk_reads_its_last_byte_as_zero();
	test_a_chunk_based_directory_lists_its_entries();
	test_salvage_keeps_a_chunked_files_prefix_through_its_hole();
	test_a_chunk_based_symbolic_link_delivers_its_target();
	test_attribute_regions_set_the_extra_data_mark_or_are_refused();
	test_device_numbers_decode_the_linux_32_bit_encoding();
	test_fifos_and_sockets_have_no_size_and_no_device_number();
	test_owner_and_permission_bits_are_delivered();
	test_the_stored_time_is_modified_with_mtime_and_changed_without();
	test_a_compact_inode_time_is_the_stored_word_after_the_epoch();
	test_open_accepts_each_compatible_bit_alone();
	test_open_refuses_every_unknown_incompatible_bit();
	test_open_refuses_each_illegal_block_size();
	test_open_accepts_extension_slots_without_a_metadata_box();
	test_a_block_count_past_the_input_lists_what_lies_in_the_input();
	test_compact_and_extended_inodes_deliver_owners_and_times();
	test_sizes_31_and_32_are_read_byte_for_byte();
	test_a_child_inode_with_an_invalid_field_is_refused_at_the_walk();
	test_a_stored_directory_block_with_a_bad_field_is_refused();
	test_a_nul_or_slash_inside_a_stored_name_is_refused();
	test_a_deep_tree_is_listed_to_its_leaf();
	test_a_directory_of_100000_entries_lists_every_name();
	test_a_cycle_to_a_grandparent_below_the_root_is_corrupt();

	if (failures != 0)
		fprintf(stderr, "test/erofs.c: %d checks failed\n", failures);
	CHECK(failures == 0);
	return (0);
}
