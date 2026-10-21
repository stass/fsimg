/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "extract.h"
#include "fsimg.h"
#include "qnx6.h"
#include "reader.h"
#include "test.h"

/*
 * A synthetic QNX6 volume generator written from the format design
 * (specs/design-qnx6.md) with its own offset tables, CRC and checksum
 * arithmetic. It shares no code with lib/formats/qnx6; the tests use the
 * driver header only to check the production codecs against the design's
 * check values.
 *
 * The generator builds a classic volume in memory: the system run of the
 * bitmap and inode-table trees from a chosen base block, a twin region, and
 * content blocks allocated upwards from 2R. Each commit lays out every
 * directory and the long-name tree again, rebuilds the snapshot's bitmap
 * from the blocks its objects own, and writes a header. Free blocks hold a
 * poison byte so that a reader exposing them is caught.
 */

#define GEN_DATA	12288u
#define GEN_SENTINEL	UINT32_C(0xffffffff)
#define GEN_INODES	64u
#define GEN_NAMES	64u
#define GEN_POISON	0xa5u

typedef struct gen_object {
	uint32_t	*blocks;	/* every block it owns, leaves and indirect */
	size_t		 block_count;
	size_t		 block_capacity;
	unsigned char	*slots;		/* a directory's slots */
	size_t		 slot_count;
	size_t		 slot_capacity;
	uint64_t	 size;
	uint32_t	 refs[16];
	uint32_t	 parent;
	uint32_t	 times[4];	/* ftime, mtime, atime, ctime */
	uint32_t	 uid;
	uint32_t	 gid;
	uint16_t	 mode;
	uint16_t	 links;
	uint8_t		 depth;
	uint8_t		 status;
	bool		 live;
} gen_object_t;

/* The shape of a new volume. objects bounds the inode numbers the
 * generator can make live, from 1; the rest of the inode table stays free.
 * A sparse volume lives in an unlinked, ftruncate-extended temporary file
 * mapped shared, so its untouched blocks cost neither memory nor disk and
 * read as zeros instead of the poison byte; it lets tests describe volumes
 * of many gigabytes and files whose content is mostly unwritten.
 */
typedef struct gen_shape {
	uint64_t	 length;
	uint32_t	 block_size;
	uint32_t	 inodes;
	uint32_t	 objects;
	uint32_t	 flags;
	bool		 big;
	bool		 sparse;
} gen_shape_t;

typedef struct qnx6_gen {
	unsigned char	*image;
	uint64_t	 length;
	int		 image_fd;	/* the sparse volume's file, or -1 */
	uint64_t	 second;
	unsigned char	*table;
	unsigned char	*bitmap;
	unsigned char	*names;		/* one block per long name */
	uint32_t	*name_blocks;
	uint32_t	*system_blocks;	/* the selected run, for mutation */
	size_t		 name_count;
	size_t		 system_count;
	uint32_t	 block_size;
	uint32_t	 pointers;
	uint32_t	 blocks;
	uint32_t	 inode_count;
	uint32_t	 reservation;
	uint32_t	 flags;
	uint32_t	 next_block;
	uint32_t	 stride;
	uint32_t	 base;		/* first block of the system run */
	uint32_t	 bitmap_refs[16];
	uint32_t	 inode_refs[16];
	uint32_t	 name_refs[16];
	uint32_t	 inode_leaf0;	/* first inode-table leaf */
	uint32_t	 object_count;	/* live inode numbers are 1..object_count */
	uint32_t	 inode_cursor;	/* gen_inode's search starts here */
	uint8_t		 bitmap_depth;
	uint8_t		 inode_depth;
	uint8_t		 name_depth;
	bool		 big;
	bool		 root_last;
	bool		 sparse;
	gen_object_t	*objects;	/* indexed by inode number */
} qnx6_gen_t;

static void put16(qnx6_gen_t *, unsigned char *, uint16_t);
static void put32(qnx6_gen_t *, unsigned char *, uint32_t);
static void put64(qnx6_gen_t *, unsigned char *, uint64_t);
static uint32_t get32(const qnx6_gen_t *, const unsigned char *);
static uint32_t gen_crc(const unsigned char *, size_t);
static uint32_t gen_name_sum(const unsigned char *, size_t);
static unsigned gen_depth(uint64_t, uint32_t);
static uint64_t gen_tree_blocks(uint64_t, uint32_t);
static unsigned char *block_at(qnx6_gen_t *, uint32_t);
static uint32_t gen_alloc(qnx6_gen_t *);
static void own(gen_object_t *, uint32_t);
static void gen_tree(qnx6_gen_t *, const uint32_t *, uint64_t, unsigned,
    uint32_t *, gen_object_t *, uint32_t *, uint8_t *);
static void temp_name(char *, size_t, const char *);
static bool gen_sparse(qnx6_gen_t *);
static bool gen_create(qnx6_gen_t *, const gen_shape_t *);
static void gen_init(qnx6_gen_t *, uint32_t, bool, uint64_t, uint32_t, uint32_t);
static void gen_free(qnx6_gen_t *);
static uint32_t gen_inode(qnx6_gen_t *, uint16_t);
static void gen_slot(qnx6_gen_t *, uint32_t, uint32_t, const char *, size_t);
static uint32_t gen_dir(qnx6_gen_t *, uint32_t, const char *);
static uint32_t gen_file(qnx6_gen_t *, uint32_t, const char *,
    const unsigned char *, uint64_t);
static uint32_t gen_symlink(qnx6_gen_t *, uint32_t, const char *, const char *);
static uint32_t gen_fifo(qnx6_gen_t *, uint32_t, const char *);
static void gen_link(qnx6_gen_t *, uint32_t, const char *, uint32_t);
static void gen_commit(qnx6_gen_t *, uint32_t, uint64_t, bool, bool);
static void gen_header(qnx6_gen_t *, unsigned char *, uint64_t);
static void gen_reseal(qnx6_gen_t *, uint64_t);
static unsigned char *gen_inode_bytes(qnx6_gen_t *, uint32_t);
static void gen_set_bit(qnx6_gen_t *, uint32_t, bool);

static void
put16(qnx6_gen_t *g, unsigned char *p, uint16_t v)
{
	p[g->big ? 0 : 1] = (unsigned char)(v >> 8);
	p[g->big ? 1 : 0] = (unsigned char)v;
}

static void
put32(qnx6_gen_t *g, unsigned char *p, uint32_t v)
{
	for (unsigned i = 0; i < 4; i++)
		p[g->big ? 3 - i : i] = (unsigned char)(v >> (8 * i));
}

static void
put64(qnx6_gen_t *g, unsigned char *p, uint64_t v)
{
	for (unsigned i = 0; i < 8; i++)
		p[g->big ? 7 - i : i] = (unsigned char)(v >> (8 * i));
}

static uint32_t
get32(const qnx6_gen_t *g, const unsigned char *p)
{
	uint32_t v = 0;

	for (unsigned i = 0; i < 4; i++)
		v |= (uint32_t)p[g->big ? 3 - i : i] << (8 * i);
	return (v);
}

/* Polynomial long division of the message, followed by 32 zero bits, by
 * x^32 + 0x04c11db7, one message bit at a time.
 */
static uint32_t
gen_crc(const unsigned char *bytes, size_t length)
{
	uint32_t remainder = 0;
	unsigned top, bit;

	for (size_t i = 0; i < length + 4; i++) {
		for (int b = 7; b >= 0; b--) {
			bit = i < length ? (bytes[i] >> b) & 1 : 0;
			top = remainder >> 31;
			remainder = remainder << 1 | bit;
			if (top)
				remainder ^= UINT32_C(0x04c11db7);
		}
	}
	return (remainder);
}

/* The long-name recurrence, kept in 64 bits and reduced each step. */
static uint32_t
gen_name_sum(const unsigned char *name, size_t length)
{
	uint64_t value = 0;
	bool odd;

	for (size_t i = 0; i < length; i++) {
		odd = (value & 1) != 0;
		value = ((value >> 1) + name[i]) % (UINT64_C(1) << 32);
		if (odd)
			value ^= UINT64_C(1) << 31;
	}
	return (value == 0 ? GEN_SENTINEL : (uint32_t)value);
}

/* The smallest depth whose capacity 16 * P^depth reaches n leaves. */
static unsigned
gen_depth(uint64_t leaves, uint32_t pointers)
{
	uint64_t capacity = 16;
	unsigned depth = 0;

	while (capacity < leaves) {
		capacity *= pointers;
		depth++;
	}
	return (depth);
}

/* Leaves plus indirect blocks of a minimal tree with n leaves. */
static uint64_t
gen_tree_blocks(uint64_t leaves, uint32_t pointers)
{
	uint64_t total = leaves, level = leaves;

	for (unsigned d = gen_depth(leaves, pointers); d > 0; d--) {
		level = (level + pointers - 1) / pointers;
		total += level;
	}
	return (total);
}

static unsigned char *
block_at(qnx6_gen_t *g, uint32_t block)
{
	CHECK(block < g->blocks);
	return (g->image + GEN_DATA + (uint64_t)block * g->block_size);
}

/* The next content block; a stride of 2 fragments every object. */
static uint32_t
gen_alloc(qnx6_gen_t *g)
{
	uint32_t block = g->next_block;

	CHECK(block < g->blocks);
	g->next_block += g->stride;
	return (block);
}

/* Grow an array of count elements to hold one more, doubling its capacity
 * so that large objects and wide directories build in linear time.
 */
static void *
grow(void *array, size_t count, size_t *capacity, size_t element)
{
	if (count < *capacity)
		return (array);
	CHECK(*capacity <= SIZE_MAX / 2 / element);
	*capacity = *capacity == 0 ? 16 : *capacity * 2;
	array = realloc(array, *capacity * element);
	CHECK(array != NULL);
	return (array);
}

static void
own(gen_object_t *object, uint32_t block)
{
	object->blocks = grow(object->blocks, object->block_count,
	    &object->block_capacity, sizeof(*object->blocks));
	object->blocks[object->block_count++] = block;
}

/* Build a tree over n leaves at the given depth, bottom up: each level
 * groups P references of the level below into one indirect block, and at
 * most 16 references remain for the root. Indirect blocks come from
 * *cursor when it is not NULL, otherwise from the content allocator. Unused
 * slots hold the sentinel.
 */
static void
gen_tree(qnx6_gen_t *g, const uint32_t *leaves, uint64_t n, unsigned depth,
    uint32_t *cursor, gen_object_t *owner, uint32_t *refs, uint8_t *out_depth)
{
	uint32_t *level, *above;
	uint64_t count, groups;
	unsigned char *block;

	level = malloc((n + 1) * sizeof(*level));
	CHECK(level != NULL);
	if (n != 0)
		bcopy(leaves, level, n * sizeof(*level));
	count = n;
	for (unsigned d = 0; d < depth; d++) {
		groups = count == 0 ? 0 : (count + g->pointers - 1) / g->pointers;
		above = malloc((groups + 1) * sizeof(*above));
		CHECK(above != NULL);
		for (uint64_t i = 0; i < groups; i++) {
			above[i] = cursor != NULL ? (*cursor)++ : gen_alloc(g);
			if (owner != NULL)
				own(owner, above[i]);
			block = block_at(g, above[i]);
			for (uint32_t k = 0; k < g->pointers; k++)
				put32(g, block + k * 4, i * g->pointers + k < count ?
				    level[i * g->pointers + k] : GEN_SENTINEL);
		}
		free(level);
		level = above;
		count = groups;
	}
	CHECK(count <= 16);
	for (unsigned i = 0; i < 16; i++)
		refs[i] = i < count ? level[i] : GEN_SENTINEL;
	*out_depth = (uint8_t)depth;
	free(level);
}

/* A mkstemp/mkdtemp template for prefix in $TMPDIR, or /tmp. */
static void
temp_name(char *buffer, size_t size, const char *prefix)
{
	const char *directory = getenv("TMPDIR");

	if (directory == NULL || directory[0] == '\0')
		directory = "/tmp";
	CHECK((size_t)snprintf(buffer, size, "%s/%s-XXXXXX", directory,
	    prefix) < size);
}

/* Cases skipped because the host cannot hold a sparse volume; a run with
 * any exits 77 after everything else passed.
 */
static unsigned skipped;

/* Back the volume with an unlinked temporary file extended to its length
 * and mapped shared. A host without the address space, a file size limit
 * (SIGXFSZ is ignored so that it shows as EFBIG), or a filesystem without
 * room, quota or support for a large shared mapping cannot hold it; that
 * is reported as a skip, not a failure. Any other error fails.
 */
static bool
gen_sparse(qnx6_gen_t *g)
{
	char path[1024];
	int saved;

	if (g->length > SIZE_MAX || g->length > (uint64_t)INT64_MAX) {
		printf("SKIP: a sparse %" PRIu64 "-byte QNX6 volume exceeds the "
		    "address space\n", g->length);
		skipped++;
		return (false);
	}
	CHECK(signal(SIGXFSZ, SIG_IGN) != SIG_ERR);
	temp_name(path, sizeof(path), "fsimg-qnx6-image");
	g->image_fd = mkstemp(path);
	CHECK(g->image_fd != -1 && unlink(path) == 0);
	g->image = MAP_FAILED;
	if (ftruncate(g->image_fd, (off_t)g->length) == 0)
		g->image = mmap(NULL, (size_t)g->length, PROT_READ | PROT_WRITE,
		    MAP_SHARED, g->image_fd, 0);
	if (g->image != MAP_FAILED)
		return (true);
	saved = errno;
	CHECK(saved == EFBIG || saved == ENOSPC || saved == EDQUOT ||
	    saved == ENOMEM || saved == ENODEV || saved == ENOTSUP ||
	    saved == EOPNOTSUPP);
	printf("SKIP: a sparse %" PRIu64 "-byte QNX6 volume: %s\n", g->length,
	    strerror(saved));
	skipped++;
	CHECK(close(g->image_fd) == 0);
	g->image_fd = -1;
	g->image = NULL;
	return (false);
}

/* Lay out an empty volume of `length` bytes: N follows from the 4096-byte
 * end, the second header sits in the last whole 4096-byte unit, and R is
 * the block count of the bitmap and inode-table trees. Root (inode 1) and
 * .boot (inode 2) are created with formatter status 1. False, with nothing
 * to free, when a sparse volume cannot be held.
 */
static bool
gen_create(qnx6_gen_t *g, const gen_shape_t *shape)
{
	uint64_t end, bitmap_bytes, length = shape->length;
	uint32_t boot, block_size = shape->block_size;

	bzero(g, sizeof(*g));
	g->block_size = block_size;
	g->pointers = block_size / 4;
	g->big = shape->big;
	g->length = length;
	g->flags = shape->flags;
	g->stride = 1;
	g->inode_count = shape->inodes;
	g->object_count = shape->objects < shape->inodes ? shape->objects :
	    shape->inodes;
	g->inode_cursor = 1;
	g->sparse = shape->sparse;
	g->image_fd = -1;
	end = length / 4096 * 4096;
	g->second = end - 4096;
	g->blocks = (uint32_t)((end - 16384) / block_size);
	bitmap_bytes = (g->blocks + 7) / 8;
	g->reservation = (uint32_t)(gen_tree_blocks((bitmap_bytes +
	    block_size - 1) / block_size, g->pointers) +
	    gen_tree_blocks((uint64_t)g->inode_count * 128 / block_size,
	    g->pointers));
	CHECK((uint64_t)g->inode_count * 128 % block_size == 0);
	CHECK(2 * (uint64_t)g->reservation < g->blocks);
	g->next_block = 2 * g->reservation;
	if (g->sparse) {
		if (!gen_sparse(g))
			return (false);
	} else {
		CHECK(length <= SIZE_MAX);
		g->image = malloc(length);
		CHECK(g->image != NULL);
		memset(g->image, GEN_POISON, length);
		bzero(g->image + end, length - end);
	}
	g->objects = calloc((size_t)g->object_count + 1, sizeof(*g->objects));
	g->table = calloc(g->inode_count, 128);
	g->bitmap = calloc((g->blocks + 7) / 8 + block_size, 1);
	g->names = calloc(GEN_NAMES, block_size);
	CHECK(g->objects != NULL && g->table != NULL && g->bitmap != NULL &&
	    g->names != NULL);
	CHECK(gen_inode(g, 040775) == 1);
	g->objects[1].parent = 1;
	g->objects[1].status = 1;
	g->objects[1].links = 2;
	gen_slot(g, 1, 1, ".", 1);
	gen_slot(g, 1, 1, "..", 2);
	boot = gen_dir(g, 1, ".boot");
	CHECK(boot == 2);
	g->objects[2].mode = 040700;
	g->objects[2].status = 1;
	return (true);
}

/* An ordinary in-memory volume with poisoned free blocks and up to
 * GEN_INODES live inodes.
 */
static void
gen_init(qnx6_gen_t *g, uint32_t block_size, bool big, uint64_t length,
    uint32_t inodes, uint32_t flags)
{
	gen_shape_t shape = {
		.length = length,
		.block_size = block_size,
		.inodes = inodes,
		.objects = GEN_INODES,
		.flags = flags,
		.big = big
	};

	CHECK(gen_create(g, &shape));
}

static void
gen_free(qnx6_gen_t *g)
{
	for (uint32_t i = 0; i <= g->object_count; i++) {
		free(g->objects[i].blocks);
		free(g->objects[i].slots);
	}
	free(g->objects);
	if (g->sparse) {
		CHECK(munmap(g->image, (size_t)g->length) == 0);
		CHECK(close(g->image_fd) == 0);
	} else {
		free(g->image);
	}
	free(g->table);
	free(g->bitmap);
	free(g->names);
	free(g->name_blocks);
	free(g->system_blocks);
}

/* The first free inode number at or above the cursor, which then moves
 * past it; a test moves the cursor to place an inode at a chosen number.
 */
static uint32_t
gen_inode(qnx6_gen_t *g, uint16_t mode)
{
	for (uint32_t i = g->inode_cursor; i <= g->object_count; i++) {
		if (g->objects[i].live)
			continue;
		g->inode_cursor = i + 1;
		g->objects[i] = (gen_object_t){
			.live = true,
			.mode = mode,
			.status = 3,
			.links = 1,
			.uid = 0,
			.gid = 0,
			.times = { 1000000000u + i, 1000000100u + i,
			    1000000200u + i, 1000000300u + i }
		};
		for (unsigned k = 0; k < 16; k++)
			g->objects[i].refs[k] = GEN_SENTINEL;
		return (i);
	}
	CHECK(false);
	return (0);
}

/* Append a slot naming inode to directory dir: inline up to 27 bytes,
 * otherwise a long-name record in its own block of the long-name tree.
 */
