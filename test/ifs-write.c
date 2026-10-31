/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "fsimg.h"
#include "format.h"
#include "fsimg-endian.h"
#include "internal.h"
#include "meta.h"
#include "test.h"
#include "ifs/write-ifs.h"

/*
 * ImageFS writer tests. The fixture builds its writers by name through the
 * registry; test_tampered_rows() uses the row functions of the driver's
 * write-ifs.h. The checker below uses nothing from lib/formats/ifs/.
 *
 * Images are checked three ways: by fsimg's reader, by its verifier, and by
 * the independent checker below, written from the format design
 * (specs/design-ifs.md) with its own offsets and sharing no code with the
 * driver. The checker decodes the header, the mountpoint, every record and
 * every file's data and sums the image. In strict mode it also requires the
 * writer profile's layout: the root record first, inode numbers in record
 * order, an explicit directory record before every child, every record its
 * kind's minimum padded to 4 bytes, files in record order each on the next
 * 4-byte boundary, and the trailer last. That gives every byte of the image
 * a checked value: each one the layout gives no meaning must be zero, and
 * images are emitted over a sink poisoned with 0xa5, so a stray or stale
 * byte anywhere fails the test. Lenient mode decodes native images, whose
 * free choices differ.
 */
#define HEADER		0x58u
#define T_TYPE		0170000u
#define T_DIR		0040000u
#define T_FILE		0100000u
#define T_LINK		0120000u
#define T_FIFO		0010000u
#define POISON		0xa5u
#define PATH_BYTES	1024u
#define IMAGE_MAX	UINT64_C(0xfffffffc)
#define DIGEST_SEED	UINT32_C(2166136261)
#define EPOCH		1700000000
#define OWNED_TIME	1234567890

/*
 * The independent checker.
 */

/* One record as the checker decodes it. path is the stored path, without a
 * leading slash; target is a link's target. size is a file's length or a
 * link's target length, and digest covers those bytes.
 */
typedef struct chk_entry {
	char		*path;
	char		*target;
	uint32_t	 offset;
	uint32_t	 length;
	uint32_t	 inode;
	uint32_t	 mode;
	uint32_t	 uid;
	uint32_t	 gid;
	uint32_t	 mtime;
	uint32_t	 data;
	uint32_t	 size;
	uint32_t	 digest;
} chk_entry_t;

typedef struct chk {
	const unsigned char *image;
	uint64_t	 length;
	bool		 big;
	bool		 strict;
	bool		 quiet;
	uint32_t	 image_size;
	uint32_t	 table_end;	/* hdr_dir_size */
	uint32_t	 dir_offset;
	char		 mountpoint[PATH_BYTES + 1];
	chk_entry_t	*entries;
	size_t		 count;
	size_t		 capacity;
	unsigned	 errors;
} chk_t;

#define CHK(c, cond, what) do {						\
	if (!(cond))							\
		chk_fail((c), (what), __LINE__);			\
} while (0)

static void
chk_fail(chk_t *c, const char *what, int line)
{
	if (!c->quiet)
		fprintf(stderr, "checker line %d: %s\n", line, what);
	c->errors++;
}

static uint32_t
get16(const chk_t *c, const unsigned char *p)
{
	return (c->big ? (uint32_t)p[0] << 8 | p[1] :
	    (uint32_t)p[1] << 8 | p[0]);
}

static uint32_t
get32(const chk_t *c, const unsigned char *p)
{
	return (c->big ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	    (uint32_t)p[2] << 8 | p[3] : (uint32_t)p[3] << 24 |
	    (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]);
}

static uint64_t
round4(uint64_t value)
{
	return ((value + 3) & ~UINT64_C(3));
}

static bool
all_zero(const unsigned char *p, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++)
		if (p[i] != 0)
			return (false);
	return (true);
}

/* FNV-1a over n bytes. */
static uint32_t
digest(uint32_t h, const unsigned char *p, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++)
		h = (h ^ p[i]) * UINT32_C(16777619);
	return (h);
}

/* The POSIX cksum CRC: polynomial 0x04c11db7 over the bytes, most
 * significant bit first, then over the length's bytes, least significant
 * first, complemented.
 */
static uint32_t
crc_update(uint32_t crc, const unsigned char *p, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++) {
		crc ^= (uint32_t)p[i] << 24;
		for (unsigned bit = 0; bit < 8; bit++)
			crc = (crc & 0x80000000u) != 0 ?
			    (crc << 1) ^ 0x04c11db7u : crc << 1;
	}
	return (crc);
}

static uint32_t
crc_finish(uint32_t crc, uint64_t length)
{
	unsigned char b;

	for (; length != 0; length >>= 8) {
		b = (unsigned char)(length & 0xff);
		crc = crc_update(crc, &b, 1);
	}
	return (~crc);
}

static char *
copy_text(const unsigned char *bytes, size_t length)
{
	char *text;

	text = malloc(length + 1);
	CHECK(text != NULL);
	bcopy(bytes, text, length);
	text[length] = '\0';
	return (text);
}

/* Decode the record at offset at, which must end by end, into e. Returns
 * false when the record cannot be walked past.
 */
static bool
chk_record(chk_t *c, uint32_t at, uint32_t end, chk_entry_t *e)
{
	const unsigned char *r = c->image + at, *nul, *target;
	uint64_t minimum;
	uint32_t length, path_at, sym_offset;
	size_t path_length;

	bzero(e, sizeof(*e));
	length = get16(c, r);
	CHK(c, length >= 24 && length % 4 == 0 && length <= end - at,
	    "record length");
	if (length < 24 || length > end - at)
		return (false);
	e->offset = at;
	e->length = length;
	CHK(c, get16(c, r + 2) == 0, "extended-attribute offset is zero");
	e->inode = get32(c, r + 4);
	CHK(c, e->inode != 0 && e->inode >> 28 == 0,
	    "inode word without flags in a nonbootable image");
	e->mode = get32(c, r + 8);
	e->gid = get32(c, r + 0x0c);
	e->uid = get32(c, r + 0x10);
	e->mtime = get32(c, r + 0x14);
	CHK(c, e->mode <= 0177777, "mode bits");
	switch (e->mode & T_TYPE) {
	case T_DIR:
		path_at = 0x18;
		break;
	case T_LINK:
		path_at = 0x1c;
		break;
	case T_FILE:
	case T_FIFO:
		path_at = 0x20;
		break;
	default:
		CHK(c, false, "record type");
		return (false);
	}
	CHK(c, path_at < length, "record holds its path");
	if (path_at >= length)
		return (false);
	nul = memchr(r + path_at, '\0', length - path_at);
	CHK(c, nul != NULL, "record path is terminated");
	if (nul == NULL)
		return (false);
	path_length = (size_t)(nul - (r + path_at));
	CHK(c, path_length <= PATH_BYTES, "record path length");
	e->path = copy_text(r + path_at, path_length);
	minimum = (uint64_t)path_at + path_length + 1;
	e->digest = DIGEST_SEED;
	switch (e->mode & T_TYPE) {
	case T_FILE:
		e->data = get32(c, r + 0x18);
		e->size = get32(c, r + 0x1c);
		break;
	case T_LINK:
		sym_offset = get16(c, r + 0x18);
		e->size = get16(c, r + 0x1a);
		CHK(c, sym_offset == path_length + 1,
		    "link target follows the path's NUL");
		CHK(c, e->size >= 1 && e->size <= PATH_BYTES,
		    "link target length");
		minimum += (uint64_t)e->size + 1;
		CHK(c, minimum <= length, "record holds its link target");
		if (minimum > length)
			return (false);
		target = r + path_at + path_length + 1;
		CHK(c, memchr(target, '\0', e->size) == NULL &&
		    target[e->size] == '\0', "link target and its NUL");
		e->target = copy_text(target, e->size);
		e->digest = digest(DIGEST_SEED, target, e->size);
		break;
	case T_FIFO:
		CHK(c, get32(c, r + 0x18) == 0 && get32(c, r + 0x1c) == 0,
		    "FIFO words are zero");
		break;
	default:
		break;
	}
	if (c->strict)
		CHK(c, length == round4(minimum),
		    "record is its minimum padded to 4 bytes");
	CHK(c, all_zero(r + minimum, length - minimum), "record padding is zero");
	return (true);
}

/* The length of the parent of a path's first length bytes: the offset of
 * the last slash before it, or zero for a top-level name.
 */
static size_t
prefix_parent(const char *path, size_t length)
{
	while (length > 0 && path[--length] != '/')
		continue;
	return (length);
}

/* The parent of a stored path: the bytes before its last slash, or the
 * root's empty path.
 */
static size_t
parent_length(const char *path)
{
	return (prefix_parent(path, strlen(path)));
}

/* The writer profile. Every directory has a record before its children,
 * which is also what makes the table hold no duplicate: a second record
 * with an earlier path would have to repeat it.
 */
static void
chk_profile(chk_t *c)
{
	const chk_entry_t *e, *root;
	uint64_t next, end;
	size_t parent;
	bool found;

	CHK(c, c->count >= 1, "the table holds the root");
	if (c->count == 0)
		return;
	root = &c->entries[0];
	CHK(c, root->path[0] == '\0' && root->mode == (T_DIR | 0755) &&
	    root->uid == 0 && root->gid == 0 && root->inode == 1,
	    "the root record comes first");
	for (size_t i = 1; i < c->count; i++) {
		e = &c->entries[i];
		CHK(c, e->inode == i + 1, "inodes follow the record order");
		CHK(c, e->path[0] != '\0', "one root record");
		parent = parent_length(e->path);
		found = false;
		for (size_t j = 0; j < i; j++) {
			CHK(c, strcmp(c->entries[j].path, e->path) != 0,
			    "no path is stored twice");
			if ((c->entries[j].mode & T_TYPE) == T_DIR &&
			    strlen(c->entries[j].path) == parent &&
			    strncmp(c->entries[j].path, e->path, parent) == 0)
				found = true;
		}
		CHK(c, found, "a directory record precedes every child");
	}
	next = c->table_end;
	for (size_t i = 0; i < c->count; i++) {
		e = &c->entries[i];
		if ((e->mode & T_TYPE) != T_FILE)
			continue;
		CHK(c, e->data == next, "file data in record order, aligned");
		if (e->data != next || (uint64_t)e->data + e->size > c->length)
			return;
		end = (uint64_t)e->data + e->size;
		CHK(c, all_zero(c->image + end, round4(end) - end),
		    "the gap after file data is zero");
		next = round4(end);
	}
	CHK(c, next + 4 == c->image_size, "the trailer follows the data");
}

/* Decode and check the image of length bytes. strict applies the writer
 * profile; quiet keeps expected failures off stderr.
 */
static void
chk_image(chk_t *c, const unsigned char *image, uint64_t length, bool strict,
    bool quiet)
{
	chk_entry_t *grown, *e;
	const unsigned char *nul;
	uint32_t sum, at, end;
	size_t mlength;

	bzero(c, sizeof(*c));
	c->image = image;
	c->length = length;
	c->strict = strict;
	c->quiet = quiet;
	CHK(c, length >= HEADER + 12 && length <= IMAGE_MAX && length % 4 == 0,
	    "image length");
	if (length < HEADER + 12 || length > IMAGE_MAX)
		return;
	CHK(c, memcmp(image, "imagefs", 7) == 0, "signature");
	CHK(c, image[7] == 0x04 || image[7] == 0x05, "flags byte");
	c->big = image[7] == 0x05;
	c->image_size = get32(c, image + 0x08);
	c->table_end = get32(c, image + 0x0c);
	c->dir_offset = get32(c, image + 0x10);
	CHK(c, c->image_size == length, "image_size is the image's length");
	CHK(c, c->dir_offset % 4 == 0 && c->table_end % 4 == 0,
	    "aligned header sizes");
	CHK(c, c->dir_offset > HEADER && (uint64_t)c->dir_offset + 4 <=
	    c->table_end && (uint64_t)c->table_end + 4 <= c->image_size &&
	    c->image_size <= length, "header sizes in order");
	if (c->dir_offset <= HEADER || (uint64_t)c->dir_offset + 4 >
	    c->table_end || (uint64_t)c->table_end + 4 > c->image_size ||
	    c->image_size > length || c->dir_offset % 4 != 0)
		return;
	CHK(c, all_zero(image + 0x14, HEADER - 0x14),
	    "references, chain_paddr, spare words and mount flags are zero");

	nul = memchr(image + HEADER, '\0', c->dir_offset - HEADER);
	CHK(c, nul != NULL, "mountpoint is terminated");
	if (nul == NULL)
		return;
	mlength = (size_t)(nul - (image + HEADER));
	CHK(c, mlength <= PATH_BYTES, "mountpoint length");
	CHK(c, round4(HEADER + mlength + 1) == c->dir_offset,
	    "dir_offset is the mountpoint's NUL rounded up to 4");
	CHK(c, all_zero(nul, c->dir_offset - (HEADER + mlength)),
	    "mountpoint padding is zero");
	if (mlength <= PATH_BYTES) {
		bcopy(image + HEADER, c->mountpoint, mlength);
		c->mountpoint[mlength] = '\0';
	}

	end = c->table_end - 4;
	for (at = c->dir_offset; at < end;) {
		if (c->count == c->capacity) {
			c->capacity = c->capacity == 0 ? 64 : 2 * c->capacity;
			grown = realloc(c->entries, c->capacity *
			    sizeof(c->entries[0]));
			CHECK(grown != NULL);
			c->entries = grown;
		}
		e = &c->entries[c->count];
		if (!chk_record(c, at, end, e)) {
			free(e->path);
			free(e->target);
			return;
		}
		at += e->length;
		c->count++;
	}
	CHK(c, at == end && all_zero(image + end, 4), "the table terminator");

	for (size_t i = 0; i < c->count; i++) {
		e = &c->entries[i];
		if ((e->mode & T_TYPE) != T_FILE || e->size == 0)
			continue;
		CHK(c, e->data >= c->table_end && (uint64_t)e->data + e->size <=
		    c->image_size - 4, "file data lies in the data region");
		if (e->data >= c->table_end &&
		    (uint64_t)e->data + e->size <= c->image_size - 4)
			e->digest = digest(DIGEST_SEED, image + e->data, e->size);
	}
	sum = 0;
	for (uint64_t i = 0; i < c->image_size; i += 4)
		sum += get32(c, image + i);
	CHK(c, sum == 0, "the words of the image sum to zero");
	if (strict)
		chk_profile(c);
}

