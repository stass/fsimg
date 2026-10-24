/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "fsimg.h"
#include "host.h"
#include "internal.h"
#include "meta.h"
#include "test.h"

/*
 * QNX6 writer tests. Images are created through the public API and checked
 * twice: by fsimg's own reader and verifier, and by the independent checker
 * below, which decodes the classic layout from the format design with its
 * own offsets, CRC, checksum and tree arithmetic and shares no code with
 * lib/formats/qnx6/. The checker also scans every byte the layout leaves
 * unused, so a stale or poisoned byte anywhere fails the test.
 */

#define D_ORIGIN	12288u
#define SENTINEL	UINT32_C(0xffffffff)
#define T_FILE		0100000u
#define T_DIR		0040000u
#define T_LINK		0120000u
#define T_FIFO		0010000u
#define POISON		0xa5u

/*
 * The independent checker.
 */

typedef struct chk_entry {
	char		 path[640];
	uint64_t	 size;
	uint32_t	 inode;
	uint32_t	 uid;
	uint32_t	 gid;
	uint32_t	 times[4];
	uint32_t	 digest;
	uint16_t	 mode;
	uint16_t	 links;
} chk_entry_t;

typedef struct chk {
	const unsigned char *img;
	uint64_t	 length;
	uint64_t	 second;
	uint32_t	 block;
	uint32_t	 pointers;
	uint32_t	 blocks;
	uint32_t	 capacity;
	uint32_t	 allocated;
	uint32_t	 used_inodes;
	uint32_t	 reservation;
	uint32_t	 names_leaves;
	uint32_t	*names;		/* leaf block of each long-name record */
	unsigned char	*owned;		/* one byte per block */
	unsigned char	*name_used;	/* one byte per long-name record */
	unsigned char	*inode_seen;	/* one byte per inode */
	uint16_t	*name_count;	/* names naming each inode */
	chk_entry_t	*entries;
	size_t		 count;
	size_t		 capacity_entries;
	unsigned	 errors;
	bool		 big;
} chk_t;

/* The expected header settings. */
typedef struct chk_expect {
	unsigned char	 uuid[16];
	uint32_t	 reserve;
	uint32_t	 time;
	bool		 big;
} chk_expect_t;

#define CHK(c, cond, what) do { \
	if (!(cond)) { \
		if ((c)->errors++ < 20) \
			fprintf(stderr, "checker: %s (%s:%d)\n", (what), \
			    __FILE__, __LINE__); \
	} \
} while (0)

static uint32_t
get32(const chk_t *c, const unsigned char *p)
{
	if (c->big)
		return ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
		    (uint32_t)p[2] << 8 | p[3]);
	return ((uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 |
	    (uint32_t)p[1] << 8 | p[0]);
}

static uint16_t
get16(const chk_t *c, const unsigned char *p)
{
	return (c->big ? (uint16_t)(p[0] << 8 | p[1]) : (uint16_t)(p[1] << 8 | p[0]));
}

static uint64_t
get64(const chk_t *c, const unsigned char *p)
{
	return (c->big ? (uint64_t)get32(c, p) << 32 | get32(c, p + 4) :
	    (uint64_t)get32(c, p + 4) << 32 | get32(c, p));
}

/* MSB-first CRC-32, polynomial 0x04c11db7, zero start, no final XOR, as a
 * bytewise table-free long division.
 */
static uint32_t
chk_crc(const unsigned char *p, size_t n)
{
	uint32_t r = 0;

	for (size_t i = 0; i < n; i++) {
		r ^= (uint32_t)p[i] << 24;
		for (int b = 0; b < 8; b++)
			r = (r & 0x80000000u) ? (r << 1) ^ 0x04c11db7u : r << 1;
	}
	return (r);
}

static uint32_t
chk_name_sum(const unsigned char *p, size_t n)
{
	uint32_t s = 0, prev;

	for (size_t i = 0; i < n; i++) {
		prev = s;
		s = (prev >> 1) + p[i];
		if (prev & 1)
			s ^= 0x80000000u;
	}
	return (s == 0 ? SENTINEL : s);
}

static bool
all_zero(const unsigned char *p, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++)
		if (p[i] != 0)
			return (false);
	return (true);
}

static const unsigned char *
chk_block(const chk_t *c, uint32_t block)
{
	return (c->img + D_ORIGIN + (uint64_t)block * c->block);
}

/* Indirect blocks a tree of n leaves needs at the minimal depth, which the
 * caller passes, and that depth's minimality.
 */
static uint64_t
chk_indirect(uint64_t n, uint32_t p, unsigned depth)
{
	uint64_t total = 0, span = 1;

	for (unsigned m = 1; m <= depth; m++) {
		span *= p;
		total += (n + span - 1) / span;
	}
	return (total);
}

static unsigned
chk_depth(uint64_t n, uint32_t p)
{
	uint64_t cap = 16;
	unsigned depth = 0;

	while (cap < n) {
		cap *= p;
		depth++;
	}
	return (depth);
}

typedef struct chk_walk {
	chk_t		*c;
	uint32_t	*leaves;
	uint64_t	 n;
	uint64_t	 next_leaf;
	uint64_t	 next_indirect;
	uint64_t	 indirect;
	uint32_t	 first;
	bool		 claim;
} chk_walk_t;

static void
chk_claim(chk_t *c, uint32_t block, bool claim)
{
	CHK(c, block < c->blocks, "block reference out of range");
	if (block >= c->blocks || !claim)
		return;
	CHK(c, c->owned[block] == 0, "block owned twice");
	c->owned[block] = 1;
}

/* Visit a node at the given height, requiring the post-order run layout:
 * indirect blocks at first + (post-order index), leaves at first + I + k.
 */
static void
chk_node(chk_walk_t *w, uint32_t ref, unsigned height)
{
	chk_t *c = w->c;
	const unsigned char *p;
	uint32_t child;

	if (height == 0) {
		CHK(c, ref == w->first + w->indirect + w->next_leaf,
		    "leaf out of its contiguous position");
		chk_claim(c, ref, w->claim);
		w->leaves[w->next_leaf++] = ref;
		return;
	}
	chk_claim(c, ref, w->claim);
	if (ref >= c->blocks)
		return;
	p = chk_block(c, ref);
	for (uint32_t k = 0; k < c->pointers; k++) {
		child = get32(c, p + (size_t)k * 4);
		if (w->next_leaf >= w->n) {
			CHK(c, child == SENTINEL, "unused pointer is not the sentinel");
			continue;
		}
		CHK(c, child != SENTINEL, "required pointer is unused");
		if (child == SENTINEL)
			return;
		chk_node(w, child, height - 1);
	}
	CHK(c, ref == w->first + w->next_indirect, "indirect block out of post-order");
	w->next_indirect++;
}

/* Walk a tree descriptor (sixteen references at refs and a depth) of length
 * bytes. Returns its leaves in logical order; the caller frees them.
 */
static uint32_t *
chk_tree(chk_t *c, const unsigned char *refs, unsigned depth, uint64_t length,
    bool claim)
{
	chk_walk_t w = { .c = c, .claim = claim };
	uint64_t used_slots, span;
	uint32_t ref;

	w.n = (length + c->block - 1) / c->block;
	w.leaves = calloc(w.n + 1, sizeof(uint32_t));
	CHECK(w.leaves != NULL);
	CHK(c, depth == chk_depth(w.n, c->pointers), "tree depth is not minimal");
	w.indirect = chk_indirect(w.n, c->pointers, depth);
	span = 1;
	for (unsigned m = 0; m < depth; m++)
		span *= c->pointers;
	used_slots = (w.n + span - 1) / span;
	/* The last root child is the last indirect block in post-order. */
	if (w.n != 0)
		w.first = get32(c, refs + (used_slots - 1) * 4) -
		    (uint32_t)(depth == 0 ? w.n - 1 : w.indirect - 1);
	for (unsigned k = 0; k < 16; k++) {
		ref = get32(c, refs + k * 4);
		if (k >= used_slots) {
			CHK(c, ref == SENTINEL, "unused root pointer is not the sentinel");
			continue;
		}
		CHK(c, ref != SENTINEL, "required root pointer is unused");
		if (ref != SENTINEL)
			chk_node(&w, ref, depth);
	}
	CHK(c, w.next_leaf == w.n && w.next_indirect == w.indirect,
	    "tree walk did not cover its blocks");
	return (w.leaves);
}

/* Copy an object's bytes [0, length) out of its leaves; the slack after
 * length in the last leaf must be zero.
 */
static unsigned char *
chk_content(chk_t *c, const uint32_t *leaves, uint64_t length)
{
	unsigned char *out;
	uint64_t n = (length + c->block - 1) / c->block, take;

	out = malloc(length + 1);
	CHECK(out != NULL);
	for (uint64_t k = 0; k < n; k++) {
		take = length - k * c->block < c->block ? length - k * c->block :
		    c->block;
		if (leaves[k] >= c->blocks)
			continue;
		memcpy(out + k * c->block, chk_block(c, leaves[k]), (size_t)take);
		if (take < c->block)
			CHK(c, all_zero(chk_block(c, leaves[k]) + take, c->block - take),
			    "partial block slack is not zero");
	}
	return (out);
}

/* FNV-1a, continued from h; a digest starts at 2166136261. */
static uint32_t
chk_digest(uint32_t h, const unsigned char *p, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++)
		h = (h ^ p[i]) * 16777619u;
	return (h);
}

static const unsigned char *
chk_inode(const chk_t *c, const uint32_t *table, uint32_t number)
{
	uint64_t offset = (uint64_t)(number - 1) * 128;

	return (chk_block(c, table[offset / c->block]) + offset % c->block);
}

static void chk_directory(chk_t *, const uint32_t *, uint32_t, uint32_t,
    const char *, unsigned);