static void
gen_slot(qnx6_gen_t *g, uint32_t dir, uint32_t inode, const char *name,
    size_t length)
{
	gen_object_t *d = &g->objects[dir];
	unsigned char *slot, *record;

	d->slots = grow(d->slots, d->slot_count, &d->slot_capacity, 32);
	slot = d->slots + d->slot_count++ * 32;
	bzero(slot, 32);
	put32(g, slot, inode);
	if (length <= 27) {
		slot[4] = (unsigned char)length;
		bcopy(name, slot + 5, length);
		return;
	}
	CHECK(g->name_count < GEN_NAMES && length <= 510);
	record = g->names + g->name_count * g->block_size;
	put16(g, record, (uint16_t)length);
	bcopy(name, record + 2, length);
	slot[4] = 0xff;
	put32(g, slot + 8, (uint32_t)g->name_count++);
	if ((g->flags & 0x100) != 0)
		put32(g, slot + 12, gen_name_sum((const unsigned char *)name,
		    length));
}

static uint32_t
gen_dir(qnx6_gen_t *g, uint32_t parent, const char *name)
{
	uint32_t inode;

	inode = gen_inode(g, 040755);
	g->objects[inode].parent = parent;
	g->objects[inode].links = 2;
	g->objects[parent].links++;
	gen_slot(g, parent, inode, name, strlen(name));
	gen_slot(g, inode, inode, ".", 1);
	gen_slot(g, inode, parent, "..", 2);
	return (inode);
}

/* A regular file's leaves are allocated and filled now; its last block is
 * zero-padded. Its tree has the minimal depth. In a sparse volume bytes may
 * be NULL: the leaves keep the image's zeros, and a test writes what it
 * checks through block_at.
 */
static uint32_t
gen_file(qnx6_gen_t *g, uint32_t parent, const char *name,
    const unsigned char *bytes, uint64_t length)
{
	gen_object_t *f;
	uint32_t inode, *leaves;
	uint64_t n, piece;

	inode = gen_inode(g, 0100644);
	f = &g->objects[inode];
	f->size = length;
	n = (length + g->block_size - 1) / g->block_size;
	leaves = malloc((n + 1) * sizeof(*leaves));
	CHECK(leaves != NULL);
	for (uint64_t i = 0; i < n; i++) {
		leaves[i] = gen_alloc(g);
		own(f, leaves[i]);
		if (bytes == NULL) {
			CHECK(g->sparse);
			continue;
		}
		piece = length - i * g->block_size;
		if (piece > g->block_size)
			piece = g->block_size;
		bzero(block_at(g, leaves[i]), g->block_size);
		bcopy(bytes + i * g->block_size, block_at(g, leaves[i]),
		    (size_t)piece);
	}
	gen_tree(g, leaves, n, gen_depth(n, g->pointers), NULL, f, f->refs,
	    &f->depth);
	free(leaves);
	gen_slot(g, parent, inode, name, strlen(name));
	return (inode);
}

static uint32_t
gen_symlink(qnx6_gen_t *g, uint32_t parent, const char *name,
    const char *target)
{
	uint32_t inode;

	inode = gen_file(g, parent, name, (const unsigned char *)target,
	    strlen(target));
	g->objects[inode].mode = 0120777;
	return (inode);
}

static uint32_t
gen_fifo(qnx6_gen_t *g, uint32_t parent, const char *name)
{
	uint32_t inode;

	inode = gen_inode(g, 010644);
	gen_slot(g, parent, inode, name, strlen(name));
	return (inode);
}

static void
gen_link(qnx6_gen_t *g, uint32_t parent, const char *name, uint32_t inode)
{
	g->objects[inode].links++;
	gen_slot(g, parent, inode, name, strlen(name));
}

static void
gen_set_bit(qnx6_gen_t *g, uint32_t block, bool set)
{
	if (set)
		g->bitmap[block / 8] |= (unsigned char)(1u << (block % 8));
	else
		g->bitmap[block / 8] &= (unsigned char)~(1u << (block % 8));
}

/* The image bytes of inode i in the committed inode table, whose leaves
 * are contiguous from inode_leaf0.
 */
static unsigned char *
gen_inode_bytes(qnx6_gen_t *g, uint32_t inode)
{
	uint64_t offset = (uint64_t)(inode - 1) * 128;

	return (block_at(g, g->inode_leaf0 + (uint32_t)(offset /
	    g->block_size)) + offset % g->block_size);
}

/* Encode the 512-byte header of the committed snapshot. */
static void
gen_header(qnx6_gen_t *g, unsigned char *h, uint64_t serial)
{
	static const uint32_t offsets[3] = { 72, 152, 232 };
	const uint32_t *refs[3] = { g->inode_refs, g->bitmap_refs, g->name_refs };
	const uint8_t depths[3] = { g->inode_depth, g->bitmap_depth,
	    g->name_depth };
	const uint64_t lengths[3] = { (uint64_t)g->inode_count * 128,
	    (g->blocks + 7) / 8, (uint64_t)g->name_count * g->block_size };
	uint32_t used_blocks, used_inodes;

	used_blocks = used_inodes = 0;
	for (uint32_t b = 0; b < g->blocks; b++)
		used_blocks += (g->bitmap[b / 8] >> (b % 8)) & 1;
	for (uint32_t i = 1; i <= g->object_count; i++)
		used_inodes += g->objects[i].live;
	bzero(h, 512);
	put32(g, h, 0x68191122);
	put64(g, h + 8, serial);
	put32(g, h + 16, 1700000000);
	put32(g, h + 20, 1700000000 + (uint32_t)serial);
	put32(g, h + 24, g->flags);
	put16(g, h + 28, 4);
	put16(g, h + 30, 3);
	for (unsigned i = 0; i < 16; i++)
		h[32 + i] = (unsigned char)(0x10 + i);
	put32(g, h + 48, g->block_size);
	put32(g, h + 52, g->inode_count);
	put32(g, h + 56, g->inode_count - used_inodes);
	put32(g, h + 60, g->blocks);
	put32(g, h + 64, g->blocks - used_blocks);
	put32(g, h + 68, 4);
	for (unsigned t = 0; t < 3; t++) {
		put64(g, h + offsets[t], lengths[t]);
		for (unsigned i = 0; i < 16; i++)
			put32(g, h + offsets[t] + 8 + i * 4, refs[t][i]);
		h[offsets[t] + 72] = depths[t];
		h[offsets[t] + 73] = 1;
	}
	for (unsigned i = 0; i < 16; i++)
		put32(g, h + 312 + 8 + i * 4, GEN_SENTINEL);
	h[312 + 73] = 1;
	put32(g, h + 4, gen_crc(h + 8, 504));
}

/* Recompute both headers' CRCs after a test changed their bytes. */
static void
gen_reseal(qnx6_gen_t *g, uint64_t offset)
{
	unsigned char *h = g->image + offset;

	put32(g, h + 4, gen_crc(h + 8, 504));
}

/* Commit a snapshot: lay out every directory and the long-name tree in new
 * content blocks, write the inode table and the bitmap into the system run
 * at `base`, and write the header with `serial` to the first header area,
 * the second, or both.
 */
static void
gen_commit(qnx6_gen_t *g, uint32_t base, uint64_t serial, bool first,
    bool second)
{
	gen_object_t *o, names = { 0 };
	unsigned char header[512], *raw;
	uint32_t *leaves, cursor, bitmap_leaves, inode_leaves, indirect;
	uint64_t n, bitmap_bytes;

	/* Directories, each in whole blocks of slots; the root optionally in
	 * the volume's last block.
	 */
	for (uint32_t i = 1; i <= g->object_count; i++) {
		o = &g->objects[i];
		if (!o->live || (o->mode & 0170000) != 040000)
			continue;
		o->block_count = 0;
		n = (o->slot_count * 32 + g->block_size - 1) / g->block_size;
		leaves = malloc(n * sizeof(*leaves));
		CHECK(leaves != NULL);
		for (uint64_t b = 0; b < n; b++) {
			leaves[b] = i == 1 && g->root_last && n == 1 ?
			    g->blocks - 1 : gen_alloc(g);
			own(o, leaves[b]);
			bzero(block_at(g, leaves[b]), g->block_size);
		}
		for (size_t s = 0; s < o->slot_count; s++)
			bcopy(o->slots + s * 32, block_at(g, leaves[s * 32 /
			    g->block_size]) + s * 32 % g->block_size, 32);
		o->size = n * g->block_size;
		gen_tree(g, leaves, n, gen_depth(n, g->pointers), NULL, o,
		    o->refs, &o->depth);
		free(leaves);
	}

	/* The long-name tree: one block per name. */
	free(g->name_blocks);
	g->name_blocks = malloc((g->name_count + 1) * sizeof(*g->name_blocks));
	CHECK(g->name_blocks != NULL);
	for (size_t i = 0; i < g->name_count; i++) {
		g->name_blocks[i] = gen_alloc(g);
		own(&names, g->name_blocks[i]);
		bcopy(g->names + i * g->block_size,
		    block_at(g, g->name_blocks[i]), g->block_size);
	}
	gen_tree(g, g->name_blocks, g->name_count, gen_depth(g->name_count,
	    g->pointers), NULL, &names, g->name_refs, &g->name_depth);

	/* The snapshot's bitmap: the reservation and its twin, every owned
	 * block, and the bits past N in the last bitmap block.
	 */
	bitmap_bytes = (g->blocks + 7) / 8;
	bitmap_leaves = (uint32_t)((bitmap_bytes + g->block_size - 1) /
	    g->block_size);
	bzero(g->bitmap, bitmap_bytes + g->block_size);
	for (uint32_t b = 0; b < 2 * g->reservation; b++)
		gen_set_bit(g, b, true);
	for (uint32_t i = 1; i <= g->object_count; i++)
		for (size_t b = 0; g->objects[i].live &&
		    b < g->objects[i].block_count; b++)
			gen_set_bit(g, g->objects[i].blocks[b], true);
	for (size_t b = 0; b < names.block_count; b++)
		gen_set_bit(g, names.blocks[b], true);
	for (uint64_t b = g->blocks; b < (uint64_t)bitmap_leaves *
	    g->block_size * 8; b++)
		g->bitmap[b / 8] |= (unsigned char)(1u << (b % 8));
	free(names.blocks);

	/* The inode table. */
	bzero(g->table, (size_t)g->inode_count * 128);
	for (uint32_t i = 1; i <= g->object_count; i++) {
		o = &g->objects[i];
		if (!o->live)
			continue;
		raw = g->table + (size_t)(i - 1) * 128;
		put64(g, raw, o->size);
		put32(g, raw + 8, o->uid);
		put32(g, raw + 12, o->gid);
		for (unsigned t = 0; t < 4; t++)
			put32(g, raw + 16 + t * 4, o->times[t]);
		put16(g, raw + 32, o->mode);
		put16(g, raw + 34, o->links);
		for (unsigned k = 0; k < 16; k++)
			put32(g, raw + 36 + k * 4, o->refs[k]);
		raw[100] = o->depth;
		raw[101] = o->status;
	}

	/* The system run from base: the bitmap tree's indirect blocks, its
	 * leaves, then the inode tree's indirect blocks and leaves.
	 */
	g->base = base;
	cursor = base;
	free(g->system_blocks);
	g->system_blocks = malloc(g->reservation * sizeof(*g->system_blocks));
	CHECK(g->system_blocks != NULL);
	indirect = (uint32_t)(gen_tree_blocks(bitmap_leaves, g->pointers) -
	    bitmap_leaves);
	leaves = malloc(bitmap_leaves * sizeof(*leaves));
	CHECK(leaves != NULL);
	for (uint32_t i = 0; i < bitmap_leaves; i++) {
		leaves[i] = cursor + indirect + i;
		bcopy(g->bitmap + (size_t)i * g->block_size,
		    block_at(g, leaves[i]), g->block_size);
	}
	gen_tree(g, leaves, bitmap_leaves, gen_depth(bitmap_leaves, g->pointers),
	    &cursor, NULL, g->bitmap_refs, &g->bitmap_depth);
	cursor += bitmap_leaves;
	free(leaves);
	inode_leaves = (uint32_t)((uint64_t)g->inode_count * 128 / g->block_size);
	indirect = (uint32_t)(gen_tree_blocks(inode_leaves, g->pointers) -
	    inode_leaves);
	leaves = malloc(inode_leaves * sizeof(*leaves));
	CHECK(leaves != NULL);
	g->inode_leaf0 = cursor + indirect;
	for (uint32_t i = 0; i < inode_leaves; i++) {
		leaves[i] = cursor + indirect + i;
		bcopy(g->table + (size_t)i * g->block_size,
		    block_at(g, leaves[i]), g->block_size);
	}
	gen_tree(g, leaves, inode_leaves, gen_depth(inode_leaves, g->pointers),
	    &cursor, NULL, g->inode_refs, &g->inode_depth);
	cursor += inode_leaves;
	free(leaves);
	CHECK(cursor == base + g->reservation);
	g->system_count = g->reservation;
	for (uint32_t i = 0; i < g->reservation; i++)
		g->system_blocks[i] = base + i;

	gen_header(g, header, serial);
	if (first)
		bcopy(header, g->image + 8192, sizeof(header));
	if (second)
		bcopy(header, g->image + g->second, sizeof(header));
}

/*
 * Reading generated volumes through the public API.
 */

/* Reads of a generated volume by block range, armed by fixture_account:
 * bytes read from the system run [D, D + 2RB) and from the content blocks
 * [D + 2RB, D + NB), and reads of a content block already read, which seen
 * (a bit per content block) detects.
 */
typedef struct account {
	uint64_t	 system_first;
	uint64_t	 content_first;
	uint64_t	 content_end;
	uint32_t	 block_size;
	unsigned char	*seen;
	uint64_t	 system_bytes;
	uint64_t	 content_bytes;
	uint64_t	 repeats;
} account_t;

/* A volume behind the callback adapter, with fault injection and read
 * accounting. Reads come from image_fd when the volume is sparse, so its
 * holes are read without being mapped, otherwise from image.
 */
typedef struct fixture {
	fsimg_ctx_t	*ctx;
	fsimg_io_t	*io;
	fsimg_reader_t	*reader;
	unsigned char	*image;
	uint64_t	 length;
	int		 image_fd;
	uint64_t	 memory;	/* charged before any I/O object */
	uint64_t	 calls;
	uint64_t	 read_bytes;
	uint64_t	 forbid_first;	/* reads may not touch [first, end) */
	uint64_t	 forbid_end;
	uint64_t	 fail_first;	/* reads touching [first, end) fail */
	uint64_t	 fail_end;
	uint64_t	 eof_at;	/* reads make no progress in [at, end) */
	uint64_t	 eof_end;
	size_t		 short_max;	/* nonzero: the most one read returns */
	account_t	 account;	/* off while account.seen is NULL */
	uint64_t	 polls;		/* cancellation callbacks so far */
	uint64_t	 cancel_after;	/* nonzero: cancel at this poll */
	bool		 cancelled;
	int		 scratch;
	char		 directory[1024];
} fixture_t;

#define RUN_LIST	0x1u	/* open, walk and read every file */
#define RUN_VERIFY	0x2u	/* open and verify with payload */

static bool
cancelled(void *argument)
{
	fixture_t *f = argument;

	f->polls++;
	return (f->cancelled ||
	    (f->cancel_after != 0 && f->polls >= f->cancel_after));
}

/* The bytes of [first, end) that fall in [low, high). */
static uint64_t
overlap(uint64_t first, uint64_t end, uint64_t low, uint64_t high)
{
	if (first < low)
		first = low;
	if (end > high)
		end = high;
	return (end > first ? end - first : 0);
}

/* Account one delivered read of [offset, offset + length). */
static void
account_read(account_t *a, uint64_t offset, size_t length)
{
	uint64_t end, first, last;

	end = offset + length;
	a->system_bytes += overlap(offset, end, a->system_first,
	    a->content_first);
	a->content_bytes += overlap(offset, end, a->content_first,
	    a->content_end);
	if (overlap(offset, end, a->content_first, a->content_end) == 0)
		return;
	first = ((offset > a->content_first ? offset : a->content_first) -
	    a->content_first) / a->block_size;
	last = ((end < a->content_end ? end : a->content_end) - 1 -
	    a->content_first) / a->block_size;
	for (uint64_t b = first; b <= last; b++) {
		a->repeats += (a->seen[b / 8] >> (b % 8)) & 1;
		a->seen[b / 8] |= (unsigned char)(1u << (b % 8));
	}
}

/* The read callback applies the fixture's policies in a fixed order:
 * every read must lie in the slice and outside the forbidden range; a read
 * touching the failure range fails without progress; a read reaching the
 * no-progress range is cut at its start, and one starting inside it makes
 * no progress, which the bounded I/O layer reports as EOF; short_max then
 * caps what remains. Only the bytes finally delivered are accounted.
 */
