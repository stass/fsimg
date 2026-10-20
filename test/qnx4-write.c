/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>

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
#include "host.h"
#include "internal.h"
#include "meta.h"
#include "test.h"

/*
 * QNX4 writer tests. Images are checked twice: by fsimg's own reader and
 * verifier, and by the independent checker below, written from the format
 * design with its own offsets. The checker decodes the root block, the
 * bitmap, every directory slot, .inodes, .longfilenames and lost+found,
 * requires the writer profile's layout -- every object one extent,
 * allocation one prefix of blocks, the native statuses and modes -- and
 * marks every byte the layout gives a meaning. Every other byte of the image
 * must be zero over a sink poisoned with 0xa5, so a stray or stale byte
 * anywhere fails the test.
 */
#define BLOCK		512u
#define SLOT		64u
#define T_TYPE		0170000u
#define T_FILE		0100000u
#define T_DIR		0040000u
#define T_LINK		0120000u
#define T_FIFO		0010000u
#define ST_USED		0x01u
#define ST_MODIFIED	0x02u
#define ST_HIDDEN	0x04u
#define ST_LINK		0x08u
#define ST_TABLE	0x10u
#define ST_CLEAN	0x20u
#define POISON		0xa5u
#define PATH_BYTES	1600u
#define TARGET_BYTES	256u

/*
 * The independent checker.
 */

/* One decoded 64-byte inode record. */
typedef struct chk_record {
	uint32_t	 size;
	uint32_t	 first;
	uint32_t	 blocks;
	uint32_t	 chain;
	uint32_t	 times[4];	/* created, modified, accessed, changed */
	uint16_t	 extents;
	uint16_t	 mode;
	uint16_t	 uid;
	uint16_t	 gid;
	uint16_t	 links;
	uint8_t		 status;
	bool		 spare_zero;	/* bytes 0x3a to 0x3e */
} chk_record_t;

/* One object the checker found: its path, record fields and the POSIX
 * cksum CRC of its content.
 */
typedef struct chk_entry {
	char		 path[PATH_BYTES];
	char		 target[TARGET_BYTES];
	uint32_t	 size;
	uint32_t	 times[4];
	uint32_t	 crc;
	uint16_t	 mode;
	uint16_t	 uid;
	uint16_t	 gid;
	uint16_t	 links;
	bool		 indirect;	/* its record lives in .inodes */
} chk_entry_t;

/* What the checker requires beyond the format: the format time of the
 * format-owned records, the label, and the .inodes slot count when the
 * test knows it.
 */
typedef struct chk_expect {
	const char	*label;
	uint32_t	 time;
	uint32_t	 inode_slots;
	bool		 quiet;		/* count errors without printing them */
} chk_expect_t;

/* The checker's state. explained has one byte per image byte, set where
 * the layout gives the byte a meaning; owned one per block, indexed by the
 * 1-based block number; inode_named one per .inodes slot and long_named one
 * per .longfilenames block, each counting the names that reach it.
 */
typedef struct chk {
	const unsigned char *img;
	unsigned char	*explained;
	unsigned char	*owned;
	unsigned char	*inode_named;
	unsigned char	*long_named;
	chk_entry_t	*entries;
	uint64_t	 length;
	size_t		 count;
	size_t		 capacity;
	uint32_t	 blocks;
	uint32_t	 bitmap_blocks;
	uint32_t	 root_first;
	uint32_t	 root_blocks;
	uint32_t	 inodes_first;
	uint32_t	 inodes_blocks;
	uint32_t	 lost_found_first;
	uint32_t	 names_first;
	uint32_t	 names_blocks;
	uint32_t	 allocated;
	uint32_t	 used_inodes;
	uint32_t	 time;
	unsigned	 errors;
	unsigned	 depth;
	bool		 quiet;
} chk_t;

#define CHK(c, cond, what) do { \
	if (!(cond)) \
		chk_fail((c), (what), __LINE__); \
} while (0)

static void
chk_fail(chk_t *c, const char *what, int line)
{
	if (c->errors++ < 20 && !c->quiet)
		fprintf(stderr, "checker: %s (%s:%d)\n", what, __FILE__, line);
}

static uint16_t
le16(const unsigned char *p)
{
	return ((uint16_t)(p[0] | p[1] << 8));
}

static uint32_t
le32(const unsigned char *p)
{
	return ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	    (uint32_t)p[3] << 24);
}

static bool
all_zero(const unsigned char *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (p[i] != 0)
			return (false);
	return (true);
}

/* The POSIX cksum CRC: polynomial 0x04c11db7 over the bytes, most
 * significant bit first, then over the length's bytes, least significant
 * first, complemented.
 */