static void
chk_free(chk_t *c)
{
	for (size_t i = 0; i < c->count; i++) {
		free(c->entries[i].path);
		free(c->entries[i].target);
	}
	free(c->entries);
	c->entries = NULL;
	c->count = 0;
}

/* The entry of an image path, which starts with a slash. */
static const chk_entry_t *
chk_find(const chk_t *c, const char *path)
{
	for (size_t i = 0; i < c->count; i++)
		if (strcmp(c->entries[i].path, path + 1) == 0)
			return (&c->entries[i]);
	return (NULL);
}

/*
 * Fixtures.
 */

typedef struct fixture {
	fsimg_ctx_t	*ctx;
	fsimg_writer_t	*writer;
	fsimg_stats_t	 baseline;
	const chk_t	*native;	/* the native image a tree comes from */
	char		 directory[64];
	int		 parent;
	unsigned	 losses;
	uint64_t	 cancel_after;	/* cancel at this poll; zero never */
	uint64_t	 polls;
	bool		 changed;
	bool		 eof;		/* the provider ends early */
} fixture_t;

typedef struct memory_sink {
	unsigned char	*bytes;
	uint64_t	 length;
	uint64_t	 writes;
	size_t		 max_write;	/* a short write past this; zero none */
	uint64_t	 fail_at;	/* this write fails; zero none */
	bool		 fail_flush;
} memory_sink_t;

/* One fixture entry. source is a provider ID, or zero for the zero source.
 * Provider 3 serves "hello.txt", provider 4 lowercase letters, provider 9
 * a target holding a NUL byte, and providers 14 to 16 the absolute, ".."
 * and non-ASCII link targets of the qualification images; every other
 * provider serves a pattern of its ID and the offset.
 */
typedef struct spec {
	const char	*path;
	fsimg_entry_kind_t kind;
	uint64_t	 size;
	uint64_t	 source;
	const fsimg_metadata_t *metadata;
} spec_t;

#define NATIVE_SOURCE	(UINT64_C(1) << 32)
#define TEN		"0123456789"
#define HUNDRED		TEN TEN TEN TEN TEN TEN TEN TEN TEN TEN
#define N255		HUNDRED HUNDRED TEN TEN TEN TEN TEN "abcde"

static const fsimg_metadata_t owned_metadata = {
	.valid = FSIMG_META_OWNER | FSIMG_META_MODE | FSIMG_META_MODIFIED,
	.uid = 4294967295u,
	.gid = 65535,
	.mode = 07777,
	.modified = { .seconds = OWNED_TIME, .precision_ns = 1000000000,
	    .basis = FSIMG_TIME_UTC }
};

static const fsimg_metadata_t private_metadata = {
	.valid = FSIMG_META_OWNER | FSIMG_META_MODE,
	.uid = 65534,
	.gid = 0,
	.mode = 0
};

/* Every kind; files of 0 to 3 bytes, so data starts at every phase of a
 * word, and of 65537 bytes, more than one copy batch; links whose targets
 * start at odd offsets inside their records, one of 1024 bytes; a 255-byte
 * component; names differing only by case; a space and UTF-8; owner, mode
 * and time off the defaults on a file, a directory and a link; a zero
 * source.
 */
static const spec_t tree[] = {
	{ "/hello.txt", FSIMG_ENTRY_FILE, 33, 1, NULL },
	{ "/Hello.txt", FSIMG_ENTRY_FILE, 5, 12, NULL },
	{ "/empty", FSIMG_ENTRY_FILE, 0, 0, NULL },
	{ "/one", FSIMG_ENTRY_FILE, 1, 2, &owned_metadata },
	{ "/two", FSIMG_ENTRY_FILE, 2, 5, NULL },
	{ "/three", FSIMG_ENTRY_FILE, 3, 6, NULL },
	{ "/dir", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL },
	{ "/dir/sub", FSIMG_ENTRY_DIRECTORY, 0, 0, &owned_metadata },
	{ "/dir/sub/deep.bin", FSIMG_ENTRY_FILE, 5000, 7, NULL },
	{ "/dir/caf\xc3\xa9", FSIMG_ENTRY_FILE, 4, 8, &private_metadata },
	{ "/dir/" N255, FSIMG_ENTRY_FILE, 7, 10, NULL },
	{ "/dir/link", FSIMG_ENTRY_SYMLINK, 9, 3, NULL },
	{ "/dir/long-link", FSIMG_ENTRY_SYMLINK, 1024, 4, NULL },
	{ "/fifo", FSIMG_ENTRY_FIFO, 0, 0, &private_metadata },
	{ "/zero.bin", FSIMG_ENTRY_FILE, 70001, 0, NULL },
	{ "/big.bin", FSIMG_ENTRY_FILE, 65537, 11, NULL },
	{ "/link", FSIMG_ENTRY_SYMLINK, 9, 3, &owned_metadata },
	{ "/with space", FSIMG_ENTRY_FILE, 1, 13, NULL }
};
#define TREE_COUNT	(sizeof(tree) / sizeof(tree[0]))
/* The entries of the tree carrying owned_metadata's time. */
#define TREE_TIMED	3u

static unsigned char
content_byte(uint64_t id, uint64_t offset)
{
	static const char target[] = "hello.txt", bad[] = "a\0b";
	static const char absolute[] = "/proc/boot/hello.txt";
	static const char up[] = "../hello.txt", accented[] = "caf\xc3\xa9";

	switch (id) {
	case 3:
		return ((unsigned char)target[offset % 9]);
	case 4:
		return ((unsigned char)('a' + offset % 26));
	case 9:
		return ((unsigned char)bad[offset % 3]);
	case 14:
		return ((unsigned char)absolute[offset % 20]);
	case 15:
		return ((unsigned char)up[offset % 12]);
	case 16:
		return ((unsigned char)accented[offset % 5]);
	default:
		return ((unsigned char)(offset * 37 + id * 11 + (offset >> 13)));
	}
}

static uint32_t
expected_digest(uint64_t id, uint64_t size)
{
	unsigned char b;
	uint32_t h = DIGEST_SEED;

	for (uint64_t i = 0; i < size; i++) {
		b = id == 0 ? 0 : content_byte(id, i);
		h = digest(h, &b, 1);
	}
	return (h);
}

static fsimg_status_t
provider_validate(void *argument, uint64_t id FSIMG_UNUSED,
    fsimg_error_t *error FSIMG_UNUSED)
{
	fixture_t *fixture = argument;

	return (fixture->changed ? FSIMG_ESOURCE_CHANGED : FSIMG_OK);
}

/* Deliberately short reads exercise the source adapter's exact-copy loop;
 * with eof set the provider ends after 1000 bytes. A native source serves
 * the bytes of a native image's entry.
 */
static fsimg_status_t
provider_read(void *argument, uint64_t id, uint64_t offset,
    void *buffer, size_t length, size_t *transferred,
    fsimg_error_t *error FSIMG_UNUSED)
{
	fixture_t *fixture = argument;
	const chk_entry_t *e;
	unsigned char *bytes = buffer;

	if (fixture->eof && offset >= 1000) {
		*transferred = 0;
		return (FSIMG_OK);
	}
	if (length > 1000)
		length = 1000;
	if (id >= NATIVE_SOURCE) {
		e = &fixture->native->entries[id - NATIVE_SOURCE];
		CHECK(offset <= e->size && length <= e->size - offset);
		if ((e->mode & T_TYPE) == T_LINK)
			bcopy(e->target + offset, bytes, length);
		else
			bcopy(fixture->native->image + e->data + offset, bytes,
			    length);
	} else {
		for (size_t i = 0; i < length; i++)
			bytes[i] = content_byte(id, offset + i);
	}
	*transferred = length;
	return (FSIMG_OK);
}

static void
loss_report(void *argument, const char *path, uint32_t field FSIMG_UNUSED)
{
	fixture_t *fixture = argument;

	CHECK(path[0] == '/');
	fixture->losses++;
}

static bool
cancel_poll(void *argument)
{
	fixture_t *fixture = argument;

	return (fixture->cancel_after != 0 &&
	    ++fixture->polls >= fixture->cancel_after);
}

/* An ImageFS writer, constructed by name as any caller does. */
static fsimg_status_t
ifs_writer(fsimg_ctx_t *ctx, const fsimg_write_options_t *options,
    fsimg_writer_t **writer, fsimg_error_t *error)
{
	return (fsimg_writer_new(ctx, "ifs", options, writer, error));
}

/* Open a fixture. pages sizes the metadata cache; scratch false keeps all
 * metadata in memory.
 */
static void
fixture_open_with(fixture_t *fixture, const fsimg_write_options_t *write_options,
    uint32_t pages, bool scratch)
{
	fsimg_options_t options;
	fsimg_source_ops_t source_ops;

	bzero(fixture, sizeof(*fixture));
	snprintf(fixture->directory, sizeof(fixture->directory),
	    "ifs-write-XXXXXX");
	CHECK(mkdtemp(fixture->directory) != NULL);
	fixture->parent = open(fixture->directory,
	    O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fixture->parent != -1);
	fsimg_options_init(&options);
	options.memory_bytes = UINT64_C(64) * 1024 * 1024;
	options.scratch_directory_fd = scratch ? fixture->parent : -1;
	/* The writer's scratch streams stay open while the reader and its
	 * verifier add their own.
	 */
	options.max_open_files = 64;
	/* Depth 512, the deepest tree a 1024-byte path reaches, above the
	 * default 256; the qualification limits case needs it.
	 */
	options.max_depth = 512;
	options.loss = loss_report;
	options.loss_argument = fixture;
	options.cancelled = cancel_poll;
	options.cancel_argument = fixture;
	CHECK(fsimg_ctx_new(&options, &fixture->ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_test_meta_cache(fixture->ctx, pages, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &fixture->baseline, NULL) ==
	    FSIMG_OK);
	CHECK(ifs_writer(fixture->ctx, write_options, &fixture->writer,
	    NULL) == FSIMG_OK);
	fsimg_source_ops_init(&source_ops);
	source_ops.validate = provider_validate;
	source_ops.read_at = provider_read;
	CHECK(fsimg_writer_set_sources(fixture->writer, &source_ops, fixture,
	    NULL) == FSIMG_OK);
}

static void
fixture_open(fixture_t *fixture, const fsimg_write_options_t *write_options)
{
	fixture_open_with(fixture, write_options, 1, true);
}

/* Free a fixture's writer and context, require that they released every
 * resource, and remove its directory.
 */
static void
fixture_close(fixture_t *fixture)
{
	fsimg_stats_t stats;

	fsimg_writer_free(fixture->writer);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == fixture->baseline.memory_bytes);
	CHECK(stats.scratch_bytes == 0);
	CHECK(stats.descriptors == fixture->baseline.descriptors);
	fsimg_ctx_free(fixture->ctx);
	CHECK(close(fixture->parent) == 0);
	CHECK(rmdir(fixture->directory) == 0);
}

static fsimg_status_t
add_one(fixture_t *fixture, const spec_t *spec)
{
	fsimg_entry_t entry;
	fsimg_source_t source;

	bzero(&entry, sizeof(entry));
	entry.path = spec->path;
	entry.kind = spec->kind;
	entry.size = spec->size;
	if (spec->metadata != NULL)
		entry.metadata = *spec->metadata;
	source.kind = spec->source == 0 ? FSIMG_SOURCE_ZERO :
	    FSIMG_SOURCE_PROVIDER;
	source.id = spec->source;
	return (fsimg_writer_add(fixture->writer, &entry,
	    spec->kind == FSIMG_ENTRY_FILE || spec->kind == FSIMG_ENTRY_SYMLINK ?
	    &source : NULL, NULL));
}

static void
add_specs(fixture_t *fixture, const spec_t *specs, size_t count, bool reverse)
{
	for (size_t i = 0; i < count; i++)
		CHECK(add_one(fixture, &specs[reverse ? count - 1 - i : i]) ==
		    FSIMG_OK);
}