static fsimg_status_t
image_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error)
{
	fixture_t *f = argument;

	f->calls++;
	CHECK(offset <= f->length && length <= f->length - offset);
	CHECK(offset + length <= f->forbid_first || offset >= f->forbid_end);
	*transferred = 0;
	if (offset < f->fail_end && offset + length > f->fail_first)
		return (fsimg_error_set(error, FSIMG_EIO, EIO, FSIMG_OP_IO,
		    "injected QNX6 read failure"));
	if (offset >= f->eof_at && offset < f->eof_end)
		return (FSIMG_OK);
	if (offset < f->eof_at && length > f->eof_at - offset)
		length = (size_t)(f->eof_at - offset);
	if (f->short_max != 0 && length > f->short_max)
		length = f->short_max;
	if (f->account.seen != NULL)
		account_read(&f->account, offset, length);
	if (f->image_fd != -1)
		CHECK(pread(f->image_fd, buffer, length, (off_t)offset) ==
		    (ssize_t)length);
	else
		bcopy(f->image + offset, buffer, length);
	f->read_bytes += length;
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
image_size(void *argument, uint64_t *size, fsimg_error_t *error FSIMG_UNUSED)
{
	*size = ((fixture_t *)argument)->length;
	return (FSIMG_OK);
}

/* Replace the fixture's I/O object with one whose slice is the first
 * length bytes of the image.
 */
static void
fixture_resize(fixture_t *f, uint64_t length)
{
	fsimg_io_ops_t ops;

	fsimg_io_free(f->io);
	f->length = length;
	fsimg_io_ops_init(&ops);
	ops.read_at = image_read;
	ops.get_size = image_size;
	CHECK(fsimg_io_from_callbacks(f->ctx, &ops, f, &f->io, NULL) == FSIMG_OK);
}

static uint64_t
charged_memory(fsimg_ctx_t *ctx)
{
	fsimg_stats_t stats;

	CHECK(fsimg_ctx_get_stats(ctx, &stats, NULL) == FSIMG_OK);
	return (stats.memory_bytes);
}

/* A fixture over image, or over image_fd when it is not -1, whose context
 * has a 16 MiB memory budget and, when nonzero, the given work and scratch
 * limits.
 */
static void
fixture_limited(fixture_t *f, unsigned char *image, int image_fd,
    uint64_t length, uint64_t max_work, uint64_t scratch_bytes)
{
	fsimg_options_t options;

	bzero(f, sizeof(*f));
	f->image = image;
	f->length = length;
	f->image_fd = image_fd;
	f->eof_at = UINT64_MAX;
	f->eof_end = UINT64_MAX;
	temp_name(f->directory, sizeof(f->directory), "fsimg-qnx6");
	CHECK(mkdtemp(f->directory) != NULL);
	f->scratch = open(f->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(f->scratch != -1);
	fsimg_options_init(&options);
	options.memory_bytes = 16 * 1024 * 1024;
	options.scratch_directory_fd = f->scratch;
	options.cancelled = cancelled;
	options.cancel_argument = f;
	if (max_work != 0)
		options.max_work = max_work;
	if (scratch_bytes != 0)
		options.scratch_bytes = scratch_bytes;
	CHECK(fsimg_ctx_new(&options, &f->ctx, NULL) == FSIMG_OK);
	f->memory = charged_memory(f->ctx);
	fixture_resize(f, length);
}

/* A fixture over an in-memory image. */
static void
fixture_new(fixture_t *f, unsigned char *image, uint64_t length,
    uint64_t max_work)
{
	fixture_limited(f, image, -1, length, max_work, 0);
}

/* A fixture over a generated volume, sparse or not. */
static void
fixture_gen(fixture_t *f, const qnx6_gen_t *g, uint64_t max_work)
{
	fixture_limited(f, g->image, g->image_fd, g->length, max_work, 0);
}

/* Give the context a metadata cache of `pages` pages, so that small
 * volumes already spill their metadata to scratch.
 */
static void
fixture_cache(fixture_t *f, uint32_t pages)
{
	fsimg_io_free(f->io);
	f->io = NULL;
	CHECK(fsimg_test_meta_cache(f->ctx, pages, NULL) == FSIMG_OK);
	f->memory = charged_memory(f->ctx);
	fixture_resize(f, f->length);
}

/* Account reads of g's system run and content blocks. */
static void
fixture_account(fixture_t *f, const qnx6_gen_t *g)
{
	account_t *a = &f->account;

	bzero(a, sizeof(*a));
	a->system_first = GEN_DATA;
	a->content_first = GEN_DATA + 2 * (uint64_t)g->reservation *
	    g->block_size;
	a->content_end = GEN_DATA + (uint64_t)g->blocks * g->block_size;
	a->block_size = g->block_size;
	a->seen = calloc(g->blocks / 8 + 1, 1);
	CHECK(a->seen != NULL);
}

/* Release everything and check that no memory, scratch or descriptor
 * outlived the reader and the I/O object.
 */
static void
fixture_free(fixture_t *f)
{
	fsimg_stats_t stats;

	free(f->account.seen);
	fsimg_reader_free(f->reader);
	fsimg_io_free(f->io);
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.scratch_bytes == 0 && stats.descriptors == 1);
	CHECK(stats.memory_bytes == f->memory);
	fsimg_ctx_free(f->ctx);
	CHECK(close(f->scratch) == 0);
	CHECK(rmdir(f->directory) == 0);
}

/* Walk every entry, reading each file and symbolic link completely. */
static fsimg_status_t
walk_all(fixture_t *f, uint64_t *entries, fsimg_error_t *error)
{
	const fsimg_entry_t *entry;
	unsigned char buffer[8192];
	uint64_t offset;
	size_t count;
	fsimg_status_t status;

	*entries = 0;
	for (;;) {
		status = fsimg_reader_next(f->reader, &entry, error);
		if (status == FSIMG_DONE)
			return (FSIMG_OK);
		if (status != FSIMG_OK)
			return (status);
		(*entries)++;
		if (entry->kind != FSIMG_ENTRY_FILE &&
		    entry->kind != FSIMG_ENTRY_SYMLINK)
			continue;
		for (offset = 0; offset < entry->size; offset += count) {
			status = fsimg_reader_read_at(f->reader, offset, buffer,
			    sizeof(buffer), &count, error);
			if (status != FSIMG_OK)
				return (status);
			CHECK(count != 0);
		}
	}
}

/* Run the selected operations on an image and return the first failure. */
static fsimg_status_t
run_image(unsigned char *image, int image_fd, uint64_t length, unsigned what,
    fsimg_error_t *error)
{
	fixture_t f;
	fsimg_verify_report_t report;
	uint64_t entries;
	fsimg_status_t status;

	status = FSIMG_OK;
	if ((what & RUN_LIST) != 0) {
		fixture_limited(&f, image, image_fd, length, 0, 0);
		status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, error);
		if (status == FSIMG_OK)
			status = walk_all(&f, &entries, error);
		fixture_free(&f);
	}
	if (status == FSIMG_OK && (what & RUN_VERIFY) != 0) {
		fixture_limited(&f, image, image_fd, length, 0, 0);
		status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, error);
		if (status == FSIMG_OK)
			status = fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD,
			    &report, error);
		CHECK(status != FSIMG_OK || report.complete);
		fixture_free(&f);
	}
	return (status);
}

static void
expect(qnx6_gen_t *g, unsigned what, fsimg_status_t expected,
    const char *label)
{
	fsimg_error_t error;
	fsimg_status_t status;

	bzero(&error, sizeof(error));
	status = run_image(g->image, g->image_fd, g->length, what, &error);
	/* QNX6_TRACE=1 shows each case's diagnostic, to confirm that a
	 * mutation fails for its intended reason.
	 */
	if (getenv("QNX6_TRACE") != NULL)
		fprintf(stderr, "TRACE %s: %d %s\n", label, status, error.message);
	if (status != expected)
		fprintf(stderr, "%s: expected %d, got %d (%s)\n", label,
		    expected, status, error.message);
	CHECK(status == expected);
}

/*
 * The sample volume: every entry kind, inline and long names, trees of
 * depth 0 and 1, a hard link and full metadata on one file.
 */

typedef struct sample {
	uint32_t	 empty;
	uint32_t	 one;
	uint32_t	 direct;
	uint32_t	 indirect;
	uint32_t	 d1;
	uint32_t	 d2;
	uint32_t	 nested;
	uint32_t	 symlink;
	uint32_t	 fifo;
	uint32_t	 attrs;
} sample_t;

/* Root slot indexes of the sample's children. */
enum {
	SLOT_BOOT = 2, SLOT_EMPTY, SLOT_ONE, SLOT_DIRECT, SLOT_INDIRECT,
	SLOT_N27, SLOT_N28, SLOT_N510, SLOT_E_ACUTE, SLOT_EMOJI, SLOT_D1,
	SLOT_HARDLINK, SLOT_SYMLINK, SLOT_FIFO, SLOT_ATTRS
};

static unsigned char *
pattern(uint64_t length, unsigned seed)
{
	unsigned char *bytes = malloc(length + 1);

	CHECK(bytes != NULL);
	for (uint64_t i = 0; i < length; i++)
		bytes[i] = (unsigned char)(i * 7 + seed + i / 251);
	return (bytes);
}

static void
sample_build(qnx6_gen_t *g, sample_t *s, uint32_t block_size, bool big,
    uint32_t flags, uint64_t length)
{
	char n27[28], n28[29], n510[511];
	unsigned char *bytes;
	gen_object_t *a;

	gen_init(g, block_size, big, length, GEN_INODES, flags);
	memset(n27, 'a', 27);
	n27[27] = '\0';
	memset(n28, 'a', 27);
	n28[27] = 'b';
	n28[28] = '\0';
	memset(n510, 'c', 510);
	n510[510] = '\0';
	s->empty = gen_file(g, 1, "empty", NULL, 0);
	s->one = gen_file(g, 1, "one", (const unsigned char *)"x", 1);
	bytes = pattern(16 * (uint64_t)block_size + 1, 3);
	s->direct = gen_file(g, 1, "direct16", bytes, 16 * (uint64_t)block_size);
	s->indirect = gen_file(g, 1, "indirect17", bytes,
	    16 * (uint64_t)block_size + 1);
	free(bytes);
	gen_file(g, 1, n27, (const unsigned char *)"27", 2);
	gen_file(g, 1, n28, (const unsigned char *)"28", 2);
	gen_file(g, 1, n510, (const unsigned char *)"510", 3);
	gen_file(g, 1, "\xc3\xa9", (const unsigned char *)"e", 1);
	gen_file(g, 1, "\xf0\x9f\x98\x80", (const unsigned char *)"s", 1);
	s->d1 = gen_dir(g, 1, "d1");
	s->d2 = gen_dir(g, s->d1, "d2");
	s->nested = gen_file(g, s->d2, "f", (const unsigned char *)"sub", 3);
	gen_link(g, 1, "hardlink", s->direct);
	s->symlink = gen_symlink(g, 1, "symlink", "direct16");
	s->fifo = gen_fifo(g, 1, "fifo");
	s->attrs = gen_file(g, 1, "attrs", (const unsigned char *)"m", 1);
	a = &g->objects[s->attrs];
	a->uid = 123;
	a->gid = 456;
	a->mode = 0104751;
	a->times[0] = 0;
	a->times[1] = UINT32_C(0xffffffff);
	a->times[2] = 1;
	a->times[3] = UINT32_C(0x80000000);
	gen_commit(g, 0, 1, true, true);
}

/* The default sample: B = 1024, little endian, checksums enabled. */
static void
sample(qnx6_gen_t *g, sample_t *s)
{
	sample_build(g, s, 1024, false, 0x100, 1048576 + 1536);
}

/* Slot index of directory dir, in the committed image. */
static unsigned char *
dir_slot(qnx6_gen_t *g, uint32_t dir, size_t index)
{
	const gen_object_t *d = &g->objects[dir];

	CHECK(d->depth == 0 && index * 32 / g->block_size < d->block_count);
	return (block_at(g, d->blocks[index * 32 / g->block_size]) +
	    index * 32 % g->block_size);
}

/* The bitmap byte holding block's bit in the committed system run, whose
 * bitmap leaves follow the bitmap tree's indirect blocks from its base.
 */
static unsigned char *
bitmap_byte(qnx6_gen_t *g, uint32_t block)
{
	uint64_t leaves, indirect;

	leaves = (((uint64_t)g->blocks + 7) / 8 + g->block_size - 1) /
	    g->block_size;
	indirect = gen_tree_blocks(leaves, g->pointers) - leaves;
	return (block_at(g, g->base + (uint32_t)indirect + block / 8 /
	    g->block_size) + block / 8 % g->block_size);
}

/* Change a field of both headers and recompute their CRCs. */
static void
header_put32(qnx6_gen_t *g, size_t offset, uint32_t value)
{
	put32(g, g->image + 8192 + offset, value);
	put32(g, g->image + g->second + offset, value);
	gen_reseal(g, 8192);
	gen_reseal(g, g->second);
}

/*
 * Tests.
 */

/* The production codecs against the design's check values, and the
 * generator's independent arithmetic against the same values.
 */
static void
test_codecs(void)
{
	unsigned char e_acute[28], cjk[30];
	uint64_t blocks;
	unsigned depth;

	CHECK(qnx6_crc32((const unsigned char *)"", 0) == 0);
	CHECK(qnx6_crc32((const unsigned char *)"123456789", 9) ==
	    UINT32_C(0x89a1897f));
	CHECK(gen_crc((const unsigned char *)"123456789", 9) ==
	    UINT32_C(0x89a1897f));
	CHECK(qnx6_name_checksum((const unsigned char *)"a", 1) == 0x61);
	CHECK(qnx6_name_checksum((const unsigned char *)"ab", 2) ==
	    UINT32_C(0x80000092));
	CHECK(qnx6_name_checksum((const unsigned char *)"abc", 3) ==
	    UINT32_C(0x400000ac));
	CHECK(qnx6_name_checksum((const unsigned char *)"", 0) ==
	    UINT32_C(0xffffffff));
	for (unsigned i = 0; i < 14; i++) {
		e_acute[i * 2] = 0xc3;
		e_acute[i * 2 + 1] = 0xa9;
	}
	for (unsigned i = 0; i < 10; i++) {
		cjk[i * 3] = 0xe4;
		cjk[i * 3 + 1] = 0xb8;
		cjk[i * 3 + 2] = 0xad;
	}
	CHECK(qnx6_name_checksum(e_acute, 28) == UINT32_C(0x55554083));
	CHECK(qnx6_name_checksum(cjk, 30) == UINT32_C(0xfffffbaf));
	CHECK(gen_name_sum(e_acute, 28) == UINT32_C(0x55554083));
	CHECK(gen_name_sum(cjk, 30) == UINT32_C(0xfffffbaf));

	/* Tree shapes: minimal depth and total blocks, including the native
	 * B = 512 system run (bitmap 33 blocks, inode tree 4128).
	 */
	for (uint32_t p = 128; p <= 1024; p *= 2) {
		for (uint64_t n = 0; n <= 16 * (uint64_t)p * p + 1; n += n < 40 ? 1 : 97) {
			CHECK(qnx6_tree_min_depth(n, p, &depth));
			CHECK(depth == gen_depth(n, p));
			CHECK(qnx6_tree_blocks(n, p, depth, &blocks));
			CHECK(blocks == gen_tree_blocks(n, p));
		}
	}
	CHECK(qnx6_tree_min_depth(32, 128, &depth) && depth == 1);
	CHECK(qnx6_tree_blocks(32, 128, 1, &blocks) && blocks == 33);
	CHECK(qnx6_tree_min_depth(4095, 128, &depth) && depth == 2);
	CHECK(qnx6_tree_blocks(4095, 128, 2, &blocks) && blocks == 4128);
	CHECK(!qnx6_tree_min_depth(UINT64_MAX, 128, &depth));
}

static const char *
kind_name(fsimg_entry_kind_t kind)
{
	switch (kind) {
	case FSIMG_ENTRY_FILE:
		return ("file");
	case FSIMG_ENTRY_DIRECTORY:
		return ("dir");
	case FSIMG_ENTRY_SYMLINK:
		return ("link");
	case FSIMG_ENTRY_FIFO:
		return ("fifo");
	}
	return ("?");
}

/* Open, list and verify the sample at one geometry, checking every entry,
 * its metadata and content, and the volume accounting.
 */
static void
check_sample(uint32_t block_size, bool big, uint32_t flags)
{
	static const struct {
		const char *path;
		const char *kind;
		uint64_t blocks;	/* size in blocks, plus extra bytes */
		uint64_t extra;
	} expected[] = {
		{ "/.boot", "dir", 0, 0 },
		{ "/empty", "file", 0, 0 },
		{ "/one", "file", 0, 1 },
		{ "/direct16", "file", 16, 0 },
		{ "/indirect17", "file", 16, 1 },
		{ "/aaaaaaaaaaaaaaaaaaaaaaaaaaa", "file", 0, 2 },
		{ "/aaaaaaaaaaaaaaaaaaaaaaaaaaab", "file", 0, 2 },
		{ NULL, "file", 0, 3 },		/* the 510-byte name */
		{ "/\xc3\xa9", "file", 0, 1 },
		{ "/\xf0\x9f\x98\x80", "file", 0, 1 },
		{ "/d1", "dir", 0, 0 },
		{ "/d1/d2", "dir", 0, 0 },
		{ "/d1/d2/f", "file", 0, 3 },
		{ "/hardlink", "file", 16, 0 },
		{ "/symlink", "link", 0, 8 },
		{ "/fifo", "fifo", 0, 0 },
		{ "/attrs", "file", 0, 1 }
	};
	qnx6_gen_t g;
	sample_t s;
	fixture_t f;
	fsimg_volume_info_t info;
	fsimg_verify_report_t report;
	const fsimg_entry_t *entry;
	unsigned char buffer[17 * 4096], *bytes;
	size_t count, entries;
	fsimg_error_t error;
	fsimg_status_t status;

	sample_build(&g, &s, block_size, big, flags, block_size == 512 ?
	    1048576 + 512 : 2097152 + 3584);
	fixture_gen(&f, &g, 0);
	status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, &error);
	if (status != FSIMG_OK)
		fprintf(stderr, "B=%u big=%d: %s\n", block_size, big, error.message);
	CHECK(status == FSIMG_OK);
	CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.format, "qnx6") == 0 && info.clean);
	CHECK(info.allocation_unit_bytes == block_size &&
	    info.allocation_units == g.blocks && info.volume_bytes == g.length);
	CHECK((info.valid & FSIMG_VOLUME_UUID) != 0 && info.uuid[0] == 0x10 &&
	    info.uuid[15] == 0x1f);

	bytes = pattern(16 * (uint64_t)block_size + 1, 3);
	entries = 0;
	while ((status = fsimg_reader_next(f.reader, &entry, &error)) == FSIMG_OK) {
		CHECK(entries < sizeof(expected) / sizeof(expected[0]));
		if (expected[entries].path != NULL &&
		    strcmp(entry->path, expected[entries].path) != 0)
			fprintf(stderr, "entry %zu: %s\n", entries, entry->path);
		CHECK(expected[entries].path != NULL ?
		    strcmp(entry->path, expected[entries].path) == 0 :
		    strlen(entry->path) == 511);
		CHECK(strcmp(kind_name(entry->kind), expected[entries].kind) == 0);
		CHECK(entry->size == expected[entries].blocks * block_size +
		    expected[entries].extra);
		CHECK(entry->metadata.valid == (FSIMG_META_CREATED |
		    FSIMG_META_MODIFIED | FSIMG_META_ACCESSED |
		    FSIMG_META_CHANGED | FSIMG_META_OWNER | FSIMG_META_MODE));
		CHECK(entry->metadata.modified.basis == FSIMG_TIME_UTC &&
		    entry->metadata.modified.precision_ns == 1000000000);
		if (entry->kind == FSIMG_ENTRY_FILE ||
		    entry->kind == FSIMG_ENTRY_SYMLINK) {
			CHECK(fsimg_reader_read_at(f.reader, 0, buffer,
			    sizeof(buffer), &count, NULL) == FSIMG_OK);
			CHECK(count == entry->size);
		}
		if (strcmp(entry->path, "/indirect17") == 0 ||
		    strcmp(entry->path, "/hardlink") == 0)
			CHECK(bcmp(buffer, bytes, count) == 0);
		if (strcmp(entry->path, "/symlink") == 0)
			CHECK(bcmp(buffer, "direct16", 8) == 0);
		if (strcmp(entry->path, "/.boot") == 0)
			CHECK(entry->metadata.mode == 0700);
		if (strcmp(entry->path, "/attrs") == 0) {
			CHECK(entry->metadata.uid == 123 && entry->metadata.gid == 456);
			CHECK(entry->metadata.mode == 04751);
			CHECK(entry->metadata.created.seconds == 0);
			CHECK(entry->metadata.modified.seconds == INT64_C(4294967295));
			CHECK(entry->metadata.accessed.seconds == 1);
			CHECK(entry->metadata.changed.seconds == INT64_C(2147483648));
		}
		entries++;
	}
	CHECK(status == FSIMG_DONE && entries == sizeof(expected) / sizeof(expected[0]));
	free(bytes);
	fsimg_reader_free(f.reader);

	/* Verification: 14 non-directories, 4 directories with the root. */
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	status = fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD, &report, &error);
	if (status != FSIMG_OK)
		fprintf(stderr, "verify B=%u big=%d: %s\n", block_size, big, error.message);
	CHECK(status == FSIMG_OK && report.complete);
	CHECK(report.files == 14 && report.directories == 4);
	CHECK(report.allocated_clusters + report.free_clusters == g.blocks);
	CHECK(report.free_clusters == get32(&g, g.image + 8192 + 64));
	/* The hard link's bytes are read once, under its first name. */
	CHECK(report.payload_bytes_read ==
	    report.logical_bytes - 16 * (uint64_t)block_size);
	CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
	CHECK((info.valid & FSIMG_VOLUME_FREE) != 0 &&
	    info.free_bytes == report.free_clusters * block_size);
	fixture_free(&f);
	gen_free(&g);
}

