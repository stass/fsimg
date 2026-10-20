/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "format.h"
#include "fsimg.h"
#include "qnx4.h"
#include "reader.h"
#include "test.h"

/*
 * A synthetic QNX4 volume generator written from the format design
 * (specs/design-qnx4.md) with its own offset tables. It shares no code with
 * lib/formats/qnx4. The tests include the driver header to compare the
 * production decoders with the generator's encoders, to read two facts
 * no public interface reports (lost+found occupancy, and in --manifest
 * each record's stored link count) and to name the registered descriptors
 * in the probe tests; no generator code calls the driver.
 *
 * The generator lays a volume out as the native formatter does: the loader
 * in block 1, the root block, B bitmap blocks, the root directory, .inodes
 * and lost+found, then every object in allocation order from a cursor.
 * Objects live in a table; directories receive their slots as children
 * are added, and gen_finish() encodes every inode record, extent chain,
 * fixed slot and the bitmap from the table. Free blocks and the unused
 * tail of every file's last block hold a poison byte, so a reader exposing
 * them is caught. Tests then mutate the finished bytes.
 */

#define GEN_POISON	0xa5u
#define GEN_BLOCK	512u
#define GEN_DEFAULT_N	2000u

/* Offsets of the 64-byte inode record. */
enum {
	INODE_NAME = 0x00,
	INODE_SIZE = 0x10,
	INODE_FIRST = 0x14,
	INODE_BLOCKS = 0x18,
	INODE_CHAIN = 0x1c,
	INODE_TIMES = 0x20,	/* creation, modification, access, change */
	INODE_EXTENTS = 0x30,
	INODE_MODE = 0x32,
	INODE_UID = 0x34,
	INODE_GID = 0x36,
	INODE_LINKS = 0x38,
	INODE_RESERVED = 0x3a,
	INODE_TYPE = 0x3e,
	INODE_STATUS = 0x3f
};

/* Offsets of the 64-byte link record. */
enum {
	LINK_NAME = 0x00,
	LINK_BLOCK = 0x30,
	LINK_SLOT = 0x34,
	LINK_LONG = 0x35,
	LINK_SPARE = 0x39,
	LINK_STATUS = 0x3f
};

/* Offsets of the 512-byte extent block. */
enum {
	XBLK_NEXT = 0x000,
	XBLK_PREVIOUS = 0x004,
	XBLK_USED = 0x008,
	XBLK_SPARE = 0x009,
	XBLK_COVERED = 0x00c,
	XBLK_EXTENTS = 0x010,
	XBLK_SIGNATURE = 0x1f0,
	XBLK_FIRST = 0x1f8
};

/* Offsets of the 512-byte long-name record. */
enum {
	LONG_BLOCK = 0x000,
	LONG_SLOT = 0x004,
	LONG_STATE = 0x005,
	LONG_NAME = 0x006,
	LONG_TAIL = 0x1ff
};

/* Status bytes the formatter and filesystem write. */
enum {
	ST_DIRECT = 0x01,
	ST_WRITTEN = 0x03,
	ST_HIDDEN = 0x04,
	ST_LINK = 0x08,
	ST_TABLE = 0x11,
	ST_TABLE_WRITTEN = 0x13,
	ST_ROOT_CLEAN = 0x21
};

/* Object ids of the format-owned objects; user objects follow. */
enum {
	OBJ_ROOT = 0,
	OBJ_INODES,
	OBJ_BOOT,
	OBJ_ALTBOOT,
	OBJ_LOST,
	OBJ_BITMAP,
	OBJ_NAMES,
	OBJ_FIRST_USER
};

typedef struct gen_extent {
	uint32_t	 first;
	uint32_t	 blocks;
} gen_extent_t;

/* One object. record is the position (block * 8 + slot) of its inode
 * record: a directory slot for a direct record, an .inodes slot for an
 * object named by links, or a root-block record.
 */
typedef struct gen_object {
	gen_extent_t	*extents;
	size_t		 extent_count;
	uint32_t	*chain;		/* extent blocks, laid out by gen_finish */
	size_t		 chain_count;
	uint64_t	 record;
	uint32_t	 parent;
	uint32_t	 size;
	uint32_t	 times[4];
	uint32_t	 slots;		/* a directory's slots in use */
	uint32_t	 subdirs;
	uint32_t	 step;		/* a directory's growth extent */
	uint16_t	 mode;
	uint16_t	 uid;
	uint16_t	 gid;
	uint16_t	 links;
	uint8_t		 status;
	bool		 table;
	char		 name[17];	/* a direct record's name */
} gen_object_t;

typedef struct gen {
	unsigned char	*image;
	uint64_t	 length;
	bool		*used;		/* blocks 1..N in use */
	gen_object_t	*objects;
	uint32_t	 object_count;
	uint32_t	 blocks;	/* N */
	uint32_t	 bitmap_blocks;	/* B */
	uint32_t	 next_block;
	uint32_t	 table_slots;	/* .inodes slots in use, the signature too */
	uint32_t	 long_records;
	uint32_t	 dir_blocks;	/* new directories' extent */
	uint32_t	 root_next;	/* the root's next entry slot */
	uint8_t		 root_status;
	bool		 names;		/* .longfilenames exists */
	bool		 force_table;	/* place every new name in .inodes */
	bool		 finished;
	char		 label[49];
} gen_t;

static void put16(unsigned char *, uint16_t);
static void put32(unsigned char *, uint32_t);
static uint32_t get32(const unsigned char *);
static unsigned char *block_at(gen_t *, uint32_t);
static uint32_t gen_alloc(gen_t *, uint32_t, unsigned char);
static void gen_skip(gen_t *, uint32_t);
static uint32_t gen_object(gen_t *, uint16_t);
static void gen_add_extent(gen_t *, uint32_t, uint32_t, uint32_t);
static uint64_t gen_logical_position(gen_t *, uint32_t, uint64_t);
static unsigned char *gen_record_at(gen_t *, uint64_t);
static uint64_t gen_dir_slot(gen_t *, uint32_t);
static uint64_t gen_table_slot(gen_t *);
static uint32_t gen_long_record(gen_t *, const char *, uint64_t);
static void gen_write_link(gen_t *, uint64_t, const char *, uint64_t);
static void gen_name(gen_t *, uint32_t, uint32_t, const char *);
static void gen_write_dots(gen_t *, uint32_t);
static void gen_init(gen_t *, uint32_t);
static void gen_free(gen_t *);
static uint32_t gen_dir(gen_t *, uint32_t, const char *);
static uint32_t gen_file_parts(gen_t *, uint32_t, const char *,
    const unsigned char *, uint32_t, uint32_t);
static uint32_t gen_file(gen_t *, uint32_t, const char *,
    const unsigned char *, uint32_t);
static uint32_t gen_symlink(gen_t *, uint32_t, const char *, const char *);
static uint32_t gen_fifo(gen_t *, uint32_t, const char *, uint32_t);
static void gen_link(gen_t *, uint32_t, const char *, uint32_t);
static void gen_encode_inode(gen_t *, const gen_object_t *, unsigned char *,
    const char *);
static void gen_lay_chain(gen_t *, gen_object_t *);
static void gen_finish(gen_t *);
static void gen_set_bit(gen_t *, uint32_t, bool);

static void
put16(unsigned char *p, uint16_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
}

static void
put32(unsigned char *p, uint32_t v)
{
	for (unsigned i = 0; i < 4; i++)
		p[i] = (unsigned char)(v >> (8 * i));
}

static uint32_t
get32(const unsigned char *p)
{
	return ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	    (uint32_t)p[3] << 24);
}

/* Block b, numbered from 1, starts at byte 512 * (b - 1). */
static unsigned char *
block_at(gen_t *g, uint32_t block)
{
	CHECK(block >= 1 && block <= g->blocks);
	return (g->image + (uint64_t)(block - 1) * GEN_BLOCK);
}

/* Allocate count blocks at the cursor, filled with fill. */
static uint32_t
gen_alloc(gen_t *g, uint32_t count, unsigned char fill)
{
	uint32_t first = g->next_block;

	CHECK(count != 0 && (uint64_t)first + count - 1 <= g->blocks);
	for (uint32_t b = first; b < first + count; b++) {
		g->used[b] = true;
		memset(block_at(g, b), fill, GEN_BLOCK);
	}
	g->next_block += count;
	return (first);
}

/* Leave count free blocks, so the next allocation does not coalesce. */
static void
gen_skip(gen_t *g, uint32_t count)
{
	g->next_block += count;
	CHECK(g->next_block <= g->blocks + 1);
}

static uint32_t
gen_object(gen_t *g, uint16_t mode)
{
	gen_object_t *o;

	g->objects = realloc(g->objects, (g->object_count + 1) *
	    sizeof(*g->objects));
	CHECK(g->objects != NULL);
	o = &g->objects[g->object_count];
	bzero(o, sizeof(*o));
	o->mode = mode;
	o->links = 1;
	for (unsigned i = 0; i < 4; i++)
		o->times[i] = 1000000000u + 100 * i + g->object_count;
	return (g->object_count++);
}

static void
gen_add_extent(gen_t *g, uint32_t id, uint32_t first, uint32_t blocks)
{
	gen_object_t *o = &g->objects[id];

	o->extents = realloc(o->extents, (o->extent_count + 1) *
	    sizeof(*o->extents));
	CHECK(o->extents != NULL);
	o->extents[o->extent_count].first = first;
	o->extents[o->extent_count].blocks = blocks;
	o->extent_count++;
}

/* The physical position of logical 64-byte record `index` of object id. */
static uint64_t
gen_logical_position(gen_t *g, uint32_t id, uint64_t index)
{
	const gen_object_t *o = &g->objects[id];
	uint64_t block = index / 8;

	for (size_t i = 0; i < o->extent_count; i++) {
		if (block < o->extents[i].blocks)
			return ((o->extents[i].first + block) * 8 + index % 8);
		block -= o->extents[i].blocks;
	}
	CHECK(!"logical record beyond the object's extents");
	return (0);
}

static unsigned char *
gen_record_at(gen_t *g, uint64_t position)
{
	return (block_at(g, (uint32_t)(position / 8)) + position % 8 * 64);
}

/* The next slot of directory id, growing it by one extent when full. */
static uint64_t
gen_dir_slot(gen_t *g, uint32_t id)
{
	gen_object_t *d = &g->objects[id];
	uint32_t first, step;

	if (id == OBJ_ROOT) {
		/* Slot 8 is .longfilenames' when it exists. */
		if (g->root_next == 8)
			g->root_next = 9;
		d->slots = g->root_next + 1;
		g->root_next = g->root_next == 7 ? 8 : g->root_next + 1;
		if (d->slots > d->size / 64) {
			step = d->step;
			first = gen_alloc(g, step, 0);
			gen_add_extent(g, id, first, step);
			d = &g->objects[id];
			d->size += step * GEN_BLOCK;
		}
		return (gen_logical_position(g, id, d->slots - 1));
	}
	if (d->slots == d->size / 64) {
		step = d->step;
		first = gen_alloc(g, step, 0);
		gen_add_extent(g, id, first, step);
		d = &g->objects[id];
		d->size += step * GEN_BLOCK;
	}
	return (gen_logical_position(g, id, d->slots++));
}

/* The next free .inodes slot, growing the table by 32 blocks when full. */
static uint64_t
gen_table_slot(gen_t *g)
{
	gen_object_t *t = &g->objects[OBJ_INODES];
	uint32_t first;

	if (g->table_slots == t->size / 64) {
		first = gen_alloc(g, 32, 0);
		gen_add_extent(g, OBJ_INODES, first, 32);
		g->objects[OBJ_INODES].size += 32 * GEN_BLOCK;
	}
	return (gen_logical_position(g, OBJ_INODES, g->table_slots++));
}

/* Write a long-name record for the link at link_position and return its
 * block. The first long name creates .longfilenames' 16 blocks.
 */
static uint32_t
gen_long_record(gen_t *g, const char *name, uint64_t link_position)
{
	gen_object_t *n = &g->objects[OBJ_NAMES];
	unsigned char *record;
	uint32_t block, first;

	CHECK(g->names);
	if (g->long_records == n->size / GEN_BLOCK) {
		first = gen_alloc(g, 16, 0);
		gen_add_extent(g, OBJ_NAMES, first, 16);
		g->objects[OBJ_NAMES].size += 16 * GEN_BLOCK;
	}
	block = (uint32_t)(gen_logical_position(g, OBJ_NAMES,
	    (uint64_t)g->long_records * 8) / 8);
	g->long_records++;
	record = block_at(g, block);
	bzero(record, GEN_BLOCK);
	put32(record + LONG_BLOCK, (uint32_t)(link_position / 8));
	record[LONG_SLOT] = (unsigned char)(link_position % 8);
	record[LONG_STATE] = 0x01;
	CHECK(strlen(name) <= 505);
	memcpy(record + LONG_NAME, name, strlen(name));
	return (block);
}

/* Write a link record at position naming target, with a long-name record
 * and mangled prefix for a name over 48 bytes.
 */
static void
gen_write_link(gen_t *g, uint64_t position, const char *name, uint64_t target)
{
	unsigned char *slot = gen_record_at(g, position);
	size_t length = strlen(name);
	uint32_t long_block;
	char mangled[49];

	bzero(slot, 64);
	if (length > 48) {
		long_block = gen_long_record(g, name, position);
		snprintf(mangled, sizeof(mangled), "%.38s~%08" PRIx32, name,
		    long_block);
		memcpy(slot + LINK_NAME, mangled, 47);
		put32(slot + LINK_LONG, long_block);
	} else {
		memcpy(slot + LINK_NAME, name, length);
	}
	put32(slot + LINK_BLOCK, (uint32_t)(target / 8));
	slot[LINK_SLOT] = (unsigned char)(target % 8);
	slot[LINK_STATUS] = ST_LINK;
}

/* Name object id in directory parent: a direct record for 1 to 16 bytes,
 * otherwise (or when forced) a link record and an .inodes record.
 */
static void
gen_name(gen_t *g, uint32_t parent, uint32_t id, const char *name)
{
	uint64_t slot;

	slot = gen_dir_slot(g, parent);
	g->objects[id].parent = parent;
	if (strlen(name) <= 16 && !g->force_table) {
		g->objects[id].record = slot;
		strcpy(g->objects[id].name, name);
		return;
	}
	g->objects[id].table = true;
	g->objects[id].record = gen_table_slot(g);
	gen_write_link(g, slot, name, g->objects[id].record);
}

/* The "." and ".." slots of directory id: "." names its own record and
 * carries its first extent's length, ".." names its parent's record.
 */
static void
gen_write_dots(gen_t *g, uint32_t id)
{
	const gen_object_t *d = &g->objects[id];
	unsigned char *dot = gen_record_at(g, gen_logical_position(g, id, 0));
	unsigned char *dotdot = gen_record_at(g, gen_logical_position(g, id, 1));
	uint64_t parent;

	bzero(dot, 64);
	memcpy(dot, ".\0I\003QNX", 7);
	put32(dot + 8, d->extents[0].blocks);
	put32(dot + LINK_BLOCK, (uint32_t)(d->record / 8));
	dot[LINK_SLOT] = (unsigned char)(d->record % 8);
	dot[LINK_STATUS] = ST_LINK;
	if (id == OBJ_LOST)
		parent = (uint64_t)(3 + g->bitmap_blocks) * 8;
	else
		parent = g->objects[d->parent].record;
	bzero(dotdot, 64);
	memcpy(dotdot, "..", 2);
	put32(dotdot + LINK_BLOCK, (uint32_t)(parent / 8));
	dotdot[LINK_SLOT] = (unsigned char)(parent % 8);
	dotdot[LINK_STATUS] = ST_LINK;
}