/* Record one live slot's inode as an entry and descend into directories. */
static void
chk_child(chk_t *c, const uint32_t *table, uint32_t number, uint32_t parent,
    const char *path, unsigned depth)
{
	const unsigned char *ino;
	chk_entry_t *e;
	uint32_t *leaves;
	uint64_t take;
	uint16_t mode;

	CHK(c, number >= 1 && number <= c->capacity, "slot names an inode out of range");
	if (number < 1 || number > c->capacity)
		return;
	c->name_count[number]++;
	if (c->inode_seen[number]) {
		CHK(c, false, "an inode has two names");
		return;
	}
	c->inode_seen[number] = 1;
	ino = chk_inode(c, table, number);
	mode = get16(c, ino + 32);
	CHK(c, ino[101] == (number <= 2 ? 1 : 3), "inode status is not the profile's");
	CHK(c, all_zero(ino + 102, 26), "inode tail is not zero");
	if (c->count == c->capacity_entries) {
		c->capacity_entries = c->capacity_entries * 2 + 16;
		c->entries = realloc(c->entries, c->capacity_entries * sizeof(*e));
		CHECK(c->entries != NULL);
	}
	e = &c->entries[c->count++];
	bzero(e, sizeof(*e));
	CHECK(strlen(path) < sizeof(e->path));
	bcopy(path, e->path, strlen(path) + 1);
	e->inode = number;
	e->size = get64(c, ino);
	e->uid = get32(c, ino + 8);
	e->gid = get32(c, ino + 12);
	for (unsigned i = 0; i < 4; i++)
		e->times[i] = get32(c, ino + 16 + i * 4);
	e->mode = mode;
	e->links = get16(c, ino + 34);
	if ((mode & 0170000) == T_DIR) {
		chk_directory(c, table, number, parent, path, depth + 1);
		return;
	}
	CHK(c, e->links == 1, "file link count is not one");
	if ((mode & 0170000) == T_FIFO)
		CHK(c, e->size == 0 && ino[100] == 0, "FIFO has content");
	else
		CHK(c, (mode & 0170000) == T_FILE || (mode & 0170000) == T_LINK,
		    "unknown inode type");
	/* Digest the content leaf by leaf, so a file of any size streams. */
	leaves = chk_tree(c, ino + 36, ino[100], e->size, true);
	e->digest = 2166136261u;
	for (uint64_t k = 0; k * c->block < e->size; k++) {
		take = e->size - k * c->block < c->block ? e->size - k * c->block :
		    c->block;
		if (leaves[k] >= c->blocks)
			continue;
		e->digest = chk_digest(e->digest, chk_block(c, leaves[k]), take);
		if (take < c->block)
			CHK(c, all_zero(chk_block(c, leaves[k]) + take, c->block - take),
			    "partial block slack is not zero");
	}
	free(leaves);
}

/* Check a directory: whole blocks, "." and ".." first, live slots with
 * inline or long names in byte order, unused slots all zero.
 */
static void
chk_directory(chk_t *c, const uint32_t *table, uint32_t number, uint32_t parent,
    const char *path, unsigned depth)
{
	const unsigned char *ino, *slot, *record;
	char child[2048], name[512], previous[512];
	uint32_t *leaves, inode, index;
	unsigned char *bytes;
	uint64_t size;
	size_t length, prev_length = 0;
	unsigned subdirectories = 0;

	CHK(c, depth < 64, "directory nesting too deep");
	if (depth >= 64)
		return;
	ino = chk_inode(c, table, number);
	size = get64(c, ino);
	CHK(c, size != 0 && size % c->block == 0, "directory is not whole blocks");
	leaves = chk_tree(c, ino + 36, ino[100], size, true);
	bytes = chk_content(c, leaves, size);
	CHK(c, get32(c, bytes) == number && bytes[4] == 1 && bytes[5] == '.' &&
	    all_zero(bytes + 6, 26), "bad . slot");
	CHK(c, get32(c, bytes + 32) == parent && bytes[36] == 2 &&
	    bytes[37] == '.' && bytes[38] == '.' && all_zero(bytes + 39, 25),
	    "bad .. slot");
	for (uint64_t s = 2; s < size / 32; s++) {
		slot = bytes + s * 32;
		inode = get32(c, slot);
		if (inode == 0) {
			CHK(c, all_zero(slot, 32), "unused slot is not zero");
			continue;
		}
		if (slot[4] == 0xff) {
			index = get32(c, slot + 8);
			CHK(c, slot[5] == 0 && slot[6] == 0 && slot[7] == 0 &&
			    all_zero(slot + 16, 16), "long-name slot padding");
			CHK(c, index < c->names_leaves, "long-name index out of range");
			if (index >= c->names_leaves)
				continue;
			CHK(c, !c->name_used[index], "long-name record used twice");
			c->name_used[index] = 1;
			record = chk_block(c, c->names[index]);
			length = get16(c, record);
			CHK(c, length >= 28 && length <= 510, "long-name length");
			if (length < 28 || length > 510)
				continue;
			CHK(c, all_zero(record + 2 + length, c->block - 2 - length),
			    "long-name block slack is not zero");
			CHK(c, get32(c, slot + 12) == chk_name_sum(record + 2, length),
			    "long-name checksum");
			memcpy(name, record + 2, length);
		} else {
			length = slot[4];
			CHK(c, length >= 1 && length <= 27, "inline name length");
			if (length < 1 || length > 27)
				continue;
			CHK(c, all_zero(slot + 5 + length, 27 - length),
			    "inline name padding is not zero");
			memcpy(name, slot + 5, length);
		}
		name[length] = '\0';
		CHK(c, memchr(name, '/', length) == NULL && memchr(name, 0, length) == NULL,
		    "name holds slash or NUL");
		if (s > 2)
			CHK(c, memcmp(previous, name, length < prev_length ? length :
			    prev_length) < 0 || (memcmp(previous, name, length <
			    prev_length ? length : prev_length) == 0 && prev_length < length),
			    "slots are not in byte order");
		memcpy(previous, name, length);
		prev_length = length;
		snprintf(child, sizeof(child), "%s%s%s", path,
		    strcmp(path, "/") == 0 ? "" : "/", name);
		if (inode >= 1 && inode <= c->capacity &&
		    (get16(c, chk_inode(c, table, inode) + 32) & 0170000) == T_DIR)
			subdirectories++;
		chk_child(c, table, inode, number, child, depth);
	}
	CHK(c, get16(c, ino + 34) == 2 + subdirectories, "directory link count");
	CHK(c, ino[101] == (number <= 2 ? 1 : 3), "directory status");
	free(bytes);
	free(leaves);
}

/* The native formatter's allocation-group count, written from the research
 * record: a ladder over the smaller of the bitmap and inode-table leaf-block
 * counts, capped at 4, 8, 16 or 32 by the size of N.
 */
static uint32_t
native_groups(uint64_t blocks, uint32_t block, uint64_t capacity)
{
	uint64_t bitmap_leaves, inode_leaves, m;
	uint32_t ladder, cap;

	bitmap_leaves = ((blocks + 7) / 8 + block - 1) / block;
	inode_leaves = (capacity * 128 + block - 1) / block;
	m = bitmap_leaves < inode_leaves ? bitmap_leaves : inode_leaves;
	ladder = m <= 3 ? 1 : m <= 7 ? 4 : m <= 15 ? 8 : m <= 31 ? 16 : 32;
	cap = blocks <= 0xfffff ? 4 : blocks <= 0x7fffff ? 8 :
	    blocks < 0x8000000 ? 16 : 32;
	return (ladder < cap ? ladder : cap);
}