static fsimg_status_t
memory_size(void *argument, uint64_t *length, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_sink_t *sink = argument;

	*length = sink->length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_sink_t *sink = argument;

	CHECK(offset <= sink->length && length <= sink->length - offset);
	bcopy(sink->bytes + offset, buffer, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_write(void *argument, uint64_t offset, const void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_sink_t *sink = argument;

	*transferred = 0;
	sink->writes++;
	if (sink->writes == sink->fail_at)
		return (FSIMG_EIO);
	CHECK(offset <= sink->length && length <= sink->length - offset);
	if (sink->max_write != 0 && length > sink->max_write)
		length = sink->max_write;
	bcopy(buffer, sink->bytes + offset, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_flush(void *argument, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_sink_t *sink = argument;

	return (sink->fail_flush ? FSIMG_EIO : FSIMG_OK);
}

/* An I/O object over a sink. The writable one has no zero_range, so the
 * common emitter must overwrite the poison itself.
 */
static fsimg_status_t
memory_io(fsimg_ctx_t *ctx, memory_sink_t *sink, bool writable,
    fsimg_io_t **out)
{
	fsimg_io_ops_t ops;

	fsimg_io_ops_init(&ops);
	ops.get_size = memory_size;
	ops.read_at = memory_read;
	if (writable) {
		ops.write_at = memory_write;
		ops.flush = memory_flush;
	}
	*out = NULL;
	return (fsimg_io_from_callbacks(ctx, &ops, sink, out, NULL));
}

/* Plan, allocate a poisoned sink of the planned size and emit into it. */
static fsimg_status_t
emit_memory(fixture_t *fixture, memory_sink_t *sink, fsimg_error_t *error)
{
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_io_t *output = NULL;
	fsimg_error_t detail;
	fsimg_status_t status;

	bzero(sink, sizeof(*sink));
	status = fsimg_writer_plan(fixture->writer, &plan, &detail);
	if (status == FSIMG_OK) {
		CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
		sink->length = info.volume_bytes;
		sink->bytes = malloc((size_t)info.volume_bytes);
		CHECK(sink->bytes != NULL);
		memset(sink->bytes, POISON, (size_t)info.volume_bytes);
		status = memory_io(fixture->ctx, sink, true, &output);
		if (status == FSIMG_OK)
			status = fsimg_writer_emit(fixture->writer, output, &detail);
		fsimg_io_free(output);
	}
	/* Report an unexpected failure where the caller cannot see it. */
	if (error != NULL)
		*error = detail;
	else if (status != FSIMG_OK && status != FSIMG_ENOMEM &&
	    status != FSIMG_ECANCELLED && status != FSIMG_EIO)
		fprintf(stderr, "emit: %d %s\n", status, detail.message);
	return (status);
}

/* The plan status of specs under options, with no emission. */
static fsimg_status_t
plan_status(const fsimg_write_options_t *options, const spec_t *specs,
    size_t count, fsimg_plan_info_t *info)
{
	fixture_t fixture;
	const fsimg_plan_t *plan;
	fsimg_status_t status;

	fixture_open(&fixture, options);
	status = FSIMG_OK;
	for (size_t i = 0; status == FSIMG_OK && i < count; i++)
		status = add_one(&fixture, &specs[i]);
	if (status == FSIMG_OK)
		status = fsimg_writer_plan(fixture.writer, &plan, NULL);
	if (status == FSIMG_OK && info != NULL)
		CHECK(fsimg_plan_get_info(plan, info, NULL) == FSIMG_OK);
	fixture_close(&fixture);
	return (status);
}

/* Create an image of specs, check it strictly and leave the checker's
 * state in c.
 */
static void
build_checked(const fsimg_write_options_t *options, const spec_t *specs,
    size_t count, memory_sink_t *sink, chk_t *c, unsigned *losses)
{
	fixture_t fixture;

	fixture_open(&fixture, options);
	add_specs(&fixture, specs, count, false);
	CHECK(emit_memory(&fixture, sink, NULL) == FSIMG_OK);
	if (losses != NULL)
		*losses = fixture.losses;
	fixture_close(&fixture);
	chk_image(c, sink->bytes, sink->length, true, false);
	CHECK(c->errors == 0);
}

static void
reproducible_options(fsimg_write_options_t *options)
{
	fsimg_write_options_init(options);
	options->flags |= FSIMG_WRITE_REPRODUCIBLE;
	options->epoch = EPOCH;
}

/* The checker decoded a record for the spec's path, and it holds what the
 * spec asked for: the kind and, for a file or link, the size and contents;
 * the owner, mode and time of the spec's metadata, or else the defaults:
 *   owner	0
 *   mode	0644 for a file, 0755 for a directory, 0777 for a link or FIFO
 *   time	fallback, the time the root carries
 * In reproducible mode every time is the fallback. A mismatch names the
 * label (the image's case) and the path.
 */
static void
check_stored(const chk_t *c, const char *label, const spec_t *spec,
    uint32_t fallback, bool reproducible)
{
	const fsimg_metadata_t *m = spec->metadata;
	uint32_t type, permissions, uid, gid, mtime;
	const chk_entry_t *e;
	bool ok;

	e = chk_find(c, spec->path);
	if (e == NULL)
		fprintf(stderr, "%s %s: no record\n", label, spec->path);
	CHECK(e != NULL);

	switch (spec->kind) {
	case FSIMG_ENTRY_DIRECTORY:
		type = T_DIR;
		permissions = 0755;
		break;
	case FSIMG_ENTRY_FIFO:
		type = T_FIFO;
		permissions = 0777;
		break;
	case FSIMG_ENTRY_SYMLINK:
		type = T_LINK;
		permissions = 0777;
		break;
	default:
		type = T_FILE;
		permissions = 0644;
		break;
	}
	uid = gid = 0;
	mtime = fallback;
	if (m != NULL && (m->valid & FSIMG_META_MODE) != 0)
		permissions = m->mode;
	if (m != NULL && (m->valid & FSIMG_META_OWNER) != 0) {
		uid = m->uid;
		gid = m->gid;
	}
	if (m != NULL && (m->valid & FSIMG_META_MODIFIED) != 0 && !reproducible)
		mtime = (uint32_t)m->modified.seconds;
	ok = e->mode == (type | permissions) && e->uid == uid &&
	    e->gid == gid && e->mtime == mtime;
	if (type == T_FILE || type == T_LINK)
		ok = ok && e->size == spec->size &&
		    e->digest == expected_digest(spec->source, spec->size);
	if (type == T_LINK)
		ok = ok && strlen(e->target) == spec->size;
	if (!ok)
		fprintf(stderr, "%s %s: the stored record differs from its "
		    "spec\n", label, spec->path);
	CHECK(ok);
}

/* The tree's entries in the checker's decoding, and nothing else but the
 * root, which carries fallback.
 */
static void
check_tree_entries(const chk_t *c, uint32_t fallback, bool reproducible)
{
	CHECK(c->count == TREE_COUNT + 1);
	CHECK(c->entries[0].mtime == fallback);
	for (size_t i = 0; i < TREE_COUNT; i++)
		check_stored(c, "tree", &tree[i], fallback, reproducible);
}

/* Read the image back through fsimg: every entry matches the checker's, the
 * mountpoint is reported, and payload verification is complete.
 */
static void
check_reader(fsimg_ctx_t *ctx, memory_sink_t *sink, const chk_t *c)
{
	fsimg_reader_t *reader;
	fsimg_io_t *input;
	const fsimg_entry_t *entry;
	const chk_entry_t *e;
	fsimg_volume_info_t info;
	fsimg_verify_report_t report;
	fsimg_error_t error;
	unsigned char buffer[4096];
	uint64_t offset, count;
	uint32_t h;
	size_t length;
	fsimg_status_t status;

	CHECK(memory_io(ctx, sink, false, &input) == FSIMG_OK);
	CHECK(fsimg_reader_open(ctx, input, NULL, &reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_get_info(reader, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.format, "ifs") == 0);
	CHECK((info.valid & FSIMG_VOLUME_MOUNTPOINT) != 0);
	CHECK(strcmp(info.mountpoint, c->mountpoint) == 0);
	CHECK(info.volume_bytes == sink->length);
	count = 0;
	while ((status = fsimg_reader_next(reader, &entry, NULL)) == FSIMG_OK) {
		count++;
		e = chk_find(c, entry->path);
		CHECK(e != NULL);
		CHECK(entry->metadata.mode == (e->mode & 07777));
		CHECK(entry->metadata.uid == e->uid && entry->metadata.gid == e->gid);
		CHECK(entry->metadata.modified.seconds == e->mtime);
		if (entry->kind != FSIMG_ENTRY_FILE &&
		    entry->kind != FSIMG_ENTRY_SYMLINK)
			continue;
		CHECK(entry->size == e->size);
		h = DIGEST_SEED;
		for (offset = 0; offset < entry->size; offset += length) {
			CHECK(fsimg_reader_read_at(reader, offset, buffer,
			    sizeof(buffer), &length, NULL) == FSIMG_OK);
			CHECK(length > 0);
			h = digest(h, buffer, length);
		}
		CHECK(h == e->digest);
	}
	CHECK(status == FSIMG_DONE);
	/* The checker's entries include the root, which the reader does not
	 * list.
	 */
	CHECK(count == c->count - 1);
	fsimg_reader_free(reader);
	CHECK(fsimg_reader_open(ctx, input, NULL, &reader, NULL) == FSIMG_OK);
	status = fsimg_verify(reader, FSIMG_VERIFY_PAYLOAD, &report, &error);
	if (status != FSIMG_OK)
		fprintf(stderr, "verify: %d %s\n", status, error.message);
	CHECK(status == FSIMG_OK);
	CHECK(report.complete);
	fsimg_reader_free(reader);
	fsimg_io_free(input);
}

/*
 * Tests.
 */

/* The tree in three passes -- forward catalog order with a one-page cache
 * and scratch, reverse order with a 1024-page cache in another time zone,
 * and forward order without scratch, all metadata in a 1024-page cache --
 * passes the checker and the reader each time and gives identical bytes.
 * Each fixture has its own scratch directory.
 */
static void
test_matrix(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sinks[3];
	chk_t c;

	reproducible_options(&options);
	for (unsigned pass = 0; pass < 3; pass++) {
		if (pass == 1) {
			CHECK(setenv("TZ", "America/New_York", 1) == 0);
			tzset();
		}
		fixture_open_with(&fixture, &options, pass == 0 ? 1 : 1024,
		    pass != 2);
		add_specs(&fixture, tree, TREE_COUNT, pass == 1);
		CHECK(emit_memory(&fixture, &sinks[pass], NULL) == FSIMG_OK);
		/* owned_metadata's time differs from the epoch. */
		CHECK(fixture.losses == TREE_TIMED);
		chk_image(&c, sinks[pass].bytes, sinks[pass].length, true, false);
		CHECK(c.errors == 0);
		CHECK(!c.big && strcmp(c.mountpoint, "/") == 0);
		check_tree_entries(&c, EPOCH, true);
		check_reader(fixture.ctx, &sinks[pass], &c);
		chk_free(&c);
		fixture_close(&fixture);
		if (pass == 1) {
			CHECK(unsetenv("TZ") == 0);
			tzset();
		}
	}
	for (unsigned pass = 1; pass < 3; pass++) {
		CHECK(sinks[pass].length == sinks[0].length);
		CHECK(bcmp(sinks[pass].bytes, sinks[0].bytes,
		    (size_t)sinks[0].length) == 0);
	}
	for (unsigned pass = 0; pass < 3; pass++)
		free(sinks[pass].bytes);
}

/* Without an epoch the tree keeps its own times, with no loss, and the
 * root and every entry without a time carry the planning clock.
 */
static void
test_tree_times(void)
{
	fsimg_write_options_t options;
	memory_sink_t sink;
	unsigned losses;
	time_t before, after;
	chk_t c;

	fsimg_write_options_init(&options);
	before = time(NULL);
	build_checked(&options, tree, TREE_COUNT, &sink, &c, &losses);
	after = time(NULL);
	CHECK(losses == 0);
	CHECK(c.entries[0].mtime >= (uint32_t)before &&
	    c.entries[0].mtime <= (uint32_t)after);
	check_tree_entries(&c, c.entries[0].mtime, false);
	chk_free(&c);
	free(sink.bytes);
}

/* An empty catalog is the root record alone: the header, "/", the root,
 * the terminator and the trailer, 128 bytes as native mkifs writes it. One
 * file of 5 bytes adds a record and two words of data: 176 bytes.
 */
static void
test_small(void)
{
	static const spec_t one[] = {
		{ "/a.txt", FSIMG_ENTRY_FILE, 5, 1, NULL }
	};
	fsimg_write_options_t options;
	fsimg_plan_info_t info;
	memory_sink_t sink;
	chk_t c;

	reproducible_options(&options);
	options.epoch = 0;
	build_checked(&options, NULL, 0, &sink, &c, NULL);
	CHECK(sink.length == 128 && c.count == 1 && c.dir_offset == 0x5c &&
	    c.table_end == 0x7c && c.entries[0].mtime == 0);
	chk_free(&c);
	free(sink.bytes);
	CHECK(plan_status(&options, NULL, 0, &info) == FSIMG_OK);
	CHECK(strcmp(info.format, "ifs") == 0 && info.volume_bytes == 128 &&
	    info.metadata_bytes == 128 && info.allocated_bytes == 128 &&
	    info.free_bytes == 0 && info.initialized_bytes == 0 &&
	    info.sector_bytes == 1 && info.allocation_unit_bytes == 1);

	build_checked(&options, one, 1, &sink, &c, NULL);
	CHECK(sink.length == 176 && c.count == 2 && c.table_end == 0xa4 &&
	    c.entries[1].data == 0xa4 && c.entries[1].length == 0x28);
	chk_free(&c);
	free(sink.bytes);
	CHECK(plan_status(&options, one, 1, &info) == FSIMG_OK);
	CHECK(info.volume_bytes == 176 && info.metadata_bytes == 0xa4 + 4 &&
	    info.initialized_bytes == 5 && info.payload_bytes == 5 &&
	    info.entries == 1);
}

/* Mountpoints of 0, 1 and 1024 bytes, the default and a UTF-8 one are
 * stored and reported; 1025 bytes, a control byte and invalid UTF-8 are
 * refused when the writer is made.
 */
static void
test_mountpoints(void)
{
	static char longest[PATH_BYTES + 1], longer[PATH_BYTES + 2];
	static const struct {
		const char	*mountpoint;
		const char	*stored;
		uint32_t	 dir_offset;
	} cases[] = {
		{ NULL, "/", 0x5c },
		{ "", "", 0x5c },
		{ "x", "x", 0x5c },
		{ "/foo", "/foo", 0x60 },
		{ "/m\xc3\xbcnt", "/m\xc3\xbcnt", 0x60 },
		{ longest, longest, 0x45c }
	};
	static const char *const refused[] = { longer, "/a\001", "/\xff" };
	fsimg_write_options_t options;
	fsimg_writer_t *writer;
	fixture_t fixture;
	fsimg_ctx_t *ctx;
	memory_sink_t sink;
	chk_t c;

	memset(longest, 'm', PATH_BYTES);
	memset(longer, 'm', PATH_BYTES + 1);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		reproducible_options(&options);
		options.mountpoint = cases[i].mountpoint;
		fixture_open(&fixture, &options);
		CHECK(add_one(&fixture, &tree[0]) == FSIMG_OK);
		CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
		chk_image(&c, sink.bytes, sink.length, true, false);
		CHECK(c.errors == 0);
		CHECK(strcmp(c.mountpoint, cases[i].stored) == 0);
		CHECK(c.dir_offset == cases[i].dir_offset);
		check_reader(fixture.ctx, &sink, &c);
		chk_free(&c);
		fixture_close(&fixture);
		free(sink.bytes);
	}
	CHECK(fsimg_ctx_new(NULL, &ctx, NULL) == FSIMG_OK);
	for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
		reproducible_options(&options);
		options.mountpoint = refused[i];
		CHECK(ifs_writer(ctx, &options, &writer, NULL) ==
		    FSIMG_EINVAL);
	}
	fsimg_ctx_free(ctx);
}

/* A big-endian image of the tree carries flags 0x05 and every field and
 * the checksum in that order, and reads back.
 */
static void
test_big_endian(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	chk_t c;

	reproducible_options(&options);
	options.byte_order = FSIMG_BYTE_ORDER_BIG;
	fixture_open(&fixture, &options);
	add_specs(&fixture, tree, TREE_COUNT, false);
	CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
	chk_image(&c, sink.bytes, sink.length, true, false);
	CHECK(c.errors == 0 && c.big);
	CHECK(sink.bytes[7] == 0x05 && sink.bytes[0x08] == 0);
	check_tree_entries(&c, EPOCH, true);
	check_reader(fixture.ctx, &sink, &c);
	chk_free(&c);
	fixture_close(&fixture);
	free(sink.bytes);
}

/* Paths: a 1024-byte path and a 1024-byte component are stored; a
 * 1025-byte path is unsupported; components outside the profile are
 * invalid wherever the catalog lets them through.
 */
static void
test_names(void)
{
	static char long_path[1 + PATH_BYTES + 1], long_name[1 + PATH_BYTES + 1];
	static char too_long[1 + PATH_BYTES + 2];
	static const char *const outside[] = { "/a\001b", "/del\177", "/\xc3" };
	spec_t specs[2];
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	const fsimg_plan_t *plan;
	fsimg_status_t status;
	spec_t spec;
	chk_t c;

	reproducible_options(&options);
	long_name[0] = '/';
	memset(long_name + 1, 'n', PATH_BYTES);
	strcpy(long_path, "/d/");
	memset(long_path + 3, 'p', PATH_BYTES - 2);
	specs[0] = (spec_t){ long_name, FSIMG_ENTRY_SYMLINK, PATH_BYTES, 4, NULL };
	specs[1] = (spec_t){ "/d", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL };
	fixture_open(&fixture, &options);
	add_specs(&fixture, specs, 2, false);
	spec = (spec_t){ long_path, FSIMG_ENTRY_FILE, 3, 2, NULL };
	CHECK(add_one(&fixture, &spec) == FSIMG_OK);
	CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
	chk_image(&c, sink.bytes, sink.length, true, false);
	CHECK(c.errors == 0 && c.count == 4);
	/* The longest record: a 1024-byte path and a 1024-byte target. */
	CHECK(chk_find(&c, long_name) != NULL &&
	    chk_find(&c, long_name)->length == 2080);
	CHECK(chk_find(&c, long_path) != NULL &&
	    strlen(chk_find(&c, long_path)->path) == PATH_BYTES);
	check_reader(fixture.ctx, &sink, &c);
	chk_free(&c);
	fixture_close(&fixture);
	free(sink.bytes);

	strcpy(too_long, "/d/");
	memset(too_long + 3, 'p', PATH_BYTES - 1);
	fixture_open(&fixture, &options);
	CHECK(add_one(&fixture, &specs[1]) == FSIMG_OK);
	spec = (spec_t){ too_long, FSIMG_ENTRY_FILE, 3, 2, NULL };
	CHECK(add_one(&fixture, &spec) == FSIMG_OK);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) ==
	    FSIMG_EUNSUPPORTED);
	fixture_close(&fixture);

	for (size_t i = 0; i < sizeof(outside) / sizeof(outside[0]); i++) {
		fixture_open(&fixture, &options);
		spec = (spec_t){ outside[i], FSIMG_ENTRY_FILE, 0, 0, NULL };
		status = add_one(&fixture, &spec);
		if (status == FSIMG_OK)
			status = fsimg_writer_plan(fixture.writer, &plan, NULL);
		CHECK(status == FSIMG_EINVAL);
		fixture_close(&fixture);
	}
}

/* Emit one file with metadata m and return the stored time, the losses
 * reported and the plan status.
 */
static fsimg_status_t
stored_time(const fsimg_write_options_t *options, const fsimg_metadata_t *m,
    uint32_t *mtime, unsigned *losses)
{
	spec_t spec = { "/m", FSIMG_ENTRY_FILE, 1, 2, m };
	fixture_t fixture;
	memory_sink_t sink;
	fsimg_error_t error;
	fsimg_status_t status;
	chk_t c;

	fixture_open(&fixture, options);
	CHECK(add_one(&fixture, &spec) == FSIMG_OK);
	status = emit_memory(&fixture, &sink, &error);
	*losses = fixture.losses;
	fixture_close(&fixture);
	if (status == FSIMG_OK) {
		chk_image(&c, sink.bytes, sink.length, true, false);
		CHECK(c.errors == 0 && c.count == 2);
		CHECK(c.entries[1].uid == m->uid && c.entries[1].gid == m->gid &&
		    c.entries[1].mode == (T_FILE | m->mode));
		*mtime = c.entries[1].mtime;
		chk_free(&c);
	}
	free(sink.bytes);
	return (status);
}

/* Owner 4294967295, mode 07777 and time 4294967295 are stored exactly. A
 * time outside 32 bits or before 1970 takes the planning clock, and one
 * between seconds keeps its whole seconds, each as a counted loss that
 * fails when losses are forbidden or the time is required. Under an epoch
 * a time equal to it is exact and another one a loss.
 */
static void
test_metadata(void)
{
	static const struct {
		int64_t		 seconds;
		uint32_t	 nanoseconds;
		bool		 clock;		/* stored as the planning clock */
	} lossy[] = {
		{ INT64_C(4294967296), 0, true },
		{ -1, 0, true },
		{ 1000, 5, false }
	};
	fsimg_write_options_t options;
	fsimg_metadata_t m;
	unsigned losses;
	uint32_t mtime;
	time_t before, after;

	bzero(&m, sizeof(m));
	m.valid = FSIMG_META_OWNER | FSIMG_META_MODE | FSIMG_META_MODIFIED;
	m.uid = UINT32_MAX;
	m.gid = UINT32_MAX;
	m.mode = 07777;
	m.modified.seconds = UINT32_MAX;
	m.modified.precision_ns = 1000000000;
	m.modified.basis = FSIMG_TIME_UTC;
	fsimg_write_options_init(&options);
	CHECK(stored_time(&options, &m, &mtime, &losses) == FSIMG_OK);
	CHECK(losses == 0 && mtime == UINT32_MAX);

	for (size_t i = 0; i < sizeof(lossy) / sizeof(lossy[0]); i++) {
		m.modified.seconds = lossy[i].seconds;
		m.modified.nanoseconds = lossy[i].nanoseconds;
		m.required = 0;
		fsimg_write_options_init(&options);
		before = time(NULL);
		CHECK(stored_time(&options, &m, &mtime, &losses) == FSIMG_OK);
		after = time(NULL);
		CHECK(losses == 1);
		if (lossy[i].clock)
			CHECK(mtime >= (uint32_t)before && mtime <= (uint32_t)after);
		else
			CHECK(mtime == (uint32_t)lossy[i].seconds);
		options.flags &= ~FSIMG_WRITE_ALLOW_LOSS;
		CHECK(stored_time(&options, &m, &mtime, &losses) ==
		    FSIMG_EUNSUPPORTED);
		fsimg_write_options_init(&options);
		m.required = FSIMG_META_MODIFIED;
		CHECK(stored_time(&options, &m, &mtime, &losses) ==
		    FSIMG_EUNSUPPORTED);
	}

	m.required = 0;
	m.modified.nanoseconds = 0;
	m.modified.seconds = EPOCH;
	reproducible_options(&options);
	CHECK(stored_time(&options, &m, &mtime, &losses) == FSIMG_OK);
	CHECK(losses == 0 && mtime == EPOCH);
	m.modified.seconds = EPOCH + 1;
	CHECK(stored_time(&options, &m, &mtime, &losses) == FSIMG_OK);
	CHECK(losses == 1 && mtime == EPOCH);
}

/* A 1024-byte link target is stored; collection refuses 1025 bytes, and a
 * target holding a NUL fails emission.
 */
static void
test_links(void)
{
	static const spec_t longer = { "/l", FSIMG_ENTRY_SYMLINK, 1025, 4, NULL };
	static const spec_t nul = { "/l", FSIMG_ENTRY_SYMLINK, 3, 9, NULL };
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	fsimg_error_t error;

	reproducible_options(&options);
	fixture_open(&fixture, &options);
	CHECK(add_one(&fixture, &longer) == FSIMG_EINVAL);
	fixture_close(&fixture);
	fixture_open(&fixture, &options);
	CHECK(add_one(&fixture, &nul) == FSIMG_OK);
	CHECK(emit_memory(&fixture, &sink, &error) == FSIMG_EINVAL);
	CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
	free(sink.bytes);
	fixture_close(&fixture);
}

/* The write settings the registry lists for a format it lists as
 * writable.
 */
static uint32_t
listed_writer_settings(const char *name)
{
	fsimg_format_info_t info;
	size_t cursor = 0;

	while (fsimg_format_next(&cursor, &info, NULL) == FSIMG_OK)
		if (strcmp(info.name, name) == 0) {
			CHECK((info.capabilities & FSIMG_CAP_WRITE) != 0);
			return (info.write_settings);
		}
	CHECK(!"the format is listed");
	return (0);
}

/* Writer construction with options fails with status. */
static void
check_refused(fsimg_ctx_t *ctx, const fsimg_write_options_t *options,
    fsimg_status_t status)
{
	fsimg_writer_t *writer;

	CHECK(ifs_writer(ctx, options, &writer, NULL) == status);
}

/* Settings: byte addressing only, no FAT root, no size or free space, no
 * label, a 32-bit reproducible epoch, a known byte order, and no UUID,
 * inode capacity or reserve. The registry lists the writer with its
 * settings, a variant of another format is refused, and a format that
 * stores no mountpoint refuses one.
 */
static void
test_validation(void)
{
	fsimg_write_options_t options, o;
	fsimg_writer_t *writer;
	fsimg_ctx_t *ctx;

	CHECK(fsimg_ctx_new(NULL, &ctx, NULL) == FSIMG_OK);
	fsimg_write_options_init(&options);
	CHECK(options.sector_bytes == 0);
	CHECK(ifs_writer(ctx, &options, &writer, NULL) == FSIMG_OK);
	fsimg_writer_free(writer);
	o = options;
	o.sector_bytes = 1;
	o.allocation_unit_bytes = 1;
	CHECK(ifs_writer(ctx, &o, &writer, NULL) == FSIMG_OK);
	fsimg_writer_free(writer);
	reproducible_options(&o);
	o.epoch = UINT32_MAX;
	CHECK(ifs_writer(ctx, &o, &writer, NULL) == FSIMG_OK);
	fsimg_writer_free(writer);
	CHECK(listed_writer_settings("ifs") == (FSIMG_SETTING_BYTE_ORDER |
	    FSIMG_SETTING_MOUNTPOINT));
	CHECK(fsimg_writer_new_variant(ctx, &fsimg_ifs_format,
	    &fsimg_qnx6_format.variants[0], &options, &writer, NULL) ==
	    FSIMG_EINVAL);

	o = options;
	o.sector_bytes = 512;
	check_refused(ctx, &o, FSIMG_EINVAL);
	o = options;
	o.allocation_unit_bytes = 512;
	check_refused(ctx, &o, FSIMG_EINVAL);
	o = options;
	o.fat_root_entries = 16;
	check_refused(ctx, &o, FSIMG_EINVAL);
	o = options;
	o.volume_bytes = 1u << 20;
	check_refused(ctx, &o, FSIMG_EINVAL);
	o = options;
	o.min_free_bytes = 4;
	check_refused(ctx, &o, FSIMG_EINVAL);
	o = options;
	o.byte_order = (fsimg_byte_order_t)2;
	check_refused(ctx, &o, FSIMG_EINVAL);
	o = options;
	o.inode_capacity = 100;
	check_refused(ctx, &o, FSIMG_EINVAL);
	o = options;
	o.reserve_percent = 5;
	check_refused(ctx, &o, FSIMG_EINVAL);
	o = options;
	o.volume_uuid[0] = 1;
	check_refused(ctx, &o, FSIMG_EINVAL);
	reproducible_options(&o);
	o.epoch = -1;
	check_refused(ctx, &o, FSIMG_EINVAL);
	o.epoch = INT64_C(4294967296);
	check_refused(ctx, &o, FSIMG_EINVAL);
	o = options;
	o.label = "LABEL";
	check_refused(ctx, &o, FSIMG_EUNSUPPORTED);

	o = options;
	o.mountpoint = "/";
	CHECK(fsimg_writer_new(ctx, "fat32", &o, &writer, NULL) == FSIMG_EINVAL);
	fsimg_ctx_free(ctx);
}

/* An ImageFS must fill its slot exactly, so a stack that puts a scheme or
 * a padding container below ifs is refused when the writer is made,
 * naming both layers, whatever the partition settings; gzip pads nothing
 * and is accepted. A wrap whose stack names ifs is refused the same way
 * before it writes a byte.
 */
static void
test_stack_rule(void)
{
	static const struct {
		const char	*stack;
		const char	*layers;
	} refused[] = {
		{ "ifs/gpt", "ifs/gpt" }, { "ifs/mbr", "ifs/mbr" },
		{ "ifs/vhd", "ifs/vhd" }, { "ifs/dmg", "ifs/dmg" },
		{ "ifs/simg", "ifs/simg" }, { "ifs/gpt/vhd", "ifs/gpt" },
		{ "ifs/-/simg", "ifs/simg" }
	};
	static const spec_t one[] = {
		{ "/a.txt", FSIMG_ENTRY_FILE, 5, 1, NULL }
	};
	fsimg_layer_options_t gpt, mbr;
	fsimg_write_options_t options, o;
	fsimg_writer_t *writer;
	fsimg_sink_t sink;
	fsimg_error_t error;
	fsimg_io_t *input, *output;
	memory_sink_t volume, out;
	fsimg_ctx_t *ctx;
	chk_t c;

	CHECK(fsimg_ctx_new(NULL, &ctx, NULL) == FSIMG_OK);
	reproducible_options(&options);
	/* A typed partition, so the refusal is not the missing type's. */
	fsimg_layer_options_init(&gpt);
	snprintf(gpt.gpt_type, sizeof(gpt.gpt_type), "linux");
	fsimg_layer_options_init(&mbr);
	snprintf(mbr.mbr_type, sizeof(mbr.mbr_type), "0x83");
	for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
		o = options;
		if (strstr(refused[i].stack, "gpt") != NULL)
			o.layers = &gpt;
		else if (strstr(refused[i].stack, "mbr") != NULL)
			o.layers = &mbr;
		writer = NULL;
		CHECK(fsimg_writer_new(ctx, refused[i].stack, &o, &writer,
		    &error) == FSIMG_EUNSUPPORTED);
		CHECK(writer == NULL &&
		    strcmp(error.path, refused[i].layers) == 0);
	}
	/* A scheme is refused before a container's settings are read. */
	o = options;
	gpt.vhd_align_bytes = 3;
	o.layers = &gpt;
	CHECK(fsimg_writer_new(ctx, "ifs/gpt/vhd", &o, &writer, &error) ==
	    FSIMG_EUNSUPPORTED && strcmp(error.path, "ifs/gpt") == 0);
	o = options;
	snprintf(o.compression, sizeof(o.compression), "%s", "gzip");
	CHECK(fsimg_writer_new(ctx, "ifs", &o, &writer, NULL) == FSIMG_OK);
	fsimg_writer_free(writer);
	CHECK(fsimg_writer_new(ctx, "ifs/-/-", &options, &writer, NULL) ==
	    FSIMG_OK);
	fsimg_writer_free(writer);

	build_checked(&options, one, 1, &volume, &c, NULL);
	chk_free(&c);
	CHECK(memory_io(ctx, &volume, false, &input) == FSIMG_OK);
	bzero(&out, sizeof(out));
	CHECK(memory_io(ctx, &out, true, &output) == FSIMG_OK);
	fsimg_sink_init(&sink, FSIMG_SINK_IO);
	sink.io = output;
	CHECK(fsimg_image_wrap(ctx, input, "ifs/vhd", &options, &sink,
	    &error) == FSIMG_EUNSUPPORTED);
	CHECK(strcmp(error.path, "ifs/vhd") == 0 && out.writes == 0);
	fsimg_io_free(output);
	fsimg_io_free(input);
	free(volume.bytes);
	fsimg_ctx_free(ctx);
}

