/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "erofs-gen.h"
#include "fsimg.h"
#include "test.h"

/*
 * Tests of full EROFS verification: fsimg_verify() over volumes built by the
 * independent generator in test/erofs-gen.c. Each test builds a small volume,
 * mutates one rule's worth of bytes and requires the status and the phrase
 * of the finding, so a case passes only through the rule it names. Every
 * expectation runs in both scopes, structural and payload, which must
 * agree. The name-filter oracle (XXH32) is implemented here from the
 * published algorithm and checked against published vectors, so the tests do
 * not depend on the library's hash. Every check is recorded, not fatal, so
 * one run lists every failing case.
 */

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* File modes, layouts and directory entry types of the format. */
enum {
	LAYOUT_FLAT = 0,
	LAYOUT_INLINE = 2,
	T_FILE = 1,
	T_DIR = 2,
	T_CHAR = 3,
	T_FIFO = 5,
	T_LINK = 7
};

#define MODE_FILE	0100644
#define MODE_DIR	040755
#define MODE_LINK	0120777
#define MODE_CHAR	020644
#define MODE_FIFO	010644

/* Superblock fields: byte offsets from the superblock, per the design. */
enum {
	SB_XATTR_BLKADDR = 0x2c,
	SB_INCOMPAT = 0x50,
	SB_PREFIX_COUNT = 0x5b,
	SB_PREFIX_START = 0x5c,
	SB_FILTER_BYTE = 0x68,
	SB_RESERVED_6A = 0x6a,
	SB_RESERVED_78 = 0x78
};

/* Feature bits. */
enum {
	COMPAT_SB_CHKSUM = 0x01,
	COMPAT_XATTR_FILTER = 0x04,
	COMPAT_PLAIN_XATTR_PFX = 0x10,
	COMPAT_UNKNOWN = 0x40,
	INCOMPAT_XATTR_PREFIXES = 0x40
};

enum {
	MEMORY_BYTES = 16 * 1024 * 1024,
	META_BYTES = 4096,		/* where the inodes of a volume start */
	SCOPE_COUNT = 2
};

static int failures;

static void
expectf(bool ok, const char *function, int line, const char *expression,
    const char *format, ...)
{
	va_list ap;

	if (ok)
		return;
	failures++;
	fprintf(stderr, "test/erofs-verify.c:%d: %s: case ", line, function);
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	fprintf(stderr, ": expected %s\n", expression);
}

#define EXPECT(ok, ...) \
	expectf((ok), __func__, __LINE__, #ok, __VA_ARGS__)

/* ---- Verifying an image in memory ---- */

typedef struct source {
	const unsigned char *bytes;
	uint64_t	 length;
} source_t;

static fsimg_status_t
source_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error)
{
	const source_t *source = argument;

	(void)error;
	CHECK(offset <= source->length && length <= source->length - offset);
	bcopy(source->bytes + offset, buffer, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
source_size(void *argument, uint64_t *size, fsimg_error_t *error)
{
	(void)error;
	*size = ((const source_t *)argument)->length;
	return (FSIMG_OK);
}

/* What fsimg_verify() answered for one scope. */
typedef struct verdict {
	fsimg_status_t	 status;
	fsimg_error_t	 error;
	fsimg_verify_report_t report;
} verdict_t;

/*
 * Open the image and either run fsimg_verify() (verify true) or list every
 * entry. The verdict's status is the first step that failed; a listing that
 * reaches its end is FSIMG_OK.
 */
static verdict_t
run_image(const gen_image_t *image, unsigned flags, bool verify,
    uint64_t memory_bytes)
{
	source_t source = { image->bytes, image->length };
	const fsimg_entry_t *walked;
	fsimg_reader_t *reader = NULL;
	fsimg_options_t options;
	fsimg_io_ops_t ops;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	verdict_t verdict;

	bzero(&verdict, sizeof(verdict));
	fsimg_options_init(&options);
	options.memory_bytes = memory_bytes;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	fsimg_io_ops_init(&ops);
	ops.read_at = source_read;
	ops.get_size = source_size;
	CHECK(fsimg_io_from_callbacks(ctx, &ops, &source, &io, NULL) ==
	    FSIMG_OK);
	verdict.status = fsimg_reader_open(ctx, io, NULL, &reader,
	    &verdict.error);
	if (verdict.status == FSIMG_OK && verify) {
		verdict.status = fsimg_verify(reader, flags, &verdict.report,
		    &verdict.error);
	} else if (verdict.status == FSIMG_OK) {
		while ((verdict.status = fsimg_reader_next(reader, &walked,
		    &verdict.error)) == FSIMG_OK)
			;
		if (verdict.status == FSIMG_DONE)
			verdict.status = FSIMG_OK;
	}
	fsimg_reader_free(reader);
	fsimg_io_free(io);
	fsimg_ctx_free(ctx);
	return (verdict);
}

static verdict_t
verify_image(const gen_image_t *image, unsigned flags)
{
	return (run_image(image, flags, true, MEMORY_BYTES));
}

/* The status of listing the image to its end. */
static fsimg_status_t
list_image(const gen_image_t *image)
{
	return (run_image(image, 0, false, MEMORY_BYTES).status);
}

static const unsigned scopes[SCOPE_COUNT] = { 0, FSIMG_VERIFY_PAYLOAD };

static const char *
scope_name(unsigned flags)
{
	return (flags == 0 ? "structural" : "payload");
}

/*
 * One verdict against an expectation: the status and, for a refusal, a
 * phrase of the message. A success must be complete; a failure must not be.
 */
static void
check_verdict(const char *name, unsigned flags, const verdict_t *verdict,
    fsimg_status_t want, const char *phrase)
{
	EXPECT(verdict->status == want, "%s (%s): status %d, expected %d (%s)",
	    name, scope_name(flags), (int)verdict->status, (int)want,
	    verdict->error.message);
	EXPECT((want == FSIMG_OK) == verdict->report.complete,
	    "%s (%s): complete is %d", name, scope_name(flags),
	    (int)verdict->report.complete);
	if (phrase != NULL && verdict->status != FSIMG_OK)
		EXPECT(strstr(verdict->error.message, phrase) != NULL,
		    "%s (%s): message \"%s\" lacks \"%s\"", name,
		    scope_name(flags), verdict->error.message, phrase);
}

/* Both scopes must answer the same way. */
static void
check_image(const char *name, const gen_image_t *image, fsimg_status_t want,
    const char *phrase)
{
	for (size_t i = 0; i < SCOPE_COUNT; i++) {
		verdict_t verdict = verify_image(image, scopes[i]);

		check_verdict(name, scopes[i], &verdict, want, phrase);
	}
}

/* The tree sealed and verified in both scopes. */
static void
check_tree(const char *name, gen_tree_t *tree, fsimg_status_t want,
    const char *phrase)
{
	gen_tree_seal(tree);
	check_image(name, &tree->image, want, phrase);
}

static void
expect_verified(const char *name, gen_tree_t *tree)
{
	check_tree(name, tree, FSIMG_OK, NULL);
}

/* The finding of a structural defect: ECORRUPT, "invalid EROFS <phrase>". */
static void
expect_corrupt(const char *name, gen_tree_t *tree, const char *phrase)
{
	char full[256];

	snprintf(full, sizeof(full), "invalid EROFS %s", phrase);
	check_tree(name, tree, FSIMG_ECORRUPT, full);
}

/* ---- Building volumes ---- */

static void
tree_new(gen_tree_t *tree, unsigned blkszbits, unsigned data_blocks)
{
	gen_tree_init(tree, blkszbits, gen_tree_first_block(blkszbits) +
	    data_blocks);
}

static size_t
block_bytes(const gen_tree_t *tree)
{
	return ((size_t)1 << tree->blkszbits);
}

static gen_inode_t
inode_proto(uint16_t mode, unsigned layout)
{
	gen_inode_t inode;

	bzero(&inode, sizeof(inode));
	inode.mode = mode;
	inode.nlink = 1;
	inode.layout = (uint8_t)layout;
	return (inode);
}

static gen_dirent_t
entry(const char *name, uint64_t nid, uint16_t type)
{
	return ((gen_dirent_t){ name, nid, type });
}

/* A file of size bytes of test content under a data layout; its NID. */
static uint64_t
add_file(gen_tree_t *tree, unsigned layout, size_t size)
{
	gen_inode_t proto = inode_proto(MODE_FILE, layout);
	unsigned char *bytes = calloc(size != 0 ? size : 1, 1);
	uint64_t nid;

	CHECK(bytes != NULL);
	for (size_t i = 0; i < size; i++)
		bytes[i] = (unsigned char)(i * 31 + (i >> 8) * 7 + 1);
	nid = gen_tree_add(tree, &proto, bytes, size);
	free(bytes);
	return (nid);
}

/* A kind that stores no content: a device or a FIFO. */
static uint64_t
add_special(gen_tree_t *tree, uint16_t mode)
{
	gen_inode_t proto = inode_proto(mode, LAYOUT_FLAT);

	return (gen_tree_add(tree, &proto, NULL, 0));
}

static uint64_t
add_symlink(gen_tree_t *tree, const char *target)
{
	gen_inode_t proto = inode_proto(MODE_LINK, LAYOUT_INLINE);

	return (gen_tree_add(tree, &proto, target, strlen(target)));
}

/* An inode slot for a directory to be filled by fill_dir(). */
static uint64_t
new_dir(gen_tree_t *tree)
{
	return (gen_tree_slot(tree, GEN_COMPACT_BYTES, 0));
}

static uint64_t
inode_offset(const gen_tree_t *tree, uint64_t nid)
{
	return (gen_inode_offset(&tree->image, nid));
}

static bool
inode_is_extended(const gen_tree_t *tree, uint64_t nid)
{
	return ((tree->image.bytes[inode_offset(tree, nid)] & 1) != 0);
}

/* The link count field of the inode, whichever form it has. */
static void
set_nlink(gen_tree_t *tree, uint64_t nid, uint32_t nlink)
{
	uint64_t at = inode_offset(tree, nid);

	if (inode_is_extended(tree, nid)) {
		gen_put(&tree->image, at + 0x2c, nlink, 4);
	} else {
		CHECK(nlink <= 65535);
		gen_put(&tree->image, at + 0x06, nlink, 2);
	}
}

/* The first data block of a flat inode, held by its layout word. */
static uint32_t
start_block(const gen_tree_t *tree, uint64_t nid)
{
	const unsigned char *p = tree->image.bytes + inode_offset(tree, nid) +
	    0x10;

	return ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	    (uint32_t)p[3] << 24);
}

static void
set_start_block(gen_tree_t *tree, uint64_t nid, uint32_t block)
{
	gen_put(&tree->image, inode_offset(tree, nid) + 0x10, block, 4);
}

/*
 * A directory with correct "." and ".." and the link count that its
 * subdirectories imply: 2 plus one per child of type T_DIR.
 */
static void
fill_dir(gen_tree_t *tree, uint64_t nid, uint64_t parent,
    const gen_dirent_t *kids, size_t count)
{
	uint32_t nlink = 2;

	gen_tree_dir(tree, nid, parent, kids, count);
	for (size_t i = 0; i < count; i++) {
		if (kids[i].type == T_DIR)
			nlink++;
	}
	set_nlink(tree, nid, nlink);
}

/* A data block of its own; the generator's blocks are handed out in order. */
static uint32_t
new_block(gen_tree_t *tree)
{
	CHECK(tree->data_next < tree->blocks);
	return (tree->data_next++);
}

static void
put_super(gen_tree_t *tree, unsigned field, uint64_t value, unsigned width)
{
	gen_super_put(&tree->image, field, value, width);
}

/* ---- Name-filter oracle ---- */

/*
 * XXH32 from the published algorithm: four accumulators over 16-byte
 * stripes for a message of 16 bytes or more, a single seeded one below, then
 * the length, the trailing words and bytes and the avalanche. It is checked
 * against the algorithm's published vectors before any test relies on it.
 */
static uint32_t
rotl(uint32_t value, unsigned bits)
{
	return (value << bits | value >> (32 - bits));
}

static uint32_t
load32(const unsigned char *p)
{
	return ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	    (uint32_t)p[3] << 24);
}