/* Check a whole image. The checker owns the entries it collects. */
static void
chk_image(chk_t *c, const unsigned char *img, uint64_t length,
    const chk_expect_t *expect)
{
	const unsigned char *h = img + 8192;
	uint32_t *bitmap, *table, inode_leaves, used_inodes, free_blocks;
	uint64_t bitmap_bytes, end, rb, ri, bit, last;
	unsigned char *bits;
	static const unsigned char spare_refs[64] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff
	};

	bzero(c, sizeof(*c));
	c->img = img;
	c->length = length;
	CHECK(length >= 16384 && length % 512 == 0);
	CHK(c, all_zero(img, 8192), "boot area is not zero");
	if (h[0] == 0x22 && h[1] == 0x11 && h[2] == 0x19 && h[3] == 0x68)
		c->big = false;
	else if (h[0] == 0x68 && h[1] == 0x19 && h[2] == 0x11 && h[3] == 0x22)
		c->big = true;
	else
		CHECK(!"no QNX6 magic at 8192");
	CHK(c, c->big == expect->big, "byte order");
	end = length / 4096 * 4096;
	c->second = end - 4096;
	CHK(c, memcmp(h, img + c->second, 512) == 0, "headers differ");
	CHK(c, all_zero(h + 512, 3584) && all_zero(img + c->second + 512, 3584),
	    "header area padding is not zero");
	CHK(c, all_zero(img + end, length - end), "tail past E is not zero");
	CHK(c, get32(c, h + 4) == chk_crc(h + 8, 504), "header CRC");
	CHK(c, get64(c, h + 8) == 1, "serial");
	CHK(c, get32(c, h + 16) == expect->time && get32(c, h + 20) == expect->time,
	    "format and sync time");
	CHK(c, get32(c, h + 24) == 0x100, "flags");
	CHK(c, get16(c, h + 28) == 4, "version");
	CHK(c, get16(c, h + 30) == expect->reserve, "reserve");
	CHK(c, memcmp(h + 32, expect->uuid, 16) == 0, "uuid");
	c->block = get32(c, h + 48);
	CHECK(c->block == 512 || c->block == 1024 || c->block == 2048 ||
	    c->block == 4096);
	c->pointers = c->block / 4;
	c->capacity = get32(c, h + 52);
	c->blocks = get32(c, h + 60);
	free_blocks = get32(c, h + 64);
	used_inodes = c->capacity - get32(c, h + 56);
	c->used_inodes = used_inodes;
	CHK(c, get32(c, h + 68) == native_groups(c->blocks, c->block,
	    c->capacity), "group count");
	CHECK(D_ORIGIN + (uint64_t)c->blocks * c->block <= c->second);
	CHK(c, D_ORIGIN + (uint64_t)c->blocks * c->block + c->block > c->second,
	    "the volume does not fill its slice");
	CHK(c, all_zero(img + D_ORIGIN + (uint64_t)c->blocks * c->block,
	    c->second - D_ORIGIN - (uint64_t)c->blocks * c->block),
	    "bytes between the blocks and the second header are not zero");
	for (unsigned t = 0; t < 4; t++) {
		const unsigned char *d = h + 72 + t * 80;

		CHK(c, d[73] == 1 && all_zero(d + 74, 6), "descriptor mode or padding");
	}
	CHK(c, get64(c, h + 312) == 0 && memcmp(h + 320, spare_refs, 64) == 0 &&
	    h[384] == 0, "spare tree is not empty");
	CHK(c, all_zero(h + 392, 120), "extension area is not zero");
	c->owned = calloc(c->blocks, 1);
	c->inode_seen = calloc((size_t)c->capacity + 1, 1);
	c->name_count = calloc((size_t)c->capacity + 1, sizeof(uint16_t));
	CHECK(c->owned != NULL && c->inode_seen != NULL && c->name_count != NULL);

	/* The system run: the bitmap tree from block 0, then the inode tree,
	 * each with its indirect blocks in post-order before its leaves.
	 */
	bitmap_bytes = get64(c, h + 152);
	CHK(c, bitmap_bytes == ((uint64_t)c->blocks + 7) / 8, "bitmap length");
	bitmap = chk_tree(c, h + 160, h[224], bitmap_bytes, true);
	rb = chk_indirect((bitmap_bytes + c->block - 1) / c->block, c->pointers,
	    h[224]) + (bitmap_bytes + c->block - 1) / c->block;
	CHK(c, get64(c, h + 72) == (uint64_t)c->capacity * 128, "inode table length");
	CHK(c, (uint64_t)c->capacity * 128 % c->block == 0,
	    "inode capacity is not whole table blocks");
	inode_leaves = (uint32_t)((uint64_t)c->capacity * 128 / c->block);
	table = chk_tree(c, h + 80, h[144], (uint64_t)c->capacity * 128, true);
	ri = chk_indirect(inode_leaves, c->pointers, h[144]) + inode_leaves;
	CHK(c, h[224] <= 2 && h[144] <= 2, "system tree deeper than 2");
	c->reservation = (uint32_t)(rb + ri);
	for (uint64_t p = 0; p < rb + ri && p < c->blocks; p++)
		CHK(c, c->owned[p], "system run has a gap");
	for (uint64_t p = rb + ri; p < 2 * (rb + ri) && p < c->blocks; p++) {
		CHK(c, !c->owned[p], "twin region is referenced");
		CHK(c, all_zero(chk_block(c, (uint32_t)p), c->block), "twin is not zero");
		c->owned[p] = 1;
	}

	/* The long-name stream: one whole block per record. */
	c->names_leaves = (uint32_t)(get64(c, h + 232) / c->block);
	CHK(c, get64(c, h + 232) % c->block == 0, "long-name stream length");
	c->names = chk_tree(c, h + 240, h[304], get64(c, h + 232), true);
	c->name_used = calloc(c->names_leaves + 1, 1);
	CHECK(c->name_used != NULL);

	/* The namespace from the root, inode 1, which is its own parent. */
	c->inode_seen[1] = 1;
	c->name_count[1]++;
	{
		chk_entry_t *e;

		c->capacity_entries = 16;
		c->entries = calloc(c->capacity_entries, sizeof(*e));
		CHECK(c->entries != NULL);
		e = &c->entries[c->count++];
		strcpy(e->path, "/");
		e->inode = 1;
		e->mode = get16(c, chk_inode(c, table, 1) + 32);
		e->uid = get32(c, chk_inode(c, table, 1) + 8);
		for (unsigned i = 0; i < 4; i++)
			e->times[i] = get32(c, chk_inode(c, table, 1) + 16 + i * 4);
	}
	CHK(c, get16(c, chk_inode(c, table, 1) + 32) == (T_DIR | 0775), "root mode");
	chk_directory(c, table, 1, 1, "/", 0);
	for (uint32_t i = 0; i < c->names_leaves; i++)
		CHK(c, c->name_used[i], "long-name record is not referenced");

	/* Inodes 1..used are live, in order; the rest are 128 zero bytes. */
	for (uint32_t i = 1; i <= c->capacity; i++) {
		if (i <= used_inodes)
			CHK(c, c->inode_seen[i], "a used inode is not referenced");
		else
			CHK(c, all_zero(chk_inode(c, table, i), 128),
			    "an unused inode is not zero");
	}

	/* Allocation is the prefix [0, A): everything below is owned, nothing
	 * above, and every free block is zero.
	 */
	c->allocated = c->blocks - free_blocks;
	for (uint32_t p = 0; p < c->blocks; p++) {
		if (p < c->allocated) {
			CHK(c, c->owned[p], "allocated block has no owner");
		} else {
			CHK(c, !c->owned[p], "owned block beyond the allocation");
			CHK(c, all_zero(chk_block(c, p), c->block), "free block is not zero");
		}
	}
	bits = chk_content(c, bitmap, (bitmap_bytes + c->block - 1) / c->block *
	    c->block);
	last = (bitmap_bytes + c->block - 1) / c->block * c->block * 8;
	for (bit = 0; bit < last; bit++) {
		bool set = (bits[bit / 8] >> (bit % 8)) & 1;

		CHK(c, set == (bit < c->allocated || bit >= c->blocks),
		    "bitmap bit disagrees with the allocation");
	}
	free(bits);
	free(bitmap);
	free(table);
}

/* The format design's sizing rule restated: whether N blocks hold the
 * system run and its twin for the native ladder capacity (raised to the
 * used inodes), the given content blocks and the reserve.
 */
static bool
chk_fits(const chk_t *c, uint64_t n, uint64_t content, uint32_t reserve)
{
	uint64_t capacity, per_block, bitmap, table, run;

	if (n <= 0xfffff)
		capacity = n / 8;
	else if (n <= 0x7fffff)
		capacity = n / 32;
	else if (n < 0x8000000)
		capacity = n / 64;
	else
		capacity = n / 128;
	if (capacity < c->used_inodes)
		capacity = c->used_inodes;
	per_block = c->block / 128;
	table = (capacity + per_block - 1) / per_block;
	bitmap = ((n + 7) / 8 + c->block - 1) / c->block;
	run = bitmap + chk_indirect(bitmap, c->pointers,
	    chk_depth(bitmap, c->pointers)) + table + chk_indirect(table,
	    c->pointers, chk_depth(table, c->pointers));
	return (2 * run + content + (n * reserve + 99) / 100 <= n);
}

/* An automatically sized volume is the smallest multiple of 64 blocks that
 * fits its content.
 */
static void
chk_minimal(const chk_t *c, uint32_t reserve)
{
	uint64_t content = c->allocated - 2 * (uint64_t)c->reservation;

	CHECK(c->blocks % 64 == 0);
	CHECK(chk_fits(c, c->blocks, content, reserve));
	CHECK(c->blocks == 64 || !chk_fits(c, c->blocks - 64, content, reserve));
}

static void
chk_free(chk_t *c)
{
	free(c->names);
	free(c->owned);
	free(c->name_used);
	free(c->inode_seen);
	free(c->name_count);
	free(c->entries);
}

static const chk_entry_t *
chk_find(const chk_t *c, const char *path)
{
	for (size_t i = 0; i < c->count; i++)
		if (strcmp(c->entries[i].path, path) == 0)
			return (&c->entries[i]);
	return (NULL);
}

/*
 * The writer fixture: a context, a writer with a provider and memory sinks.
 */

typedef struct fixture {
	fsimg_ctx_t	*ctx;
	fsimg_writer_t	*writer;
	fsimg_stats_t	 baseline;
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
	bool		 fail_write;
	bool		 fail_flush;
} memory_sink_t;

/* One fixture entry. source is a provider ID, or zero for the zero source.
 * Provider 3 serves "hello.txt" and provider 9 a target holding a NUL byte;
 * every other provider serves a pattern of its ID and the offset.
 */
typedef struct spec {
	const char	*path;
	fsimg_entry_kind_t kind;
	uint64_t	 size;
	uint64_t	 source;
} spec_t;

#define LONGEST_NAME	"/dir/" \
	"nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn" \
	"nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn" \
	"nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn" \
	"nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn" \
	"nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn"

static const spec_t tree[] = {
	{ "/hello.txt", FSIMG_ENTRY_FILE, 33, 1 },
	{ "/dir", FSIMG_ENTRY_DIRECTORY, 0, 0 },
	{ "/dir/sub", FSIMG_ENTRY_DIRECTORY, 0, 0 },
	{ "/dir/sub/deep.bin", FSIMG_ENTRY_FILE, 5000, 2 },
	{ "/dir/abcdefghijklmnopqrstuvwxyz0", FSIMG_ENTRY_FILE, 1, 4 },
	{ "/dir/abcdefghijklmnopqrstuvwxyz01", FSIMG_ENTRY_FILE, 0, 4 },
	{ LONGEST_NAME, FSIMG_ENTRY_FILE, 7, 5 },
	{ "/dir/.boot", FSIMG_ENTRY_FILE, 3, 6 },
	{ "/link", FSIMG_ENTRY_SYMLINK, 9, 3 },
	{ "/fifo", FSIMG_ENTRY_FIFO, 0, 0 },
	{ "/zero.bin", FSIMG_ENTRY_FILE, 70001, 0 },
	{ "/R\303\251sum\303\251 collection.txt", FSIMG_ENTRY_FILE, 100, 7 },
	{ "/big.bin", FSIMG_ENTRY_FILE, 300000, 8 },
	{ "/-before-boot", FSIMG_ENTRY_DIRECTORY, 0, 0 },
	{ "/Upper", FSIMG_ENTRY_DIRECTORY, 0, 0 },
	{ "/empty", FSIMG_ENTRY_FILE, 0, 0 }
};
#define TREE_COUNT	(sizeof(tree) / sizeof(tree[0]))