/* A fresh volume of `blocks` blocks in the formatter's default layout. */
static void
gen_init(gen_t *g, uint32_t blocks)
{
	uint32_t first;

	bzero(g, sizeof(*g));
	g->blocks = blocks;
	g->bitmap_blocks = (blocks + 4095) / 4096;
	g->length = (uint64_t)blocks * GEN_BLOCK;
	g->image = malloc((size_t)g->length);
	g->used = calloc((size_t)blocks + 2, sizeof(*g->used));
	CHECK(g->image != NULL && g->used != NULL);
	memset(g->image, GEN_POISON, (size_t)g->length);
	g->dir_blocks = 8;
	g->root_next = 7;
	g->root_status = ST_ROOT_CLEAN;
	g->names = true;
	g->next_block = 1;

	/* The loader: a jump, text and a boot signature, but no BPB. */
	gen_alloc(g, 1, 0);
	memcpy(block_at(g, 1), "\xeb\x3e\x90QNX4 loader", 14);
	block_at(g, 1)[510] = 0x55;
	block_at(g, 1)[511] = 0xaa;
	gen_alloc(g, 1, 0);
	gen_alloc(g, g->bitmap_blocks, 0);

	for (uint32_t id = 0; id < OBJ_FIRST_USER; id++)
		gen_object(g, 0);
	g->objects[OBJ_ROOT].mode = 040775;
	g->objects[OBJ_ROOT].record = 2 * 8 + 0;
	g->objects[OBJ_ROOT].status = ST_ROOT_CLEAN;
	g->objects[OBJ_ROOT].step = 8;
	g->objects[OBJ_INODES].mode = 0100444;
	g->objects[OBJ_INODES].record = 2 * 8 + 1;
	g->objects[OBJ_INODES].status = ST_TABLE;
	g->objects[OBJ_BOOT].mode = 0100600;
	g->objects[OBJ_BOOT].record = 2 * 8 + 2;
	g->objects[OBJ_BOOT].status = ST_TABLE;
	g->objects[OBJ_ALTBOOT].mode = 0100600;
	g->objects[OBJ_ALTBOOT].record = 2 * 8 + 3;
	g->objects[OBJ_ALTBOOT].status = ST_TABLE;
	g->objects[OBJ_LOST].mode = 040755;
	g->objects[OBJ_LOST].record = 2 * 8 + 4;
	g->objects[OBJ_LOST].status = ST_TABLE;
	g->objects[OBJ_BITMAP].mode = 0100444;
	g->objects[OBJ_NAMES].mode = 0100444;

	first = gen_alloc(g, 8, 0);
	gen_add_extent(g, OBJ_ROOT, first, 8);
	g->objects[OBJ_ROOT].size = 8 * GEN_BLOCK;
	g->objects[OBJ_ROOT].slots = 9;
	first = gen_alloc(g, 16, 0);
	gen_add_extent(g, OBJ_INODES, first, 16);
	g->objects[OBJ_INODES].size = 16 * GEN_BLOCK;
	g->table_slots = 1;
	first = gen_alloc(g, 16, 0);
	gen_add_extent(g, OBJ_LOST, first, 16);
	g->objects[OBJ_LOST].size = 16 * GEN_BLOCK;
	g->objects[OBJ_LOST].slots = 2;
	g->objects[OBJ_ROOT].record = 2 * 8 + 0;
	g->objects[OBJ_ROOT].parent = OBJ_ROOT;
	g->objects[OBJ_LOST].parent = OBJ_ROOT;
	gen_add_extent(g, OBJ_BITMAP, 3, g->bitmap_blocks);
	g->objects[OBJ_BITMAP].size = (blocks + 7) / 8;
	g->objects[OBJ_BITMAP].record = gen_logical_position(g, OBJ_ROOT, 2);
	g->objects[OBJ_BITMAP].status = ST_DIRECT;
	strcpy(g->objects[OBJ_BITMAP].name, ".bitmap");
	g->objects[OBJ_NAMES].record = gen_logical_position(g, OBJ_ROOT, 8);
	g->objects[OBJ_NAMES].status = ST_DIRECT;
	strcpy(g->objects[OBJ_NAMES].name, ".longfilenames");
}

static void
gen_free(gen_t *g)
{
	for (uint32_t i = 0; i < g->object_count; i++) {
		free(g->objects[i].extents);
		free(g->objects[i].chain);
	}
	free(g->objects);
	free(g->used);
	free(g->image);
	bzero(g, sizeof(*g));
}

static uint32_t
gen_dir(gen_t *g, uint32_t parent, const char *name)
{
	uint32_t id, first;

	id = gen_object(g, 040755);
	g->objects[id].step = g->dir_blocks;
	gen_name(g, parent, id, name);
	g->objects[id].status = g->objects[id].table ? ST_TABLE : ST_DIRECT;
	first = gen_alloc(g, g->dir_blocks, 0);
	gen_add_extent(g, id, first, g->dir_blocks);
	g->objects[id].size = g->dir_blocks * GEN_BLOCK;
	g->objects[id].slots = 2;
	gen_write_dots(g, id);
	g->objects[parent].subdirs++;
	return (id);
}

/* A regular file of size bytes in `parts` extents separated by free
 * blocks; the last block's unused tail keeps the poison.
 */
static uint32_t
gen_file_parts(gen_t *g, uint32_t parent, const char *name,
    const unsigned char *bytes, uint32_t size, uint32_t parts)
{
	uint32_t id, blocks, first, count, done;

	id = gen_object(g, 0100644);
	g->objects[id].status = ST_WRITTEN;
	gen_name(g, parent, id, name);
	g->objects[id].size = size;
	blocks = size / GEN_BLOCK + (size % GEN_BLOCK != 0);
	CHECK(parts <= blocks);
	done = 0;
	for (uint32_t p = 0; p < parts; p++) {
		count = p + 1 < parts ? 1 : blocks - done;
		first = gen_alloc(g, count, GEN_POISON);
		gen_add_extent(g, id, first, count);
		for (uint32_t b = 0; b < count; b++) {
			uint64_t offset = (uint64_t)(done + b) * GEN_BLOCK;
			uint64_t n = size - offset < GEN_BLOCK ? size - offset :
			    GEN_BLOCK;

			memcpy(block_at(g, first + b), bytes + offset, (size_t)n);
		}
		done += count;
		if (p + 1 < parts)
			gen_skip(g, 1);
	}
	if (g->objects[id].table)
		g->objects[id].status = ST_TABLE_WRITTEN;
	return (id);
}

static uint32_t
gen_file(gen_t *g, uint32_t parent, const char *name,
    const unsigned char *bytes, uint32_t size)
{
	return (gen_file_parts(g, parent, name, bytes, size, size == 0 ? 0 : 1));
}

static uint32_t
gen_symlink(gen_t *g, uint32_t parent, const char *name, const char *target)
{
	uint32_t id;

	id = gen_file(g, parent, name, (const unsigned char *)target,
	    (uint32_t)strlen(target));
	g->objects[id].mode = 0120777;
	g->objects[id].status = g->objects[id].table ? ST_TABLE : ST_DIRECT;
	return (id);
}

/* A FIFO owning `blocks` blocks it never writes, as native mkfifo does. */
static uint32_t
gen_fifo(gen_t *g, uint32_t parent, const char *name, uint32_t blocks)
{
	uint32_t id, first;

	id = gen_object(g, 010664);
	gen_name(g, parent, id, name);
	g->objects[id].status = g->objects[id].table ? ST_TABLE : ST_DIRECT;
	g->objects[id].size = blocks * GEN_BLOCK;
	if (blocks != 0) {
		first = gen_alloc(g, blocks, GEN_POISON);
		gen_add_extent(g, id, first, blocks);
	}
	return (id);
}

/* Another name for an .inodes object: a link record naming its slot. */
static void
gen_link(gen_t *g, uint32_t parent, const char *name, uint32_t id)
{
	uint64_t slot;

	CHECK(g->objects[id].table);
	slot = gen_dir_slot(g, parent);
	gen_write_link(g, slot, name, g->objects[id].record);
	g->objects[id].links++;
}

/* Encode an object's inode record; name is its direct name or NULL. */
static void
gen_encode_inode(gen_t *g FSIMG_UNUSED, const gen_object_t *o,
    unsigned char *raw, const char *name)
{
	bzero(raw, 64);
	if (name != NULL)
		memcpy(raw + INODE_NAME, name, strlen(name));
	put32(raw + INODE_SIZE, o->size);
	if (o->extent_count != 0) {
		put32(raw + INODE_FIRST, o->extents[0].first);
		put32(raw + INODE_BLOCKS, o->extents[0].blocks);
	}
	put32(raw + INODE_CHAIN, o->chain_count != 0 ? o->chain[0] : 0);
	for (unsigned i = 0; i < 4; i++)
		put32(raw + INODE_TIMES + 4 * i, o->times[i]);
	put16(raw + INODE_EXTENTS, (uint16_t)o->extent_count);
	put16(raw + INODE_MODE, o->mode);
	put16(raw + INODE_UID, o->uid);
	put16(raw + INODE_GID, o->gid);
	put16(raw + INODE_LINKS, o->links);
	raw[INODE_STATUS] = o->status;
}

/* Lay out the extent blocks of an object with more than one extent: the
 * extents after the first, sixty to a block, every block full but the last.
 */
static void
gen_lay_chain(gen_t *g, gen_object_t *o)
{
	size_t rest, used;
	unsigned char *x;
	uint32_t covered;

	if (o->extent_count < 2)
		return;
	rest = o->extent_count - 1;
	o->chain_count = (rest + 59) / 60;
	o->chain = calloc(o->chain_count, sizeof(*o->chain));
	CHECK(o->chain != NULL);
	for (size_t i = 0; i < o->chain_count; i++)
		o->chain[i] = gen_alloc(g, 1, 0);
	for (size_t i = 0; i < o->chain_count; i++) {
		x = block_at(g, o->chain[i]);
		used = rest - i * 60 > 60 ? 60 : rest - i * 60;
		put32(x + XBLK_NEXT, i + 1 < o->chain_count ? o->chain[i + 1] : 0);
		put32(x + XBLK_PREVIOUS, i == 0 ? 0 : o->chain[i - 1]);
		x[XBLK_USED] = (unsigned char)used;
		covered = 0;
		for (size_t j = 0; j < used; j++) {
			const gen_extent_t *e = &o->extents[1 + i * 60 + j];

			put32(x + XBLK_EXTENTS + j * 8, e->first);
			put32(x + XBLK_EXTENTS + j * 8 + 4, e->blocks);
			covered += e->blocks;
		}
		put32(x + XBLK_COVERED, covered);
		memcpy(x + XBLK_SIGNATURE, "IamXblk", 8);
		if (i == 0) {
			put32(x + XBLK_FIRST, o->extents[0].first);
			put32(x + XBLK_FIRST + 4, o->extents[0].blocks);
		}
	}
}

/* Encode everything the table describes: chains, records, the root
 * block's records and label, the fixed slots, the .inodes signature, link
 * counts and the bitmap.
 */
static void
gen_finish(gen_t *g)
{
	unsigned char *root_block, *slot, *bitmap;
	uint64_t position;
	gen_object_t *o;

	CHECK(!g->finished);
	g->finished = true;
	for (uint32_t id = 0; id < g->object_count; id++)
		gen_lay_chain(g, &g->objects[id]);
	g->objects[OBJ_ROOT].links = (uint16_t)(2 + g->objects[OBJ_ROOT].subdirs + 1);
	g->objects[OBJ_ROOT].status = g->root_status;
	g->objects[OBJ_LOST].links = 2;
	for (uint32_t id = OBJ_FIRST_USER; id < g->object_count; id++) {
		o = &g->objects[id];
		if ((o->mode & 0170000) == 040000)
			o->links = (uint16_t)(2 + o->subdirs);
	}

	/* Object records: root-block records, direct slots, .inodes slots. */
	root_block = block_at(g, 2);
	bzero(root_block, GEN_BLOCK);
	for (uint32_t id = 0; id < g->object_count; id++) {
		o = &g->objects[id];
		if (id == OBJ_NAMES && !g->names)
			continue;
		if (id == OBJ_ROOT) {
			gen_encode_inode(g, o, root_block, "/");
			continue;
		}
		gen_encode_inode(g, o, gen_record_at(g, o->record),
		    o->table || o->record / 8 == 2 ? NULL : o->name);
	}
	memcpy(root_block + 0x1c0, g->label, strlen(g->label));

	/* The root's fixed slots and lost+found's dots. */
	gen_write_dots(g, OBJ_ROOT);
	gen_write_dots(g, OBJ_LOST);
	gen_write_link(g, gen_logical_position(g, OBJ_ROOT, 3), ".inodes",
	    2 * 8 + 1);
	gen_write_link(g, gen_logical_position(g, OBJ_ROOT, 4), ".boot",
	    2 * 8 + 2);
	gen_write_link(g, gen_logical_position(g, OBJ_ROOT, 5), ".altboot",
	    2 * 8 + 3);
	position = gen_logical_position(g, OBJ_ROOT, 6);
	gen_write_link(g, position, "lost+found", 2 * 8 + 4);
	gen_record_at(g, position)[LINK_STATUS] = ST_HIDDEN;
	if (g->root_next == 7) {
		slot = gen_record_at(g, gen_logical_position(g, OBJ_ROOT, 7));
		bzero(slot, 64);
		slot[INODE_STATUS] = 0x02;
	}

	/* The .inodes signature. */
	slot = gen_record_at(g, gen_logical_position(g, OBJ_INODES, 0));
	bzero(slot, 64);
	memcpy(slot, "IamTHE.inodeFILE", 16);
	put32(slot + 16, 0x00100000u);

	/* The bitmap: allocated blocks, set padding bits, 0xff bytes. */
	bitmap = block_at(g, 3);
	memset(bitmap, 0, (size_t)g->bitmap_blocks * GEN_BLOCK);
	for (uint32_t b = 1; b <= g->blocks; b++)
		if (g->used[b])
			bitmap[(b - 1) / 8] |= (unsigned char)(1u << ((b - 1) % 8));
	for (uint64_t k = g->blocks; k < (uint64_t)g->bitmap_blocks * 4096; k++)
		bitmap[k / 8] |= (unsigned char)(1u << (k % 8));
}

static void
gen_set_bit(gen_t *g, uint32_t block, bool set)
{
	unsigned char *byte = block_at(g, 3) + (block - 1) / 8;
	unsigned char mask = (unsigned char)(1u << ((block - 1) % 8));

	if (set)
		*byte |= mask;
	else
		*byte &= (unsigned char)~mask;
}

/*
 * The fixture: a volume behind the callback adapter, with read accounting
 * and fault injection, in a context with a 16 MiB budget and a private
 * scratch directory. fixture_free checks that nothing outlived the reader.
 */