static uint32_t
xxh32(const void *input, size_t length, uint32_t seed)
{
	const uint32_t p1 = UINT32_C(2654435761), p2 = UINT32_C(2246822519),
	    p3 = UINT32_C(3266489917), p4 = UINT32_C(668265263),
	    p5 = UINT32_C(374761393);
	const unsigned char *p = input, *end = p + length;
	uint32_t hash;

	if (length >= 16) {
		uint32_t v[4] = { seed + p1 + p2, seed + p2, seed, seed - p1 };

		for (; end - p >= 16; p += 16) {
			for (unsigned lane = 0; lane < 4; lane++)
				v[lane] = rotl(v[lane] +
				    load32(p + 4 * lane) * p2, 13) * p1;
		}
		hash = rotl(v[0], 1) + rotl(v[1], 7) + rotl(v[2], 12) +
		    rotl(v[3], 18);
	} else {
		hash = seed + p5;
	}
	hash += (uint32_t)length;
	for (; end - p >= 4; p += 4)
		hash = rotl(hash + load32(p) * p3, 17) * p4;
	for (; p < end; p++)
		hash = rotl(hash + *p * p5, 11) * p1;
	hash ^= hash >> 15;
	hash *= p2;
	hash ^= hash >> 13;
	hash *= p3;
	hash ^= hash >> 16;
	return (hash);
}

static void
test_the_filter_hash_matches_published_vectors(void)
{
	static const char message[] =
	    "Nobody inspects the spammish repetition";

	EXPECT(xxh32("", 0, 0) == UINT32_C(0x02cc5d05), "empty message");
	EXPECT(xxh32("a", 1, 0) == UINT32_C(0x550d7456), "one byte");
	EXPECT(xxh32("abc", 3, 0) == UINT32_C(0x32d153ff), "three bytes");
	EXPECT(xxh32(message, strlen(message), 0) == UINT32_C(0xe2293b2f),
	    "a message of two stripes");
}

/* The filter seed of the design: the base seed plus the built-in index. */
#define FILTER_SEED	UINT32_C(0x25bbe08f)

/*
 * The filter bit of an attribute named by a built-in index, whose name
 * after the built-in prefix is the infix followed by the suffix, as the
 * design's name filter states.
 */
static unsigned
filter_bit(unsigned base_index, const char *infix, const char *suffix)
{
	size_t length = strlen(infix) + strlen(suffix);
	char *name = malloc(length + 1);
	unsigned bit;

	CHECK(name != NULL);
	snprintf(name, length + 1, "%s%s", infix, suffix);
	bit = xxh32(name, length, FILTER_SEED + base_index) & 31;
	free(name);
	return (bit);
}

/* ---- Attributes ---- */

typedef struct attr {
	unsigned	 index;
	const char	*suffix;
	const char	*value;
} attr_t;

/* The region of an inode: the filter word, shared indexes, inline entries. */
typedef struct region {
	uint32_t	 filter;
	const uint32_t	*shared;
	size_t		 shared_count;
	const attr_t	*inline_attrs;
	size_t		 inline_count;
} region_t;

static size_t
entry_bytes(const attr_t *attr)
{
	return ((4 + strlen(attr->suffix) + strlen(attr->value) + 3) &
	    ~(size_t)3);
}

static size_t
region_bytes(const region_t *region)
{
	size_t bytes = 12 + 4 * region->shared_count;

	for (size_t i = 0; i < region->inline_count; i++)
		bytes += entry_bytes(&region->inline_attrs[i]);
	return (bytes);
}

/* The inline attribute entry written at a volume offset; its padded size. */
static size_t
put_entry(gen_tree_t *tree, uint64_t at, const attr_t *attr)
{
	return (gen_xattr_entry(&tree->image, at, attr->index, attr->suffix,
	    attr->value, strlen(attr->value)));
}

/* A compact file of no content carrying the region; its NID. */
static uint64_t
add_file_with_region(gen_tree_t *tree, const region_t *region)
{
	gen_inode_t proto = inode_proto(MODE_FILE, LAYOUT_FLAT);
	size_t bytes = region_bytes(region);
	uint64_t nid, at;

	proto.xattr_count = (uint16_t)(1 + (bytes - 12) / 4);
	nid = gen_tree_add(tree, &proto, NULL, 0);
	at = inode_offset(tree, nid) + GEN_COMPACT_BYTES;
	gen_put(&tree->image, at, region->filter, 4);
	gen_put(&tree->image, at + 4, region->shared_count, 1);
	at += 12;
	for (size_t i = 0; i < region->shared_count; i++, at += 4)
		gen_put(&tree->image, at, region->shared[i], 4);
	for (size_t i = 0; i < region->inline_count; i++) {
		const attr_t *attr = &region->inline_attrs[i];

		at += put_entry(tree, at, attr);
	}
	return (nid);
}

/* Make block the first block of the shared pool: index x names 4 * x. */
static void
set_pool(gen_tree_t *tree, uint32_t block)
{
	put_super(tree, SB_XATTR_BLKADDR, block, 4);
}

static uint64_t
block_offset(const gen_tree_t *tree, uint32_t block)
{
	return ((uint64_t)block * block_bytes(tree));
}

/*
 * Long prefixes: the table at volume byte at, each prefix a base index and
 * an infix. The table is plain (PLAIN_XATTR_PFX), so the volume needs no
 * packed inode.
 */
typedef struct prefix {
	unsigned	 base;
	const char	*infix;
} prefix_t;

static size_t
put_prefix(gen_tree_t *tree, uint64_t at, const prefix_t *prefix)
{
	size_t infix = strlen(prefix->infix);

	gen_put(&tree->image, at, infix + 1, 2);
	gen_put(&tree->image, at + 2, prefix->base, 1);
	gen_put_bytes(&tree->image, at + 3, prefix->infix, infix);
	return ((2 + infix + 1 + 3) & ~(size_t)3);
}

/* Declare count prefixes starting at the 4-aligned volume byte at. */
static void
set_prefix_table(gen_tree_t *tree, uint64_t at, unsigned count)
{
	CHECK(at % 4 == 0);
	put_super(tree, SB_INCOMPAT, INCOMPAT_XATTR_PREFIXES, 4);
	put_super(tree, SB_PREFIX_COUNT, count, 1);
	put_super(tree, SB_PREFIX_START, at / 4, 4);
	gen_tree_set_compat(tree, COMPAT_SB_CHKSUM | COMPAT_PLAIN_XATTR_PFX);
}

/*
 * Write the prefixes at the start of a block of their own, reserve every
 * block the table runs into, and declare them.
 */
static void
add_prefix_table(gen_tree_t *tree, const prefix_t *prefixes, size_t count)
{
	uint64_t start = block_offset(tree, new_block(tree));
	uint64_t at = start;

	for (size_t i = 0; i < count; i++)
		at += put_prefix(tree, at, &prefixes[i]);
	while (at > block_offset(tree, tree->data_next))
		new_block(tree);
	set_prefix_table(tree, start, (unsigned)count);
}

/* ---- Clean volumes ---- */

/*
 * A volume of every kind the format stores:
 *
 *	/hard1 /hard2	one file, nlink 2, two names in one directory
 *	/a/x1 /a/b/x2	one file, nlink 2, names in two directories
 *	/sym /dev /fifo	a symbolic link, a device and a FIFO
 *	/a/b/inline	a block of data and an inline tail
 *	/a/b/flat	whole blocks and a last partial one, no tail
 *	/d		an empty directory
 */
enum {
	HARD_BYTES = 300,
	SHARED_BYTES = 50,
	INLINE_BYTES = 100,
	SYMLINK_BYTES = 10
};

typedef struct clean {
	gen_tree_t	 tree;
	uint64_t	 files;			/* delivered non-directories */
	uint64_t	 directories;
	uint64_t	 logical_bytes;
} clean_t;