static void
test_matrix(void)
{
	static const uint32_t sizes[] = { 512, 1024, 2048, 4096 };

	for (unsigned big = 0; big < 2; big++)
		for (unsigned i = 0; i < 4; i++)
			for (unsigned checksum = 0; checksum < 2; checksum++)
				check_sample(sizes[i], big != 0,
				    checksum != 0 ? 0x100 : 0);
}

/* Deep and fragmented trees, every system-tree depth, and an object in the
 * volume's last block.
 */
static void
test_shapes(void)
{
	qnx6_gen_t g;
	unsigned char *bytes;
	uint64_t length;

	/* A depth-2 file at B = 512 (more than 16 * 128 leaves), fragmented
	 * so every leaf and indirect block is a separate run.
	 */
	gen_init(&g, 512, true, 4 * 1048576 + 512, GEN_INODES, 0x100);
	g.stride = 2;
	length = 2100 * 512 + 77;
	bytes = pattern(length, 9);
	gen_file(&g, 1, "deep", bytes, length);
	free(bytes);
	gen_commit(&g, 0, 1, true, true);
	CHECK(g.objects[3].depth == 2);
	expect(&g, RUN_LIST | RUN_VERIFY, FSIMG_OK, "depth-2 fragmented file");
	gen_free(&g);

	/* Inode trees of depth 1 and 2 and a bitmap tree of depth 1. */
	gen_init(&g, 512, false, 1048576, 256, 0x100);
	gen_commit(&g, 0, 1, true, true);
	CHECK(g.inode_depth == 1 && g.bitmap_depth == 0);
	expect(&g, RUN_LIST | RUN_VERIFY, FSIMG_OK, "inode tree depth 1");
	gen_free(&g);
	gen_init(&g, 512, false, 4 * 1048576 + 512, 8448, 0x100);
	gen_commit(&g, 0, 1, true, true);
	CHECK(g.inode_depth == 2);
	expect(&g, RUN_LIST | RUN_VERIFY, FSIMG_OK, "inode tree depth 2");
	gen_free(&g);
	gen_init(&g, 512, false, 40 * 1048576, GEN_INODES, 0x100);
	gen_commit(&g, 0, 1, true, true);
	CHECK(g.bitmap_depth == 1);
	expect(&g, RUN_LIST | RUN_VERIFY, FSIMG_OK, "bitmap tree depth 1");
	gen_free(&g);

	/* The root directory in block N - 1, as in the research fixture: the
	 * last ownership run ends exactly at N.
	 */
	gen_init(&g, 1024, false, 1048576, GEN_INODES, 0x100);
	g.root_last = true;
	gen_file(&g, 1, "f", (const unsigned char *)"x", 1);
	gen_commit(&g, 0, 1, true, true);
	CHECK(g.objects[1].blocks[0] == g.blocks - 1);
	expect(&g, RUN_LIST | RUN_VERIFY, FSIMG_OK, "root in the last block");
	gen_free(&g);
}

/* A wide root of hard links to one file with an indirect block: each name
 * exposes the inode again without claiming its indirect block twice, and
 * the root itself needs an indirect block.
 */
static void
test_hard_links(void)
{
	qnx6_gen_t g;
	fixture_t f;
	const fsimg_entry_t *entry;
	unsigned char *bytes;
	uint64_t entries;
	uint32_t file;
	unsigned reads;
	size_t count;
	fsimg_status_t status;
	unsigned char last[1];
	char name[16];

	gen_init(&g, 1024, false, 1048576, GEN_INODES, 0x100);
	bytes = pattern(17 * 1024, 5);
	file = gen_file(&g, 1, "f", bytes, 17 * 1024);
	free(bytes);
	for (unsigned i = 0; i < 3000; i++) {
		snprintf(name, sizeof(name), "l%u", i);
		gen_link(&g, 1, name, file);
	}
	gen_commit(&g, 0, 1, true, true);
	CHECK(g.objects[1].depth == 1);
	expect(&g, RUN_LIST | RUN_VERIFY, FSIMG_OK, "3001 names of one file");
	gen_free(&g);

	/* Listing walks a file's tree for its first name only, so 3001 names
	 * of a 4000-block file list well within a work limit that 3001 walks
	 * of its tree would exceed. Reading a later name walks it once more.
	 */
	gen_init(&g, 1024, false, 8 * 1048576, GEN_INODES, 0x100);
	bytes = pattern(4000 * 1024, 6);
	file = gen_file(&g, 1, "f", bytes, 4000 * 1024);
	free(bytes);
	for (unsigned i = 0; i < 3000; i++) {
		snprintf(name, sizeof(name), "l%u", i);
		gen_link(&g, 1, name, file);
	}
	gen_commit(&g, 0, 1, true, true);
	fixture_gen(&f, &g, 1000000);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	entries = reads = 0;
	while ((status = fsimg_reader_next(f.reader, &entry, NULL)) ==
	    FSIMG_OK) {
		entries++;
		if (entry->kind != FSIMG_ENTRY_FILE || reads == 10)
			continue;
		CHECK(fsimg_reader_read_at(f.reader, entry->size - 1, last, 1,
		    &count, NULL) == FSIMG_OK && count == 1);
		reads++;
	}
	CHECK(status == FSIMG_DONE && entries == 3002);	/* and .boot */
	CHECK(reads == 10);
	fixture_free(&f);
	gen_free(&g);
}

/* The second header is found from the slice length alone. */
static void
test_geometry(void)
{
	qnx6_gen_t g;
	sample_t s;
	fixture_t f;
	unsigned char *bigger;
	uint64_t length, data_end, entries;

	/* A short filesystem whose slice extends past D + N * B: the second
	 * header is at the slice end and the gap is never read.
	 */
	sample(&g, &s);
	data_end = GEN_DATA + (uint64_t)g.blocks * g.block_size;
	length = g.length + 65536;
	bigger = malloc(length);
	CHECK(bigger != NULL);
	memset(bigger, GEN_POISON, length);
	bcopy(g.image, bigger, data_end);
	bcopy(g.image + g.second, bigger + (length / 4096 * 4096 - 4096), 4096);
	fixture_new(&f, bigger, length, 0);
	f.forbid_first = data_end;
	f.forbid_end = length / 4096 * 4096 - 4096;
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(walk_all(&f, &entries, NULL) == FSIMG_OK && entries == 17);
	fixture_free(&f);
	CHECK(run_image(bigger, -1, length, RUN_VERIFY, NULL) == FSIMG_OK);

	/* The same bytes with the second header left at D + N * B: QNX looks
	 * for it at the slice end, so the volume is corrupt.
	 */
	memset(bigger + (length / 4096 * 4096 - 4096), GEN_POISON, 4096);
	bcopy(g.image + g.second, bigger + g.second, 4096);
	CHECK(run_image(bigger, -1, length, RUN_LIST, NULL) == FSIMG_ECORRUPT);

	/* A volume image padded past its slice end. */
	bcopy(g.image, bigger, g.length);
	bzero(bigger + g.length, 4096);
	CHECK(run_image(bigger, -1, g.length + 4096, RUN_LIST, NULL) ==
	    FSIMG_ECORRUPT);

	/* S2 <= 12288 and truncated headers. */
	CHECK(run_image(g.image, -1, 20479, RUN_LIST, NULL) == FSIMG_ECORRUPT);
	CHECK(run_image(g.image, -1, 8703, RUN_LIST, NULL) == FSIMG_EFORMAT);
	free(bigger);
	gen_free(&g);

	/* N must fit before the second header. */
	sample(&g, &s);
	header_put32(&g, 60, g.blocks + 4);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "N past the second header");
	gen_free(&g);
}

/* Two snapshots: either header may be newer, and each describes its own
 * namespace and allocation.
 */
static void
test_snapshots(void)
{
	qnx6_gen_t g;
	sample_t s;
	fixture_t f;
	fsimg_volume_info_t info;
	const fsimg_entry_t *entry;
	bool second_seen;

	sample(&g, &s);
	gen_file(&g, 1, "second", (const unsigned char *)"2", 1);
	gen_commit(&g, g.reservation, 2, false, true);
	expect(&g, RUN_LIST | RUN_VERIFY, FSIMG_OK, "newer second header");
	fixture_gen(&f, &g, 0);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	second_seen = false;
	while (fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK)
		second_seen |= strcmp(entry->path, "/second") == 0;
	CHECK(second_seen);
	fixture_free(&f);

	/* Make the first header newer: the older namespace returns, with its
	 * own bitmap.
	 */
	put64(&g, g.image + 8192 + 8, 3);
	gen_reseal(&g, 8192);
	expect(&g, RUN_LIST | RUN_VERIFY, FSIMG_OK, "newer first header");
	fixture_gen(&f, &g, 0);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	while (fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK)
		CHECK(strcmp(entry->path, "/second") != 0);
	fixture_free(&f);

	/* Equal serials with different snapshots are ambiguous. */
	put64(&g, g.image + 8192 + 8, 2);
	gen_reseal(&g, 8192);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "equal conflicting serials");
	gen_free(&g);

	/* A damaged CRC in either header, including the last byte. */
	for (unsigned which = 0; which < 2; which++) {
		sample(&g, &s);
		g.image[(which == 0 ? 8192 : g.second) + 511] ^= 1;
		expect(&g, RUN_LIST, FSIMG_ECORRUPT, "header CRC");
		gen_free(&g);
	}

	/* The mounted bit is snapshot state: accepted in one header and
	 * reported as an unclean volume.
	 */
	sample(&g, &s);
	put32(&g, g.image + g.second + 24, 0x101);
	put64(&g, g.image + g.second + 8, 2);
	gen_reseal(&g, g.second);
	expect(&g, RUN_LIST | RUN_VERIFY, FSIMG_OK, "mounted bit");
	fixture_gen(&f, &g, 0);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
	CHECK(!info.clean);
	fixture_free(&f);

	/* Equal serials differing only in the mounted bit are still two
	 * different snapshot descriptions.
	 */
	put64(&g, g.image + g.second + 8, 1);
	gen_reseal(&g, g.second);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "equal serials, mounted bit differs");

	/* Feature flags must agree, and only the classic ones are known. */
	put32(&g, g.image + g.second + 24, 0x001);
	gen_reseal(&g, g.second);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "feature flags differ");
	gen_free(&g);
	sample(&g, &s);
	header_put32(&g, 24, 0x300);
	expect(&g, RUN_LIST, FSIMG_EUNSUPPORTED, "unknown feature flag");
	gen_free(&g);

	/* Identity fields must agree. */
	sample(&g, &s);
	g.image[g.second + 40] ^= 1;
	gen_reseal(&g, g.second);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "UUID differs");
	gen_free(&g);
}

/* The probe claims only a classic header at byte 8192; byte-zero layouts
 * and extended headers are recognized but unsupported.
 */
static void
test_probe(void)
{
	qnx6_gen_t g;
	sample_t s;
	fixture_t f;
	fsimg_read_options_t options;
	unsigned char *zero;

	sample(&g, &s);
	bcopy(g.image + 8192, g.image, 512);
	expect(&g, RUN_LIST, FSIMG_EUNSUPPORTED, "magic at 0 and 8192");
	bzero(g.image + 8192, 4);
	expect(&g, RUN_LIST, FSIMG_EUNSUPPORTED, "magic at 0 only");
	gen_free(&g);

	zero = calloc(1, 65536);
	CHECK(zero != NULL);
	CHECK(run_image(zero, -1, 65536, RUN_LIST, NULL) == FSIMG_EFORMAT);
	fixture_new(&f, zero, 65536, 0);
	fsimg_read_options_init(&options);
	options.format = "qnx6";
	CHECK(fsimg_reader_open(f.ctx, f.io, &options, &f.reader, NULL) ==
	    FSIMG_EFORMAT);
	fixture_free(&f);
	free(zero);

	sample(&g, &s);
	header_put32(&g, 48, 8192);
	expect(&g, RUN_LIST, FSIMG_EUNSUPPORTED, "large block size");
	gen_free(&g);
	sample(&g, &s);
	put16(&g, g.image + 8192 + 28, 3);
	put16(&g, g.image + g.second + 28, 3);
	gen_reseal(&g, 8192);
	gen_reseal(&g, g.second);
	expect(&g, RUN_LIST, FSIMG_EUNSUPPORTED, "version 3");
	gen_free(&g);
	sample(&g, &s);
	header_put32(&g, 400, 1);
	expect(&g, RUN_LIST, FSIMG_EUNSUPPORTED, "extension bytes");
	gen_free(&g);
	sample(&g, &s);
	header_put32(&g, 312 + 8, 5);
	expect(&g, RUN_LIST, FSIMG_EUNSUPPORTED, "fourth tree in use");
	gen_free(&g);
	sample(&g, &s);
	g.image[8192 + 72 + 73] = 2;
	g.image[g.second + 72 + 73] = 2;
	gen_reseal(&g, 8192);
	gen_reseal(&g, g.second);
	expect(&g, RUN_LIST, FSIMG_EUNSUPPORTED, "tree mode");
	gen_free(&g);
	sample(&g, &s);
	header_put32(&g, 30, 101);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "reserve above 100");
	gen_free(&g);
	sample(&g, &s);
	header_put32(&g, 68, 65);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "65 groups");
	gen_free(&g);
	sample(&g, &s);
	header_put32(&g, 152, (g.blocks + 7) / 8 + 1);
	expect(&g, RUN_LIST, FSIMG_EUNSUPPORTED, "oversized bitmap");
	gen_free(&g);
	sample(&g, &s);
	header_put32(&g, 72, 64 * 128 - 128);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "short inode table");
	gen_free(&g);
}

/* Block references: required, unused, out of range, cyclic, shared, and
 * aliasing metadata, each found while listing or by verification.
 */
static void
test_trees(void)
{
	qnx6_gen_t g;
	sample_t s;
	unsigned char *inode, *indirect;
	uint32_t root_block, indirect_block;

	/* indirect17: sixteen leaves under one indirect block. */
	sample(&g, &s);
	inode = gen_inode_bytes(&g, s.indirect);
	put32(&g, inode + 36, GEN_SENTINEL);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "unused required reference");
	gen_free(&g);
	sample(&g, &s);
	put32(&g, gen_inode_bytes(&g, s.indirect) + 36, g.blocks);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "reference past N");
	gen_free(&g);
	/* At depth 2 the indirect block is its own child. */
	sample(&g, &s);
	indirect_block = g.objects[s.indirect].blocks[17];
	put32(&g, block_at(&g, indirect_block), indirect_block);
	gen_inode_bytes(&g, s.indirect)[100] = 2;
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "ancestor cycle");
	gen_free(&g);
	sample(&g, &s);
	gen_inode_bytes(&g, s.one)[100] = 6;
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "depth 6");
	gen_free(&g);

	/* A valid reference past the logical end is unqualified
	 * preallocation; an invalid one is corruption.
	 */
	sample(&g, &s);
	put32(&g, gen_inode_bytes(&g, s.one) + 40, 2 * g.reservation + 1);
	expect(&g, RUN_LIST, FSIMG_EUNSUPPORTED, "preallocation");
	put32(&g, gen_inode_bytes(&g, s.one) + 40, g.blocks + 1);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "invalid unused reference");
	gen_free(&g);
	sample(&g, &s);
	put32(&g, gen_inode_bytes(&g, s.fifo) + 36, 2 * g.reservation);
	expect(&g, RUN_LIST, FSIMG_EUNSUPPORTED, "FIFO with a block");
	gen_free(&g);

	/* Two leaves of one file in one block: the second claim fails. */
	sample(&g, &s);
	indirect = block_at(&g, g.objects[s.indirect].blocks[17]);
	put32(&g, indirect + 4, get32(&g, indirect));
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "same-block branches");
	gen_free(&g);

	/* A tree whose branches share an indirect block: three root slots
	 * name one indirect block whose every slot names one leaf, so three
	 * real blocks would declare 3 * P leaves. Every block is claimed
	 * once, so the second visit fails while listing.
	 */
	sample(&g, &s);
	indirect_block = g.objects[s.indirect].blocks[17];
	inode = gen_inode_bytes(&g, s.indirect);
	put64(&g, inode, 3 * (uint64_t)g.pointers * g.block_size);
	for (unsigned k = 0; k < 3; k++)
		put32(&g, inode + 36 + k * 4, indirect_block);
	for (uint32_t k = 0; k < g.pointers; k++)
		put32(&g, block_at(&g, indirect_block) + k * 4,
		    g.objects[s.indirect].blocks[0]);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "shared indirect block");
	gen_free(&g);

	/* A file aliasing a directory entered after the file is exposed: the
	 * file's leaves are recorded, so entering the directory fails.
	 */
	sample(&g, &s);
	put32(&g, gen_inode_bytes(&g, s.one) + 36, g.objects[s.d2].blocks[0]);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "payload in a later directory");
	gen_free(&g);

	/* Two files sharing a leaf. */
	sample(&g, &s);
	put32(&g, gen_inode_bytes(&g, s.one) + 36,
	    g.objects[s.direct].blocks[0]);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "shared leaf");
	gen_free(&g);

	/* More leaves than the depth can address, and a nonminimal depth. */
	sample(&g, &s);
	put64(&g, gen_inode_bytes(&g, s.direct), 17 * (uint64_t)g.block_size);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "17 leaves at depth 0");
	gen_free(&g);
	/* One leaf under an indirect block: the depth is not minimal, which
	 * the walker accepts. The leaves the file drops are freed.
	 */
	sample(&g, &s);
	put64(&g, gen_inode_bytes(&g, s.indirect), g.block_size);
	for (uint32_t k = 1; k < 17; k++) {
		uint32_t dropped = g.objects[s.indirect].blocks[k];

		put32(&g, block_at(&g, g.objects[s.indirect].blocks[17]) + k * 4,
		    0xffffffffu);
		*bitmap_byte(&g, dropped) &= (unsigned char)~(1u << (dropped % 8));
	}
	header_put32(&g, 64, get32(&g, g.image + 8192 + 64) + 16);
	expect(&g, RUN_LIST | RUN_VERIFY, FSIMG_OK, "one leaf at depth 1");
	gen_free(&g);

	/* System trees stay inside the reservation and ordinary objects
	 * outside it.
	 */
	sample(&g, &s);
	header_put32(&g, 72 + 8, 2 * g.reservation);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "inode table outside the reservation");
	gen_free(&g);
	sample(&g, &s);
	put32(&g, gen_inode_bytes(&g, s.d2) + 36, g.reservation);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "directory in the reservation");
	gen_free(&g);

	/* Payload aliasing the root directory, the system run, or a block
	 * the bitmap marks free, is refused as the file is exposed.
	 */
	sample(&g, &s);
	root_block = g.objects[1].blocks[0];
	put32(&g, gen_inode_bytes(&g, s.one) + 36, root_block);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "payload in a directory block");
	put32(&g, gen_inode_bytes(&g, s.one) + 36, 0);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "payload in the system run");
	gen_free(&g);
	sample(&g, &s);
	*bitmap_byte(&g, g.objects[s.one].blocks[0]) &=
	    (unsigned char)~(1u << (g.objects[s.one].blocks[0] % 8));
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "free payload block");
	gen_free(&g);
	sample(&g, &s);
	*bitmap_byte(&g, g.objects[s.indirect].blocks[17]) &=
	    (unsigned char)~(1u << (g.objects[s.indirect].blocks[17] % 8));
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "free indirect block");
	gen_free(&g);
}