typedef struct fixture {
	fsimg_ctx_t	*ctx;
	fsimg_io_t	*io;
	fsimg_reader_t	*reader;
	const unsigned char *image;
	uint64_t	 length;	/* the slice length */
	uint64_t	 memory;	/* charged before any I/O object */
	uint64_t	 watch;		/* count reads touching this byte */
	uint64_t	 watch_hits;
	uint64_t	 fail_first;	/* reads touching [first, end) fail */
	uint64_t	 fail_end;
	size_t		 short_max;	/* nonzero: the most one read returns */
	uint64_t	 polls;		/* cancellation callbacks so far */
	uint64_t	 cancel_after;	/* nonzero: cancel at this poll */
	int		 scratch;
	char		 directory[1024];
} fixture_t;

static void temp_name(char *, size_t, const char *);
static fsimg_status_t image_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static fsimg_status_t image_size(void *, uint64_t *, fsimg_error_t *);
static uint64_t charged_memory(fsimg_ctx_t *);
static bool cancel_poll(void *);
static void fixture_limited(fixture_t *, const unsigned char *, uint64_t,
    uint64_t, uint32_t);
static void fixture_new(fixture_t *, const unsigned char *, uint64_t);
static void fixture_free(fixture_t *);
static fsimg_status_t walk_all(fixture_t *, uint64_t *, fsimg_error_t *);
static fsimg_status_t run_image(const unsigned char *, uint64_t, uint64_t *,
    fsimg_error_t *);
static void expect_length(gen_t *, uint64_t, fsimg_status_t, const char *);
static void expect(gen_t *, fsimg_status_t, const char *);

static void
temp_name(char *buffer, size_t size, const char *prefix)
{
	const char *directory = getenv("TMPDIR");

	if (directory == NULL || directory[0] == '\0')
		directory = "/tmp";
	CHECK((size_t)snprintf(buffer, size, "%s/%s-XXXXXX", directory,
	    prefix) < size);
}

static fsimg_status_t
image_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error)
{
	fixture_t *f = argument;

	CHECK(offset <= f->length && length <= f->length - offset);
	*transferred = 0;
	if (offset < f->fail_end && offset + length > f->fail_first)
		return (fsimg_error_set(error, FSIMG_EIO, EIO, FSIMG_OP_IO,
		    "injected QNX4 read failure"));
	if (f->short_max != 0 && length > f->short_max)
		length = f->short_max;
	if (f->watch >= offset && f->watch < offset + length)
		f->watch_hits++;
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

static uint64_t
charged_memory(fsimg_ctx_t *ctx)
{
	fsimg_stats_t stats;

	CHECK(fsimg_ctx_get_stats(ctx, &stats, NULL) == FSIMG_OK);
	return (stats.memory_bytes);
}

static bool
cancel_poll(void *argument)
{
	fixture_t *f = argument;

	f->polls++;
	return (f->cancel_after != 0 && f->polls >= f->cancel_after);
}

/* A fixture over the first length bytes of image; max_work, when nonzero,
 * limits the session, and cache_pages, when nonzero, shrinks the metadata
 * cache so small volumes already spill to scratch.
 */
static void
fixture_limited(fixture_t *f, const unsigned char *image, uint64_t length,
    uint64_t max_work, uint32_t cache_pages)
{
	fsimg_options_t options;
	fsimg_io_ops_t ops;

	bzero(f, sizeof(*f));
	f->image = image;
	f->length = length;
	f->watch = UINT64_MAX;
	temp_name(f->directory, sizeof(f->directory), "fsimg-qnx4");
	CHECK(mkdtemp(f->directory) != NULL);
	f->scratch = open(f->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(f->scratch != -1);
	fsimg_options_init(&options);
	options.memory_bytes = 16 * 1024 * 1024;
	options.scratch_directory_fd = f->scratch;
	if (max_work != 0)
		options.max_work = max_work;
	options.cancelled = cancel_poll;
	options.cancel_argument = f;
	CHECK(fsimg_ctx_new(&options, &f->ctx, NULL) == FSIMG_OK);
	if (cache_pages != 0)
		CHECK(fsimg_test_meta_cache(f->ctx, cache_pages, NULL) ==
		    FSIMG_OK);
	f->memory = charged_memory(f->ctx);
	fsimg_io_ops_init(&ops);
	ops.read_at = image_read;
	ops.get_size = image_size;
	CHECK(fsimg_io_from_callbacks(f->ctx, &ops, f, &f->io, NULL) == FSIMG_OK);
}

static void
fixture_new(fixture_t *f, const unsigned char *image, uint64_t length)
{
	fixture_limited(f, image, length, 0, 0);
}

static void
fixture_free(fixture_t *f)
{
	fsimg_stats_t stats;

	fsimg_reader_free(f->reader);
	fsimg_io_free(f->io);
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.scratch_bytes == 0 && stats.descriptors == 1);
	CHECK(stats.memory_bytes == f->memory);
	fsimg_ctx_free(f->ctx);
	CHECK(close(f->scratch) == 0);
	CHECK(rmdir(f->directory) == 0);
}

/* Walk every entry, reading each file and symbolic link completely; no
 * read may return more than the bytes left before the entry's size.
 */
static fsimg_status_t
walk_all(fixture_t *f, uint64_t *entries, fsimg_error_t *error)
{
	const fsimg_entry_t *entry;
	unsigned char buffer[3000];
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
			CHECK(count != 0 && count <= entry->size - offset);
		}
	}
}

/* Open and list a volume; *entries counts what the listing exposed. */
static fsimg_status_t
run_image(const unsigned char *image, uint64_t length, uint64_t *entries,
    fsimg_error_t *error)
{
	fixture_t f;
	uint64_t count;
	fsimg_status_t status;

	fixture_new(&f, image, length);
	count = 0;
	status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, error);
	if (status == FSIMG_OK)
		status = walk_all(&f, &count, error);
	fixture_free(&f);
	if (entries != NULL)
		*entries = count;
	return (status);
}

/* QNX4_TRACE=1 shows each case's diagnostic, to confirm that a mutation
 * fails for its intended reason.
 */
static void
expect_length(gen_t *g, uint64_t length, fsimg_status_t expected,
    const char *label)
{
	fsimg_error_t error;
	fsimg_status_t status;

	bzero(&error, sizeof(error));
	status = run_image(g->image, length, NULL, &error);
	if (getenv("QNX4_TRACE") != NULL)
		fprintf(stderr, "TRACE %s: %d %s\n", label, status, error.message);
	if (status != expected)
		fprintf(stderr, "%s: expected %d, got %d (%s)\n", label,
		    expected, status, error.message);
	CHECK(status == expected);
}

static void
expect(gen_t *g, fsimg_status_t expected, const char *label)
{
	expect_length(g, g->length, expected, label);
}

static unsigned char *
pattern(uint64_t length, unsigned seed)
{
	unsigned char *bytes = malloc(length + 1);

	CHECK(bytes != NULL);
	for (uint64_t i = 0; i < length; i++)
		bytes[i] = (unsigned char)(i * 7 + seed + i / 251);
	return (bytes);
}

static char *
repeat(char c, size_t length)
{
	char *name = malloc(length + 1);

	CHECK(name != NULL);
	memset(name, c, length);
	name[length] = '\0';
	return (name);
}

/*
 * The sample volume: every entry kind, names at the length boundaries of
 * each record kind, a nested tree, a hard link, a file with an extent
 * chain and full metadata on one file.
 */
typedef struct sample {
	uint32_t	 one;
	uint32_t	 chained;	/* 61 extents: one extent block */
	uint32_t	 d1;
	uint32_t	 d2;
	uint32_t	 nested;
	uint32_t	 linked;	/* in .inodes with two names */
	uint32_t	 symlink;
	uint32_t	 fifo;
	uint32_t	 attrs;
	uint32_t	 long505;
	uint32_t	 name16;
	uint32_t	 name48;
} sample_t;

#define SAMPLE_ENTRIES 15u

static void
sample_build(gen_t *g, sample_t *s, uint32_t blocks)
{
	unsigned char *bytes;
	char *n16, *n48, *n505;
	gen_object_t *a;

	gen_init(g, blocks);
	n16 = repeat('p', 16);
	n48 = repeat('q', 48);
	n505 = repeat('r', 505);
	bytes = pattern(64 * 512, 9);
	s->one = gen_file(g, OBJ_ROOT, "one", (const unsigned char *)"x", 1);
	s->chained = gen_file_parts(g, OBJ_ROOT, "chained", bytes, 61 * 512 - 100,
	    61);
	s->name16 = gen_file(g, OBJ_ROOT, n16, bytes, 700);
	s->name48 = gen_file(g, OBJ_ROOT, n48, bytes + 1, 513);
	s->long505 = gen_file(g, OBJ_ROOT, n505, bytes + 2, 2);
	s->d1 = gen_dir(g, OBJ_ROOT, "d1");
	s->d2 = gen_dir(g, s->d1, "d2-with-a-long-name");
	s->nested = gen_file(g, s->d2, "f", (const unsigned char *)"sub", 3);
	g->force_table = true;
	s->linked = gen_file(g, OBJ_ROOT, "linked", bytes + 3, 1500);
	g->force_table = false;
	gen_link(g, s->d1, "hardlink", s->linked);
	s->symlink = gen_symlink(g, OBJ_ROOT, "symlink", "d1/hardlink");
	s->fifo = gen_fifo(g, OBJ_ROOT, "fifo", 10);
	s->attrs = gen_file(g, OBJ_ROOT, "attrs", (const unsigned char *)"m", 1);
	a = &g->objects[s->attrs];
	a->uid = 65535;
	a->gid = 65534;
	a->mode = 0104751;
	a->times[0] = 0;
	a->times[1] = UINT32_C(0xffffffff);
	a->times[2] = UINT32_C(0x7fffffff);
	a->times[3] = 1;
	gen_finish(g);
	free(bytes);
	free(n16);
	free(n48);
	free(n505);
}

static void
sample(gen_t *g, sample_t *s)
{
	sample_build(g, s, GEN_DEFAULT_N);
}

/* The record bytes of object id. */
static unsigned char *
record_of(gen_t *g, uint32_t id)
{
	return (gen_record_at(g, g->objects[id].record));
}

/* The bytes of directory slot `index` of directory id. */
static unsigned char *
slot_of(gen_t *g, uint32_t dir, uint64_t index)
{
	return (gen_record_at(g, gen_logical_position(g, dir, index)));
}

/* The slot index in dir of the link record naming position target. */
static uint64_t
link_slot(gen_t *g, uint32_t dir, uint64_t target)
{
	unsigned char *slot;

	for (uint64_t i = 2; i < g->objects[dir].size / 64; i++) {
		slot = slot_of(g, dir, i);
		if ((slot[LINK_STATUS] & 0x08) != 0 &&
		    get32(slot + LINK_BLOCK) == target / 8 &&
		    slot[LINK_SLOT] == target % 8)
			return (i);
	}
	CHECK(!"no link to the target");
	return (0);
}

/*
 * Tests.
 */