static void
clean_new(clean_t *clean, unsigned blkszbits)
{
	size_t block = (size_t)1 << blkszbits;
	size_t flat_bytes = 2 * block + 7;
	size_t inline_bytes = block + INLINE_BYTES;
	uint64_t hard, shared, sym, dev, fifo, inline_file, flat;
	uint64_t a, b, d;
	gen_dirent_t kids[8];

	tree_new(&clean->tree, blkszbits, 24);
	hard = add_file(&clean->tree, LAYOUT_INLINE, HARD_BYTES);
	shared = add_file(&clean->tree, LAYOUT_INLINE, SHARED_BYTES);
	sym = add_symlink(&clean->tree, "a/b/inline");
	dev = add_special(&clean->tree, MODE_CHAR);
	fifo = add_special(&clean->tree, MODE_FIFO);
	inline_file = add_file(&clean->tree, LAYOUT_INLINE, inline_bytes);
	flat = add_file(&clean->tree, LAYOUT_FLAT, flat_bytes);
	a = new_dir(&clean->tree);
	b = new_dir(&clean->tree);
	d = new_dir(&clean->tree);
	set_nlink(&clean->tree, hard, 2);
	set_nlink(&clean->tree, shared, 2);

	kids[0] = entry("inline", inline_file, T_FILE);
	kids[1] = entry("flat", flat, T_FILE);
	kids[2] = entry("x2", shared, T_FILE);
	fill_dir(&clean->tree, b, a, kids, 3);
	kids[0] = entry("b", b, T_DIR);
	kids[1] = entry("x1", shared, T_FILE);
	fill_dir(&clean->tree, a, 0, kids, 2);
	fill_dir(&clean->tree, d, 0, NULL, 0);
	kids[0] = entry("a", a, T_DIR);
	kids[1] = entry("d", d, T_DIR);
	kids[2] = entry("hard1", hard, T_FILE);
	kids[3] = entry("hard2", hard, T_FILE);
	kids[4] = entry("sym", sym, T_LINK);
	kids[5] = entry("dev", dev, T_CHAR);
	kids[6] = entry("fifo", fifo, T_FIFO);
	fill_dir(&clean->tree, 0, 0, kids, 7);

	clean->files = 9;
	clean->directories = 4;
	clean->logical_bytes = 2 * HARD_BYTES + 2 * SHARED_BYTES +
	    SYMLINK_BYTES + inline_bytes + flat_bytes;
}

static void
test_clean_volumes_of_every_kind_verify_complete_in_both_block_sizes(void)
{
	static const unsigned sizes[] = { 9, 12 };

	for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
		char name[64];
		clean_t clean;

		snprintf(name, sizeof(name), "clean volume, block size %u",
		    1u << sizes[i]);
		clean_new(&clean, sizes[i]);
		expect_verified(name, &clean.tree);
		gen_tree_free(&clean.tree);
	}
}

static void
test_the_report_counts_what_the_walk_delivered(void)
{
	static const unsigned sizes[] = { 9, 12 };

	for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
		clean_t clean;

		clean_new(&clean, sizes[i]);
		gen_tree_seal(&clean.tree);
		for (size_t s = 0; s < SCOPE_COUNT; s++) {
			verdict_t v = verify_image(&clean.tree.image, scopes[s]);

			EXPECT(v.status == FSIMG_OK, "block size %u (%s): %s",
			    1u << sizes[i], scope_name(scopes[s]),
			    v.error.message);
			EXPECT(v.report.files == clean.files,
			    "block size %u (%s): %llu files, expected %llu",
			    1u << sizes[i], scope_name(scopes[s]),
			    (unsigned long long)v.report.files,
			    (unsigned long long)clean.files);
			EXPECT(v.report.directories == clean.directories,
			    "block size %u (%s): %llu directories",
			    1u << sizes[i], scope_name(scopes[s]),
			    (unsigned long long)v.report.directories);
			EXPECT(v.report.logical_bytes == clean.logical_bytes,
			    "block size %u (%s): %llu logical bytes, expected "
			    "%llu", 1u << sizes[i], scope_name(scopes[s]),
			    (unsigned long long)v.report.logical_bytes,
			    (unsigned long long)clean.logical_bytes);
			EXPECT(v.report.omitted_entries == 0,
			    "block size %u (%s): omitted entries",
			    1u << sizes[i], scope_name(scopes[s]));
			EXPECT((v.report.payload_bytes_read > 0) ==
			    (scopes[s] == FSIMG_VERIFY_PAYLOAD),
			    "block size %u (%s): %llu payload bytes",
			    1u << sizes[i], scope_name(scopes[s]),
			    (unsigned long long)v.report.payload_bytes_read);
		}
		gen_tree_free(&clean.tree);
	}
}

/* ---- Link counts ---- */

/*
 * The root naming one file under the given number of names, with the link
 * count given. The volume verifies exactly when the two agree; otherwise
 * the finding names the count, the inode and the number of names.
 */
static void
file_case(const char *name, unsigned names, uint32_t nlink)
{
	static const char *const labels[] = { "n0", "n1", "n2", "n3" };
	gen_dirent_t kids[ARRAY_SIZE(labels)];
	char phrase[128];
	gen_tree_t tree;
	uint64_t file;

	CHECK(names >= 1 && names <= ARRAY_SIZE(labels));
	tree_new(&tree, 9, 4);
	file = add_file(&tree, LAYOUT_INLINE, 40);
	for (unsigned i = 0; i < names; i++)
		kids[i] = entry(labels[i], file, T_FILE);
	fill_dir(&tree, 0, 0, kids, names);
	set_nlink(&tree, file, nlink);
	if (nlink == names) {
		expect_verified(name, &tree);
	} else {
		snprintf(phrase, sizeof(phrase),
		    "link count %u of inode %llu for %u names", (unsigned)nlink,
		    (unsigned long long)file, names);
		expect_corrupt(name, &tree, phrase);
	}
	gen_tree_free(&tree);
}

static void
test_a_file_link_count_below_its_names_is_corrupt(void)
{
	file_case("nlink 2 for 3 names", 3, 2);
	file_case("nlink 0 for 1 name", 1, 0);
}

static void
test_a_file_link_count_above_its_names_is_corrupt(void)
{
	file_case("nlink 2 for 1 name", 1, 2);
	file_case("nlink 5 for 3 names", 3, 5);
}

static void
test_a_file_link_count_equal_to_its_names_verifies(void)
{
	file_case("nlink 1 for 1 name", 1, 1);
	file_case("nlink 3 for 3 names", 3, 3);
}

static void
test_a_second_name_for_a_file_with_link_count_1_is_corrupt(void)
{
	gen_dirent_t root_kids[2], dir_kids[1];
	char phrase[128];
	gen_tree_t tree;
	uint64_t file, dir;

	tree_new(&tree, 9, 4);
	file = add_file(&tree, LAYOUT_INLINE, 40);
	dir = new_dir(&tree);
	dir_kids[0] = entry("second", file, T_FILE);
	fill_dir(&tree, dir, 0, dir_kids, 1);
	root_kids[0] = entry("d", dir, T_DIR);
	root_kids[1] = entry("first", file, T_FILE);
	fill_dir(&tree, 0, 0, root_kids, 2);
	snprintf(phrase, sizeof(phrase),
	    "link count 1 of inode %llu for 2 names", (unsigned long long)file);
	expect_corrupt("two names in two directories, nlink 1", &tree, phrase);
	gen_tree_free(&tree);
}

static void
test_a_directory_named_twice_in_one_parent_is_corrupt(void)
{
	gen_dirent_t root_kids[2];
	char phrase[128];
	gen_tree_t tree;
	uint64_t dir;

	tree_new(&tree, 9, 4);
	dir = new_dir(&tree);
	fill_dir(&tree, dir, 0, NULL, 0);
	root_kids[0] = entry("d1", dir, T_DIR);
	root_kids[1] = entry("d2", dir, T_DIR);
	fill_dir(&tree, 0, 0, root_kids, 2);
	snprintf(phrase, sizeof(phrase), "directory inode %llu with 2 names",
	    (unsigned long long)dir);
	expect_corrupt("directory named d1 and d2", &tree, phrase);
	gen_tree_free(&tree);
}

/* A root with one empty subdirectory, whose link counts the cases change. */
static void
one_subdirectory(gen_tree_t *tree, uint64_t *dir)
{
	gen_dirent_t root_kids[1];

	tree_new(tree, 9, 4);
	*dir = new_dir(tree);
	fill_dir(tree, *dir, 0, NULL, 0);
	root_kids[0] = entry("d", *dir, T_DIR);
	fill_dir(tree, 0, 0, root_kids, 1);
}

static void
test_a_directory_link_count_above_its_subdirectories_is_corrupt(void)
{
	char phrase[128];
	gen_tree_t tree;
	uint64_t dir;

	one_subdirectory(&tree, &dir);
	set_nlink(&tree, dir, 3);
	snprintf(phrase, sizeof(phrase),
	    "directory link count 3 of inode %llu for 0 subdirectories",
	    (unsigned long long)dir);
	expect_corrupt("empty directory with nlink 3", &tree, phrase);
	gen_tree_free(&tree);
}

static void
test_a_directory_link_count_below_its_subdirectories_is_corrupt(void)
{
	gen_dirent_t kids[1];
	char phrase[128];
	gen_tree_t tree;
	uint64_t dir, child;

	tree_new(&tree, 9, 4);
	dir = new_dir(&tree);
	child = new_dir(&tree);
	fill_dir(&tree, child, dir, NULL, 0);
	kids[0] = entry("c", child, T_DIR);
	fill_dir(&tree, dir, 0, kids, 1);
	set_nlink(&tree, dir, 2);
	kids[0] = entry("d", dir, T_DIR);
	fill_dir(&tree, 0, 0, kids, 1);
	snprintf(phrase, sizeof(phrase),
	    "directory link count 2 of inode %llu for 1 subdirectories",
	    (unsigned long long)dir);
	expect_corrupt("directory with a subdirectory and nlink 2", &tree,
	    phrase);
	gen_tree_free(&tree);
}