static const unsigned char test_uuid[16] = {
	0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x33, 0x33,
	0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55
};

static unsigned char
content_byte(uint64_t id, uint64_t offset)
{
	static const char target[] = "hello.txt", bad[] = "a\0b";

	if (id == 3)
		return ((unsigned char)target[offset % 9]);
	if (id == 9)
		return ((unsigned char)bad[offset % 3]);
	return ((unsigned char)(offset * 37 + id * 11 + (offset >> 13)));
}

static uint32_t
expected_digest(uint64_t id, uint64_t size)
{
	unsigned char b;
	uint32_t h = 2166136261u;

	for (uint64_t i = 0; i < size; i++) {
		b = id == 0 ? 0 : content_byte(id, i);
		h = chk_digest(h, &b, 1);
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
 * with eof set the provider ends after 1000 bytes.
 */
static fsimg_status_t
provider_read(void *argument, uint64_t id, uint64_t offset,
    void *buffer, size_t length, size_t *transferred,
    fsimg_error_t *error FSIMG_UNUSED)
{
	fixture_t *fixture = argument;
	unsigned char *bytes = buffer;

	if (fixture->eof && offset >= 1000) {
		*transferred = 0;
		return (FSIMG_OK);
	}
	if (length > 1000)
		length = 1000;
	for (size_t i = 0; i < length; i++)
		bytes[i] = content_byte(id, offset + i);
	*transferred = length;
	return (FSIMG_OK);
}

/* Installed by test_link_range only: the link target, provider 3, claims
 * to be one hole, which would turn the target into NUL bytes if the source
 * layer consulted it for a link.
 */
static bool ranges_enabled;

static fsimg_status_t
provider_range(void *argument FSIMG_UNUSED, uint64_t id, uint64_t offset,
    fsimg_range_t *range, fsimg_error_t *error FSIMG_UNUSED)
{
	CHECK(id == 3 && offset < 9);
	range->offset = offset;
	range->length = 9 - offset;
	range->kind = FSIMG_RANGE_ZERO;
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

	return (fixture->cancel_after != 0 && ++fixture->polls >= fixture->cancel_after);
}

/* Open a fixture. pages sizes the metadata cache; scratch false keeps all
 * metadata in memory.
 */
static void
fixture_open_with(fixture_t *fixture, const char *format,
    const fsimg_write_options_t *write_options, uint32_t pages, bool scratch)
{
	fsimg_options_t options;
	fsimg_source_ops_t source_ops;

	bzero(fixture, sizeof(*fixture));
	snprintf(fixture->directory, sizeof(fixture->directory), "qnx6-write-XXXXXX");
	CHECK(mkdtemp(fixture->directory) != NULL);
	fixture->parent = open(fixture->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fixture->parent != -1);
	fsimg_options_init(&options);
	options.memory_bytes = UINT64_C(64) * 1024 * 1024;
	options.scratch_directory_fd = scratch ? fixture->parent : -1;
	options.max_open_files = 16;
	options.loss = loss_report;
	options.loss_argument = fixture;
	options.cancelled = cancel_poll;
	options.cancel_argument = fixture;
	CHECK(fsimg_ctx_new(&options, &fixture->ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_test_meta_cache(fixture->ctx, pages, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &fixture->baseline, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_new(fixture->ctx, format, write_options,
	    &fixture->writer, NULL) == FSIMG_OK);
	fsimg_source_ops_init(&source_ops);
	source_ops.validate = provider_validate;
	source_ops.read_at = provider_read;
	if (ranges_enabled)
		source_ops.next_range = provider_range;
	CHECK(fsimg_writer_set_sources(fixture->writer, &source_ops, fixture,
	    NULL) == FSIMG_OK);
}

static void
fixture_open(fixture_t *fixture, const fsimg_write_options_t *write_options)
{
	fixture_open_with(fixture, "qnx6", write_options, 1, true);
}

/* Close a fixture and require that it released every resource. */
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
add_one(fixture_t *fixture, const spec_t *spec, const fsimg_metadata_t *metadata)
{
	fsimg_entry_t entry;
	fsimg_source_t source;

	bzero(&entry, sizeof(entry));
	entry.path = spec->path;
	entry.kind = spec->kind;
	entry.size = spec->size;
	if (metadata != NULL)
		entry.metadata = *metadata;
	source.kind = spec->source == 0 ? FSIMG_SOURCE_ZERO : FSIMG_SOURCE_PROVIDER;
	source.id = spec->source;
	return (fsimg_writer_add(fixture->writer, &entry,
	    spec->kind == FSIMG_ENTRY_FILE || spec->kind == FSIMG_ENTRY_SYMLINK ?
	    &source : NULL, NULL));
}

static void
add_tree(fixture_t *fixture, bool reverse)
{
	for (size_t i = 0; i < TREE_COUNT; i++)
		CHECK(add_one(fixture, &tree[reverse ? TREE_COUNT - 1 - i : i],
		    NULL) == FSIMG_OK);
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
	if (sink->fail_write)
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
memory_io(fixture_t *fixture, memory_sink_t *sink, bool writable,
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
	return (fsimg_io_from_callbacks(fixture->ctx, &ops, sink, out, NULL));
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
		status = memory_io(fixture, sink, true, &output);
		if (status == FSIMG_OK)
			status = fsimg_writer_emit(fixture->writer, output, &detail);
		fsimg_io_free(output);
	}
	/* Report an unexpected failure where the caller cannot see it. */
	if (error != NULL)
		*error = detail;
	else if (status != FSIMG_OK && status != FSIMG_ENOMEM &&
	    status != FSIMG_ECANCELLED && status != FSIMG_EIO)
		fprintf(stderr, "emit: %d %s (resource %d)\n", status,
		    detail.message, (int)detail.resource);
	return (status);
}

static void
expect_defaults(chk_expect_t *expect, const fsimg_write_options_t *options)
{
	bzero(expect, sizeof(*expect));
	bcopy(options->volume_uuid, expect->uuid, sizeof(expect->uuid));
	expect->reserve = options->reserve_percent;
	expect->time = (uint32_t)options->epoch;
	expect->big = options->byte_order == FSIMG_BYTE_ORDER_BIG;
}

/* Check every fixture entry in the checker's namespace: its kind, size and
 * content digest, and that nothing else is there but the root and .boot.
 */
static void
check_tree_entries(const chk_t *c)
{
	const chk_entry_t *e;
	uint16_t type;

	CHECK(c->count == TREE_COUNT + 2);
	for (size_t i = 0; i < TREE_COUNT; i++) {
		e = chk_find(c, tree[i].path);
		CHECK(e != NULL);
		type = e->mode & 0170000;
		switch (tree[i].kind) {
		case FSIMG_ENTRY_DIRECTORY:
			CHECK(type == T_DIR && (e->mode & 07777) == 0755);
			continue;
		case FSIMG_ENTRY_FIFO:
			CHECK(type == T_FIFO && (e->mode & 07777) == 0777);
			break;
		case FSIMG_ENTRY_SYMLINK:
			CHECK(type == T_LINK && (e->mode & 07777) == 0777);
			break;
		default:
			CHECK(type == T_FILE && (e->mode & 07777) == 0644);
			break;
		}
		CHECK(e->size == tree[i].size);
		CHECK(e->digest == expected_digest(tree[i].source, tree[i].size));
		CHECK(e->uid == 0 && e->gid == 0);
	}
	e = chk_find(c, "/.boot");
	CHECK(e != NULL && e->inode == 2 && e->mode == (T_DIR | 0700));
}

/* Read the image back through fsimg: every entry's content, then a payload
 * verification.
 */
static void
check_reader(fixture_t *fixture, memory_sink_t *sink, uint64_t expected)
{
	fsimg_reader_t *reader;
	fsimg_io_t *input;
	const fsimg_entry_t *entry;
	fsimg_verify_report_t report;
	unsigned char buffer[4096];
	uint64_t offset, count, source;
	size_t length;
	fsimg_status_t status;

	CHECK(memory_io(fixture, sink, false, &input) == FSIMG_OK);
	CHECK(fsimg_reader_open(fixture->ctx, input, NULL, &reader, NULL) == FSIMG_OK);
	count = 0;
	while ((status = fsimg_reader_next(reader, &entry, NULL)) == FSIMG_OK) {
		count++;
		source = UINT64_MAX;
		for (size_t i = 0; i < TREE_COUNT; i++)
			if (strcmp(tree[i].path, entry->path) == 0)
				source = tree[i].source;
		if (entry->kind != FSIMG_ENTRY_FILE && entry->kind != FSIMG_ENTRY_SYMLINK)
			continue;
		for (offset = 0; offset < entry->size; offset += length) {
			CHECK(fsimg_reader_read_at(reader, offset, buffer,
			    sizeof(buffer), &length, NULL) == FSIMG_OK);
			CHECK(length > 0);
			for (size_t i = 0; source != UINT64_MAX && i < length; i++)
				CHECK(buffer[i] == (source == 0 ? 0 :
				    content_byte(source, offset + i)));
		}
	}
	CHECK(status == FSIMG_DONE);
	CHECK(count == expected);
	fsimg_reader_free(reader);
	CHECK(fsimg_reader_open(fixture->ctx, input, NULL, &reader, NULL) == FSIMG_OK);
	CHECK(fsimg_verify(reader, FSIMG_VERIFY_PAYLOAD, &report, NULL) == FSIMG_OK);
	CHECK(report.complete);
	fsimg_reader_free(reader);
	fsimg_io_free(input);
}

/* Every block size in both byte orders: the independent checker and the
 * reader agree with the fixture, and the bytes do not depend on catalog
 * order, metadata cache size, scratch placement or the time zone.
 */
static void
test_matrix(void)
{
	static const uint32_t blocks[] = { 512, 1024, 2048, 4096 };
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sinks[3];
	chk_expect_t expect;
	chk_t c;

	for (unsigned b = 0; b < 4; b++) {
		for (unsigned order = 0; order < 2; order++) {
			fsimg_write_options_init(&options);
			options.flags |= FSIMG_WRITE_REPRODUCIBLE;
			options.epoch = 1700000000;
			options.allocation_unit_bytes = blocks[b];
			options.byte_order = order == 0 ? FSIMG_BYTE_ORDER_LITTLE :
			    FSIMG_BYTE_ORDER_BIG;
			bcopy(test_uuid, options.volume_uuid, 16);
			expect_defaults(&expect, &options);
			for (unsigned pass = 0; pass < 3; pass++) {
				if (pass == 1) {
					CHECK(setenv("TZ", "America/New_York", 1) == 0);
					tzset();
				}
				fixture_open_with(&fixture, "qnx6", &options,
				    pass == 0 ? 1 : 1024, pass != 2);
				add_tree(&fixture, pass == 1);
				CHECK(emit_memory(&fixture, &sinks[pass], NULL) == FSIMG_OK);
				CHECK(fixture.losses == 0);
				chk_image(&c, sinks[pass].bytes, sinks[pass].length, &expect);
				CHECK(c.errors == 0);
				CHECK(c.block == blocks[b]);
				check_tree_entries(&c);
				chk_minimal(&c, 3);
				chk_free(&c);
				check_reader(&fixture, &sinks[pass], TREE_COUNT + 1);
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
	}
}

/* An empty catalog: the root and .boot only, the native formatter's ladder
 * capacity, N a multiple of 64 and the smallest that fits.
 */
static void
test_empty(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	chk_expect_t expect;
	chk_t c;

	fsimg_write_options_init(&options);
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	expect_defaults(&expect, &options);
	fixture_open(&fixture, &options);
	CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
	chk_image(&c, sink.bytes, sink.length, &expect);
	CHECK(c.errors == 0);
	CHECK(c.count == 2 && c.block == 1024 && c.blocks % 64 == 0);
	CHECK(c.capacity == c.blocks / 8);
	CHECK(chk_find(&c, "/.boot") != NULL);
	chk_minimal(&c, 3);
	chk_free(&c);
	fixture_close(&fixture);
	free(sink.bytes);
}

/* An exact size: N follows from the size alone, the tail past the last
 * 4096-byte boundary is zero, and a size that cannot hold the content or a
 * size below the profile minimum fails.
 */
static void
test_exact_size(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	chk_expect_t expect;
	chk_t c;
	fsimg_error_t error;
	const fsimg_plan_t *plan;
	uint64_t volume;

	volume = UINT64_C(4) * 1024 * 1024 + 3 * 512;
	fsimg_write_options_init(&options);
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	options.volume_bytes = volume;
	options.allocation_unit_bytes = 2048;
	expect_defaults(&expect, &options);
	fixture_open(&fixture, &options);
	add_tree(&fixture, false);
	CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
	CHECK(sink.length == volume);
	chk_image(&c, sink.bytes, sink.length, &expect);
	CHECK(c.errors == 0);
	CHECK(c.blocks == (volume / 4096 * 4096 - 16384) / 2048);
	chk_free(&c);
	fixture_close(&fixture);
	free(sink.bytes);

	/* N = 67: the last bitmap byte mixes blocks below and past N. */
	options.volume_bytes = 16384 + 4096 * 67;
	options.allocation_unit_bytes = 4096;
	fixture_open(&fixture, &options);
	CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
	chk_image(&c, sink.bytes, sink.length, &expect);
	CHECK(c.errors == 0 && c.blocks == 67);
	chk_free(&c);
	fixture_close(&fixture);
	free(sink.bytes);

	/* The content needs more than 64 blocks of 512 bytes. */
	options.volume_bytes = 16384 + 64 * 512;
	options.allocation_unit_bytes = 512;
	fixture_open(&fixture, &options);
	add_tree(&fixture, false);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, &error) == FSIMG_ERANGE);
	fixture_close(&fixture);
}

/* Trees above depth 0 everywhere the default matrix keeps them flat, at
 * B = 512 in both byte orders: a 22-block directory, a long-name stream of
 * 40 records, a depth-2 file and a depth-2 inode table. Reverse catalog
 * order gives the same bytes.
 */
static void
test_trees(void)
{
	static const spec_t wide = { "/wide", FSIMG_ENTRY_DIRECTORY, 0, 0 };
	static const spec_t file = { "/mid.bin", FSIMG_ENTRY_FILE,
	    1200 * 1024, 8 };
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sinks[2];
	chk_expect_t expect;
	const chk_entry_t *e;
	spec_t spec;
	char path[64];
	chk_t c;

	for (unsigned order = 0; order < 2; order++) {
		fsimg_write_options_init(&options);
		options.flags |= FSIMG_WRITE_REPRODUCIBLE;
		options.allocation_unit_bytes = 512;
		options.inode_capacity = 10000;
		options.byte_order = order == 0 ? FSIMG_BYTE_ORDER_LITTLE :
		    FSIMG_BYTE_ORDER_BIG;
		expect_defaults(&expect, &options);
		for (unsigned pass = 0; pass < 2; pass++) {
			fixture_open(&fixture, &options);
			if (pass == 0) {
				CHECK(add_one(&fixture, &wide, NULL) == FSIMG_OK);
				CHECK(add_one(&fixture, &file, NULL) == FSIMG_OK);
			}
			for (unsigned i = 0; i < 340; i++) {
				unsigned k = pass == 0 ? i : 339 - i;

				if (k < 300)
					snprintf(path, sizeof(path), "/wide/f%03u", k);
				else
					snprintf(path, sizeof(path),
					    "/wide/a-name-longer-than-27-bytes-%03u", k);
				spec = (spec_t){ path, FSIMG_ENTRY_FILE, k % 3, 2 };
				CHECK(add_one(&fixture, &spec, NULL) == FSIMG_OK);
			}
			if (pass == 1) {
				CHECK(add_one(&fixture, &file, NULL) == FSIMG_OK);
				CHECK(add_one(&fixture, &wide, NULL) == FSIMG_OK);
			}
			CHECK(emit_memory(&fixture, &sinks[pass], NULL) == FSIMG_OK);
			chk_image(&c, sinks[pass].bytes, sinks[pass].length, &expect);
			CHECK(c.errors == 0 && c.count == 344);
			CHECK(c.capacity == 10000 && c.names_leaves == 40);
			e = chk_find(&c, "/mid.bin");
			CHECK(e != NULL && e->digest == expected_digest(8, 1200 * 1024));
			e = chk_find(&c, "/wide/a-name-longer-than-27-bytes-339");
			CHECK(e != NULL && e->digest == expected_digest(2, 0));
			chk_free(&c);
			check_reader(&fixture, &sinks[pass], 343);
			fixture_close(&fixture);
		}
		CHECK(sinks[0].length == sinks[1].length);
		CHECK(bcmp(sinks[0].bytes, sinks[1].bytes,
		    (size_t)sinks[0].length) == 0);
		free(sinks[0].bytes);
		free(sinks[1].bytes);
	}
}

/* Settings outside the profile fail writer construction, and every other
 * format rejects a QNX6 setting that is not its default.
 */
static void
test_validation(void)
{
	fsimg_write_options_t options;
	fsimg_ctx_t *ctx;
	fsimg_writer_t *writer;
	fsimg_options_t ctx_options;
	static const char *const others[] = { "fat12", "fat16", "fat32", "exfat" };
	fsimg_status_t status;

	fsimg_options_init(&ctx_options);
	CHECK(fsimg_ctx_new(&ctx_options, &ctx, NULL) == FSIMG_OK);
	for (unsigned k = 0; k < 13; k++) {
		fsimg_write_options_init(&options);
		switch (k) {
		case 0: options.sector_bytes = 4096; break;
		case 1: options.allocation_unit_bytes = 8192; break;
		case 2: options.allocation_unit_bytes = 256; break;
		case 3: options.label = "LABEL"; break;
		case 4: options.fat_root_entries = 16; break;
		case 5: options.volume_bytes = 16384 + 64 * 1024 - 512; break;
		case 6: options.volume_bytes = 1024 * 1024 + 100; break;
		case 7: options.byte_order = (fsimg_byte_order_t)2; break;
		case 8: options.reserve_percent = 101; break;
		case 9:
			options.flags |= FSIMG_WRITE_REPRODUCIBLE;
			options.epoch = -1;
			break;
		case 10:
			options.flags |= FSIMG_WRITE_REPRODUCIBLE;
			options.epoch = (int64_t)UINT32_MAX + 1;
			break;
		case 11:
			options.allocation_unit_bytes = 512;
			options.volume_bytes = 16384 + 64 * 512;
			break;
		default:
			options.volume_bytes = (uint64_t)INT64_MAX + 1;
			break;
		}
		writer = NULL;
		if (k == 11) {
			CHECK(fsimg_writer_new(ctx, "qnx6", &options, &writer,
			    NULL) == FSIMG_OK);
			fsimg_writer_free(writer);
			continue;
		}
		/* The common capability check refuses a label first. */
		status = fsimg_writer_new(ctx, "qnx6", &options, &writer, NULL);
		if (status != (k == 3 ? FSIMG_EUNSUPPORTED : FSIMG_EINVAL))
			fprintf(stderr, "validation case %u: %d\n", k, status);
		CHECK(status == (k == 3 ? FSIMG_EUNSUPPORTED : FSIMG_EINVAL));
		CHECK(writer == NULL);
	}
	for (unsigned f = 0; f < 4; f++) {
		for (unsigned k = 0; k < 4; k++) {
			fsimg_write_options_init(&options);
			switch (k) {
			case 0: options.byte_order = FSIMG_BYTE_ORDER_BIG; break;
			case 1: options.inode_capacity = 64; break;
			case 2: options.reserve_percent = 0; break;
			default: options.volume_uuid[15] = 1; break;
			}
			CHECK(fsimg_writer_new(ctx, others[f], &options, &writer,
			    NULL) == FSIMG_EINVAL);
		}
	}
	fsimg_ctx_free(ctx);
}

/* Names: a control byte fails as outside the name profile; a name of more
 * than 510 bytes never reaches the plan.
 */
static void
test_names(void)
{
	fixture_t fixture;
	const fsimg_plan_t *plan;
	fsimg_error_t error;
	spec_t spec = { "/a\001b", FSIMG_ENTRY_FILE, 0, 0 };
	char path[600];
	fsimg_status_t status;

	fixture_open(&fixture, NULL);
	status = add_one(&fixture, &spec, NULL);
	if (status == FSIMG_OK) {
		CHECK(fsimg_writer_plan(fixture.writer, &plan, &error) == FSIMG_EINVAL);
	} else {
		CHECK(status == FSIMG_EINVAL);
	}
	fixture_close(&fixture);

	path[0] = '/';
	memset(path + 1, 'n', 511);
	path[512] = '\0';
	spec.path = path;
	fixture_open(&fixture, NULL);
	status = add_one(&fixture, &spec, NULL);
	if (status == FSIMG_OK)
		CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) < FSIMG_OK);
	else
		CHECK(status < FSIMG_OK);
	fixture_close(&fixture);
}

/* .boot: a catalog directory keeps its children and takes inode 2 and the
 * profile mode; another mode is a loss or a failure; a non-directory fails.
 */
static void
test_boot(void)
{
	fsimg_write_options_t options;
	fsimg_metadata_t metadata;
	fixture_t fixture;
	memory_sink_t sink;
	chk_expect_t expect;
	const fsimg_plan_t *plan;
	const chk_entry_t *e;
	chk_t c;
	static const spec_t boot = { "/.boot", FSIMG_ENTRY_DIRECTORY, 0, 0 };
	static const spec_t child = { "/.boot/loader", FSIMG_ENTRY_FILE, 77, 2 };
	static const spec_t file = { "/.boot", FSIMG_ENTRY_FILE, 1, 2 };
	static const spec_t early = { "/-a", FSIMG_ENTRY_FILE, 1, 2 };

	fsimg_write_options_init(&options);
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	expect_defaults(&expect, &options);
	for (unsigned pass = 0; pass < 4; pass++) {
		bzero(&metadata, sizeof(metadata));
		metadata.valid = FSIMG_META_MODE;
		metadata.mode = pass == 0 ? 0700 : 0755;
		metadata.required = pass == 3 ? FSIMG_META_MODE : 0;
		options.flags = FSIMG_WRITE_REPRODUCIBLE |
		    (pass == 2 ? 0 : FSIMG_WRITE_ALLOW_LOSS);
		fixture_open(&fixture, &options);
		CHECK(add_one(&fixture, &child, NULL) == FSIMG_OK);
		CHECK(add_one(&fixture, &early, NULL) == FSIMG_OK);
		CHECK(add_one(&fixture, &boot, &metadata) == FSIMG_OK);
		if (pass >= 2) {
			CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) ==
			    FSIMG_EUNSUPPORTED);
			fixture_close(&fixture);
			continue;
		}
		CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
		CHECK(fixture.losses == pass);
		chk_image(&c, sink.bytes, sink.length, &expect);
		CHECK(c.errors == 0 && c.count == 4);
		e = chk_find(&c, "/.boot");
		CHECK(e != NULL && e->inode == 2 && e->mode == (T_DIR | 0700));
		e = chk_find(&c, "/.boot/loader");
		CHECK(e != NULL && e->digest == expected_digest(2, 77));
		chk_free(&c);
		free(sink.bytes);
		fixture_close(&fixture);
	}
	fixture_open(&fixture, NULL);
	CHECK(add_one(&fixture, &file, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_EUNSUPPORTED);
	fixture_close(&fixture);
}

/* Metadata: owner, mode and whole-second UTC times are stored exactly;
 * absent times take the format time; a sub-second, negative or offset time
 * is a loss under ALLOW_LOSS and a failure otherwise or when required.
 */
static void
test_metadata(void)
{
	static const spec_t file = { "/meta", FSIMG_ENTRY_FILE, 5, 2 };
	fsimg_write_options_t options;
	fsimg_metadata_t metadata;
	fixture_t fixture;
	memory_sink_t sink;
	chk_expect_t expect;
	const fsimg_plan_t *plan;
	const chk_entry_t *e;
	chk_t c;
	time_t before, after;

	for (unsigned pass = 0; pass < 7; pass++) {
		bzero(&metadata, sizeof(metadata));
		metadata.valid = FSIMG_META_OWNER | FSIMG_META_MODE |
		    FSIMG_META_MODIFIED | FSIMG_META_ACCESSED;
		metadata.uid = 1000;
		metadata.gid = 4294967295u;
		metadata.mode = 04751;
		metadata.modified = (fsimg_timestamp_t){ .seconds = 1700000001,
		    .precision_ns = 1000000000, .basis = FSIMG_TIME_UTC };
		metadata.accessed = (fsimg_timestamp_t){ .seconds = 4294967295,
		    .precision_ns = 1000000000, .basis = FSIMG_TIME_UTC };
		fsimg_write_options_init(&options);
		switch (pass) {
		case 1:
			metadata.modified.nanoseconds = 5;
			break;
		case 2:
			metadata.modified.seconds = -5;
			break;
		case 3:
			metadata.modified.basis = FSIMG_TIME_WALL;
			break;
		case 4:
			metadata.modified.nanoseconds = 5;
			options.flags = 0;
			break;
		case 5:
			metadata.modified.nanoseconds = 5;
			metadata.required = FSIMG_META_MODIFIED;
			break;
		case 6:
			/* Reproducible: the epoch replaces every time. */
			options.flags |= FSIMG_WRITE_REPRODUCIBLE;
			options.epoch = 1700000001;
			metadata.valid &= ~(uint32_t)FSIMG_META_ACCESSED;
			break;
		default:
			break;
		}
		fixture_open(&fixture, &options);
		CHECK(add_one(&fixture, &file, &metadata) == FSIMG_OK);
		before = time(NULL);
		if (pass == 4 || pass == 5) {
			CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) ==
			    FSIMG_EUNSUPPORTED);
			fixture_close(&fixture);
			continue;
		}
		CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
		after = time(NULL);
		CHECK(fixture.losses == (pass >= 1 && pass <= 3 ? 1 : 0));
		/* Without an epoch the format time is the planning clock. */
		expect_defaults(&expect, &options);
		if (pass != 6) {
			chk_t probe = { .big = false };

			expect.time = get32(&probe, sink.bytes + 8192 + 16);
			CHECK(expect.time >= (uint32_t)before &&
			    expect.time <= (uint32_t)after);
		}
		chk_image(&c, sink.bytes, sink.length, &expect);
		CHECK(c.errors == 0);
		e = chk_find(&c, "/meta");
		CHECK(e != NULL && e->uid == 1000 && e->gid == 4294967295u);
		CHECK(e->mode == (T_FILE | 04751));
		CHECK(e->times[0] == expect.time && e->times[3] == expect.time);
		CHECK(e->times[1] == (pass == 0 || pass == 1 || pass == 3 ||
		    pass == 6 ? 1700000001u : expect.time));
		CHECK(e->times[2] == (pass == 6 ? expect.time : 4294967295u));
		e = chk_find(&c, "/");
		CHECK(e->times[0] == expect.time && e->uid == 0);
		chk_free(&c);
		free(sink.bytes);
		fixture_close(&fixture);
	}
}