/* The production decoders read what the generator's offset tables write. */
static void
test_codecs(void)
{
	gen_object_t o;
	unsigned char raw[64], block[512];
	qnx4_inode_t inode;
	qnx4_link_t link;
	qnx4_xblock_t xblock;
	gen_extent_t extents[2] = { { 77, 5 }, { 90, 3 } };
	uint32_t chain = 123;

	bzero(&o, sizeof(o));
	o.extents = extents;
	o.extent_count = 2;
	o.chain = &chain;
	o.chain_count = 1;
	o.size = 3500;
	o.times[0] = 1;
	o.times[1] = 2;
	o.times[2] = 3;
	o.times[3] = 4;
	o.mode = 0100640;
	o.uid = 65535;
	o.gid = 7;
	o.links = 3;
	o.status = ST_TABLE_WRITTEN;
	gen_encode_inode(NULL, &o, raw, "abcdefghijklmnop");
	qnx4_inode_decode(raw, &inode);
	CHECK(inode.name_length == 16 && inode.name_padding_zero);
	CHECK(inode.size == 3500 && inode.first_extent.first == 77 &&
	    inode.first_extent.blocks == 5 && inode.chain == 123);
	CHECK(inode.created == 1 && inode.modified == 2 && inode.accessed == 3 &&
	    inode.changed == 4);
	CHECK(inode.extents == 2 && inode.mode == 0100640 &&
	    inode.uid == 65535 && inode.gid == 7 && inode.links == 3);
	CHECK(inode.status == ST_TABLE_WRITTEN && inode.reserved_zero &&
	    inode.type == 0);
	raw[INODE_RESERVED + 3] = 1;
	raw[2] = '\0';
	qnx4_inode_decode(raw, &inode);
	CHECK(!inode.reserved_zero && inode.name_length == 2 &&
	    !inode.name_padding_zero);

	bzero(raw, sizeof(raw));
	memcpy(raw + LINK_NAME, "linkname", 8);
	put32(raw + LINK_BLOCK, 0x12345678);
	raw[LINK_SLOT] = 5;
	put32(raw + LINK_LONG, 0x9abcdef0);
	raw[LINK_STATUS] = ST_LINK;
	qnx4_link_decode(raw, &link);
	CHECK(link.name_length == 8 && link.target_block == 0x12345678 &&
	    link.target_slot == 5 && link.long_block == 0x9abcdef0 &&
	    link.spare_zero && link.status == ST_LINK);
	raw[LINK_SPARE + 5] = 1;
	qnx4_link_decode(raw, &link);
	CHECK(!link.spare_zero);

	bzero(block, sizeof(block));
	put32(block + XBLK_NEXT, 9);
	put32(block + XBLK_PREVIOUS, 8);
	block[XBLK_USED] = 60;
	put32(block + XBLK_COVERED, 77);
	put32(block + XBLK_EXTENTS + 59 * 8, 1000);
	put32(block + XBLK_EXTENTS + 59 * 8 + 4, 77);
	memcpy(block + XBLK_SIGNATURE, "IamXblk", 8);
	put32(block + XBLK_FIRST, 5);
	put32(block + XBLK_FIRST + 4, 6);
	qnx4_xblock_decode(block, &xblock);
	CHECK(xblock.next == 9 && xblock.previous == 8 && xblock.used == 60 &&
	    xblock.covered == 77 && xblock.extent[59].first == 1000 &&
	    xblock.extent[59].blocks == 77 && xblock.signature_ok &&
	    xblock.spare_zero && xblock.first_copy.first == 5 &&
	    xblock.first_copy.blocks == 6);
	block[XBLK_SIGNATURE + 7] = 'x';
	block[XBLK_SPARE] = 1;
	qnx4_xblock_decode(block, &xblock);
	CHECK(!xblock.signature_ok && !xblock.spare_zero);
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

/* Read the current file completely and compare it with the generator's
 * bytes at seed offset.
 */
static void
check_content(fixture_t *f, const unsigned char *expected, uint64_t size)
{
	unsigned char *buffer = malloc(size + 1);
	uint64_t done;
	size_t count;

	CHECK(buffer != NULL);
	for (done = 0; done < size; done += count) {
		CHECK(fsimg_reader_read_at(f->reader, done, buffer + done,
		    (size_t)(size - done), &count, NULL) == FSIMG_OK);
		CHECK(count != 0);
	}
	CHECK(bcmp(buffer, expected, (size_t)size) == 0);
	free(buffer);
}

/* Open and list the sample at one geometry: every entry's kind, size,
 * metadata and content, and the volume information.
 */
static void
check_sample(uint32_t blocks)
{
	static const struct {
		const char	*path;
		fsimg_entry_kind_t kind;
		uint64_t	 size;
	} expected[] = {
		{ "/.boot", FSIMG_ENTRY_FILE, 0 },
		{ "/.altboot", FSIMG_ENTRY_FILE, 0 },
		{ "/one", FSIMG_ENTRY_FILE, 1 },
		{ "/chained", FSIMG_ENTRY_FILE, 61 * 512 - 100 },
		{ "/pppppppppppppppp", FSIMG_ENTRY_FILE, 700 },
		{ "/qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq",
		    FSIMG_ENTRY_FILE, 513 },
		{ "/d1", FSIMG_ENTRY_DIRECTORY, 0 },
		{ "/d1/d2-with-a-long-name", FSIMG_ENTRY_DIRECTORY, 0 },
		{ "/d1/d2-with-a-long-name/f", FSIMG_ENTRY_FILE, 3 },
		{ "/d1/hardlink", FSIMG_ENTRY_FILE, 1500 },
		{ "/linked", FSIMG_ENTRY_FILE, 1500 },
		{ "/symlink", FSIMG_ENTRY_SYMLINK, 11 },
		{ "/fifo", FSIMG_ENTRY_FIFO, 0 },
		{ "/attrs", FSIMG_ENTRY_FILE, 1 }
	};
	gen_t g;
	sample_t s;
	fixture_t f;
	fsimg_volume_info_t info;
	fsimg_verify_report_t report;
	const fsimg_entry_t *entry;
	fsimg_error_t error;
	unsigned char *bytes;
	bool seen[sizeof(expected) / sizeof(expected[0])];
	bool long_seen;
	uint64_t count;
	size_t i;
	fsimg_status_t status;

	sample_build(&g, &s, blocks);
	bytes = pattern(64 * 512, 9);
	bzero(seen, sizeof(seen));
	long_seen = false;
	count = 0;
	fixture_new(&f, g.image, g.length);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.format, "qnx4") == 0);
	CHECK(strcmp(info.profile, "long names") == 0);
	CHECK(info.valid == FSIMG_VOLUME_GEOMETRY);
	CHECK(info.capabilities == FSIMG_CAP_READ);
	CHECK(info.volume_bytes == (uint64_t)blocks * 512);
	CHECK(info.allocation_units == blocks);
	CHECK(info.sector_bytes == 512 && info.allocation_unit_bytes == 512);
	CHECK(info.clean && !info.io_error);
	CHECK(info.time_basis == FSIMG_TIME_UTC);
	while ((status = fsimg_reader_next(f.reader, &entry, &error)) ==
	    FSIMG_OK) {
		count++;
		if (strlen(entry->path) == 506) {
			CHECK(!long_seen);
			long_seen = true;
			CHECK(entry->kind == FSIMG_ENTRY_FILE && entry->size == 2);
			check_content(&f, bytes + 2, 2);
			continue;
		}
		for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
			if (strcmp(entry->path, expected[i].path) == 0)
				break;
		if (i == sizeof(expected) / sizeof(expected[0]))
			fprintf(stderr, "unexpected entry %s\n", entry->path);
		CHECK(i < sizeof(expected) / sizeof(expected[0]) && !seen[i]);
		seen[i] = true;
		if (entry->kind != expected[i].kind ||
		    entry->size != expected[i].size)
			fprintf(stderr, "%s: %s %" PRIu64 "\n", entry->path,
			    kind_name(entry->kind), entry->size);
		CHECK(entry->kind == expected[i].kind);
		CHECK(entry->size == expected[i].size);
		CHECK(entry->metadata.valid == (FSIMG_META_CREATED |
		    FSIMG_META_MODIFIED | FSIMG_META_ACCESSED |
		    FSIMG_META_CHANGED | FSIMG_META_OWNER | FSIMG_META_MODE));
		if (strcmp(entry->path, "/chained") == 0)
			check_content(&f, bytes, entry->size);
		if (strcmp(entry->path, "/pppppppppppppppp") == 0)
			check_content(&f, bytes, entry->size);
		if (entry->kind == FSIMG_ENTRY_FILE && entry->size == 513)
			check_content(&f, bytes + 1, entry->size);
		if (entry->size == 1500)
			check_content(&f, bytes + 3, entry->size);
		if (entry->kind == FSIMG_ENTRY_SYMLINK) {
			check_content(&f, (const unsigned char *)"d1/hardlink", 11);
			CHECK(entry->metadata.mode == 0777);
		}
		if (strcmp(entry->path, "/d1/d2-with-a-long-name/f") == 0)
			check_content(&f, (const unsigned char *)"sub", 3);
		if (strcmp(entry->path, "/fifo") == 0)
			CHECK(entry->metadata.mode == 0664);
		if (strcmp(entry->path, "/attrs") == 0) {
			CHECK(entry->metadata.uid == 65535);
			CHECK(entry->metadata.gid == 65534);
			CHECK(entry->metadata.mode == 04751);
			CHECK(entry->metadata.created.seconds == 0);
			CHECK(entry->metadata.modified.seconds == 4294967295);
			CHECK(entry->metadata.accessed.seconds == 2147483647);
			CHECK(entry->metadata.changed.seconds == 1);
			CHECK(entry->metadata.modified.basis == FSIMG_TIME_UTC);
			CHECK(entry->metadata.modified.precision_ns == 1000000000);
		}
	}
	if (status != FSIMG_DONE)
		fprintf(stderr, "sample of %" PRIu32 " blocks: %d (%s)\n", blocks,
		    status, error.message);
	CHECK(status == FSIMG_DONE);
	CHECK(long_seen);
	for (i = 0; i < sizeof(seen) / sizeof(seen[0]); i++)
		CHECK(seen[i]);
	CHECK(count == sizeof(expected) / sizeof(expected[0]) + 1);
	fixture_free(&f);

	/* Without a verifier, verification is unsupported, fails before any
	 * work and leaves the reader untouched.
	 */
	fixture_new(&f, g.image, g.length);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	bzero(&report, sizeof(report));
	CHECK(fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD, &report, NULL) ==
	    FSIMG_EUNSUPPORTED);
	CHECK(!report.complete);
	CHECK(walk_all(&f, &count, NULL) == FSIMG_OK);
	fixture_free(&f);
	free(bytes);
	gen_free(&g);
}

/* The sample at volume sizes with one, two and three bitmap blocks and
 * bitmaps ending inside a byte, at a byte and at a block, and with ten
 * bitmap blocks, whose bytes span more than one 4 KiB window.
 */
static void
test_sample(void)
{
	static const uint32_t sizes[] = { 1000, 4095, 4096, 4097, 5001, 8192,
	    8193, 40000 };

	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
		check_sample(sizes[i]);
}

/* A hard link's two names list its content once: the first name's walk
 * reads the extent block; the later name walks only when it is read.
 */
static void
test_hard_links(void)
{
	gen_t g;
	fixture_t f;
	const fsimg_entry_t *entry;
	unsigned char *bytes, buffer[1];
	uint32_t file;
	uint64_t entries;
	size_t count;

	gen_init(&g, GEN_DEFAULT_N);
	bytes = pattern(70 * 512, 4);
	g.force_table = true;
	file = gen_file_parts(&g, OBJ_ROOT, "a", bytes, 70 * 512, 62);
	g.force_table = false;
	gen_link(&g, OBJ_ROOT, "b", file);
	gen_link(&g, OBJ_ROOT, "a-much-longer-third-name-for-it", file);
	gen_finish(&g);
	CHECK(g.objects[file].chain_count == 2);

	fixture_new(&f, g.image, g.length);
	f.watch = (uint64_t)(g.objects[file].chain[1] - 1) * 512;
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	entries = 0;
	while (fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK)
		entries++;
	CHECK(entries == 5);
	CHECK(f.watch_hits == 1);
	fixture_free(&f);

	fixture_new(&f, g.image, g.length);
	f.watch = (uint64_t)(g.objects[file].chain[1] - 1) * 512;
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	while (fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK) {
		if (entry->kind != FSIMG_ENTRY_FILE || entry->size == 0)
			continue;
		CHECK(entry->size == 70 * 512);
		check_content(&f, bytes, entry->size);
		CHECK(fsimg_reader_read_at(f.reader, entry->size - 1, buffer, 1,
		    &count, NULL) == FSIMG_OK && buffer[0] == bytes[70 * 512 - 1]);
	}
	CHECK(f.watch_hits == 3);
	fixture_free(&f);
	free(bytes);
	gen_free(&g);
}

/* Geometry: the block count from the slice length, its bounds, and the
 * root directory's fixed position after the bitmap.
 */
static void
test_geometry(void)
{
	gen_t g;
	sample_t s;
	unsigned char *longer;
	uint64_t entries;

	/* The formatter's minimum: 43 blocks, none free. */
	gen_init(&g, 43);
	CHECK(g.next_block == 44);
	gen_finish(&g);
	CHECK(run_image(g.image, g.length, &entries, NULL) == FSIMG_OK);
	CHECK(entries == 2);
	expect_length(&g, 42 * 512, FSIMG_ECORRUPT, "42 blocks");
	gen_free(&g);

	sample(&g, &s);
	expect_length(&g, g.length - 1, FSIMG_ECORRUPT, "partial block");
	expect_length(&g, g.length - 512, FSIMG_ECORRUPT, "slice short of N");
	expect_length(&g, 1023, FSIMG_EFORMAT, "below 1024 bytes");
	expect_length(&g, 0, FSIMG_EFORMAT, "empty slice");
	expect_length(&g, 8 * 512, FSIMG_ECORRUPT, "8 blocks");
	/* A slice one block longer than the formatted volume. */
	longer = calloc(1, (size_t)g.length + 512);
	CHECK(longer != NULL);
	bcopy(g.image, longer, (size_t)g.length);
	CHECK(run_image(longer, g.length + 512, NULL, NULL) == FSIMG_ECORRUPT);
	free(longer);
	/* The root directory's extent ending past the slice. */
	put32(block_at(&g, 2) + INODE_FIRST, g.blocks - 2);
	expect(&g, FSIMG_ECORRUPT, "root past the slice");
	gen_free(&g);

	sample(&g, &s);
	put32(block_at(&g, 2) + INODE_FIRST, 5);
	expect(&g, FSIMG_ECORRUPT, "root not after the bitmap");
	gen_free(&g);
	sample(&g, &s);
	put32(block_at(&g, 2) + INODE_FIRST, 0);
	expect(&g, FSIMG_ECORRUPT, "root without an extent");
	gen_free(&g);
}

/* The bitmap: its record, its padding and its own blocks. */
static void
test_bitmap(void)
{
	gen_t g;
	sample_t s;
	unsigned char *record;

	sample_build(&g, &s, 4097);
	record = record_of(&g, OBJ_BITMAP);
	put32(record + INODE_FIRST, 4);
	expect(&g, FSIMG_ECORRUPT, "bitmap position");
	put32(record + INODE_FIRST, 3);
	put32(record + INODE_BLOCKS, 3);
	expect(&g, FSIMG_ECORRUPT, "bitmap extent");
	put32(record + INODE_BLOCKS, 2);
	put32(record + INODE_SIZE, 512);
	expect(&g, FSIMG_ECORRUPT, "bitmap size");
	put32(record + INODE_SIZE, 513);
	record[INODE_STATUS] = 0;
	expect(&g, FSIMG_ECORRUPT, "bitmap record free");
	record[INODE_STATUS] = ST_DIRECT;
	record[1] = 'B';
	expect(&g, FSIMG_ECORRUPT, "bitmap record renamed");
	record[1] = 'b';
	expect(&g, FSIMG_OK, "bitmap restored");

	/* N = 4097: bit 4097 opens the padding bits of byte 512. */
	gen_set_bit(&g, 4098, false);
	expect(&g, FSIMG_ECORRUPT, "clear padding bit");
	gen_set_bit(&g, 4098, true);
	block_at(&g, 3)[700] = 0xfe;
	expect(&g, FSIMG_ECORRUPT, "padding byte not 0xff");
	block_at(&g, 3)[700] = 0xff;
	gen_set_bit(&g, 4, false);
	expect(&g, FSIMG_ECORRUPT, "bitmap block free");
	gen_set_bit(&g, 4, true);
	gen_set_bit(&g, 1, false);
	expect(&g, FSIMG_ECORRUPT, "loader free");
	gen_set_bit(&g, 1, true);

	/* A block an object references must be allocated; an allocated
	 * block nothing references is the verifier's to report.
	 */
	gen_set_bit(&g, g.objects[s.one].extents[0].first, false);
	expect(&g, FSIMG_ECORRUPT, "referenced free block");
	gen_set_bit(&g, g.objects[s.one].extents[0].first, true);
	gen_set_bit(&g, 4000, true);
	expect(&g, FSIMG_OK, "allocated orphan");
	put32(record_of(&g, s.one) + INODE_FIRST,
	    g.objects[s.chained].extents[1].first);
	expect(&g, FSIMG_ECORRUPT, "a block with two owners");
	gen_free(&g);

	/* Three bitmap blocks, at the end of a byte. */
	sample_build(&g, &s, 8193);
	CHECK(g.bitmap_blocks == 3);
	expect(&g, FSIMG_OK, "three bitmap blocks");
	block_at(&g, 5)[511] = 0x7f;
	expect(&g, FSIMG_ECORRUPT, "last padding byte");
	gen_free(&g);
}

/* Records of the root block and their reserved ranges, the label and the
 * clean flag.
 */