/* Rewrite a little-endian image's trailer so its words sum to zero again,
 * so that only the checker's layout and zero checks can see a mutation.
 */
static void
rebalance(unsigned char *image, uint64_t length)
{
	uint32_t sum = 0;

	for (uint64_t i = 0; i + 4 < length; i += 4)
		sum += (uint32_t)image[i] | (uint32_t)image[i + 1] << 8 |
		    (uint32_t)image[i + 2] << 16 | (uint32_t)image[i + 3] << 24;
	sum = 0 - sum;
	for (unsigned i = 0; i < 4; i++)
		image[length - 4 + i] = (unsigned char)(sum >> (8 * i));
}

/* The checker sees one changed byte, with the checksum kept valid, in the
 * mountpoint padding, a spare word, a record's padding, the gap after a
 * file and the terminator, none of which the reader interprets; and a
 * changed trailer alone breaks the checksum.
 */
static void
test_checker(void)
{
	static const spec_t files[] = {
		{ "/a", FSIMG_ENTRY_FILE, 1, 2, NULL },
		{ "/bb", FSIMG_ENTRY_FILE, 3, 5, NULL }
	};
	fsimg_write_options_t options;
	memory_sink_t sink;
	unsigned char *pristine;
	uint64_t offsets[6];
	chk_t c;

	reproducible_options(&options);
	options.mountpoint = "/x";
	build_checked(&options, files, 2, &sink, &c, NULL);
	CHECK(!c.big);		/* rebalance() is little-endian */
	offsets[0] = HEADER + 3;
	offsets[1] = 0x30;
	offsets[2] = c.entries[1].offset + c.entries[1].length - 1;
	offsets[3] = c.entries[1].data + 1;
	offsets[4] = c.table_end - 2;
	offsets[5] = sink.length - 1;
	chk_free(&c);
	pristine = malloc((size_t)sink.length);
	CHECK(pristine != NULL);
	bcopy(sink.bytes, pristine, (size_t)sink.length);
	for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
		sink.bytes[offsets[i]] ^= 0x40;
		if (offsets[i] < sink.length - 4)
			rebalance(sink.bytes, sink.length);
		chk_image(&c, sink.bytes, sink.length, true, true);
		CHECK(c.errors != 0);
		chk_free(&c);
		bcopy(pristine, sink.bytes, (size_t)sink.length);
	}
	chk_image(&c, sink.bytes, sink.length, true, true);
	CHECK(c.errors == 0);
	chk_free(&c);
	free(pristine);
	free(sink.bytes);
}