static void
test_a_wrong_root_link_count_is_corrupt(void)
{
	gen_tree_t tree;
	uint64_t dir;

	one_subdirectory(&tree, &dir);
	set_nlink(&tree, 0, 5);
	expect_corrupt("root with nlink 5 and one subdirectory", &tree,
	    "directory link count 5 of inode 0 for 1 subdirectories");
	set_nlink(&tree, 0, 2);
	expect_corrupt("root with nlink 2 and one subdirectory", &tree,
	    "directory link count 2 of inode 0 for 1 subdirectories");
	set_nlink(&tree, 0, 3);
	expect_verified("root with nlink 3 and one subdirectory", &tree);
	gen_tree_free(&tree);
}

/* ---- Omitted entries ---- */

/* The root naming one file by a name that is not UTF-8, and by good names. */
static void
omitted_case(const char *name, unsigned good_names, uint32_t nlink,
    bool corrupt)
{
	static const char invalid[] = "\xff";
	gen_dirent_t kids[2];
	char phrase[128];
	gen_tree_t tree;
	uint64_t file;
	size_t count = 0;

	tree_new(&tree, 9, 4);
	file = add_file(&tree, LAYOUT_INLINE, 40);
	kids[count++] = entry(invalid, file, T_FILE);
	if (good_names > 0)
		kids[count++] = entry("good", file, T_FILE);
	fill_dir(&tree, 0, 0, kids, count);
	set_nlink(&tree, file, nlink);
	if (corrupt) {
		snprintf(phrase, sizeof(phrase),
		    "link count %u of inode %llu for %zu names", (unsigned)nlink,
		    (unsigned long long)file, count);
		expect_corrupt(name, &tree, phrase);
	} else {
		gen_tree_seal(&tree);
		check_image(name, &tree.image, FSIMG_OK, NULL);
		for (size_t s = 0; s < SCOPE_COUNT; s++) {
			verdict_t v = verify_image(&tree.image, scopes[s]);

			EXPECT(v.report.omitted_entries == 1,
			    "%s (%s): %llu omitted entries", name,
			    scope_name(scopes[s]),
			    (unsigned long long)v.report.omitted_entries);
		}
	}
	gen_tree_free(&tree);
}

static void
test_an_omitted_entry_counts_as_a_name_of_its_inode(void)
{
	omitted_case("omitted name, nlink 1", 0, 1, false);
	omitted_case("omitted and a good name, nlink 2", 1, 2, false);
}

static void
test_an_omitted_entry_with_a_wrong_link_count_is_corrupt(void)
{
	omitted_case("omitted name, nlink 2", 0, 2, true);
	omitted_case("omitted and a good name, nlink 1", 1, 1, true);
}

/* ---- Directories with more than 65535 links ---- */

/*
 * A root of many empty subdirectories: with 65534, 2 plus them is 65536, one
 * more than a compact inode's link count field holds; with 65533 it is
 * exactly 65535, the most the field holds. The subdirectories are inline
 * directories of 64 bytes each, after a root of 64 bytes (room for either
 * form); the root's entries fill whole blocks after the inodes.
 */
enum {
	BIG_CHILDREN = 65534,
	BIG_CHILDREN_FITTING = 65533,
	BIG_BITS = 12,
	BIG_ROOT_BLOCKS = 320,
	BIG_NID_STRIDE = 2,
	BIG_FIRST_NID = 2,
	BIG_MEMORY = 256 * 1024 * 1024
};

typedef struct big {
	gen_image_t	 image;
	unsigned	 children;	/* the root's subdirectories */
	uint32_t	 dir_block;	/* where the root's entries start */
	size_t		 dir_size;
} big_t;

/* The root inode in the form and with the link count given. */
static void
big_root(big_t *big, bool extended, uint32_t nlink)
{
	gen_inode_t root = inode_proto(MODE_DIR, LAYOUT_FLAT);

	root.nid = 0;
	root.extended = extended;
	root.nlink = nlink;
	root.size = big->dir_size;
	root.layout_word = big->dir_block;
	gen_inode_write(&big->image, &root);
}

static void
big_new(big_t *big, unsigned children)
{
	size_t block = (size_t)1 << BIG_BITS;
	uint64_t meta_end = META_BYTES + 32 * (BIG_FIRST_NID +
	    (uint64_t)BIG_NID_STRIDE * children);
	uint32_t blocks;
	gen_dirent_t *records = calloc(children + 2, sizeof(*records));
	char (*names)[8] = calloc(children, sizeof(*names));
	unsigned char *tail = calloc(1, block);
	unsigned char *root_blocks = calloc(BIG_ROOT_BLOCKS, block);
	gen_super_t sb;

	CHECK(records != NULL && names != NULL && tail != NULL &&
	    root_blocks != NULL);
	bzero(big, sizeof(*big));
	big->children = children;
	big->dir_block = (uint32_t)((meta_end + block - 1) / block);
	blocks = big->dir_block + BIG_ROOT_BLOCKS;
	gen_image_init(&big->image, (size_t)blocks << BIG_BITS);
	sb = gen_super_default(BIG_BITS, blocks);
	sb.inode_count = children + 1;
	gen_super_write(&big->image, &sb);

	records[0] = entry(".", 0, T_DIR);
	records[1] = entry("..", 0, T_DIR);
	for (unsigned i = 0; i < children; i++) {
		uint64_t nid = BIG_FIRST_NID + (uint64_t)BIG_NID_STRIDE * i;
		gen_dirent_t dots[2] = { entry(".", nid, T_DIR),
		    entry("..", 0, T_DIR) };
		gen_inode_t child = inode_proto(MODE_DIR, LAYOUT_INLINE);
		size_t size;

		snprintf(names[i], sizeof(names[i]), "d%05u", i);
		records[i + 2] = entry(names[i], nid, T_DIR);
		size = gen_dir_pack(tail, block, block, dots, 2);
		child.nid = nid;
		child.nlink = 2;
		child.size = size;
		gen_inode_write(&big->image, &child);
		gen_put_bytes(&big->image, gen_inode_offset(&big->image, nid) +
		    GEN_COMPACT_BYTES, tail, size);
	}
	big->dir_size = gen_dir_pack(root_blocks, BIG_ROOT_BLOCKS * block,
	    block, records, children + 2);
	gen_put_bytes(&big->image, (uint64_t)big->dir_block << BIG_BITS,
	    root_blocks, (big->dir_size + block - 1) / block * block);
	free(root_blocks);
	free(tail);
	free(names);
	free(records);
}

static void
big_check(const char *name, big_t *big, bool extended, uint32_t nlink,
    bool verifies)
{
	char phrase[128];
	verdict_t v;

	big_root(big, extended, nlink);
	gen_seal(&big->image);
	v = run_image(&big->image, 0, true, BIG_MEMORY);
	EXPECT(v.status == (verifies ? FSIMG_OK : FSIMG_ECORRUPT),
	    "%s: status %d (%s)", name, (int)v.status, v.error.message);
	EXPECT(v.report.complete == verifies, "%s: complete is %d", name,
	    (int)v.report.complete);
	if (verifies)
		return;
	snprintf(phrase, sizeof(phrase),
	    "invalid EROFS directory link count %u of inode 0 for %u "
	    "subdirectories", (unsigned)nlink, big->children);
	EXPECT(strstr(v.error.message, phrase) != NULL,
	    "%s: message \"%s\" lacks \"%s\"", name, v.error.message, phrase);
}

static void
test_a_compact_directory_past_65535_links_may_record_1(void)
{
	big_t big;

	big_new(&big, BIG_CHILDREN);
	big_check("compact root, 65534 subdirectories, nlink 1", &big, false, 1,
	    true);
	big_check("compact root, 65534 subdirectories, nlink 65535", &big,
	    false, 65535, false);
	big_check("compact root, 65534 subdirectories, nlink 2", &big, false, 2,
	    false);
	gen_image_free(&big.image);
}

static void
test_an_extended_directory_past_65535_links_records_the_count(void)
{
	big_t big;

	big_new(&big, BIG_CHILDREN);
	big_check("extended root, 65534 subdirectories, nlink 65536", &big, true,
	    65536, true);
	big_check("extended root, 65534 subdirectories, nlink 1", &big, true, 1,
	    false);
	big_check("extended root, 65534 subdirectories, nlink 65535", &big, true,
	    65535, false);
	gen_image_free(&big.image);
}

static void
test_a_compact_directory_at_exactly_65535_links_must_record_the_count(void)
{
	big_t big;

	big_new(&big, BIG_CHILDREN_FITTING);
	big_check("compact root, 65533 subdirectories, nlink 65535", &big,
	    false, 65535, true);
	big_check("compact root, 65533 subdirectories, nlink 1", &big, false, 1,
	    false);
	gen_image_free(&big.image);
}

static void
test_a_compact_directory_with_few_subdirectories_and_link_count_1_is_corrupt(
    void)
{
	gen_dirent_t kids[3];
	gen_tree_t tree;
	uint64_t dirs[3];

	tree_new(&tree, 9, 8);
	for (unsigned i = 0; i < 3; i++) {
		static const char *const names[] = { "d0", "d1", "d2" };

		dirs[i] = new_dir(&tree);
		fill_dir(&tree, dirs[i], 0, NULL, 0);
		kids[i] = entry(names[i], dirs[i], T_DIR);
	}
	fill_dir(&tree, 0, 0, kids, 3);
	set_nlink(&tree, 0, 1);
	expect_corrupt("compact root, 3 subdirectories, nlink 1", &tree,
	    "directory link count 1 of inode 0 for 3 subdirectories");
	gen_tree_free(&tree);
}

/* ---- Ownership overlaps ---- */

/* The root naming one file of a whole block of data; the file's NID. */
static uint64_t
one_block_file(gen_tree_t *tree)
{
	gen_dirent_t kids[1];
	uint64_t file = add_file(tree, LAYOUT_FLAT, block_bytes(tree));

	kids[0] = entry("f", file, T_FILE);
	fill_dir(tree, 0, 0, kids, 1);
	return (file);
}