/* Inode numbers, states, types and lengths. */
static void
test_inodes(void)
{
	qnx6_gen_t g;
	sample_t s;

	/* An unused slot (inode zero) is skipped, not decoded. */
	sample(&g, &s);
	put32(&g, dir_slot(&g, 1, SLOT_ONE), 0);
	expect(&g, RUN_LIST, FSIMG_OK, "unused slot");
	put32(&g, dir_slot(&g, 1, SLOT_ONE), GEN_INODES + 1);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "inode past capacity");
	gen_free(&g);

	sample(&g, &s);
	gen_inode_bytes(&g, s.one)[101] = 2;
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "free inode named");
	gen_inode_bytes(&g, s.one)[101] = 7;
	expect(&g, RUN_LIST, FSIMG_EUNSUPPORTED, "unknown status bit");
	gen_inode_bytes(&g, s.one)[101] = 1;
	expect(&g, RUN_LIST, FSIMG_OK, "formatter status");
	gen_inode_bytes(&g, s.one)[110] = 1;
	expect(&g, RUN_LIST, FSIMG_EUNSUPPORTED, "nonzero inode tail");
	gen_free(&g);

	/* An unsupported type fails its directory, without omission. */
	sample(&g, &s);
	put16(&g, gen_inode_bytes(&g, s.nested) + 32, 0140644);
	expect(&g, RUN_LIST, FSIMG_EUNSUPPORTED, "socket");
	gen_free(&g);
	sample(&g, &s);
	put64(&g, gen_inode_bytes(&g, s.d2), 33);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "directory length");
	put64(&g, gen_inode_bytes(&g, s.d2), 32);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "directory without ..");
	gen_free(&g);
	sample(&g, &s);
	put64(&g, gen_inode_bytes(&g, s.symlink), 0);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "empty symbolic link");
	gen_free(&g);
	sample(&g, &s);
	put64(&g, gen_inode_bytes(&g, s.fifo), 1);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "FIFO with content");
	gen_free(&g);
	sample(&g, &s);
	put64(&g, gen_inode_bytes(&g, s.one), (uint64_t)g.blocks * 1024 + 1);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "file larger than the volume");
	gen_free(&g);

	/* The last inode of the table. */
	gen_init(&g, 1024, false, 1048576, GEN_INODES, 0x100);
	for (uint32_t i = 3; i <= GEN_INODES; i++) {
		char name[16];

		snprintf(name, sizeof(name), "f%u", i);
		CHECK(gen_fifo(&g, 1, name) == i);
	}
	gen_commit(&g, 0, 1, true, true);
	CHECK(get32(&g, g.image + 8192 + 56) == 0);
	expect(&g, RUN_LIST | RUN_VERIFY, FSIMG_OK, "every inode in use");
	gen_free(&g);
}

/* Inline and long names, checksums and the name profile. */
static void
test_names(void)
{
	qnx6_gen_t g;
	sample_t s;
	unsigned char *slot, *record;

	sample(&g, &s);
	dir_slot(&g, 1, SLOT_N27)[4] = 28;
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "inline length 28");
	gen_free(&g);

	/* Long-name records: index 0 is the 28-byte name. */
	sample(&g, &s);
	record = block_at(&g, g.name_blocks[0]);
	put16(&g, record, 27);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "long name of 27 bytes");
	put16(&g, record, 511);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "long name of 511 bytes");
	gen_free(&g);
	sample(&g, &s);
	put32(&g, dir_slot(&g, 1, SLOT_N28) + 8, 2);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "long-name index past the tree");
	put32(&g, dir_slot(&g, 1, SLOT_N28) + 8, 1);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "long name with another checksum");
	gen_free(&g);
	sample(&g, &s);
	dir_slot(&g, 1, SLOT_N28)[20] = 1;
	expect(&g, RUN_LIST, FSIMG_EUNSUPPORTED, "long-name slot extension");
	gen_free(&g);

	/* With checksums disabled the field must be zero. */
	sample_build(&g, &s, 1024, false, 0, 1048576);
	expect(&g, RUN_LIST, FSIMG_OK, "checksums disabled");
	put32(&g, dir_slot(&g, 1, SLOT_N28) + 12, 1);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "checksum without the flag");
	gen_free(&g);

	/* QNX forbids NUL, slash and the dot names. A control byte or
	 * invalid UTF-8 is outside the profile and omits its entry, which
	 * lists without it; test_omitted_names covers omission.
	 */
	sample(&g, &s);
	slot = dir_slot(&g, 1, SLOT_ONE);
	slot[5] = '\0';
	slot[4] = 2;
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "embedded NUL");
	slot[4] = 1;
	slot[5] = '/';
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "slash");
	slot[5] = '.';
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "child named .");
	slot[5] = 0x01;
	expect(&g, RUN_LIST | RUN_VERIFY, FSIMG_OK, "control byte");
	slot[5] = 0x7f;
	expect(&g, RUN_LIST | RUN_VERIFY, FSIMG_OK, "DEL");
	slot[5] = 0xff;
	expect(&g, RUN_LIST | RUN_VERIFY, FSIMG_OK, "invalid UTF-8");
	slot[5] = 'e';
	slot[4] = 5;
	bcopy("empty", slot + 5, 5);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "duplicate siblings");
	slot[5] = 'E';
	expect(&g, RUN_LIST, FSIMG_OK, "case-distinct siblings");
	gen_free(&g);
}

/* A volume holding one symbolic link of `length` target bytes lists and
 * verifies with expected.
 */
static void
expect_link_target(size_t length, fsimg_status_t expected, const char *label)
{
	qnx6_gen_t g;
	char target[FSIMG_SYMLINK_TARGET_MAX + 1];

	CHECK(length < sizeof(target));
	memset(target, 't', length);
	target[length] = '\0';
	gen_init(&g, 1024, false, 1048576, GEN_INODES, 0x100);
	gen_symlink(&g, 1, "link", target);
	gen_commit(&g, 0, 1, true, true);
	expect(&g, RUN_LIST | RUN_VERIFY, expected, label);
	gen_free(&g);
}

/* A stored target holds 1 to QNX6_LINK_TARGET_MAX, 1023, bytes, the limit
 * QNX6's capability record declares, although the entry model carries 1024.
 */
static void
test_link_targets(void)
{
	expect_link_target(1023, FSIMG_OK, "1023-byte link target");
	expect_link_target(1024, FSIMG_EUNSUPPORTED, "1024-byte link target");
}

/* Dot entries, unused slots and directory sharing. */
static void
test_directories(void)
{
	qnx6_gen_t g;
	sample_t s;
	unsigned char *slot;

	sample(&g, &s);
	put32(&g, dir_slot(&g, s.d1, 0), s.d2);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "wrong .");
	gen_free(&g);
	sample(&g, &s);
	put32(&g, dir_slot(&g, s.d2, 1), s.d2);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "wrong ..");
	gen_free(&g);
	sample(&g, &s);
	dir_slot(&g, 1, 1)[4] = 1;
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "root .. renamed");
	gen_free(&g);

	/* Unused slots before live ones do not end the directory. */
	sample(&g, &s);
	slot = dir_slot(&g, 1, SLOT_EMPTY);
	bzero(slot, 32);
	expect(&g, RUN_LIST, FSIMG_OK, "unused slot before live ones");
	gen_free(&g);

	/* A directory named twice in its parent, or from below itself. */
	sample(&g, &s);
	put32(&g, dir_slot(&g, 1, SLOT_ONE), s.d1);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "directory named twice");
	gen_free(&g);
	sample(&g, &s);
	put32(&g, dir_slot(&g, s.d2, 2), s.d1);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "ancestor link");
	gen_free(&g);

	/* Directory blocks shared with the long-name tree. */
	sample(&g, &s);
	put32(&g, gen_inode_bytes(&g, s.d2) + 36, g.name_blocks[0]);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "directory in a long-name block");
	gen_free(&g);
}

/* Allocation: the bitmap's self-check, padding bits, orphans, counters and
 * link counts.
 */
static void
test_allocation(void)
{
	qnx6_gen_t g;
	sample_t s;
	uint32_t bitmap_block, free_block;

	sample(&g, &s);
	bitmap_block = g.bitmap_refs[0];
	*bitmap_byte(&g, bitmap_block) &= (unsigned char)~(1u << (bitmap_block % 8));
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "bitmap block marked free");
	gen_free(&g);
	sample(&g, &s);
	*bitmap_byte(&g, g.inode_leaf0) &= (unsigned char)~(1u << (g.inode_leaf0 % 8));
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "inode table block marked free");
	gen_free(&g);
	sample(&g, &s);
	*bitmap_byte(&g, g.blocks) &= (unsigned char)~(1u << (g.blocks % 8));
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "padding bit clear");
	gen_free(&g);
	/* The padding runs to the end of the last bitmap block. */
	sample(&g, &s);
	CHECK(g.blocks / 8 + 1 < g.block_size);
	block_at(&g, g.bitmap_refs[0])[g.block_size - 1] = 0x7f;
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "last padding byte clear");
	gen_free(&g);

	/* The system trees are not claimed while listing: two inode-table
	 * leaves holding only free inodes may name one block and still list.
	 * Only the ownership merge finds the block with two owners.
	 */
	sample(&g, &s);
	CHECK(g.inode_depth == 0 && g.inode_count == GEN_INODES &&
	    g.inode_count * 128 / g.block_size == 8);
	for (uint32_t i = 6 * g.block_size / 128 + 1; i <= GEN_INODES; i++)
		CHECK(!g.objects[i].live);
	header_put32(&g, 80 + 7 * 4, g.inode_refs[6]);
	expect(&g, RUN_LIST, FSIMG_OK, "inode leaves sharing a block listed");
	expect(&g, RUN_VERIFY, FSIMG_ECORRUPT, "inode leaves sharing a block verified");
	gen_free(&g);

	/* The twin region is allocated in every snapshot. */
	sample(&g, &s);
	*bitmap_byte(&g, g.reservation) &= (unsigned char)~(1u << (g.reservation % 8));
	header_put32(&g, 64, get32(&g, g.image + 8192 + 64) + 1);
	expect(&g, RUN_LIST, FSIMG_OK, "twin bit listed");
	expect(&g, RUN_VERIFY, FSIMG_ECORRUPT, "twin bit verified");
	gen_free(&g);

	/* An allocated block nothing owns, and wrong counters. */
	sample(&g, &s);
	free_block = g.next_block + 1;
	*bitmap_byte(&g, free_block) |= (unsigned char)(1u << (free_block % 8));
	header_put32(&g, 64, get32(&g, g.image + 8192 + 64) - 1);
	expect(&g, RUN_LIST, FSIMG_OK, "orphan listed");
	expect(&g, RUN_VERIFY, FSIMG_ECORRUPT, "orphan verified");
	gen_free(&g);
	sample(&g, &s);
	header_put32(&g, 64, get32(&g, g.image + 8192 + 64) - 1);
	expect(&g, RUN_VERIFY, FSIMG_ECORRUPT, "free block count");
	gen_free(&g);
	sample(&g, &s);
	header_put32(&g, 56, get32(&g, g.image + 8192 + 56) - 1);
	expect(&g, RUN_VERIFY, FSIMG_ECORRUPT, "free inode count");
	gen_free(&g);
	sample(&g, &s);
	gen_inode_bytes(&g, GEN_INODES)[101] = 1;
	expect(&g, RUN_LIST, FSIMG_OK, "unnamed allocated inode listed");
	expect(&g, RUN_VERIFY, FSIMG_ECORRUPT, "unnamed allocated inode verified");
	gen_free(&g);
	sample(&g, &s);
	put16(&g, gen_inode_bytes(&g, s.direct) + 34, 1);
	expect(&g, RUN_VERIFY, FSIMG_ECORRUPT, "hard link count");
	gen_free(&g);
	sample(&g, &s);
	put16(&g, gen_inode_bytes(&g, 1) + 34, 5);
	expect(&g, RUN_VERIFY, FSIMG_ECORRUPT, "directory link count");
	gen_free(&g);
}

/* Allocation failures at every step, read failures, cancellation and the
 * work limit end the operation cleanly; fixture_free checks that nothing
 * leaked.
 */
static void
test_failures(void)
{
	qnx6_gen_t g;
	sample_t s;
	fixture_t f;
	fsimg_verify_report_t report;
	uint64_t entries;
	fsimg_status_t status;

	sample(&g, &s);
	for (uint64_t n = 1;; n++) {
		fixture_gen(&f, &g, 0);
		fsimg_test_fail_alloc_after(f.ctx, n);
		status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL);
		if (status == FSIMG_OK)
			status = fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD,
			    &report, NULL);
		fsimg_test_fail_alloc_after(f.ctx, 0);
		fixture_free(&f);
		if (status == FSIMG_OK)
			break;
		CHECK(status == FSIMG_ENOMEM || status == FSIMG_ELIMIT);
		CHECK(n < 10000);
	}

	/* A read failure inside file content fails the read, not the
	 * listing before it.
	 */
	fixture_gen(&f, &g, 0);
	f.fail_first = GEN_DATA + (uint64_t)g.objects[s.indirect].blocks[3] *
	    g.block_size;
	f.fail_end = f.fail_first + g.block_size;
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(walk_all(&f, &entries, NULL) == FSIMG_EIO);
	fixture_free(&f);

	fixture_gen(&f, &g, 0);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	f.cancelled = true;
	CHECK(fsimg_verify(f.reader, 0, &report, NULL) == FSIMG_ECANCELLED);
	CHECK(!report.complete);
	f.cancelled = false;
	fixture_free(&f);

	fixture_gen(&f, &g, 200);
	status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL);
	if (status == FSIMG_OK)
		status = walk_all(&f, &entries, NULL);
	CHECK(status == FSIMG_ELIMIT);
	fixture_free(&f);
	gen_free(&g);
}

/* The image length of a volume of at least `blocks` blocks: the data
 * origin, the blocks rounded up to whole 4096-byte units, and the second
 * header's unit after them.
 */
static uint64_t
gen_length(uint32_t block_size, uint64_t blocks)
{
	return (16384 + (blocks * block_size + 4095) / 4096 * 4096);
}

/* The image length of the smallest volume with this inode table and
 * `content` free blocks after the reservation and its twin. R depends on N
 * through the bitmap, so N grows until it holds 2R + content.
 */
static uint64_t
gen_fit(uint32_t block_size, uint32_t inodes, uint64_t content)
{
	uint64_t table, blocks, bitmap, needed;

	table = gen_tree_blocks((uint64_t)inodes * 128 / block_size,
	    block_size / 4);
	blocks = 2 * table + content;
	for (;;) {
		/* The N that gen_create derives from this length. */
		blocks = (gen_length(block_size, blocks) - 16384) / block_size;
		bitmap = gen_tree_blocks(((blocks + 7) / 8 + block_size - 1) /
		    block_size, block_size / 4);
		needed = 2 * (table + bitmap) + content;
		if (needed <= blocks)
			return (gen_length(block_size, blocks));
		blocks = needed;
	}
}

static gen_shape_t
sparse_shape(uint32_t block_size, bool big, uint64_t length, uint32_t inodes,
    uint32_t objects)
{
	return ((gen_shape_t){
		.length = length,
		.block_size = block_size,
		.inodes = inodes,
		.objects = objects,
		.flags = 0x100,
		.big = big,
		.sparse = true
	});
}

/* Open the volume, find path and read length bytes at offset from it. */
static void
read_file_at(qnx6_gen_t *g, const char *path, uint64_t offset,
    unsigned char *bytes, size_t length)
{
	fixture_t f;
	const fsimg_entry_t *entry;
	size_t count;
	fsimg_status_t status;

	fixture_gen(&f, g, 0);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	while ((status = fsimg_reader_next(f.reader, &entry, NULL)) == FSIMG_OK)
		if (strcmp(entry->path, path) == 0)
			break;
	CHECK(status == FSIMG_OK);
	CHECK(fsimg_reader_read_at(f.reader, offset, bytes, length, &count,
	    NULL) == FSIMG_OK && count == length);
	fixture_free(&f);
}

/* System trees at their deeper shapes. A bitmap of more than 16 * P leaves
 * has depth 2, which at B = 512 takes N > 16 * 128 * 4096 blocks: a sparse
 * 4 GiB volume whose block N - 1 holds a file, so that its bit lies in the
 * last bitmap leaf, below the seventeenth indirect block. Inode tables of
 * depth 1 and 2 span several top-level indirect blocks, with a file at the
 * first inode of each later subtree and at the table's last inode.
 */