enum { FAULT_REFUSED, FAULT_INCOMPLETE, FAULT_REFERENCE };

typedef struct fault {
	uint64_t	 extra_sink;	/* bytes past the volume */
	bool		 durable;	/* on a sink without durability */
	uint64_t	 fail_at;	/* this write fails */
	bool		 fail_flush;
	bool		 changed;	/* a source changes */
	bool		 eof;		/* a source ends early */
	size_t		 max_write;	/* short writes */
	int		 outcome;
} fault_t;

/* Emit the tree into a poisoned sink under one fault and check the
 * outcome; a second emission is always refused.
 */
static void
check_fault(const fault_t *fault, const memory_sink_t *reference)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_io_t *output;
	fsimg_error_t error;

	reproducible_options(&options);
	if (fault->durable)
		options.flags |= FSIMG_WRITE_DURABLE;
	fixture_open(&fixture, &options);
	add_specs(&fixture, tree, TREE_COUNT, false);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	bzero(&sink, sizeof(sink));
	sink.length = info.volume_bytes + fault->extra_sink;
	sink.bytes = malloc((size_t)sink.length);
	CHECK(sink.bytes != NULL);
	memset(sink.bytes, POISON, (size_t)sink.length);
	CHECK(memory_io(fixture.ctx, &sink, true, &output) == FSIMG_OK);
	sink.fail_at = fault->fail_at;
	sink.fail_flush = fault->fail_flush;
	sink.max_write = fault->max_write;
	fixture.changed = fault->changed;
	fixture.eof = fault->eof;
	switch (fault->outcome) {
	case FAULT_REFERENCE:
		CHECK(fsimg_writer_emit(fixture.writer, output, NULL) == FSIMG_OK);
		CHECK(sink.length == reference->length &&
		    bcmp(sink.bytes, reference->bytes, (size_t)sink.length) == 0);
		break;
	case FAULT_REFUSED:
		CHECK(fsimg_writer_emit(fixture.writer, output, NULL) < FSIMG_OK);
		CHECK(sink.writes == 0);
		break;
	default:
		CHECK(fsimg_writer_emit(fixture.writer, output, &error) <
		    FSIMG_OK);
		CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
		break;
	}
	CHECK(fsimg_writer_emit(fixture.writer, output, NULL) == FSIMG_ESTATE);
	fsimg_io_free(output);
	free(sink.bytes);
	fixture_close(&fixture);
}

/* Sink and source faults: an oversized fixed sink and a durable request on
 * a sink without durability are refused before any write, a failure of
 * each write in turn, a failed flush and a changed or short source leave
 * the output incomplete, short writes give the reference bytes, and a
 * writer emits once.
 */
static void
test_faults(void)
{
	static const fault_t cases[] = {
		{ .extra_sink = 4, .outcome = FAULT_REFUSED },
		{ .durable = true, .outcome = FAULT_REFUSED },
		{ .fail_flush = true, .outcome = FAULT_INCOMPLETE },
		{ .changed = true, .outcome = FAULT_INCOMPLETE },
		{ .eof = true, .outcome = FAULT_INCOMPLETE },
		{ .max_write = 7, .outcome = FAULT_REFERENCE }
	};
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t reference;
	fault_t fault;

	reproducible_options(&options);
	fixture_open(&fixture, &options);
	add_specs(&fixture, tree, TREE_COUNT, false);
	CHECK(emit_memory(&fixture, &reference, NULL) == FSIMG_OK);
	fixture_close(&fixture);
	CHECK(reference.writes > 1);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		check_fault(&cases[i], &reference);
	for (uint64_t n = 1; n <= reference.writes; n++) {
		fault = (fault_t){ .fail_at = n, .outcome = FAULT_INCOMPLETE };
		check_fault(&fault, &reference);
	}
	free(reference.bytes);
}

/* Allocation, metadata-storage and cancellation failures at every point of
 * planning and emission fail cleanly; the fixture close checks that no
 * memory, scratch or descriptor is left behind.
 */
static void
test_injected(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	fsimg_status_t status;
	bool succeeded;

	reproducible_options(&options);
	succeeded = false;
	for (uint64_t attempt = 1; attempt < 4096 && !succeeded; attempt++) {
		fixture_open(&fixture, &options);
		add_specs(&fixture, tree, TREE_COUNT, false);
		fsimg_test_fail_alloc_after(fixture.ctx, attempt);
		status = emit_memory(&fixture, &sink, NULL);
		fsimg_test_fail_alloc_after(fixture.ctx, 0);
		CHECK(status == FSIMG_OK || status == FSIMG_ENOMEM);
		succeeded = status == FSIMG_OK;
		free(sink.bytes);
		fixture_close(&fixture);
	}
	CHECK(succeeded);
	for (int fault = FSIMG_META_OPEN; fault <= FSIMG_META_WRITE; fault++) {
		succeeded = false;
		for (uint64_t attempt = 1; attempt < 4096 && !succeeded;
		    attempt++) {
			fixture_open(&fixture, &options);
			add_specs(&fixture, tree, TREE_COUNT, false);
			fsimg_test_meta_fail_after(fixture.ctx,
			    (fsimg_meta_fault_t)fault, attempt, EIO);
			status = emit_memory(&fixture, &sink, NULL);
			fsimg_test_meta_fail_after(fixture.ctx,
			    (fsimg_meta_fault_t)fault, 0, 0);
			CHECK(status == FSIMG_OK || status == FSIMG_EIO);
			succeeded = status == FSIMG_OK;
			free(sink.bytes);
			fixture_close(&fixture);
		}
		CHECK(succeeded);
	}
	/* Cancel at every poll of the run. */
	succeeded = false;
	for (uint64_t attempt = 1; attempt < 100000 && !succeeded; attempt++) {
		fixture_open(&fixture, &options);
		add_specs(&fixture, tree, TREE_COUNT, false);
		fixture.cancel_after = attempt;
		fixture.polls = 0;
		status = emit_memory(&fixture, &sink, NULL);
		CHECK(status == FSIMG_OK || status == FSIMG_ECANCELLED);
		succeeded = status == FSIMG_OK;
		free(sink.bytes);
		fixture_close(&fixture);
	}
	CHECK(succeeded);
}

/*
 * The 2^32 - 4 byte limit, on a sparse sink.
 */

#define SPARSE_EDGE	65536u

/* A sink that stores only its first and last SPARSE_EDGE bytes and reads
 * zero everywhere else. A nonzero byte written between them fails the
 * test: the image's metadata lies at its ends and its payload is a zero
 * source, which the emitter never writes.
 */
typedef struct sparse_sink {
	uint64_t	 length;
	unsigned char	 head[SPARSE_EDGE];
	unsigned char	 tail[SPARSE_EDGE];
} sparse_sink_t;

/* Where [offset, offset + length) meets the stored edges: the head part
 * ends at head_end, at most offset when the range starts past the head,
 * and the tail part starts at tail_start, at least offset.
 */