/* A provider's ranges never reach a link: its target is the checked bytes
 * read at opening, one data range.
 */
static void
test_link_range(void)
{
	static const spec_t link = { "/link", FSIMG_ENTRY_SYMLINK, 9, 3 };
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	chk_expect_t expect;
	const chk_entry_t *e;
	chk_t c;

	fsimg_write_options_init(&options);
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	expect_defaults(&expect, &options);
	ranges_enabled = true;
	fixture_open(&fixture, &options);
	ranges_enabled = false;
	CHECK(add_one(&fixture, &link, NULL) == FSIMG_OK);
	CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
	fixture_close(&fixture);
	chk_image(&c, sink.bytes, sink.length, &expect);
	e = chk_find(&c, "/link");
	CHECK(c.errors == 0 && e != NULL && e->digest == expected_digest(3, 9));
	chk_free(&c);
	free(sink.bytes);
}

/* The checker itself: one wrong byte in a free block, the twin region, the
 * header, an unused root slot or the bitmap must each be reported.
 */
static void
test_checker(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	chk_expect_t expect;
	chk_t c;
	uint64_t offsets[5];
	uint32_t blocks;
	unsigned char saved;

	fsimg_write_options_init(&options);
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	expect_defaults(&expect, &options);
	fixture_open(&fixture, &options);
	add_tree(&fixture, false);
	CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
	fixture_close(&fixture);
	chk_image(&c, sink.bytes, sink.length, &expect);
	CHECK(c.errors == 0 && c.allocated < c.blocks);
	offsets[0] = D_ORIGIN + (uint64_t)(c.blocks - 1) * c.block + 7;
	offsets[1] = D_ORIGIN + (uint64_t)c.reservation * c.block;
	offsets[2] = 8192 + 100;
	offsets[3] = D_ORIGIN + (uint64_t)(2 * c.reservation + 1) * c.block - 1;
	offsets[4] = D_ORIGIN + (c.blocks - 1) / 8;
	blocks = c.blocks;
	chk_free(&c);
	for (unsigned k = 0; k < 5; k++) {
		saved = sink.bytes[offsets[k]];
		sink.bytes[offsets[k]] ^= k == 4 ? 1u << ((blocks - 1) % 8) : 0x40;
		fprintf(stderr, "checker mutation %u (expected reports follow):\n", k);
		chk_image(&c, sink.bytes, sink.length, &expect);
		CHECK(c.errors != 0);
		chk_free(&c);
		sink.bytes[offsets[k]] = saved;
	}
	free(sink.bytes);
}