/* The finding of an exclusive claim overlapped at the given byte. */
static const char *
metadata_overlap(char *buffer, size_t size, uint64_t byte)
{
	snprintf(buffer, size, "metadata at byte %llu overlaps other storage",
	    (unsigned long long)byte);
	return (buffer);
}

static void
test_file_data_over_a_directory_block_is_corrupt(void)
{
	char phrase[128];
	gen_tree_t tree;
	uint64_t file;

	tree_new(&tree, 9, 8);
	file = one_block_file(&tree);
	set_start_block(&tree, file, start_block(&tree, 0));
	snprintf(phrase, sizeof(phrase), "metadata at byte %llu overlaps other "
	    "storage", (unsigned long long)block_offset(&tree,
	    start_block(&tree, 0)));
	expect_corrupt("file data on the root's block", &tree, phrase);
	gen_tree_free(&tree);
}

static void
test_file_data_over_the_inode_area_is_corrupt(void)
{
	gen_tree_t tree;
	uint64_t file;

	tree_new(&tree, 9, 8);
	file = one_block_file(&tree);
	set_start_block(&tree, file, META_BYTES >> 9);
	expect_corrupt("file data on the block of the inodes", &tree,
	    "metadata at byte 4096 overlaps other storage");
	gen_tree_free(&tree);
}

static void
test_file_data_over_the_superblock_is_corrupt(void)
{
	gen_tree_t tree;
	uint64_t file;

	tree_new(&tree, 12, 8);
	file = one_block_file(&tree);
	set_start_block(&tree, file, 0);
	expect_corrupt("file data on block 0 of 4096", &tree,
	    "metadata at byte 1024 overlaps other storage");
	gen_tree_free(&tree);

	tree_new(&tree, 9, 8);
	file = one_block_file(&tree);
	set_start_block(&tree, file, 2);
	expect_corrupt("file data on block 2 of 512, holding byte 1024", &tree,
	    "metadata at byte 1024 overlaps other storage");
	gen_tree_free(&tree);
}

static void
test_two_files_sharing_data_blocks_verify(void)
{
	gen_dirent_t kids[2];
	gen_tree_t tree;
	uint64_t first, second;

	tree_new(&tree, 9, 8);
	first = add_file(&tree, LAYOUT_FLAT, 1024);
	second = add_file(&tree, LAYOUT_FLAT, 1024);
	set_start_block(&tree, second, start_block(&tree, first));
	kids[0] = entry("first", first, T_FILE);
	kids[1] = entry("second", second, T_FILE);
	fill_dir(&tree, 0, 0, kids, 2);
	expect_verified("two files over the same two blocks", &tree);

	set_start_block(&tree, second, start_block(&tree, first) + 1);
	expect_verified("two files overlapping by one block", &tree);
	gen_tree_free(&tree);
}

/*
 * An inode body that is the later claim of an overlapping pair. The
 * claims sort by start, so the body must start after what it overlaps:
 * file data that begins before the inode area, or an inode body that begins
 * inside another body.
 */
static void
test_file_data_starting_before_the_inode_area_is_corrupt(void)
{
	char phrase[128];
	gen_dirent_t kids[1];
	gen_tree_t tree;
	uint64_t file;

	tree_new(&tree, 9, 8);
	file = add_file(&tree, LAYOUT_FLAT, 2 * block_bytes(&tree));
	set_start_block(&tree, file, (META_BYTES >> 9) - 1);
	kids[0] = entry("f", file, T_FILE);
	fill_dir(&tree, 0, 0, kids, 1);
	expect_corrupt("two blocks of data ending inside the root's body",
	    &tree, metadata_overlap(phrase, sizeof(phrase), META_BYTES));
	gen_tree_free(&tree);
}

/*
 * An extended inode at slot m is 64 bytes and covers slot m + 1, where a
 * compact inode that is also reachable has its body. The two NIDs differ,
 * so the bodies are two structures and the second starts inside the first.
 */
static void
test_a_compact_inode_inside_an_extended_inode_body_is_corrupt(void)
{
	gen_inode_t extended = inode_proto(MODE_FILE, LAYOUT_FLAT);
	gen_inode_t compact = inode_proto(MODE_FILE, LAYOUT_FLAT);
	gen_dirent_t kids[2];
	char phrase[128];
	gen_tree_t tree;
	uint64_t first, second;

	tree_new(&tree, 9, 4);
	first = gen_tree_slot(&tree, GEN_EXTENDED_BYTES, 0);
	second = first + 1;
	extended.extended = true;
	gen_tree_fill(&tree, first, &extended, NULL, 0);
	gen_tree_fill(&tree, second, &compact, NULL, 0);
	/* The extended link count shares its bytes with the compact time. */
	set_nlink(&tree, first, 1);
	kids[0] = entry("extended", first, T_FILE);
	kids[1] = entry("compact", second, T_FILE);
	fill_dir(&tree, 0, 0, kids, 2);
	expect_corrupt("a compact body inside an extended body", &tree,
	    metadata_overlap(phrase, sizeof(phrase),
	    inode_offset(&tree, second)));
	gen_tree_free(&tree);
}

/*
 * An inode whose inline tail runs into the slot of the next but one inode:
 * the bodies overlap partly, not wholly and not at the same start, and the
 * two NIDs must not be taken for one structure.
 */
static void
test_two_inode_bodies_overlapping_partly_are_corrupt(void)
{
	gen_inode_t compact = inode_proto(MODE_FILE, LAYOUT_FLAT);
	gen_dirent_t kids[2];
	char phrase[128];
	gen_tree_t tree;
	uint64_t first, third;

	tree_new(&tree, 9, 4);
	/* 32 + 40 bytes: slots first, first + 1 and the start of first + 2. */
	first = add_file(&tree, LAYOUT_INLINE, 40);
	third = first + 2;
	gen_tree_fill(&tree, third, &compact, NULL, 0);
	kids[0] = entry("long", first, T_FILE);
	kids[1] = entry("short", third, T_FILE);
	fill_dir(&tree, 0, 0, kids, 2);
	expect_corrupt("a body starting inside another's inline tail", &tree,
	    metadata_overlap(phrase, sizeof(phrase),
	    inode_offset(&tree, third)));
	gen_tree_free(&tree);
}

/* The shared entry the cases below use: user.k = "vvv", 8 bytes. */
static const attr_t shared_attr = { 1, "k", "vvv" };

static uint32_t
index_of(uint64_t pool, uint64_t offset)
{
	CHECK(offset >= pool && (offset - pool) % 4 == 0);
	return ((uint32_t)((offset - pool) / 4));
}

static void
test_a_shared_entry_inside_an_inode_body_is_corrupt(void)
{
	attr_t carried = { 1, "k", "v" };
	region_t carrier_region = { .inline_attrs = &carried,
	    .inline_count = 1 };
	region_t user_region;
	gen_dirent_t kids[2];
	uint32_t index;
	gen_tree_t tree;
	char phrase[128];
	uint64_t carrier, user, entry_at;

	tree_new(&tree, 9, 4);
	carrier = add_file_with_region(&tree, &carrier_region);
	entry_at = inode_offset(&tree, carrier) + GEN_COMPACT_BYTES + 12;
	set_pool(&tree, META_BYTES >> 9);
	index = index_of(META_BYTES, entry_at);
	user_region = (region_t){ .shared = &index, .shared_count = 1 };
	user = add_file_with_region(&tree, &user_region);
	kids[0] = entry("carrier", carrier, T_FILE);
	kids[1] = entry("user", user, T_FILE);
	fill_dir(&tree, 0, 0, kids, 2);
	expect_corrupt("shared entry in another inode's attribute region",
	    &tree, metadata_overlap(phrase, sizeof(phrase), entry_at));
	gen_tree_free(&tree);
}

static void
test_a_shared_entry_over_file_data_is_corrupt(void)
{
	region_t user_region;
	gen_dirent_t kids[2];
	uint32_t index = 0;
	gen_tree_t tree;
	uint64_t file, user;

	tree_new(&tree, 9, 8);
	file = add_file(&tree, LAYOUT_FLAT, block_bytes(&tree));
	put_entry(&tree, block_offset(&tree, start_block(&tree, file)),
	    &shared_attr);
	set_pool(&tree, start_block(&tree, file));
	user_region = (region_t){ .shared = &index, .shared_count = 1 };
	user = add_file_with_region(&tree, &user_region);
	kids[0] = entry("file", file, T_FILE);
	kids[1] = entry("user", user, T_FILE);
	fill_dir(&tree, 0, 0, kids, 2);
	check_tree("shared entry in a file's data block", &tree, FSIMG_ECORRUPT,
	    "invalid EROFS shared attribute at byte");
	gen_tree_free(&tree);
}

/* Two files naming shared pool entries x0 and x1 (word offsets into it). */
static void
pool_case(gen_tree_t *tree, uint32_t first, uint32_t second)
{
	region_t first_region = { .shared = &first, .shared_count = 1 };
	region_t second_region = { .shared = &second, .shared_count = 1 };
	gen_dirent_t kids[2];
	uint64_t a, b;

	tree_new(tree, 9, 4);
	a = add_file_with_region(tree, &first_region);
	b = add_file_with_region(tree, &second_region);
	kids[0] = entry("a", a, T_FILE);
	kids[1] = entry("b", b, T_FILE);
	fill_dir(tree, 0, 0, kids, 2);
}

/* A pool block holding user.k at word 0 and user.k2 at word 2. */
static void
fill_pool(gen_tree_t *tree)
{
	static const attr_t second = { 1, "k2", "vv" };
	uint64_t at = block_offset(tree, new_block(tree));

	set_pool(tree, tree->data_next - 1);
	put_entry(tree, at, &shared_attr);
	put_entry(tree, at + 8, &second);
}