static void
sparse_split(sparse_sink_t *sink, uint64_t offset, uint64_t length,
    uint64_t *head_end, uint64_t *tail_start)
{
	*head_end = offset + length < SPARSE_EDGE ? offset + length : SPARSE_EDGE;
	*tail_start = sink->length - SPARSE_EDGE;
	if (*tail_start < offset)
		*tail_start = offset;
}

static fsimg_status_t
sparse_size(void *argument, uint64_t *length, fsimg_error_t *error FSIMG_UNUSED)
{
	sparse_sink_t *sink = argument;

	*length = sink->length;
	return (FSIMG_OK);
}

static fsimg_status_t
sparse_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	sparse_sink_t *sink = argument;
	unsigned char *bytes = buffer;
	uint64_t head_end, tail_start, base;

	CHECK(offset <= sink->length && length <= sink->length - offset);
	memset(bytes, 0, length);
	sparse_split(sink, offset, length, &head_end, &tail_start);
	if (offset < head_end)
		bcopy(sink->head + offset, bytes, (size_t)(head_end - offset));
	base = sink->length - SPARSE_EDGE;
	if (tail_start < offset + length)
		bcopy(sink->tail + (tail_start - base), bytes + (tail_start - offset),
		    (size_t)(offset + length - tail_start));
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
sparse_write(void *argument, uint64_t offset, const void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	sparse_sink_t *sink = argument;
	const unsigned char *bytes = buffer;
	uint64_t head_end, tail_start, base;

	CHECK(offset <= sink->length && length <= sink->length - offset);
	sparse_split(sink, offset, length, &head_end, &tail_start);
	for (uint64_t i = offset < head_end ? head_end : offset;
	    i < tail_start && i < offset + length; i++)
		CHECK(bytes[i - offset] == 0);
	if (offset < head_end)
		bcopy(bytes, sink->head + offset, (size_t)(head_end - offset));
	base = sink->length - SPARSE_EDGE;
	if (tail_start < offset + length)
		bcopy(bytes + (tail_start - offset), sink->tail + (tail_start - base),
		    (size_t)(offset + length - tail_start));
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
sparse_zero(void *argument, uint64_t offset, uint64_t length,
    fsimg_error_t *error FSIMG_UNUSED)
{
	sparse_sink_t *sink = argument;
	uint64_t head_end, tail_start, base;

	CHECK(offset <= sink->length && length <= sink->length - offset);
	sparse_split(sink, offset, length, &head_end, &tail_start);
	if (offset < head_end)
		memset(sink->head + offset, 0, (size_t)(head_end - offset));
	base = sink->length - SPARSE_EDGE;
	if (tail_start < offset + length)
		memset(sink->tail + (tail_start - base), 0,
		    (size_t)(offset + length - tail_start));
	return (FSIMG_OK);
}

static fsimg_io_t *
sparse_io(fsimg_ctx_t *ctx, sparse_sink_t *sink, bool writable)
{
	fsimg_io_ops_t ops;
	fsimg_io_t *io;

	fsimg_io_ops_init(&ops);
	ops.get_size = sparse_size;
	ops.read_at = sparse_read;
	if (writable) {
		ops.write_at = sparse_write;
		ops.zero_range = sparse_zero;
	}
	CHECK(fsimg_io_from_callbacks(ctx, &ops, sink, &io, NULL) == FSIMG_OK);
	return (io);
}

/* One 3-byte file and one zero file of payload bytes. The header, "/",
 * the root, two 0x24-byte records and the terminator end at 0xc4, the small
 * file takes one word, and the payload rounded up to 4 and the trailer
 * follow, so a payload of 2^32 - 4 - 0xc8 - 4 bytes makes the image
 * exactly 2^32 - 4.
 */
#define LIMIT_METADATA	0xc4u
#define LIMIT_PAYLOAD	(IMAGE_MAX - LIMIT_METADATA - 4 - 4)

static void
limit_specs(uint64_t payload, spec_t *specs)
{
	specs[0] = (spec_t){ "/a", FSIMG_ENTRY_FILE, 3, 2, NULL };
	specs[1] = (spec_t){ "/z", FSIMG_ENTRY_FILE, payload, 0, NULL };
}

/* Plan rows read back from metadata storage are not trusted: a row whose
 * kind, path length, link target length or record length disagrees with
 * the profile stops emission with FSIMG_ECORRUPT before its record is
 * encoded, rather than overrunning the emission buffer, and leaves the
 * output incomplete.
 */
static void
test_tampered_rows(void)
{
	static const spec_t link[] = {
		{ "/l", FSIMG_ENTRY_SYMLINK, 3, 9, NULL }
	};
	enum { HUGE_TARGET, LONG_PATH, RECORD_LENGTH, KIND, CASES };
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_io_t *output;
	fsimg_error_t error;
	unsigned char raw[IFS_ROW_BYTES];
	ifs_plan_t *ifs_plan;
	ifs_row_t row;

	for (int tamper = 0; tamper < CASES; tamper++) {
		reproducible_options(&options);
		fixture_open(&fixture, &options);
		add_specs(&fixture, link, 1, false);
		CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
		CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
		ifs_plan = (ifs_plan_t *)fixture.writer->plan;
		CHECK(ifs_row_get(ifs_plan, 1, &row, NULL) == FSIMG_OK);
		switch (tamper) {
		case HUGE_TARGET:
			row.length = 70000;
			break;
		case LONG_PATH:
			row.path_length = 2000;
			break;
		case RECORD_LENGTH:
			row.record_length += 4;
			break;
		}
		if (tamper != RECORD_LENGTH)
			row.record_length = (uint32_t)ifs_round4(
			    ifs_record_minimum(row.kind, row.path_length,
			    row.length));
		CHECK(ifs_row_set(ifs_plan, 1, &row, NULL) == FSIMG_OK);
		if (tamper == KIND) {
			CHECK(fsimg_array_get(ifs_plan->rows, 1, raw, NULL) ==
			    FSIMG_OK);
			fsimg_store_le32(raw, 7);
			CHECK(fsimg_array_set(ifs_plan->rows, 1, raw, NULL) ==
			    FSIMG_OK);
		}
		bzero(&sink, sizeof(sink));
		sink.length = info.volume_bytes;
		sink.bytes = malloc((size_t)sink.length);
		CHECK(sink.bytes != NULL);
		memset(sink.bytes, POISON, (size_t)sink.length);
		CHECK(memory_io(fixture.ctx, &sink, true, &output) == FSIMG_OK);
		CHECK(fsimg_writer_emit(fixture.writer, output, &error) ==
		    FSIMG_ECORRUPT);
		CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
		fsimg_io_free(output);
		free(sink.bytes);
		fixture_close(&fixture);
	}
}

/* Images one word below 2^32 - 4 bytes and at it are emitted, read and
 * verified, with a checksum the checker recomputes from the stored edges;
 * one word more is out of range, and a file of 2^32 bytes unsupported.
 */
static void
test_limits(void)
{
	fsimg_write_options_t options;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fixture_t fixture;
	sparse_sink_t *sink;
	fsimg_io_t *io;
	fsimg_reader_t *reader;
	fsimg_verify_report_t report;
	spec_t specs[2];
	chk_t c;
	uint32_t sum;

	reproducible_options(&options);
	sink = calloc(1, sizeof(*sink));
	CHECK(sink != NULL);
	for (unsigned below = 0; below < 2; below++) {
		limit_specs(LIMIT_PAYLOAD - 4 * below, specs);
		fixture_open(&fixture, &options);
		add_specs(&fixture, specs, 2, false);
		CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
		CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
		CHECK(info.volume_bytes == IMAGE_MAX - 4 * below);
		memset(sink->head, POISON, sizeof(sink->head));
		memset(sink->tail, POISON, sizeof(sink->tail));
		sink->length = info.volume_bytes;
		io = sparse_io(fixture.ctx, sink, true);
		CHECK(fsimg_writer_emit(fixture.writer, io, NULL) == FSIMG_OK);
		fsimg_io_free(io);

		/* The checker decodes the head, which holds the whole table. */
		bzero(&c, sizeof(c));
		c.big = false;
		CHECK(get32(&c, sink->head + 0x08) == info.volume_bytes);
		CHECK(get32(&c, sink->head + 0x0c) == LIMIT_METADATA);
		sum = 0;
		for (unsigned i = 0; i < SPARSE_EDGE; i += 4)
			sum += get32(&c, sink->head + i) +
			    get32(&c, sink->tail + i);
		CHECK(sum == 0);

		io = sparse_io(fixture.ctx, sink, false);
		CHECK(fsimg_reader_open(fixture.ctx, io, NULL, &reader, NULL) ==
		    FSIMG_OK);
		CHECK(fsimg_verify(reader, 0, &report, NULL) == FSIMG_OK);
		CHECK(report.complete && report.files == 2);
		fsimg_reader_free(reader);
		fsimg_io_free(io);
		fixture_close(&fixture);
	}
	free(sink);

	limit_specs(LIMIT_PAYLOAD + 4, specs);
	CHECK(plan_status(&options, specs, 2, NULL) == FSIMG_ERANGE);
	limit_specs(UINT64_C(1) << 32, specs);
	CHECK(plan_status(&options, specs, 2, NULL) == FSIMG_EUNSUPPORTED);
}

/*
 * Native comparison (--native). FSIMG_IFS_NATIVE names the captures
 * directory; each image of cases/ifs-basic, the E1 trees and the header,
 * kind, metadata and byte-order experiments of the same capture, is
 * decoded by the checker and rebuilt by the writer from its own tree.
 * Only counts and image file names are printed, never a member name.
 */

static unsigned char *
read_file(const char *path, size_t *length)
{
	struct stat st;
	unsigned char *bytes;
	ssize_t got;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	CHECK(fd != -1 && fstat(fd, &st) == 0);
	*length = (size_t)st.st_size;
	bytes = malloc(*length == 0 ? 1 : *length);
	CHECK(bytes != NULL);
	for (size_t done = 0; done < *length; done += (size_t)got) {
		got = read(fd, bytes + done, *length - done);
		CHECK(got > 0);
	}
	CHECK(close(fd) == 0);
	return (bytes);
}

/* What one comparison found. */
typedef struct native_result {
	unsigned	 differences;
	unsigned	 added_directories;	/* a free choice */
	bool		 skipped;
} native_result_t;

/* Whether a stored path is already in the list. */
static bool
path_listed(char *const *paths, size_t count, const char *path, size_t length)
{
	for (size_t i = 0; i < count; i++)
		if (strlen(paths[i]) == length &&
		    strncmp(paths[i], path, length) == 0)
			return (true);
	return (false);
}

/* Add the native image's entries to the fixture as a tree: each record with
 * its kind, owner, mode, time and content, and each parent the table leaves
 * implicit as a directory without metadata.
 */
static void
add_native_tree(fixture_t *fixture, const chk_t *native, uint32_t time)
{
	fsimg_entry_t entry;
	fsimg_source_t source;
	const chk_entry_t *e;
	char **paths, path[2 + PATH_BYTES];
	size_t count, capacity;

	/* The records' paths, then at most one implicit parent per slash. */
	capacity = native->count;
	for (size_t i = 1; i < native->count; i++)
		for (const char *p = native->entries[i].path; *p != '\0'; p++)
			capacity += *p == '/';
	paths = calloc(capacity, sizeof(paths[0]));
	CHECK(paths != NULL);
	count = 0;
	for (size_t i = 1; i < native->count; i++)
		paths[count++] = native->entries[i].path;
	for (size_t i = 1; i < native->count; i++) {
		e = &native->entries[i];
		bzero(&entry, sizeof(entry));
		snprintf(path, sizeof(path), "/%s", e->path);
		entry.path = path;
		entry.metadata.valid = FSIMG_META_OWNER | FSIMG_META_MODE |
		    FSIMG_META_MODIFIED;
		entry.metadata.uid = e->uid;
		entry.metadata.gid = e->gid;
		entry.metadata.mode = e->mode & 07777;
		entry.metadata.modified.seconds = time;
		entry.metadata.modified.precision_ns = 1000000000;
		entry.metadata.modified.basis = FSIMG_TIME_UTC;
		source.kind = FSIMG_SOURCE_PROVIDER;
		source.id = NATIVE_SOURCE + i;
		switch (e->mode & T_TYPE) {
		case T_DIR:
			entry.kind = FSIMG_ENTRY_DIRECTORY;
			break;
		case T_FIFO:
			entry.kind = FSIMG_ENTRY_FIFO;
			break;
		case T_LINK:
			entry.kind = FSIMG_ENTRY_SYMLINK;
			entry.size = e->size;
			break;
		default:
			entry.kind = FSIMG_ENTRY_FILE;
			entry.size = e->size;
			break;
		}
		CHECK(fsimg_writer_add(fixture->writer, &entry,
		    entry.kind == FSIMG_ENTRY_FILE ||
		    entry.kind == FSIMG_ENTRY_SYMLINK ? &source : NULL,
		    NULL) == FSIMG_OK);
		/* Every ancestor without a record of its own, nearest first;
		 * one already listed is, or will be, processed with its own
		 * ancestors.
		 */
		for (size_t end = parent_length(e->path); end != 0 &&
		    !path_listed(paths, count, e->path, end);
		    end = prefix_parent(e->path, end)) {
			paths[count] = copy_text((const unsigned char *)e->path,
			    end);
			bzero(&entry, sizeof(entry));
			snprintf(path, sizeof(path), "/%s", paths[count]);
			entry.path = path;
			entry.kind = FSIMG_ENTRY_DIRECTORY;
			CHECK(fsimg_writer_add(fixture->writer, &entry, NULL,
			    NULL) == FSIMG_OK);
			count++;
		}
	}
	for (size_t i = native->count - 1; i < count; i++)
		free(paths[i]);
	free(paths);
}

/* Count and report one differing field of the header (record -1) or of a
 * native record, naming its index, never its path.
 */
static void
note_field(native_result_t *result, const char *name, long record,
    const char *field, bool same)
{
	if (same)
		return;
	result->differences++;
	if (record < 0)
		printf("%s: header: %s differs\n", name, field);
	else
		printf("%s: record %ld: %s differs\n", name, record, field);
}

/* Compare a record of ours with its native twin, field by field. */
static void
note_record(native_result_t *result, const char *name, long record,
    const chk_entry_t *ours, const chk_entry_t *theirs)
{
	note_field(result, name, record, "presence", ours != NULL);
	if (ours == NULL)
		return;
	note_field(result, name, record, "mode", ours->mode == theirs->mode);
	note_field(result, name, record, "uid", ours->uid == theirs->uid);
	note_field(result, name, record, "gid", ours->gid == theirs->gid);
	note_field(result, name, record, "mtime", ours->mtime == theirs->mtime);
	note_field(result, name, record, "record length",
	    ours->length == theirs->length);
	note_field(result, name, record, "size", ours->size == theirs->size);
	note_field(result, name, record, "content",
	    ours->digest == theirs->digest);
}

/* Compare one native image with the writer's image of its tree. Every
 * native record must have its twin: the same path, kind, mode, owner,
 * time, record length and content. The writer's other records must be the
 * explicit records of the parents the native table leaves implicit, with
 * the values a reader synthesizes for them. The header, the mountpoint and
 * the root must agree. Inode numbers, record and data order, data
 * alignment and therefore the table and image sizes are free choices.
 */
static native_result_t
compare_native_one(const char *path, const char *name)
{
	fsimg_write_options_t options;
	native_result_t result;
	fixture_t fixture;
	memory_sink_t sink;
	const chk_entry_t *theirs, *ours;
	unsigned char *image;
	size_t length;
	uint32_t time;
	chk_t native, c;

	bzero(&result, sizeof(result));
	image = read_file(path, &length);
	chk_image(&native, image, length, false, false);
	if (native.errors != 0 || native.count == 0) {
		result.differences = 1;
		chk_free(&native);
		free(image);
		return (result);
	}
	/* The writer stores one time per image under -R; a tree whose times
	 * differ, such as mkifs -n's, cannot be expressed.
	 */
	time = native.entries[0].mtime;
	for (size_t i = 1; i < native.count; i++)
		if (native.entries[i].mtime != time)
			result.skipped = true;
	if (result.skipped) {
		chk_free(&native);
		free(image);
		return (result);
	}
	reproducible_options(&options);
	options.epoch = time;
	options.mountpoint = native.mountpoint;
	options.byte_order = native.big ? FSIMG_BYTE_ORDER_BIG :
	    FSIMG_BYTE_ORDER_LITTLE;
	fixture_open(&fixture, &options);
	fixture.native = &native;
	add_native_tree(&fixture, &native, time);
	CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
	CHECK(fixture.losses == 0);
	fixture_close(&fixture);
	chk_image(&c, sink.bytes, sink.length, true, false);
	CHECK(c.errors == 0);

	note_field(&result, name, -1, "byte order", c.big == native.big);
	note_field(&result, name, -1, "dir_offset",
	    c.dir_offset == native.dir_offset);
	note_field(&result, name, -1, "flags byte",
	    bcmp(sink.bytes + 7, image + 7, 1) == 0);
	note_field(&result, name, -1, "words after the sizes",
	    bcmp(sink.bytes + 0x14, image + 0x14, HEADER - 0x14) == 0);
	note_field(&result, name, -1, "mountpoint",
	    strcmp(c.mountpoint, native.mountpoint) == 0);
	note_field(&result, name, 0, "root path",
	    native.entries[0].path[0] == '\0');
	note_field(&result, name, 0, "root inode",
	    c.entries[0].inode == native.entries[0].inode);
	note_record(&result, name, 0, &c.entries[0], &native.entries[0]);
	for (size_t i = 1; i < native.count; i++) {
		theirs = &native.entries[i];
		ours = NULL;
		for (size_t j = 1; j < c.count && ours == NULL; j++)
			if (strcmp(c.entries[j].path, theirs->path) == 0)
				ours = &c.entries[j];
		note_record(&result, name, (long)i, ours, theirs);
	}
	for (size_t j = 1; j < c.count; j++) {
		theirs = NULL;
		for (size_t i = 1; i < native.count && theirs == NULL; i++)
			if (strcmp(c.entries[j].path, native.entries[i].path) == 0)
				theirs = &native.entries[i];
		if (theirs != NULL)
			continue;
		result.added_directories++;
		ours = &c.entries[j];
		note_field(&result, name, (long)j,
		    "metadata of our added directory record",
		    ours->mode == (T_DIR | 0755) && ours->uid == 0 &&
		    ours->gid == 0 && ours->mtime == time);
	}
	chk_free(&c);
	chk_free(&native);
	free(sink.bytes);
	free(image);
	return (result);
}

static int
compare_native(void)
{
	const char *root = getenv("FSIMG_IFS_NATIVE");
	native_result_t result;
	struct dirent *entry;
	char directory[1024], path[2048];
	unsigned compared, skipped, differing, added;
	DIR *dir;

	if (root == NULL || root[0] == '\0') {
		printf("SKIP: FSIMG_IFS_NATIVE is not set\n");
		return (77);
	}
	snprintf(directory, sizeof(directory), "%s/cases/ifs-basic", root);
	dir = opendir(directory);
	if (dir == NULL) {
		printf("SKIP: %s is absent\n", directory);
		return (77);
	}
	compared = skipped = differing = added = 0;
	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.' ||
		    strstr(entry->d_name, ".ifs") == NULL)
			continue;
		snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
		result = compare_native_one(path, entry->d_name);
		if (result.skipped) {
			printf("%s: times differ between records; not compared\n",
			    entry->d_name);
			skipped++;
			continue;
		}
		compared++;
		added += result.added_directories;
		differing += result.differences != 0;
	}
	CHECK(closedir(dir) == 0);
	printf("native: %u images compared, %u differing, %u not expressible; "
	    "%u directory records the native tables leave implicit\n",
	    compared, differing, skipped, added);
	if (compared == 0)
		return (77);
	return (differing == 0 ? 0 : 1);
}