static void
test_root_block(void)
{
	gen_t g;
	sample_t s;
	fixture_t f;
	fsimg_volume_info_t info;
	unsigned char *root;
	char *label;
	static const size_t lengths[] = { 0, 1, 47, 48 };

	for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
		gen_init(&g, 100);
		label = repeat('L', lengths[i]);
		strcpy(g.label, label);
		g.root_status = 0x01;
		gen_finish(&g);
		fixture_new(&f, g.image, g.length);
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) ==
		    FSIMG_OK);
		CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
		CHECK(!info.clean);
		if (lengths[i] == 0) {
			CHECK((info.valid & FSIMG_VOLUME_LABEL) == 0);
		} else {
			CHECK((info.valid & FSIMG_VOLUME_LABEL) != 0);
			CHECK(strcmp(info.label, label) == 0);
		}
		fixture_free(&f);
		free(label);
		gen_free(&g);
	}

	/* An invalid label never blocks reading: it is reported absent. */
	sample(&g, &s);
	root = block_at(&g, 2);
	memcpy(root + 0x1c0, "AB\001", 3);
	fixture_new(&f, g.image, g.length);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
	CHECK((info.valid & FSIMG_VOLUME_LABEL) == 0 && info.label[0] == '\0');
	fixture_free(&f);
	memcpy(root + 0x1c0, "A/B", 3);
	fixture_new(&f, g.image, g.length);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
	CHECK((info.valid & FSIMG_VOLUME_LABEL) == 0);
	fixture_free(&f);
	memcpy(root + 0x1c0, "AB\0C", 4);
	fixture_new(&f, g.image, g.length);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
	CHECK((info.valid & FSIMG_VOLUME_LABEL) == 0);
	fixture_free(&f);
	memcpy(root + 0x1c0, "\xff", 1);
	expect(&g, FSIMG_OK, "non-UTF-8 label lists");
	gen_free(&g);

	/* Statuses: 0x13 only on .boot and .altboot. */
	sample(&g, &s);
	root = block_at(&g, 2);
	root[64 * 2 + INODE_STATUS] = ST_TABLE_WRITTEN;
	root[64 * 3 + INODE_STATUS] = ST_TABLE_WRITTEN;
	expect(&g, FSIMG_OK, "written boot records");
	root[64 * 1 + INODE_STATUS] = ST_TABLE_WRITTEN;
	expect(&g, FSIMG_EUNSUPPORTED, ".inodes record 0x13");
	root[64 * 1 + INODE_STATUS] = ST_TABLE;
	root[64 * 4 + INODE_STATUS] = ST_TABLE_WRITTEN;
	expect(&g, FSIMG_EUNSUPPORTED, "lost+found record 0x13");
	root[64 * 4 + INODE_STATUS] = 0x01;
	expect(&g, FSIMG_EUNSUPPORTED, "lost+found record 0x01");
	root[64 * 4 + INODE_STATUS] = ST_TABLE;
	root[64 * 2 + 3] = 'x';
	expect(&g, FSIMG_EUNSUPPORTED, "named boot record");
	root[64 * 2 + 3] = 0;
	put16(root + 64 * 1 + INODE_MODE, 040444);
	expect(&g, FSIMG_ECORRUPT, ".inodes record a directory");
	put16(root + 64 * 1 + INODE_MODE, 0100444);
	put16(root + 64 * 4 + INODE_MODE, 0100755);
	expect(&g, FSIMG_ECORRUPT, "lost+found record a file");
	put16(root + 64 * 4 + INODE_MODE, 040755);
	/* A size a directory may have, so the kind check itself fails. */
	put16(root + 64 * 2 + INODE_MODE, 040600);
	put32(root + 64 * 2 + INODE_SIZE, 512);
	expect(&g, FSIMG_ECORRUPT, ".boot record a directory");
	put16(root + 64 * 2 + INODE_MODE, 0100600);
	put32(root + 64 * 2 + INODE_SIZE, 0);
	root[INODE_STATUS] = 0x03;
	expect(&g, FSIMG_EFORMAT, "root status 0x03");
	root[INODE_STATUS] = ST_ROOT_CLEAN;
	root[INODE_TYPE] = 1;
	expect(&g, FSIMG_EUNSUPPORTED, "root type byte");
	root[INODE_TYPE] = 0;
	root[INODE_RESERVED] = 1;
	expect(&g, FSIMG_EUNSUPPORTED, "root reserved bytes");
	root[INODE_RESERVED] = 0;
	put16(root + INODE_MODE, 0100775);
	expect(&g, FSIMG_EFORMAT, "root a file");
	put16(root + INODE_MODE, 040775);
	root[1] = 'x';
	expect(&g, FSIMG_EFORMAT, "root named /x");
	root[1] = 0;
	root[0x150] = 1;
	expect(&g, FSIMG_EUNSUPPORTED, "reserved 0x140");
	root[0x150] = 0;
	root[0x1f8] = 1;
	expect(&g, FSIMG_EUNSUPPORTED, "reserved 0x1f0");
	root[0x1f8] = 0;
	bzero(root + 64 * 4, 64);
	expect(&g, FSIMG_EUNSUPPORTED, "zero lost+found record");
	gen_free(&g);

	/* .boot with content is a regular file of that content, written
	 * through the filesystem.
	 */
	gen_init(&g, 100);
	gen_add_extent(&g, OBJ_BOOT, gen_alloc(&g, 2, GEN_POISON), 2);
	memcpy(block_at(&g, g.objects[OBJ_BOOT].extents[0].first), "boot", 4);
	g.objects[OBJ_BOOT].size = 4;
	g.objects[OBJ_BOOT].status = ST_TABLE_WRITTEN;
	gen_finish(&g);
	expect(&g, FSIMG_ECORRUPT, ".boot allocated beyond its size");
	put32(block_at(&g, 2) + 64 * 2 + INODE_SIZE, 513);
	fixture_new(&f, g.image, g.length);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	{
		const fsimg_entry_t *entry;
		unsigned char buffer[600];
		size_t count;

		CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
		CHECK(strcmp(entry->path, "/.boot") == 0 && entry->size == 513);
		CHECK(fsimg_reader_read_at(f.reader, 0, buffer, sizeof(buffer),
		    &count, NULL) == FSIMG_OK);
		CHECK(count == 513 && bcmp(buffer, "boot", 4) == 0 &&
		    buffer[512] == GEN_POISON);
	}
	fixture_free(&f);
	gen_free(&g);
}

/* The probe claims QNX4 alone and nothing else, against FAT, exFAT and
 * QNX6 volumes the writers make, and against QNX6 with a native-like boot
 * area. A forced decoder never falls back.
 */
typedef struct memory_sink {
	unsigned char	*bytes;
	uint64_t	 length;
} memory_sink_t;

static fsimg_status_t
sink_size(void *argument, uint64_t *length, fsimg_error_t *error FSIMG_UNUSED)
{
	*length = ((memory_sink_t *)argument)->length;
	return (FSIMG_OK);
}