static uint32_t
crc_update(uint32_t crc, const unsigned char *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		crc ^= (uint32_t)p[i] << 24;
		for (unsigned bit = 0; bit < 8; bit++)
			crc = (crc & 0x80000000u) != 0 ? crc << 1 ^ 0x04c11db7u :
			    crc << 1;
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

static uint64_t
volume_offset(uint32_t block)
{
	return ((uint64_t)(block - 1) * BLOCK);
}

static const unsigned char *
chk_slot(const chk_t *c, uint32_t block, unsigned slot)
{
	return (c->img + volume_offset(block) + (size_t)slot * SLOT);
}

static void
chk_explain(chk_t *c, uint64_t offset, uint64_t n)
{
	CHK(c, offset <= c->length && n <= c->length - offset,
	    "explained range leaves the image");
	if (offset <= c->length && n <= c->length - offset)
		memset(c->explained + offset, 1, (size_t)n);
}

/* Claim blocks first to first + count - 1 for one owner. */
static bool
chk_claim(chk_t *c, uint32_t first, uint32_t count, const char *what)
{
	if (count == 0)
		return (true);
	if (first < 1 || first > c->blocks || count > c->blocks - first + 1) {
		CHK(c, false, what);
		return (false);
	}
	for (uint32_t b = first; b < first + count; b++) {
		CHK(c, c->owned[b] == 0, "a block has two owners");
		c->owned[b] = 1;
	}
	return (true);
}

static void
chk_decode(const unsigned char *raw, chk_record_t *r)
{
	r->size = le32(raw + 0x10);
	r->first = le32(raw + 0x14);
	r->blocks = le32(raw + 0x18);
	r->chain = le32(raw + 0x1c);
	for (unsigned i = 0; i < 4; i++)
		r->times[i] = le32(raw + 0x20 + 4 * i);
	r->extents = le16(raw + 0x30);
	r->mode = le16(raw + 0x32);
	r->uid = le16(raw + 0x34);
	r->gid = le16(raw + 0x36);
	r->links = le16(raw + 0x38);
	r->spare_zero = all_zero(raw + 0x3a, 5);
	r->status = raw[0x3f];
}

/* The length of a NUL-padded name field; *padded is whether everything
 * after the name is zero.
 */
static size_t
field_length(const unsigned char *field, size_t width, bool *padded)
{
	size_t length = strnlen((const char *)field, width);

	*padded = all_zero(field + length, width - length);
	return (length);
}

/* The writer's name profile, checked on its own: UTF-8 is left to the
 * reader, this checks the byte rules.
 */
static bool
name_allowed(const char *name, size_t length)
{
	if (length == 0 || (length == 1 && name[0] == '.') ||
	    (length == 2 && name[0] == '.' && name[1] == '.'))
		return (false);
	for (size_t i = 0; i < length; i++)
		if ((unsigned char)name[i] < 0x20 || name[i] == 0x7f ||
		    name[i] == '/')
			return (false);
	return (true);
}

static chk_entry_t *
chk_add(chk_t *c, const char *path, const chk_record_t *r, bool indirect)
{
	chk_entry_t *e, *grown;

	if (c->count == c->capacity) {
		c->capacity = c->capacity == 0 ? 64 : c->capacity * 2;
		grown = realloc(c->entries, c->capacity * sizeof(*grown));
		CHECK(grown != NULL);
		c->entries = grown;
	}
	e = &c->entries[c->count++];
	bzero(e, sizeof(*e));
	CHECK(strlen(path) < sizeof(e->path));
	strcpy(e->path, path);
	e->size = r->size;
	bcopy(r->times, e->times, sizeof(e->times));
	e->mode = r->mode;
	e->uid = r->uid;
	e->gid = r->gid;
	e->links = r->links;
	e->indirect = indirect;
	return (e);
}

/* A file's or symbolic link's content: exactly ceil(size / 512) blocks in
 * one extent, the content explained and its CRC recorded; the rest of the
 * last block is left to the zero scan.
 */
static void
chk_content(chk_t *c, const chk_record_t *r, chk_entry_t *e, const char *path)
{
	uint32_t blocks = r->size / BLOCK + (r->size % BLOCK != 0);
	const unsigned char *data;

	CHK(c, r->blocks == blocks && r->extents == (blocks != 0 ? 1 : 0) &&
	    (r->first == 0) == (blocks == 0), "content allocation is not exact");
	if (r->blocks != blocks || !chk_claim(c, r->first, r->blocks, path)) {
		e->crc = 0;
		return;
	}
	if (blocks == 0) {
		e->crc = crc_finish(0, 0);
		return;
	}
	data = c->img + volume_offset(r->first);
	chk_explain(c, volume_offset(r->first), r->size);
	e->crc = crc_finish(crc_update(0, data, r->size), r->size);
	if ((r->mode & T_TYPE) == T_LINK && r->size < TARGET_BYTES) {
		bcopy(data, e->target, r->size);
		e->target[r->size] = '\0';
	}
}

/* A 64-byte record's position: its block and its slot there. */
typedef struct chk_position {
	uint32_t	 block;
	unsigned	 slot;
} chk_position_t;

/* A directory's "." or "..": a link record of status 0x08 to target; "."
 * carries "I", 0x03, "QNX" after its NUL and its first-extent length at 8.
 */
static void
chk_dot(chk_t *c, const unsigned char *raw, bool dotdot, chk_position_t target,
    uint32_t blocks)
{
	static const unsigned char dot[8] = { '.', 0, 'I', 3, 'Q', 'N', 'X', 0 };

	if (dotdot)
		CHK(c, raw[0] == '.' && raw[1] == '.' && all_zero(raw + 2, 46),
		    "\"..\" has a wrong name field");
	else
		CHK(c, memcmp(raw, dot, sizeof(dot)) == 0 &&
		    le32(raw + 8) == blocks && all_zero(raw + 12, 36),
		    "\".\" has a wrong name field or extent length");
	CHK(c, le32(raw + 0x30) == target.block && raw[0x34] == target.slot,
	    "dot entry has a wrong target");
	CHK(c, all_zero(raw + 0x35, 10) && raw[0x3f] == ST_LINK,
	    "dot entry has a wrong pointer, spare byte or status");
}

static unsigned chk_directory(chk_t *, const char *, const chk_record_t *,
    chk_position_t, chk_position_t);

/* Check one object reached through a name, whose record is at self: its
 * kind's storage rule and the writer's status for a direct or an .inodes
 * record, then recurse into a directory, whose own record is at parent.
 */
static void
chk_object(chk_t *c, const char *path, const chk_record_t *r,
    chk_position_t self, chk_position_t parent, unsigned *subdirectories)
{
	chk_entry_t *e;
	unsigned children;
	size_t index;
	bool table = self.block >= c->inodes_first &&
	    self.block - c->inodes_first < c->inodes_blocks;
	uint8_t base = table ? (ST_TABLE | ST_USED) : ST_USED;

	CHK(c, r->chain == 0 && r->spare_zero, "record has a chain or spare bytes");
	switch (r->mode & T_TYPE) {
	case T_DIR:
		CHK(c, r->status == base, "directory status");
		CHK(c, r->blocks != 0 && r->blocks % 8 == 0 &&
		    r->size == r->blocks * BLOCK && r->extents == 1,
		    "directory storage is not whole 4096-byte steps");
		if (r->blocks == 0 || r->size != r->blocks * BLOCK ||
		    !chk_claim(c, r->first, r->blocks, path) || c->depth > 16)
			return;
		e = chk_add(c, path, r, table);
		index = c->count - 1;
		e->crc = 0;
		c->depth++;
		children = chk_directory(c, path, r, self, parent);
		c->depth--;
		CHK(c, c->entries[index].links == 2 + children,
		    "directory link count");
		(*subdirectories)++;
		return;
	case T_FILE:
		CHK(c, r->status == (base | ST_MODIFIED), "file status");
		break;
	case T_LINK:
	case T_FIFO:
		CHK(c, r->status == base, "link or FIFO status");
		break;
	default:
		CHK(c, false, "record has an unexpected kind");
		return;
	}
	CHK(c, r->links == 1, "file link count");
	e = chk_add(c, path, r, table);
	if ((r->mode & T_TYPE) == T_FIFO) {
		CHK(c, r->size == 0 && r->first == 0 && r->blocks == 0 &&
		    r->extents == 0, "FIFO has storage");
		e->crc = crc_finish(0, 0);
		return;
	}
	chk_content(c, r, e, path);
}

/* The long-name record of the link at `at`: in .longfilenames, naming the
 * link's position back, state used, a 49- to 505-byte name with zeros after
 * it and a zero last byte; the link's field is the mangled prefix.
 */
static bool
chk_long_name(chk_t *c, const unsigned char *link, chk_position_t at,
    uint32_t long_block, char *name, size_t *length)
{
	unsigned char mangled[48];
	const unsigned char *record;
	char digits[10];
	bool padded;

	if (long_block < c->names_first || c->names_blocks == 0 ||
	    long_block - c->names_first >= c->names_blocks) {
		CHK(c, false, "long-name pointer leaves .longfilenames");
		return (false);
	}
	record = c->img + volume_offset(long_block);
	CHK(c, le32(record) == at.block && record[4] == at.slot &&
	    record[5] == 0x01 && record[511] == 0,
	    "long-name record does not name its link");
	*length = field_length(record + 6, 505, &padded);
	CHK(c, padded && *length >= 49, "long-name record name");
	bcopy(record + 6, name, *length);
	name[*length] = '\0';
	bzero(mangled, sizeof(mangled));
	bcopy(record + 6, mangled, 38);
	snprintf(digits, sizeof(digits), "~%08" PRIx32, long_block);
	bcopy(digits, mangled + 38, 9);
	CHK(c, memcmp(link, mangled, sizeof(mangled)) == 0,
	    "link field is not the mangled prefix");
	CHK(c, c->long_named[long_block - c->names_first]++ == 0,
	    "long-name record is named twice");
	chk_explain(c, volume_offset(long_block), BLOCK);
	return (true);
}

/* The used slot at `at` in a directory whose own record is at dir: a
 * direct record of a 1- to 16-byte name, or a link record of a longer one
 * naming an .inodes slot.
 */
static void
chk_child(chk_t *c, const char *path, chk_position_t at,
    const unsigned char *raw, chk_position_t dir, char *previous,
    unsigned *subdirectories)
{
	char name[512], child[PATH_BYTES];
	const unsigned char *inode;
	chk_record_t r;
	chk_position_t target;
	uint32_t long_block;
	unsigned k;
	size_t length;
	bool padded;

	if ((raw[0x3f] & ST_LINK) == 0) {
		CHK(c, (raw[0x3f] & ~ST_MODIFIED) == ST_USED, "direct status");
		length = field_length(raw, 16, &padded);
		CHK(c, padded, "direct name padding");
		bcopy(raw, name, length);
		name[length] = '\0';
		chk_decode(raw, &r);
		chk_explain(c, (uint64_t)(raw - c->img), SLOT);
		target = at;
	} else {
		CHK(c, raw[0x3f] == ST_LINK, "link status");
		CHK(c, all_zero(raw + 0x39, 6), "link spare bytes");
		long_block = le32(raw + 0x35);
		if (long_block == 0) {
			length = field_length(raw, 48, &padded);
			CHK(c, padded && length > 16,
			    "link name of 16 bytes or fewer, or bad padding");
			bcopy(raw, name, length);
			name[length] = '\0';
		} else if (!chk_long_name(c, raw, at, long_block, name,
		    &length)) {
			return;
		}
		chk_explain(c, (uint64_t)(raw - c->img), SLOT);
		target.block = le32(raw + 0x30);
		target.slot = raw[0x34];
		if (target.block < c->inodes_first || target.slot > 7 ||
		    target.block - c->inodes_first >= c->inodes_blocks ||
		    (target.block == c->inodes_first && target.slot == 0)) {
			CHK(c, false, "link target is not a used .inodes slot");
			return;
		}
		k = (target.block - c->inodes_first) * 8 + target.slot;
		CHK(c, c->inode_named[k]++ == 0, ".inodes record named twice");
		inode = chk_slot(c, target.block, target.slot);
		CHK(c, all_zero(inode, 16), ".inodes record has a name");
		chk_decode(inode, &r);
		chk_explain(c, (uint64_t)(inode - c->img), SLOT);
	}
	CHK(c, name_allowed(name, length), "name outside the profile");
	CHK(c, previous[0] == '\0' || strcmp(previous, name) < 0,
	    "siblings are not in byte order");
	strcpy(previous, name);
	CHECK(strlen(path) + 1 + length < sizeof(child));
	snprintf(child, sizeof(child), "%s%s%s", path,
	    strcmp(path, "/") == 0 ? "" : "/", name);
	chk_object(c, child, &r, target, dir, subdirectories);
}

/* Root slots 2 to 6 and 8, which the formatter fixes. */
static void
chk_root_fixed(chk_t *c)
{
	static const char *const links[] = { ".inodes", ".boot", ".altboot" };
	const unsigned char *raw;
	chk_record_t r;
	bool padded;

	raw = chk_slot(c, c->root_first, 2);
	chk_decode(raw, &r);
	CHK(c, field_length(raw, 16, &padded) == 7 &&
	    memcmp(raw, ".bitmap", 7) == 0 && padded, ".bitmap name");
	CHK(c, r.size == (c->blocks + 7) / 8 && r.first == 3 &&
	    r.blocks == c->bitmap_blocks && r.extents == 1 && r.chain == 0,
	    ".bitmap storage");
	CHK(c, r.mode == (T_FILE | 0444) && r.links == 1 && r.status == ST_USED &&
	    r.uid == 0 && r.gid == 0 && r.spare_zero, ".bitmap record");
	for (unsigned i = 0; i < 4; i++)
		CHK(c, r.times[i] == c->time, ".bitmap time");
	chk_explain(c, (uint64_t)(raw - c->img), SLOT);

	for (unsigned i = 0; i < 3; i++) {
		raw = chk_slot(c, c->root_first, 3 + i);
		CHK(c, field_length(raw, 48, &padded) == strlen(links[i]) &&
		    memcmp(raw, links[i], strlen(links[i])) == 0 && padded,
		    "table link name");
		CHK(c, le32(raw + 0x30) == 2 && raw[0x34] == 1 + i &&
		    all_zero(raw + 0x35, 10) && raw[0x3f] == ST_LINK,
		    "table link target");
		chk_explain(c, (uint64_t)(raw - c->img), SLOT);
	}

	/* Slot 6 names lost+found with status 0x04, which hides it; the
	 * formatter leaves 0x08 at byte 0x36.
	 */
	raw = chk_slot(c, c->root_first, 6);
	CHK(c, field_length(raw, 48, &padded) == 10 &&
	    memcmp(raw, "lost+found", 10) == 0 && padded, "lost+found slot name");
	CHK(c, le32(raw + 0x30) == 2 && raw[0x34] == 4 && raw[0x35] == 0 &&
	    raw[0x36] == 0x08 && all_zero(raw + 0x37, 8) &&
	    raw[0x3f] == ST_HIDDEN, "lost+found slot");
	chk_explain(c, (uint64_t)(raw - c->img), SLOT);

	raw = chk_slot(c, c->root_first, 8);
	chk_decode(raw, &r);
	CHK(c, field_length(raw, 16, &padded) == 14 &&
	    memcmp(raw, ".longfilenames", 14) == 0 && padded,
	    ".longfilenames name");
	CHK(c, r.mode == (T_FILE | 0444) && r.links == 1 && r.status == ST_USED &&
	    r.uid == 0 && r.gid == 0 && r.spare_zero && r.chain == 0,
	    ".longfilenames record");
	CHK(c, r.size == r.blocks * BLOCK && r.blocks % 16 == 0 &&
	    r.extents == (r.blocks != 0 ? 1 : 0) &&
	    (r.first == 0) == (r.blocks == 0), ".longfilenames storage");
	for (unsigned i = 0; i < 4; i++)
		CHK(c, r.times[i] == c->time, ".longfilenames time");
	chk_explain(c, (uint64_t)(raw - c->img), SLOT);
	c->names_first = r.first;
	c->names_blocks = r.size == r.blocks * BLOCK ? r.blocks : 0;
	if (chk_claim(c, c->names_first, c->names_blocks, ".longfilenames")) {
		c->long_named = calloc(c->names_blocks + 1, 1);
		CHECK(c->long_named != NULL);
	} else {
		c->names_blocks = 0;
	}
}

/* Walk the slots of the directory at path, whose own record is at self
 * and whose parent's is at parent, and return its subdirectory count. Used
 * slots follow the dot entries without a gap, in byte order of their
 * names. The root's slots 2 to 8 are the fixed ones, except that slot 7
 * holds its first entry, or without one is free with status 0x02.
 */
static unsigned
chk_directory(chk_t *c, const char *path, const chk_record_t *dir,
    chk_position_t self, chk_position_t parent)
{
	char previous[512];
	const unsigned char *raw;
	unsigned subdirectories = 0;
	uint64_t slots = (uint64_t)dir->blocks * 8;
	chk_position_t at;
	bool root = strcmp(path, "/") == 0;
	bool ended = false;

	previous[0] = '\0';
	raw = chk_slot(c, dir->first, 0);
	chk_dot(c, raw, false, self, dir->blocks);
	chk_explain(c, (uint64_t)(raw - c->img), 2 * SLOT);
	chk_dot(c, raw + SLOT, true, parent, 0);
	if (root)
		chk_root_fixed(c);
	for (uint64_t s = 2; s < slots; s++) {
		if (root && s <= 8 && s != 7)
			continue;
		at.block = dir->first + (uint32_t)(s / 8);
		at.slot = (unsigned)(s % 8);
		raw = chk_slot(c, at.block, at.slot);
		if ((raw[0x3f] & (ST_USED | ST_LINK)) == 0) {
			if (root && s == 7) {
				CHK(c, raw[0x3f] == ST_MODIFIED &&
				    all_zero(raw, SLOT - 1), "free root slot 7");
				chk_explain(c, (uint64_t)(raw - c->img) + SLOT - 1,
				    1);
			}
			ended = true;
			continue;
		}
		CHK(c, !ended, "a used slot follows a free one");
		chk_child(c, path, at, raw, self, previous, &subdirectories);
	}
	return (subdirectories);
}

/* lost+found: 16 blocks whose "." names record (2, 4) and whose ".." names
 * the root's own "." slot; every other slot is free and zero.
 */
static void
chk_lost_found(chk_t *c, const chk_record_t *r)
{
	const unsigned char *raw;

	CHK(c, r->mode == (T_DIR | 0755) && r->links == 2 &&
	    r->status == (ST_TABLE | ST_USED) && r->size == 8192 &&
	    r->blocks == 16 && r->extents == 1 && r->chain == 0 &&
	    r->uid == 0 && r->gid == 0 && r->spare_zero, "lost+found record");
	if (r->blocks != 16 || !chk_claim(c, r->first, 16, "lost+found"))
		return;
	c->lost_found_first = r->first;
	raw = chk_slot(c, r->first, 0);
	chk_dot(c, raw, false, (chk_position_t){ 2, 4 }, 16);
	chk_dot(c, raw + SLOT, true, (chk_position_t){ c->root_first, 0 }, 0);
	chk_explain(c, (uint64_t)(raw - c->img), 2 * SLOT);
}

/* Root-block records 2 and 3: regular files with no name, written (0x13)
 * exactly when they hold content.
 */
static void
chk_boot(chk_t *c, const unsigned char *raw, const char *path)
{
	chk_record_t r;
	chk_entry_t *e;

	chk_decode(raw, &r);
	CHK(c, all_zero(raw, 16) && (r.mode & T_TYPE) == T_FILE && r.links == 1 &&
	    r.chain == 0 && r.spare_zero, "boot record");
	CHK(c, r.status == (r.size != 0 ? 0x13 : 0x11), "boot record status");
	e = chk_add(c, path, &r, true);
	chk_content(c, &r, e, path);
}

/* Block 2: the five records, the label and zeros. */
static void
chk_root_block(chk_t *c, const chk_expect_t *expect, chk_record_t *root,
    chk_record_t *lost_found)
{
	const unsigned char *block = c->img + BLOCK;
	unsigned char label[48];
	chk_record_t r;

	chk_decode(block, root);
	CHK(c, block[0] == '/' && all_zero(block + 1, 15), "root record name");
	CHK(c, root->status == (ST_CLEAN | ST_USED) && root->mode == (T_DIR | 0775) &&
	    root->uid == 0 && root->gid == 0 && root->chain == 0 &&
	    root->spare_zero, "root record");
	CHK(c, root->first == 3 + c->bitmap_blocks && root->extents == 1 &&
	    root->blocks != 0 && root->blocks % 8 == 0 &&
	    root->size == root->blocks * BLOCK, "root directory storage");
	for (unsigned i = 0; i < 4; i++)
		CHK(c, root->times[i] == c->time, "root time");
	c->root_first = root->first;
	c->root_blocks = root->blocks;
	chk_add(c, "/", root, false)->crc = 0;

	chk_decode(block + 64, &r);
	CHK(c, all_zero(block + 64, 16) && r.mode == (T_FILE | 0444) &&
	    r.links == 1 && r.status == (ST_TABLE | ST_USED) && r.uid == 0 &&
	    r.gid == 0 && r.chain == 0 && r.spare_zero && r.extents == 1 &&
	    r.size == r.blocks * BLOCK && r.blocks >= 16, ".inodes record");
	for (unsigned i = 0; i < 4; i++)
		CHK(c, r.times[i] == c->time, ".inodes time");
	CHK(c, expect->inode_slots == 0 || r.blocks * 8 == expect->inode_slots,
	    ".inodes capacity");
	if (chk_claim(c, r.first, r.blocks, ".inodes")) {
		c->inodes_first = r.first;
		c->inodes_blocks = r.blocks;
		c->inode_named = calloc((size_t)r.blocks * 8, 1);
		CHECK(c->inode_named != NULL);
	}

	chk_boot(c, block + 128, "/.boot");
	chk_boot(c, block + 192, "/.altboot");
	chk_decode(block + 256, lost_found);
	CHK(c, all_zero(block + 256, 16), "lost+found record name");
	for (unsigned i = 0; i < 4; i++)
		CHK(c, lost_found->times[i] == c->time, "lost+found time");

	bzero(label, sizeof(label));
	if (expect->label != NULL)
		bcopy(expect->label, label, strlen(expect->label));
	CHK(c, memcmp(block + 0x1c0, label, sizeof(label)) == 0, "volume label");
	chk_explain(c, BLOCK, 5 * SLOT);
	chk_explain(c, BLOCK + 0x1c0, 48);
}

/* .inodes: the signature in slot 0 and the used slots 1 to U in a row; a
 * never-used slot is left to the zero scan.
 */
static void
chk_inodes(chk_t *c)
{
	static const char signature[] = "IamTHE.inodeFILE";
	const unsigned char *raw;
	uint64_t slots = (uint64_t)c->inodes_blocks * 8;
	bool ended = false;

	if (c->inode_named == NULL)
		return;
	raw = chk_slot(c, c->inodes_first, 0);
	CHK(c, memcmp(raw, signature, 16) == 0 && le32(raw + 16) == 0x00100000 &&
	    all_zero(raw + 20, 44), ".inodes signature");
	chk_explain(c, (uint64_t)(raw - c->img), SLOT);
	for (uint64_t k = 1; k < slots; k++) {
		if (c->inode_named[k] == 0) {
			ended = true;
			continue;
		}
		CHK(c, !ended, ".inodes slots are not assigned in a row");
		c->used_inodes++;
	}
	ended = false;
	for (uint64_t j = 0; c->long_named != NULL && j < c->names_blocks; j++) {
		if (c->long_named[j] == 0) {
			ended = true;
			continue;
		}
		CHK(c, !ended, "long-name records are not assigned in a row");
	}
}

/* The bitmap: bit i, block i + 1, is set exactly for an owned block, every
 * bit from N on is set, and the bytes after ceil(N / 8) are 0xff.
 */
static void
chk_bitmap(chk_t *c)
{
	const unsigned char *bitmap = c->img + 2 * BLOCK;
	uint64_t bits = (uint64_t)c->bitmap_blocks * BLOCK * 8;
	bool expected, actual;

	for (uint64_t i = 0; i < bits; i++) {
		expected = i >= c->blocks || c->owned[i + 1] != 0;
		actual = (bitmap[i / 8] >> (i % 8) & 1) != 0;
		if (expected != actual) {
			CHK(c, false, "bitmap disagrees with the owners");
			break;
		}
	}
	chk_explain(c, 2 * BLOCK, (uint64_t)c->bitmap_blocks * BLOCK);
}

static void
chk_free(chk_t *c)
{
	free(c->explained);
	free(c->owned);
	free(c->inode_named);
	free(c->long_named);
	free(c->entries);
	bzero(c, sizeof(*c));
}

/* Check one image completely. The decoded entries stay in c until
 * chk_free(); "/" is the first.
 */
static void
chk_image(chk_t *c, const unsigned char *img, uint64_t length,
    const chk_expect_t *expect)
{
	chk_record_t root, lost_found;
	unsigned subdirectories;

	bzero(c, sizeof(*c));
	c->quiet = expect->quiet;
	c->img = img;
	c->length = length;
	c->time = expect->time;
	if (length % BLOCK != 0 || length / BLOCK < 43 ||
	    length / BLOCK > (UINT64_C(1) << 31)) {
		CHK(c, false, "volume length");
		return;
	}
	c->blocks = (uint32_t)(length / BLOCK);
	c->bitmap_blocks = (c->blocks + 4095) / 4096;
	c->explained = calloc((size_t)length, 1);
	c->owned = calloc((size_t)c->blocks + 1, 1);
	CHECK(c->explained != NULL && c->owned != NULL);
	chk_claim(c, 1, 2 + c->bitmap_blocks, "loader, root block and bitmap");

	chk_root_block(c, expect, &root, &lost_found);
	if (!chk_claim(c, root.first, root.blocks, "root directory") ||
	    c->inode_named == NULL)
		return;
	/* The root counts ".", its own name in block 2 and lost+found's "..". */
	subdirectories = chk_directory(c, "/", &root, (chk_position_t){ 2, 0 },
	    (chk_position_t){ 2, 0 });
	CHK(c, c->entries[0].links == 3 + subdirectories, "root link count");
	chk_lost_found(c, &lost_found);
	chk_inodes(c);

	/* The writer's fixed order: root, .inodes, lost+found, then
	 * .longfilenames when it has blocks.
	 */
	CHK(c, c->inodes_first == c->root_first + c->root_blocks &&
	    c->lost_found_first == c->inodes_first + c->inodes_blocks &&
	    (c->names_blocks == 0 ||
	    c->names_first == c->lost_found_first + 16), "format-owned layout");
	for (uint32_t b = 1; b <= c->blocks; b++)
		if (c->owned[b] != 0)
			c->allocated = b;
	for (uint32_t b = 1; b <= c->allocated; b++)
		if (c->owned[b] == 0) {
			CHK(c, false, "allocation is not one prefix");
			break;
		}
	chk_bitmap(c);

	/* Every byte without a meaning is zero, slack and free blocks
	 * included.
	 */
	for (uint64_t i = 0; i < length; i++) {
		if (c->explained[i] == 0 && img[i] != 0) {
			if (c->errors < 20 && !c->quiet)
				fprintf(stderr, "checker: byte %" PRIu64
				    " (block %" PRIu64 ") is %02x\n", i,
				    i / BLOCK + 1, img[i]);
			c->errors++;
		}
	}
}

static const chk_entry_t *
chk_find(const chk_t *c, const char *path)
{
	for (size_t i = 0; i < c->count; i++)
		if (strcmp(c->entries[i].path, path) == 0)
			return (&c->entries[i]);
	return (NULL);
}

/* Whether N blocks hold demand D after the loader, root block and bitmap. */
static bool
chk_fits(uint64_t blocks, uint64_t demand)
{
	return (2 + (blocks + 4095) / 4096 + demand <= blocks);
}

/* An automatically sized volume is the smallest that holds its demand,
 * which for a volume without requested free space is A - 2 - B.
 */
static void
chk_minimal(const chk_t *c, uint64_t free_blocks)
{
	uint64_t demand = c->allocated - 2 - c->bitmap_blocks + free_blocks;

	CHECK(chk_fits(c->blocks, demand));
	CHECK(c->blocks == 43 || !chk_fits(c->blocks - 1, demand));
}

/*
 * Fixtures.
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
	const fsimg_metadata_t *metadata;
} spec_t;

#define TEN	"0123456789"
#define HUNDRED	TEN TEN TEN TEN TEN TEN TEN TEN TEN TEN
#define N16	"abcdefghijklmnop"
#define N17	N16 "q"
#define N48	"abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKL"
#define N505	HUNDRED HUNDRED HUNDRED HUNDRED HUNDRED "abcde"
#define LINKED	"/a-directory-with-a-link-name"
#define LONGDIR	"/" N48 "-directory"

static const fsimg_metadata_t owned_metadata = {
	.valid = FSIMG_META_OWNER | FSIMG_META_MODE | FSIMG_META_MODIFIED |
	    FSIMG_META_ACCESSED | FSIMG_META_CREATED | FSIMG_META_CHANGED,
	.uid = 1000,
	.gid = 65534,
	.mode = 04750,
	.created = { .seconds = 0, .precision_ns = 1000000000,
	    .basis = FSIMG_TIME_UTC },
	.modified = { .seconds = 1234567890, .precision_ns = 1000000000,
	    .basis = FSIMG_TIME_UTC },
	.accessed = { .seconds = 2147483647, .precision_ns = 1000000000,
	    .basis = FSIMG_TIME_UTC },
	.changed = { .seconds = 1700000000, .precision_ns = 1000000000,
	    .basis = FSIMG_TIME_UTC }
};

static const fsimg_metadata_t private_metadata = {
	.valid = FSIMG_META_OWNER | FSIMG_META_MODE,
	.uid = 65534,
	.gid = 0,
	.mode = 0
};

/* Every kind, names of 1, 16, 17, 48, 49 and 505 bytes, two long names
 * sharing their first 48 bytes, a UTF-8 name, directories with direct,
 * link and long names and children of each, symbolic links and FIFOs
 * with direct and link names, /.boot with content, owner, mode and times
 * off the defaults, and empty, sub-block and multi-block files.
 */
static const spec_t tree[] = {
	{ "/hello.txt", FSIMG_ENTRY_FILE, 33, 1, NULL },
	{ "/.boot", FSIMG_ENTRY_FILE, 700, 6, NULL },
	{ "/.diskroot", FSIMG_ENTRY_FILE, 0, 0, NULL },
	{ "/x", FSIMG_ENTRY_FILE, 1, 15, &owned_metadata },
	{ "/dir", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL },
	{ "/dir/sub", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL },
	{ "/dir/sub/deep.bin", FSIMG_ENTRY_FILE, 5000, 2, NULL },
	{ "/dir/" N16, FSIMG_ENTRY_FILE, 1, 4, NULL },
	{ "/dir/" N17, FSIMG_ENTRY_FILE, 0, 4, NULL },
	{ "/dir/" N48, FSIMG_ENTRY_FILE, 513, 5, NULL },
	{ "/dir/" N48 "M", FSIMG_ENTRY_FILE, 7, 7, NULL },
	{ "/dir/" N48 "N", FSIMG_ENTRY_FILE, 9, 8, NULL },
	{ "/dir/" N505, FSIMG_ENTRY_FILE, 3, 10, NULL },
	{ "/dir/caf\xc3\xa9", FSIMG_ENTRY_FILE, 2, 11, &private_metadata },
	{ LINKED, FSIMG_ENTRY_DIRECTORY, 0, 0, &owned_metadata },
	{ LINKED "/inner", FSIMG_ENTRY_FILE, 100, 12, NULL },
	{ LINKED "/nested-directory-name", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL },
	{ LINKED "/nested-directory-name/leaf", FSIMG_ENTRY_FILE, 1, 13, NULL },
	{ LONGDIR, FSIMG_ENTRY_DIRECTORY, 0, 0, NULL },
	{ LONGDIR "/x", FSIMG_ENTRY_FILE, 10, 14, NULL },
	{ "/link", FSIMG_ENTRY_SYMLINK, 9, 3, NULL },
	{ "/a-symbolic-link-with-a-link-name", FSIMG_ENTRY_SYMLINK, 9, 3, NULL },
	{ "/fifo", FSIMG_ENTRY_FIFO, 0, 0, &private_metadata },
	{ "/a-fifo-with-a-link-name", FSIMG_ENTRY_FIFO, 0, 0, NULL },
	{ "/zero.bin", FSIMG_ENTRY_FILE, 70001, 0, NULL },
	{ "/empty", FSIMG_ENTRY_FILE, 0, 0, NULL }
};
#define TREE_COUNT	(sizeof(tree) / sizeof(tree[0]))

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
expected_crc(uint64_t id, uint64_t size)
{
	unsigned char b;
	uint32_t crc = 0;

	for (uint64_t i = 0; i < size; i++) {
		b = id == 0 ? 0 : content_byte(id, i);
		crc = crc_update(crc, &b, 1);
	}
	return (crc_finish(crc, size));
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
	    "qnx4-write-XXXXXX");
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
	options.loss = loss_report;
	options.loss_argument = fixture;
	options.cancelled = cancel_poll;
	options.cancel_argument = fixture;
	CHECK(fsimg_ctx_new(&options, &fixture->ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_test_meta_cache(fixture->ctx, pages, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &fixture->baseline, NULL) ==
	    FSIMG_OK);
	CHECK(fsimg_writer_new(fixture->ctx, "qnx4", write_options,
	    &fixture->writer, NULL) == FSIMG_OK);
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

/* Free a fixture's writer and context and require that they released
 * every resource; its directory stays.
 */
static void
fixture_release(fixture_t *fixture)
{
	fsimg_stats_t stats;

	fsimg_writer_free(fixture->writer);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == fixture->baseline.memory_bytes);
	CHECK(stats.scratch_bytes == 0);
	CHECK(stats.descriptors == fixture->baseline.descriptors);
	fsimg_ctx_free(fixture->ctx);
	CHECK(close(fixture->parent) == 0);
}

/* Close a fixture: release it and remove its directory. */
static void
fixture_close(fixture_t *fixture)
{
	fixture_release(fixture);
	CHECK(rmdir(fixture->directory) == 0);
}

static fsimg_status_t
add_one(fixture_t *fixture, const spec_t *spec)
{
	fsimg_entry_t entry;
	fsimg_source_t source;
	fsimg_error_t error;
	fsimg_status_t status;

	bzero(&entry, sizeof(entry));
	entry.path = spec->path;
	entry.kind = spec->kind;
	entry.size = spec->size;
	if (spec->metadata != NULL)
		entry.metadata = *spec->metadata;
	source.kind = spec->source == 0 ? FSIMG_SOURCE_ZERO :
	    FSIMG_SOURCE_PROVIDER;
	source.id = spec->source;
	status = fsimg_writer_add(fixture->writer, &entry,
	    spec->kind == FSIMG_ENTRY_FILE || spec->kind == FSIMG_ENTRY_SYMLINK ?
	    &source : NULL, &error);
	if (status != FSIMG_OK && getenv("QNX4_WRITE_DEBUG") != NULL)
		fprintf(stderr, "add %s: %d %s (resource %d)\n", spec->path,
		    status, error.message, (int)error.resource);
	return (status);
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

/* The format time an image carries: the root record's creation time. */
static uint32_t
image_time(const memory_sink_t *sink)
{
	return (le32(sink->bytes + BLOCK + 0x20));
}

/* Create an image of specs and check it with the checker; the checker's
 * state stays in c for the caller.
 */
static void
build_checked(const fsimg_write_options_t *options, const spec_t *specs,
    size_t count, memory_sink_t *sink, chk_t *c, unsigned *losses)
{
	fixture_t fixture;
	chk_expect_t expect;

	fixture_open(&fixture, options);
	add_specs(&fixture, specs, count, false);
	CHECK(emit_memory(&fixture, sink, NULL) == FSIMG_OK);
	if (losses != NULL)
		*losses = fixture.losses;
	fixture_close(&fixture);
	bzero(&expect, sizeof(expect));
	expect.time = image_time(sink);
	expect.label = options->label;
	chk_image(c, sink->bytes, sink->length, &expect);
	CHECK(c->errors == 0);
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

/* Check the tree's entries in the checker's namespace: kind, size, content,
 * owner, mode and times, and that nothing else is there but the root and
 * the two boot records. Reproducible mode stores every time as the epoch,
 * the format time; otherwise the entries' own times are kept.
 */
static void
check_tree_entries(const chk_t *c, uint32_t format_time, bool reproducible)
{
	const fsimg_metadata_t *m;
	const chk_entry_t *e;
	uint16_t type, permissions;

	CHECK(c->count == TREE_COUNT + 2);
	for (size_t i = 0; i < TREE_COUNT; i++) {
		e = chk_find(c, tree[i].path);
		CHECK(e != NULL);
		m = tree[i].metadata;
		type = e->mode & T_TYPE;
		switch (tree[i].kind) {
		case FSIMG_ENTRY_DIRECTORY:
			CHECK(type == T_DIR);
			permissions = 0755;
			break;
		case FSIMG_ENTRY_FIFO:
			CHECK(type == T_FIFO && e->size == 0);
			permissions = 0777;
			break;
		case FSIMG_ENTRY_SYMLINK:
			CHECK(type == T_LINK && strcmp(e->target, "hello.txt") == 0);
			permissions = 0777;
			break;
		default:
			CHECK(type == T_FILE);
			permissions = strcmp(tree[i].path, "/.boot") == 0 ? 0600 :
			    0644;
			break;
		}
		if (m != NULL && (m->valid & FSIMG_META_MODE) != 0)
			permissions = (uint16_t)m->mode;
		CHECK((e->mode & 07777) == permissions);
		CHECK(e->indirect == (strlen(strrchr(tree[i].path, '/') + 1) > 16 ||
		    strcmp(tree[i].path, "/.boot") == 0));
		if (m != NULL && (m->valid & FSIMG_META_OWNER) != 0)
			CHECK(e->uid == m->uid && e->gid == m->gid);
		else
			CHECK(e->uid == 0 && e->gid == 0);
		if (m == &owned_metadata && !reproducible) {
			CHECK(e->times[0] == 0 && e->times[1] == 1234567890 &&
			    e->times[2] == 2147483647 && e->times[3] == 1700000000);
		} else {
			for (unsigned t = 0; t < 4; t++)
				CHECK(e->times[t] == format_time);
		}
		if (tree[i].kind == FSIMG_ENTRY_DIRECTORY)
			continue;
		CHECK(e->size == tree[i].size);
		CHECK(e->crc == expected_crc(tree[i].source, tree[i].size));
	}
	e = chk_find(c, "/.altboot");
	CHECK(e != NULL && e->size == 0 && e->mode == (T_FILE | 0600));
}

/* Read the image back through fsimg: every entry matches the checker's,
 * every file's and link's content its CRC, and payload verification is
 * complete.
 */
static void
check_reader(fixture_t *fixture, memory_sink_t *sink, const chk_t *c)
{
	fsimg_reader_t *reader;
	fsimg_io_t *input;
	const fsimg_entry_t *entry;
	const chk_entry_t *e;
	fsimg_verify_report_t report;
	fsimg_error_t error;
	unsigned char buffer[4096];
	uint64_t offset, count;
	uint32_t crc;
	size_t length;
	fsimg_status_t status;

	CHECK(memory_io(fixture, sink, false, &input) == FSIMG_OK);
	CHECK(fsimg_reader_open(fixture->ctx, input, NULL, &reader, NULL) ==
	    FSIMG_OK);
	count = 0;
	while ((status = fsimg_reader_next(reader, &entry, NULL)) == FSIMG_OK) {
		count++;
		e = chk_find(c, entry->path);
		CHECK(e != NULL);
		CHECK((entry->metadata.mode & 07777) == (e->mode & 07777u));
		CHECK(entry->metadata.uid == e->uid && entry->metadata.gid == e->gid);
		CHECK(entry->metadata.modified.seconds == e->times[1]);
		if (entry->kind != FSIMG_ENTRY_FILE &&
		    entry->kind != FSIMG_ENTRY_SYMLINK)
			continue;
		CHECK(entry->size == e->size);
		crc = 0;
		for (offset = 0; offset < entry->size; offset += length) {
			CHECK(fsimg_reader_read_at(reader, offset, buffer,
			    sizeof(buffer), &length, NULL) == FSIMG_OK);
			CHECK(length > 0);
			crc = crc_update(crc, buffer, length);
		}
		CHECK(crc_finish(crc, entry->size) == e->crc);
	}
	CHECK(status == FSIMG_DONE);
	/* The checker's entries include the root, which the reader does not
	 * list.
	 */
	CHECK(count == c->count - 1);
	fsimg_reader_free(reader);
	CHECK(fsimg_reader_open(fixture->ctx, input, NULL, &reader, NULL) ==
	    FSIMG_OK);
	status = fsimg_verify(reader, FSIMG_VERIFY_PAYLOAD, &report, &error);
	if (status != FSIMG_OK)
		fprintf(stderr, "verify: %d %s (resource %d)\n", status,
		    error.message, (int)error.resource);
	CHECK(status == FSIMG_OK);
	CHECK(report.complete);
	fsimg_reader_free(reader);
	fsimg_io_free(input);
}

static void
reproducible_options(fsimg_write_options_t *options)
{
	fsimg_write_options_init(options);
	options->flags |= FSIMG_WRITE_REPRODUCIBLE;
	options->epoch = 1700000000;
}

/*
 * Tests.
 */

/* The tree in three passes -- forward catalog order with a one-page cache
 * and scratch, reverse order with a 1024-page cache in another time zone,
 * and forward order without scratch, all metadata in a 1024-page cache --
 * passes the checker and the reader each time and gives identical bytes.
 */
static void
test_matrix(void)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sinks[3];
	chk_expect_t expect;
	chk_t c;

	reproducible_options(&options);
	options.label = "fsimg QNX4";
	for (unsigned pass = 0; pass < 3; pass++) {
		if (pass == 1) {
			CHECK(setenv("TZ", "America/New_York", 1) == 0);
			tzset();
		}
		fixture_open_with(&fixture, &options, pass == 0 ? 1 : 1024,
		    pass != 2);
		add_specs(&fixture, tree, TREE_COUNT, pass == 1);
		CHECK(emit_memory(&fixture, &sinks[pass], NULL) == FSIMG_OK);
		/* Three of owned_metadata's four times differ from the epoch,
		 * in two entries.
		 */
		CHECK(fixture.losses == 6);
		bzero(&expect, sizeof(expect));
		expect.time = 1700000000;
		expect.label = options.label;
		expect.inode_slots = 128;
		chk_image(&c, sinks[pass].bytes, sinks[pass].length, &expect);
		CHECK(c.errors == 0);
		check_tree_entries(&c, 1700000000, true);
		CHECK(c.names_blocks == 16 && c.used_inodes == 10);
		chk_minimal(&c, 0);
		check_reader(&fixture, &sinks[pass], &c);
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
 * format-owned records carry the planning clock.
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
	CHECK(c.time >= (uint32_t)before && c.time <= (uint32_t)after);
	check_tree_entries(&c, c.time, false);
	chk_free(&c);
	free(sink.bytes);
}

/* An empty catalog is the formatter's 43-block minimum, in which no block
 * is free; exact sizes at and above it hold the same layout, and below it
 * validation refuses the size.
 */
static void
test_empty(void)
{
	fsimg_write_options_t options;
	fsimg_writer_t *writer;
	fsimg_ctx_t *ctx;
	memory_sink_t sink;
	fsimg_plan_info_t info;
	chk_t c;

	reproducible_options(&options);
	build_checked(&options, NULL, 0, &sink, &c, NULL);
	CHECK(c.blocks == 43 && c.allocated == 43 && c.count == 3);
	CHECK(c.root_first == 4 && c.inodes_first == 12 &&
	    c.lost_found_first == 28 && c.names_blocks == 0);
	chk_free(&c);
	free(sink.bytes);
	CHECK(plan_status(&options, NULL, 0, &info) == FSIMG_OK);
	CHECK(info.volume_bytes == 43 * 512 && info.free_bytes == 0 &&
	    info.allocated_bytes == 43 * 512 && info.metadata_bytes == 43 * 512 &&
	    info.allocation_unit_bytes == 512 && info.sector_bytes == 512);

	options.volume_bytes = 44 * 512;
	build_checked(&options, NULL, 0, &sink, &c, NULL);
	CHECK(c.blocks == 44 && c.allocated == 43);
	chk_free(&c);
	free(sink.bytes);

	options.volume_bytes = 42 * 512;
	CHECK(fsimg_ctx_new(NULL, &ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_new(ctx, "qnx4", &options, &writer, NULL) ==
	    FSIMG_EINVAL);
	fsimg_ctx_free(ctx);
}

/* Exact and automatic sizes at the bitmap-block boundaries: a demand D of
 * 4093 blocks fits 4096 blocks with one bitmap block, 4094 needs 4098, as
 * 4097 blocks take two; 8188 fits 8192 with two and 8193 with three.
 * Files of zero bytes stand in for content.
 */
static void
test_sizing(void)
{
	static const struct {
		uint64_t demand;
		uint64_t volume_blocks;	/* 0 automatic */
		uint64_t expected;	/* 0 ERANGE */
	} cases[] = {
		{ 4093, 0, 4096 }, { 4094, 0, 4098 }, { 4093, 4096, 4096 },
		{ 4094, 4096, 0 }, { 4093, 4097, 4097 }, { 4094, 4097, 0 },
		{ 4095, 4097, 0 }, { 8188, 0, 8192 }, { 8188, 8192, 8192 },
		{ 8189, 8192, 0 }, { 8188, 8193, 8193 }, { 8189, 0, 8194 },
		{ 41, 0, 44 }, { 40, 0, 43 }
	};
	fsimg_write_options_t options;
	memory_sink_t sink;
	spec_t file = { "/fill", FSIMG_ENTRY_FILE, 0, 0, NULL };
	chk_t c;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		reproducible_options(&options);
		options.volume_bytes = cases[i].volume_blocks * 512;
		/* The empty layout's demand is 40 blocks. */
		file.size = (cases[i].demand - 40) * 512;
		if (cases[i].expected == 0) {
			CHECK(plan_status(&options, &file, 1, NULL) ==
			    FSIMG_ERANGE);
			continue;
		}
		build_checked(&options, &file, 1, &sink, &c, NULL);
		CHECK(c.blocks == cases[i].expected);
		CHECK(c.allocated == 2 + c.bitmap_blocks + cases[i].demand);
		if (cases[i].volume_blocks == 0)
			chk_minimal(&c, 0);
		chk_free(&c);
		free(sink.bytes);
	}

	/* Requested free space is part of the demand. */
	reproducible_options(&options);
	options.min_free_bytes = 100 * 512 - 1;
	build_checked(&options, NULL, 0, &sink, &c, NULL);
	CHECK(c.blocks == 143 && c.allocated == 43);
	chk_minimal(&c, 100);
	chk_free(&c);
	free(sink.bytes);

	/* A full volume: the exact size of the automatic one has no free
	 * block, and one block less does not hold the tree.
	 */
	reproducible_options(&options);
	build_checked(&options, tree, TREE_COUNT, &sink, &c, NULL);
	options.volume_bytes = (uint64_t)c.allocated * 512;
	CHECK(c.blocks == c.allocated);
	chk_free(&c);
	free(sink.bytes);
	build_checked(&options, tree, TREE_COUNT, &sink, &c, NULL);
	CHECK(c.blocks == c.allocated);
	chk_free(&c);
	free(sink.bytes);
	options.volume_bytes -= 512;
	CHECK(plan_status(&options, tree, TREE_COUNT, NULL) == FSIMG_ERANGE);
}

/* The largest volume and file plan without emission: 2^31 blocks exactly,
 * a file of 2^31 - 1 bytes. 511 such files fit the largest volume and 512,
 * 2^31 blocks of content alone, fail as ERANGE; a larger file is
 * unsupported.
 */
static void
test_limits(void)
{
	static spec_t big[512];
	static char paths[512][16];
	fsimg_write_options_t options;
	fsimg_plan_info_t info;
	spec_t file = { "/big", FSIMG_ENTRY_FILE, UINT32_C(0x7fffffff), 0, NULL };

	reproducible_options(&options);
	options.volume_bytes = (UINT64_C(1) << 31) * 512;
	CHECK(plan_status(&options, &file, 1, &info) == FSIMG_OK);
	CHECK(info.volume_bytes == options.volume_bytes &&
	    info.initialized_bytes == UINT32_C(0x7fffffff));
	options.volume_bytes = 0;
	file.size = UINT64_C(0x80000000);
	CHECK(plan_status(&options, &file, 1, NULL) == FSIMG_EUNSUPPORTED);
	for (size_t i = 0; i < 512; i++) {
		snprintf(paths[i], sizeof(paths[i]), "/f%zu", i);
		big[i] = (spec_t){ paths[i], FSIMG_ENTRY_FILE,
		    UINT32_C(0x7fffffff), 0, NULL };
	}
	CHECK(plan_status(&options, big, 511, &info) == FSIMG_OK);
	CHECK(plan_status(&options, big, 512, NULL) == FSIMG_ERANGE);
}

/* The root's width: 0, 1, 2 and 56 entries fit its 8 blocks with the nine
 * fixed slots, 57 and 60 take 16 blocks. Slots fill 7, then 9 on.
 */
static void
test_root_widths(void)
{
	static const size_t widths[] = { 0, 1, 2, 56, 57, 60 };
	static spec_t specs[60];
	static char paths[60][16];
	fsimg_write_options_t options;
	memory_sink_t sink;
	chk_t c;

	for (size_t i = 0; i < 60; i++) {
		snprintf(paths[i], sizeof(paths[i]), "/e%02zu", i);
		specs[i] = (spec_t){ paths[i], FSIMG_ENTRY_FILE, 1, 2, NULL };
	}
	for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
		reproducible_options(&options);
		build_checked(&options, specs, widths[w], &sink, &c, NULL);
		CHECK(c.root_blocks == (widths[w] <= 56 ? 8 : 16));
		CHECK(c.count == widths[w] + 3);
		CHECK((sink.bytes[(c.root_first - 1) * 512 + 7 * 64 + 63] ==
		    ST_MODIFIED) == (widths[w] == 0));
		chk_free(&c);
		free(sink.bytes);
	}
}

/* .inodes capacity: max(128, inodes=, 1 + linked entries), rounded up to
 * eight slots.
 */
static void
test_inodes(void)
{
	static const struct {
		uint32_t setting;
		size_t linked;
		uint32_t slots;
	} cases[] = {
		{ 0, 0, 128 }, { 100, 0, 128 }, { 128, 0, 128 }, { 129, 0, 136 },
		{ 0, 127, 128 }, { 0, 128, 136 }, { 150, 200, 208 },
		{ 300, 200, 304 }
	};
	static spec_t specs[200];
	static char paths[200][40];
	fsimg_write_options_t options;
	fsimg_writer_t *writer;
	fsimg_ctx_t *ctx;
	memory_sink_t sink;
	chk_expect_t expect;
	fixture_t fixture;
	chk_t c;

	for (size_t i = 0; i < 200; i++) {
		snprintf(paths[i], sizeof(paths[i]), "/a-linked-file-name-%03zu", i);
		specs[i] = (spec_t){ paths[i], FSIMG_ENTRY_FILE, 0, 0, NULL };
	}
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		reproducible_options(&options);
		options.inode_capacity = cases[i].setting;
		fixture_open(&fixture, &options);
		add_specs(&fixture, specs, cases[i].linked, false);
		CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
		fixture_close(&fixture);
		bzero(&expect, sizeof(expect));
		expect.time = 1700000000;
		expect.inode_slots = cases[i].slots;
		chk_image(&c, sink.bytes, sink.length, &expect);
		CHECK(c.errors == 0 && c.used_inodes == cases[i].linked);
		chk_minimal(&c, 0);
		chk_free(&c);
		free(sink.bytes);
	}
	/* The largest .inodes within the 2^31 - 1 byte object maximum. */
	reproducible_options(&options);
	options.inode_capacity = UINT32_C(33554424) + 1;
	CHECK(fsimg_ctx_new(NULL, &ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_new(ctx, "qnx4", &options, &writer, NULL) ==
	    FSIMG_EINVAL);
	options.inode_capacity = UINT32_C(33554424);
	CHECK(fsimg_writer_new(ctx, "qnx4", &options, &writer, NULL) ==
	    FSIMG_OK);
	fsimg_writer_free(writer);
	fsimg_ctx_free(ctx);
}

/* /.boot and /.altboot: an empty file keeps status 0x11 and no extent, one
 * with content is 0x13 with its own mode and owner; any other kind is
 * unsupported. Elsewhere the names are ordinary.
 */
static void
test_boot(void)
{
	static const fsimg_metadata_t boot_metadata = {
		.valid = FSIMG_META_OWNER | FSIMG_META_MODE,
		.uid = 7,
		.gid = 8,
		.mode = 0644
	};
	static const spec_t files[] = {
		{ "/.altboot", FSIMG_ENTRY_FILE, 0, 0, NULL },
		{ "/.boot", FSIMG_ENTRY_FILE, 1025, 2, &boot_metadata },
		{ "/sub", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL },
		{ "/sub/.boot", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL },
		{ "/sub/.altboot", FSIMG_ENTRY_SYMLINK, 9, 3, NULL }
	};
	static const spec_t directory = { "/.boot", FSIMG_ENTRY_DIRECTORY, 0, 0,
	    NULL };
	static const spec_t link = { "/.altboot", FSIMG_ENTRY_SYMLINK, 9, 3,
	    NULL };
	fsimg_write_options_t options;
	memory_sink_t sink;
	const chk_entry_t *e;
	chk_t c;

	reproducible_options(&options);
	build_checked(&options, files, 5, &sink, &c, NULL);
	e = chk_find(&c, "/.boot");
	CHECK(e != NULL && e->size == 1025 && e->mode == (T_FILE | 0644) &&
	    e->uid == 7 && e->gid == 8 && e->crc == expected_crc(2, 1025));
	CHECK(sink.bytes[512 + 128 + 63] == 0x13);
	e = chk_find(&c, "/.altboot");
	CHECK(e != NULL && e->size == 0 && e->mode == (T_FILE | 0600));
	CHECK(sink.bytes[512 + 192 + 63] == 0x11);
	CHECK(chk_find(&c, "/sub/.boot") != NULL &&
	    chk_find(&c, "/sub/.altboot") != NULL);
	/* No root slot for either: slot 7 is /sub. */
	CHECK(memcmp(sink.bytes + (c.root_first - 1) * 512 + 7 * 64, "sub", 4) == 0);
	chk_free(&c);
	free(sink.bytes);
	CHECK(plan_status(&options, &directory, 1, NULL) == FSIMG_EUNSUPPORTED);
	CHECK(plan_status(&options, &link, 1, NULL) == FSIMG_EUNSUPPORTED);
}

/* The format-owned root names are refused; below the root they are
 * ordinary. Names outside the profile and of 506 bytes are refused, and
 * every length at a record-kind boundary is stored: direct up to 16 bytes,
 * link up to 48, long-name record up to 505.
 */
static void
test_names(void)
{
	static const char *const reserved[] = { "/.bitmap", "/.inodes",
	    "/.longfilenames", "/lost+found" };
	static const spec_t ordinary[] = {
		{ "/d", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL },
		{ "/d/.bitmap", FSIMG_ENTRY_FILE, 1, 2, NULL },
		{ "/d/.inodes", FSIMG_ENTRY_FILE, 1, 2, NULL },
		{ "/d/.longfilenames", FSIMG_ENTRY_FILE, 1, 2, NULL },
		{ "/d/lost+found", FSIMG_ENTRY_DIRECTORY, 0, 0, NULL }
	};
	static const char *const invalid[] = { "/del\x7f", "/bad\xff",
	    "/ctl\x01", "/" N505 "x" };
	static const size_t lengths[] = { 1, 15, 16, 17, 47, 48, 49, 504, 505 };
	enum { BOUNDARIES = sizeof(lengths) / sizeof(lengths[0]) };
	static char paths[BOUNDARIES][520];
	spec_t boundary[BOUNDARIES];
	fsimg_write_options_t options;
	memory_sink_t sink;
	spec_t spec;
	fsimg_status_t status;
	chk_t c;

	reproducible_options(&options);
	for (size_t i = 0; i < 4; i++) {
		spec = (spec_t){ reserved[i], FSIMG_ENTRY_FILE, 0, 0, NULL };
		CHECK(plan_status(&options, &spec, 1, NULL) == FSIMG_EINVAL);
		spec.kind = FSIMG_ENTRY_DIRECTORY;
		CHECK(plan_status(&options, &spec, 1, NULL) == FSIMG_EINVAL);
	}
	build_checked(&options, ordinary, 5, &sink, &c, NULL);
	CHECK(chk_find(&c, "/d/lost+found") != NULL && c.count == 8);
	chk_free(&c);
	free(sink.bytes);
	for (size_t i = 0; i < 4; i++) {
		spec = (spec_t){ invalid[i], FSIMG_ENTRY_FILE, 0, 0, NULL };
		status = plan_status(&options, &spec, 1, NULL);
		CHECK(status == FSIMG_EINVAL);
	}

	for (size_t i = 0; i < BOUNDARIES; i++) {
		paths[i][0] = '/';
		for (size_t k = 0; k < lengths[i]; k++)
			paths[i][1 + k] = (char)('a' + (i + k) % 26);
		paths[i][1 + lengths[i]] = '\0';
		boundary[i] = (spec_t){ paths[i], FSIMG_ENTRY_FILE, i + 1, 2,
		    NULL };
	}
	build_checked(&options, boundary, BOUNDARIES, &sink, &c, NULL);
	for (size_t i = 0; i < BOUNDARIES; i++)
		CHECK(chk_find(&c, paths[i]) != NULL);
	chk_free(&c);
	free(sink.bytes);
}

/* What the writer stores for test_metadata's file: the losses it counts,
 * the owner, and the modification and access times, where FORMAT_TIME
 * stands for the image's format time. Creation and change times are always
 * the format time.
 */
#define FORMAT_TIME	UINT32_MAX
#define META_MODIFIED	1700000001u
#define META_ACCESSED	2147483647u

typedef struct stored {
	unsigned	 losses;
	uint16_t	 uid;
	uint16_t	 gid;
	uint32_t	 modified;
	uint32_t	 accessed;
} stored_t;

/* Owner 65534:12, mode 04751, a modification time and the largest QNX4
 * access time, all representable.
 */
static fsimg_metadata_t
representable_metadata(void)
{
	return ((fsimg_metadata_t){
		.valid = FSIMG_META_OWNER | FSIMG_META_MODE |
		    FSIMG_META_MODIFIED | FSIMG_META_ACCESSED,
		.uid = 65534,
		.gid = 12,
		.mode = 04751,
		.modified = { .seconds = META_MODIFIED,
		    .precision_ns = 1000000000, .basis = FSIMG_TIME_UTC },
		.accessed = { .seconds = META_ACCESSED,
		    .precision_ns = 1000000000, .basis = FSIMG_TIME_UTC }
	});
}

/* One file with metadata is written and checked against what is stored;
 * the format time is the epoch when reproducible and the clock otherwise.
 */
static void
check_stored(const fsimg_write_options_t *options,
    const fsimg_metadata_t *metadata, stored_t expect)
{
	const spec_t spec = { "/meta", FSIMG_ENTRY_FILE, 5, 2, metadata };
	const chk_entry_t *e;
	unsigned losses;
	time_t before, after;
	chk_t c;
	memory_sink_t sink;

	before = time(NULL);
	build_checked(options, &spec, 1, &sink, &c, &losses);
	after = time(NULL);
	CHECK(losses == expect.losses);
	if ((options->flags & FSIMG_WRITE_REPRODUCIBLE) != 0)
		CHECK(c.time == (uint32_t)options->epoch);
	else
		CHECK(c.time >= (uint32_t)before && c.time <= (uint32_t)after);
	e = chk_find(&c, "/meta");
	CHECK(e != NULL && e->mode == (T_FILE | 04751));
	CHECK(e->uid == expect.uid && e->gid == expect.gid);
	CHECK(e->times[0] == c.time && e->times[3] == c.time);
	CHECK(e->times[1] == (expect.modified == FORMAT_TIME ? c.time :
	    expect.modified));
	CHECK(e->times[2] == (expect.accessed == FORMAT_TIME ? c.time :
	    expect.accessed));
	chk_free(&c);
	free(sink.bytes);
}

static void
check_metadata_refused(const fsimg_write_options_t *options,
    const fsimg_metadata_t *metadata)
{
	const spec_t spec = { "/meta", FSIMG_ENTRY_FILE, 5, 2, metadata };

	CHECK(plan_status(options, &spec, 1, NULL) == FSIMG_EUNSUPPORTED);
}

/* The metadata policy: an owner or group above 65534 is stored as 0 and a
 * time outside 0 through 2147483647, or with a fraction or a wall-clock
 * basis, takes the format time or its whole seconds, each one counted
 * loss; a caller that forbids losses, or requires the field, gets
 * FSIMG_EUNSUPPORTED. Reproducible mode stores the epoch for an absent
 * time.
 */
static void
test_metadata(void)
{
	const stored_t kept = { .losses = 0, .uid = 65534, .gid = 12,
	    .modified = META_MODIFIED, .accessed = META_ACCESSED };
	fsimg_write_options_t lossy, strict, reproducible;
	fsimg_metadata_t m;
	stored_t expect;

	fsimg_write_options_init(&lossy);
	strict = lossy;
	strict.flags &= ~(uint32_t)FSIMG_WRITE_ALLOW_LOSS;
	reproducible_options(&reproducible);
	reproducible.epoch = 1700000001;

	m = representable_metadata();
	check_stored(&lossy, &m, kept);

	m = representable_metadata();
	m.uid = 65535;
	m.gid = 70000;
	expect = kept;
	expect.losses = 1;
	expect.uid = 0;
	expect.gid = 0;
	check_stored(&lossy, &m, expect);
	check_metadata_refused(&strict, &m);

	m = representable_metadata();
	m.accessed.seconds = 2147483648;
	expect = kept;
	expect.losses = 1;
	expect.accessed = FORMAT_TIME;
	check_stored(&lossy, &m, expect);
	check_metadata_refused(&strict, &m);
	m.required = FSIMG_META_ACCESSED;
	check_metadata_refused(&lossy, &m);

	m = representable_metadata();
	m.modified.seconds = -1;
	expect = kept;
	expect.losses = 1;
	expect.modified = FORMAT_TIME;
	check_stored(&lossy, &m, expect);

	/* A fraction or a wall-clock basis keeps the whole seconds. */
	m = representable_metadata();
	m.modified.nanoseconds = 5;
	expect = kept;
	expect.losses = 1;
	check_stored(&lossy, &m, expect);
	m = representable_metadata();
	m.modified.basis = FSIMG_TIME_WALL;
	check_stored(&lossy, &m, expect);

	m = representable_metadata();
	m.valid &= ~(uint32_t)FSIMG_META_ACCESSED;
	expect = kept;
	expect.accessed = FORMAT_TIME;
	check_stored(&reproducible, &m, expect);
}

/* Labels of 0, 1, 47 and 48 bytes sit at 0x1c0, NUL-padded, the 48-byte one
 * without a terminator; a longer label or one outside the byte rules is
 * refused, and the reader reports the label.
 */
static void
test_labels(void)
{
	static const char *const labels[] = { "", "A",
	    "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJK", N48 };
	static const char *const invalid[] = { N48 "M", "a/b", "tab\t",
	    "bad\xff" };
	fsimg_write_options_t options;
	fsimg_volume_info_t volume;
	fsimg_writer_t *writer;
	fsimg_reader_t *reader;
	fsimg_io_t *input;
	fixture_t fixture;
	memory_sink_t sink;
	chk_t c;

	for (size_t i = 0; i < 4; i++) {
		reproducible_options(&options);
		options.label = labels[i];
		build_checked(&options, NULL, 0, &sink, &c, NULL);
		chk_free(&c);
		fixture_open(&fixture, &options);
		CHECK(memory_io(&fixture, &sink, false, &input) == FSIMG_OK);
		CHECK(fsimg_reader_open(fixture.ctx, input, NULL, &reader,
		    NULL) == FSIMG_OK);
		CHECK(fsimg_reader_get_info(reader, &volume, NULL) == FSIMG_OK);
		CHECK(((volume.valid & FSIMG_VOLUME_LABEL) != 0) ==
		    (labels[i][0] != '\0'));
		CHECK(labels[i][0] == '\0' || strcmp(volume.label, labels[i]) == 0);
		fsimg_reader_free(reader);
		fsimg_io_free(input);
		fixture_close(&fixture);
		free(sink.bytes);
	}
	fixture_open(&fixture, NULL);
	for (size_t i = 0; i < 4; i++) {
		reproducible_options(&options);
		options.label = invalid[i];
		CHECK(fsimg_writer_new(fixture.ctx, "qnx4", &options, &writer, NULL) ==
		    FSIMG_EINVAL);
	}
	fixture_close(&fixture);
}

/* Writer construction from options fails with FSIMG_EINVAL. */
static void
check_refused(fsimg_ctx_t *ctx, const fsimg_write_options_t *options)
{
	fsimg_writer_t *writer;

	CHECK(fsimg_writer_new(ctx, "qnx4", options, &writer, NULL) ==
	    FSIMG_EINVAL);
}

/* Settings: 512-byte sectors and blocks only, no FAT root, sizes of whole
 * blocks from 43 to 2^31, a 31-bit reproducible epoch, and no byte order,
 * UUID or reserve. A capability record of another descriptor is refused.
 */
static void
test_validation(void)
{
	fsimg_write_options_t options, o;
	fsimg_writer_t *writer;
	fsimg_ctx_t *ctx;

	CHECK(fsimg_ctx_new(NULL, &ctx, NULL) == FSIMG_OK);
	fsimg_write_options_init(&options);
	CHECK(fsimg_writer_new_variant(ctx, &fsimg_qnx4_format,
	    &fsimg_qnx6_format.variants[0], &options, &writer, NULL) ==
	    FSIMG_EINVAL);
	o = options;
	o.sector_bytes = 1024;
	check_refused(ctx, &o);
	o = options;
	o.allocation_unit_bytes = 1024;
	check_refused(ctx, &o);
	o = options;
	o.fat_root_entries = 16;
	check_refused(ctx, &o);
	o = options;
	o.volume_bytes = 43 * 512 + 1;
	check_refused(ctx, &o);
	o = options;
	o.volume_bytes = 42 * 512;
	check_refused(ctx, &o);
	o = options;
	o.volume_bytes = ((UINT64_C(1) << 31) + 1) * 512;
	check_refused(ctx, &o);
	reproducible_options(&o);
	o.epoch = 2147483648;
	check_refused(ctx, &o);
	reproducible_options(&o);
	o.epoch = -1;
	check_refused(ctx, &o);
	o = options;
	o.byte_order = FSIMG_BYTE_ORDER_BIG;
	check_refused(ctx, &o);
	o = options;
	o.volume_uuid[0] = 1;
	check_refused(ctx, &o);
	o = options;
	o.reserve_percent = 5;
	check_refused(ctx, &o);
	o = options;
	o.label = N48 "M";
	check_refused(ctx, &o);
	fsimg_write_options_init(&options);
	options.sector_bytes = 0;
	options.allocation_unit_bytes = 512;
	options.volume_bytes = 43 * 512;
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	options.epoch = 2147483647;
	CHECK(fsimg_writer_new(ctx, "qnx4", &options, &writer, NULL) ==
	    FSIMG_OK);
	fsimg_writer_free(writer);
	fsimg_ctx_free(ctx);
}

/* The independent checker sees one changed byte anywhere: the loader, a
 * free block, a free slot, an unused .inodes slot, a file's slack, a dot
 * entry, a padding bit of the bitmap and slot 6's mark.
 */
static void
test_checker(void)
{
	static const spec_t files[] = {
		{ "/a", FSIMG_ENTRY_FILE, 10, 2, NULL },
		{ "/b-with-a-link-name", FSIMG_ENTRY_FILE, 3, 4, NULL }
	};
	fsimg_write_options_t options;
	memory_sink_t sink;
	chk_expect_t expect;
	uint64_t offsets[8];
	chk_t c;

	reproducible_options(&options);
	options.volume_bytes = 4097 * 512;
	build_checked(&options, files, 2, &sink, &c, NULL);
	offsets[0] = 17;
	offsets[1] = (uint64_t)(c.blocks - 1) * 512 + 100;
	offsets[2] = (uint64_t)(c.root_first - 1) * 512 + 10 * 64 + 3;
	offsets[3] = (uint64_t)(c.inodes_first - 1) * 512 + 5 * 64 + 63;
	offsets[4] = (uint64_t)(c.allocated - 1) * 512 + 300;
	offsets[5] = (uint64_t)(c.root_first - 1) * 512 + 64 + 0x34;
	offsets[6] = 1024 + 4097 / 8;
	offsets[7] = (uint64_t)(c.root_first - 1) * 512 + 6 * 64 + 0x36;
	chk_free(&c);
	for (size_t i = 0; i < 8; i++) {
		sink.bytes[offsets[i]] ^= 0x40;
		bzero(&expect, sizeof(expect));
		expect.time = 1700000000;
		expect.quiet = true;
		chk_image(&c, sink.bytes, sink.length, &expect);
		CHECK(c.errors != 0);
		chk_free(&c);
		sink.bytes[offsets[i]] ^= 0x40;
	}
	free(sink.bytes);

	/* An empty root's slot 7 is free with status 0x02, not 0. */
	build_checked(&options, NULL, 0, &sink, &c, NULL);
	sink.bytes[(uint64_t)(c.root_first - 1) * 512 + 7 * 64 + 63] = 0;
	chk_free(&c);
	bzero(&expect, sizeof(expect));
	expect.time = 1700000000;
	expect.quiet = true;
	chk_image(&c, sink.bytes, sink.length, &expect);
	CHECK(c.errors != 0);
	chk_free(&c);
	free(sink.bytes);
}

/* Sink and source faults: an oversized fixed sink is refused before any
 * write, failed writes and flushes and a changed or short source leave the
 * output incomplete, short writes give the reference bytes, and a writer
 * emits once.
 */
static void
test_faults(void)
{
	/* Each fault and its outcome: refused before any write, a failure
	 * that leaves the output incomplete, or the reference bytes.
	 */
	enum { REFUSED, INCOMPLETE, REFERENCE };
	static const struct {
		uint64_t	 extra_sink;	/* bytes past the volume */
		bool		 durable;	/* on a sink without durability */
		bool		 fail_write;
		bool		 fail_flush;
		bool		 changed;	/* a source changes size */
		bool		 eof;		/* a source ends early */
		size_t		 max_write;	/* short writes */
		int		 outcome;
	} cases[] = {
		{ .extra_sink = 512, .outcome = REFUSED },
		{ .durable = true, .outcome = REFUSED },
		{ .fail_write = true, .outcome = INCOMPLETE },
		{ .fail_flush = true, .outcome = INCOMPLETE },
		{ .changed = true, .outcome = INCOMPLETE },
		{ .eof = true, .outcome = INCOMPLETE },
		{ .max_write = 700, .outcome = REFERENCE }
	};
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink, reference;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_io_t *output;
	fsimg_error_t error;

	reproducible_options(&options);
	fixture_open(&fixture, &options);
	add_specs(&fixture, tree, TREE_COUNT, false);
	CHECK(emit_memory(&fixture, &reference, NULL) == FSIMG_OK);
	fixture_close(&fixture);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		reproducible_options(&options);
		if (cases[i].durable)
			options.flags |= FSIMG_WRITE_DURABLE;
		fixture_open(&fixture, &options);
		add_specs(&fixture, tree, TREE_COUNT, false);
		CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
		CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
		bzero(&sink, sizeof(sink));
		sink.length = info.volume_bytes + cases[i].extra_sink;
		sink.bytes = calloc(1, (size_t)sink.length);
		CHECK(sink.bytes != NULL);
		CHECK(memory_io(&fixture, &sink, true, &output) == FSIMG_OK);
		sink.fail_write = cases[i].fail_write;
		sink.fail_flush = cases[i].fail_flush;
		sink.max_write = cases[i].max_write;
		fixture.changed = cases[i].changed;
		fixture.eof = cases[i].eof;
		switch (cases[i].outcome) {
		case REFERENCE:
			CHECK(fsimg_writer_emit(fixture.writer, output, NULL) ==
			    FSIMG_OK);
			CHECK(sink.length == reference.length &&
			    bcmp(sink.bytes, reference.bytes,
			    (size_t)sink.length) == 0);
			break;
		case REFUSED:
			CHECK(fsimg_writer_emit(fixture.writer, output, NULL) <
			    FSIMG_OK);
			CHECK(sink.writes == 0);
			break;
		default:
			CHECK(fsimg_writer_emit(fixture.writer, output, &error) <
			    FSIMG_OK);
			CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
			break;
		}
		CHECK(fsimg_writer_emit(fixture.writer, output, NULL) ==
		    FSIMG_ESTATE);
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
	succeeded = false;
	/* Cancel at polls spaced about 6% apart over the whole run, every
	 * poll among the first sixteen.
	 */
	for (uint64_t attempt = 1; attempt < 100000 && !succeeded;
	    attempt += attempt / 16 + 1) {
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
 * Native comparison.
 */

/* Read a gzip file into memory through gzip -dc; NULL when the file is
 * absent.
 */
static unsigned char *
read_gzip(const char *path, size_t *length)
{
	unsigned char *bytes, *grown;
	size_t capacity, used;
	ssize_t n;
	int pipefd[2], status;
	pid_t child;

	if (access(path, R_OK) != 0)
		return (NULL);
	CHECK(pipe(pipefd) == 0);
	child = fork();
	CHECK(child != -1);
	if (child == 0) {
		close(pipefd[0]);
		if (dup2(pipefd[1], STDOUT_FILENO) == -1)
			_exit(127);
		execlp("gzip", "gzip", "-dc", path, (char *)NULL);
		_exit(127);
	}
	CHECK(close(pipefd[1]) == 0);
	capacity = 1 << 20;
	used = 0;
	bytes = malloc(capacity);
	CHECK(bytes != NULL);
	for (;;) {
		if (used == capacity) {
			capacity *= 2;
			grown = realloc(bytes, capacity);
			CHECK(grown != NULL);
			bytes = grown;
		}
		n = read(pipefd[0], bytes + used, capacity - used);
		if (n == -1 && errno == EINTR)
			continue;
		CHECK(n >= 0);
		if (n == 0)
			break;
		used += (size_t)n;
	}
	CHECK(close(pipefd[0]) == 0);
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	*length = used;
	return (bytes);
}

/* Captures of native dinit -hq: whole-disk volumes of 131072 blocks, with
 * and without -l, and exact block counts formatted on a file and copied to
 * the start of the disk.
 */
typedef struct native_capture {
	const char	*name;
	const char	*label;
	uint32_t	 blocks;
} native_capture_t;

static const native_capture_t native_captures[] = {
	{ "q4-empty-131072-dinitonly", NULL, 131072 },
	{ "q4-empty-131072-default", NULL, 131072 },
	{ "q4-empty-131072-labelA", "LabelAAAA", 131072 },
	{ "q4-empty-131072-labelC", "0123456789abcdef0123456789abcdef"
	    "0123456789abcdef", 131072 },
	{ "q4-empty-1000-file", NULL, 1000 },
	{ "q4-empty-4095-file", NULL, 4095 },
	{ "q4-empty-4096-file", NULL, 4096 },
	{ "q4-empty-4097-file", NULL, 4097 },
	{ "q4-empty-5001-file", NULL, 5001 },
	{ "q4-empty-8192-file", NULL, 8192 },
	{ "q4-empty-8193-file", NULL, 8193 },
	{ "q4-empty-131071-file", NULL, 131071 }
};

/* Compare fsimg's empty volume with one capture: the same N, label and
 * format time (the capture's root-record creation time). Every byte of
 * blocks 2 to A must match. Block 1 is the proprietary loader, which fsimg
 * writes as zeros. A free block must be zero on both sides or, where the
 * formatter never wrote it, the capture's 0xaa disk fill; such blocks are
 * counted, not compared. Returns the number of differing bytes.
 */
static uint64_t
compare_native_one(const char *directory, const native_capture_t *capture,
    bool *skipped)
{
	fsimg_write_options_t options;
	fixture_t fixture;
	memory_sink_t sink;
	chk_expect_t expect;
	chk_t c;
	char path[1024];
	unsigned char *native;
	const unsigned char *theirs, *ours;
	uint64_t differences, zero_free, filled_free;
	size_t length;
	bool zero, filled;

	snprintf(path, sizeof(path), "%s/%s.img.gz", directory, capture->name);
	native = read_gzip(path, &length);
	*skipped = native == NULL;
	if (native == NULL) {
		printf("SKIP: %s is absent\n", path);
		return (0);
	}
	CHECK(length >= (size_t)capture->blocks * 512);
	reproducible_options(&options);
	options.epoch = le32(native + BLOCK + 0x20);
	options.volume_bytes = (uint64_t)capture->blocks * 512;
	options.label = capture->label;
	fixture_open(&fixture, &options);
	CHECK(emit_memory(&fixture, &sink, NULL) == FSIMG_OK);
	fixture_close(&fixture);
	bzero(&expect, sizeof(expect));
	expect.time = (uint32_t)options.epoch;
	expect.label = capture->label;
	chk_image(&c, sink.bytes, sink.length, &expect);
	CHECK(c.errors == 0);

	differences = 0;
	zero_free = 0;
	filled_free = 0;
	for (uint32_t b = 2; b <= capture->blocks; b++) {
		theirs = native + volume_offset(b);
		ours = sink.bytes + volume_offset(b);
		if (b > c.allocated) {
			zero = true;
			filled = true;
			for (unsigned i = 0; i < BLOCK; i++) {
				zero = zero && theirs[i] == 0;
				filled = filled && theirs[i] == 0xaa;
			}
			if (zero || filled) {
				zero_free += zero;
				filled_free += filled;
				continue;
			}
		}
		for (unsigned i = 0; i < BLOCK; i++) {
			if (theirs[i] == ours[i])
				continue;
			if (differences++ < 10)
				printf("%s: block %" PRIu32 " byte %u is %02x, "
				    "native %02x\n", capture->name, b, i, ours[i],
				    theirs[i]);
		}
	}
	printf("%s: N = %" PRIu32 ", B = %" PRIu32 ", A = %" PRIu32 ": %" PRIu64
	    " differing bytes in blocks 2 to N; %" PRIu64 " free blocks zero, %"
	    PRIu64 " never written (0xaa); the loader block not compared\n",
	    capture->name, c.blocks, c.bitmap_blocks, c.allocated, differences,
	    zero_free, filled_free);
	chk_free(&c);
	free(sink.bytes);
	free(native);
	return (differences);
}

static int
compare_native(const char *directory)
{
	uint64_t differences = 0;
	unsigned compared = 0;
	bool skipped;

	for (size_t i = 0; i < sizeof(native_captures) /
	    sizeof(native_captures[0]); i++) {
		differences += compare_native_one(directory, &native_captures[i],
		    &skipped);
		compared += !skipped;
	}
	printf("%u native captures compared, %" PRIu64 " differing bytes\n",
	    compared, differences);
	if (compared == 0)
		return (77);
	return (differences == 0 ? 0 : 1);
}

/*
 * Native qualification images.
 */

/* ls(1)'s mode string. */
static void
mode_string(uint16_t mode, char *out)
{
	static const char rwx[] = "rwxrwxrwx";

	switch (mode & T_TYPE) {
	case T_DIR:
		out[0] = 'd';
		break;
	case T_LINK:
		out[0] = 'l';
		break;
	case T_FIFO:
		out[0] = 'p';
		break;
	default:
		out[0] = '-';
		break;
	}
	for (unsigned i = 0; i < 9; i++)
		out[1 + i] = (mode & (0400u >> i)) != 0 ? rwx[i] : '-';
	if ((mode & 04000) != 0)
		out[3] = (mode & 0100) != 0 ? 's' : 'S';
	if ((mode & 02000) != 0)
		out[6] = (mode & 0010) != 0 ? 's' : 'S';
	if ((mode & 01000) != 0)
		out[9] = (mode & 0001) != 0 ? 't' : 'T';
	out[10] = '\0';
}

/* The manifest of an image as the checker decodes it, in the integration
 * script's line format: "E path mode links uid gid size [target]" for
 * every object and "C path crc size" for every regular file, tab
 * separated. Compare it with a guest manifest after LC_ALL=C sort.
 */
static void
write_manifest(const chk_t *c, FILE *out)
{
	const chk_entry_t *e;
	char mode[11];

	for (size_t i = 0; i < c->count; i++) {
		e = &c->entries[i];
		mode_string(e->mode, mode);
		fprintf(out, "E\t%s\t%s\t%u\t%u\t%u\t%" PRIu32 "%s%s\n", e->path,
		    mode, e->links, e->uid, e->gid, e->size,
		    (e->mode & T_TYPE) == T_LINK ? "\t" : "",
		    (e->mode & T_TYPE) == T_LINK ? e->target : "");
		if ((e->mode & T_TYPE) == T_FILE)
			fprintf(out, "C\t%s\t%" PRIu32 "\t%" PRIu32 "\n", e->path,
			    e->crc, e->size);
	}
}

/* Build DIRECTORY/writer.img for the native qualification in
 * tools/qnx/guest/qualify4.sh, and DIRECTORY/writer.manifest:
 *   empty	no entries; with BLOCKS 0, the 43-block minimum
 *   tree	the fixture tree
 *   full	the tree and a zero-filled file that leaves 256 blocks free,
 *		room for the guest's own writes; BLOCKS must be given
 * BLOCKS 0 selects the smallest volume, LABEL the volume label. The image
 * is not reproducible, so that entries keep their own times; the
 * format-owned records carry the planning clock. It is published through
 * write_at, read back and checked, and the manifest comes from the
 * checker's decoding.
 */
static int
create_external(const char *directory, const char *name, const char *blocks,
    const char *label)
{
	static const uint64_t reserve = 256;
	fsimg_write_options_t options;
	fsimg_plan_info_t info;
	spec_t specs[TREE_COUNT + 1];
	chk_expect_t expect;
	fixture_t fixture;
	struct stat st;
	chk_t c;
	char path[1024];
	const unsigned char *image;
	size_t count;
	FILE *manifest;
	int parent, fd;

	fsimg_write_options_init(&options);
	options.volume_bytes = strtoull(blocks, NULL, 10) * 512;
	options.label = label;
	count = 0;
	if (strcmp(name, "tree") == 0 || strcmp(name, "full") == 0) {
		bcopy(tree, specs, sizeof(tree));
		count = TREE_COUNT;
	} else if (strcmp(name, "empty") != 0) {
		fprintf(stderr, "unknown tree %s\n", name);
		return (1);
	}
	if (strcmp(name, "full") == 0) {
		CHECK(options.volume_bytes != 0);
		CHECK(plan_status(&options, specs, count, &info) == FSIMG_OK);
		CHECK(info.free_bytes / 512 > reserve);
		specs[count++] = (spec_t){ "/filler", FSIMG_ENTRY_FILE,
		    (info.free_bytes / 512 - reserve) * 512, 0, NULL };
	}
	fixture_open(&fixture, &options);
	add_specs(&fixture, specs, count, false);
	parent = open(directory, O_RDONLY | O_DIRECTORY);
	CHECK(parent != -1);
	CHECK(fsimg_writer_write_at(fixture.writer, parent, "writer.img", NULL) ==
	    FSIMG_OK);
	fixture_close(&fixture);

	fd = openat(parent, "writer.img", O_RDONLY);
	CHECK(fd != -1 && fstat(fd, &st) == 0);
	/* Mapped, not read: a volume the size of the PCM navigation partition
	 * qualify4.sh checks is 52 GB, almost all of it holes. The checker
	 * still keeps one byte of state per image byte, so such a run needs
	 * that much address space and touches every page of it.
	 */
	CHECK(st.st_size >= 2 * BLOCK && (uint64_t)st.st_size <= SIZE_MAX);
	image = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	CHECK(image != MAP_FAILED);
	CHECK(close(fd) == 0);
	bzero(&expect, sizeof(expect));
	expect.time = le32(image + BLOCK + 0x20);
	expect.label = label;
	chk_image(&c, image, (uint64_t)st.st_size, &expect);
	CHECK(c.errors == 0);
	snprintf(path, sizeof(path), "%s/writer.manifest", directory);
	manifest = fopen(path, "w");
	CHECK(manifest != NULL);
	write_manifest(&c, manifest);
	CHECK(fclose(manifest) == 0);
	printf("%s/writer.img: %" PRIu32 " blocks, %" PRIu32 " allocated, %zu "
	    "objects\n", directory, c.blocks, c.allocated, c.count);
	chk_free(&c);
	CHECK(munmap((void *)image, (size_t)st.st_size) == 0);
	CHECK(close(parent) == 0);
	return (0);
}

/*
 * Writer scale (--scale and --scale-limits). Each case runs in a child
 * process of its own and prints what it took. --scale emits and reads back:
 *
 * - the largest volume, 2^31 blocks (1 TiB), filled to its last block by
 *   the tree, 511 files of 2^31 - 1 zero bytes and a filler, so the
 *   bitmap is ones from end to end and the last extent ends in block N;
 * - one directory of 200,000 files, one in sixteen with a long name, whose
 *   catalog and rows spill far beyond the 16 MiB budget into scratch.
 *
 * --scale-limits plans, without emitting, the object maximum of 2^31 - 1
 * bytes that every directory and table must respect: 4,194,288 long names
 * fit .longfilenames and one more fails, and a directory of 33,554,366
 * children fits and one more fails, each failure for that reason. They
 * run under a 1 GiB budget and need tens of GiB of transient scratch.
 *
 * No case writes an image file. Emission goes to a page sink, which keeps
 * only the nonzero 4 KiB pages of the volume in memory and shares
 * identical ones: the common emitter zeroes the sink through zero_range
 * first and skips zero sources, so free blocks and zero payload are never
 * written, and the ones of a full bitmap are one shared page. The sink
 * refuses to hold more pages than the plan's metadata and content need,
 * so a writer that wrote free space would fail the case before it could
 * fill memory. The fsimg reader opens and verifies the volume through the
 * same sink. Every context has an explicit scratch budget, and a case runs
 * only when the scratch filesystem has that free and 1 GiB more, so
 * scratch, the only data that reaches a disk, is bounded too.
 * QNX4_SCALE_SCRATCH names the scratch parent, such as a RAM disk.
 *
 * The checks restate the driver design's bounds: charged memory stays
 * below SCALE_MEMORY, scratch within the case's budget, the growth of the
 * child's peak resident set, less the sink's pages, below
 * SCALE_RSS_GROWTH, and the bytes written at most the plan's metadata and
 * the tree's content.
 */

/* The charged-memory ceiling, below the context's 16 MiB budget. */
#define SCALE_MEMORY		(UINT64_C(8) << 20)
#define SCALE_BUDGET		(UINT64_C(16) << 20)

/* The limit cases' budget: they show where planning refuses, not what it
 * keeps resident, and a large budget spills less of their tens of
 * millions of entries.
 */
#define SCALE_LIMIT_BUDGET	(UINT64_C(1) << 30)

/* The most a case may raise its process's peak resident set beyond the
 * pages its sink holds.
 */
#define SCALE_RSS_GROWTH	(UINT64_C(64) << 20)

/* The QNX4 object maximum, the largest file. */
#define FILE_MAX		UINT64_C(0x7fffffff)

#define PAGE_BYTES		4096u
#define PAGE_SLOT_BUCKETS	(1u << 20)
#define PAGE_CONTENT_BUCKETS	(1u << 16)

/* One distinct page content, shared by every volume page that holds it. */
typedef struct page {
	struct page	*next;		/* in its content bucket */
	uint64_t	 hash;
	uint64_t	 refs;
	unsigned char	 bytes[PAGE_BYTES];
} page_t;

/* A volume page that is not all zero. */
typedef struct page_slot {
	struct page_slot *next;		/* in its index bucket */
	uint64_t	 index;
	page_t		*page;
} page_slot_t;

typedef struct page_sink {
	uint64_t	 length;
	uint64_t	 slot_limit;	/* the most nonzero pages allowed */
	page_slot_t	**slots;	/* by page index */
	page_t		**contents;	/* by content hash */
	uint64_t	 slot_count;
	uint64_t	 page_count;	/* distinct contents held */
	uint64_t	 page_peak;
	uint64_t	 written_bytes;	/* through write_at, zeros included */
	uint64_t	 write_limit;	/* the most write_at may take */
	uint64_t	 zeroed_bytes;	/* through zero_range */
	uint64_t	 zero_calls;
} page_sink_t;

static uint64_t
page_hash(const unsigned char *bytes)
{
	uint64_t hash = UINT64_C(0xcbf29ce484222325);

	for (size_t i = 0; i < PAGE_BYTES; i++)
		hash = (hash ^ bytes[i]) * UINT64_C(0x100000001b3);
	return (hash);
}

/* The link to page index's slot in its bucket, or to the bucket's end. */
static page_slot_t **
page_slot_link(page_sink_t *sink, uint64_t index)
{
	page_slot_t **link;

	/* Fibonacci hashing: the product's top 20 bits pick one of the
	 * PAGE_SLOT_BUCKETS buckets.
	 */
	link = &sink->slots[(index * UINT64_C(0x9e3779b97f4a7c15)) >> 44];
	while (*link != NULL && (*link)->index != index)
		link = &(*link)->next;
	return (link);
}

/* A shared page holding bytes, with one more reference. */
static page_t *
page_intern(page_sink_t *sink, const unsigned char *bytes)
{
	page_t *page;
	uint64_t hash;

	hash = page_hash(bytes);
	for (page = sink->contents[hash % PAGE_CONTENT_BUCKETS]; page != NULL;
	    page = page->next) {
		if (page->hash == hash && bcmp(page->bytes, bytes,
		    PAGE_BYTES) == 0) {
			page->refs++;
			return (page);
		}
	}
	page = malloc(sizeof(*page));
	CHECK(page != NULL);
	page->hash = hash;
	page->refs = 1;
	bcopy(bytes, page->bytes, PAGE_BYTES);
	page->next = sink->contents[hash % PAGE_CONTENT_BUCKETS];
	sink->contents[hash % PAGE_CONTENT_BUCKETS] = page;
	if (++sink->page_count > sink->page_peak)
		sink->page_peak = sink->page_count;
	return (page);
}

static void
page_release(page_sink_t *sink, page_t *page)
{
	page_t **link;

	if (--page->refs != 0)
		return;
	link = &sink->contents[page->hash % PAGE_CONTENT_BUCKETS];
	while (*link != page)
		link = &(*link)->next;
	*link = page->next;
	free(page);
	sink->page_count--;
}

/* Unlink and free the slot at link. */
static void
page_drop(page_sink_t *sink, page_slot_t **link)
{
	page_slot_t *slot = *link;

	*link = slot->next;
	page_release(sink, slot->page);
	free(slot);
	sink->slot_count--;
}

/* Store one whole page; an all-zero page is dropped. */
static void
page_store(page_sink_t *sink, uint64_t index, const unsigned char *bytes)
{
	page_slot_t **link, *slot;
	page_t *old;

	link = page_slot_link(sink, index);
	slot = *link;
	if (all_zero(bytes, PAGE_BYTES)) {
		if (slot != NULL)
			page_drop(sink, link);
		return;
	}
	if (slot != NULL) {
		/* Intern first: the new content may be the old one. */
		old = slot->page;
		slot->page = page_intern(sink, bytes);
		page_release(sink, old);
		return;
	}
	CHECK(sink->slot_count < sink->slot_limit);
	slot = malloc(sizeof(*slot));
	CHECK(slot != NULL);
	slot->index = index;
	slot->page = page_intern(sink, bytes);
	slot->next = NULL;
	*link = slot;
	sink->slot_count++;
}

/* Copy page index's bytes, zeros when absent. */
static void
page_load(page_sink_t *sink, uint64_t index, unsigned char *bytes)
{
	page_slot_t *slot;

	slot = *page_slot_link(sink, index);
	if (slot != NULL)
		bcopy(slot->page->bytes, bytes, PAGE_BYTES);
	else
		bzero(bytes, PAGE_BYTES);
}

/* Apply bytes, or zeros when bytes is NULL, to [offset, offset + length). */
static void
page_apply(page_sink_t *sink, uint64_t offset, const unsigned char *bytes,
    uint64_t length)
{
	unsigned char page[PAGE_BYTES];
	uint64_t index;
	size_t within, n;

	while (length > 0) {
		index = offset / PAGE_BYTES;
		within = (size_t)(offset % PAGE_BYTES);
		n = PAGE_BYTES - within;
		if (n > length)
			n = (size_t)length;
		if (n != PAGE_BYTES)
			page_load(sink, index, page);
		if (bytes != NULL)
			bcopy(bytes, page + within, n);
		else
			bzero(page + within, n);
		page_store(sink, index, page);
		if (bytes != NULL)
			bytes += n;
		offset += n;
		length -= n;
	}
}

/* Zero [offset, offset + length): the partial pages at either end through
 * page_apply, the whole pages between by dropping their slots, walking
 * whichever is smaller, the range or the slots held.
 */
static void
page_clear(page_sink_t *sink, uint64_t offset, uint64_t length)
{
	page_slot_t **link;
	uint64_t first, last, head, tail;

	head = (PAGE_BYTES - offset % PAGE_BYTES) % PAGE_BYTES;
	if (head >= length) {
		page_apply(sink, offset, NULL, length);
		return;
	}
	tail = (offset + length) % PAGE_BYTES;
	page_apply(sink, offset, NULL, head);
	page_apply(sink, offset + length - tail, NULL, tail);
	first = (offset + head) / PAGE_BYTES;
	last = (offset + length - tail) / PAGE_BYTES;	/* exclusive */
	if (last - first <= sink->slot_count) {
		for (uint64_t index = first; index < last; index++) {
			link = page_slot_link(sink, index);
			if (*link != NULL)
				page_drop(sink, link);
		}
		return;
	}
	for (size_t b = 0; b < PAGE_SLOT_BUCKETS; b++) {
		link = &sink->slots[b];
		while (*link != NULL) {
			if ((*link)->index >= first && (*link)->index < last)
				page_drop(sink, link);
			else
				link = &(*link)->next;
		}
	}
}

static fsimg_status_t
page_size(void *argument, uint64_t *length, fsimg_error_t *error FSIMG_UNUSED)
{
	page_sink_t *sink = argument;

	*length = sink->length;
	return (FSIMG_OK);
}

static fsimg_status_t
page_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	page_sink_t *sink = argument;
	unsigned char page[PAGE_BYTES], *out = buffer;
	size_t within, n;

	CHECK(offset <= sink->length && length <= sink->length - offset);
	*transferred = length;
	while (length > 0) {
		within = (size_t)(offset % PAGE_BYTES);
		n = PAGE_BYTES - within;
		if (n > length)
			n = length;
		page_load(sink, offset / PAGE_BYTES, page);
		bcopy(page + within, out, n);
		out += n;
		offset += n;
		length -= n;
	}
	return (FSIMG_OK);
}

static fsimg_status_t
page_write(void *argument, uint64_t offset, const void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	page_sink_t *sink = argument;

	CHECK(offset <= sink->length && length <= sink->length - offset);
	CHECK(length <= sink->write_limit - sink->written_bytes);
	page_apply(sink, offset, buffer, length);
	sink->written_bytes += length;
	*transferred = length;
	return (FSIMG_OK);
}

/* Zeroing an empty sink, as the emitter does first, touches nothing. */
static fsimg_status_t
page_zero(void *argument, uint64_t offset, uint64_t length,
    fsimg_error_t *error FSIMG_UNUSED)
{
	page_sink_t *sink = argument;

	CHECK(offset <= sink->length && length <= sink->length - offset);
	if (sink->slot_count != 0)
		page_clear(sink, offset, length);
	sink->zeroed_bytes += length;
	sink->zero_calls++;
	return (FSIMG_OK);
}

/* Allocate the sink's tables and touch them, so that the resident set
 * measured after this includes them.
 */
static void
page_sink_open(page_sink_t *sink)
{
	bzero(sink, sizeof(*sink));
	sink->slots = malloc(PAGE_SLOT_BUCKETS * sizeof(*sink->slots));
	sink->contents = malloc(PAGE_CONTENT_BUCKETS * sizeof(*sink->contents));
	CHECK(sink->slots != NULL && sink->contents != NULL);
	memset(sink->slots, 0, PAGE_SLOT_BUCKETS * sizeof(*sink->slots));
	memset(sink->contents, 0,
	    PAGE_CONTENT_BUCKETS * sizeof(*sink->contents));
}

/* The memory the sink's slots and its peak of pages took. */
static uint64_t
page_sink_bytes(const page_sink_t *sink)
{
	return (sink->slot_count * sizeof(page_slot_t) +
	    sink->page_peak * sizeof(page_t));
}

static void
page_sink_close(page_sink_t *sink)
{
	for (size_t b = 0; b < PAGE_SLOT_BUCKETS; b++) {
		while (sink->slots[b] != NULL)
			page_drop(sink, &sink->slots[b]);
	}
	CHECK(sink->page_count == 0 && sink->slot_count == 0);
	free(sink->slots);
	free(sink->contents);
}

static fsimg_status_t
page_io(fsimg_ctx_t *ctx, page_sink_t *sink, bool writable, fsimg_io_t **out)
{
	fsimg_io_ops_t ops;

	fsimg_io_ops_init(&ops);
	ops.get_size = page_size;
	ops.read_at = page_read;
	if (writable) {
		ops.write_at = page_write;
		ops.zero_range = page_zero;
	}
	*out = NULL;
	return (fsimg_io_from_callbacks(ctx, &ops, sink, out, NULL));
}

/* The page sink itself, which the scale cases rely on: unaligned writes
 * and reads, sharing of identical pages, and zeroing of ranges shorter and
 * longer than the pages held, through both of page_clear's walks.
 */
static void
test_page_sink(void)
{
	static const uint64_t offsets[] = { 100, 4096 * 7 + 3, 4096 * 20 };
	unsigned char data[3 * PAGE_BYTES], back[3 * PAGE_BYTES];
	page_sink_t sink;
	size_t n;

	page_sink_open(&sink);
	sink.length = UINT64_C(1) << 40;
	sink.slot_limit = 16;
	sink.write_limit = UINT64_MAX;
	memset(data, 0x5a, sizeof(data));
	for (size_t i = 0; i < 3; i++)
		CHECK(page_write(&sink, offsets[i], data, sizeof(data), &n,
		    NULL) == FSIMG_OK && n == sizeof(data));
	/* Three runs of 12 KiB touch 11 pages, sharing the all-0x5a one. */
	CHECK(sink.slot_count == 11 && sink.page_count < sink.slot_count);
	CHECK(page_read(&sink, offsets[1], back, sizeof(back), &n, NULL) ==
	    FSIMG_OK && bcmp(back, data, sizeof(back)) == 0);
	/* A short range walks its pages: the middle run loses bytes 10 to
	 * 8191, its ends survive.
	 */
	CHECK(page_zero(&sink, offsets[1] + 10, 8182, NULL) == FSIMG_OK);
	CHECK(page_read(&sink, offsets[1], back, sizeof(back), &n, NULL) ==
	    FSIMG_OK);
	CHECK(all_zero(back + 10, 8182) && back[9] == 0x5a &&
	    back[8192] == 0x5a);
	/* A range longer than the pages held walks the slot table. */
	CHECK(page_zero(&sink, 1, sink.length - 1, NULL) == FSIMG_OK);
	CHECK(sink.slot_count == 0 && sink.page_count == 0);
	CHECK(page_read(&sink, offsets[0], back, sizeof(back), &n, NULL) ==
	    FSIMG_OK && all_zero(back, sizeof(back)));
	page_sink_close(&sink);
}

/* What one case measured in its child. */
typedef struct scale_result {
	fsimg_status_t	 plan_status;
	uint64_t	 volume_bytes;
	uint64_t	 memory_peak;
	uint64_t	 plan_scratch;	/* the scratch peak after planning */
	uint64_t	 scratch_peak;	/* and after reading back */
	uint64_t	 io_calls;	/* emission's, the zeroing excluded */
	uint64_t	 zero_calls;	/* the zeroing's, 64 KiB each */
	uint64_t	 written_bytes;	/* through write_at */
	uint64_t	 sink_pages;	/* peak distinct nonzero contents */
	uint64_t	 sink_slots;	/* nonzero volume pages at the end */
	uint64_t	 rss_growth;	/* beyond the sink's pages */
	uint64_t	 files;
	uint64_t	 directories;
	uint64_t	 allocated;	/* blocks, as verification counts them */
	uint64_t	 free;
	double		 plan_seconds;
	double		 emit_seconds;
	double		 verify_seconds;
} scale_result_t;

typedef enum scale_kind {
	SCALE_EMIT,	/* plan, emit and read back; --scale */
	SCALE_LIMIT	/* plan only, at an object maximum; --scale-limits */
} scale_kind_t;

/* A case: its entries, generated without a catalog of their own, and its
 * expectations.
 */
typedef struct scale_case {
	const char	*name;
	scale_kind_t	 kind;
	uint64_t	 volume_bytes;	/* zero sizes automatically */
	uint64_t	 memory_bytes;	/* zero is SCALE_BUDGET */
	uint64_t	 scratch_bytes;
	uint64_t	 max_entries;	/* zero keeps the default */
	bool		 tree;		/* the volume holds the tree */
	bool		 spills;	/* planning needs scratch beyond memory */
	void		(*add)(fixture_t *, const struct scale_case *);
	uint64_t	 count;		/* the generator's size */
	fsimg_status_t	 expect;
	const char	*refusal;	/* in the failing plan's message */
	const char	*refusal_path;	/* the failing plan's path, if any */
} scale_case_t;

static double
seconds_since(const struct timespec *start)
{
	struct timespec now;

	CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
	return ((double)(now.tv_sec - start->tv_sec) +
	    (double)(now.tv_nsec - start->tv_nsec) / 1e9);
}

/* The process's peak resident set so far, in bytes. */
static uint64_t
peak_rss(void)
{
	struct rusage usage;

	CHECK(getrusage(RUSAGE_SELF, &usage) == 0 && usage.ru_maxrss >= 0);
#if defined(__APPLE__)
	return ((uint64_t)usage.ru_maxrss);
#else
	return ((uint64_t)usage.ru_maxrss * 1024);
#endif
}

/* A writer for a scale case: a 16 MiB budget, the default metadata cache,
 * and scratch under the case's budget in directory, which the parent
 * process made and removes.
 */
static void
scale_fixture_open(fixture_t *fixture, const scale_case_t *sc,
    const char *directory)
{
	fsimg_write_options_t write_options;
	fsimg_options_t options;
	fsimg_source_ops_t source_ops;

	bzero(fixture, sizeof(*fixture));
	CHECK(snprintf(fixture->directory, sizeof(fixture->directory), "%s",
	    directory) < (int)sizeof(fixture->directory));
	fixture->parent = open(fixture->directory,
	    O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fixture->parent != -1);
	fsimg_options_init(&options);
	options.memory_bytes = sc->memory_bytes != 0 ? sc->memory_bytes :
	    SCALE_BUDGET;
	options.scratch_bytes = sc->scratch_bytes;
	if (sc->max_entries != 0)
		options.max_entries = sc->max_entries;
	options.scratch_directory_fd = fixture->parent;
	options.max_open_files = 64;
	options.loss = loss_report;
	options.loss_argument = fixture;
	CHECK(fsimg_ctx_new(&options, &fixture->ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &fixture->baseline, NULL) ==
	    FSIMG_OK);
	reproducible_options(&write_options);
	write_options.volume_bytes = sc->volume_bytes;
	CHECK(fsimg_writer_new(fixture->ctx, "qnx4", &write_options,
	    &fixture->writer, NULL) == FSIMG_OK);
	fsimg_source_ops_init(&source_ops);
	source_ops.validate = provider_validate;
	source_ops.read_at = provider_read;
	CHECK(fsimg_writer_set_sources(fixture->writer, &source_ops, fixture,
	    NULL) == FSIMG_OK);
}

/* Add one generated entry; the path is copied by the writer. */
static void
scale_add(fixture_t *fixture, const char *path, fsimg_entry_kind_t kind,
    uint64_t size)
{
	spec_t spec = { path, kind, size, 0, NULL };

	CHECK(add_one(fixture, &spec) == FSIMG_OK);
}

/* The tree, then 511 files of the 2^31 - 1 byte maximum and a filler that
 * takes every block still free, all of zero content.
 */
static void
scale_add_full(fixture_t *fixture, const scale_case_t *sc)
{
	static spec_t specs[TREE_COUNT + 511];
	static char paths[511][16];
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fixture_t sizing;

	bcopy(tree, specs, sizeof(tree));
	for (size_t i = 0; i < 511; i++) {
		snprintf(paths[i], sizeof(paths[i]), "/max%03zu", i);
		specs[TREE_COUNT + i] = (spec_t){ paths[i], FSIMG_ENTRY_FILE,
		    FILE_MAX, 0, NULL };
	}
	/* The filler takes what a plan of everything else leaves free. */
	scale_fixture_open(&sizing, sc, fixture->directory);
	add_specs(&sizing, specs, TREE_COUNT + 511, false);
	CHECK(fsimg_writer_plan(sizing.writer, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	fixture_release(&sizing);
	CHECK(info.free_bytes % 512 == 0 && info.free_bytes <= FILE_MAX);
	add_specs(fixture, specs, TREE_COUNT + 511, false);
	scale_add(fixture, "/zfiller", FSIMG_ENTRY_FILE, info.free_bytes);
}

/* count files in /wide, every sixteenth with a 49-byte name, each 1000
 * bytes of zero content.
 */
static void
scale_add_wide(fixture_t *fixture, const scale_case_t *sc)
{
	char path[64];

	scale_add(fixture, "/wide", FSIMG_ENTRY_DIRECTORY, 0);
	for (uint64_t i = 0; i < sc->count; i++) {
		if (i % 16 == 0)
			snprintf(path, sizeof(path), "/wide/%049" PRIu64, i);
		else
			snprintf(path, sizeof(path), "/wide/w%015" PRIu64, i);
		scale_add(fixture, path, FSIMG_ENTRY_FILE, 1000);
	}
}

/* count empty files with 49-byte names in /n. */
static void
scale_add_long(fixture_t *fixture, const scale_case_t *sc)
{
	char path[64];

	scale_add(fixture, "/n", FSIMG_ENTRY_DIRECTORY, 0);
	for (uint64_t i = 0; i < sc->count; i++) {
		snprintf(path, sizeof(path), "/n/%049" PRIu64, i);
		scale_add(fixture, path, FSIMG_ENTRY_FILE, 0);
	}
}

/* count empty files with 16-byte names, direct records, in /d. */
static void
scale_add_children(fixture_t *fixture, const scale_case_t *sc)
{
	char path[32];

	scale_add(fixture, "/d", FSIMG_ENTRY_DIRECTORY, 0);
	for (uint64_t i = 0; i < sc->count; i++) {
		snprintf(path, sizeof(path), "/d/%016" PRIu64, i);
		scale_add(fixture, path, FSIMG_ENTRY_FILE, 0);
	}
}

/* The tree's nonzero content, in whole blocks. */
static uint64_t
tree_content_bytes(void)
{
	uint64_t bytes = 0;

	for (size_t i = 0; i < TREE_COUNT; i++) {
		if (tree[i].source != 0)
			bytes += (tree[i].size + 511) / 512 * 512;
	}
	return (bytes);
}

/* Read the volume back: open, list every entry, check the tree's content
 * when the case holds it, and verify the structure.
 */
static void
scale_read_back(fixture_t *fixture, page_sink_t *sink, scale_result_t *result,
    bool tree_content)
{
	fsimg_reader_t *reader;
	fsimg_io_t *input;
	const fsimg_entry_t *entry;
	fsimg_verify_report_t report;
	fsimg_error_t error;
	unsigned char buffer[4096];
	uint64_t offset, listed;
	uint32_t crc;
	size_t length;
	fsimg_status_t status;

	CHECK(page_io(fixture->ctx, sink, false, &input) == FSIMG_OK);
	CHECK(fsimg_reader_open(fixture->ctx, input, NULL, &reader, NULL) ==
	    FSIMG_OK);
	listed = 0;
	while ((status = fsimg_reader_next(reader, &entry, NULL)) == FSIMG_OK) {
		listed++;
		if (!tree_content)
			continue;
		for (size_t i = 0; i < TREE_COUNT; i++) {
			if (strcmp(entry->path, tree[i].path) != 0 ||
			    tree[i].kind != FSIMG_ENTRY_FILE)
				continue;
			crc = 0;
			for (offset = 0; offset < entry->size; offset += length) {
				CHECK(fsimg_reader_read_at(reader, offset,
				    buffer, sizeof(buffer), &length, NULL) ==
				    FSIMG_OK);
				CHECK(length > 0);
				crc = crc_update(crc, buffer, length);
			}
			CHECK(crc_finish(crc, entry->size) ==
			    expected_crc(tree[i].source, tree[i].size));
		}
	}
	CHECK(status == FSIMG_DONE);
	fsimg_reader_free(reader);
	CHECK(fsimg_reader_open(fixture->ctx, input, NULL, &reader, NULL) ==
	    FSIMG_OK);
	status = fsimg_verify(reader, 0, &report, &error);
	if (status != FSIMG_OK)
		fprintf(stderr, "scale verify: %d %s\n", status, error.message);
	CHECK(status == FSIMG_OK && report.complete);
	/* Verification counts the root, which the listing does not show. */
	CHECK(report.files + report.directories == listed + 1);
	result->files = report.files;
	result->directories = report.directories;
	result->allocated = report.allocated_clusters;
	result->free = report.free_clusters;
	fsimg_reader_free(reader);
	fsimg_io_free(input);
}

/* Emit the plan into the sink and read it back. The sink takes no more
 * bytes than the metadata and the tree's content, and holds no more pages
 * than they fill, plus one for each edge an object or a 64-byte record
 * leaves within a page; either limit fails the write that crosses it.
 */
static void
scale_emit(fixture_t *fixture, const scale_case_t *sc,
    const fsimg_plan_info_t *info, page_sink_t *sink, scale_result_t *result)
{
	fsimg_stats_t before, after;
	fsimg_io_t *output;
	struct timespec start;
	uint64_t content;

	content = sc->tree ? tree_content_bytes() : 0;
	sink->length = info->volume_bytes;
	sink->write_limit = info->metadata_bytes + content;
	sink->slot_limit = (info->metadata_bytes + content) / PAGE_BYTES +
	    2 * (sc->tree ? TREE_COUNT : 0) + 1024;
	CHECK(page_io(fixture->ctx, sink, true, &output) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &before, NULL) == FSIMG_OK);
	CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
	CHECK(fsimg_writer_emit(fixture->writer, output, NULL) == FSIMG_OK);
	result->emit_seconds = seconds_since(&start);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &after, NULL) == FSIMG_OK);
	fsimg_io_free(output);
	result->zero_calls = sink->zero_calls;
	result->io_calls = after.io_calls - before.io_calls - sink->zero_calls;
	result->written_bytes = sink->written_bytes;
	CHECK(sink->zeroed_bytes == info->volume_bytes);

	CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
	scale_read_back(fixture, sink, result, sc->tree);
	result->verify_seconds = seconds_since(&start);
	/* Verification finds free exactly the blocks the plan left free; the
	 * tree's volume has none.
	 */
	CHECK(result->free * 512 == info->free_bytes);
	CHECK(!sc->tree || result->free == 0);
	result->sink_pages = sink->page_peak;
	result->sink_slots = sink->slot_count;
}

/* One case, in the calling process, with scratch in directory. */
static void
scale_run(const scale_case_t *sc, const char *directory,
    scale_result_t *result)
{
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_stats_t stats;
	fsimg_error_t error;
	page_sink_t sink;
	fixture_t fixture;
	struct timespec start;
	uint64_t baseline, sink_bytes;

	bzero(result, sizeof(*result));
	page_sink_open(&sink);
	baseline = peak_rss();
	scale_fixture_open(&fixture, sc, directory);
	CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
	sc->add(&fixture, sc);
	bzero(&error, sizeof(error));
	result->plan_status = fsimg_writer_plan(fixture.writer, &plan, &error);
	result->plan_seconds = seconds_since(&start);
	if (result->plan_status != sc->expect)
		fprintf(stderr, "scale %s: plan %d %s\n", sc->name,
		    result->plan_status, error.message);
	CHECK(result->plan_status == sc->expect);
	CHECK(sc->refusal == NULL || strstr(error.message, sc->refusal) !=
	    NULL);
	CHECK(sc->refusal_path == NULL || strcmp(error.path,
	    sc->refusal_path) == 0);
	CHECK(fsimg_ctx_get_stats(fixture.ctx, &stats, NULL) == FSIMG_OK);
	result->plan_scratch = stats.scratch_peak;
	CHECK(!sc->spills || result->plan_scratch > SCALE_BUDGET);
	if (result->plan_status == FSIMG_OK) {
		CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
		result->volume_bytes = info.volume_bytes;
		if (sc->kind == SCALE_EMIT)
			scale_emit(&fixture, sc, &info, &sink, result);
	}
	CHECK(fsimg_ctx_get_stats(fixture.ctx, &stats, NULL) == FSIMG_OK);
	result->memory_peak = stats.memory_peak;
	result->scratch_peak = stats.scratch_peak;
	fixture_release(&fixture);
	sink_bytes = page_sink_bytes(&sink);
	page_sink_close(&sink);
	result->rss_growth = peak_rss() - baseline;
	result->rss_growth = result->rss_growth > sink_bytes ?
	    result->rss_growth - sink_bytes : 0;
}

/* Where scratch directories go: QNX4_SCALE_SCRATCH, such as a RAM disk,
 * or the current directory.
 */
static const char *
scale_scratch_parent(void)
{
	const char *parent = getenv("QNX4_SCALE_SCRATCH");

	return (parent != NULL && parent[0] != '\0' ? parent : ".");
}

/* Run one case in a child process and collect its measurements. The
 * parent makes the scratch directory and removes it, so a child that fails
 * a check leaves nothing behind: its scratch files are unlinked as they
 * are created.
 */
static void
scale_measure(const scale_case_t *sc, scale_result_t *result)
{
	char directory[sizeof(((fixture_t *)0)->directory)];
	int channel[2], status;
	pid_t child;

	CHECK(snprintf(directory, sizeof(directory), "%s/qnx4-scale-XXXXXX",
	    scale_scratch_parent()) < (int)sizeof(directory));
	CHECK(mkdtemp(directory) != NULL);
	CHECK(pipe(channel) == 0);
	CHECK(fflush(stdout) == 0 && fflush(stderr) == 0);
	child = fork();
	CHECK(child != -1);
	if (child == 0) {
		CHECK(close(channel[0]) == 0);
		scale_run(sc, directory, result);
		CHECK(write(channel[1], result, sizeof(*result)) ==
		    (ssize_t)sizeof(*result));
		_exit(0);
	}
	CHECK(close(channel[1]) == 0);
	CHECK(waitpid(child, &status, 0) == child);
	if (WIFSIGNALED(status))
		fprintf(stderr, "scale: the child died of signal %d\n",
		    WTERMSIG(status));
	CHECK(rmdir(directory) == 0 || errno == ENOENT);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	CHECK(read(channel[0], result, sizeof(*result)) ==
	    (ssize_t)sizeof(*result));
	CHECK(close(channel[0]) == 0);
}

/* Whether the scratch filesystem has room for a case's scratch budget,
 * which the context enforces, and 1 GiB more; a case that does not fit is
 * skipped, not run.
 */
static bool
scale_room(const scale_case_t *sc)
{
	struct statvfs fs;
	uint64_t need;

	need = sc->scratch_bytes + (UINT64_C(1) << 30);
	CHECK(statvfs(scale_scratch_parent(), &fs) == 0);
	if ((uint64_t)fs.f_bavail * fs.f_frsize >= need)
		return (true);
	printf("SKIP: %s needs %" PRIu64 " bytes of free scratch space\n",
	    sc->name, need);
	return (false);
}

/* Run the cases of one kind, or those QNX4_SCALE_CASE names a part of.
 * Returns 77 when a case was skipped or none ran.
 */
static int
test_scale(scale_kind_t kind)
{
	static const scale_case_t cases[] = {
		{ .name = "2^31 blocks, full", .kind = SCALE_EMIT,
		    .volume_bytes = (UINT64_C(1) << 31) * 512,
		    .scratch_bytes = UINT64_C(1) << 30, .tree = true,
		    .add = scale_add_full, .expect = FSIMG_OK },
		{ .name = "200,000 files in one directory", .kind = SCALE_EMIT,
		    .scratch_bytes = UINT64_C(1) << 30, .spills = true,
		    .add = scale_add_wide, .count = 200000,
		    .expect = FSIMG_OK },
		{ .name = "4,194,288 long names", .kind = SCALE_LIMIT,
		    .memory_bytes = SCALE_LIMIT_BUDGET,
		    .scratch_bytes = UINT64_C(4) << 30, .add = scale_add_long,
		    .count = 4194288, .expect = FSIMG_OK },
		{ .name = "4,194,289 long names", .kind = SCALE_LIMIT,
		    .memory_bytes = SCALE_LIMIT_BUDGET,
		    .scratch_bytes = UINT64_C(4) << 30, .add = scale_add_long,
		    .count = 4194289, .expect = FSIMG_ERANGE,
		    .refusal = ".longfilenames exceeds 2 GiB" },
		{ .name = "33,554,366 children", .kind = SCALE_LIMIT,
		    .memory_bytes = SCALE_LIMIT_BUDGET,
		    .scratch_bytes = UINT64_C(24) << 30,
		    .max_entries = 33554368, .add = scale_add_children,
		    .count = 33554366, .expect = FSIMG_OK },
		{ .name = "33,554,367 children", .kind = SCALE_LIMIT,
		    .memory_bytes = SCALE_LIMIT_BUDGET,
		    .scratch_bytes = UINT64_C(24) << 30,
		    .max_entries = 33554368, .add = scale_add_children,
		    .count = 33554367, .expect = FSIMG_ERANGE,
		    .refusal = "directory exceeds 2 GiB", .refusal_path = "/d" }
	};
	scale_result_t r;
	const char *only;
	unsigned ran, skipped;

	only = getenv("QNX4_SCALE_CASE");
	ran = skipped = 0;
	printf("%-31s %4s %14s %8s %11s %11s %8s %8s %10s %6s %6s %9s %7s "
	    "%6s %6s\n", "case", "plan", "volume", "memory", "plan scr",
	    "scratch", "io", "zeroing", "written", "pages", "slots", "rss",
	    "plan s", "emit s", "read s");
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		if (cases[i].kind != kind || (only != NULL &&
		    strstr(cases[i].name, only) == NULL))
			continue;
		if (!scale_room(&cases[i])) {
			skipped++;
			continue;
		}
		scale_measure(&cases[i], &r);
		ran++;
		printf("%-31s %4d %14" PRIu64 " %8" PRIu64 " %11" PRIu64
		    " %11" PRIu64 " %8" PRIu64 " %8" PRIu64 " %10" PRIu64
		    " %6" PRIu64 " %6" PRIu64 " %9" PRIu64 " %7.2f %6.2f"
		    " %6.2f\n", cases[i].name, (int)r.plan_status,
		    r.volume_bytes, r.memory_peak, r.plan_scratch,
		    r.scratch_peak, r.io_calls, r.zero_calls,
		    r.written_bytes, r.sink_pages, r.sink_slots,
		    r.rss_growth, r.plan_seconds, r.emit_seconds,
		    r.verify_seconds);
		/* The resident bounds are the emitting cases'; the limit
		 * cases run under a budget large enough to plan quickly.
		 */
		CHECK(r.scratch_peak <= cases[i].scratch_bytes);
		if (kind == SCALE_EMIT) {
			CHECK(r.memory_peak <= SCALE_MEMORY);
			CHECK(r.rss_growth <= SCALE_RSS_GROWTH);
		}
	}
	return (skipped != 0 || ran == 0 ? 77 : 0);
}

/* Run one test; QNX4_WRITE_DEBUG names each as it starts. */
#define RUN(test) do { \
	if (getenv("QNX4_WRITE_DEBUG") != NULL) \
		fprintf(stderr, "%s\n", #test); \
	test(); \
} while (0)

int
main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--native") == 0)
		return (compare_native(argv[2]));
	if ((argc == 5 || argc == 6) && strcmp(argv[1], "--create") == 0)
		return (create_external(argv[2], argv[3], argv[4],
		    argc == 6 ? argv[5] : NULL));
	if (argc == 2 && strcmp(argv[1], "--scale") == 0)
		return (test_scale(SCALE_EMIT));
	if (argc == 2 && strcmp(argv[1], "--scale-limits") == 0)
		return (test_scale(SCALE_LIMIT));
	CHECK(argc == 1);
	RUN(test_checker);
	RUN(test_validation);
	RUN(test_matrix);
	RUN(test_tree_times);
	RUN(test_empty);
	RUN(test_sizing);
	RUN(test_limits);
	RUN(test_root_widths);
	RUN(test_inodes);
	RUN(test_boot);
	RUN(test_names);
	RUN(test_metadata);
	RUN(test_labels);
	RUN(test_faults);
	RUN(test_injected);
	RUN(test_page_sink);
	return (0);
}