/*
 * Native qualification images. Each case is one image of the native
 * qualification cycle (docs/ifs-research.md, "fsimg writer output"); the
 * plain run builds and checks every case in memory, and --create DIRECTORY
 * also writes DIRECTORY/NAME.ifs and DIRECTORY/NAME.manifest, which
 * tools/qnx/decode/ifsqual.py packs for tools/qnx/guest/qualifyifs.sh and
 * compares with what QNX 6.5.0 lists, extracts and rebuilds.
 *
 * The images are deliberately not reproducible, so that every entry
 * without a time of its own, directories included, carries the planning
 * clock: every case has the nonzero directory times mkifs never writes, and
 * the cycle shows whether dumpifs accepts them.
 */

/* A case's entries, built at run time because several paths are too long
 * to spell. Every path is allocated.
 */
typedef struct spec_list {
	spec_t		*specs;
	size_t		 count;
	size_t		 capacity;
} spec_list_t;

static void
spec_push(spec_list_t *list, const char *path, fsimg_entry_kind_t kind,
    uint64_t size, uint64_t source, const fsimg_metadata_t *metadata)
{
	spec_t *grown;
	char *copy;

	if (list->count == list->capacity) {
		list->capacity = list->capacity == 0 ? 64 : 2 * list->capacity;
		grown = realloc(list->specs, list->capacity *
		    sizeof(list->specs[0]));
		CHECK(grown != NULL);
		list->specs = grown;
	}
	copy = strdup(path);
	CHECK(copy != NULL);
	list->specs[list->count++] = (spec_t){ copy, kind, size, source,
	    metadata };
}

static void
spec_list_free(spec_list_t *list)
{
	for (size_t i = 0; i < list->count; i++)
		free((char *)(uintptr_t)list->specs[i].path);
	free(list->specs);
	bzero(list, sizeof(*list));
}

/* A slash, then length copies of c: a root-level path of one component,
 * or a component to append to a path, in a buffer of size bytes.
 */
static char *
slash_run(char *buffer, size_t size, char c, size_t length)
{
	CHECK(length + 2 <= size);
	buffer[0] = '/';
	memset(buffer + 1, c, length);
	buffer[1 + length] = '\0';
	return (buffer);
}

/* Directories /D, /D/D and so on to depth - 1, then the file /D/.../D at
 * depth, where D is the one-byte component c. Every case names each parent
 * directory itself; build_case() relies on that.
 */
static void
push_deep(spec_list_t *list, char c, unsigned depth, uint64_t source)
{
	char path[2 * 512 + 1];
	size_t length = 0;

	CHECK(depth >= 1 && depth <= 512);
	for (unsigned i = 1; i <= depth; i++) {
		path[length++] = '/';
		path[length++] = c;
		path[length] = '\0';
		if (i < depth)
			spec_push(list, path, FSIMG_ENTRY_DIRECTORY, 0, 0, NULL);
	}
	spec_push(list, path, FSIMG_ENTRY_FILE, 3, source, NULL);
}

/* E3 within what dumpifs shows and extracts: components of 1, 48, 49,
 * 255, 256 and 505 bytes (48 is the longest name the guest's RAM /tmp
 * holds, 505 the longest its QNX4 scratch volume holds); two-, three- and
 * four-byte UTF-8; a space; names differing only by case; depth 201; and a
 * 991-byte path, the longest whose file record fits the 1024-byte record
 * buffer dumpifs reads.
 */
static void
qualify_names(spec_list_t *list)
{
	static const unsigned lengths[] = { 1, 48, 49, 255, 256, 505 };
	static const char *const others[] = { "/caf\xc3\xa9",
	    "/\xe6\x97\xa5\xe6\x9c\xac", "/\xf0\x9f\x98\x80", "/with space",
	    "/case", "/Case", "/CASE" };
	char path[PATH_BYTES + 2], first[512], second[512];
	unsigned source = 20;

	for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++)
		spec_push(list, slash_run(path, sizeof(path), 'n', lengths[i]),
		    FSIMG_ENTRY_FILE, lengths[i] % 7, source++, NULL);
	for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++)
		spec_push(list, others[i], FSIMG_ENTRY_FILE, 2 + i, source++,
		    NULL);
	push_deep(list, 'd', 201, source++);
	/* /ppp and /ppp/qqq with 505- and 480-byte names, and the file
	 * /ppp/qqq/rrrr.
	 */
	slash_run(first, sizeof(first), 'p', 505);
	slash_run(second, sizeof(second), 'q', 480);
	spec_push(list, first, FSIMG_ENTRY_DIRECTORY, 0, 0, NULL);
	snprintf(path, sizeof(path), "%s%s", first, second);
	spec_push(list, path, FSIMG_ENTRY_DIRECTORY, 0, 0, NULL);
	CHECK(snprintf(path, sizeof(path), "%s%s/rrrr", first, second) ==
	    1 + 991);
	spec_push(list, path, FSIMG_ENTRY_FILE, 5, source, NULL);
}

/* E3 beyond what dumpifs shows whole: a 992-byte path, the shortest whose
 * file record outgrows dumpifs's 1024-byte record buffer; a single
 * 1024-byte component, which is also the longest path; and depth 512, the
 * most a path holds.
 */
static void
qualify_limits(spec_list_t *list)
{
	char path[PATH_BYTES + 2];

	spec_push(list, slash_run(path, sizeof(path), 's', 992),
	    FSIMG_ENTRY_FILE, 2, 29, NULL);
	spec_push(list, slash_run(path, sizeof(path), 'n', PATH_BYTES),
	    FSIMG_ENTRY_FILE, 4, 30, NULL);
	push_deep(list, 'a', 512, 31);
}

/* E4: the fixture tree (every kind, nested and case-differing names,
 * relative and 1024-byte link targets, a zero source) with absolute, "..",
 * non-ASCII link targets and an empty directory.
 */
static void
qualify_kinds(spec_list_t *list)
{
	for (size_t i = 0; i < TREE_COUNT; i++)
		spec_push(list, tree[i].path, tree[i].kind, tree[i].size,
		    tree[i].source, tree[i].metadata);
	spec_push(list, "/abs", FSIMG_ENTRY_SYMLINK, 20, 14, NULL);
	spec_push(list, "/dir/up", FSIMG_ENTRY_SYMLINK, 12, 15, NULL);
	spec_push(list, "/non-ascii", FSIMG_ENTRY_SYMLINK, 5, 16, NULL);
	spec_push(list, "/empty-dir", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL);
}

/* E5: owners 0, 65535 and 2^32 - 1 on files and nonzero owners on every
 * other kind; modes 0000, 0644, 0755, 4755 and 7777 on files, 0000, 0700
 * and 7777 on directories, 0755 on a link and 0600 and 7777 on FIFOs;
 * times 0, 2^31 - 1 and 2^32 - 1 on files and directories, 2^32 - 1 on
 * the link and 0 and 2^31 - 1 on the FIFOs.
 * Rows that test no time carry time 1. One file inside a directory has no
 * metadata of its own and takes the writer's defaults.
 */