static fsimg_status_t
sink_write(void *argument, uint64_t offset, const void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_sink_t *sink = argument;

	CHECK(offset <= sink->length && length <= sink->length - offset);
	bcopy(buffer, sink->bytes + offset, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
sink_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_sink_t *sink = argument;

	CHECK(offset <= sink->length && length <= sink->length - offset);
	bcopy(sink->bytes + offset, buffer, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
sink_flush(void *argument FSIMG_UNUSED, fsimg_error_t *error FSIMG_UNUSED)
{
	return (FSIMG_OK);
}

/* An empty volume of format, made by fsimg's own writer. */
static void
make_volume(const char *format, memory_sink_t *sink)
{
	fsimg_ctx_t *ctx;
	fsimg_writer_t *writer;
	fsimg_write_options_t options;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_io_ops_t ops;
	fsimg_io_t *io;

	CHECK(fsimg_ctx_new(NULL, &ctx, NULL) == FSIMG_OK);
	fsimg_write_options_init(&options);
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	CHECK(fsimg_writer_new(ctx, format, &options, &writer, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_plan(writer, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	sink->length = info.volume_bytes;
	sink->bytes = calloc(1, (size_t)sink->length);
	CHECK(sink->bytes != NULL);
	fsimg_io_ops_init(&ops);
	ops.get_size = sink_size;
	ops.read_at = sink_read;
	ops.write_at = sink_write;
	ops.flush = sink_flush;
	CHECK(fsimg_io_from_callbacks(ctx, &ops, sink, &io, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_emit(writer, io, NULL) == FSIMG_OK);
	fsimg_io_free(io);
	fsimg_writer_free(writer);
	fsimg_ctx_free(ctx);
}

/* The format the registry claims for an image, or NULL with *status. */
static const fsimg_format_t *
claim(const unsigned char *image, uint64_t length, fsimg_status_t *status)
{
	const fsimg_format_t *const registry[] = {
		&fsimg_fat_format, &fsimg_exfat_format, &fsimg_qnx6_format,
		&fsimg_qnx4_format
	};
	const fsimg_format_t *claimed;
	size_t prefix;

	prefix = 0;
	for (size_t i = 0; i < 4; i++)
		if (registry[i]->probe_bytes > prefix)
			prefix = registry[i]->probe_bytes;
	if (prefix > length)
		prefix = (size_t)length;
	*status = fsimg_format_claim(registry, 4, NULL, image, prefix, length,
	    &claimed, NULL);
	return (claimed);
}

static fsimg_status_t
open_forced(const unsigned char *image, uint64_t length, const char *format)
{
	fixture_t f;
	fsimg_read_options_t options;
	fsimg_status_t status;

	fixture_new(&f, image, length);
	fsimg_read_options_init(&options);
	options.format = format;
	status = fsimg_reader_open(f.ctx, f.io, &options, &f.reader, NULL);
	fixture_free(&f);
	return (status);
}

static void
test_probe(void)
{
	static const char *const others[] = { "fat12", "exfat", "qnx6" };
	const fsimg_format_t *const formats[] = { &fsimg_fat_format,
	    &fsimg_exfat_format, &fsimg_qnx6_format };
	memory_sink_t sink;
	gen_t g;
	sample_t s;
	fsimg_status_t status;
	unsigned char *zero;

	for (size_t i = 0; i < 3; i++) {
		make_volume(others[i], &sink);
		CHECK(claim(sink.bytes, sink.length, &status) == formats[i]);
		CHECK(status == FSIMG_OK);
		CHECK(open_forced(sink.bytes, sink.length, "qnx4") ==
		    FSIMG_EFORMAT);
		if (i == 2) {
			/* A native QNX6 boot area holds a loader, not zeros. */
			for (size_t b = 0; b < 8192; b++)
				sink.bytes[b] = (unsigned char)(b * 131 + b / 7);
			sink.bytes[510] = 0x55;
			sink.bytes[511] = 0xaa;
			CHECK(claim(sink.bytes, sink.length, &status) ==
			    &fsimg_qnx6_format && status == FSIMG_OK);
			CHECK(open_forced(sink.bytes, sink.length, "qnx4") ==
			    FSIMG_EFORMAT);
			/* "/" and fifteen NULs at 512 alone are no root record. */
			bzero(sink.bytes + 512, 64);
			sink.bytes[512] = '/';
			CHECK(claim(sink.bytes, sink.length, &status) ==
			    &fsimg_qnx6_format && status == FSIMG_OK);
		}
		free(sink.bytes);
	}

	sample(&g, &s);
	CHECK(claim(g.image, g.length, &status) == &fsimg_qnx4_format &&
	    status == FSIMG_OK);
	CHECK(open_forced(g.image, g.length, "qnx4") == FSIMG_OK);
	for (size_t i = 0; i < 3; i++)
		CHECK(open_forced(g.image, g.length, others[i]) == FSIMG_EFORMAT);
	/* The forced decoder still validates. */
	block_at(&g, 2)[64 + INODE_TYPE] = 1;
	CHECK(open_forced(g.image, g.length, "qnx4") == FSIMG_EUNSUPPORTED);
	gen_free(&g);

	zero = calloc(1, 65536);
	CHECK(zero != NULL);
	CHECK(run_image(zero, 65536, NULL, NULL) == FSIMG_EFORMAT);
	CHECK(open_forced(zero, 65536, "qnx4") == FSIMG_EFORMAT);
	free(zero);
}

/* Names of every length boundary are covered by the sample; these are the
 * faults of each name encoding.
 */
static void
test_names(void)
{
	gen_t g;
	sample_t s;
	unsigned char *slot, *record;
	uint64_t entries, index;
	uint32_t file, other;
	char *name;

	/* Byte rules: corruption for what QNX forbids, unsupported for what
	 * QNX allows but the profile does not.
	 */
	static const struct {
		const char	*name;
		size_t		 length;
		fsimg_status_t	 status;
	} direct[] = {
		{ "ab\0cd", 5, FSIMG_ECORRUPT },
		{ "a/b", 3, FSIMG_ECORRUPT },
		{ ".", 1, FSIMG_ECORRUPT },
		{ "..", 2, FSIMG_ECORRUPT },
		{ "", 0, FSIMG_ECORRUPT },
		{ "a\001b", 3, FSIMG_EUNSUPPORTED },
		{ "a\177b", 3, FSIMG_EUNSUPPORTED },
		{ "a\377b", 3, FSIMG_EUNSUPPORTED },
		{ "\xc3\xa9", 2, FSIMG_OK },
		{ "...", 3, FSIMG_OK }
	};

	for (size_t i = 0; i < sizeof(direct) / sizeof(direct[0]); i++) {
		sample(&g, &s);
		record = record_of(&g, s.one);
		bzero(record, 16);
		memcpy(record, direct[i].name, direct[i].length);
		expect(&g, direct[i].status, "direct name");
		gen_free(&g);
	}

	/* The same faults in a link's own field. */
	sample(&g, &s);
	index = link_slot(&g, OBJ_ROOT, g.objects[s.name48].record);
	slot = slot_of(&g, OBJ_ROOT, index);
	slot[10] = '/';
	expect(&g, FSIMG_ECORRUPT, "slash in a link name");
	slot[10] = '\0';
	expect(&g, FSIMG_ECORRUPT, "NUL inside a link name");
	bzero(slot, 48);
	expect(&g, FSIMG_ECORRUPT, "empty link name");
	memcpy(slot, "pppppppppppppppp", 16);
	expect(&g, FSIMG_ECORRUPT, "duplicate 16-byte name in a link record");
	memcpy(slot, "rrrrrrrrrrrrrrrr", 16);
	expect(&g, FSIMG_OK, "a 16-byte name in a link record");
	bzero(slot, 48);
	slot[0] = '.';
	expect(&g, FSIMG_ECORRUPT, "a link named .");
	slot[1] = '.';
	expect(&g, FSIMG_ECORRUPT, "a link named ..");
	bzero(slot, 48);
	memcpy(slot, "one", 3);
	expect(&g, FSIMG_ECORRUPT, "duplicate name");
	memcpy(slot, "One", 3);
	expect(&g, FSIMG_OK, "names differing by case");
	gen_free(&g);

	/* Long names: the prefix, its zero byte, the pointer, the
	 * back-reference, the state and the record's own bytes.
	 */
	sample(&g, &s);
	index = link_slot(&g, OBJ_ROOT, g.objects[s.long505].record);
	slot = slot_of(&g, OBJ_ROOT, index);
	record = block_at(&g, get32(slot + LINK_LONG));
	CHECK(record[LONG_NAME + 504] == 'r' && record[LONG_TAIL] == 0);
	slot[3] = 's';
	expect(&g, FSIMG_ECORRUPT, "long-name prefix");
	slot[3] = 'r';
	slot[39] = 'X';
	expect(&g, FSIMG_ECORRUPT, "long-name prefix digits");
	gen_free(&g);
	sample(&g, &s);
	index = link_slot(&g, OBJ_ROOT, g.objects[s.long505].record);
	slot = slot_of(&g, OBJ_ROOT, index);
	record = block_at(&g, get32(slot + LINK_LONG));
	slot[47] = 'z';
	expect(&g, FSIMG_ECORRUPT, "long-name prefix zero byte");
	slot[47] = 0;
	expect(&g, FSIMG_OK, "long name restored");
	record[LONG_STATE] = 0;
	expect(&g, FSIMG_ECORRUPT, "long-name state");
	record[LONG_STATE] = 1;
	record[LONG_SLOT] ^= 1;
	expect(&g, FSIMG_ECORRUPT, "long-name back-reference slot");
	record[LONG_SLOT] ^= 1;
	put32(record + LONG_BLOCK, get32(record + LONG_BLOCK) + 1);
	expect(&g, FSIMG_ECORRUPT, "long-name back-reference block");
	put32(record + LONG_BLOCK, get32(record + LONG_BLOCK) - 1);
	record[LONG_TAIL] = 1;
	expect(&g, FSIMG_ECORRUPT, "long-name tail byte");
	record[LONG_TAIL] = 0;
	record[LONG_NAME + 48] = 0;
	expect(&g, FSIMG_ECORRUPT, "long-name record of 48 bytes");
	record[LONG_NAME + 49] = 0;
	record[LONG_NAME + 48] = 'r';
	expect(&g, FSIMG_ECORRUPT, "NUL inside a long name");
	gen_free(&g);
	sample(&g, &s);
	index = link_slot(&g, OBJ_ROOT, g.objects[s.long505].record);
	slot = slot_of(&g, OBJ_ROOT, index);
	put32(slot + LINK_LONG, g.objects[s.one].extents[0].first);
	expect(&g, FSIMG_ECORRUPT, "long-name pointer outside .longfilenames");
	gen_free(&g);

	/* Names of 49 and 504 bytes, and a record that agrees with a second
	 * link: two names sharing 48 bytes never collide.
	 */
	gen_init(&g, GEN_DEFAULT_N);
	name = repeat('n', 504);
	gen_file(&g, OBJ_ROOT, name, (const unsigned char *)"a", 1);
	name[49] = '\0';
	file = gen_file(&g, OBJ_ROOT, name, (const unsigned char *)"b", 1);
	name[48] = 'm';
	other = gen_file(&g, OBJ_ROOT, name, (const unsigned char *)"c", 1);
	gen_finish(&g);
	CHECK(run_image(g.image, g.length, &entries, NULL) == FSIMG_OK);
	CHECK(entries == 5);
	/* Swap the two 49-byte links' pointers: each record then names the
	 * other link.
	 */
	{
		unsigned char *a = slot_of(&g, OBJ_ROOT, link_slot(&g, OBJ_ROOT,
		    g.objects[file].record));
		unsigned char *b = slot_of(&g, OBJ_ROOT, link_slot(&g, OBJ_ROOT,
		    g.objects[other].record));
		unsigned char saved[4];

		memcpy(saved, a + LINK_LONG, 4);
		memcpy(a + LINK_LONG, b + LINK_LONG, 4);
		memcpy(b + LINK_LONG, saved, 4);
		expect(&g, FSIMG_ECORRUPT, "swapped long-name pointers");
	}
	free(name);
	gen_free(&g);

	/* Without .longfilenames, a long-name pointer names nothing. */
	gen_init(&g, GEN_DEFAULT_N);
	g.names = false;
	g.force_table = true;
	file = gen_file(&g, OBJ_ROOT, "x", (const unsigned char *)"x", 1);
	gen_finish(&g);
	CHECK(run_image(g.image, g.length, &entries, NULL) == FSIMG_OK);
	slot = slot_of(&g, OBJ_ROOT, link_slot(&g, OBJ_ROOT,
	    g.objects[file].record));
	put32(slot + LINK_LONG, 100);
	expect(&g, FSIMG_ECORRUPT, "long-name pointer without .longfilenames");
	gen_free(&g);
}

/* Names of every length boundary, direct and in links, read back
 * exactly; the 16-, 48- and 505-byte ones fill their fields unterminated.
 */
static void
test_name_lengths(void)
{
	static const size_t lengths[] = { 1, 15, 16, 17, 47, 48, 49, 504, 505 };
	gen_t g;
	fixture_t f;
	const fsimg_entry_t *entry;
	char *names[sizeof(lengths) / sizeof(lengths[0])];
	bool seen[sizeof(lengths) / sizeof(lengths[0])];
	size_t i;

	gen_init(&g, GEN_DEFAULT_N);
	for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
		names[i] = repeat((char)('a' + i), lengths[i]);
		gen_file(&g, OBJ_ROOT, names[i], (const unsigned char *)names[i],
		    (uint32_t)lengths[i]);
		seen[i] = false;
	}
	gen_finish(&g);
	fixture_new(&f, g.image, g.length);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	while (fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK) {
		if (entry->path[1] == '.')
			continue;
		i = (size_t)(entry->path[1] - 'a');
		CHECK(i < sizeof(lengths) / sizeof(lengths[0]) && !seen[i]);
		CHECK(strcmp(entry->path + 1, names[i]) == 0);
		CHECK(entry->size == lengths[i]);
		check_content(&f, (const unsigned char *)names[i], lengths[i]);
		seen[i] = true;
	}
	fixture_free(&f);
	for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
		CHECK(seen[i]);
		free(names[i]);
	}
	gen_free(&g);
}

/* Every status value of each record kind falls into its class. */
static void
test_statuses(void)
{
	gen_t g;
	sample_t s;
	uint64_t entries, index;
	unsigned char *record, saved;
	fsimg_status_t status, expected;
	fsimg_error_t error;

	sample(&g, &s);
	for (unsigned v = 0; v < 256; v++) {
		/* A direct regular file. */
		record = record_of(&g, s.one);
		saved = record[INODE_STATUS];
		record[INODE_STATUS] = (unsigned char)v;
		if ((v & 0x09) == 0)
			expected = FSIMG_OK;
		else if ((v & 0x09) == 0x09)
			expected = FSIMG_ECORRUPT;
		else if ((v & 0x08) != 0)
			expected = v == 0x08 ? FSIMG_ECORRUPT : FSIMG_EUNSUPPORTED;
		else
			expected = v == 0x01 || v == 0x03 ? FSIMG_OK :
			    FSIMG_EUNSUPPORTED;
		status = run_image(g.image, g.length, &entries, &error);
		if (status != expected)
			fprintf(stderr, "direct status %#x: %d (%s)\n", v, status,
			    error.message);
		CHECK(status == expected);
		if ((v & 0x09) == 0)
			CHECK(entries == SAMPLE_ENTRIES - 1);
		record[INODE_STATUS] = saved;

		/* A direct directory: no modified bit. */
		record = record_of(&g, s.d1);
		saved = record[INODE_STATUS];
		record[INODE_STATUS] = (unsigned char)v;
		if ((v & 0x09) == 0)
			expected = FSIMG_OK;
		else if ((v & 0x09) == 0x09)
			expected = FSIMG_ECORRUPT;
		else if ((v & 0x08) != 0)
			expected = v == 0x08 ? FSIMG_ECORRUPT : FSIMG_EUNSUPPORTED;
		else
			expected = v == 0x01 ? FSIMG_OK : FSIMG_EUNSUPPORTED;
		status = run_image(g.image, g.length, NULL, &error);
		if (status != expected)
			fprintf(stderr, "directory status %#x: %d (%s)\n", v,
			    status, error.message);
		CHECK(status == expected);
		record[INODE_STATUS] = saved;

		/* An .inodes regular file. */
		record = record_of(&g, s.linked);
		saved = record[INODE_STATUS];
		record[INODE_STATUS] = (unsigned char)v;
		if ((v & 0x01) == 0 || (v & 0x08) != 0)
			expected = FSIMG_ECORRUPT;
		else
			expected = v == 0x11 || v == 0x13 ? FSIMG_OK :
			    FSIMG_EUNSUPPORTED;
		status = run_image(g.image, g.length, NULL, &error);
		if (status != expected)
			fprintf(stderr, "table status %#x: %d (%s)\n", v, status,
			    error.message);
		CHECK(status == expected);
		record[INODE_STATUS] = saved;

		/* A link record. */
		index = link_slot(&g, OBJ_ROOT, g.objects[s.linked].record);
		record = slot_of(&g, OBJ_ROOT, index);
		saved = record[LINK_STATUS];
		record[LINK_STATUS] = (unsigned char)v;
		if ((v & 0x09) == 0)
			expected = FSIMG_OK;
		else if ((v & 0x09) == 0x09)
			expected = FSIMG_ECORRUPT;
		else if ((v & 0x08) != 0)
			expected = v == 0x08 ? FSIMG_OK : FSIMG_EUNSUPPORTED;
		else
			expected = FSIMG_OK;
		/* Used without the link bit, the slot reads as a direct
		 * record whose bytes are a link's; no single class applies.
		 */
		if ((v & 0x09) != 0x01) {
			status = run_image(g.image, g.length, NULL, &error);
			if (status != expected)
				fprintf(stderr, "link status %#x: %d (%s)\n", v,
				    status, error.message);
			CHECK(status == expected);
		}
		record[LINK_STATUS] = saved;
	}
	gen_free(&g);

	/* The modified bit is accepted on regular files only. */
	sample(&g, &s);
	record_of(&g, s.symlink)[INODE_STATUS] = 0x03;
	expect(&g, FSIMG_EUNSUPPORTED, "written symbolic link");
	record_of(&g, s.symlink)[INODE_STATUS] = 0x01;
	record_of(&g, s.fifo)[INODE_STATUS] = 0x03;
	expect(&g, FSIMG_EUNSUPPORTED, "written FIFO");
	record_of(&g, s.fifo)[INODE_STATUS] = 0x01;
	record_of(&g, s.d2)[INODE_STATUS] = ST_TABLE_WRITTEN;
	expect(&g, FSIMG_EUNSUPPORTED, "written directory in .inodes");
	record_of(&g, s.d2)[INODE_STATUS] = ST_TABLE;
	expect(&g, FSIMG_OK, "statuses restored");
	gen_free(&g);
}

/* Reserved, type and spare bytes, the .inodes name field, freed slots and
 * every kind of link target.
 */
static void
test_records(void)
{
	gen_t g;
	sample_t s;
	unsigned char *record, *link;
	uint64_t index, entries;

	sample(&g, &s);
	record = record_of(&g, s.one);
	record[INODE_TYPE] = 2;
	expect(&g, FSIMG_EUNSUPPORTED, "type byte");
	record[INODE_TYPE] = 0;
	record[INODE_RESERVED + 1] = 2;
	expect(&g, FSIMG_EUNSUPPORTED, "reserved bytes");
	record[INODE_RESERVED + 1] = 0;
	record = record_of(&g, s.linked);
	record[5] = 'n';
	expect(&g, FSIMG_EUNSUPPORTED, ".inodes record with a name");
	record[5] = 0;
	index = link_slot(&g, OBJ_ROOT, g.objects[s.linked].record);
	link = slot_of(&g, OBJ_ROOT, index);
	link[LINK_SPARE + 2] = 1;
	expect(&g, FSIMG_EUNSUPPORTED, "link spare bytes");
	link[LINK_SPARE + 2] = 0;

	/* Link targets. */
	link[LINK_SLOT] = 8;
	expect(&g, FSIMG_ECORRUPT, "link slot 8");
	link[LINK_SLOT] = (unsigned char)(g.objects[s.linked].record % 8);
	put32(link + LINK_BLOCK, 1);
	expect(&g, FSIMG_ECORRUPT, "link into the loader");
	put32(link + LINK_BLOCK, 2);
	link[LINK_SLOT] = 1;
	expect(&g, FSIMG_ECORRUPT, "link into the root block");
	put32(link + LINK_BLOCK, 3);
	expect(&g, FSIMG_ECORRUPT, "link into the bitmap");
	put32(link + LINK_BLOCK, g.objects[s.one].extents[0].first);
	expect(&g, FSIMG_ECORRUPT, "link into a payload block");
	put32(link + LINK_BLOCK, g.objects[s.d1].extents[0].first);
	link[LINK_SLOT] = 2;
	expect(&g, FSIMG_ECORRUPT, "link into a directory");
	put32(link + LINK_BLOCK, g.blocks + 5);
	expect(&g, FSIMG_ECORRUPT, "link past the volume");
	put32(link + LINK_BLOCK, g.objects[OBJ_INODES].extents[0].first);
	link[LINK_SLOT] = 0;
	expect(&g, FSIMG_ECORRUPT, "link to the .inodes signature");
	link[LINK_SLOT] = 7;
	expect(&g, FSIMG_ECORRUPT, "link to a free .inodes slot");
	gen_free(&g);

	/* A freed .inodes slot keeps stale bytes and is ignored. */
	sample(&g, &s);
	record = gen_record_at(&g, gen_logical_position(&g, OBJ_INODES, 100));
	memset(record, 0x5a, 64);
	record[INODE_STATUS] = 0;
	CHECK(run_image(g.image, g.length, &entries, NULL) == FSIMG_OK);
	CHECK(entries == SAMPLE_ENTRIES);
	/* A deleted directory slot keeps its bytes too. */
	link = slot_of(&g, OBJ_ROOT, link_slot(&g, OBJ_ROOT,
	    g.objects[s.linked].record));
	link[LINK_STATUS] = 0;
	CHECK(run_image(g.image, g.length, &entries, NULL) == FSIMG_OK);
	CHECK(entries == SAMPLE_ENTRIES - 1);
	gen_free(&g);

	/* The root's links into the root block are fixed by position. */
	for (unsigned i = 3; i <= 5; i++) {
		sample(&g, &s);
		link = slot_of(&g, OBJ_ROOT, i);
		link[LINK_SLOT] = (unsigned char)(i == 5 ? 2 : i);
		expect(&g, FSIMG_ECORRUPT, "root link target");
		link[LINK_SLOT] = (unsigned char)(i - 2);
		link[LINK_STATUS] = 0;
		expect(&g, FSIMG_ECORRUPT, "root link missing");
		gen_free(&g);
	}
	sample(&g, &s);
	slot_of(&g, OBJ_ROOT, 2)[INODE_STATUS] = 0;
	expect(&g, FSIMG_ECORRUPT, "no .bitmap slot");
	gen_free(&g);

	/* Format-owned names cannot appear twice in the root. */
	sample(&g, &s);
	record = record_of(&g, s.one);
	memcpy(record, ".inodes\0", 8);
	expect(&g, FSIMG_ECORRUPT, "second .inodes");
	memcpy(record, ".bitmap\0", 8);
	expect(&g, FSIMG_ECORRUPT, "second .bitmap");
	memcpy(record, ".longfilenames\0", 15);
	expect(&g, FSIMG_ECORRUPT, "second .longfilenames");
	memcpy(record, "lost+found\0\0\0\0\0", 15);
	expect(&g, FSIMG_OK, "an ordinary lost+found entry");
	gen_free(&g);
}

/* The bytes of extent block i of object id. */
static unsigned char *
chain_of(gen_t *g, uint32_t id, size_t i)
{
	CHECK(i < g->objects[id].chain_count);
	return (block_at(g, g->objects[id].chain[i]));
}

/* Files of every extent-count boundary list and read back; each extent
 * block fault of a three-block chain fails as corruption.
 */
static void
test_extents(void)
{
	static const uint32_t counts[] = { 0, 1, 2, 60, 61, 62, 121, 122 };
	gen_t g;
	fixture_t f;
	const fsimg_entry_t *entry;
	unsigned char *bytes, *x, *record;
	uint32_t file, size, first;

	bytes = pattern(130 * 512, 5);
	for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
		gen_init(&g, GEN_DEFAULT_N);
		size = counts[i] == 0 ? 0 : counts[i] * 512 - 7;
		file = gen_file_parts(&g, OBJ_ROOT, "f", bytes, size, counts[i]);
		gen_finish(&g);
		CHECK(g.objects[file].chain_count ==
		    (counts[i] < 2 ? 0 : (counts[i] - 1 + 59) / 60));
		fixture_new(&f, g.image, g.length);
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) ==
		    FSIMG_OK);
		while (fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK) {
			if (strcmp(entry->path, "/f") != 0)
				continue;
			CHECK(entry->size == size);
			check_content(&f, bytes, size);
		}
		fixture_free(&f);
		gen_free(&g);
	}

	gen_init(&g, GEN_DEFAULT_N);
	file = gen_file_parts(&g, OBJ_ROOT, "f", bytes, 122 * 512, 122);
	gen_finish(&g);
	CHECK(g.objects[file].chain_count == 3);
	expect(&g, FSIMG_OK, "122 extents");
	x = chain_of(&g, file, 1);
	put32(x + XBLK_NEXT, g.objects[file].chain[0]);
	expect(&g, FSIMG_ECORRUPT, "chain loop");
	put32(x + XBLK_NEXT, g.objects[file].chain[1]);
	expect(&g, FSIMG_ECORRUPT, "chain repeating a block");
	put32(x + XBLK_NEXT, g.objects[file].chain[2]);
	put32(x + XBLK_PREVIOUS, 0);
	expect(&g, FSIMG_ECORRUPT, "wrong previous pointer");
	put32(x + XBLK_PREVIOUS, g.objects[file].chain[0]);
	x[XBLK_SPARE + 1] = 1;
	expect(&g, FSIMG_ECORRUPT, "nonzero spare bytes");
	x[XBLK_SPARE + 1] = 0;
	put32(x + XBLK_COVERED, get32(x + XBLK_COVERED) + 1);
	expect(&g, FSIMG_ECORRUPT, "covered count");
	put32(x + XBLK_COVERED, get32(x + XBLK_COVERED) - 1);
	x[XBLK_SIGNATURE + 2] = 'M';
	expect(&g, FSIMG_ECORRUPT, "signature");
	x[XBLK_SIGNATURE + 2] = 'm';
	put32(x + XBLK_FIRST, 1);
	expect(&g, FSIMG_ECORRUPT, "first-extent copy in a later block");
	put32(x + XBLK_FIRST, 0);
	x = chain_of(&g, file, 0);
	put32(x + XBLK_FIRST + 4, 2);
	expect(&g, FSIMG_ECORRUPT, "wrong first-extent copy");
	put32(x + XBLK_FIRST + 4, 1);
	x[XBLK_USED] = 61;
	expect(&g, FSIMG_ECORRUPT, "61 descriptors used");
	x[XBLK_USED] = 60;
	x = chain_of(&g, file, 2);
	x[XBLK_USED] = 0;
	expect(&g, FSIMG_ECORRUPT, "no descriptor used");
	x[XBLK_USED] = 1;
	put32(x + XBLK_EXTENTS + 8 * 5 + 4, 1);
	expect(&g, FSIMG_ECORRUPT, "stray unused descriptor");
	put32(x + XBLK_EXTENTS + 8 * 5 + 4, 0);
	record = record_of(&g, file);
	put16(record + INODE_EXTENTS, 123);
	expect(&g, FSIMG_ECORRUPT, "count beyond the chain");
	put16(record + INODE_EXTENTS, 121);
	expect(&g, FSIMG_ECORRUPT, "chain beyond the count");
	put16(record + INODE_EXTENTS, 122);
	put32(record + INODE_CHAIN, 0);
	expect(&g, FSIMG_ECORRUPT, "count without a chain");
	put32(record + INODE_CHAIN, g.objects[file].chain[0]);
	expect(&g, FSIMG_OK, "chain restored");
	gen_free(&g);

	/* A chain block holding 59 descriptors before another block: QNX
	 * fills each block before starting the next.
	 */
	gen_init(&g, GEN_DEFAULT_N);
	file = gen_file_parts(&g, OBJ_ROOT, "f", bytes, 62 * 512, 62);
	gen_finish(&g);
	{
		unsigned char *x0 = chain_of(&g, file, 0);
		unsigned char *x1 = chain_of(&g, file, 1);
		uint32_t moved_first = get32(x0 + XBLK_EXTENTS + 59 * 8);
		uint32_t moved_blocks = get32(x0 + XBLK_EXTENTS + 59 * 8 + 4);

		bcopy(x1 + XBLK_EXTENTS, x1 + XBLK_EXTENTS + 8, 8);
		put32(x1 + XBLK_EXTENTS, moved_first);
		put32(x1 + XBLK_EXTENTS + 4, moved_blocks);
		x1[XBLK_USED] = 2;
		put32(x1 + XBLK_COVERED, get32(x1 + XBLK_COVERED) + moved_blocks);
		bzero(x0 + XBLK_EXTENTS + 59 * 8, 8);
		x0[XBLK_USED] = 59;
		put32(x0 + XBLK_COVERED, get32(x0 + XBLK_COVERED) - moved_blocks);
		expect(&g, FSIMG_EUNSUPPORTED, "partly used block before another");
	}
	gen_free(&g);

	/* Extent bounds, and allocation against size for each kind. */
	gen_init(&g, GEN_DEFAULT_N);
	file = gen_file(&g, OBJ_ROOT, "f", bytes, 1000);
	gen_finish(&g);
	record = record_of(&g, file);
	first = get32(record + INODE_FIRST);
	put32(record + INODE_FIRST, g.blocks);
	expect(&g, FSIMG_ECORRUPT, "extent crossing N");
	put32(record + INODE_FIRST, 0);
	expect(&g, FSIMG_ECORRUPT, "zero start with a length");
	put32(record + INODE_FIRST, first);
	put32(record + INODE_SIZE, 512);
	expect(&g, FSIMG_ECORRUPT, "file size short of its allocation");
	put32(record + INODE_SIZE, 1025);
	expect(&g, FSIMG_ECORRUPT, "file size beyond its allocation");
	put32(record + INODE_SIZE, 1024);
	expect(&g, FSIMG_OK, "file size equal to its allocation");
	put32(record + INODE_SIZE, 0);
	put32(record + INODE_FIRST, 0);
	put32(record + INODE_BLOCKS, 0);
	expect(&g, FSIMG_ECORRUPT, "empty file with an extent count");
	put16(record + INODE_EXTENTS, 0);
	expect(&g, FSIMG_OK, "empty file");
	gen_free(&g);

	{
		sample_t s;

		sample(&g, &s);
		record = record_of(&g, s.symlink);
		put32(record + INODE_SIZE, 513);
		expect(&g, FSIMG_ECORRUPT, "link size beyond its allocation");
		put32(record + INODE_SIZE, 0);
		expect(&g, FSIMG_ECORRUPT, "empty symbolic link");
		put32(record + INODE_SIZE, 11);
		record = record_of(&g, s.d1);
		put32(record + INODE_SIZE, 4608);
		expect(&g, FSIMG_ECORRUPT, "directory size beyond its allocation");
		put32(record + INODE_SIZE, 3584);
		expect(&g, FSIMG_ECORRUPT, "directory size short of its allocation");
		put32(record + INODE_SIZE, 4000);
		expect(&g, FSIMG_ECORRUPT, "directory size not whole blocks");
		put32(record + INODE_SIZE, 0);
		expect(&g, FSIMG_ECORRUPT, "empty directory");
		put32(record + INODE_SIZE, 4096);
		record = record_of(&g, s.fifo);
		put32(record + INODE_SIZE, 0);
		expect(&g, FSIMG_OK, "FIFO size below its allocation");
		put32(record + INODE_SIZE, 100000);
		expect(&g, FSIMG_OK, "FIFO size beyond its allocation");
		gen_free(&g);
	}

	gen_init(&g, GEN_DEFAULT_N);
	gen_fifo(&g, OBJ_ROOT, "empty", 0);
	gen_fifo(&g, OBJ_ROOT, "owning", 3);
	gen_finish(&g);
	fixture_new(&f, g.image, g.length);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	while (fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK)
		if (entry->kind == FSIMG_ENTRY_FIFO)
			CHECK(entry->size == 0);
	fixture_free(&f);
	gen_free(&g);
	free(bytes);
}

/* Dot entries, directory shapes, cycles, second parents, unsupported
 * kinds, and a directory wide enough to spill to scratch.
 */
static void
test_directories(void)
{
	gen_t g;
	sample_t s;
	fixture_t f;
	fsimg_stats_t stats;
	unsigned char *dot, *dotdot;
	uint64_t entries;
	uint32_t dir, other;
	char name[32];

	sample(&g, &s);
	dot = slot_of(&g, s.d1, 0);
	dotdot = slot_of(&g, s.d1, 1);
	dot[LINK_STATUS] = 0;
	expect(&g, FSIMG_ECORRUPT, "free dot slot");
	dot[LINK_STATUS] = ST_LINK;
	dot[3] = 4;
	expect(&g, FSIMG_ECORRUPT, "dot signature");
	dot[3] = 3;
	dot[12] = 1;
	expect(&g, FSIMG_ECORRUPT, "dot name bytes");
	dot[12] = 0;
	dot[LINK_SLOT] ^= 1;
	expect(&g, FSIMG_ECORRUPT, "dot target");
	dot[LINK_SLOT] ^= 1;
	put32(dot + LINK_LONG, 5);
	expect(&g, FSIMG_ECORRUPT, "dot long-name pointer");
	put32(dot + LINK_LONG, 0);
	dotdot[1] = 'x';
	expect(&g, FSIMG_ECORRUPT, "dot-dot name");
	dotdot[1] = '.';
	dotdot[LINK_SLOT] = 1;
	expect(&g, FSIMG_ECORRUPT, "dot-dot target");
	dotdot[LINK_SLOT] = 0;
	dotdot[LINK_SPARE] = 1;
	expect(&g, FSIMG_ECORRUPT, "dot-dot spare byte");
	dotdot[LINK_SPARE] = 0;
	/* The value after "." is not interpreted. */
	put32(dot + 8, 12345);
	expect(&g, FSIMG_OK, "dot value");
	/* The nested directory's ".." names its parent's record. */
	dotdot = slot_of(&g, s.d2, 1);
	put32(dotdot + LINK_BLOCK, 2);
	expect(&g, FSIMG_ECORRUPT, "nested dot-dot naming the root");
	gen_free(&g);

	/* Two-block directories grown by two-block extents into a chain, and
	 * a sixteen-block one.
	 */
	gen_init(&g, GEN_DEFAULT_N);
	g.dir_blocks = 2;
	dir = gen_dir(&g, OBJ_ROOT, "two");
	for (unsigned i = 0; i < 200; i++) {
		snprintf(name, sizeof(name), "e%u", i);
		gen_file(&g, dir, name, (const unsigned char *)name, 1);
	}
	g.dir_blocks = 16;
	other = gen_dir(&g, OBJ_ROOT, "sixteen");
	gen_file(&g, other, "x", (const unsigned char *)"x", 1);
	gen_finish(&g);
	CHECK(g.objects[dir].extent_count == 13 &&
	    g.objects[dir].chain_count == 1);
	CHECK(run_image(g.image, g.length, &entries, NULL) == FSIMG_OK);
	CHECK(entries == 2 + 2 + 200 + 1);
	gen_free(&g);

	/* A directory in .inodes reached a second time: from itself, from a
	 * second parent, and from below itself.
	 */
	gen_init(&g, GEN_DEFAULT_N);
	g.force_table = true;
	dir = gen_dir(&g, OBJ_ROOT, "d");
	g.force_table = false;
	gen_link(&g, dir, "self", dir);
	gen_finish(&g);
	expect(&g, FSIMG_ECORRUPT, "directory naming itself");
	gen_free(&g);
	gen_init(&g, GEN_DEFAULT_N);
	g.force_table = true;
	dir = gen_dir(&g, OBJ_ROOT, "d");
	g.force_table = false;
	other = gen_dir(&g, OBJ_ROOT, "e");
	gen_link(&g, other, "again", dir);
	gen_finish(&g);
	expect(&g, FSIMG_ECORRUPT, "directory with two parents");
	gen_free(&g);
	gen_init(&g, GEN_DEFAULT_N);
	g.force_table = true;
	dir = gen_dir(&g, OBJ_ROOT, "d");
	g.force_table = false;
	other = gen_dir(&g, dir, "e");
	gen_link(&g, other, "up", dir);
	gen_finish(&g);
	expect(&g, FSIMG_ECORRUPT, "directory cycle");
	gen_free(&g);

	/* Kinds QNX4 can store that the profile does not read. */
	{
		static const uint16_t modes[] = { 020644, 060644, 0140644,
		    0170644, 0 };

		for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
			sample(&g, &s);
			put16(record_of(&g, s.attrs) + INODE_MODE, modes[i]);
			expect(&g, FSIMG_EUNSUPPORTED, "unsupported kind");
			gen_free(&g);
		}
	}

	/* 2000 entries in one directory with a one-page metadata cache. */
	gen_init(&g, 4000);
	dir = gen_dir(&g, OBJ_ROOT, "wide");
	for (unsigned i = 0; i < 2000; i++) {
		snprintf(name, sizeof(name), "w%04u", i);
		gen_file(&g, dir, name, NULL, 0);
	}
	gen_finish(&g);
	fixture_limited(&f, g.image, g.length, 0, 1);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(walk_all(&f, &entries, NULL) == FSIMG_OK);
	CHECK(entries == 2 + 1 + 2000);
	CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.scratch_peak > 0);
	fixture_free(&f);
	gen_free(&g);
}