static void
test_system_trees(void)
{
	static const struct {
		uint32_t	 block_size;
		uint32_t	 inodes;
		uint8_t		 depth;
		unsigned	 roots;		/* top-level blocks used */
	} tables[] = {
		{ 512, 2048, 1, 4 },
		{ 4096, 32768 + 32, 1, 2 },
		{ 512, 2 * 65536 + 4096, 2, 3 }
	};
	qnx6_gen_t g;
	gen_shape_t shape;
	unsigned char byte[1];
	uint64_t span;
	uint32_t next, first, block_size;
	char name[16];

	for (unsigned big = 0; big < 2; big++) {
		shape = sparse_shape(512, big != 0, gen_length(512,
		    16 * 128 * 4096 + 1), GEN_INODES, GEN_INODES);
		if (!gen_create(&g, &shape))
			continue;
		next = g.next_block;
		g.next_block = g.blocks - 1;
		gen_file(&g, 1, "last", (const unsigned char *)"N-1", 3);
		g.next_block = next;
		gen_commit(&g, 0, 1, true, true);
		CHECK(g.bitmap_depth == 2 && g.objects[3].blocks[0] == g.blocks - 1);
		expect(&g, RUN_LIST | RUN_VERIFY, FSIMG_OK, "bitmap tree depth 2");
		read_file_at(&g, "/last", 0, byte, 1);
		CHECK(byte[0] == 'N');
		*bitmap_byte(&g, g.blocks - 1) &= (unsigned char)~(1u <<
		    ((g.blocks - 1) % 8));
		header_put32(&g, 64, get32(&g, g.image + 8192 + 64) + 1);
		expect(&g, RUN_LIST, FSIMG_ECORRUPT, "free bit in the last bitmap leaf");
		gen_free(&g);
	}

	for (size_t t = 0; t < sizeof(tables) / sizeof(tables[0]); t++) {
		for (unsigned big = 0; big < 2; big++) {
			/* One root slot covers B / 128 inodes per leaf times
			 * P^L leaves. The content is the root and .boot
			 * directories and one block per file.
			 */
			block_size = tables[t].block_size;
			span = block_size / 128;
			for (unsigned l = 0; l < tables[t].depth; l++)
				span *= block_size / 4;
			shape = sparse_shape(block_size, big != 0, gen_fit(block_size,
			    tables[t].inodes, 2 + tables[t].roots), tables[t].inodes,
			    tables[t].inodes);
			if (!gen_create(&g, &shape))
				continue;
			for (unsigned k = 1; k < tables[t].roots; k++) {
				first = (uint32_t)(k * span + 1);
				g.inode_cursor = first;
				snprintf(name, sizeof(name), "s%u", k);
				CHECK(gen_file(&g, 1, name, (const unsigned char *)name,
				    strlen(name)) == first);
			}
			g.inode_cursor = tables[t].inodes;
			gen_file(&g, 1, "last", (const unsigned char *)"z", 1);
			gen_commit(&g, 0, 1, true, true);
			CHECK(g.next_block <= g.blocks);
			CHECK(g.inode_depth == tables[t].depth);
			CHECK(g.inode_refs[tables[t].roots - 1] != GEN_SENTINEL &&
			    g.inode_refs[tables[t].roots] == GEN_SENTINEL);
			expect(&g, RUN_LIST | RUN_VERIFY, FSIMG_OK,
			    "inode table across top-level blocks");
			read_file_at(&g, "/last", 0, byte, 1);
			CHECK(byte[0] == 'z');
			/* Without its last top-level block the table is short. */
			header_put32(&g, 80 + (tables[t].roots - 1) * 4, GEN_SENTINEL);
			expect(&g, RUN_LIST, FSIMG_ECORRUPT,
			    "inode table missing a top-level block");
			gen_free(&g);
		}
	}
}

/* A volume with no free block and no free inode at every block size and in
 * both byte orders: files fill every content block, FIFOs every inode.
 */
static void
check_full(uint32_t block_size, bool big)
{
	qnx6_gen_t g;
	fixture_t f;
	fsimg_volume_info_t info;
	fsimg_verify_report_t report;
	unsigned char *bytes;
	uint64_t remaining, leaves;
	uint32_t live;
	char name[16];

	gen_init(&g, block_size, big, 1048576, GEN_INODES, 0x100);
	/* Every inode but the root is named in the root, after its two dot
	 * slots; .boot has one block.
	 */
	remaining = g.blocks - g.next_block - 1 -
	    ((2 + GEN_INODES - 1) * 32 + block_size - 1) / block_size;
	live = 2;
	while (remaining != 0) {
		leaves = remaining;
		while (gen_tree_blocks(leaves, g.pointers) > remaining)
			leaves--;
		bytes = pattern(leaves * block_size, live);
		snprintf(name, sizeof(name), "f%u", live);
		gen_file(&g, 1, name, bytes, leaves * block_size);
		free(bytes);
		remaining -= gen_tree_blocks(leaves, g.pointers);
		live++;
	}
	for (; live < GEN_INODES; live++) {
		snprintf(name, sizeof(name), "p%u", live);
		gen_fifo(&g, 1, name);
	}
	gen_commit(&g, 0, 1, true, true);
	CHECK(g.next_block == g.blocks);
	CHECK(get32(&g, g.image + 8192 + 64) == 0 &&
	    get32(&g, g.image + 8192 + 56) == 0);
	expect(&g, RUN_LIST, FSIMG_OK, "full volume listed");
	fixture_gen(&f, &g, 0);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD, &report, NULL) ==
	    FSIMG_OK && report.complete);
	CHECK(report.free_clusters == 0 && report.allocated_clusters == g.blocks);
	CHECK(report.files == GEN_INODES - 2);
	CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
	CHECK((info.valid & FSIMG_VOLUME_FREE) != 0 && info.free_bytes == 0);
	fixture_free(&f);
	gen_free(&g);
}

static void
test_full(void)
{
	for (uint32_t block_size = 512; block_size <= 4096; block_size *= 2) {
		check_full(block_size, false);
		check_full(block_size, true);
	}
}

/* A chain of directories as deep as the context's depth limit lists and
 * verifies; one level more is refused as a limit by both. (Extraction
 * holds a descriptor per open directory level, so its own bound is the
 * context's open-file limit.)
 */
static void
test_deep_paths(void)
{
	qnx6_gen_t g;
	gen_shape_t shape = {
		.length = 1048576,
		.block_size = 512,
		.inodes = 512,
		.objects = 512,
		.flags = 0x100
	};
	fsimg_options_t defaults;
	uint32_t dir;
	unsigned levels;

	fsimg_options_init(&defaults);
	levels = defaults.max_depth;
	for (unsigned extra = 0; extra < 2; extra++) {
		CHECK(gen_create(&g, &shape));
		dir = 1;
		for (unsigned i = 0; i < levels + extra; i++)
			dir = gen_dir(&g, dir, "d");
		gen_commit(&g, 0, 1, true, true);
		if (extra == 0) {
			expect(&g, RUN_LIST | RUN_VERIFY, FSIMG_OK,
			    "chain at the depth limit");
		} else {
			expect(&g, RUN_LIST, FSIMG_ELIMIT,
			    "chain past the depth limit listed");
			expect(&g, RUN_VERIFY, FSIMG_ELIMIT,
			    "chain past the depth limit verified");
		}
		gen_free(&g);
	}
}

/*
 * Short and failing I/O through the bounded I/O layer.
 */

/* Short reads: the callback returns at most a few bytes per call, and the
 * bounded I/O layer completes every exact read from the pieces.
 */
static void
test_short_reads(void)
{
	static const size_t caps[] = { 1, 3, 512, 4095 };
	qnx6_gen_t g;
	sample_t s;
	fixture_t f;
	fsimg_verify_report_t report;
	uint64_t entries;

	for (unsigned big = 0; big < 2; big++) {
		sample_build(&g, &s, big ? 4096 : 1024, big != 0, 0x100,
		    2097152 + 3584);
		for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
			fixture_gen(&f, &g, 0);
			f.short_max = caps[i];
			CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader,
			    NULL) == FSIMG_OK);
			CHECK(walk_all(&f, &entries, NULL) == FSIMG_OK &&
			    entries == 17);
			fsimg_reader_free(f.reader);
			CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader,
			    NULL) == FSIMG_OK);
			CHECK(fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD, &report,
			    NULL) == FSIMG_OK && report.complete);
			CHECK(report.files == 14 && report.directories == 4);
			fixture_free(&f);
		}
		gen_free(&g);
	}
}

/* When the source ends: at open, or after a successful open, during a
 * listing that reads every file or a verification with payload.
 */
typedef enum eof_stage {
	EOF_OPEN,
	EOF_LIST,
	EOF_VERIFY
} eof_stage_t;

/* A source that makes no progress from `end`: to its end when span is 0,
 * otherwise for span bytes, which isolates one structure from the ones
 * after it. The read that reaches `end` fails with an I/O error at exactly
 * that offset, and everything is released.
 *
 * The offset is exact because image_read cuts a read that straddles `end`
 * there, and the bounded I/O layer's exact-read loop then asks for the rest
 * from `end` and reports the zero progress at that offset. So the case must
 * make the first read reaching [end, end + span) one that starts before
 * `end`: span covers the rest of the structure's block, and the volume's
 * layout puts no structure read earlier inside that range (see test_eof).
 */
static void
check_eof(qnx6_gen_t *g, uint64_t end, uint64_t span, eof_stage_t stage,
    size_t short_max, const char *label)
{
	fixture_t f;
	fsimg_verify_report_t report;
	fsimg_error_t error;
	uint64_t entries;
	fsimg_status_t status;

	fixture_gen(&f, g, 0);
	f.short_max = short_max;
	f.eof_end = span == 0 ? UINT64_MAX : end + span;
	if (stage == EOF_OPEN)
		f.eof_at = end;
	bzero(&error, sizeof(error));
	status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, &error);
	if (stage != EOF_OPEN) {
		CHECK(status == FSIMG_OK);
		f.eof_at = end;
		if (stage == EOF_LIST)
			status = walk_all(&f, &entries, &error);
		else
			status = fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD,
			    &report, &error);
	}
	if (status != FSIMG_EIO || error.offset != end)
		fprintf(stderr, "%s: expected EIO at %" PRIu64 ", got %d at %"
		    PRIu64 " (%s)\n", label, end, status, error.offset,
		    error.message);
	CHECK(status == FSIMG_EIO && error.status == FSIMG_EIO);
	CHECK(error.offset_valid && error.offset == end);
	CHECK(strstr(error.message, "EOF") != NULL);
	fixture_free(&f);
}

static void
test_eof(void)
{
	qnx6_gen_t g;
	sample_t s;
	fixture_t f;
	fsimg_status_t status, expected;
	uint64_t entries, floor, length, bitmap, inode, root, payload, table;
	char path[1100];
	int fd, input;

	/* Every slot the listing replays names a higher inode in a higher
	 * block than the one before, so the first read to reach a structure
	 * is the one that straddles its start. direct16 has no indirect block,
	 * and the last inode-table leaf holds only free inodes: listing never
	 * reads it, verification's table pass does.
	 */
	sample(&g, &s);
	bitmap = GEN_DATA + (uint64_t)g.bitmap_refs[0] * g.block_size;
	inode = GEN_DATA + (uint64_t)g.inode_leaf0 * g.block_size +
	    (uint64_t)(s.indirect - 1) * 128;
	root = GEN_DATA + (uint64_t)g.objects[1].blocks[0] * g.block_size;
	payload = GEN_DATA + (uint64_t)g.objects[s.direct].blocks[3] *
	    g.block_size;
	table = GEN_DATA + (uint64_t)(g.inode_leaf0 + 7) * g.block_size;
	CHECK(g.bitmap_depth == 0 && g.inode_depth == 0 && s.indirect <= 8 &&
	    g.inode_count * 128 == 8 * g.block_size &&
	    g.objects[s.direct].depth == 0);
	check_eof(&g, 0, 0, EOF_OPEN, 0, "EOF at 0");
	check_eof(&g, 8192 + 1, 0, EOF_OPEN, 0, "EOF in the first header");
	check_eof(&g, g.second + 511, 0, EOF_OPEN, 0, "EOF in the second header");
	check_eof(&g, bitmap + 1, g.block_size, EOF_OPEN, 0, "EOF in the bitmap");
	check_eof(&g, inode + 64, g.block_size, EOF_LIST, 0, "EOF in an inode");
	check_eof(&g, root + 100, g.block_size, EOF_LIST, 0,
	    "EOF in the root directory");
	check_eof(&g, payload + 1, g.block_size, EOF_LIST, 0,
	    "EOF in file content");
	check_eof(&g, payload + 1, g.block_size, EOF_LIST, 3,
	    "EOF after short reads");
	check_eof(&g, payload + 1, g.block_size, EOF_VERIFY, 0,
	    "EOF in verified content");
	check_eof(&g, table + 1, g.block_size, EOF_VERIFY, 0,
	    "EOF in the inode-table pass");

	/* A slice cut short has its second header where the cut puts it,
	 * which holds no header unless the cut leaves the last 4096-byte unit
	 * whole; with less than the first header there is no classic magic.
	 * The cuts fall at each 4096-byte boundary and halfway between, and
	 * reads never pass the slice's end.
	 */
	floor = g.length / 4096 * 4096;
	fixture_gen(&f, &g, 0);
	for (length = 0; length < g.length; length += 2048) {
		expected = length >= floor ? FSIMG_OK : length < 8192 + 512 ?
		    FSIMG_EFORMAT : FSIMG_ECORRUPT;
		fixture_resize(&f, length);
		status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL);
		if (status == FSIMG_OK)
			status = walk_all(&f, &entries, NULL);
		fsimg_reader_free(f.reader);
		f.reader = NULL;
		if (status != expected)
			fprintf(stderr, "truncated to %" PRIu64 ": %d\n", length,
			    status);
		CHECK(status == expected);
	}
	fixture_free(&f);

	/* An image file that shrinks after open is a changed source. */
	fixture_gen(&f, &g, 0);
	CHECK((size_t)snprintf(path, sizeof(path), "%s/image", f.directory) <
	    sizeof(path));
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	CHECK(fd != -1 && write(fd, g.image, g.length) == (ssize_t)g.length);
	input = open(path, O_RDONLY | O_CLOEXEC);
	CHECK(input != -1);
	fsimg_io_free(f.io);
	CHECK(fsimg_io_from_fd(f.ctx, input, FSIMG_IO_READ, &f.io, NULL) ==
	    FSIMG_OK);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(ftruncate(fd, (off_t)(g.length / 2)) == 0);
	CHECK(walk_all(&f, &entries, NULL) == FSIMG_ESOURCE_CHANGED);
	fsimg_reader_free(f.reader);
	fsimg_io_free(f.io);
	f.reader = NULL;
	f.io = NULL;
	CHECK(close(input) == 0 && close(fd) == 0 && unlink(path) == 0);
	fixture_free(&f);
	gen_free(&g);
}

/* Scratch failures in a listing and in a verification whose context keeps
 * a single cache page, so that every store spills. Each kind of metadata
 * operation fails at its first 64 occurrences one by one, then at a spread
 * of later ones, until the operation has fewer and succeeds; every failure
 * is an I/O error carrying the injected errno.
 */
static void
test_scratch_faults(void)
{
	static const unsigned operations[] = { RUN_LIST, RUN_VERIFY };
	qnx6_gen_t g;
	sample_t s;
	fixture_t f;
	fsimg_verify_report_t report;
	fsimg_error_t error;
	fsimg_status_t status;
	uint64_t n, entries;
	unsigned what;

	sample(&g, &s);
	for (size_t o = 0; o < sizeof(operations) / sizeof(operations[0]);
	    o++) {
		what = operations[o];
		for (unsigned point = FSIMG_META_OPEN; point <= FSIMG_META_WRITE;
		    point++) {
			for (n = 1;; n += n < 64 ? 1 : n / 8) {
				fixture_gen(&f, &g, 0);
				fixture_cache(&f, 1);
				fsimg_test_meta_fail_after(f.ctx,
				    (fsimg_meta_fault_t)point, n, ENOSPC);
				status = fsimg_reader_open(f.ctx, f.io, NULL,
				    &f.reader, &error);
				if (status == FSIMG_OK && what == RUN_LIST)
					status = walk_all(&f, &entries, &error);
				else if (status == FSIMG_OK)
					status = fsimg_verify(f.reader,
					    FSIMG_VERIFY_PAYLOAD, &report, &error);
				fsimg_test_meta_fail_after(f.ctx,
				    (fsimg_meta_fault_t)point, 0, 0);
				fixture_free(&f);
				if (status == FSIMG_OK)
					break;
				CHECK(status == FSIMG_EIO &&
				    error.os_error == ENOSPC);
				CHECK(n < 100000);
			}
			CHECK(n > 1);
		}
	}
	gen_free(&g);
}

/*
 * Cancellation during a traversal.
 */

#define CANCEL_LIST	0u
#define CANCEL_VERIFY	1u
#define CANCEL_EXTRACT	2u

/* Remove a partial extraction below parent; name may be absent. Each pass
 * removes the first entry it finds and rewinds, so no entry is skipped
 * while the directory changes under readdir.
 */
static void
remove_tree(int parent, const char *name)
{
	struct dirent *child;
	struct stat sb;
	DIR *dir;
	int fd;

	fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
	    O_CLOEXEC);
	if (fd == -1) {
		CHECK(errno == ENOENT || errno == ENOTDIR || errno == ELOOP ||
		    errno == EMLINK);
		CHECK(unlinkat(parent, name, 0) == 0 || errno == ENOENT);
		return;
	}
	dir = fdopendir(fd);
	CHECK(dir != NULL);
	while ((child = readdir(dir)) != NULL) {
		if (strcmp(child->d_name, ".") == 0 ||
		    strcmp(child->d_name, "..") == 0)
			continue;
		CHECK(fstatat(fd, child->d_name, &sb, AT_SYMLINK_NOFOLLOW) == 0);
		if (S_ISDIR(sb.st_mode))
			remove_tree(fd, child->d_name);
		else
			CHECK(unlinkat(fd, child->d_name, 0) == 0);
		rewinddir(dir);
	}
	CHECK(closedir(dir) == 0);
	CHECK(unlinkat(parent, name, AT_REMOVEDIR) == 0);
}

/*
 * Names outside the profile. The omitted sample holds, beside delivered
 * siblings, six omitted subtree roots: a file named with a control byte in
 * the delivered directory /mixed, a Latin-1 file, a symbolic link
 * named with a control byte, a FIFO named with DEL, a 40-byte Latin-1 long
 * name and a directory with a subtree of its own. Its plain twin holds
 * only the delivered entries, laid out first and in the same order, so it
 * is the oracle for what a listing, an extraction and a verification of
 * the omitted sample must deliver.
 */
typedef struct omitted_sample {
	uint32_t	 kept;		/* "/kept", a delivered directory */
	uint32_t	 long_name;	/* 39 'l' bytes and 0xe9 */
	uint32_t	 directory;	/* "/dir\xff" */
	uint32_t	 deep;		/* "/dir\xff/sub/deep" */
} omitted_sample_t;

#define OMITTED_ROOTS		6u
/* Bytes a payload read-through reads beyond the plain twin's: one byte in
 * each of five omitted files and the link's 4-byte target.
 */
#define OMITTED_PAYLOAD_BYTES	9u