static void
qualify_metadata(spec_list_t *list)
{
	static const struct {
		const char	*path;
		fsimg_entry_kind_t kind;
		uint64_t	 source;
		bool		 defaults;	/* no metadata of its own */
		uint32_t	 uid, gid, time;
		uint16_t	 mode;
	} rows[] = {
		{ .path = "/id-0", .kind = FSIMG_ENTRY_FILE, .source = 40,
		    .uid = 0, .gid = 0, .time = 1, .mode = 0644 },
		{ .path = "/id-65535", .kind = FSIMG_ENTRY_FILE, .source = 41,
		    .uid = 65535, .gid = 65535, .time = 1, .mode = 0644 },
		{ .path = "/id-max", .kind = FSIMG_ENTRY_FILE, .source = 42,
		    .uid = 4294967295u, .gid = 4294967295u, .time = 1, .mode = 0644 },
		{ .path = "/mode-0000", .kind = FSIMG_ENTRY_FILE, .source = 43,
		    .uid = 0, .gid = 0, .time = 1, .mode = 0 },
		{ .path = "/mode-0755", .kind = FSIMG_ENTRY_FILE, .source = 44,
		    .uid = 0, .gid = 0, .time = 1, .mode = 0755 },
		{ .path = "/mode-4755", .kind = FSIMG_ENTRY_FILE, .source = 45,
		    .uid = 0, .gid = 0, .time = 1, .mode = 04755 },
		{ .path = "/mode-7777", .kind = FSIMG_ENTRY_FILE, .source = 46,
		    .uid = 0, .gid = 0, .time = 1, .mode = 07777 },
		{ .path = "/time-0", .kind = FSIMG_ENTRY_FILE, .source = 47,
		    .uid = 0, .gid = 0, .time = 0, .mode = 0644 },
		{ .path = "/time-2038", .kind = FSIMG_ENTRY_FILE, .source = 48,
		    .uid = 0, .gid = 0, .time = 2147483647u, .mode = 0644 },
		{ .path = "/time-max", .kind = FSIMG_ENTRY_FILE, .source = 49,
		    .uid = 0, .gid = 0, .time = 4294967295u, .mode = 0644 },
		{ .path = "/dir-0700", .kind = FSIMG_ENTRY_DIRECTORY, .source = 0,
		    .uid = 7, .gid = 8, .time = 2147483647u, .mode = 0700 },
		{ .path = "/dir-0700/f", .kind = FSIMG_ENTRY_FILE, .source = 50,
		    .defaults = true },
		{ .path = "/dir-7777", .kind = FSIMG_ENTRY_DIRECTORY, .source = 0,
		    .uid = 4294967295u, .gid = 1, .time = 4294967295u, .mode = 07777 },
		{ .path = "/dir-0000", .kind = FSIMG_ENTRY_DIRECTORY, .source = 0,
		    .uid = 0, .gid = 0, .time = 0, .mode = 0 },
		{ .path = "/dir-0000/f", .kind = FSIMG_ENTRY_FILE, .source = 51,
		    .uid = 0, .gid = 0, .time = 0, .mode = 0 },
		{ .path = "/link", .kind = FSIMG_ENTRY_SYMLINK, .source = 3,
		    .uid = 3, .gid = 4, .time = 4294967295u, .mode = 0755 },
		{ .path = "/fifo", .kind = FSIMG_ENTRY_FIFO, .source = 0,
		    .uid = 5, .gid = 6, .time = 2147483647u, .mode = 0600 },
		{ .path = "/fifo-7777", .kind = FSIMG_ENTRY_FIFO, .source = 0,
		    .uid = 65535, .gid = 0, .time = 0, .mode = 07777 }
	};
	/* Static: each spec keeps a pointer to its row's metadata until the
	 * case's image is built, after this function returns.
	 */
	static fsimg_metadata_t metadata[sizeof(rows) / sizeof(rows[0])];
	uint64_t size;

	for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		metadata[i] = (fsimg_metadata_t){
			.valid = FSIMG_META_OWNER | FSIMG_META_MODE |
			    FSIMG_META_MODIFIED,
			.uid = rows[i].uid,
			.gid = rows[i].gid,
			.mode = rows[i].mode,
			.modified = { .seconds = rows[i].time,
			    .precision_ns = 1000000000, .basis = FSIMG_TIME_UTC }
		};
		/* Files hold 3 bytes; the link's target is "hello.txt". */
		size = rows[i].kind == FSIMG_ENTRY_FILE ? 3 :
		    rows[i].kind == FSIMG_ENTRY_SYMLINK ? 9 : 0;
		spec_push(list, rows[i].path, rows[i].kind, size, rows[i].source,
		    rows[i].defaults ? NULL : &metadata[i]);
	}
}

/* E6: sizes on both sides of 4 bytes and of 65536, two files with the same
 * contents, which the writer stores twice as mkifs does, and a zero source.
 */
static void
qualify_data(spec_list_t *list)
{
	static const uint64_t sizes[] = { 0, 1, 3, 4, 65535, 65536, 65537 };
	char path[32];

	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		snprintf(path, sizeof(path), "/f%" PRIu64, sizes[i]);
		spec_push(list, path, FSIMG_ENTRY_FILE, sizes[i], 60 + i, NULL);
	}
	spec_push(list, "/same-a", FSIMG_ENTRY_FILE, 65537, 70, NULL);
	spec_push(list, "/same-b", FSIMG_ENTRY_FILE, 65537, 70, NULL);
	spec_push(list, "/zero", FSIMG_ENTRY_FILE, 4096, 0, NULL);
}

static void
qualify_one(spec_list_t *list)
{
	spec_push(list, "/hello.txt", FSIMG_ENTRY_FILE, 9, 3, NULL);
}

static void
qualify_empty(spec_list_t *list FSIMG_UNUSED)
{
}

typedef struct qualify_case {
	const char	*name;
	void		(*build)(spec_list_t *);
	const char	*mountpoint;	/* NULL for the default */
	fsimg_byte_order_t byte_order;
} qualify_case_t;

/* The manifest of an image as the checker decodes it, one line per record
 * in table order and tab separated:
 *   H flags dir_offset hdr_dir_size image_size trailer mountpoint
 *   E path mode uid gid mtime inode data size [target]
 *   C path cksum size		(every regular file)
 * flags is the header's flags byte. Numbers are decimal but for
 * flags, the H offsets and sizes, mtime, data and the trailer in
 * hexadecimal and mode in octal, as dumpifs prints them; the trailer word
 * is in the image's byte order, while dumpifs prints it little-endian.
 * Paths are stored paths, the root's empty; cksum is the POSIX cksum CRC.
 */
static void
write_manifest(const chk_t *c, FILE *out)
{
	const chk_entry_t *e;
	uint32_t crc;

	/* The manifest's fields are tab separated, one record a line. */
	CHECK(strpbrk(c->mountpoint, "\t\n") == NULL);
	fprintf(out, "H\t%x\t%" PRIx32 "\t%" PRIx32 "\t%" PRIx32 "\t%" PRIx32
	    "\t%s\n", (unsigned)c->image[7], c->dir_offset, c->table_end,
	    c->image_size, get32(c, c->image + c->image_size - 4),
	    c->mountpoint);
	for (size_t i = 0; i < c->count; i++) {
		e = &c->entries[i];
		CHECK(strpbrk(e->path, "\t\n") == NULL);
		CHECK(e->target == NULL || strpbrk(e->target, "\t\n") == NULL);
		fprintf(out, "E\t%s\t%" PRIo32 "\t%" PRIu32 "\t%" PRIu32 "\t%"
		    PRIx32 "\t%" PRIu32 "\t%" PRIx32 "\t%" PRIu32 "%s%s\n",
		    e->path, e->mode, e->uid, e->gid, e->mtime, e->inode,
		    e->data, e->size, e->target != NULL ? "\t" : "",
		    e->target != NULL ? e->target : "");
		if ((e->mode & T_TYPE) != T_FILE)
			continue;
		crc = crc_update(0, c->image + e->data, e->size);
		fprintf(out, "C\t%s\t%" PRIu32 "\t%" PRIu32 "\n", e->path,
		    crc_finish(crc, e->size), e->size);
	}
}

/* Create parent/name exclusively, failing with its name. */
static FILE *
create_file(int parent, const char *name)
{
	FILE *out;
	int fd;

	fd = openat(parent, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
	    0644);
	if (fd == -1) {
		fprintf(stderr, "cannot create %s: %s\n", name,
		    strerror(errno));
		exit(1);
	}
	out = fdopen(fd, "w");
	CHECK(out != NULL);
	return (out);
}

/* Write a checked case's image and manifest into parent as NAME.ifs and
 * NAME.manifest, failing on any write error.
 */
static void
write_case(int parent, const char *case_name, const memory_sink_t *sink,
    const chk_t *c)
{
	char name[64];
	FILE *out;

	snprintf(name, sizeof(name), "%s.ifs", case_name);
	out = create_file(parent, name);
	CHECK(fwrite(sink->bytes, 1, (size_t)sink->length, out) ==
	    sink->length);
	CHECK(ferror(out) == 0 && fclose(out) == 0);
	snprintf(name, sizeof(name), "%s.manifest", case_name);
	out = create_file(parent, name);
	write_manifest(c, out);
	CHECK(ferror(out) == 0 && fclose(out) == 0);
	printf("%s: %" PRIu64 " bytes, %zu records\n", case_name,
	    sink->length, c->count);
}

/* Build one case's image over a poisoned sink, check it strictly with the
 * checker, which must decode the case's mountpoint, byte order and
 * exactly its entries and the root (so every builder names each parent
 * directory itself) with their contents, and read and verify it through
 * fsimg. With a parent directory, also write the image and its manifest
 * there.
 */
static void
build_case(const qualify_case_t *q, int parent)
{
	fsimg_write_options_t options;
	spec_list_t list;
	fixture_t fixture;
	memory_sink_t sink;
	bool header_ok;
	chk_t c;

	fsimg_write_options_init(&options);
	options.mountpoint = q->mountpoint;
	options.byte_order = q->byte_order;
	bzero(&list, sizeof(list));
	q->build(&list);
	fixture_open(&fixture, &options);
	add_specs(&fixture, list.specs, list.count, false);
	CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
	CHECK(fixture.losses == 0);
	chk_image(&c, sink.bytes, sink.length, true, false);
	/* The root carries the planning clock, which is never zero. */
	header_ok = c.errors == 0 && c.count == list.count + 1 &&
	    strcmp(c.mountpoint, q->mountpoint != NULL ? q->mountpoint :
	    "/") == 0 && c.big == (q->byte_order == FSIMG_BYTE_ORDER_BIG) &&
	    c.entries[0].mtime != 0;
	if (!header_ok)
		fprintf(stderr, "%s: the image's header or record count differs "
		    "from its case\n", q->name);
	CHECK(header_ok);
	for (size_t i = 0; i < list.count; i++)
		check_stored(&c, q->name, &list.specs[i], c.entries[0].mtime,
		    false);
	check_reader(fixture.ctx, &sink, &c);
	fixture_close(&fixture);

	if (parent != -1)
		write_case(parent, q->name, &sink, &c);
	chk_free(&c);
	spec_list_free(&list);
	free(sink.bytes);
}

/* Every qualification case; with a parent directory, written there. */
static void
build_cases(int parent)
{
	static char long_mountpoint[PATH_BYTES + 1];
	const qualify_case_t cases[] = {
		{ "one", qualify_one, NULL, FSIMG_BYTE_ORDER_LITTLE },
		{ "names", qualify_names, NULL, FSIMG_BYTE_ORDER_LITTLE },
		{ "limits", qualify_limits, NULL, FSIMG_BYTE_ORDER_LITTLE },
		{ "kinds", qualify_kinds, NULL, FSIMG_BYTE_ORDER_LITTLE },
		{ "metadata", qualify_metadata, NULL, FSIMG_BYTE_ORDER_LITTLE },
		{ "data", qualify_data, NULL, FSIMG_BYTE_ORDER_LITTLE },
		{ "empty", qualify_empty, NULL, FSIMG_BYTE_ORDER_LITTLE },
		{ "mount-empty", qualify_one, "", FSIMG_BYTE_ORDER_LITTLE },
		{ "mount-long", qualify_one, long_mountpoint,
		    FSIMG_BYTE_ORDER_LITTLE },
		{ "big-endian", qualify_kinds, NULL, FSIMG_BYTE_ORDER_BIG }
	};

	/* The longest mountpoint: 1024 bytes. */
	(void)slash_run(long_mountpoint, sizeof(long_mountpoint), 'm',
	    PATH_BYTES - 1);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		build_case(&cases[i], parent);
}

/* The qualification images build and check, so a writer change that
 * breaks one fails the plain run before anyone repeats the guest cycle.
 */
static void
test_qualification_images(void)
{
	static const unsigned char hello[] = "hello.txt";

	/* The manifest's checksum is POSIX cksum's: printf hello.txt | cksum */
	CHECK(crc_finish(crc_update(0, hello, 9), 9) == 233972757u);
	build_cases(-1);
}

static int
create_external(const char *directory)
{
	int parent;

	parent = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (parent == -1) {
		fprintf(stderr, "cannot open the directory %s: %s\n", directory,
		    strerror(errno));
		return (1);
	}
	build_cases(parent);
	CHECK(close(parent) == 0);
	return (0);
}

int
main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--native") == 0)
		return (compare_native());
	if (argc == 3 && strcmp(argv[1], "--create") == 0)
		return (create_external(argv[2]));
	CHECK(argc == 1);
	test_validation();
	test_stack_rule();
	test_small();
	test_matrix();
	test_tree_times();
	test_mountpoints();
	test_big_endian();
	test_names();
	test_metadata();
	test_links();
	test_checker();
	test_faults();
	test_injected();
	test_tampered_rows();
	test_limits();
	test_qualification_images();
	return (0);
}
