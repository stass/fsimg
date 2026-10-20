/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

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
	unsigned char	*slots;		/* a directory's slots */
	size_t		 slot_count;
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

typedef struct qnx6_gen {
	unsigned char	*image;
	uint64_t	 length;
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
	uint8_t		 bitmap_depth;
	uint8_t		 inode_depth;
	uint8_t		 name_depth;
	bool		 big;
	bool		 root_last;
	gen_object_t	 objects[GEN_INODES + 1];
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

static void
own(gen_object_t *object, uint32_t block)
{
	object->blocks = realloc(object->blocks,
	    (object->block_count + 1) * sizeof(*object->blocks));
	CHECK(object->blocks != NULL);
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

/* Lay out an empty volume of `length` bytes: N follows from the 4096-byte
 * end, the second header sits in the last whole 4096-byte unit, and R is
 * the block count of the bitmap and inode-table trees. Root (inode 1) and
 * .boot (inode 2) are created with formatter status 1.
 */
static void
gen_init(qnx6_gen_t *g, uint32_t block_size, bool big, uint64_t length,
    uint32_t inodes, uint32_t flags)
{
	uint64_t end, bitmap_bytes;
	uint32_t boot;

	bzero(g, sizeof(*g));
	g->block_size = block_size;
	g->pointers = block_size / 4;
	g->big = big;
	g->length = length;
	g->flags = flags;
	g->stride = 1;
	g->inode_count = inodes;
	end = length / 4096 * 4096;
	g->second = end - 4096;
	g->blocks = (uint32_t)((end - 16384) / block_size);
	bitmap_bytes = (g->blocks + 7) / 8;
	g->reservation = (uint32_t)(gen_tree_blocks((bitmap_bytes +
	    block_size - 1) / block_size, g->pointers) +
	    gen_tree_blocks((uint64_t)inodes * 128 / block_size, g->pointers));
	CHECK((uint64_t)inodes * 128 % block_size == 0);
	g->next_block = 2 * g->reservation;
	g->image = malloc(length);
	g->table = calloc(inodes, 128);
	g->bitmap = calloc((g->blocks + 7) / 8 + block_size, 1);
	g->names = calloc(GEN_NAMES, block_size);
	CHECK(g->image != NULL && g->table != NULL && g->bitmap != NULL &&
	    g->names != NULL);
	memset(g->image, GEN_POISON, length);
	bzero(g->image + end, length - end);
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
}

static void
gen_free(qnx6_gen_t *g)
{
	for (unsigned i = 0; i <= GEN_INODES; i++) {
		free(g->objects[i].blocks);
		free(g->objects[i].slots);
	}
	free(g->image);
	free(g->table);
	free(g->bitmap);
	free(g->names);
	free(g->name_blocks);
	free(g->system_blocks);
}

static uint32_t
gen_inode(qnx6_gen_t *g, uint16_t mode)
{
	for (uint32_t i = 1; i <= GEN_INODES && i <= g->inode_count; i++) {
		if (g->objects[i].live)
			continue;
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

	d->slots = realloc(d->slots, (d->slot_count + 1) * 32);
	CHECK(d->slots != NULL);
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
 * zero-padded. Its tree has the minimal depth.
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
	for (uint32_t i = 1; i <= GEN_INODES; i++)
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
	for (uint32_t i = 1; i <= GEN_INODES; i++) {
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
	for (uint32_t i = 1; i <= GEN_INODES; i++)
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
	for (uint32_t i = 1; i <= GEN_INODES && i <= g->inode_count; i++) {
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

typedef struct fixture {
	fsimg_ctx_t	*ctx;
	fsimg_io_t	*io;
	fsimg_reader_t	*reader;
	unsigned char	*image;
	uint64_t	 length;
	uint64_t	 calls;
	uint64_t	 forbid_first;	/* reads may not touch [first, end) */
	uint64_t	 forbid_end;
	uint64_t	 fail_first;	/* reads touching [first, end) fail */
	uint64_t	 fail_end;
	bool		 cancelled;
	int		 scratch;
	char		 directory[64];
} fixture_t;

#define RUN_LIST	0x1u	/* open, walk and read every file */
#define RUN_VERIFY	0x2u	/* open and verify with payload */

static bool
cancelled(void *argument)
{
	return (((fixture_t *)argument)->cancelled);
}

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
	bcopy(f->image + offset, buffer, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
image_size(void *argument, uint64_t *size, fsimg_error_t *error FSIMG_UNUSED)
{
	*size = ((fixture_t *)argument)->length;
	return (FSIMG_OK);
}

static void
fixture_new(fixture_t *f, unsigned char *image, uint64_t length,
    uint64_t max_work)
{
	fsimg_options_t options;
	fsimg_io_ops_t ops;

	bzero(f, sizeof(*f));
	f->image = image;
	f->length = length;
	strcpy(f->directory, "/tmp/fsimg-qnx6-XXXXXX");
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
	CHECK(fsimg_ctx_new(&options, &f->ctx, NULL) == FSIMG_OK);
	fsimg_io_ops_init(&ops);
	ops.read_at = image_read;
	ops.get_size = image_size;
	CHECK(fsimg_io_from_callbacks(f->ctx, &ops, f, &f->io, NULL) == FSIMG_OK);
}

/* Release everything and check that no memory, scratch or descriptor
 * outlived the reader.
 */
static void
fixture_free(fixture_t *f)
{
	fsimg_stats_t stats;

	fsimg_reader_free(f->reader);
	fsimg_io_free(f->io);
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.scratch_bytes == 0 && stats.descriptors == 1);
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
run_image(unsigned char *image, uint64_t length, unsigned what,
    fsimg_error_t *error)
{
	fixture_t f;
	fsimg_verify_report_t report;
	uint64_t entries;
	fsimg_status_t status;

	status = FSIMG_OK;
	if ((what & RUN_LIST) != 0) {
		fixture_new(&f, image, length, 0);
		status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, error);
		if (status == FSIMG_OK)
			status = walk_all(&f, &entries, error);
		fixture_free(&f);
	}
	if (status == FSIMG_OK && (what & RUN_VERIFY) != 0) {
		fixture_new(&f, image, length, 0);
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
	status = run_image(g->image, g->length, what, &error);
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

/* The bitmap byte holding block's bit; sample bitmaps have one leaf. */
static unsigned char *
bitmap_byte(qnx6_gen_t *g, uint32_t block)
{
	CHECK(g->bitmap_depth == 0 && block / 8 < g->block_size);
	return (block_at(g, g->bitmap_refs[0]) + block / 8);
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
	fixture_new(&f, g.image, g.length, 0);
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
	fixture_new(&f, g.image, g.length, 1000000);
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
	CHECK(run_image(bigger, length, RUN_VERIFY, NULL) == FSIMG_OK);

	/* The same bytes with the second header left at D + N * B: QNX looks
	 * for it at the slice end, so the volume is corrupt.
	 */
	memset(bigger + (length / 4096 * 4096 - 4096), GEN_POISON, 4096);
	bcopy(g.image + g.second, bigger + g.second, 4096);
	CHECK(run_image(bigger, length, RUN_LIST, NULL) == FSIMG_ECORRUPT);

	/* A volume image padded past its slice end. */
	bcopy(g.image, bigger, g.length);
	bzero(bigger + g.length, 4096);
	CHECK(run_image(bigger, g.length + 4096, RUN_LIST, NULL) ==
	    FSIMG_ECORRUPT);

	/* S2 <= 12288 and truncated headers. */
	CHECK(run_image(g.image, 20479, RUN_LIST, NULL) == FSIMG_ECORRUPT);
	CHECK(run_image(g.image, 8703, RUN_LIST, NULL) == FSIMG_EFORMAT);
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
	fixture_new(&f, g.image, g.length, 0);
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
	fixture_new(&f, g.image, g.length, 0);
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
	fixture_new(&f, g.image, g.length, 0);
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
	CHECK(run_image(zero, 65536, RUN_LIST, NULL) == FSIMG_EFORMAT);
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

	/* QNX forbids NUL, slash and the dot names; the profile also
	 * forbids control bytes and invalid UTF-8.
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
	expect(&g, RUN_LIST, FSIMG_EUNSUPPORTED, "control byte");
	slot[5] = 0x7f;
	expect(&g, RUN_LIST, FSIMG_EUNSUPPORTED, "DEL");
	slot[5] = 0xff;
	expect(&g, RUN_LIST, FSIMG_EUNSUPPORTED, "invalid UTF-8");
	slot[5] = 'e';
	slot[4] = 5;
	bcopy("empty", slot + 5, 5);
	expect(&g, RUN_LIST, FSIMG_ECORRUPT, "duplicate siblings");
	slot[5] = 'E';
	expect(&g, RUN_LIST, FSIMG_OK, "case-distinct siblings");
	gen_free(&g);
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
		fixture_new(&f, g.image, g.length, 0);
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
	fixture_new(&f, g.image, g.length, 0);
	f.fail_first = GEN_DATA + (uint64_t)g.objects[s.indirect].blocks[3] *
	    g.block_size;
	f.fail_end = f.fail_first + g.block_size;
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(walk_all(&f, &entries, NULL) == FSIMG_EIO);
	fixture_free(&f);

	fixture_new(&f, g.image, g.length, 0);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	f.cancelled = true;
	CHECK(fsimg_verify(f.reader, 0, &report, NULL) == FSIMG_ECANCELLED);
	CHECK(!report.complete);
	f.cancelled = false;
	fixture_free(&f);

	fixture_new(&f, g.image, g.length, 200);
	status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL);
	if (status == FSIMG_OK)
		status = walk_all(&f, &entries, NULL);
	CHECK(status == FSIMG_ELIMIT);
	fixture_free(&f);
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
	fixture_new(&f, g.image, g.length, 0);
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
	char directory[64];
	int fd, scratch;

	strcpy(directory, "/tmp/fsimg-qnx6-XXXXXX");
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
	test_directories();
	test_allocation();
	test_failures();
	test_extract();
	return (0);
}