/* A provider's symbolic-link target is checked once in the common source
 * layer: a NUL byte fails before any driver stores it.
 */
static void
test_link_nul(void)
{
	static const spec_t link = { "/bad", FSIMG_ENTRY_SYMLINK, 3, 9 };
	fixture_t fixture;
	memory_sink_t sink;
	fsimg_error_t error;

	fixture_open(&fixture, NULL);
	CHECK(add_one(&fixture, &link, NULL) == FSIMG_OK);
	CHECK(emit_memory(&fixture, &sink, &error) == FSIMG_EINVAL);
	CHECK(strstr(error.message, "NUL") != NULL);
	free(sink.bytes);
	fixture_close(&fixture);
}

/* Sizing: an explicit inode capacity below the entries fails; one above
 * them is rounded to a whole table block; reserve and free space raise N.
 */
static void
test_sizing(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	chk_expect_t expect;
	const fsimg_plan_t *plan;
	chk_t c;
	uint32_t plain = 0;

	fsimg_write_options_init(&options);
	options.inode_capacity = TREE_COUNT;
	fixture_open(&fixture, &options);
	add_tree(&fixture, false);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_ERANGE);
	fixture_close(&fixture);

	for (unsigned pass = 0; pass < 4; pass++) {
		fsimg_write_options_init(&options);
		options.flags |= FSIMG_WRITE_REPRODUCIBLE;
		if (pass == 1)
			options.inode_capacity = 1001;
		if (pass == 2)
			options.reserve_percent = 50;
		if (pass == 3)
			options.min_free_bytes = 10 * 1024 * 1024;
		expect_defaults(&expect, &options);
		fixture_open(&fixture, &options);
		add_tree(&fixture, false);
		CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
		chk_image(&c, sink.bytes, sink.length, &expect);
		CHECK(c.errors == 0);
		CHECK(c.blocks % 64 == 0);
		if (pass == 0)
			plain = c.blocks;
		if (pass == 1)
			CHECK(c.capacity == 1008);
		if (pass == 2)
			CHECK(c.blocks > plain && c.blocks - c.allocated >=
			    c.blocks / 2);
		if (pass == 3)
			CHECK(c.blocks - c.allocated >= 10240 + c.blocks * 3 / 100);
		chk_free(&c);
		free(sink.bytes);
		fixture_close(&fixture);
	}
}