static void
omitted_sample_build(qnx6_gen_t *g, omitted_sample_t *s, bool omitted)
{
	char name[41];
	uint32_t mixed, sub;

	gen_init(g, 1024, false, 1048576, GEN_INODES, 0x100);
	gen_file(g, 1, "keep", (const unsigned char *)"k", 1);
	s->kept = gen_dir(g, 1, "kept");
	gen_file(g, s->kept, "inner", (const unsigned char *)"i", 1);
	mixed = gen_dir(g, 1, "mixed");
	gen_file(g, mixed, "ok", (const unsigned char *)"o", 1);
	if (omitted)
		gen_file(g, mixed, "bad\x02", (const unsigned char *)"b", 1);
	if (omitted) {
		gen_file(g, 1, "caf\xe9", (const unsigned char *)"c", 1);
		gen_symlink(g, 1, "ctl\x01", "keep");
		gen_fifo(g, 1, "del\x7f");
		memset(name, 'l', 39);
		name[39] = (char)0xe9;
		name[40] = '\0';
		s->long_name = gen_file(g, 1, name,
		    (const unsigned char *)"n", 1);
		s->directory = gen_dir(g, 1, "dir\xff");
		gen_file(g, s->directory, "child", (const unsigned char *)"x",
		    1);
		sub = gen_dir(g, s->directory, "sub");
		s->deep = gen_file(g, sub, "deep", (const unsigned char *)"y",
		    1);
	}
	gen_commit(g, 0, 1, true, true);
}

/* The context statistics after an operation: omitted subtree roots, and
 * never an entry skipped for its kind.
 */
static void
check_omission_counts(fixture_t *f, uint64_t omitted)
{
	fsimg_stats_t stats;

	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	if (stats.omitted_entries != omitted)
		fprintf(stderr, "omitted entries: expected %" PRIu64
		    ", got %" PRIu64 "\n", omitted, stats.omitted_entries);
	CHECK(stats.omitted_entries == omitted);
	CHECK(stats.skipped_entries == 0);
}

/* Extract selection from the fixture's image into a fresh directory "out"
 * beneath its scratch directory through the extractor's internal entry
 * point, which leaves the error naming the failing entry rather than the
 * destination. The caller removes "out".
 */
static fsimg_status_t
extract_omitted(fixture_t *f, const char *selection, bool skip,
    fsimg_extract_result_t *result, fsimg_error_t *error)
{
	fsimg_extract_options_t options;
	fsimg_status_t status;
	int root;

	CHECK(fsimg_reader_open(f->ctx, f->io, NULL, &f->reader, NULL) ==
	    FSIMG_OK);
	fsimg_extract_options_init(&options);
	options.selection = selection;
	if (skip)
		options.flags |= FSIMG_EXTRACT_SKIP_UNSUPPORTED;
	CHECK(mkdirat(f->scratch, "out", 0700) == 0);
	root = openat(f->scratch, "out", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(root != -1);
	status = fsimg_extract_into(f->reader, root, &options, result, error);
	CHECK(close(root) == 0);
	fsimg_reader_free(f->reader);
	f->reader = NULL;
	return (status);
}

/* The number of names in directory path beneath the fixture's scratch. */
static unsigned
directory_names(fixture_t *f, const char *path)
{
	struct dirent *entry;
	unsigned names;
	DIR *dir;
	int fd;

	fd = openat(f->scratch, path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fd != -1);
	dir = fdopendir(fd);
	CHECK(dir != NULL);
	names = 0;
	while ((entry = readdir(dir)) != NULL)
		if (strcmp(entry->d_name, ".") != 0 &&
		    strcmp(entry->d_name, "..") != 0)
			names++;
	CHECK(closedir(dir) == 0);
	return (names);
}

/* The extracted file at path beneath the fixture's scratch holds byte. */
static void
check_extracted(fixture_t *f, const char *path, char byte)
{
	char buffer[2];
	ssize_t got;
	int fd;

	fd = openat(f->scratch, path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	CHECK(fd != -1);
	got = read(fd, buffer, sizeof(buffer));
	CHECK(got == 1 && buffer[0] == byte);
	CHECK(close(fd) == 0);
}

static void
test_omitted_names(void)
{
	qnx6_gen_t g, plain;
	omitted_sample_t s, p;
	fixture_t f;
	fsimg_verify_report_t report, plain_report;
	fsimg_extract_options_t options;
	fsimg_extract_result_t result;
	fsimg_error_t error;
	uint64_t entries, plain_entries;
	unsigned plain_names;
	unsigned char *inode;
	uint32_t inner;

	omitted_sample_build(&g, &s, true);
	omitted_sample_build(&plain, &p, false);

	/* A listing delivers exactly the plain twin's entries and counts
	 * each omitted subtree root once, never its descendants.
	 */
	fixture_gen(&f, &plain, 0);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) ==
	    FSIMG_OK);
	CHECK(walk_all(&f, &plain_entries, NULL) == FSIMG_OK);
	check_omission_counts(&f, 0);
	fixture_free(&f);
	fixture_gen(&f, &g, 0);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) ==
	    FSIMG_OK);
	CHECK(walk_all(&f, &entries, NULL) == FSIMG_OK);
	CHECK(entries == plain_entries);
	check_omission_counts(&f, OMITTED_ROOTS);
	fixture_free(&f);

	/* Verification walks the omitted subtrees: their blocks are owned
	 * and their inodes referenced, so it completes. Files, directories
	 * and logical bytes count delivered entries; payload bytes cover all.
	 */
	fixture_gen(&f, &plain, 0);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) ==
	    FSIMG_OK);
	CHECK(fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD, &plain_report,
	    NULL) == FSIMG_OK);
	fixture_free(&f);
	fixture_gen(&f, &g, 0);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) ==
	    FSIMG_OK);
	CHECK(fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD, &report, NULL) ==
	    FSIMG_OK);
	CHECK(report.complete);
	CHECK(report.files == plain_report.files);
	CHECK(report.directories == plain_report.directories);
	CHECK(report.logical_bytes == plain_report.logical_bytes);
	CHECK(report.payload_bytes_read == plain_report.payload_bytes_read +
	    OMITTED_PAYLOAD_BYTES);
	CHECK(report.allocated_clusters > plain_report.allocated_clusters);
	CHECK(report.omitted_entries == OMITTED_ROOTS);
	CHECK(plain_report.omitted_entries == 0);
	check_omission_counts(&f, OMITTED_ROOTS);
	fixture_free(&f);

	/* Extracting a scope that holds an omitted entry fails naming the
	 * first one in traversal order, rendered, and no name byte enters
	 * the message; -k omits all six and extracts what the plain twin
	 * does.
	 */
	fixture_gen(&f, &plain, 0);
	CHECK(extract_omitted(&f, "/", false, &result, NULL) == FSIMG_OK);
	plain_names = directory_names(&f, "out");
	remove_tree(f.scratch, "out");
	fixture_free(&f);
	fixture_gen(&f, &g, 0);
	bzero(&error, sizeof(error));
	CHECK(extract_omitted(&f, "/", false, &result, &error) ==
	    FSIMG_EUNSUPPORTED);
	CHECK(strcmp(error.path, "/mixed/bad\\x02") == 0 &&
	    !error.path_truncated);
	CHECK(strchr(error.message, '\x02') == NULL);
	CHECK(result.omitted_entries == 1);
	remove_tree(f.scratch, "out");
	CHECK(extract_omitted(&f, "/", true, &result, NULL) == FSIMG_OK);
	CHECK(result.omitted_entries == OMITTED_ROOTS);
	CHECK(result.skipped_entries == 0);
	check_extracted(&f, "out/keep", 'k');
	check_extracted(&f, "out/kept/inner", 'i');
	CHECK(directory_names(&f, "out") == plain_names);
	CHECK(directory_names(&f, "out/kept") == 1);
	CHECK(directory_names(&f, "out/mixed") == 1);
	remove_tree(f.scratch, "out");
	fixture_free(&f);

	/* An omitted entry below a delivered directory is in that
	 * directory's scope. The public entry point keeps the entry's path
	 * in the error rather than naming the destination.
	 */
	fixture_gen(&f, &g, 0);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) ==
	    FSIMG_OK);
	fsimg_extract_options_init(&options);
	options.selection = "/mixed";
	bzero(&error, sizeof(error));
	CHECK(fsimg_extract_at(f.reader, f.scratch, "out", &options, &error) ==
	    FSIMG_EUNSUPPORTED);
	CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
	CHECK(strcmp(error.path, "/mixed/bad\\x02") == 0);
	fsimg_reader_free(f.reader);
	f.reader = NULL;
	remove_tree(f.scratch, "out");
	CHECK(extract_omitted(&f, "/mixed", true, &result, NULL) == FSIMG_OK);
	CHECK(result.omitted_entries == 1);
	CHECK(directory_names(&f, "out/mixed") == 1);
	remove_tree(f.scratch, "out");
	fixture_free(&f);

	/* A sibling's scope holds no omitted entry: it extracts without -k
	 * and counts nothing, though the walker passes the omitted roots.
	 */
	fixture_gen(&f, &g, 0);
	CHECK(extract_omitted(&f, "/kept", false, &result, NULL) == FSIMG_OK);
	CHECK(result.omitted_entries == 0);
	check_omission_counts(&f, 0);
	check_extracted(&f, "out/kept/inner", 'i');
	remove_tree(f.scratch, "out");
	fixture_free(&f);

	/* No query can spell an omitted name: invalid UTF-8 fails path
	 * validation and a control byte fails the driver's key.
	 */
	fixture_gen(&f, &g, 0);
	CHECK(extract_omitted(&f, "/caf\xe9", true, &result, NULL) ==
	    FSIMG_EINVAL);
	remove_tree(f.scratch, "out");
	CHECK(extract_omitted(&f, "/ctl\x01", true, &result, NULL) ==
	    FSIMG_EINVAL);
	remove_tree(f.scratch, "out");
	fixture_free(&f);

	/* Verification still finds what is wrong below an omitted name: a
	 * block pointer past the volume, and link counts disagreeing with
	 * the names of an omitted file and an omitted directory. Listing
	 * never opens the subtree or reads a link count.
	 */
	inode = gen_inode_bytes(&g, s.deep);
	put32(&g, inode + 36, g.blocks);
	expect(&g, RUN_LIST, FSIMG_OK, "a bad pointer below an omitted directory");
	expect(&g, RUN_VERIFY, FSIMG_ECORRUPT,
	    "a bad pointer below an omitted directory");
	put32(&g, inode + 36, g.objects[s.deep].blocks[0]);
	put16(&g, gen_inode_bytes(&g, s.long_name) + 34,
	    (uint16_t)(g.objects[s.long_name].links + 1));
	expect(&g, RUN_LIST, FSIMG_OK, "an omitted file's link count");
	expect(&g, RUN_VERIFY, FSIMG_ECORRUPT, "an omitted file's link count");
	put16(&g, gen_inode_bytes(&g, s.long_name) + 34,
	    g.objects[s.long_name].links);
	put16(&g, gen_inode_bytes(&g, s.directory) + 34,
	    (uint16_t)(g.objects[s.directory].links + 1));
	expect(&g, RUN_LIST, FSIMG_OK, "an omitted directory's link count");
	expect(&g, RUN_VERIFY, FSIMG_ECORRUPT,
	    "an omitted directory's link count");
	put16(&g, gen_inode_bytes(&g, s.directory) + 34,
	    g.objects[s.directory].links);
	expect(&g, RUN_LIST | RUN_VERIFY, FSIMG_OK, "the omitted sample restored");
	gen_free(&g);
	gen_free(&plain);

	/* An omitted name is still a key: a duplicate is corruption. */
	gen_init(&g, 1024, false, 1048576, GEN_INODES, 0x100);
	gen_file(&g, 1, "caf\xe9", (const unsigned char *)"a", 1);
	gen_file(&g, 1, "caf\xe9", (const unsigned char *)"b", 1);
	gen_commit(&g, 0, 1, true, true);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "a duplicate omitted name");
	bzero(&error, sizeof(error));
	CHECK(run_image(g.image, g.image_fd, g.length, RUN_LIST, &error) ==
	    FSIMG_ECORRUPT);
	CHECK(strcmp(error.path, "/caf\\xe9") == 0);
	CHECK(strstr(error.message, "collides with entry") != NULL &&
	    strchr(error.message, '\xe9') == NULL);
	gen_free(&g);

	/* Below an omitted directory only verification prepares the
	 * directory, and its collision names the path with the omitted
	 * component rendered.
	 */
	gen_init(&g, 1024, false, 1048576, GEN_INODES, 0x100);
	inner = gen_dir(&g, 1, "d\xff");
	gen_file(&g, inner, "x", (const unsigned char *)"a", 1);
	gen_file(&g, inner, "x", (const unsigned char *)"b", 1);
	gen_commit(&g, 0, 1, true, true);
	expect(&g, RUN_LIST, FSIMG_OK, "a collision below an omitted directory");
	bzero(&error, sizeof(error));
	CHECK(run_image(g.image, g.image_fd, g.length, RUN_VERIFY, &error) ==
	    FSIMG_ECORRUPT);
	CHECK(strcmp(error.path, "/d\\xff/x") == 0);
	gen_free(&g);
}

/* Run one operation, cancelled at poll `after` (never when 0), with a
 * metadata cache of `pages` pages when nonzero. *polls receives the polls
 * it made and *opened whether the open succeeded; fixture_free checks that
 * nothing leaked.
 */
static fsimg_status_t
cancel_run(qnx6_gen_t *g, unsigned operation, uint32_t pages, uint64_t after,
    uint64_t *polls, bool *opened, fsimg_error_t *error)
{
	fixture_t f;
	fsimg_extract_options_t options;
	fsimg_verify_report_t report;
	uint64_t entries;
	fsimg_status_t status;

	fixture_gen(&f, g, 0);
	if (pages != 0)
		fixture_cache(&f, pages);
	f.cancel_after = after;
	bzero(error, sizeof(*error));
	status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, error);
	*opened = status == FSIMG_OK;
	if (status == FSIMG_OK) {
		switch (operation) {
		case CANCEL_LIST:
			status = walk_all(&f, &entries, error);
			break;
		case CANCEL_VERIFY:
			status = fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD,
			    &report, error);
			CHECK(report.complete == (status == FSIMG_OK));
			break;
		case CANCEL_EXTRACT:
			fsimg_extract_options_init(&options);
			options.flags |= FSIMG_EXTRACT_SKIP_UNSUPPORTED;
			status = fsimg_extract_at(f.reader, f.scratch, "out",
			    &options, error);
			remove_tree(f.scratch, "out");
			break;
		}
	}
	*polls = f.polls;
	fixture_free(&f);
	return (status);
}

/* Cancellation at a spread of polls through each operation, with the
 * default cache and with metadata spilled to scratch: every run reports
 * the cancellation, an interrupted extraction reports its output
 * incomplete, and nothing outlives the reader.
 */
static void
test_cancellation(void)
{
	static const char *names[] = { "list", "verify", "extract" };
	qnx6_gen_t g;
	fsimg_error_t error;
	unsigned char *bytes;
	uint64_t total, polls;
	uint32_t file, dir;
	bool opened;
	fsimg_status_t status;

	/* The sample's kinds and shapes without its 510-byte name, which no
	 * host can extract.
	 */
	gen_init(&g, 1024, false, 1048576 + 1536, GEN_INODES, 0x100);
	bytes = pattern(17 * 1024 + 1, 4);
	file = gen_file(&g, 1, "direct16", bytes, 16 * 1024);
	gen_file(&g, 1, "indirect17", bytes, 17 * 1024 + 1);
	free(bytes);
	gen_file(&g, 1, "aaaaaaaaaaaaaaaaaaaaaaaaaaab", (const unsigned char *)"28",
	    2);
	dir = gen_dir(&g, gen_dir(&g, 1, "d1"), "d2");
	gen_file(&g, dir, "f", (const unsigned char *)"sub", 3);
	gen_link(&g, 1, "hardlink", file);
	gen_symlink(&g, 1, "symlink", "direct16");
	gen_fifo(&g, 1, "fifo");
	gen_commit(&g, 0, 1, true, true);
	for (uint32_t pages = 0; pages <= 1; pages++) {
		for (unsigned op = CANCEL_LIST; op <= CANCEL_EXTRACT; op++) {
			status = cancel_run(&g, op, pages, 0, &total, &opened,
			    &error);
			if (status != FSIMG_OK)
				fprintf(stderr, "%s: %d (%s)\n", names[op], status,
				    error.message);
			CHECK(status == FSIMG_OK);
			CHECK(total > 1);
			/* Every poll of the first 64, then a step of a
			 * sixteenth: dense through open, spread through the
			 * traversal.
			 */
			for (uint64_t after = 1; after <= total;
			    after += after < 64 ? 1 : after / 16) {
				status = cancel_run(&g, op, pages, after, &polls,
				    &opened, &error);
				if (status != FSIMG_ECANCELLED)
					fprintf(stderr, "%s cancelled at poll %"
					    PRIu64 " of %" PRIu64 ": %d (%s)\n",
					    names[op], after, total, status,
					    error.message);
				CHECK(status == FSIMG_ECANCELLED);
				CHECK(error.status == FSIMG_ECANCELLED &&
				    polls >= after);
				if (op == CANCEL_EXTRACT && opened)
					CHECK(error.output_state ==
					    FSIMG_OUTPUT_INCOMPLETE &&
					    strcmp(error.path, "out") == 0);
			}
		}
	}
	gen_free(&g);
}

/* Safe extraction of a selected subtree, and byte-exact name matching. */
static void
test_extract(void)
{
	qnx6_gen_t g;
	sample_t s;
	fixture_t f;
	fsimg_extract_options_t options;
	fsimg_stats_t stats;
	const fsimg_entry_t *entry;
	fsimg_error_t error;
	char path[128], bytes[8], long_name[512];
	bool matches;
	ssize_t got;
	int fd;

	sample(&g, &s);
	fixture_gen(&f, &g, 0);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	fsimg_extract_options_init(&options);
	options.selection = "/d1";
	CHECK(fsimg_extract_at(f.reader, f.scratch, "out", &options, NULL) ==
	    FSIMG_OK);
	snprintf(path, sizeof(path), "%s/out/d1/d2/f", f.directory);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	CHECK(fd != -1);
	got = read(fd, bytes, sizeof(bytes));
	CHECK(got == 3 && bcmp(bytes, "sub", 3) == 0);
	CHECK(close(fd) == 0);
	CHECK(unlink(path) == 0);
	snprintf(path, sizeof(path), "%s/out/d1/d2", f.directory);
	CHECK(rmdir(path) == 0);
	snprintf(path, sizeof(path), "%s/out/d1", f.directory);
	CHECK(rmdir(path) == 0);
	snprintf(path, sizeof(path), "%s/out", f.directory);
	CHECK(rmdir(path) == 0);
	fsimg_reader_free(f.reader);
	f.reader = NULL;

	/* Without the skip option a symbolic link or FIFO fails extraction;
	 * with it they are counted.
	 */
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	options.selection = "/fifo";
	CHECK(fsimg_extract_at(f.reader, f.scratch, "out", &options, NULL) ==
	    FSIMG_EUNSUPPORTED);
	CHECK(rmdir(path) == 0);
	fsimg_reader_free(f.reader);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	options.flags |= FSIMG_EXTRACT_SKIP_UNSUPPORTED;
	CHECK(fsimg_extract_at(f.reader, f.scratch, "out", &options, NULL) ==
	    FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.skipped_entries == 1);
	CHECK(rmdir(path) == 0);
	fsimg_reader_free(f.reader);

	/* A 510-byte name exceeds every supported host's 255-byte component
	 * limit: the host cannot represent it, which is not an I/O failure.
	 */
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	long_name[0] = '/';
	memset(long_name + 1, 'c', 510);
	long_name[511] = '\0';
	options.selection = long_name;
	CHECK(fsimg_extract_at(f.reader, f.scratch, "out", &options, &error) ==
	    FSIMG_EUNSUPPORTED);
	CHECK(error.os_error == ENAMETOOLONG);
	CHECK(rmdir(path) == 0);
	fsimg_reader_free(f.reader);

	/* Names compare byte for byte, case significant. */
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(strcmp(entry->path, "/empty") == 0);
	CHECK(fsimg_reader_match_name(f.reader, "empty", &matches, NULL) ==
	    FSIMG_OK && matches);
	CHECK(fsimg_reader_match_name(f.reader, "EMPTY", &matches, NULL) ==
	    FSIMG_OK && !matches);
	CHECK(fsimg_reader_match_name(f.reader, "a\x01", &matches, NULL) ==
	    FSIMG_EINVAL);
	fixture_free(&f);
	gen_free(&g);
}