static void
test_two_inodes_sharing_one_shared_entry_verify(void)
{
	gen_tree_t tree;

	pool_case(&tree, 0, 0);
	fill_pool(&tree);
	expect_verified("two inodes naming the same shared entry", &tree);
	gen_tree_free(&tree);

	pool_case(&tree, 0, 2);
	fill_pool(&tree);
	expect_verified("two inodes naming adjacent shared entries", &tree);
	gen_tree_free(&tree);
}

static void
test_partly_overlapping_shared_entries_are_corrupt(void)
{
	/*
	 * Word 0 holds an 8-byte entry whose value begins with the 4 bytes
	 * of a valid empty user. entry, which word 1 names: the two ranges
	 * overlap without being the same range.
	 */
	static const unsigned char inner[] = {
		0, 1, 8, 0,		/* suffix 0, user., value 8 bytes */
		0, 1, 0, 0, 0, 0, 0, 0	/* the value, led by an empty entry */
	};
	gen_tree_t tree;

	pool_case(&tree, 0, 1);
	set_pool(&tree, new_block(&tree));
	gen_put_bytes(&tree.image, block_offset(&tree, tree.data_next - 1),
	    inner, sizeof(inner));
	check_tree("shared entries [0,12) and [4,8)", &tree, FSIMG_ECORRUPT,
	    "invalid EROFS shared attribute at byte");
	gen_tree_free(&tree);
}

static void
test_a_prefix_table_over_an_inode_body_is_corrupt(void)
{
	/* A prefix record, 8 bytes, as the inline tail of a file. */
	static const unsigned char record[] = { 5, 0, 1, 'a', 'b', 'c', 'd', 0 };
	gen_inode_t proto = inode_proto(MODE_FILE, LAYOUT_INLINE);
	gen_dirent_t kids[1];
	char phrase[128];
	gen_tree_t tree;
	uint64_t file;

	tree_new(&tree, 9, 4);
	file = gen_tree_add(&tree, &proto, record, sizeof(record));
	kids[0] = entry("f", file, T_FILE);
	fill_dir(&tree, 0, 0, kids, 1);
	set_prefix_table(&tree, inode_offset(&tree, file) + GEN_COMPACT_BYTES,
	    1);
	expect_corrupt("prefix table in a file's inline tail", &tree,
	    metadata_overlap(phrase, sizeof(phrase),
	    inode_offset(&tree, file) + GEN_COMPACT_BYTES));
	gen_tree_free(&tree);
}

static void
test_a_prefix_table_over_a_shared_entry_is_corrupt(void)
{
	/* The shared entry's 8-byte value is a prefix record. */
	static const unsigned char pool_bytes[] = {
		0, 1, 8, 0,
		5, 0, 1, 'a', 'b', 'c', 'd', 0
	};
	region_t user_region;
	gen_dirent_t kids[1];
	uint32_t index = 0;
	char phrase[128];
	gen_tree_t tree;
	uint64_t pool, user;

	tree_new(&tree, 9, 4);
	pool = block_offset(&tree, new_block(&tree));
	set_pool(&tree, tree.data_next - 1);
	gen_put_bytes(&tree.image, pool, pool_bytes, sizeof(pool_bytes));
	user_region = (region_t){ .shared = &index, .shared_count = 1 };
	user = add_file_with_region(&tree, &user_region);
	kids[0] = entry("f", user, T_FILE);
	fill_dir(&tree, 0, 0, kids, 1);
	set_prefix_table(&tree, pool + 4, 1);
	expect_corrupt("prefix table inside a shared entry", &tree,
	    metadata_overlap(phrase, sizeof(phrase), pool + 4));
	gen_tree_free(&tree);
}

/* ---- Name filters ---- */

/*
 * One file F carrying a shared attribute (security.selinux), an inline
 * attribute (user.inline) and a long-prefix attribute (index 0x81: the
 * second prefix, user. plus "vendor.", suffix "lp"), with the filter word
 * given; a second file G carrying user.inline with filter 0. The first
 * prefix ({ 4, "other." }) is a decoy: hashing "lp" under it, or the suffix
 * alone, gives a bit other than the one the right record gives, so a
 * verifier that chose the wrong record or dropped the infix would find a
 * present attribute excluded by a filter that is correct.
 */
typedef struct filter_bits {
	unsigned	 inline_bit, shared_bit, prefix_bit, suffix_only_bit,
			 wrong_record_bit;
} filter_bits_t;

static filter_bits_t
filter_bits(void)
{
	filter_bits_t bits = {
		.inline_bit = filter_bit(1, "", "inline"),
		.shared_bit = filter_bit(6, "", "selinux"),
		.prefix_bit = filter_bit(1, "vendor.", "lp"),
		.suffix_only_bit = filter_bit(1, "", "lp"),
		.wrong_record_bit = filter_bit(4, "other.", "lp")
	};

	/* The cases tell the attributes, and name from suffix, apart. */
	CHECK(bits.inline_bit != bits.shared_bit &&
	    bits.inline_bit != bits.prefix_bit &&
	    bits.shared_bit != bits.prefix_bit);
	CHECK(bits.suffix_only_bit != bits.prefix_bit &&
	    bits.suffix_only_bit != bits.inline_bit &&
	    bits.suffix_only_bit != bits.shared_bit);
	CHECK(bits.wrong_record_bit != bits.prefix_bit &&
	    bits.wrong_record_bit != bits.inline_bit &&
	    bits.wrong_record_bit != bits.shared_bit &&
	    bits.wrong_record_bit != bits.suffix_only_bit);
	return (bits);
}

static void
filter_tree(gen_tree_t *tree, uint32_t filter, uint32_t compat)
{
	static const prefix_t prefixes[] = { { 4, "other." }, { 1, "vendor." } };
	static const attr_t pool_attr = { 6, "selinux", "s" };
	static const attr_t inline_attr = { 1, "inline", "i" };
	static const attr_t prefix_attr = { 0x81, "lp", "p" };
	attr_t f_attrs[2] = { inline_attr, prefix_attr };
	uint32_t shared_index = 0;
	region_t f_region = { .filter = filter, .shared = &shared_index,
	    .shared_count = 1, .inline_attrs = f_attrs, .inline_count = 2 };
	region_t g_region = { .inline_attrs = &inline_attr, .inline_count = 1 };
	gen_dirent_t kids[2];
	uint64_t f, g;

	tree_new(tree, 9, 8);
	set_pool(tree, new_block(tree));
	put_entry(tree, block_offset(tree, tree->data_next - 1), &pool_attr);
	add_prefix_table(tree, prefixes, ARRAY_SIZE(prefixes));
	f = add_file_with_region(tree, &f_region);
	g = add_file_with_region(tree, &g_region);
	kids[0] = entry("f", f, T_FILE);
	kids[1] = entry("g", g, T_FILE);
	fill_dir(tree, 0, 0, kids, 2);
	gen_tree_set_compat(tree, compat | COMPAT_SB_CHKSUM |
	    COMPAT_PLAIN_XATTR_PFX);
}

static void
test_a_correct_name_filter_with_built_in_and_prefix_names_verifies(void)
{
	filter_bits_t bits = filter_bits();
	uint32_t present = UINT32_C(1) << bits.inline_bit |
	    UINT32_C(1) << bits.shared_bit | UINT32_C(1) << bits.prefix_bit;
	gen_tree_t tree;

	filter_tree(&tree, ~present, COMPAT_XATTR_FILTER);
	expect_verified("every absent bit set", &tree);
	gen_tree_free(&tree);

	filter_tree(&tree, 0, COMPAT_XATTR_FILTER);
	expect_verified("filter 0 promises nothing", &tree);
	gen_tree_free(&tree);
}

static void
test_a_filter_bit_for_an_absent_name_is_not_an_error(void)
{
	filter_bits_t bits = filter_bits();
	gen_tree_t tree;
	unsigned absent = 0;

	while (absent == bits.inline_bit || absent == bits.shared_bit ||
	    absent == bits.prefix_bit)
		absent++;
	filter_tree(&tree, UINT32_C(1) << absent, COMPAT_XATTR_FILTER);
	expect_verified("one bit no attribute hashes to", &tree);
	gen_tree_free(&tree);
}

static void
test_a_filter_bit_of_a_present_inline_attribute_is_corrupt(void)
{
	filter_bits_t bits = filter_bits();
	gen_tree_t tree;

	filter_tree(&tree, UINT32_C(1) << bits.inline_bit, COMPAT_XATTR_FILTER);
	expect_corrupt("inline user.inline excluded", &tree,
	    "attribute name filter excludes a present attribute");
	gen_tree_free(&tree);
}

static void
test_a_filter_bit_of_a_present_shared_attribute_is_corrupt(void)
{
	filter_bits_t bits = filter_bits();
	gen_tree_t tree;

	filter_tree(&tree, UINT32_C(1) << bits.shared_bit, COMPAT_XATTR_FILTER);
	expect_corrupt("shared security.selinux excluded", &tree,
	    "attribute name filter excludes a present attribute");
	gen_tree_free(&tree);
}

static void
test_a_filter_bit_of_a_present_prefix_attribute_is_corrupt(void)
{
	filter_bits_t bits = filter_bits();
	gen_tree_t tree;

	filter_tree(&tree, UINT32_C(1) << bits.prefix_bit, COMPAT_XATTR_FILTER);
	expect_corrupt("long-prefix user.vendor.lp excluded", &tree,
	    "attribute name filter excludes a present attribute");
	gen_tree_free(&tree);
}

static void
test_a_long_prefix_name_hashes_its_infix_and_suffix_together(void)
{
	filter_bits_t bits = filter_bits();
	gen_tree_t tree;

	/* The suffix alone is not the name: its bit excludes nothing. */
	filter_tree(&tree, UINT32_C(1) << bits.suffix_only_bit,
	    COMPAT_XATTR_FILTER);
	expect_verified("the bit of the suffix alone", &tree);
	gen_tree_free(&tree);

	/* Nor does the bit "lp" would have under the other prefix. */
	filter_tree(&tree, UINT32_C(1) << bits.wrong_record_bit,
	    COMPAT_XATTR_FILTER);
	expect_verified("the bit of the name under the other prefix", &tree);
	gen_tree_free(&tree);
}