/* The .inodes table and lost+found. */
static void
test_tables(void)
{
	gen_t g;
	sample_t s;
	fixture_t f;
	unsigned char *slot, *record;
	uint64_t entries;

	sample(&g, &s);
	slot = slot_of(&g, OBJ_INODES, 0);
	slot[3] = 'x';
	expect(&g, FSIMG_ECORRUPT, ".inodes signature");
	slot[3] = 'T';
	slot[18] = 0x11;
	expect(&g, FSIMG_ECORRUPT, ".inodes signature value");
	slot[18] = 0x10;
	slot[40] = 1;
	expect(&g, FSIMG_ECORRUPT, ".inodes signature padding");
	slot[40] = 0;
	record = block_at(&g, 2) + 64;
	put32(record + INODE_SIZE, 8000);
	expect(&g, FSIMG_ECORRUPT, ".inodes size not whole blocks");
	put32(record + INODE_SIZE, 8192);
	/* A used .inodes slot no link names is the verifier's to report. */
	slot = slot_of(&g, OBJ_INODES, 9);
	bcopy(record_of(&g, s.linked), slot, 64);
	CHECK(run_image(g.image, g.length, &entries, NULL) == FSIMG_OK);
	CHECK(entries == SAMPLE_ENTRIES);
	gen_free(&g);

	/* lost+found: ".." at the root's "." slot, as the formatter writes it;
	 * at the root record, which every other child of the root names but
	 * no native lost+found does; or anywhere else. A used slot is still
	 * never listed.
	 */
	sample(&g, &s);
	slot = slot_of(&g, OBJ_LOST, 1);
	CHECK(get32(slot + LINK_BLOCK) == 3 + g.bitmap_blocks &&
	    slot[LINK_SLOT] == 0);
	put32(slot + LINK_BLOCK, 2);
	expect(&g, FSIMG_EUNSUPPORTED, "lost+found dot-dot at the root record");
	slot[LINK_SLOT] = 1;
	expect(&g, FSIMG_ECORRUPT, "lost+found dot-dot elsewhere");
	put32(slot + LINK_BLOCK, 3 + g.bitmap_blocks);
	slot[LINK_SLOT] = 0;
	expect(&g, FSIMG_OK, "lost+found dot-dot restored");
	slot = slot_of(&g, OBJ_LOST, 0);
	slot[LINK_SLOT] = 3;
	expect(&g, FSIMG_ECORRUPT, "lost+found dot");
	slot[LINK_SLOT] = 4;
	gen_write_link(&g, gen_logical_position(&g, OBJ_LOST, 5), "orphan",
	    g.objects[s.linked].record);
	fixture_new(&f, g.image, g.length);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(qnx4_state(f.reader)->lost_found_used);
	CHECK(walk_all(&f, &entries, NULL) == FSIMG_OK);
	CHECK(entries == SAMPLE_ENTRIES);
	fixture_free(&f);
	/* The lost+found slot in the root stays hidden whatever it names. */
	slot = slot_of(&g, OBJ_ROOT, 6);
	slot[0] = 'L';
	expect(&g, FSIMG_OK, "renamed lost+found slot");
	gen_free(&g);
}