/* Failures: a sink of the wrong size, a durable request the sink cannot
 * honour, write and flush failures, a changed source and a source that ends
 * early leave the writer failed and every resource released; short writes
 * still succeed.
 */
static void
test_faults(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink, reference;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_io_t *output;
	fsimg_error_t error;

	fsimg_write_options_init(&options);
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	fixture_open(&fixture, &options);
	add_tree(&fixture, false);
	CHECK(emit_memory(&fixture, &reference, NULL) == FSIMG_OK);
	fixture_close(&fixture);
	for (unsigned scenario = 0; scenario < 7; scenario++) {
		bzero(&sink, sizeof(sink));
		if (scenario == 4)
			options.flags |= FSIMG_WRITE_DURABLE;
		fixture_open(&fixture, &options);
		options.flags &= ~(uint32_t)FSIMG_WRITE_DURABLE;
		add_tree(&fixture, false);
		CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
		CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
		sink.length = info.volume_bytes + (scenario == 0 ? 512 : 0);
		sink.bytes = calloc(1, (size_t)sink.length);
		CHECK(sink.bytes != NULL);
		CHECK(memory_io(&fixture, &sink, true, &output) == FSIMG_OK);
		sink.fail_write = scenario == 1;
		sink.fail_flush = scenario == 2;
		fixture.changed = scenario == 3;
		fixture.eof = scenario == 6;
		sink.max_write = scenario == 5 ? 700 : 0;
		if (scenario == 5) {
			CHECK(fsimg_writer_emit(fixture.writer, output, NULL) == FSIMG_OK);
			CHECK(sink.length == reference.length &&
			    bcmp(sink.bytes, reference.bytes, (size_t)sink.length) == 0);
		} else {
			CHECK(fsimg_writer_emit(fixture.writer, output, &error) < FSIMG_OK);
			if (scenario == 0 || scenario == 4)
				CHECK(sink.writes == 0);
			else
				CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
		}
		CHECK(fsimg_writer_emit(fixture.writer, output, NULL) == FSIMG_ESTATE);
		fsimg_io_free(output);
		free(sink.bytes);
		fixture_close(&fixture);
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

	fsimg_write_options_init(&options);
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	succeeded = false;
	for (uint64_t attempt = 1; attempt < 4096 && !succeeded; attempt++) {
		fixture_open(&fixture, &options);
		add_tree(&fixture, false);
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
		for (uint64_t attempt = 1; attempt < 4096 && !succeeded; attempt++) {
			fixture_open_with(&fixture, "qnx6", &options, 1, true);
			add_tree(&fixture, false);
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
	succeeded = false;
	/* Cancel at polls spaced about 6% apart over the whole run. */
	for (uint64_t attempt = 1; attempt < 100000 && !succeeded;
	    attempt += attempt / 16 + 1) {
		fixture_open(&fixture, &options);
		add_tree(&fixture, false);
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

/* The fixture tree published through write_at as DIRECTORY/writer.img, for
 * the native qualification in tools/qnx/guest/qualify.sh: the byte order,
 * block size, exact volume size and optionally the UUID as 32 hexadecimal
 * digits are given, the epoch is the default.
 */
static void
create_external(const char *directory, const char *order, const char *block,
    const char *volume, const char *uuid)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	unsigned value;
	int parent;

	fsimg_write_options_init(&options);
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	CHECK(strcmp(order, "big") == 0 || strcmp(order, "little") == 0);
	options.byte_order = strcmp(order, "big") == 0 ? FSIMG_BYTE_ORDER_BIG :
	    FSIMG_BYTE_ORDER_LITTLE;
	options.allocation_unit_bytes = (uint32_t)strtoul(block, NULL, 10);
	options.volume_bytes = strtoull(volume, NULL, 10);
	CHECK(uuid == NULL || strlen(uuid) == 32);
	for (unsigned i = 0; uuid != NULL && i < 16; i++) {
		CHECK(sscanf(uuid + 2 * i, "%2x", &value) == 1);
		options.volume_uuid[i] = (unsigned char)value;
	}
	fixture_open(&fixture, &options);
	add_tree(&fixture, false);
	parent = open(directory, O_RDONLY | O_DIRECTORY);
	CHECK(parent != -1);
	CHECK(fsimg_writer_write_at(fixture.writer, parent, "writer.img", NULL) ==
	    FSIMG_OK);
	CHECK(close(parent) == 0);
	fixture_close(&fixture);
}

/* Publish an image through write_at, map it and run the checker over it. */
static void
scale_case(const char *label, const fsimg_write_options_t *options,
    const spec_t *specs, size_t count, uint64_t generated)
{
	fixture_t fixture;
	fsimg_stats_t stats;
	chk_expect_t expect;
	spec_t spec;
	struct stat st;
	char path[64];
	unsigned char *image;
	chk_t c;
	int fd;

	expect_defaults(&expect, options);
	fixture_open(&fixture, options);
	for (size_t i = 0; i < count; i++)
		CHECK(add_one(&fixture, &specs[i], NULL) == FSIMG_OK);
	for (uint64_t i = 0; i < generated; i++) {
		snprintf(path, sizeof(path), "/wide/f%07" PRIu64, generated - 1 - i);
		spec = (spec_t){ path, FSIMG_ENTRY_FILE, 0, 0 };
		CHECK(add_one(&fixture, &spec, NULL) == FSIMG_OK);
	}
	CHECK(fsimg_writer_write_at(fixture.writer, fixture.parent, "scale.img",
	    NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture.ctx, &stats, NULL) == FSIMG_OK);
	fd = openat(fixture.parent, "scale.img", O_RDONLY);
	CHECK(fd != -1 && fstat(fd, &st) == 0);
	image = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	CHECK(image != MAP_FAILED);
	chk_image(&c, image, (uint64_t)st.st_size, &expect);
	CHECK(c.errors == 0);
	CHECK(c.count == count + generated + 2);
	printf("QNX6 writer %s: blocks=%" PRIu32 " inodes=%" PRIu32 " memory=%"
	    PRIu64 " scratch=%" PRIu64 " io=%" PRIu64 " disk=%" PRIu64 "\n",
	    label, c.blocks, c.capacity, stats.memory_peak, stats.scratch_peak,
	    stats.io_calls, (uint64_t)st.st_blocks * 512);
	chk_free(&c);
	CHECK(munmap(image, (size_t)st.st_size) == 0);
	CHECK(close(fd) == 0);
	CHECK(unlinkat(fixture.parent, "scale.img", 0) == 0);
	fixture_close(&fixture);
}

/* One empty volume's stored allocation-group count. The volume is a sparse
 * file; only its first header is read, after checking that N and the inode
 * capacity are the geometry the case names.
 */
typedef struct groups_case {
	uint32_t	 block;
	uint64_t	 blocks;
	uint64_t	 volume;	/* 0: exactly D + N * B plus one header area */
	uint32_t	 inodes;	/* 0: the native default ladder */
	uint32_t	 groups;
} groups_case_t;

static void
groups_case(const groups_case_t *gc)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	unsigned char header[512];
	chk_t probe;
	int fd;

	fsimg_write_options_init(&options);
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	options.allocation_unit_bytes = gc->block;
	options.volume_bytes = gc->volume != 0 ? gc->volume :
	    D_ORIGIN + gc->blocks * gc->block + 4096;
	options.inode_capacity = gc->inodes;
	fixture_open(&fixture, &options);
	CHECK(fsimg_writer_write_at(fixture.writer, fixture.parent, "groups.img",
	    NULL) == FSIMG_OK);
	fd = openat(fixture.parent, "groups.img", O_RDONLY);
	CHECK(fd != -1);
	CHECK(pread(fd, header, sizeof(header), 8192) == (ssize_t)sizeof(header));
	CHECK(close(fd) == 0);
	CHECK(unlinkat(fixture.parent, "groups.img", 0) == 0);
	fixture_close(&fixture);
	bzero(&probe, sizeof(probe));
	CHECK(get32(&probe, header + 60) == gc->blocks);
	CHECK(gc->inodes == 0 || get32(&probe, header + 52) == gc->inodes);
	if (get32(&probe, header + 68) != gc->groups) {
		printf("B=%" PRIu32 " N=%" PRIu64 " inodes=%" PRIu32 ": %" PRIu32
		    " groups, expected %" PRIu32 "\n", gc->block, gc->blocks,
		    get32(&probe, header + 52), get32(&probe, header + 68),
		    gc->groups);
		CHECK(0);
	}
}

/* The stored allocation-group count, as literal values. The first cases
 * are geometries whose count native mkqnx6fs wrote in the research record:
 * the 64 MiB disks at every block size, the -n 16384 volume, the 9830368-
 * block volume with 1048576 inodes and the head unit's 2154991104-byte
 * partition. The rest sit on each side of every ladder threshold (the
 * inode capacity sets m at B = 4096 with a large bitmap, and small volumes
 * make the bitmap the smaller tree) and every cap threshold (m = 32 through
 * a 1024-inode table). The two cases at the last cap threshold, the only
 * ones where the ladder's top rung (m = 31/32) is not capped, need 512 GiB
 * sparse volumes and run only with --scale.
 */
static void
test_groups(bool scale)
{
	static const groups_case_t cases[] = {
		{ 512, 131040, 67108864, 0, 4 },
		{ 1024, 65520, 67108864, 0, 4 },
		{ 2048, 32760, 67108864, 0, 1 },
		{ 4096, 16380, 67108864, 0, 1 },
		{ 1024, 16384, 0, 0, 1 },
		{ 512, 9830368, 0, 1048576, 16 },
		{ 1024, 2104464, UINT64_C(2154991104), 0, 8 },
		/* ladder over the inode table at cap 16: m = 3/4, 7/8, 15/16 */
		{ 4096, 0x800000, 0, 96, 1 },
		{ 4096, 0x800000, 0, 128, 4 },
		{ 4096, 0x800000, 0, 224, 4 },
		{ 4096, 0x800000, 0, 256, 8 },
		{ 4096, 0x800000, 0, 480, 8 },
		{ 4096, 0x800000, 0, 512, 16 },
		/* ladder over the bitmap: 3 and 4 leaves at B = 512 */
		{ 512, 12288, 0, 0, 1 },
		{ 512, 16384, 0, 0, 4 },
		/* caps: N on each side of 0x100000 and 0x800000 */
		{ 4096, 0xfffff, 0, 1024, 4 },
		{ 4096, 0x100000, 0, 1024, 8 },
		{ 4096, 0x7fffff, 0, 1024, 8 },
	};
	static const groups_case_t large[] = {
		{ 4096, 0x7ffffff, 0, 1024, 16 },
		{ 4096, 0x8000000, 0, 992, 16 },
		{ 4096, 0x8000000, 0, 1024, 32 },
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		groups_case(&cases[i]);
	if (scale)
		for (size_t i = 0; i < sizeof(large) / sizeof(large[0]); i++)
			groups_case(&large[i]);
}

/* Grow N, the inode capacity, one directory's width, a file's depth and a
 * file past 4 GiB separately. Zero content stays sparse in the published
 * image.
 */
static void
test_scale(void)
{
	static const spec_t wide = { "/wide", FSIMG_ENTRY_DIRECTORY, 0, 0 };
	static const spec_t deep = { "/deep", FSIMG_ENTRY_FILE,
	    UINT64_C(130) * 1024 * 1024, 0 };
	static const spec_t huge = { "/huge", FSIMG_ENTRY_FILE,
	    UINT64_C(4) * 1024 * 1024 * 1024 + 4097, 0 };
	fsimg_write_options_t options;

	fsimg_write_options_init(&options);
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	scale_case("wide directory 100000", &options, &wide, 1, 100000);
	options.inode_capacity = 4000000;
	scale_case("inode capacity 4000000", &options, NULL, 0, 0);
	options.inode_capacity = 0;
	options.allocation_unit_bytes = 512;
	scale_case("depth-3 file", &options, &deep, 1, 0);
	options.volume_bytes = UINT64_C(8) * 1024 * 1024 * 1024;
	scale_case("8 GiB volume of 512-byte blocks", &options, NULL, 0, 0);
	options.volume_bytes = 0;
	options.allocation_unit_bytes = 4096;
	scale_case("file past 4 GiB", &options, &huge, 1, 0);
}

/* Compare an empty volume formatted natively (mkqnx6fs with long-name
 * checksums) with fsimg's empty volume of the same size, block size, byte
 * order, UUID, reserve and format time. Every byte must match except the
 * header CRC and the sync time, which the native formatter may set after
 * the format time.
 */
static int
compare_native(const char *path)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	struct stat st;
	unsigned char *native;
	chk_t probe;
	uint64_t second, offset, differences, end;
	uint32_t allocated;
	int fd;

	fd = open(path, O_RDONLY);
	CHECK(fd != -1 && fstat(fd, &st) == 0 && st.st_size > 16384);
	native = malloc((size_t)st.st_size);
	CHECK(native != NULL);
	CHECK(pread(fd, native, (size_t)st.st_size, 0) == st.st_size);
	CHECK(close(fd) == 0);
	bzero(&probe, sizeof(probe));
	probe.big = native[8192] == 0x68;
	fsimg_write_options_init(&options);
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	options.epoch = get32(&probe, native + 8192 + 16);
	options.volume_bytes = (uint64_t)st.st_size;
	options.allocation_unit_bytes = get32(&probe, native + 8192 + 48);
	options.byte_order = probe.big ? FSIMG_BYTE_ORDER_BIG : FSIMG_BYTE_ORDER_LITTLE;
	options.reserve_percent = get16(&probe, native + 8192 + 30);
	bcopy(native + 8192 + 32, options.volume_uuid, 16);
	fixture_open(&fixture, &options);
	CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
	fixture_close(&fixture);
	CHECK(sink.length == (uint64_t)st.st_size);
	/* The native formatter leaves the boot area to a loader and does not
	 * clear free blocks, so only the two headers and the allocated blocks
	 * [0, A) are compared.
	 */
	second = sink.length / 4096 * 4096 - 4096;
	allocated = get32(&probe, native + 8192 + 60) -
	    get32(&probe, native + 8192 + 64);
	end = D_ORIGIN + (uint64_t)allocated * options.allocation_unit_bytes;
	differences = 0;
	for (uint64_t i = 8192; i < sink.length; i++) {
		if (i == 8192 + 512)
			i = D_ORIGIN;
		if (i == end)
			i = second;
		if (i == second + 512)
			break;
		if (sink.bytes[i] == native[i])
			continue;
		offset = i >= second ? i - second : i - 8192;
		if (i < D_ORIGIN || i >= second) {
			if ((offset >= 4 && offset < 8) ||
			    (offset >= 20 && offset < 24))
				continue;
		}
		if (differences++ < 10)
			printf("%s: byte %" PRIu64 " is %02x, native %02x\n",
			    path, i, sink.bytes[i], native[i]);
	}
	printf("%s: %" PRIu64 " differing bytes outside the CRC and sync "
	    "time\n", path, differences);
	free(sink.bytes);
	free(native);
	return (differences == 0 ? 0 : 1);
}

int
main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--native") == 0)
		return (compare_native(argv[2]));
	if (argc == 2 && strcmp(argv[1], "--scale") == 0) {
		test_scale();
		test_groups(true);
		return (0);
	}
	if ((argc == 6 || argc == 7) && strcmp(argv[1], "--create") == 0) {
		create_external(argv[2], argv[3], argv[4], argv[5],
		    argc == 7 ? argv[6] : NULL);
		return (0);
	}
	CHECK(argc == 1);
	test_checker();
	test_link_nul();
	test_link_range();
	test_matrix();
	test_empty();
	test_exact_size();
	test_trees();
	test_validation();
	test_names();
	test_boot();
	test_metadata();
	test_sizing();
	test_groups(false);
	test_faults();
	test_injected();
	return (0);
}