/*
 * One file carrying two inline attributes on the same long prefix, whose
 * only record is { user., infix }, with the given filter word: user.<infix>
 * "first" and user.<infix>"second". Each name hashes alone, so a verifier
 * that carried one attribute's suffix into the next one's hash, or that
 * stopped absorbing a long infix part-way, would find a bit that is not
 * the right one.
 */
typedef struct pair_bits {
	unsigned	 first_bit, second_bit;
} pair_bits_t;

static pair_bits_t
pair_bits(const char *infix)
{
	pair_bits_t bits = {
		.first_bit = filter_bit(1, infix, "first"),
		.second_bit = filter_bit(1, infix, "second")
	};

	CHECK(bits.first_bit != bits.second_bit);
	return (bits);
}

static void
pair_tree(gen_tree_t *tree, const char *infix, uint32_t filter)
{
	const prefix_t prefix = { 1, infix };
	const attr_t attrs[2] = { { 0x80, "first", "1" },
	    { 0x80, "second", "2" } };
	region_t region = { .filter = filter, .inline_attrs = attrs,
	    .inline_count = 2 };
	gen_dirent_t kids[1];
	uint64_t file;

	tree_new(tree, 9, 8);
	add_prefix_table(tree, &prefix, 1);
	file = add_file_with_region(tree, &region);
	kids[0] = entry("f", file, T_FILE);
	fill_dir(tree, 0, 0, kids, 1);
	gen_tree_set_compat(tree, COMPAT_XATTR_FILTER | COMPAT_SB_CHKSUM |
	    COMPAT_PLAIN_XATTR_PFX);
}

/* The three outcomes a pair of names on one prefix can have. */
static void
pair_cases(const char *label, const char *infix)
{
	pair_bits_t bits = pair_bits(infix);
	uint32_t present = UINT32_C(1) << bits.first_bit |
	    UINT32_C(1) << bits.second_bit;
	static const char excludes[] =
	    "attribute name filter excludes a present attribute";
	char name[160];
	gen_tree_t tree;

	snprintf(name, sizeof(name), "%s: every absent bit set", label);
	pair_tree(&tree, infix, ~present);
	expect_verified(name, &tree);
	gen_tree_free(&tree);

	snprintf(name, sizeof(name), "%s: the first name's bit", label);
	pair_tree(&tree, infix, UINT32_C(1) << bits.first_bit);
	expect_corrupt(name, &tree, excludes);
	gen_tree_free(&tree);

	snprintf(name, sizeof(name), "%s: the second name's bit", label);
	pair_tree(&tree, infix, UINT32_C(1) << bits.second_bit);
	expect_corrupt(name, &tree, excludes);
	gen_tree_free(&tree);
}

static void
test_two_attributes_on_one_long_prefix_are_each_hashed_alone(void)
{
	pair_cases("user.vendor. with two suffixes", "vendor.");
}

static void
test_a_long_prefix_infix_of_1300_bytes_is_hashed_whole(void)
{
	char infix[1301];

	for (size_t i = 0; i < sizeof(infix) - 1; i++)
		infix[i] = (char)('a' + (i * 7 + i / 26) % 26);
	infix[sizeof(infix) - 1] = '\0';
	pair_cases("a 1300-byte infix", infix);
}

static void
test_filters_are_not_checked_without_the_feature(void)
{
	filter_bits_t bits = filter_bits();
	gen_tree_t tree;

	filter_tree(&tree, UINT32_C(1) << bits.inline_bit, 0);
	expect_verified("a wrong filter without XATTR_FILTER", &tree);
	gen_tree_free(&tree);
}

/* ---- What verification cannot vouch for ---- */

static void
check_incomplete(const char *name, gen_tree_t *tree, const char *phrase)
{
	char full[256];

	snprintf(full, sizeof(full), "unsupported EROFS %s", phrase);
	check_tree(name, tree, FSIMG_EUNSUPPORTED, full);
}

static void
test_a_reserved_superblock_byte_leaves_the_report_incomplete(void)
{
	static const unsigned fields[] = { SB_RESERVED_6A, SB_RESERVED_78 };

	for (size_t i = 0; i < ARRAY_SIZE(fields); i++) {
		char name[64];
		clean_t clean;

		snprintf(name, sizeof(name), "reserved byte at 0x%x", fields[i]);
		clean_new(&clean, 9);
		put_super(&clean.tree, fields[i], 1, 1);
		check_incomplete(name, &clean.tree,
		    "reserved superblock byte; verification is incomplete");
		gen_tree_free(&clean.tree);
	}
}

static void
test_an_unknown_compatible_feature_leaves_the_report_incomplete(void)
{
	clean_t clean;

	clean_new(&clean, 9);
	gen_tree_set_compat(&clean.tree, COMPAT_SB_CHKSUM | COMPAT_UNKNOWN);
	check_incomplete("compatible bit 0x40", &clean.tree,
	    "compatible feature; verification is incomplete");
	gen_tree_free(&clean.tree);
}

static void
test_a_disabled_name_filter_leaves_the_report_incomplete(void)
{
	filter_bits_t bits = filter_bits();
	gen_tree_t tree;

	filter_tree(&tree, 0, COMPAT_XATTR_FILTER);
	put_super(&tree, SB_FILTER_BYTE, 1, 1);
	check_incomplete("filter byte 1 under XATTR_FILTER", &tree,
	    "disabled name filter; verification is incomplete");
	gen_tree_free(&tree);

	/* Nothing is hashed, so a wrong filter is not found either. */
	filter_tree(&tree, UINT32_C(1) << bits.inline_bit, COMPAT_XATTR_FILTER);
	put_super(&tree, SB_FILTER_BYTE, 1, 1);
	check_incomplete("a wrong filter with the filter byte 1", &tree,
	    "disabled name filter; verification is incomplete");
	gen_tree_free(&tree);
}

static void
test_the_filter_byte_without_the_feature_is_ignored(void)
{
	filter_bits_t bits = filter_bits();
	gen_tree_t tree;

	filter_tree(&tree, UINT32_C(1) << bits.inline_bit, 0);
	put_super(&tree, SB_FILTER_BYTE, 1, 1);
	expect_verified("filter byte 1 without XATTR_FILTER", &tree);
	gen_tree_free(&tree);
}

static void
test_corruption_is_reported_before_an_incomplete_scope(void)
{
	clean_t clean;

	clean_new(&clean, 9);
	put_super(&clean.tree, SB_RESERVED_6A, 1, 1);
	gen_tree_set_compat(&clean.tree, COMPAT_SB_CHKSUM | COMPAT_UNKNOWN);
	set_nlink(&clean.tree, 0, 9);
	expect_corrupt("reserved byte and unknown feature, root nlink 9",
	    &clean.tree,
	    "directory link count 9 of inode 0 for 2 subdirectories");
	gen_tree_free(&clean.tree);
}

/* ---- Attribute structures that listing does not read ---- */

/* A tree whose one file names shared index 0; the file's NID. */
static uint64_t
shared_index_tree(gen_tree_t *tree)
{
	uint32_t index = 0;
	region_t region = { .shared = &index, .shared_count = 1 };
	gen_dirent_t kids[1];
	uint64_t file;

	tree_new(tree, 9, 4);
	set_pool(tree, new_block(tree));
	file = add_file_with_region(tree, &region);
	kids[0] = entry("f", file, T_FILE);
	fill_dir(tree, 0, 0, kids, 1);
	return (file);
}

/* Replace the file's only shared index. */
static void
set_shared_index(gen_tree_t *tree, uint64_t file, uint32_t index)
{
	gen_put(&tree->image, inode_offset(tree, file) + GEN_COMPACT_BYTES + 12,
	    index, 4);
}

static void
test_a_shared_index_outside_the_volume_is_corrupt_though_listing_works(void)
{
	gen_tree_t tree;
	uint64_t file, pool;

	file = shared_index_tree(&tree);
	set_shared_index(&tree, file, 0x00ffffff);
	gen_tree_seal(&tree);
	EXPECT(list_image(&tree.image) == FSIMG_OK, "far index: listing works");
	check_image("an index far past the volume", &tree.image, FSIMG_ECORRUPT,
	    "invalid EROFS shared attribute outside the volume");
	gen_tree_free(&tree);

	file = shared_index_tree(&tree);
	pool = block_offset(&tree, gen_tree_first_block(9));
	set_shared_index(&tree, file,
	    (uint32_t)((tree.image.length - pool) / 4));
	gen_tree_seal(&tree);
	EXPECT(list_image(&tree.image) == FSIMG_OK, "end index: listing works");
	check_image("an index at the volume's end", &tree.image, FSIMG_ECORRUPT,
	    "invalid EROFS shared attribute outside the volume");
	gen_tree_free(&tree);

	file = shared_index_tree(&tree);
	pool = block_offset(&tree, gen_tree_first_block(9));
	put_entry(&tree, tree.image.length - 8, &shared_attr);
	set_shared_index(&tree, file, (uint32_t)((tree.image.length - 8 - pool) /
	    4));
	expect_verified("an entry ending exactly at the volume's end", &tree);
	gen_tree_free(&tree);
}

/*
 * A volume whose long-prefix table is the given bytes at volume byte at,
 * declaring count prefixes; no inode names a prefix.
 */
static void
table_tree(gen_tree_t *tree, const unsigned char *bytes, size_t length,
    int64_t from_end, unsigned count)
{
	gen_dirent_t kids[1];
	uint64_t file, at;

	tree_new(tree, 9, 4);
	file = add_file(tree, LAYOUT_INLINE, 40);
	kids[0] = entry("f", file, T_FILE);
	fill_dir(tree, 0, 0, kids, 1);
	at = (uint64_t)((int64_t)tree->image.length + from_end);
	gen_put_bytes(&tree->image, at, bytes, length);
	set_prefix_table(tree, at, count);
}