/*
 * Scale: each case grows one dimension of a sparse volume, then lists it
 * (reading every file) and verifies it with payload, each in a fresh
 * context. Ordinary runs use each case's small size; --scale the large
 * one, and prints the measurements. The checks restate the driver design's
 * resource bounds for these volumes:
 *
 * - the listing and verification see every live inode, and verification
 *   completes;
 * - charged memory stays below SCALE_MEMORY at every size, and scratch
 *   within a budget the context enforces (see test_scale);
 * - no owned block is read twice by one operation;
 * - the system run is read in bounded passes: only verification reads
 *   the bitmap and the inode table in full, once each (see scale_system).
 */

typedef struct scale_run {
	uint64_t	 entries;	/* listed, or verified with the root */
	uint64_t	 image_calls;
	uint64_t	 image_bytes;
	uint64_t	 system_bytes;	/* image bytes read from blocks [0, 2R) */
	uint64_t	 content_bytes;	/* and from blocks [2R, N) */
	uint64_t	 repeats;	/* reads of a content block read before */
	uint64_t	 io_calls;	/* image and scratch */
	uint64_t	 memory_peak;
	uint64_t	 scratch_peak;
	double		 seconds;
} scale_run_t;

typedef struct scale_case {
	const char	*name;
	bool		(*build)(qnx6_gen_t *, uint64_t);
	uint64_t	 small;
	uint64_t	 large;
	bool		 marked;	/* /f carries scale_mark bytes */
} scale_case_t;

/* The charged-memory ceiling: a regression bound below the context's
 * 16 MiB budget that no case approaches by growing.
 */
#define SCALE_MEMORY	(UINT64_C(4) << 20)

/* Facts about a committed volume the bounds are stated in. */
typedef struct scale_volume {
	uint64_t	 live;		/* live inodes, the root included */
	uint64_t	 owned;		/* blocks objects own, indirect included */
	uint64_t	 branches;	/* indirect blocks objects own */
	uint64_t	 bitmap_leaves;
	uint64_t	 bitmap_blocks;	/* the bitmap tree, indirect included */
	uint64_t	 table_leaves;
	uint64_t	 system_indirect; /* both system trees' indirect blocks */
} scale_volume_t;

static void
scale_describe(const qnx6_gen_t *g, scale_volume_t *v)
{
	const gen_object_t *o;
	uint64_t table_blocks;

	bzero(v, sizeof(*v));
	v->owned = gen_tree_blocks(g->name_count, g->pointers);
	v->branches = v->owned - g->name_count;
	for (uint32_t i = 1; i <= g->object_count; i++) {
		o = &g->objects[i];
		if (!o->live)
			continue;
		v->live++;
		v->owned += o->block_count;
		v->branches += o->block_count - (o->size + g->block_size - 1) /
		    g->block_size;
	}
	v->bitmap_leaves = (((uint64_t)g->blocks + 7) / 8 + g->block_size - 1) /
	    g->block_size;
	v->bitmap_blocks = gen_tree_blocks(v->bitmap_leaves, g->pointers);
	v->table_leaves = (uint64_t)g->inode_count * 128 / g->block_size;
	table_blocks = gen_tree_blocks(v->table_leaves, g->pointers);
	v->system_indirect = v->bitmap_blocks - v->bitmap_leaves +
	    table_blocks - v->table_leaves;
}

/* The most bytes of the system run an operation may read. The indirect
 * and passes terms restate rows of the driver design's resource table; the
 * inodes and windows terms restate the reader's access pattern on these
 * volumes, as regression ceilings. The windows term depends on the
 * generator's layout: gen_file allocates a file's leaves contiguously and
 * gen_tree its indirect blocks after them, gen_commit lays out directories
 * after every file, and scale_blocks places a file in block N - 1. A layout
 * change that moves blocks apart can exceed it.
 *
 * indirect	open indexes the bitmap and inode-table trees, reading
 *		their indirect blocks once; verification walks them again
 *		to record ownership.
 * passes	verification reads the whole bitmap and the whole inode
 *		table once each.
 * inodes	the reader reads an inode when its name is admitted to the
 *		directory sort and again when the entry is replayed: two
 *		128-byte reads per live inode.
 * windows	allocation checks read the bitmap through one 4 KiB window
 *		covering 32768 blocks, loading a window whenever a checked
 *		block lies outside the current one.
 */
static uint64_t
scale_system(const qnx6_gen_t *g, const scale_volume_t *v, bool verify)
{
	uint64_t indirect, passes, inodes, own, tail, walk, between, windows;

	indirect = v->system_indirect * g->block_size * (verify ? 2 : 1);
	passes = verify ? (v->bitmap_leaves + v->table_leaves) *
	    g->block_size : 0;
	inodes = 2 * 128 * v->live;
	/* Open checks the bits of the bitmap's own blocks, which are
	 * contiguous, so the windows they span, plus one for a span that
	 * straddles a window boundary.
	 */
	own = v->bitmap_blocks / 32768 + 2;
	/* Open checks the bits past N in the last bitmap block. */
	tail = 1;
	/* A walk visits an object's leaves in block order, laid out
	 * contiguously, and an indirect block, laid out after its leaves,
	 * before and after the leaves under it: at most two loads per
	 * indirect block and one per 32768 blocks.
	 */
	walk = 2 * v->branches + v->owned / 32768;
	/* Between objects: the directories, laid out at commit after the
	 * files, the files, and a file a case places in block N - 1.
	 */
	between = 4;
	windows = 4096 * (own + tail + walk + between);
	return (indirect + passes + inodes + windows);
}

static void
scale_measure(qnx6_gen_t *g, bool verify, uint64_t scratch_bytes,
    scale_run_t *run)
{
	fixture_t f;
	fsimg_verify_report_t report;
	fsimg_stats_t stats;
	fsimg_error_t error;
	struct timespec start, end;
	fsimg_status_t status;

	fixture_limited(&f, g->image, g->image_fd, g->length, 0, scratch_bytes);
	fixture_account(&f, g);
	bzero(&error, sizeof(error));
	CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
	status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, &error);
	if (status == FSIMG_OK && verify)
		status = fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD, &report,
		    &error);
	else if (status == FSIMG_OK)
		status = walk_all(&f, &run->entries, &error);
	CHECK(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
	if (status != FSIMG_OK)
		fprintf(stderr, "scale: %d %s\n", status, error.message);
	CHECK(status == FSIMG_OK);
	if (verify) {
		CHECK(report.complete);
		run->entries = report.files + report.directories;
	}
	CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
	run->image_calls = f.calls;
	run->image_bytes = f.read_bytes;
	run->system_bytes = f.account.system_bytes;
	run->content_bytes = f.account.content_bytes;
	run->repeats = f.account.repeats;
	run->io_calls = stats.io_calls;
	run->memory_peak = stats.memory_peak;
	run->scratch_peak = stats.scratch_peak;
	run->seconds = (double)(end.tv_sec - start.tv_sec) +
	    (double)(end.tv_nsec - start.tv_nsec) / 1e9;
	fixture_free(&f);
}

/* N grows: B = 512, two files, one of them in block N - 1. */
static bool
scale_blocks(qnx6_gen_t *g, uint64_t blocks)
{
	gen_shape_t shape = sparse_shape(512, false, gen_length(512, blocks),
	    GEN_INODES, GEN_INODES);
	uint32_t next;

	if (!gen_create(g, &shape))
		return (false);
	gen_file(g, 1, "first", (const unsigned char *)"a", 1);
	next = g->next_block;
	g->next_block = g->blocks - 1;
	gen_file(g, 1, "last", (const unsigned char *)"z", 1);
	g->next_block = next;
	gen_commit(g, 0, 1, true, true);
	return (true);
}

/* The inode capacity grows: B = 4096, big endian, one file. The content
 * is the root and .boot directories and the file's block.
 */
static bool
scale_inodes(qnx6_gen_t *g, uint64_t inodes)
{
	gen_shape_t shape = sparse_shape(4096, true, gen_fit(4096,
	    (uint32_t)inodes, 3), (uint32_t)inodes, GEN_INODES);

	if (!gen_create(g, &shape))
		return (false);
	gen_file(g, 1, "f", (const unsigned char *)"i", 1);
	gen_commit(g, 0, 1, true, true);
	return (true);
}

/* One directory widens: B = 1024, n files in the root, every 64th with one
 * byte of content. The content is the root's tree over its n + 3 slots,
 * the .boot directory and the files' blocks.
 */
static bool
scale_width(qnx6_gen_t *g, uint64_t files)
{
	gen_shape_t shape;
	uint64_t root;
	uint32_t inodes;
	char name[16];

	inodes = (uint32_t)((files + 2 + 7) / 8 * 8);
	root = gen_tree_blocks(((files + 3) * 32 + 1023) / 1024, 256);
	shape = sparse_shape(1024, false, gen_fit(1024, inodes, root + 1 +
	    (files + 63) / 64), inodes, inodes);
	if (!gen_create(g, &shape))
		return (false);
	for (uint64_t i = 0; i < files; i++) {
		snprintf(name, sizeof(name), "f%07" PRIu64, i);
		gen_file(g, 1, name, (const unsigned char *)"w",
		    i % 64 == 0 ? 1 : 0);
	}
	gen_commit(g, 0, 1, true, true);
	return (true);
}

/* The offsets of a marked file that hold the bytes 1 to 4: its first,
 * middle and last byte, and 4 GiB or a third of the way in.
 */
static uint64_t
scale_mark(uint64_t length, unsigned which)
{
	switch (which) {
	case 0:
		return (0);
	case 1:
		return (length / 2);
	case 2:
		return (length > (UINT64_C(1) << 32) ? UINT64_C(1) << 32 :
		    length / 3);
	default:
		return (length - 1);
	}
}

/* A volume whose one file /f has the given length, sparse but for its
 * marks. The content is the root and .boot directories and the file's
 * tree.
 */
static bool
scale_file(qnx6_gen_t *g, uint32_t block_size, bool big, uint64_t length)
{
	gen_shape_t shape;
	uint64_t leaves, offset;
	uint32_t file;

	leaves = (length + block_size - 1) / block_size;
	shape = sparse_shape(block_size, big, gen_fit(block_size, GEN_INODES, 2 +
	    gen_tree_blocks(leaves, block_size / 4)), GEN_INODES, GEN_INODES);
	if (!gen_create(g, &shape))
		return (false);
	file = gen_file(g, 1, "f", NULL, length);
	for (unsigned m = 0; m < 4; m++) {
		offset = scale_mark(length, m);
		*(block_at(g, g->objects[file].blocks[offset / block_size]) +
		    offset % block_size) = (unsigned char)(m + 1);
	}
	gen_commit(g, 0, 1, true, true);
	return (true);
}

/* A file's tree deepens: B = 512, big endian, n leaves. */
static bool
scale_depth(qnx6_gen_t *g, uint64_t leaves)
{
	return (scale_file(g, 512, true, leaves * 512 - 7));
}

/* A file passes 4 GiB: B = 4096, little endian. */
static bool
scale_large(qnx6_gen_t *g, uint64_t length)
{
	return (scale_file(g, 4096, false, length));
}

static void
scale_print(const char *name, uint64_t size, const char *what,
    const scale_run_t *run)
{
	printf("qnx6 scale %s=%" PRIu64 " %s entries=%" PRIu64
	    " image_calls=%" PRIu64 " image_read=%" PRIu64 " system_read=%"
	    PRIu64 " content_read=%" PRIu64 " io_calls=%" PRIu64
	    " memory_peak=%" PRIu64 " scratch_peak=%" PRIu64 " seconds=%.3f\n",
	    name, size, what, run->entries, run->image_calls, run->image_bytes,
	    run->system_bytes, run->content_bytes, run->io_calls,
	    run->memory_peak, run->scratch_peak, run->seconds);
}

static void
test_scale(bool large)
{
	static const scale_case_t cases[] = {
		{ "blocks", scale_blocks, 80000, UINT64_C(64) << 20, false },
		{ "inodes", scale_inodes, 4096, 1048576, false },
		{ "width", scale_width, 2000, 200000, false },
		{ "depth", scale_depth, 2100, 16 * 128 * 128 + 1, true },
		{ "large", scale_large, (UINT64_C(16) << 20) + 1,
		    (UINT64_C(4) << 30) + 4097, true }
	};
	qnx6_gen_t g;
	scale_volume_t v;
	scale_run_t list, verify;
	unsigned char byte[1];
	uint64_t size, scratch;

	for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		size = large ? cases[c].large : cases[c].small;
		if (!cases[c].build(&g, size))
			continue;
		scale_describe(&g, &v);
		/* The scratch budget: the bitsets of claimed blocks (N bits)
		 * and exposed inodes (one bit per inode), at most one 16-byte
		 * ownership run per owned block, and 128 bytes per live inode
		 * for its directory key record, the record's sorted copy and
		 * its 4-byte slot reference, measured at about 110 bytes for
		 * 8-byte names; 1 MiB covers store headers and partly filled
		 * pages. The context fails any operation that needs more.
		 */
		scratch = g.blocks / 8 + g.inode_count / 8 + 16 * v.owned +
		    128 * v.live + (UINT64_C(1) << 20);
		scale_measure(&g, false, scratch, &list);
		scale_measure(&g, true, scratch, &verify);
		if (large) {
			scale_print(cases[c].name, size, "list", &list);
			scale_print(cases[c].name, size, "verify", &verify);
		}
		CHECK(list.entries == v.live - 1 && verify.entries == v.live);
		CHECK(list.memory_peak <= SCALE_MEMORY &&
		    verify.memory_peak <= SCALE_MEMORY);
		CHECK(list.repeats == 0 && verify.repeats == 0);
		CHECK(list.system_bytes <= scale_system(&g, &v, false));
		CHECK(verify.system_bytes <= scale_system(&g, &v, true));
		for (unsigned m = 0; cases[c].marked && m < 4; m++) {
			read_file_at(&g, "/f", scale_mark(g.objects[3].size, m),
			    byte, 1);
			CHECK(byte[0] == m + 1);
		}
		gen_free(&g);
	}
	CHECK(fflush(stdout) == 0);
}

/* Read and verify an external image: the integration script's entry to
 * native and research fixtures. Prints entry and verification counts.
 */
static void
read_external(const char *image)
{
	fsimg_options_t options;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	fsimg_reader_t *reader;
	fsimg_verify_report_t report;
	fsimg_error_t error;
	fsimg_status_t status;
	uint64_t entries;
	fixture_t view;
	char directory[1024];
	int fd, scratch;

	temp_name(directory, sizeof(directory), "fsimg-qnx6");
	CHECK(mkdtemp(directory) != NULL);
	scratch = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(scratch != -1);
	fsimg_options_init(&options);
	options.memory_bytes = 64 * 1024 * 1024;
	options.scratch_directory_fd = scratch;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	fd = open(image, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	CHECK(fd != -1);
	CHECK(fsimg_io_from_fd(ctx, fd, FSIMG_IO_READ, &io, NULL) == FSIMG_OK);
	status = fsimg_reader_open(ctx, io, NULL, &reader, &error);
	if (status != FSIMG_OK) {
		printf("open %d %s\n", status, error.message);
		goto done;
	}
	bzero(&view, sizeof(view));
	view.reader = reader;
	status = walk_all(&view, &entries, &error);
	fsimg_reader_free(reader);
	if (status != FSIMG_OK) {
		printf("list %d %s\n", status, error.message);
		goto done;
	}
	CHECK(fsimg_reader_open(ctx, io, NULL, &reader, NULL) == FSIMG_OK);
	status = fsimg_verify(reader, FSIMG_VERIFY_PAYLOAD, &report, &error);
	fsimg_reader_free(reader);
	if (status != FSIMG_OK) {
		printf("verify %d %s\n", status, error.message);
		goto done;
	}
	printf("entries %" PRIu64 " files %" PRIu64 " directories %" PRIu64
	    " free %" PRIu64 "\n", entries, report.files, report.directories,
	    report.free_clusters);
done:
	fsimg_io_free(io);
	fsimg_ctx_free(ctx);
	CHECK(close(scratch) == 0);
	CHECK(rmdir(directory) == 0);
	exit(status == FSIMG_OK ? 0 : 1);
}

/* Write a compact sample, 64 KiB at B = 512, as a fuzzing seed. */
static void
emit_seed(const char *path)
{
	qnx6_gen_t g;
	sample_t s;
	int fd;

	sample_build(&g, &s, 512, false, 0x100, 65536 + 512);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	CHECK(fd != -1);
	CHECK(write(fd, g.image, g.length) == (ssize_t)g.length);
	CHECK(close(fd) == 0);
	gen_free(&g);
}

int
main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--read-external") == 0)
		read_external(argv[2]);
	if (argc == 3 && strcmp(argv[1], "--seed") == 0) {
		emit_seed(argv[2]);
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--scale") == 0) {
		test_scale(true);
		return (skipped == 0 ? 0 : 77);
	}
	CHECK(argc == 1);
	test_codecs();
	test_matrix();
	test_shapes();
	test_hard_links();
	test_geometry();
	test_snapshots();
	test_probe();
	test_trees();
	test_inodes();
	test_names();
	test_link_targets();
	test_omitted_names();
	test_directories();
	test_allocation();
	test_failures();
	test_system_trees();
	test_full();
	test_deep_paths();
	test_short_reads();
	test_eof();
	test_scratch_faults();
	test_cancellation();
	test_extract();
	test_scale(false);
	if (skipped != 0)
		printf("SKIP: %u sparse-volume cases; every other case passed\n",
		    skipped);
	return (skipped == 0 ? 0 : 77);
}