/* Owners, modes and times read as stored. */
static void
test_metadata(void)
{
	static const uint16_t modes[] = { 0100000, 0100644, 0100755, 0104755,
	    0102755, 0101777 };
	gen_t g;
	fixture_t f;
	const fsimg_entry_t *entry;
	uint32_t ids[sizeof(modes) / sizeof(modes[0])];
	char name[16];
	size_t i;

	gen_init(&g, GEN_DEFAULT_N);
	for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		snprintf(name, sizeof(name), "m%zu", i);
		ids[i] = gen_file(&g, OBJ_ROOT, name, NULL, 0);
		g.objects[ids[i]].mode = modes[i];
		g.objects[ids[i]].uid = (uint16_t)(i == 0 ? 0 : 65533 + i % 3);
		g.objects[ids[i]].gid = (uint16_t)(i == 0 ? 0 : 65535 - i % 3);
		g.objects[ids[i]].times[0] = UINT32_C(2147483648) + (uint32_t)i;
	}
	gen_finish(&g);
	fixture_new(&f, g.image, g.length);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	while (fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK) {
		if (entry->path[1] != 'm')
			continue;
		i = (size_t)(entry->path[2] - '0');
		CHECK(entry->metadata.mode == (modes[i] & 07777u));
		CHECK(entry->metadata.uid == g.objects[ids[i]].uid);
		CHECK(entry->metadata.gid == g.objects[ids[i]].gid);
		CHECK(entry->metadata.created.seconds ==
		    (int64_t)UINT32_C(2147483648) + (int64_t)i);
	}
	fixture_free(&f);
	gen_free(&g);
}

/* Allocation, scratch, read and cancellation failures and the work limit
 * end the operation cleanly; fixture_free checks that nothing leaked.
 */
static void
test_failures(void)
{
	gen_t g;
	sample_t s;
	fixture_t f;
	fsimg_error_t error;
	fsimg_status_t status;
	uint64_t entries, n, failures;

	sample(&g, &s);
	for (n = 1;; n++) {
		fixture_new(&f, g.image, g.length);
		fsimg_test_fail_alloc_after(f.ctx, n);
		status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL);
		if (status == FSIMG_OK)
			status = walk_all(&f, &entries, NULL);
		fsimg_test_fail_alloc_after(f.ctx, 0);
		fixture_free(&f);
		if (status == FSIMG_OK)
			break;
		CHECK(status == FSIMG_ENOMEM || status == FSIMG_ELIMIT);
		CHECK(n < 100000);
	}
	CHECK(n > 1);

	/* Every kind of scratch operation failing, one occurrence after
	 * another, in a context whose single cache page makes stores spill.
	 */
	for (unsigned point = FSIMG_META_OPEN; point <= FSIMG_META_WRITE;
	    point++) {
		failures = 0;
		for (n = 1;; n += n < 64 ? 1 : n / 8) {
			fixture_limited(&f, g.image, g.length, 0, 1);
			fsimg_test_meta_fail_after(f.ctx, (fsimg_meta_fault_t)point,
			    n, ENOSPC);
			status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader,
			    &error);
			if (status == FSIMG_OK)
				status = walk_all(&f, &entries, &error);
			fsimg_test_meta_fail_after(f.ctx, (fsimg_meta_fault_t)point,
			    0, 0);
			fixture_free(&f);
			if (status == FSIMG_OK)
				break;
			CHECK(status == FSIMG_EIO && error.os_error == ENOSPC);
			failures++;
			CHECK(n < 100000);
		}
		CHECK(failures > 0);
	}

	/* Cancellation at every poll until the listing completes. */
	failures = 0;
	for (n = 1;; n++) {
		fixture_new(&f, g.image, g.length);
		f.cancel_after = n;
		status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL);
		if (status == FSIMG_OK)
			status = walk_all(&f, &entries, NULL);
		fixture_free(&f);
		if (status == FSIMG_OK)
			break;
		CHECK(status == FSIMG_ECANCELLED);
		failures++;
		CHECK(n < 100000);
	}
	CHECK(failures > 0);

	/* The work limit. */
	fixture_limited(&f, g.image, g.length, 40, 0);
	status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL);
	if (status == FSIMG_OK)
		status = walk_all(&f, &entries, NULL);
	CHECK(status == FSIMG_ELIMIT);
	fixture_free(&f);

	/* A failed read of the bitmap fails the open, of an extent block the
	 * listing, and of file content the read, never the listing before it.
	 */
	fixture_new(&f, g.image, g.length);
	f.fail_first = 2 * 512;
	f.fail_end = 3 * 512;
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_EIO);
	fixture_free(&f);
	fixture_new(&f, g.image, g.length);
	f.fail_first = (uint64_t)(g.objects[s.chained].chain[0] - 1) * 512;
	f.fail_end = f.fail_first + 512;
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(walk_all(&f, &entries, NULL) == FSIMG_EIO);
	fixture_free(&f);
	fixture_new(&f, g.image, g.length);
	f.fail_first = (uint64_t)(g.objects[s.one].extents[0].first - 1) * 512;
	f.fail_end = f.fail_first + 512;
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	{
		const fsimg_entry_t *entry;
		unsigned char buffer[4];
		size_t count;
		bool failed = false;

		while ((status = fsimg_reader_next(f.reader, &entry, NULL)) ==
		    FSIMG_OK) {
			if (strcmp(entry->path, "/one") != 0)
				continue;
			CHECK(fsimg_reader_read_at(f.reader, 0, buffer,
			    sizeof(buffer), &count, NULL) == FSIMG_EIO);
			failed = true;
		}
		CHECK(failed);
	}
	fixture_free(&f);

	/* Short reads of seven bytes at most change nothing. */
	fixture_new(&f, g.image, g.length);
	f.short_max = 7;
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(walk_all(&f, &entries, NULL) == FSIMG_OK);
	CHECK(entries == SAMPLE_ENTRIES);
	fixture_free(&f);
	gen_free(&g);
}

/* Remove what extraction created beneath directory fd: a tree of
 * directories and regular files.
 */
static void
remove_tree(int parent, const char *name)
{
	struct stat st;
	struct dirent *entry;
	DIR *dir;
	int fd;

	CHECK(fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) == 0);
	if (!S_ISDIR(st.st_mode)) {
		CHECK(unlinkat(parent, name, 0) == 0);
		return;
	}
	fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC |
	    O_NOFOLLOW);
	CHECK(fd != -1);
	dir = fdopendir(fd);
	CHECK(dir != NULL);
	while ((entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0)
			continue;
		remove_tree(fd, entry->d_name);
		rewinddir(dir);
	}
	CHECK(closedir(dir) == 0);
	CHECK(unlinkat(parent, name, AT_REMOVEDIR) == 0);
}

static void
check_file(fixture_t *f, const char *path, const void *bytes, size_t length)
{
	char full[1200], buffer[2000];
	ssize_t got;
	int fd;

	snprintf(full, sizeof(full), "%s/%s", f->directory, path);
	fd = open(full, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	CHECK(fd != -1);
	got = read(fd, buffer, sizeof(buffer));
	CHECK(got == (ssize_t)length && bcmp(buffer, bytes, length) == 0);
	CHECK(close(fd) == 0);
}

static void
test_extract(void)
{
	gen_t g;
	sample_t s;
	fixture_t f;
	fsimg_extract_options_t options;
	fsimg_stats_t stats;
	fsimg_error_t error;
	unsigned char *bytes;

	sample(&g, &s);
	bytes = pattern(64 * 512, 9);
	fixture_new(&f, g.image, g.length);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	fsimg_extract_options_init(&options);
	options.selection = "/d1";
	CHECK(fsimg_extract_at(f.reader, f.scratch, "out", &options, NULL) ==
	    FSIMG_OK);
	check_file(&f, "out/d1/d2-with-a-long-name/f", "sub", 3);
	check_file(&f, "out/d1/hardlink", bytes + 3, 1500);
	remove_tree(f.scratch, "out");
	fsimg_reader_free(f.reader);

	/* A symbolic link or FIFO fails extraction unless skipped. */
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	options.selection = "/fifo";
	CHECK(fsimg_extract_at(f.reader, f.scratch, "out", &options, NULL) ==
	    FSIMG_EUNSUPPORTED);
	remove_tree(f.scratch, "out");
	fsimg_reader_free(f.reader);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	options.flags |= FSIMG_EXTRACT_SKIP_UNSUPPORTED;
	options.selection = NULL;
	/* The whole volume: the 505-byte name exceeds every supported host's
	 * component limit, after the other entries were written or skipped.
	 */
	CHECK(fsimg_extract_at(f.reader, f.scratch, "out", &options, &error) ==
	    FSIMG_EUNSUPPORTED);
	CHECK(error.os_error == ENAMETOOLONG);
	remove_tree(f.scratch, "out");
	fsimg_reader_free(f.reader);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	options.selection = "/symlink";
	CHECK(fsimg_extract_at(f.reader, f.scratch, "out", &options, NULL) ==
	    FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.skipped_entries >= 1);
	remove_tree(f.scratch, "out");
	fixture_free(&f);
	free(bytes);
	gen_free(&g);
}

/*
 * Command modes for the fuzz seeds and the native-capture comparison.
 */

static void
write_image(const char *path, const gen_t *g)
{
	int fd;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	CHECK(fd != -1);
	CHECK(write(fd, g->image, (size_t)g->length) == (ssize_t)g->length);
	CHECK(close(fd) == 0);
}

static void
emit_seed(const char *path)
{
	gen_t g;
	sample_t s;

	sample(&g, &s);
	write_image(path, &g);
	gen_free(&g);
}

/* A small volume whose one file has 122 extents in a three-block chain. */
static void
emit_chain_seed(const char *path)
{
	gen_t g;
	unsigned char *bytes;

	gen_init(&g, 400);
	bytes = pattern(122 * 512, 1);
	gen_file_parts(&g, OBJ_ROOT, "f", bytes, 122 * 512 - 3, 122);
	gen_finish(&g);
	write_image(path, &g);
	free(bytes);
	gen_free(&g);
}

/* ls(1)'s ten-character mode string. */
static void
mode_string(uint16_t mode, char out[11])
{
	static const char rwx[] = "rwxrwxrwx";

	switch (mode & 0170000) {
	case 040000:
		out[0] = 'd';
		break;
	case 0120000:
		out[0] = 'l';
		break;
	case 010000:
		out[0] = 'p';
		break;
	default:
		out[0] = '-';
		break;
	}
	for (unsigned i = 0; i < 9; i++)
		out[1 + i] = (mode & (0400u >> i)) != 0 ? rwx[i] : '-';
	if ((mode & 04000) != 0)
		out[3] = out[3] == 'x' ? 's' : 'S';
	if ((mode & 02000) != 0)
		out[6] = out[6] == 'x' ? 's' : 'S';
	if ((mode & 01000) != 0)
		out[9] = out[9] == 'x' ? 't' : 'T';
	out[10] = '\0';
}

static void
manifest_line(const char *path, const qnx4_inode_t *record, uint64_t size,
    const char *target)
{
	char mode[11];

	mode_string(record->mode, mode);
	printf("E\t%s\t%s\t%u\t%u\t%u\t%" PRIu64 "%s%s\n", path, mode,
	    record->links, record->uid, record->gid, size,
	    target != NULL ? "\t" : "", target != NULL ? target : "");
}

/*
 * Print one line per object as ls(1) -l would describe it, for the
 * integration script to compare with a native manifest: "E", path, mode
 * string, the record's link count, owner, size and a symbolic link's
 * target, separated by tabs so a name may hold spaces. A directory's size
 * is its record's, as ls(1) shows it.
 */
static int
manifest(const char *image, uint64_t offset, uint64_t length)
{
	fsimg_options_t options;
	fsimg_ctx_t *ctx;
	fsimg_io_t *file, *io;
	fsimg_reader_t *reader;
	const fsimg_entry_t *entry;
	const qnx4_inode_t *record;
	fsimg_error_t error;
	fsimg_status_t status;
	char directory[1024], target[1024];
	uint64_t size;
	size_t count;
	int fd, scratch;

	temp_name(directory, sizeof(directory), "fsimg-qnx4");
	CHECK(mkdtemp(directory) != NULL);
	scratch = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(scratch != -1);
	fsimg_options_init(&options);
	options.memory_bytes = 64 * 1024 * 1024;
	options.scratch_directory_fd = scratch;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	fd = open(image, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	CHECK(fd != -1);
	CHECK(fsimg_io_from_fd(ctx, fd, FSIMG_IO_READ, &file, NULL) == FSIMG_OK);
	if (length == 0) {
		CHECK(fsimg_io_get_size(file, &length, NULL) == FSIMG_OK);
		length -= offset;
	}
	CHECK(fsimg_io_slice(file, offset, length, &io, NULL) == FSIMG_OK);
	status = fsimg_reader_open(ctx, io, NULL, &reader, &error);
	if (status != FSIMG_OK) {
		fprintf(stderr, "open %d %s\n", status, error.message);
		goto done;
	}
	manifest_line("/", &qnx4_state(reader)->root[QNX4_RECORD_ROOT],
	    qnx4_state(reader)->root[QNX4_RECORD_ROOT].size, NULL);
	while ((status = fsimg_reader_next(reader, &entry, &error)) == FSIMG_OK) {
		record = &qnx4_state(reader)->current;
		size = entry->kind == FSIMG_ENTRY_DIRECTORY ? record->size :
		    entry->size;
		if (entry->kind != FSIMG_ENTRY_SYMLINK) {
			manifest_line(entry->path, record, size, NULL);
			continue;
		}
		CHECK(entry->size < sizeof(target));
		status = fsimg_reader_read_at(reader, 0, target,
		    (size_t)entry->size, &count, &error);
		if (status != FSIMG_OK)
			break;
		CHECK(count == entry->size);
		target[count] = '\0';
		manifest_line(entry->path, record, size, target);
	}
	if (status == FSIMG_DONE)
		status = FSIMG_OK;
	else
		fprintf(stderr, "list %d %s\n", status, error.message);
	fsimg_reader_free(reader);
done:
	fsimg_io_free(io);
	fsimg_io_free(file);
	fsimg_ctx_free(ctx);
	CHECK(close(scratch) == 0);
	CHECK(rmdir(directory) == 0);
	return (status == FSIMG_OK ? 0 : 1);
}

static uint64_t
number(const char *text)
{
	char *end;
	unsigned long long value;

	errno = 0;
	value = strtoull(text, &end, 10);
	CHECK(errno == 0 && end != text && *end == '\0');
	return ((uint64_t)value);
}

int
main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--seed") == 0) {
		emit_seed(argv[2]);
		return (0);
	}
	if (argc == 3 && strcmp(argv[1], "--seed-chain") == 0) {
		emit_chain_seed(argv[2]);
		return (0);
	}
	if (argc >= 3 && argc <= 5 && strcmp(argv[1], "--manifest") == 0)
		return (manifest(argv[2], argc > 3 ? number(argv[3]) : 0,
		    argc > 4 ? number(argv[4]) : 0));
	CHECK(argc == 1);
	test_codecs();
	test_sample();
	test_hard_links();
	test_geometry();
	test_bitmap();
	test_root_block();
	test_probe();
	test_names();
	test_name_lengths();
	test_statuses();
	test_records();
	test_extents();
	test_directories();
	test_tables();
	test_metadata();
	test_failures();
	test_extract();
	return (0);
}