static void
table_case(const char *name, const unsigned char *bytes, size_t length,
    int64_t from_end, unsigned count, const char *phrase)
{
	gen_tree_t tree;
	char full[256];

	table_tree(&tree, bytes, length, from_end, count);
	gen_tree_seal(&tree);
	EXPECT(list_image(&tree.image) == FSIMG_OK, "%s: listing succeeds",
	    name);
	snprintf(full, sizeof(full), "invalid EROFS %s", phrase);
	check_image(name, &tree.image, FSIMG_ECORRUPT, full);
	gen_tree_free(&tree);
}

static void
test_a_bad_long_prefix_table_is_corrupt_though_listing_works(void)
{
	static const unsigned char base_five[] = { 2, 0, 5, 0 };
	static const unsigned char length_zero[] = { 0, 0, 1, 0 };
	static const unsigned char too_long[] = { 100, 0, 1, 0, 0, 0, 0, 0 };
	static const unsigned char one_empty[] = { 1, 0, 1, 0 };

	table_case("base index 5 is not a built-in prefix", base_five,
	    sizeof(base_five), -8, 1, "long prefix base index");
	table_case("record length 0", length_zero, sizeof(length_zero), -8, 1,
	    "long prefix length");
	table_case("a record running past the volume", too_long,
	    sizeof(too_long), -8, 1, "long prefix record");
	table_case("a second record after the volume's end", one_empty,
	    sizeof(one_empty), -4, 2, "long prefix truncated");
	table_case("a table starting at the volume's end", NULL, 0, 0, 1,
	    "long prefix truncated");
}

static void
test_a_good_long_prefix_table_verifies(void)
{
	static const unsigned char two_records[] = {
		1, 0, 1, 0,
		5, 0, 4, 'a', 'b', 'c', 'd', 0
	};
	gen_tree_t tree;

	table_tree(&tree, two_records, sizeof(two_records), -16, 2);
	expect_verified("two valid records at the end of the volume", &tree);
	gen_tree_free(&tree);
}

/* ---- Truncation ---- */

static void
test_a_block_count_past_the_input_fails_as_truncated(void)
{
	gen_tree_t tree;
	uint64_t file;
	gen_dirent_t kids[1];

	tree_new(&tree, 9, 4);
	file = add_file(&tree, LAYOUT_INLINE, 40);
	kids[0] = entry("f", file, T_FILE);
	fill_dir(&tree, 0, 0, kids, 1);
	gen_tree_set_blocks(&tree, tree.blocks + 1000);
	gen_tree_seal(&tree);
	EXPECT(list_image(&tree.image) == FSIMG_OK, "listing succeeds");
	check_image("1000 blocks past the input", &tree.image, FSIMG_ECORRUPT,
	    "truncated");
	gen_tree_free(&tree);
}

/* ---- Entries naming the root ---- */

/*
 * A non-dot entry naming the root, listed by the directory `depth` levels
 * below it (0 is the root itself). Listing and verification must both fail
 * as corrupt, never with a depth or resource error.
 */
static void
root_entry_case(const char *name, unsigned depth, const char *phrase)
{
	gen_dirent_t loop[1], down[1];
	gen_tree_t tree;
	uint64_t dirs[3];
	verdict_t listing;
	char label[160];

	tree_new(&tree, 9, 8);
	loop[0] = entry("loop", 0, T_DIR);
	for (unsigned i = 0; i < depth; i++)
		dirs[i] = new_dir(&tree);
	for (unsigned i = depth; i > 0; i--) {
		uint64_t parent = i == 1 ? 0 : dirs[i - 2];

		if (i == depth) {
			fill_dir(&tree, dirs[i - 1], parent, loop, 1);
		} else {
			down[0] = entry("d", dirs[i], T_DIR);
			fill_dir(&tree, dirs[i - 1], parent, down, 1);
		}
	}
	if (depth == 0) {
		fill_dir(&tree, 0, 0, loop, 1);
	} else {
		down[0] = entry("d", dirs[0], T_DIR);
		fill_dir(&tree, 0, 0, down, 1);
	}
	gen_tree_seal(&tree);
	listing = run_image(&tree.image, 0, false, MEMORY_BYTES);
	snprintf(label, sizeof(label), "%s, listing", name);
	check_verdict(label, 0, &listing, FSIMG_ECORRUPT, phrase);
	check_image(name, &tree.image, FSIMG_ECORRUPT, phrase);
	gen_tree_free(&tree);
}

/* Directory scanning's findings for an entry naming the root. */
#define ROOT_NAMED	"directory entry naming the directory or its parent"
#define DOT_PARENT	"dot entry breaks the parent rule"

static void
test_an_entry_naming_the_root_is_corrupt_at_every_depth(void)
{
	root_entry_case("the root names itself", 0, ROOT_NAMED);
	root_entry_case("a child of the root names the root", 1, ROOT_NAMED);
	root_entry_case("a grandchild names the root", 2, DOT_PARENT);
}

/* ---- Payload accounting ---- */

/*
 * This pins the current behaviour recorded in TODO.md ("EROFS payload
 * verification reads a hard-linked file once per name"): the walk reaches
 * the inode once per name and reads its content each time. It is expected
 * to change with that fix, to a count of the content once.
 */
static void
test_payload_of_a_file_with_two_names_is_counted_once_per_name(void)
{
	enum { SIZE = 1000 };
	gen_dirent_t kids[2];
	gen_tree_t tree;
	uint64_t file;

	tree_new(&tree, 9, 8);
	file = add_file(&tree, LAYOUT_FLAT, SIZE);
	set_nlink(&tree, file, 2);
	kids[0] = entry("one", file, T_FILE);
	kids[1] = entry("two", file, T_FILE);
	fill_dir(&tree, 0, 0, kids, 2);
	gen_tree_seal(&tree);
	for (size_t i = 0; i < SCOPE_COUNT; i++) {
		verdict_t v = verify_image(&tree.image, scopes[i]);

		check_verdict("a file named twice", scopes[i], &v, FSIMG_OK,
		    NULL);
		EXPECT(v.report.files == 2 && v.report.logical_bytes ==
		    2 * SIZE, "%s: %llu files, %llu logical bytes",
		    scope_name(scopes[i]), (unsigned long long)v.report.files,
		    (unsigned long long)v.report.logical_bytes);
		EXPECT(v.report.payload_bytes_read ==
		    (scopes[i] == 0 ? 0 : 2 * SIZE),
		    "%s: %llu payload bytes", scope_name(scopes[i]),
		    (unsigned long long)v.report.payload_bytes_read);
	}
	gen_tree_free(&tree);
}

int
main(void)
{
	test_the_filter_hash_matches_published_vectors();
	test_clean_volumes_of_every_kind_verify_complete_in_both_block_sizes();
	test_the_report_counts_what_the_walk_delivered();
	test_a_file_link_count_below_its_names_is_corrupt();
	test_a_file_link_count_above_its_names_is_corrupt();
	test_a_file_link_count_equal_to_its_names_verifies();
	test_a_second_name_for_a_file_with_link_count_1_is_corrupt();
	test_a_directory_named_twice_in_one_parent_is_corrupt();
	test_a_directory_link_count_above_its_subdirectories_is_corrupt();
	test_a_directory_link_count_below_its_subdirectories_is_corrupt();
	test_a_wrong_root_link_count_is_corrupt();
	test_an_omitted_entry_counts_as_a_name_of_its_inode();
	test_an_omitted_entry_with_a_wrong_link_count_is_corrupt();
	test_a_compact_directory_past_65535_links_may_record_1();
	test_an_extended_directory_past_65535_links_records_the_count();
	test_a_compact_directory_at_exactly_65535_links_must_record_the_count();
	test_a_compact_directory_with_few_subdirectories_and_link_count_1_is_corrupt();
	test_file_data_over_a_directory_block_is_corrupt();
	test_file_data_over_the_inode_area_is_corrupt();
	test_file_data_over_the_superblock_is_corrupt();
	test_file_data_starting_before_the_inode_area_is_corrupt();
	test_a_compact_inode_inside_an_extended_inode_body_is_corrupt();
	test_two_inode_bodies_overlapping_partly_are_corrupt();
	test_two_files_sharing_data_blocks_verify();
	test_a_shared_entry_inside_an_inode_body_is_corrupt();
	test_a_shared_entry_over_file_data_is_corrupt();
	test_two_inodes_sharing_one_shared_entry_verify();
	test_partly_overlapping_shared_entries_are_corrupt();
	test_a_prefix_table_over_an_inode_body_is_corrupt();
	test_a_prefix_table_over_a_shared_entry_is_corrupt();
	test_a_correct_name_filter_with_built_in_and_prefix_names_verifies();
	test_a_filter_bit_for_an_absent_name_is_not_an_error();
	test_a_filter_bit_of_a_present_inline_attribute_is_corrupt();
	test_a_filter_bit_of_a_present_shared_attribute_is_corrupt();
	test_a_filter_bit_of_a_present_prefix_attribute_is_corrupt();
	test_a_long_prefix_name_hashes_its_infix_and_suffix_together();
	test_two_attributes_on_one_long_prefix_are_each_hashed_alone();
	test_a_long_prefix_infix_of_1300_bytes_is_hashed_whole();
	test_filters_are_not_checked_without_the_feature();
	test_a_reserved_superblock_byte_leaves_the_report_incomplete();
	test_an_unknown_compatible_feature_leaves_the_report_incomplete();
	test_a_disabled_name_filter_leaves_the_report_incomplete();
	test_the_filter_byte_without_the_feature_is_ignored();
	test_corruption_is_reported_before_an_incomplete_scope();
	test_a_shared_index_outside_the_volume_is_corrupt_though_listing_works();
	test_a_bad_long_prefix_table_is_corrupt_though_listing_works();
	test_a_good_long_prefix_table_verifies();
	test_a_block_count_past_the_input_fails_as_truncated();
	test_an_entry_naming_the_root_is_corrupt_at_every_depth();
	test_payload_of_a_file_with_two_names_is_counted_once_per_name();
	return (failures == 0 ? 0 : 1);
}
