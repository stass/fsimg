/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "extract.h"
#include "format.h"
#include "fsimg.h"
#include "test.h"

/*
 * Tests of the ImageFS reader, written from the format design
 * (specs/design-ifs.md) with offset tables of their own; nothing here
 * shares code with lib/formats/ifs. format.h names the registered
 * descriptors for the probe tests; no test calls a driver function.
 *
 * Two independent pieces serve as oracles:
 *
 * * The generator builds an image in memory from a list of members in
 *   table order: the header, the mountpoint and its padding, one record per
 *   member, the terminator, the file data and the checksum trailer. Tests
 *   then mutate the finished bytes.
 *
 * * The model decodes an image by the design's rules with a different
 *   algorithm from the driver's: it inserts every record into an in-memory
 *   tree in table order, a later record with a path replacing the earlier
 *   one, synthesizing parents as needed, and lists the tree depth first
 *   with children in component order. It predicts the status of open and,
 *   for an image that opens, every delivered entry and every info count.
 *   The random differential test and the native captures compare the
 *   reader with it.
 */

#define GEN_HEADER	0x58u
#define GEN_POISON	0xa5u
#define GEN_MTIME	0x6ad7c93au
/* Comfortably above the reader's 8 KiB table reads. */
#define IFS_TEST_WINDOW	8192u

/* Header offsets. */
enum {
	H_FLAGS = 0x07,
	H_IMAGE_SIZE = 0x08,
	H_HDR_DIR_SIZE = 0x0c,
	H_DIR_OFFSET = 0x10,
	H_BOOT_INO = 0x14,
	H_SCRIPT_INO = 0x24,
	H_CHAIN_PADDR = 0x28,
	H_SPARE = 0x2c,
	H_MOUNT_FLAGS = 0x54,
	H_MOUNTPOINT = 0x58
};

/* Record offsets: the common prefix, then each kind's fields. */
enum {
	R_LENGTH = 0x00,
	R_EXTATTR = 0x02,
	R_INODE = 0x04,
	R_MODE = 0x08,
	R_GID = 0x0c,
	R_UID = 0x10,
	R_MTIME = 0x14,
	R_FILE_OFFSET = 0x18,
	R_FILE_SIZE = 0x1c,
	R_LINK_OFFSET = 0x18,
	R_LINK_SIZE = 0x1a,
	R_FIFO_WORDS = 0x18
};

typedef enum gen_kind {
	GEN_DIR,
	GEN_FILE,
	GEN_LINK,
	GEN_FIFO
} gen_kind_t;

/* The path offset and the mode type of each kind. */
static const struct {
	uint32_t	 path;
	uint32_t	 type;
	const char	*name;
} kinds[] = {
	[GEN_DIR] = { 0x18, 0040000, "directory" },
	[GEN_FILE] = { 0x20, 0100000, "file" },
	[GEN_LINK] = { 0x1c, 0120000, "link" },
	[GEN_FIFO] = { 0x20, 0010000, "fifo" }
};

/* One record. share, when not -1, names an earlier file whose data range
 * this file reuses. pad adds bytes of record padding. The generator fills
 * table_offset, length and data_offset.
 */
typedef struct gen_member {
	gen_kind_t	 kind;
	unsigned char	*path;
	size_t		 path_length;
	unsigned char	*bytes;		/* file data or link target */
	size_t		 bytes_length;
	uint32_t	 mode;
	uint32_t	 uid;
	uint32_t	 gid;
	uint32_t	 mtime;
	uint32_t	 inode;
	uint32_t	 pad;
	int		 share;
	uint32_t	 table_offset;
	uint32_t	 length;
	uint32_t	 data_offset;
} gen_member_t;

typedef struct gen {
	gen_member_t	*members;
	size_t		 count;
	size_t		 capacity;
	unsigned char	 mountpoint[1200];
	size_t		 mountpoint_length;
	unsigned char	 mountpoint_pad;
	bool		 big;
	uint32_t	 boot[4];
	uint32_t	 script;
	uint32_t	 chain;
	uint32_t	 mount_flags;
	uint32_t	 next_inode;
	uint32_t	 data_align;
	unsigned char	*image;
	size_t		 length;
	uint32_t	 dir_offset;
	uint32_t	 hdr_dir_size;
	uint32_t	 image_size;
} gen_t;

static void
put16(const gen_t *g, unsigned char *p, uint32_t v)
{
	if (g->big) {
		p[0] = (unsigned char)(v >> 8);
		p[1] = (unsigned char)v;
	} else {
		p[0] = (unsigned char)v;
		p[1] = (unsigned char)(v >> 8);
	}
}

static void
put32(const gen_t *g, unsigned char *p, uint32_t v)
{
	for (unsigned i = 0; i < 4; i++)
		p[g->big ? 3 - i : i] = (unsigned char)(v >> (8 * i));
}

static uint32_t
get32(const gen_t *g, const unsigned char *p)
{
	uint32_t v = 0;

	for (unsigned i = 0; i < 4; i++)
		v |= (uint32_t)p[g->big ? 3 - i : i] << (8 * i);
	return (v);
}

static uint32_t
round4(uint64_t v)
{
	return ((uint32_t)((v + 3) & ~(uint64_t)3));
}

static void *
xmalloc(size_t length)
{
	void *p = malloc(length == 0 ? 1 : length);

	CHECK(p != NULL);
	return (p);
}

static unsigned char *
copy_bytes(const void *bytes, size_t length)
{
	unsigned char *p = xmalloc(length + 1);

	if (length != 0)
		memcpy(p, bytes, length);
	p[length] = '\0';
	return (p);
}

static void
gen_init(gen_t *g)
{
	bzero(g, sizeof(*g));
	g->mountpoint[0] = '/';
	g->mountpoint_length = 1;
	g->next_inode = 1;
	g->data_align = 1;
}

static void
gen_free(gen_t *g)
{
	for (size_t i = 0; i < g->count; i++) {
		free(g->members[i].path);
		free(g->members[i].bytes);
	}
	free(g->members);
	free(g->image);
	bzero(g, sizeof(*g));
}

static void
gen_mountpoint(gen_t *g, const void *bytes, size_t length)
{
	CHECK(length < sizeof(g->mountpoint));
	memcpy(g->mountpoint, bytes, length);
	g->mountpoint_length = length;
}

/* Append a member with path of length bytes and its kind's defaults; the
 * index is returned for later adjustment.
 */
static size_t
gen_add_bytes(gen_t *g, gen_kind_t kind, const void *path, size_t length,
    const void *bytes, size_t bytes_length)
{
	static const uint32_t permissions[] = {
		[GEN_DIR] = 0755, [GEN_FILE] = 0644, [GEN_LINK] = 0777,
		[GEN_FIFO] = 0644
	};
	gen_member_t *m;

	if (g->count == g->capacity) {
		g->capacity = g->capacity == 0 ? 16 : 2 * g->capacity;
		g->members = realloc(g->members,
		    g->capacity * sizeof(*g->members));
		CHECK(g->members != NULL);
	}
	m = &g->members[g->count];
	bzero(m, sizeof(*m));
	m->kind = kind;
	m->path = copy_bytes(path, length);
	m->path_length = length;
	m->bytes = copy_bytes(bytes, bytes_length);
	m->bytes_length = bytes_length;
	m->mode = kinds[kind].type | permissions[kind];
	m->mtime = GEN_MTIME;
	m->inode = g->next_inode++;
	m->share = -1;
	return (g->count++);
}

static size_t
gen_dir(gen_t *g, const char *path)
{
	return (gen_add_bytes(g, GEN_DIR, path, strlen(path), NULL, 0));
}

static size_t
gen_file(gen_t *g, const char *path, const char *data)
{
	return (gen_add_bytes(g, GEN_FILE, path, strlen(path), data,
	    strlen(data)));
}

static size_t
gen_link(gen_t *g, const char *path, const char *target)
{
	return (gen_add_bytes(g, GEN_LINK, path, strlen(path), target,
	    strlen(target)));
}

static size_t
gen_fifo(gen_t *g, const char *path)
{
	return (gen_add_bytes(g, GEN_FIFO, path, strlen(path), NULL, 0));
}

/* The root record mkifs writes first: a directory with the empty path. */
static size_t
gen_root(gen_t *g)
{
	return (gen_dir(g, ""));
}

/* The additive inverse of the 32-bit word sum of the image before the
 * trailer, so the sum of every word is zero.
 */
static void
gen_trailer(gen_t *g)
{
	uint32_t sum = 0;

	for (uint32_t o = 0; o + 4 < g->image_size; o += 4)
		sum += get32(g, g->image + o);
	put32(g, g->image + g->image_size - 4, (uint32_t)(0 - sum));
}

/* Encode the header from g's layout into out, which holds dir_offset
 * zeroed bytes: the fixed fields, the mountpoint, its NUL, and the padding
 * to dir_offset filled with mountpoint_pad.
 */
static void
gen_encode_header(const gen_t *g, unsigned char *out)
{
	memcpy(out, "imagefs", 7);
	out[H_FLAGS] = g->big ? 0x05 : 0x04;
	put32(g, out + H_IMAGE_SIZE, g->image_size);
	put32(g, out + H_HDR_DIR_SIZE, g->hdr_dir_size);
	put32(g, out + H_DIR_OFFSET, g->dir_offset);
	for (unsigned i = 0; i < 4; i++)
		put32(g, out + H_BOOT_INO + 4 * i, g->boot[i]);
	put32(g, out + H_SCRIPT_INO, g->script);
	put32(g, out + H_CHAIN_PADDR, g->chain);
	put32(g, out + H_MOUNT_FLAGS, g->mount_flags);
	memcpy(out + H_MOUNTPOINT, g->mountpoint, g->mountpoint_length);
	for (size_t o = H_MOUNTPOINT + g->mountpoint_length + 1;
	    o < g->dir_offset; o++)
		out[o] = g->mountpoint_pad;
}

/* Encode member m's record into r, which holds m->length zeroed bytes: the
 * prefix, the kind's fields and the path, and a link's target. A file's
 * data lies elsewhere and is not copied here.
 */
static void
gen_encode_record(const gen_t *g, const gen_member_t *m, unsigned char *r)
{
	put16(g, r + R_LENGTH, m->length);
	put32(g, r + R_INODE, m->inode);
	put32(g, r + R_MODE, m->mode);
	put32(g, r + R_GID, m->gid);
	put32(g, r + R_UID, m->uid);
	put32(g, r + R_MTIME, m->mtime);
	memcpy(r + kinds[m->kind].path, m->path, m->path_length);
	if (m->kind == GEN_FILE) {
		put32(g, r + R_FILE_OFFSET, m->data_offset);
		put32(g, r + R_FILE_SIZE, (uint32_t)m->bytes_length);
	}
	if (m->kind == GEN_LINK) {
		put16(g, r + R_LINK_OFFSET, (uint32_t)m->path_length + 1);
		put16(g, r + R_LINK_SIZE, (uint32_t)m->bytes_length);
		memcpy(r + kinds[m->kind].path + m->path_length + 1, m->bytes,
		    m->bytes_length);
	}
}

/* The bytes of a member's record before padding: its kind's path offset,
 * the path and its NUL, and a link's target and its NUL.
 */
static uint64_t
gen_record_bytes(gen_kind_t kind, size_t path_length, size_t target_length)
{
	uint64_t length = kinds[kind].path + path_length + 1;

	if (kind == GEN_LINK)
		length += target_length + 1;
	return (length);
}

/* Lay the image out and encode it: the header, the mountpoint padded to
 * dir_offset, each record padded with zeros to a multiple of 4 plus its
 * extra padding, the terminator, each file's data at the next multiple of
 * data_align, and the trailer. Bytes past image_size, in a buffer of
 * extra bytes more, hold a poison byte.
 */
static void
gen_finish_extra(gen_t *g, size_t extra)
{
	uint64_t offset, data;

	free(g->image);
	g->dir_offset = round4(GEN_HEADER + g->mountpoint_length + 1);
	offset = g->dir_offset;
	for (size_t i = 0; i < g->count; i++) {
		gen_member_t *m = &g->members[i];

		m->table_offset = (uint32_t)offset;
		m->length = round4(gen_record_bytes(m->kind, m->path_length,
		    m->bytes_length)) + m->pad;
		offset += m->length;
	}
	g->hdr_dir_size = (uint32_t)offset + 4;
	data = g->hdr_dir_size;
	for (size_t i = 0; i < g->count; i++) {
		gen_member_t *m = &g->members[i];

		if (m->kind != GEN_FILE)
			continue;
		if (m->share >= 0) {
			m->data_offset = g->members[m->share].data_offset;
			continue;
		}
		data = (data + g->data_align - 1) / g->data_align * g->data_align;
		m->data_offset = (uint32_t)data;
		data += m->bytes_length;
	}
	g->image_size = round4(data) + 4;
	g->length = g->image_size + extra;
	g->image = calloc(1, g->length);
	CHECK(g->image != NULL);
	memset(g->image + g->image_size, GEN_POISON, extra);

	gen_encode_header(g, g->image);
	for (size_t i = 0; i < g->count; i++) {
		gen_member_t *m = &g->members[i];

		gen_encode_record(g, m, g->image + m->table_offset);
		if (m->kind == GEN_FILE)
			memcpy(g->image + m->data_offset, m->bytes,
			    m->bytes_length);
	}
	gen_trailer(g);
}

static void
gen_finish(gen_t *g)
{
	gen_finish_extra(g, 0);
}

static unsigned char *
gen_record(gen_t *g, size_t index)
{
	return (g->image + g->members[index].table_offset);
}

/* Make the last record length bytes long, moving the terminator to its new
 * end, so the table still tiles; the record's bytes past the new length
 * become the terminator and padding.
 */
static void
gen_shorten_last(gen_t *g, uint32_t length)
{
	gen_member_t *m = &g->members[g->count - 1];

	put16(g, g->image + m->table_offset + R_LENGTH, length);
	g->hdr_dir_size = m->table_offset + length + 4;
	put32(g, g->image + H_HDR_DIR_SIZE, g->hdr_dir_size);
	bzero(g->image + m->table_offset + length, 4);
	gen_trailer(g);
}

/*
 * The model.
 */

/* Strict UTF-8 without control bytes or DEL, and neither "." nor "..",
 * decoded here byte by byte rather than through the library.
 */
static bool
model_utf8(const unsigned char *s, size_t length)
{
	size_t i = 0;

	while (i < length) {
		uint32_t c = s[i], minimum;
		unsigned more;

		if (c < 0x80) {
			i++;
			continue;
		}
		if (c >= 0xc2 && c <= 0xdf) {
			more = 1;
			c &= 0x1f;
			minimum = 0x80;
		} else if (c >= 0xe0 && c <= 0xef) {
			more = 2;
			c &= 0x0f;
			minimum = 0x800;
		} else if (c >= 0xf0 && c <= 0xf4) {
			more = 3;
			c &= 0x07;
			minimum = 0x10000;
		} else {
			return (false);
		}
		if (length - i <= more)
			return (false);
		for (unsigned k = 1; k <= more; k++) {
			if ((s[i + k] & 0xc0) != 0x80)
				return (false);
			c = c << 6 | (s[i + k] & 0x3f);
		}
		if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
			return (false);
		i += more + 1;
	}
	return (true);
}

static bool
model_bytes_rule(const unsigned char *s, size_t length)
{
	for (size_t i = 0; i < length; i++)
		if (s[i] < 0x20 || s[i] == 0x7f)
			return (false);
	return (model_utf8(s, length));
}

static bool
model_in_profile(const unsigned char *s, size_t length)
{
	if (length == 0 || (length == 1 && s[0] == '.') ||
	    (length == 2 && s[0] == '.' && s[1] == '.'))
		return (false);
	return (memchr(s, '/', length) == NULL && model_bytes_rule(s, length));
}

/* A node of the model's tree. record is the index of the record at its
 * path, or -1 for a synthesized directory.
 */
typedef struct model_node {
	unsigned char	*name;
	size_t		 name_length;
	long		 record;
	size_t		 first_child;	/* index + 1, 0 for none */
	size_t		 next_sibling;	/* index + 1, 0 for none */
} model_node_t;

typedef struct model_record {
	gen_kind_t	 kind;
	uint32_t	 offset;
	uint32_t	 inode;
	uint32_t	 permissions;
	uint32_t	 uid;
	uint32_t	 gid;
	uint32_t	 mtime;
	const unsigned char *path;
	size_t		 path_length;
	const unsigned char *content;
	size_t		 content_length;
	bool		 shadowed;	/* no node delivers this record */
} model_record_t;

/* One entry the reader must deliver, in traversal order. */
typedef struct model_entry {
	char		*path;		/* "/a/b" */
	gen_kind_t	 kind;
	uint32_t	 permissions;
	uint32_t	 uid;
	uint32_t	 gid;
	uint32_t	 mtime;
	const unsigned char *content;
	size_t		 content_length;
} model_entry_t;

typedef struct model {
	fsimg_status_t	 status;
	model_record_t	*records;
	size_t		 record_count;
	model_node_t	*nodes;
	size_t		 node_count;
	model_entry_t	*entries;
	size_t		 entry_count;
	uint64_t	 by_flag[4];
	uint64_t	 implicit;
	uint64_t	 shadowed;
	uint64_t	 below;		/* the shadowed below a non-directory */
	uint64_t	 omitted;
	uint64_t	 omitted_references;
	/* The nonzero references whose word a shadowed record carries. */
	uint64_t	 shadowed_references;
	uint32_t	 references[5];
	/* Per reference, the nodes carrying its inode word, the root and
	 * omitted ones included, and the kind of the last of them.
	 */
	uint64_t	 reference_matches[5];
	gen_kind_t	 reference_kind[5];
	/* The content bytes of every walked file and link, omitted ones
	 * included: what payload verification reads.
	 */
	uint64_t	 walked_bytes;
	uint32_t	 mount_flags;
	uint32_t	 chain;
	uint32_t	 image_size;
	/* The slice length, and the word sum of [0, image_size). */
	size_t		 length;
	uint32_t	 sum;
	bool		 big;
	bool		 mountpoint_valid;
	char		 mountpoint[1025];
} model_t;

static uint32_t
model_u16(const model_t *m, const unsigned char *p)
{
	return (m->big ? (uint32_t)p[0] << 8 | p[1] :
	    (uint32_t)p[1] << 8 | p[0]);
}

static uint32_t
model_u32(const model_t *m, const unsigned char *p)
{
	return (m->big ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	    (uint32_t)p[2] << 8 | p[3] : (uint32_t)p[3] << 24 |
	    (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]);
}

/* The header and mountpoint rules; FSIMG_OK when the table may be read. */
static fsimg_status_t
model_header(model_t *m, const unsigned char *image, size_t length,
    uint32_t *dir_offset, uint32_t *hdr_dir_size)
{
	const unsigned char *nul;
	size_t region, search;

	if (length < GEN_HEADER || memcmp(image, "imagefs", 7) != 0)
		return (FSIMG_EFORMAT);
	if (image[H_FLAGS] != 0x04 && image[H_FLAGS] != 0x05)
		return (FSIMG_EUNSUPPORTED);
	m->big = image[H_FLAGS] == 0x05;
	m->image_size = model_u32(m, image + H_IMAGE_SIZE);
	*hdr_dir_size = model_u32(m, image + H_HDR_DIR_SIZE);
	*dir_offset = model_u32(m, image + H_DIR_OFFSET);
	if (*dir_offset < GEN_HEADER ||
	    (uint64_t)*dir_offset + 4 > *hdr_dir_size ||
	    (uint64_t)*hdr_dir_size + 4 > m->image_size ||
	    m->image_size % 4 != 0 || m->image_size > length)
		return (FSIMG_ECORRUPT);
	for (unsigned i = 0; i < 40; i++)
		if (image[H_SPARE + i] != 0)
			return (FSIMG_EUNSUPPORTED);
	if (*dir_offset % 4 != 0 || *hdr_dir_size % 4 != 0)
		return (FSIMG_EUNSUPPORTED);
	for (unsigned i = 0; i < 4; i++)
		m->references[i] = model_u32(m, image + H_BOOT_INO + 4 * i);
	m->references[4] = model_u32(m, image + H_SCRIPT_INO);
	m->chain = model_u32(m, image + H_CHAIN_PADDR);
	m->mount_flags = model_u32(m, image + H_MOUNT_FLAGS);

	region = *dir_offset - GEN_HEADER;
	search = region < 1025 ? region : 1025;
	nul = memchr(image + GEN_HEADER, '\0', search);
	if (nul == NULL)
		return (region <= 1025 ? FSIMG_ECORRUPT : FSIMG_EUNSUPPORTED);
	if (round4((uint64_t)(nul - image) + 1) != *dir_offset)
		return (FSIMG_EUNSUPPORTED);
	m->mountpoint_valid = model_bytes_rule(image + GEN_HEADER,
	    (size_t)(nul - image) - GEN_HEADER);
	if (m->mountpoint_valid)
		memcpy(m->mountpoint, image + GEN_HEADER,
		    (size_t)(nul - image) - GEN_HEADER + 1);
	return (FSIMG_OK);
}

/* Whether a path has a shape fsimg represents: relative, without empty,
 * "." or ".." components.
 */
static bool
model_path_shape(const unsigned char *path, size_t length)
{
	size_t start = 0;

	if (length == 0)
		return (true);
	for (size_t i = 0; i <= length; i++) {
		if (i < length && path[i] != '/')
			continue;
		if (i == start || (i - start == 1 && path[start] == '.') ||
		    (i - start == 2 && path[start] == '.' &&
		    path[start + 1] == '.'))
			return (false);
		start = i + 1;
	}
	return (true);
}

/* Decode the record at offset under every record rule. */
static fsimg_status_t
model_record(model_t *m, const unsigned char *image, uint32_t offset,
    uint32_t terminator, uint32_t hdr_dir_size, uint32_t *length_out)
{
	const unsigned char *r = image + offset, *nul;
	model_record_t rec;
	uint32_t length, mode, type, sym_offset, sym_size;
	size_t span;
	unsigned nibble;

	bzero(&rec, sizeof(rec));
	length = model_u16(m, r + R_LENGTH);
	*length_out = length;
	if (length < 24 || (uint64_t)offset + length > terminator)
		return (FSIMG_ECORRUPT);
	if (length % 4 != 0)
		return (FSIMG_EUNSUPPORTED);
	mode = model_u32(m, r + R_MODE);
	type = mode & 0170000;
	if (mode > 0177777)
		return (FSIMG_EUNSUPPORTED);
	switch (type) {
	case 0040000: rec.kind = GEN_DIR; break;
	case 0100000: rec.kind = GEN_FILE; break;
	case 0120000: rec.kind = GEN_LINK; break;
	case 0010000: rec.kind = GEN_FIFO; break;
	default: return (FSIMG_EUNSUPPORTED);
	}
	if (model_u16(m, r + R_EXTATTR) != 0)
		return (FSIMG_EUNSUPPORTED);
	rec.inode = model_u32(m, r + R_INODE);
	nibble = rec.inode >> 28;
	if (rec.inode == 0 || (nibble != 0 && nibble != 8 && nibble != 0xc &&
	    nibble != 0xe))
		return (FSIMG_EUNSUPPORTED);
	if (kinds[rec.kind].path >= length)
		return (FSIMG_ECORRUPT);
	span = length - kinds[rec.kind].path;
	nul = memchr(r + kinds[rec.kind].path, '\0', span < 1025 ? span : 1025);
	if (nul == NULL)
		return (span <= 1025 ? FSIMG_ECORRUPT : FSIMG_EUNSUPPORTED);
	rec.path = r + kinds[rec.kind].path;
	rec.path_length = (size_t)(nul - rec.path);
	if (rec.kind == GEN_LINK) {
		sym_offset = model_u16(m, r + R_LINK_OFFSET);
		sym_size = model_u16(m, r + R_LINK_SIZE);
		if ((uint64_t)kinds[GEN_LINK].path + rec.path_length + 1 +
		    sym_size + 1 > length)
			return (FSIMG_ECORRUPT);
		if (sym_offset != rec.path_length + 1 || sym_size == 0 ||
		    sym_size > 1024 || rec.path[sym_offset + sym_size] != '\0' ||
		    memchr(rec.path + sym_offset, '\0', sym_size) != NULL)
			return (FSIMG_EUNSUPPORTED);
		rec.content = rec.path + sym_offset;
		rec.content_length = sym_size;
	}
	if (rec.kind == GEN_FIFO && (model_u32(m, r + R_FIFO_WORDS) != 0 ||
	    model_u32(m, r + R_FIFO_WORDS + 4) != 0))
		return (FSIMG_EUNSUPPORTED);
	if (!model_path_shape(rec.path, rec.path_length))
		return (FSIMG_EUNSUPPORTED);
	if (rec.kind == GEN_FILE) {
		uint32_t start = model_u32(m, r + R_FILE_OFFSET);
		uint32_t size = model_u32(m, r + R_FILE_SIZE);

		if (size != 0 && (start < hdr_dir_size ||
		    (uint64_t)start + size > (uint64_t)m->image_size - 4))
			return (FSIMG_ECORRUPT);
		rec.content = image + start;
		rec.content_length = size;
	}
	rec.offset = offset;
	rec.permissions = mode & 07777;
	rec.uid = model_u32(m, r + R_UID);
	rec.gid = model_u32(m, r + R_GID);
	rec.mtime = model_u32(m, r + R_MTIME);
	switch (nibble) {
	case 0x0: m->by_flag[0]++; break;
	case 0x8: m->by_flag[1]++; break;
	case 0xc: m->by_flag[2]++; break;
	default: m->by_flag[3]++; break;
	}
	m->records = realloc(m->records, (m->record_count + 1) *
	    sizeof(*m->records));
	CHECK(m->records != NULL);
	m->records[m->record_count++] = rec;
	return (FSIMG_OK);
}


/* The child of node with component name, created as a synthesized
 * directory and appended to node's children when absent.
 */
static size_t
model_child(model_t *m, size_t node, const unsigned char *name, size_t length)
{
	size_t child, last;

	last = 0;
	for (child = m->nodes[node].first_child; child != 0;
	    child = m->nodes[child - 1].next_sibling) {
		model_node_t *c = &m->nodes[child - 1];

		if (c->name_length == length &&
		    memcmp(c->name, name, length) == 0)
			return (child - 1);
		last = child;
	}
	m->nodes = realloc(m->nodes, (m->node_count + 1) * sizeof(*m->nodes));
	CHECK(m->nodes != NULL);
	m->nodes[m->node_count] = (model_node_t){
		.name = copy_bytes(name, length),
		.name_length = length,
		.record = -1
	};
	m->node_count++;
	if (last == 0)
		m->nodes[node].first_child = m->node_count;
	else
		m->nodes[last - 1].next_sibling = m->node_count;
	return (m->node_count - 1);
}

static int
model_name_order(const model_node_t *a, const model_node_t *b)
{
	size_t n = a->name_length < b->name_length ? a->name_length :
	    b->name_length;
	int c = memcmp(a->name, b->name, n);

	if (c != 0)
		return (c);
	return ((a->name_length > b->name_length) -
	    (a->name_length < b->name_length));
}

/* Record the delivered entry for node at path. */
static void
model_deliver(model_t *m, const model_node_t *node, const char *path)
{
	const model_record_t *rec;
	model_entry_t *e;

	m->entries = realloc(m->entries, (m->entry_count + 1) *
	    sizeof(*m->entries));
	CHECK(m->entries != NULL);
	e = &m->entries[m->entry_count++];
	bzero(e, sizeof(*e));
	e->path = (char *)copy_bytes(path, strlen(path));
	e->kind = GEN_DIR;
	e->permissions = 0755;
	if (node->record < 0)
		return;
	rec = &m->records[node->record];
	e->kind = rec->kind;
	e->permissions = rec->permissions;
	e->uid = rec->uid;
	e->gid = rec->gid;
	e->mtime = rec->mtime;
	e->content = rec->content;
	e->content_length = rec->content_length;
}

/* Mark the record of every node below node shadowed and count them. */
static uint64_t
model_shadow_below(model_t *m, size_t node)
{
	uint64_t count = 0;

	for (size_t c = m->nodes[node].first_child; c != 0;
	    c = m->nodes[c - 1].next_sibling) {
		if (m->nodes[c - 1].record >= 0) {
			m->records[m->nodes[c - 1].record].shadowed = true;
			count++;
		}
		count += model_shadow_below(m, c - 1);
	}
	return (count);
}

/* A node's record matches each nonzero reference carrying its inode word;
 * hidden names a node at or below a name outside the profile.
 */
static void
model_match(model_t *m, long record, bool hidden)
{
	const model_record_t *rec = &m->records[record];

	for (unsigned r = 0; r < 5; r++) {
		if (m->references[r] == 0 || m->references[r] != rec->inode)
			continue;
		m->reference_matches[r]++;
		m->reference_kind[r] = rec->kind;
		if (hidden)
			m->omitted_references++;
	}
}

/* List the subtree of node depth first, children in component order;
 * hidden is set below a name outside the profile. Below a node whose last
 * record is not a directory nothing is listed and every record is
 * shadowed, as procnto hides the records below a link (the format design
 * extends this to files and FIFOs); the parents there are neither listed
 * nor counted. model_shadow_below() counts nodes, one per path: the
 * earlier records of a duplicate path were counted by model_build().
 */
static void
model_list(model_t *m, size_t node, const char *path, bool hidden)
{
	size_t *children, count, capacity;
	uint64_t below;

	if (m->nodes[node].record >= 0 &&
	    m->records[m->nodes[node].record].kind != GEN_DIR) {
		below = model_shadow_below(m, node);
		m->below += below;
		m->shadowed += below;
		return;
	}
	count = 0;
	capacity = 16;
	children = xmalloc(capacity * sizeof(*children));
	for (size_t c = m->nodes[node].first_child; c != 0;
	    c = m->nodes[c - 1].next_sibling) {
		if (count == capacity) {
			capacity *= 2;
			children = realloc(children,
			    capacity * sizeof(*children));
			CHECK(children != NULL);
		}
		children[count++] = c - 1;
	}
	/* Insertion sort by component bytes. */
	for (size_t i = 1; i < count; i++) {
		for (size_t j = i; j > 0 && model_name_order(
		    &m->nodes[children[j]], &m->nodes[children[j - 1]]) < 0;
		    j--) {
			size_t t = children[j];

			children[j] = children[j - 1];
			children[j - 1] = t;
		}
	}
	for (size_t i = 0; i < count; i++) {
		const model_node_t *c = &m->nodes[children[i]];
		bool outside = !model_in_profile(c->name, c->name_length);
		size_t prefix = strlen(path);
		char *child_path = xmalloc(prefix + 1 + c->name_length + 1);

		memcpy(child_path, path, prefix);
		child_path[prefix] = '/';
		memcpy(child_path + prefix + 1, c->name, c->name_length);
		child_path[prefix + 1 + c->name_length] = '\0';
		if (outside && !hidden)
			m->omitted++;
		if (c->record < 0)
			m->implicit++;
		else {
			model_match(m, c->record, hidden || outside);
			if (m->records[c->record].kind == GEN_FILE ||
			    m->records[c->record].kind == GEN_LINK)
				m->walked_bytes +=
				    m->records[c->record].content_length;
		}
		if (!hidden && !outside)
			model_deliver(m, c, child_path);
		model_list(m, children[i], child_path, hidden || outside);
		free(child_path);
	}
	free(children);
}

static void
model_free(model_t *m)
{
	for (size_t i = 0; i < m->node_count; i++)
		free(m->nodes[i].name);
	for (size_t i = 0; i < m->entry_count; i++)
		free(m->entries[i].path);
	free(m->nodes);
	free(m->records);
	free(m->entries);
	bzero(m, sizeof(*m));
}

/* Decode image, a slice of length bytes, into m: m->status is what
 * opening and listing it must return.
 */
static void
model_build(model_t *m, const unsigned char *image, size_t length)
{
	uint32_t dir_offset, hdr_dir_size, offset, record_length;

	bzero(m, sizeof(*m));
	m->length = length;
	m->status = model_header(m, image, length, &dir_offset, &hdr_dir_size);
	if (m->status != FSIMG_OK)
		return;
	for (uint32_t o = 0; o < m->image_size; o += 4)
		m->sum += model_u32(m, image + o);
	for (offset = dir_offset; offset < hdr_dir_size - 4;
	    offset += record_length) {
		m->status = model_record(m, image, offset, hdr_dir_size - 4,
		    hdr_dir_size, &record_length);
		if (m->status != FSIMG_OK)
			return;
	}
	if (memcmp(image + hdr_dir_size - 4, "\0\0\0\0", 4) != 0) {
		m->status = FSIMG_ECORRUPT;
		return;
	}

	/* Node 0 is the root. Every record in table order takes the node at
	 * its path, creating missing parents; a node that already had a
	 * record shadows it.
	 */
	m->nodes = calloc(1, sizeof(*m->nodes));
	CHECK(m->nodes != NULL);
	m->nodes[0].record = -1;
	m->node_count = 1;
	for (size_t i = 0; i < m->record_count; i++) {
		const model_record_t *rec = &m->records[i];
		size_t node = 0, start = 0;

		if (rec->path_length == 0 && rec->kind != GEN_DIR) {
			m->status = FSIMG_EUNSUPPORTED;
			return;
		}
		for (size_t j = 0; rec->path_length != 0 &&
		    j <= rec->path_length; j++) {
			if (j < rec->path_length && rec->path[j] != '/')
				continue;
			node = model_child(m, node, rec->path + start,
			    j - start);
			start = j + 1;
		}
		if (m->nodes[node].record >= 0) {
			m->records[m->nodes[node].record].shadowed = true;
			m->shadowed++;
		}
		m->nodes[node].record = (long)i;
	}
	if (m->nodes[0].record < 0)
		m->implicit++;
	else
		model_match(m, m->nodes[0].record, false);
	model_list(m, 0, "", false);

	/* A reference counts once however many shadowed records carry it. */
	for (unsigned r = 0; r < 5; r++) {
		for (size_t i = 0; m->references[r] != 0 &&
		    i < m->record_count; i++) {
			if (m->records[i].shadowed &&
			    m->records[i].inode == m->references[r]) {
				m->shadowed_references++;
				break;
			}
		}
	}
}

/* What full verification of an image that opens must return: an invalid
 * mountpoint is out of scope, a volume longer than image_size and a
 * nonzero word sum are corrupt, and each nonzero reference must name
 * exactly one node, a regular file.
 */
static fsimg_status_t
model_verify(const model_t *m)
{
	if (m->status != FSIMG_OK)
		return (m->status);
	if (!m->mountpoint_valid)
		return (FSIMG_EUNSUPPORTED);
	if (m->length != m->image_size || m->sum != 0)
		return (FSIMG_ECORRUPT);
	for (unsigned r = 0; r < 5; r++) {
		if (m->references[r] == 0)
			continue;
		if (m->reference_matches[r] != 1)
			return (FSIMG_ECORRUPT);
		if (m->reference_kind[r] != GEN_FILE)
			return (FSIMG_EUNSUPPORTED);
	}
	return (FSIMG_OK);
}

/*
 * Reading through the library. A fixture is an image behind the callback
 * adapter, in a context with a 16 MiB budget and a private scratch
 * directory, with read accounting and fault injection; fixture_close
 * checks that nothing outlived the reader.
 */

/* The regions of an image a scale case counts reads by, so that the bytes
 * read from each give the passes over it.
 */
typedef enum region_kind {
	REGION_HEADER,		/* [0, dir_offset): the header and mountpoint */
	REGION_TABLE,		/* [dir_offset, hdr_dir_size) */
	REGION_DATA,		/* [hdr_dir_size, image_size - 4) */
	REGION_TRAILER,
	REGION_KINDS
} region_kind_t;

/* What one operation read: image calls and bytes in all, and bytes by
 * region. The first read is the format registry's probe of the image
 * prefix every registered probe may inspect; it is counted apart, as no
 * pass over ImageFS metadata.
 */
typedef struct read_account {
	uint64_t	 table_start;	/* dir_offset */
	uint64_t	 data_start;	/* hdr_dir_size */
	uint64_t	 trailer_start;	/* image_size - 4 */
	uint64_t	 calls;
	uint64_t	 bytes;
	uint64_t	 probe_bytes;
	uint64_t	 region_bytes[REGION_KINDS];
} read_account_t;

typedef struct scale_image scale_image_t;

/* The image is image's first length bytes, or synthesized from scale. A
 * read touching [fail_first, fail_end) fails; one starting in [eof_at,
 * eof_end) makes no progress, and one crossing eof_at stops there; no read
 * returns more than short_max bytes when it is nonzero. Counting read
 * calls from 1, call fail_call fails and call stall_call makes no
 * progress, either recording its offset in injected_offset. The context's
 * cancellation callback counts polls and cancels from cancel_after on
 * when that is nonzero. Zeroed fields inject nothing.
 */
typedef struct fixture {
	fsimg_ctx_t	*ctx;
	fsimg_io_t	*io;
	fsimg_reader_t	*reader;
	const unsigned char *image;
	const scale_image_t *scale;
	uint64_t	 length;
	uint64_t	 memory;	/* charged before any I/O object */
	uint64_t	 fail_first;
	uint64_t	 fail_end;
	uint64_t	 eof_at;
	uint64_t	 eof_end;
	size_t		 short_max;
	uint64_t	 calls;
	uint64_t	 fail_call;
	uint64_t	 stall_call;
	uint64_t	 injected_offset;
	uint64_t	 polls;
	uint64_t	 cancel_after;
	read_account_t	*account;	/* nonnull: count every read */
	int		 scratch;
	char		 directory[256];
} fixture_t;

/* How a fixture's context and image differ from the defaults; a zero field
 * keeps its default.
 */
typedef struct fixture_spec {
	const scale_image_t *scale;	/* nonnull: synthesize the image */
	uint64_t	 scratch_bytes;	/* the context's scratch budget */
	uint64_t	 max_work;	/* the session's work limit */
	uint32_t	 cache_pages;	/* a smaller metadata cache, to spill */
	uint32_t	 max_depth;	/* the directory depth limit */
} fixture_spec_t;

static void scale_fill(const scale_image_t *, uint64_t, unsigned char *,
    size_t);

/* One entry the reader delivered, with its content read completely. */
typedef struct seen {
	char		*path;
	fsimg_entry_kind_t kind;
	uint64_t	 size;
	uint32_t	 valid;
	uint32_t	 mode;
	uint32_t	 uid;
	uint32_t	 gid;
	int64_t		 mtime;
	unsigned char	*content;
} seen_t;

/* status is the first failure; opened tells whether it came after open
 * succeeded.
 */
typedef struct listing {
	seen_t		*entries;
	size_t		 count;
	fsimg_volume_info_t info;
	fsimg_error_t	 error;
	fsimg_status_t	 status;
	bool		 opened;
} listing_t;

/* Count one read: the first apart, as the probe's, the rest by region. */
static void
account_read(read_account_t *a, uint64_t offset, size_t length)
{
	const uint64_t starts[REGION_KINDS] = {
		0, a->table_start, a->data_start, a->trailer_start
	};
	uint64_t end = offset + length;

	a->calls++;
	a->bytes += length;
	if (a->calls == 1) {
		CHECK(offset == 0);
		a->probe_bytes = length;
		return;
	}
	for (unsigned r = 0; r < REGION_KINDS; r++) {
		uint64_t first = starts[r];
		uint64_t last = r + 1 < REGION_KINDS ? starts[r + 1] : UINT64_MAX;

		first = first > offset ? first : offset;
		last = last < end ? last : end;
		if (first < last)
			a->region_bytes[r] += last - first;
	}
}

static fsimg_status_t
image_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error)
{
	fixture_t *f = argument;
	bool eof = f->eof_end > f->eof_at;

	CHECK(offset <= f->length && length <= f->length - offset);
	*transferred = 0;
	f->calls++;
	if (f->calls == f->fail_call || f->calls == f->stall_call)
		f->injected_offset = offset;
	if (f->calls == f->fail_call)
		return (fsimg_error_set(error, FSIMG_EIO, EIO, FSIMG_OP_IO,
		    "injected ImageFS read failure"));
	if (f->calls == f->stall_call)
		return (FSIMG_OK);
	if (offset < f->fail_end && offset + length > f->fail_first)
		return (fsimg_error_set(error, FSIMG_EIO, EIO, FSIMG_OP_IO,
		    "injected ImageFS read failure"));
	if (eof && offset >= f->eof_at && offset < f->eof_end)
		return (FSIMG_OK);
	if (eof && offset < f->eof_at && length > f->eof_at - offset)
		length = (size_t)(f->eof_at - offset);
	if (f->short_max != 0 && length > f->short_max)
		length = f->short_max;
	if (f->account != NULL)
		account_read(f->account, offset, length);
	if (f->scale != NULL)
		scale_fill(f->scale, offset, buffer, length);
	else
		memcpy(buffer, f->image + offset, length);
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

/* A fixture over the first length bytes of image, or over the synthetic
 * image spec names, as spec describes.
 */
static void
fixture_new(fixture_t *f, const unsigned char *image, uint64_t length,
    const fixture_spec_t *spec)
{
	fsimg_options_t options;
	fsimg_io_ops_t ops;
	const char *tmp = getenv("TMPDIR");

	bzero(f, sizeof(*f));
	f->image = image;
	f->scale = spec->scale;
	f->length = length;
	CHECK((size_t)snprintf(f->directory, sizeof(f->directory),
	    "%s/fsimg-ifs-XXXXXX", tmp != NULL && tmp[0] != '\0' ? tmp :
	    "/tmp") < sizeof(f->directory));
	CHECK(mkdtemp(f->directory) != NULL);
	f->scratch = open(f->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(f->scratch != -1);
	fsimg_options_init(&options);
	options.memory_bytes = 16 * 1024 * 1024;
	options.scratch_directory_fd = f->scratch;
	if (spec->scratch_bytes != 0)
		options.scratch_bytes = spec->scratch_bytes;
	if (spec->max_work != 0)
		options.max_work = spec->max_work;
	if (spec->max_depth != 0)
		options.max_depth = spec->max_depth;
	options.cancelled = cancel_poll;
	options.cancel_argument = f;
	CHECK(fsimg_ctx_new(&options, &f->ctx, NULL) == FSIMG_OK);
	if (spec->cache_pages != 0)
		CHECK(fsimg_test_meta_cache(f->ctx, spec->cache_pages, NULL) ==
		    FSIMG_OK);
	f->memory = charged_memory(f->ctx);
	fsimg_io_ops_init(&ops);
	ops.read_at = image_read;
	ops.get_size = image_size;
	CHECK(fsimg_io_from_callbacks(f->ctx, &ops, f, &f->io, NULL) ==
	    FSIMG_OK);
}

/* A fixture over the first length bytes of image; max_depth, when nonzero,
 * replaces the default depth limit.
 */
static void
fixture_open(fixture_t *f, const unsigned char *image, uint64_t length,
    uint32_t max_depth)
{
	fixture_spec_t spec = { .max_depth = max_depth };

	fixture_new(f, image, length, &spec);
}

/* Free the fixture and check that the reader released everything. */
static void
fixture_close(fixture_t *f)
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

static void
listing_free(listing_t *l)
{
	for (size_t i = 0; i < l->count; i++) {
		free(l->entries[i].path);
		free(l->entries[i].content);
	}
	free(l->entries);
	bzero(l, sizeof(*l));
}

/* Read the current entry's content completely, in reads of at most 4096
 * bytes; a read never returns more than the bytes left.
 */
static fsimg_status_t
read_content(fixture_t *f, seen_t *s, fsimg_error_t *error)
{
	uint64_t offset;
	size_t count;
	fsimg_status_t status;

	s->content = xmalloc((size_t)s->size);
	for (offset = 0; offset < s->size; offset += count) {
		size_t want = s->size - offset < 4096 ?
		    (size_t)(s->size - offset) : 4096;

		status = fsimg_reader_read_at(f->reader, offset,
		    s->content + offset, want, &count, error);
		if (status != FSIMG_OK)
			return (status);
		CHECK(count != 0 && count <= want);
	}
	return (FSIMG_OK);
}

/* List the fixture's open reader, reading every file and link; l->status
 * is the first failure. The reader is freed when the listing completes.
 */
static void
listing_walk(fixture_t *f, listing_t *l)
{
	const fsimg_entry_t *entry;
	fsimg_status_t status;

	l->opened = true;
	CHECK(fsimg_reader_get_info(f->reader, &l->info, NULL) == FSIMG_OK);
	for (;;) {
		seen_t *s;

		status = fsimg_reader_next(f->reader, &entry, &l->error);
		if (status == FSIMG_DONE)
			break;
		if (status != FSIMG_OK) {
			l->status = status;
			return;
		}
		l->entries = realloc(l->entries, (l->count + 1) *
		    sizeof(*l->entries));
		CHECK(l->entries != NULL);
		s = &l->entries[l->count++];
		bzero(s, sizeof(*s));
		s->path = (char *)copy_bytes(entry->path, strlen(entry->path));
		s->kind = entry->kind;
		s->size = entry->size;
		s->valid = entry->metadata.valid;
		s->mode = entry->metadata.mode;
		s->uid = entry->metadata.uid;
		s->gid = entry->metadata.gid;
		s->mtime = entry->metadata.modified.seconds;
		CHECK(entry->metadata.modified.basis == FSIMG_TIME_UTC);
		if (entry->kind != FSIMG_ENTRY_FILE &&
		    entry->kind != FSIMG_ENTRY_SYMLINK)
			continue;
		status = read_content(f, s, &l->error);
		if (status != FSIMG_OK) {
			l->status = status;
			return;
		}
	}
	fsimg_reader_free(f->reader);
	f->reader = NULL;
}

/* Open the fixture's image (forced to ifs when forced is set) and list it,
 * reading every file and link. l->status is the first failure.
 */
static void
fixture_list(fixture_t *f, bool forced, listing_t *l)
{
	fsimg_read_options_t options;

	bzero(l, sizeof(*l));
	fsimg_read_options_init(&options);
	if (forced)
		options.format = "ifs";
	l->status = fsimg_reader_open(f->ctx, f->io, &options, &f->reader,
	    &l->error);
	if (l->status == FSIMG_OK)
		listing_walk(f, l);
}

/* List the first length bytes of image in a fresh fixture. */
static void
list_image(const unsigned char *image, size_t length, uint32_t max_depth,
    listing_t *l)
{
	fixture_t f;

	fixture_open(&f, image, length, max_depth);
	fixture_list(&f, false, l);
	fixture_close(&f);
}

static uint64_t
detail(const fsimg_volume_info_t *info, const char *name)
{
	for (uint32_t i = 0; i < info->detail_count; i++)
		if (strcmp(info->details[i].name, name) == 0)
			return (info->details[i].value);
	fprintf(stderr, "no detail %s\n", name);
	CHECK(false);
	return (0);
}

static fsimg_entry_kind_t
public_kind(gen_kind_t kind)
{
	switch (kind) {
	case GEN_DIR: return (FSIMG_ENTRY_DIRECTORY);
	case GEN_FILE: return (FSIMG_ENTRY_FILE);
	case GEN_LINK: return (FSIMG_ENTRY_SYMLINK);
	case GEN_FIFO: return (FSIMG_ENTRY_FIFO);
	}
	return (0);
}

/* Compare a listing with the model of the same image: the status, every
 * delivered entry with its metadata and content, and every info value the
 * model predicts. Open validates every record and the whole hierarchy, so
 * every failure the model predicts must fail open itself.
 */
static bool
listing_matches(const listing_t *l, const model_t *m, const char *label)
{
	if (l->status != m->status) {
		fprintf(stderr, "%s: reader %d (%s), model %d\n", label,
		    l->status, l->error.message, m->status);
		return (false);
	}
	if (m->status != FSIMG_OK && l->opened) {
		fprintf(stderr, "%s: failed after open (%s)\n", label,
		    l->error.message);
		return (false);
	}
	if (m->status != FSIMG_OK)
		return (true);
	if (l->count != m->entry_count) {
		fprintf(stderr, "%s: %zu entries, model %zu\n", label,
		    l->count, m->entry_count);
		return (false);
	}
	for (size_t i = 0; i < l->count; i++) {
		const seen_t *s = &l->entries[i];
		const model_entry_t *e = &m->entries[i];
		uint64_t size = e->kind == GEN_FILE || e->kind == GEN_LINK ?
		    e->content_length : 0;

		if (strcmp(s->path, e->path) != 0 ||
		    s->kind != public_kind(e->kind) || s->size != size ||
		    s->valid != (FSIMG_META_MODIFIED | FSIMG_META_OWNER |
		    FSIMG_META_MODE) || s->mode != e->permissions ||
		    s->uid != e->uid || s->gid != e->gid ||
		    s->mtime != (int64_t)e->mtime ||
		    (size != 0 && memcmp(s->content, e->content, size) != 0)) {
			fprintf(stderr, "%s: entry %zu differs from the model\n",
			    label, i);
			return (false);
		}
	}
	if (strcmp(l->info.format, "ifs") != 0 ||
	    l->info.volume_bytes != m->image_size ||
	    l->info.sector_bytes != 1 || l->info.allocation_unit_bytes != 1 ||
	    ((l->info.valid & FSIMG_VOLUME_MOUNTPOINT) != 0) !=
	    m->mountpoint_valid ||
	    (m->mountpoint_valid &&
	    strcmp(l->info.mountpoint, m->mountpoint) != 0) ||
	    detail(&l->info, "flags_0") != m->by_flag[0] ||
	    detail(&l->info, "flags_8") != m->by_flag[1] ||
	    detail(&l->info, "flags_c") != m->by_flag[2] ||
	    detail(&l->info, "flags_e") != m->by_flag[3] ||
	    detail(&l->info, "implicit_directories") != m->implicit ||
	    detail(&l->info, "shadowed") != m->shadowed ||
	    detail(&l->info, "omitted") != m->omitted ||
	    detail(&l->info, "omitted_references") != m->omitted_references ||
	    detail(&l->info, "shadowed_references") !=
	    m->shadowed_references ||
	    detail(&l->info, "mount_flags") != m->mount_flags ||
	    detail(&l->info, "chain_paddr") != m->chain ||
	    detail(&l->info, "boot_ino_0") != m->references[0] ||
	    detail(&l->info, "boot_ino_1") != m->references[1] ||
	    detail(&l->info, "boot_ino_2") != m->references[2] ||
	    detail(&l->info, "boot_ino_3") != m->references[3] ||
	    detail(&l->info, "script_ino") != m->references[4]) {
		fprintf(stderr, "%s: volume information differs from the "
		    "model\n", label);
		return (false);
	}
	return (true);
}

/* Open the first length bytes of image and verify them with flags; the
 * status of open is returned when it fails. A failed report must be
 * incomplete and count no allocation, and only a completed verification
 * makes info report the free bytes, which are zero.
 */
static fsimg_status_t
verify_slice(const unsigned char *image, size_t length, unsigned flags,
    fsimg_verify_report_t *report, fsimg_error_t *error)
{
	fsimg_volume_info_t info;
	fsimg_status_t status;
	fixture_t f;

	bzero(report, sizeof(*report));
	fixture_open(&f, image, length, 600);
	status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, error);
	if (status == FSIMG_OK) {
		CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
		CHECK((info.valid & FSIMG_VOLUME_FREE) == 0);
		CHECK(info.capabilities == (FSIMG_CAP_READ | FSIMG_CAP_VERIFY));
		status = fsimg_verify(f.reader, flags, report, error);
		CHECK(report->complete == (status == FSIMG_OK));
		/* A failed verification reports no allocation and leaves no
		 * volume information to read.
		 */
		CHECK(status == FSIMG_OK || (report->allocated_clusters == 0 &&
		    fsimg_reader_get_info(f.reader, &info, NULL) ==
		    FSIMG_ESTATE));
	}
	if (status == FSIMG_OK) {
		CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
		CHECK((info.valid & FSIMG_VOLUME_FREE) != 0 &&
		    info.free_bytes == 0);
	}
	fixture_close(&f);
	return (status);
}

/* Verify an image that opens, structurally and with payload reads, and
 * compare the status with the model's and each report with the model's
 * counts: the delivered entries, the root included, and the payload of
 * every walked file and link, omitted ones included.
 */
static bool
verify_matches(const unsigned char *image, size_t length, const model_t *m,
    const char *label)
{
	static const unsigned scopes[] = { 0, FSIMG_VERIFY_PAYLOAD };
	fsimg_status_t expected = model_verify(m);

	for (size_t i = 0; i < sizeof(scopes) / sizeof(scopes[0]); i++) {
		fsimg_verify_report_t report;
		fsimg_error_t error;
		fsimg_status_t status;
		uint64_t files = 0, directories = 1, bytes = 0;

		bzero(&error, sizeof(error));
		status = verify_slice(image, length, scopes[i], &report, &error);
		if (status != expected) {
			fprintf(stderr, "%s: verify %d (%s), model %d\n", label,
			    status, error.message, expected);
			return (false);
		}
		if (status != FSIMG_OK)
			continue;
		for (size_t e = 0; e < m->entry_count; e++) {
			if (m->entries[e].kind == GEN_DIR) {
				directories++;
				continue;
			}
			files++;
			if (m->entries[e].kind != GEN_FIFO)
				bytes += m->entries[e].content_length;
		}
		if (report.scope != (scopes[i] == 0 ?
		    FSIMG_VALIDATION_STRUCTURAL : FSIMG_VALIDATION_PAYLOAD) ||
		    report.files != files || report.directories != directories ||
		    report.logical_bytes != bytes ||
		    report.payload_bytes_read != (scopes[i] == 0 ? 0 :
		    m->walked_bytes) ||
		    report.allocated_clusters != m->image_size ||
		    report.free_clusters != 0 || report.bad_clusters != 0 ||
		    report.omitted_entries != m->omitted) {
			fprintf(stderr, "%s: verification report differs from "
			    "the model\n", label);
			return (false);
		}
	}
	return (true);
}

/* Read the first length bytes of image and require status, and agreement
 * with the model, which must predict the same status. An image that opens
 * is also verified against the model.
 */
static void
expect_slice(const unsigned char *image, size_t length, fsimg_status_t status,
    const char *label)
{
	listing_t l;
	model_t m;

	model_build(&m, image, length);
	list_image(image, length, 0, &l);
	if (l.status != status || m.status != status)
		fprintf(stderr, "%s: expected %d, reader %d (%s), model %d\n",
		    label, status, l.status, l.error.message, m.status);
	CHECK(l.status == status && m.status == status);
	CHECK(listing_matches(&l, &m, label));
	if (m.status == FSIMG_OK)
		CHECK(verify_matches(image, length, &m, label));
	listing_free(&l);
	model_free(&m);
}

static void
expect(gen_t *g, fsimg_status_t status, const char *label)
{
	expect_slice(g->image, g->length, status, label);
}

/* Require an image that opens to verify with status. */
static void
expect_verify(gen_t *g, fsimg_status_t status, const char *label)
{
	model_t m;

	expect(g, FSIMG_OK, label);
	model_build(&m, g->image, g->length);
	if (model_verify(&m) != status)
		fprintf(stderr, "%s: the model verifies %d, expected %d\n",
		    label, model_verify(&m), status);
	CHECK(model_verify(&m) == status);
	model_free(&m);
}

/* Require the image to list exactly the paths in order, NULL-terminated,
 * and agree with the model.
 */
static void
expect_paths(gen_t *g, const char *const *paths, const char *label)
{
	listing_t l;
	size_t i;

	expect(g, FSIMG_OK, label);
	list_image(g->image, g->length, 0, &l);
	for (i = 0; paths[i] != NULL; i++) {
		if (i >= l.count || strcmp(l.entries[i].path, paths[i]) != 0)
			fprintf(stderr, "%s: entry %zu is %s, expected %s\n",
			    label, i, i < l.count ? l.entries[i].path : "none",
			    paths[i]);
		CHECK(i < l.count && strcmp(l.entries[i].path, paths[i]) == 0);
	}
	CHECK(i == l.count);
	listing_free(&l);
}

/* The info of a well-formed image. */
static void
image_info(gen_t *g, fsimg_volume_info_t *info)
{
	listing_t l;

	list_image(g->image, g->length, 0, &l);
	CHECK(l.status == FSIMG_OK);
	*info = l.info;
	listing_free(&l);
}

/* The entry at path in a listing, which must exist. */
static const seen_t *
listing_find(const listing_t *l, const char *path)
{
	for (size_t i = 0; i < l->count; i++)
		if (strcmp(l->entries[i].path, path) == 0)
			return (&l->entries[i]);
	fprintf(stderr, "no entry %s\n", path);
	CHECK(false);
	return (NULL);
}

/* A small image of every kind under an explicit root, as mkifs writes. */
static void
sample(gen_t *g)
{
	gen_init(g);
	gen_root(g);
	gen_file(g, "a.txt", "hello");
	gen_dir(g, "dir");
	gen_file(g, "dir/inner", "inner data");
	gen_link(g, "dir/up", "../a.txt");
	gen_fifo(g, "pipe");
	gen_file(g, "deep/implicit/file", "x");
	gen_finish(g);
}

/*
 * The sample and the volume information.
 */

static void
test_sample(void)
{
	static const char *const paths[] = {
		"/a.txt", "/deep", "/deep/implicit", "/deep/implicit/file",
		"/dir", "/dir/inner", "/dir/up", "/pipe", NULL
	};
	static const char *const details[] = {
		"mount_flags", "boot_ino_0", "boot_ino_1", "boot_ino_2",
		"boot_ino_3", "script_ino", "chain_paddr", "flags_0", "flags_8",
		"flags_c", "flags_e", "implicit_directories", "shadowed",
		"omitted", "omitted_references", "shadowed_references"
	};
	gen_t g;
	listing_t l;
	const seen_t *s;

	sample(&g);
	expect_paths(&g, paths, "the sample");
	list_image(g.image, g.length, 0, &l);
	CHECK(strcmp(l.info.format, "ifs") == 0);
	CHECK(strcmp(l.info.profile, "little-endian") == 0);
	CHECK(l.info.valid == (FSIMG_VOLUME_GEOMETRY | FSIMG_VOLUME_MOUNTPOINT));
	CHECK(strcmp(l.info.mountpoint, "/") == 0);
	CHECK(l.info.capabilities == (FSIMG_CAP_READ | FSIMG_CAP_VERIFY));
	CHECK(l.info.volume_bytes == g.image_size);
	CHECK(l.info.allocation_units == g.image_size);
	CHECK(l.info.time_basis == FSIMG_TIME_UTC);
	CHECK(l.info.scope == FSIMG_VALIDATION_HEADER);
	CHECK(l.info.label[0] == '\0');
	/* Sixteen details, the table full, in the documented order. */
	CHECK(l.info.detail_count == sizeof(details) / sizeof(details[0]));
	for (size_t i = 0; i < l.info.detail_count; i++)
		CHECK(strcmp(l.info.details[i].name, details[i]) == 0);
	for (size_t i = l.info.detail_count; i < FSIMG_VOLUME_DETAILS_MAX; i++)
		CHECK(l.info.details[i].name[0] == '\0' &&
		    l.info.details[i].value == 0);
	CHECK(detail(&l.info, "flags_0") == 7);
	CHECK(detail(&l.info, "implicit_directories") == 2);
	CHECK(detail(&l.info, "shadowed") == 0);

	s = listing_find(&l, "/a.txt");
	CHECK(s->kind == FSIMG_ENTRY_FILE && s->size == 5 &&
	    memcmp(s->content, "hello", 5) == 0 && s->mode == 0644 &&
	    s->mtime == GEN_MTIME);
	s = listing_find(&l, "/dir/up");
	CHECK(s->kind == FSIMG_ENTRY_SYMLINK && s->size == 8 &&
	    memcmp(s->content, "../a.txt", 8) == 0 && s->mode == 0777);
	s = listing_find(&l, "/pipe");
	CHECK(s->kind == FSIMG_ENTRY_FIFO && s->size == 0);
	/* An implicit directory has mkifs's directory values. */
	s = listing_find(&l, "/deep/implicit");
	CHECK(s->kind == FSIMG_ENTRY_DIRECTORY && s->mode == 0755 &&
	    s->uid == 0 && s->gid == 0 && s->mtime == 0);
	listing_free(&l);
	gen_free(&g);

	/* The big-endian twin lists the same tree. */
	gen_init(&g);
	g.big = true;
	gen_root(&g);
	gen_file(&g, "a.txt", "hello");
	gen_dir(&g, "dir");
	gen_file(&g, "dir/inner", "inner data");
	gen_link(&g, "dir/up", "../a.txt");
	gen_fifo(&g, "pipe");
	gen_file(&g, "deep/implicit/file", "x");
	gen_finish(&g);
	expect_paths(&g, paths, "the big-endian sample");
	list_image(g.image, g.length, 0, &l);
	CHECK(strcmp(l.info.profile, "big-endian") == 0);
	listing_free(&l);
	gen_free(&g);
}

/*
 * Probe and header.
 */

typedef struct memory_sink {
	unsigned char	*bytes;
	uint64_t	 length;
} memory_sink_t;

static fsimg_status_t
sink_size(void *argument, uint64_t *size, fsimg_error_t *error FSIMG_UNUSED)
{
	*size = ((memory_sink_t *)argument)->length;
	return (FSIMG_OK);
}

static fsimg_status_t
sink_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_sink_t *sink = argument;

	CHECK(offset <= sink->length && length <= sink->length - offset);
	memcpy(buffer, sink->bytes + offset, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
sink_write(void *argument, uint64_t offset, const void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_sink_t *sink = argument;

	CHECK(offset <= sink->length && length <= sink->length - offset);
	memcpy(sink->bytes + offset, buffer, length);
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
	CHECK(fsimg_writer_new(ctx, format, &options, &writer, NULL) ==
	    FSIMG_OK);
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

/* The format the whole registry claims for an image, or NULL. */
static const fsimg_format_t *
claim(const unsigned char *image, uint64_t length, fsimg_status_t *status)
{
	const fsimg_format_t *const registry[] = {
		&fsimg_fat_format, &fsimg_exfat_format, &fsimg_qnx6_format,
		&fsimg_qnx4_format, &fsimg_ifs_format
	};
	const size_t count = sizeof(registry) / sizeof(registry[0]);
	const fsimg_format_t *claimed;
	size_t prefix;

	prefix = 0;
	for (size_t i = 0; i < count; i++)
		if (registry[i]->probe_bytes > prefix)
			prefix = registry[i]->probe_bytes;
	if (prefix > length)
		prefix = (size_t)length;
	*status = fsimg_format_claim(registry, count, NULL, image, prefix,
	    length, &claimed, NULL);
	return (claimed);
}

static fsimg_status_t
open_forced(const unsigned char *image, uint64_t length, const char *format)
{
	fixture_t f;
	fsimg_read_options_t options;
	fsimg_status_t status;

	fixture_open(&f, image, length, 0);
	fsimg_read_options_init(&options);
	options.format = format;
	status = fsimg_reader_open(f.ctx, f.io, &options, &f.reader, NULL);
	fixture_close(&f);
	return (status);
}

static fsimg_status_t
probe(const unsigned char *prefix, size_t length, uint64_t volume_bytes)
{
	const fsimg_format_variant_t *variant = NULL;
	fsimg_error_t error;
	fsimg_status_t status;

	status = fsimg_ifs_format.probe(prefix, length, volume_bytes, &variant,
	    &error);
	/* A claim names the format's one variant. */
	CHECK(status != FSIMG_OK || variant == fsimg_ifs_format.variants);
	return (status);
}

/* A little-endian header with the three sizes and native flags. */
static fsimg_status_t
probe_sizes(uint32_t dir_offset, uint32_t hdr_dir_size, uint32_t image_size,
    uint64_t volume_bytes)
{
	unsigned char header[GEN_HEADER];
	gen_t g;

	gen_init(&g);
	bzero(header, sizeof(header));
	memcpy(header, "imagefs", 7);
	header[H_FLAGS] = 0x04;
	put32(&g, header + H_DIR_OFFSET, dir_offset);
	put32(&g, header + H_HDR_DIR_SIZE, hdr_dir_size);
	put32(&g, header + H_IMAGE_SIZE, image_size);
	return (probe(header, sizeof(header), volume_bytes));
}

static void
test_probe(void)
{
	static const char *const others[] = { "fat12", "exfat", "qnx6", "qnx4" };
	const fsimg_format_t *const formats[] = { &fsimg_fat_format,
	    &fsimg_exfat_format, &fsimg_qnx6_format, &fsimg_qnx4_format };
	memory_sink_t sink;
	fsimg_status_t status;
	unsigned char *copy;
	gen_t g;
	size_t data;

	/* Every other format's volume is claimed by its own probe alone,
	 * and the forced ImageFS decoder finds no signature in it.
	 */
	for (size_t i = 0; i < 4; i++) {
		make_volume(others[i], &sink);
		CHECK(claim(sink.bytes, sink.length, &status) == formats[i]);
		CHECK(status == FSIMG_OK);
		CHECK(probe(sink.bytes, GEN_HEADER, sink.length) ==
		    FSIMG_EFORMAT);
		CHECK(open_forced(sink.bytes, sink.length, "ifs") ==
		    FSIMG_EFORMAT);
		free(sink.bytes);
	}

	/* An ImageFS is claimed by ifs alone, and every forced decoder of
	 * another format refuses it.
	 */
	sample(&g);
	CHECK(claim(g.image, g.length, &status) == &fsimg_ifs_format &&
	    status == FSIMG_OK);
	for (size_t i = 0; i < 4; i++)
		CHECK(open_forced(g.image, g.length, others[i]) ==
		    FSIMG_EFORMAT);
	CHECK(open_forced(g.image, g.length, "ifs") == FSIMG_OK);

	/* One changed signature byte is no claim. */
	copy = xmalloc(g.length);
	for (size_t i = 0; i < 7; i++) {
		memcpy(copy, g.image, g.length);
		copy[i] ^= 0x20;
		CHECK(probe(copy, GEN_HEADER, g.length) == FSIMG_EFORMAT);
		CHECK(claim(copy, g.length, &status) == NULL &&
		    status == FSIMG_EFORMAT);
	}
	/* A prefix shorter than the header is no claim. */
	CHECK(probe(g.image, GEN_HEADER - 1, g.length) == FSIMG_EFORMAT);
	CHECK(open_forced(g.image, GEN_HEADER - 1, "ifs") == FSIMG_EFORMAT);
	CHECK(probe(g.image, GEN_HEADER, g.length) == FSIMG_OK);

	/* Only the two native flags bytes are read. */
	for (unsigned flags = 0; flags <= 0xff; flags++) {
		if (flags > 7 && flags != 0xff)
			continue;
		memcpy(copy, g.image, g.length);
		copy[H_FLAGS] = (unsigned char)flags;
		if (flags == 0x04)
			CHECK(probe(copy, GEN_HEADER, g.length) == FSIMG_OK);
		else if (flags == 0x05)
			/* Big-endian sizes of a little-endian image. */
			CHECK(probe(copy, GEN_HEADER, g.length) ==
			    FSIMG_ECORRUPT);
		else
			expect_slice(copy, g.length, FSIMG_EUNSUPPORTED,
			    "an unknown flags byte");
	}
	free(copy);
	gen_free(&g);

	/* File data holding a QNX4 root block at byte 512 draws a second
	 * claim, which only a forced format resolves.
	 */
	gen_init(&g);
	gen_root(&g);
	data = gen_add_bytes(&g, GEN_FILE, "carrier", 7, NULL, 0);
	free(g.members[data].bytes);
	g.members[data].bytes = calloc(1, 2048);
	CHECK(g.members[data].bytes != NULL);
	g.members[data].bytes_length = 2048;
	gen_finish(&g);
	CHECK(g.members[data].data_offset < 512);
	{
		unsigned char *block = g.image + 512;

		bzero(block, 512);
		block[0] = '/';
		block[0x14] = 3;	/* first extent block */
		block[0x18] = 1;	/* its length */
		block[0x32] = 0xed;	/* mode 040755, little-endian */
		block[0x33] = 0x41;
		block[0x3f] = 0x01;	/* used */
		for (unsigned r = 1; r < 5; r++)
			block[64 * r + 0x3f] = 0x11;
	}
	CHECK(claim(g.image, g.length, &status) == NULL &&
	    status == FSIMG_EFORMAT);
	CHECK(open_forced(g.image, g.length, "ifs") == FSIMG_OK);
	CHECK(open_forced(g.image, g.length, "qnx4") != FSIMG_EFORMAT);
	gen_free(&g);
}

/* Each header bound at x - 1, x and x + 1, and the volume length. */
static void
test_header_bounds(void)
{
	gen_t g;
	listing_t l;
	model_t m;

	/* 0x58 <= dir_offset */
	CHECK(probe_sizes(0x57, 0x100, 0x200, 0x200) == FSIMG_ECORRUPT);
	CHECK(probe_sizes(0x58, 0x100, 0x200, 0x200) == FSIMG_OK);
	CHECK(probe_sizes(0x59, 0x100, 0x200, 0x200) == FSIMG_OK);
	/* dir_offset + 4 <= hdr_dir_size: the table holds its terminator */
	CHECK(probe_sizes(0x80, 0x83, 0x200, 0x200) == FSIMG_ECORRUPT);
	CHECK(probe_sizes(0x80, 0x84, 0x200, 0x200) == FSIMG_OK);
	CHECK(probe_sizes(0x80, 0x85, 0x200, 0x200) == FSIMG_OK);
	CHECK(probe_sizes(0x80, 0x80, 0x200, 0x200) == FSIMG_ECORRUPT);
	/* hdr_dir_size + 4 <= image_size */
	CHECK(probe_sizes(0x5c, 0x1fb, 0x200, 0x200) == FSIMG_OK);
	CHECK(probe_sizes(0x5c, 0x1fc, 0x200, 0x200) == FSIMG_OK);
	CHECK(probe_sizes(0x5c, 0x1fd, 0x200, 0x200) == FSIMG_ECORRUPT);
	/* image_size is whole words: 0, 4, and 1, 2 and 3 modulo 4 */
	CHECK(probe_sizes(0x5c, 0x60, 0, 0x300) == FSIMG_ECORRUPT);
	CHECK(probe_sizes(0x5c, 0x60, 4, 0x300) == FSIMG_ECORRUPT);
	for (uint32_t r = 1; r < 4; r++)
		CHECK(probe_sizes(0x5c, 0x60, 0x200 + r, 0x300) ==
		    FSIMG_ECORRUPT);
	/* image_size <= the volume */
	CHECK(probe_sizes(0x5c, 0x100, 0x200, 0x1ff) == FSIMG_ECORRUPT);
	CHECK(probe_sizes(0x5c, 0x100, 0x200, 0x200) == FSIMG_OK);
	CHECK(probe_sizes(0x5c, 0x100, 0x200, 0x201) == FSIMG_OK);
	/* The largest sizes do not wrap. */
	CHECK(probe_sizes(0x5c, UINT32_MAX - 3, UINT32_MAX - 3,
	    UINT64_C(1) << 33) == FSIMG_ECORRUPT);
	CHECK(probe_sizes(0x5c, UINT32_MAX - 7, UINT32_MAX - 3,
	    UINT64_C(1) << 33) == FSIMG_OK);

	/* A slice shorter than image_size is refused; a longer one reads,
	 * and its tail, poison bytes, is never delivered or counted.
	 */
	sample(&g);
	expect_slice(g.image, g.length - 1, FSIMG_ECORRUPT,
	    "a slice one byte short");
	gen_free(&g);
	for (size_t extra = 1; extra <= 512; extra += 511) {
		sample(&g);
		gen_finish_extra(&g, extra);
		expect(&g, FSIMG_OK, "a slice longer than the image");
		list_image(g.image, g.length, 0, &l);
		CHECK(l.info.volume_bytes == g.image_size);
		for (size_t i = 0; i < l.count; i++)
			CHECK(l.entries[i].size == 0 ||
			    memchr(l.entries[i].content, GEN_POISON,
			    (size_t)l.entries[i].size) == NULL);
		listing_free(&l);
		gen_free(&g);
	}

	/* Open checks what the probe leaves: alignment and spare words. */
	sample(&g);
	put32(&g, g.image + H_DIR_OFFSET, g.dir_offset + 1);
	expect(&g, FSIMG_EUNSUPPORTED, "an unaligned dir_offset");
	put32(&g, g.image + H_DIR_OFFSET, g.dir_offset);
	put32(&g, g.image + H_HDR_DIR_SIZE, g.hdr_dir_size + 1);
	expect(&g, FSIMG_EUNSUPPORTED, "an unaligned hdr_dir_size");
	put32(&g, g.image + H_HDR_DIR_SIZE, g.hdr_dir_size);
	for (unsigned word = 0; word < 10; word += 9) {
		put32(&g, g.image + H_SPARE + 4 * word, 1);
		expect(&g, FSIMG_EUNSUPPORTED, "a nonzero spare word");
		put32(&g, g.image + H_SPARE + 4 * word, 0);
	}
	expect(&g, FSIMG_OK, "the restored header");
	gen_free(&g);

	/* Mount flags and chain_paddr are reported, never interpreted. */
	sample(&g);
	g.mount_flags = 0x12345678;
	g.chain = 0xfedcba98;
	gen_finish(&g);
	expect(&g, FSIMG_OK, "nonzero mount flags and chain_paddr");
	model_build(&m, g.image, g.length);
	list_image(g.image, g.length, 0, &l);
	CHECK(detail(&l.info, "mount_flags") == 0x12345678);
	CHECK(detail(&l.info, "chain_paddr") == 0xfedcba98);
	CHECK(strcmp(l.info.profile, "little-endian") == 0);
	listing_free(&l);
	model_free(&m);
	gen_free(&g);
}

/* A sample whose mountpoint is length bytes of 'm' after a slash. */
static void
mountpoint_sample(gen_t *g, size_t length)
{
	unsigned char bytes[1100];

	CHECK(length < sizeof(bytes));
	memset(bytes, 'm', length);
	if (length != 0)
		bytes[0] = '/';
	gen_init(g);
	gen_mountpoint(g, bytes, length);
	gen_root(g);
	gen_file(g, "f", "data");
	gen_finish(g);
}

static void
test_mountpoint(void)
{
	static const size_t lengths[] = { 0, 1, 4, 70, 1023, 1024 };
	fsimg_volume_info_t info;
	gen_t g;

	for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
		mountpoint_sample(&g, lengths[i]);
		expect(&g, FSIMG_OK, "a mountpoint");
		image_info(&g, &info);
		CHECK((info.valid & FSIMG_VOLUME_MOUNTPOINT) != 0);
		CHECK(strlen(info.mountpoint) == lengths[i]);
		CHECK(memcmp(info.mountpoint, g.mountpoint, lengths[i]) == 0);
		gen_free(&g);
	}
	mountpoint_sample(&g, 1025);
	expect(&g, FSIMG_EUNSUPPORTED, "a 1025-byte mountpoint");
	gen_free(&g);

	/* "/foo" ends exactly at dir_offset - 1. */
	gen_init(&g);
	gen_mountpoint(&g, "/foo", 4);
	gen_root(&g);
	gen_finish(&g);
	CHECK(g.dir_offset == 0x60 && g.image[0x5c] == '\0');
	expect(&g, FSIMG_OK, "a NUL at dir_offset - 1");
	memset(g.image + 0x5c, 'x', 4);
	expect(&g, FSIMG_ECORRUPT, "no NUL before dir_offset");
	bzero(g.image + 0x5c, 4);
	/* A NUL whose rounding falls short of dir_offset. */
	g.image[0x5a] = '\0';
	expect(&g, FSIMG_EUNSUPPORTED, "a mountpoint shorter than its padding");
	gen_free(&g);

	/* Nonzero padding is never read. */
	gen_init(&g);
	gen_mountpoint(&g, "/", 1);
	g.mountpoint_pad = 0x5a;
	gen_root(&g);
	gen_file(&g, "f", "data");
	gen_finish(&g);
	expect(&g, FSIMG_OK, "nonzero mountpoint padding");
	gen_free(&g);

	/* A mountpoint outside the byte rules is withheld, never fatal. */
	gen_init(&g);
	gen_mountpoint(&g, "/m\xff", 3);
	gen_root(&g);
	gen_file(&g, "f", "data");
	gen_finish(&g);
	expect(&g, FSIMG_OK, "an invalid UTF-8 mountpoint");
	image_info(&g, &info);
	CHECK((info.valid & FSIMG_VOLUME_MOUNTPOINT) == 0);
	CHECK(info.mountpoint[0] == '\0');
	gen_free(&g);
	gen_init(&g);
	gen_mountpoint(&g, "/m\x01", 3);
	gen_root(&g);
	gen_finish(&g);
	image_info(&g, &info);
	CHECK((info.valid & FSIMG_VOLUME_MOUNTPOINT) == 0);
	gen_free(&g);
}

/*
 * Records.
 */

/* A root and one record of kind with path "abc", whose length is its
 * kind's exact minimum.
 */
static void
one_record(gen_t *g, gen_kind_t kind)
{
	gen_init(g);
	gen_root(g);
	switch (kind) {
	case GEN_DIR:
		gen_dir(g, "abc");
		break;
	case GEN_FILE:
		gen_file(g, "abc", "data");
		break;
	case GEN_LINK:
		gen_link(g, "abc", "xyz");
		break;
	case GEN_FIFO:
		gen_fifo(g, "abc");
		break;
	}
	gen_finish(g);
}

static void
test_records(void)
{
	static const uint32_t types[] = {
		0000000, 0020000, 0030000, 0050000, 0060000, 0070000, 0110000,
		0130000, 0140000, 0150000, 0160000, 0170000
	};
	static const uint32_t minimum[] = {
		[GEN_DIR] = 0x18 + 4, [GEN_FILE] = 0x20 + 4,
		[GEN_LINK] = 0x1c + 4 + 4, [GEN_FIFO] = 0x20 + 4
	};
	unsigned char *r;
	gen_t g;

	/* The exact minimum of each kind reads; four bytes less do not. */
	for (gen_kind_t kind = GEN_DIR; kind <= GEN_FIFO; kind++) {
		one_record(&g, kind);
		CHECK(g.members[1].length == minimum[kind]);
		expect(&g, FSIMG_OK, "a record of its kind's minimum length");
		gen_shorten_last(&g, minimum[kind] - 4);
		expect(&g, FSIMG_ECORRUPT, "a record below its kind's minimum");
		gen_free(&g);
	}

	/* Lengths 0, 1 and 23 are below the prefix; 24 holds no path. */
	for (uint32_t length = 0; length <= 24; length++) {
		if (length != 0 && length != 1 && length != 23 && length != 24)
			continue;
		one_record(&g, GEN_FILE);
		put16(&g, gen_record(&g, 1) + R_LENGTH, length);
		expect(&g, FSIMG_ECORRUPT, "a record shorter than its prefix");
		gen_free(&g);
	}
	one_record(&g, GEN_DIR);
	put16(&g, gen_record(&g, 1) + R_LENGTH, 25);
	expect(&g, FSIMG_EUNSUPPORTED, "a record length of 25");
	put16(&g, gen_record(&g, 1) + R_LENGTH, g.members[1].length + 4);
	expect(&g, FSIMG_ECORRUPT, "a record crossing the table end");
	put16(&g, gen_record(&g, 1) + R_LENGTH, g.members[1].length);

	/* The terminator: nonzero, and missing when the table ends early. */
	g.image[g.hdr_dir_size - 2] = 1;
	expect(&g, FSIMG_ECORRUPT, "a nonzero terminator");
	g.image[g.hdr_dir_size - 2] = 0;
	put32(&g, g.image + H_HDR_DIR_SIZE, g.hdr_dir_size - 4);
	expect(&g, FSIMG_ECORRUPT, "a table without room for its terminator");
	put32(&g, g.image + H_HDR_DIR_SIZE, g.hdr_dir_size);
	expect(&g, FSIMG_OK, "the restored table");
	gen_free(&g);

	/* Unobserved prefix values. */
	one_record(&g, GEN_FILE);
	r = gen_record(&g, 1);
	put32(&g, r + R_INODE, 0);
	expect(&g, FSIMG_EUNSUPPORTED, "a zero inode word");
	put32(&g, r + R_INODE, 2);
	put16(&g, r + R_EXTATTR, 4);
	expect(&g, FSIMG_EUNSUPPORTED, "a nonzero extended-attribute offset");
	put16(&g, r + R_EXTATTR, 0);
	for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		put32(&g, r + R_MODE, types[i] | 0644);
		expect(&g, FSIMG_EUNSUPPORTED, "an unknown record type");
	}
	put32(&g, r + R_MODE, 0200000 | 0100644);
	expect(&g, FSIMG_EUNSUPPORTED, "mode bits above 0177777");
	put32(&g, r + R_MODE, 0100644);
	/* A path running to the record's end has no NUL. */
	memset(r + 0x20, 'p', g.members[1].length - 0x20);
	expect(&g, FSIMG_ECORRUPT, "a path without a NUL");
	gen_free(&g);

	/* A FIFO's two words are zero. */
	for (unsigned word = 0; word < 2; word++) {
		one_record(&g, GEN_FIFO);
		put32(&g, gen_record(&g, 1) + R_FIFO_WORDS + 4 * word, 1);
		expect(&g, FSIMG_EUNSUPPORTED, "a nonzero FIFO word");
		gen_free(&g);
	}

	/* An empty table, with the terminator at dir_offset, is the root
	 * alone, synthesized.
	 */
	gen_init(&g);
	gen_finish(&g);
	CHECK(g.hdr_dir_size == g.dir_offset + 4);
	expect(&g, FSIMG_OK, "an empty table");
	{
		fsimg_volume_info_t info;

		image_info(&g, &info);
		CHECK(detail(&info, "implicit_directories") == 1);
		CHECK(detail(&info, "flags_0") == 0);
	}
	gen_free(&g);
}

/* The slot of an accepted flag nibble among 0x0, 0x8, 0xc and 0xe, or -1. */
static int
flag_slot(uint32_t nibble)
{
	switch (nibble) {
	case 0x0: return (0);
	case 0x8: return (1);
	case 0xc: return (2);
	case 0xe: return (3);
	}
	return (-1);
}

/* Every inode flag nibble on the sample's first file, whose six other
 * records carry none; the accepted nibbles are counted.
 */
static void
test_inode_flags(void)
{
	static const char *const names[] = {
		"flags_0", "flags_8", "flags_c", "flags_e"
	};
	fsimg_volume_info_t info;
	gen_t g;

	for (uint32_t nibble = 0; nibble < 16; nibble++) {
		uint64_t expected[4] = { 6, 0, 0, 0 };
		int slot = flag_slot(nibble);

		sample(&g);
		put32(&g, gen_record(&g, 1) + R_INODE, nibble << 28 | 2);
		if (slot < 0) {
			expect(&g, FSIMG_EUNSUPPORTED, "an unknown flag nibble");
			gen_free(&g);
			continue;
		}
		expect(&g, FSIMG_OK, "an accepted flag nibble");
		image_info(&g, &info);
		expected[slot]++;
		for (int i = 0; i < 4; i++)
			CHECK(detail(&info, names[i]) == expected[i]);
		gen_free(&g);
	}
	/* A flagged directory is accepted and counted. */
	sample(&g);
	put32(&g, gen_record(&g, 2) + R_INODE, 0xe0000003);
	expect(&g, FSIMG_OK, "a flagged directory");
	image_info(&g, &info);
	CHECK(detail(&info, "flags_e") == 1 && detail(&info, "flags_0") == 6);
	gen_free(&g);
}

/*
 * Hierarchy.
 */

/* An image of the given members, each "kind:path", in table order;
 * kind is d, f, l or p, and a file holds its own path as data.
 */
static void
tree_image(gen_t *g, const char *const *members)
{
	gen_init(g);
	for (size_t i = 0; members[i] != NULL; i++) {
		const char *path = members[i] + 2;

		switch (members[i][0]) {
		case 'd':
			gen_dir(g, path);
			break;
		case 'f':
			gen_file(g, path, path);
			break;
		case 'l':
			gen_link(g, path, "target");
			break;
		case 'p':
			gen_fifo(g, path);
			break;
		default:
			CHECK(false);
		}
	}
	gen_finish(g);
}

static void
expect_tree(const char *const *members, fsimg_status_t status,
    const char *label)
{
	gen_t g;

	tree_image(&g, members);
	expect(&g, status, label);
	gen_free(&g);
}

static void
expect_tree_paths(const char *const *members, const char *const *paths,
    const char *label)
{
	gen_t g;

	tree_image(&g, members);
	expect_paths(&g, paths, label);
	gen_free(&g);
}

static uint64_t
tree_detail(const char *const *members, const char *name)
{
	fsimg_volume_info_t info;
	gen_t g;

	tree_image(&g, members);
	image_info(&g, &info);
	gen_free(&g);
	return (detail(&info, name));
}

/* Require the tree to list exactly paths, as expect_tree_paths(), with
 * shadowed records counted by info.
 */
static void
expect_shadowed(const char *const *members, const char *const *paths,
    uint64_t shadowed, const char *label)
{
	expect_tree_paths(members, paths, label);
	if (tree_detail(members, "shadowed") != shadowed)
		fprintf(stderr, "%s: shadowed is not %" PRIu64 "\n", label,
		    shadowed);
	CHECK(tree_detail(members, "shadowed") == shadowed);
}

/* A path of depth components "d/d/.../f", the last one a file. */
static void
deep_path(char *path, size_t size, unsigned depth)
{
	size_t length = 0;

	for (unsigned i = 1; i < depth; i++) {
		CHECK(length + 2 < size);
		path[length++] = 'd';
		path[length++] = '/';
	}
	CHECK(length + 2 <= size);
	path[length++] = 'f';
	path[length] = '\0';
}

static void
test_hierarchy(void)
{
	char path[1100];
	listing_t l;
	model_t m;
	gen_t g;

	/* Implicit parents: one level, and the root itself. */
	expect_tree_paths((const char *[]){ "d:", "f:a/f", NULL },
	    (const char *[]){ "/a", "/a/f", NULL }, "an implicit parent");
	CHECK(tree_detail((const char *[]){ "d:", "f:a/f", NULL },
	    "implicit_directories") == 1);
	CHECK(tree_detail((const char *[]){ "f:a/f", NULL },
	    "implicit_directories") == 2);
	/* The root record last, and record order reversed. */
	expect_tree_paths((const char *[]){ "f:b/c", "f:a", "d:b", "d:", NULL },
	    (const char *[]){ "/a", "/b", "/b/c", NULL }, "the root last");
	expect_tree_paths((const char *[]){ "d:", "d:b", "f:a", "f:b/c", NULL },
	    (const char *[]){ "/a", "/b", "/b/c", NULL }, "the root first");
	CHECK(tree_detail((const char *[]){ "f:b/c", "f:a", "d:b", "d:", NULL },
	    "implicit_directories") == 0);

	/* Implicit parents at depth 32, and at 512 with the depth limit
	 * raised; the default limit of 256 levels refuses the latter.
	 */
	deep_path(path, sizeof(path), 32);
	gen_init(&g);
	gen_file(&g, path, "deep");
	gen_finish(&g);
	expect(&g, FSIMG_OK, "depth 32");
	gen_free(&g);
	deep_path(path, sizeof(path), 512);
	CHECK(strlen(path) == 1023);
	gen_init(&g);
	gen_file(&g, path, "deepest");
	gen_finish(&g);
	model_build(&m, g.image, g.length);
	CHECK(m.status == FSIMG_OK && m.entry_count == 512 &&
	    m.implicit == 512);
	list_image(g.image, g.length, 600, &l);
	CHECK(listing_matches(&l, &m, "depth 512"));
	CHECK(m.status != FSIMG_OK ||
	    verify_matches(g.image, g.length, &m, "depth 512"));
	listing_free(&l);
	list_image(g.image, g.length, 0, &l);
	CHECK(l.status == FSIMG_ELIMIT &&
	    l.error.resource == FSIMG_RESOURCE_DEPTH);
	listing_free(&l);
	model_free(&m);
	gen_free(&g);

	/* Duplicates: the last record of a path is the entry. */
	expect_tree((const char *[]){ "d:", "l:dup", "f:dup", NULL }, FSIMG_OK,
	    "a link then a file");
	expect_tree((const char *[]){ "d:", "f:dup", "l:dup", NULL }, FSIMG_OK,
	    "a file then a link");
	expect_tree((const char *[]){ "d:", "f:dup", "f:dup", NULL }, FSIMG_OK,
	    "two files");
	expect_tree((const char *[]){ "d:", "d:dup", "d:dup", "f:dup/x",
	    NULL }, FSIMG_OK, "two directories");
	expect_tree((const char *[]){ "d:", "d:", "f:a", NULL }, FSIMG_OK,
	    "a duplicate root");
	CHECK(tree_detail((const char *[]){ "d:", "l:dup", "f:dup", NULL },
	    "shadowed") == 1);
	CHECK(tree_detail((const char *[]){ "d:", "d:", "d:", NULL },
	    "shadowed") == 2);
	tree_image(&g, (const char *[]){ "d:", "f:dup", "l:dup", NULL });
	list_image(g.image, g.length, 0, &l);
	CHECK(l.count == 1 && l.entries[0].kind == FSIMG_ENTRY_SYMLINK);
	listing_free(&l);
	gen_free(&g);
	/* A shadowed record does not move the stack: x/y, then z twice. */
	expect_tree_paths((const char *[]){ "f:x/y", "f:z", "f:z", NULL },
	    (const char *[]){ "/x", "/x/y", "/z", NULL },
	    "a shadowed record after a subtree");

	/* The empty path must be a directory, as the entry or shadowed. */
	expect_tree((const char *[]){ "f:", NULL }, FSIMG_EUNSUPPORTED,
	    "a file root");
	expect_tree((const char *[]){ "f:", "d:", NULL }, FSIMG_EUNSUPPORTED,
	    "a shadowed file root");
	expect_tree((const char *[]){ "d:", "l:", NULL }, FSIMG_EUNSUPPORTED,
	    "a link root");

	/* A path below a file, a link or a FIFO is shadowed, its record
	 * before and after the descendant's; no parent is synthesized for it.
	 */
	expect_shadowed((const char *[]){ "f:x", "f:x/y", NULL },
	    (const char *[]){ "/x", NULL }, 1, "a path below a file");
	expect_shadowed((const char *[]){ "f:x/y", "f:x", NULL },
	    (const char *[]){ "/x", NULL }, 1, "a path below a later file");
	expect_shadowed((const char *[]){ "l:x", "f:x/y/z", NULL },
	    (const char *[]){ "/x", NULL }, 1, "a path below a link");
	CHECK(tree_detail((const char *[]){ "l:x", "f:x/y/z", NULL },
	    "implicit_directories") == 1);
	expect_shadowed((const char *[]){ "f:x/y", "l:x", NULL },
	    (const char *[]){ "/x", NULL }, 1, "a path below a later link");
	expect_shadowed((const char *[]){ "p:x", "d:x/y", NULL },
	    (const char *[]){ "/x", NULL }, 1, "a path below a FIFO");
	expect_shadowed((const char *[]){ "f:x/y", "p:x", NULL },
	    (const char *[]){ "/x", NULL }, 1, "a path below a later FIFO");
	/* Every record of a subtree below a link, at any depth and in any
	 * order, as in the H3 boot images, and the walk continuing after it.
	 */
	expect_shadowed((const char *[]){ "d:", "f:L/c/d/e", "l:L/a", "l:L",
	    "d:L/c", "f:L/b", "f:z", NULL }, (const char *[]){ "/L", "/z",
	    NULL }, 4, "a subtree below a link");
	expect_shadowed((const char *[]){ "d:", "f:a/L/x", "l:a/L", "f:a/b",
	    NULL }, (const char *[]){ "/a", "/a/L", "/a/b", NULL }, 1,
	    "a link with a sibling after its subtree");
	expect_shadowed((const char *[]){ "l:L", "f:L/a", "f:L/a", NULL },
	    (const char *[]){ "/L", NULL }, 2, "a duplicate below a link");
	/* The record delivered at a path decides: a directory shadowed by a
	 * file or link leaves its descendants below a non-directory, and a
	 * file or link shadowed by a directory is a parent.
	 */
	expect_shadowed((const char *[]){ "d:d", "f:d/x", "f:d", NULL },
	    (const char *[]){ "/d", NULL }, 2, "a directory shadowed by a file");
	expect_shadowed((const char *[]){ "d:L", "f:L/x", "l:L", NULL },
	    (const char *[]){ "/L", NULL }, 2, "a directory shadowed by a link");
	expect_shadowed((const char *[]){ "f:d", "d:d", "f:d/x", NULL },
	    (const char *[]){ "/d", "/d/x", NULL }, 1,
	    "a file shadowed by a directory");
	expect_shadowed((const char *[]){ "l:L", "f:L/x", "d:L", NULL },
	    (const char *[]){ "/L", "/L/x", NULL }, 1,
	    "a link shadowed by a directory");
	/* d-x sorts beside d, not below it; d/x is below the file d. */
	expect_tree_paths((const char *[]){ "f:d", "f:d-x", NULL },
	    (const char *[]){ "/d", "/d-x", NULL }, "a file beside d-x");
	expect_shadowed((const char *[]){ "f:d", "f:d-x", "f:d/x", NULL },
	    (const char *[]){ "/d", "/d-x", NULL }, 1,
	    "a file d with d-x and d/x");
	/* A link outside the profile is omitted once; the record below it is
	 * shadowed, not omitted.
	 */
	CHECK(tree_detail((const char *[]){ "l:x\x01", "f:x\x01/y", NULL },
	    "omitted") == 1);
	expect_shadowed((const char *[]){ "l:x\x01", "f:x\x01/y", "f:z", NULL },
	    (const char *[]){ "/z", NULL }, 1, "a path below an omitted link");
	/* A shadowed record is validated like any other: a range inside the
	 * table below a link still fails open.
	 */
	tree_image(&g, (const char *[]){ "l:L", "f:L/x", NULL });
	expect(&g, FSIMG_OK, "a valid file below a link");
	put32(&g, gen_record(&g, 1) + R_FILE_OFFSET, g.dir_offset);
	expect(&g, FSIMG_ECORRUPT, "a corrupt range below a link");
	gen_free(&g);
	expect_tree_paths((const char *[]){ "f:a-b", "f:a/b", NULL },
	    (const char *[]){ "/a", "/a/b", "/a-b", NULL },
	    "siblings ordered by component");

	/* Paths mkifs stores verbatim and fsimg does not represent. */
	expect_tree((const char *[]){ "f:/a", NULL }, FSIMG_EUNSUPPORTED,
	    "an absolute path");
	expect_tree((const char *[]){ "f:a/./b", NULL }, FSIMG_EUNSUPPORTED,
	    "a . component");
	expect_tree((const char *[]){ "f:a/../b", NULL }, FSIMG_EUNSUPPORTED,
	    "a .. component");
	expect_tree((const char *[]){ "f:..", NULL }, FSIMG_EUNSUPPORTED,
	    "a .. path");
	expect_tree((const char *[]){ "d:.", NULL }, FSIMG_EUNSUPPORTED,
	    "a . path");
	expect_tree((const char *[]){ "f:a//b", NULL }, FSIMG_EUNSUPPORTED,
	    "an empty component");
	expect_tree((const char *[]){ "d:a/", NULL }, FSIMG_EUNSUPPORTED,
	    "a trailing slash");
}

/*
 * Names.
 */

static void
test_name_lengths(void)
{
	static const size_t lengths[] = { 1, 255, 256, 1020, 1021, 1024 };
	char name[1100];
	listing_t l;
	gen_t g;

	for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
		memset(name, 'n', lengths[i]);
		name[lengths[i]] = '\0';
		gen_init(&g);
		gen_root(&g);
		gen_file(&g, name, "x");
		gen_link(&g, "link", name);
		gen_finish(&g);
		expect(&g, FSIMG_OK, "a long component");
		list_image(g.image, g.length, 0, &l);
		CHECK(l.count == 2 && strlen(l.entries[1].path) ==
		    lengths[i] + 1);
		listing_free(&l);
		gen_free(&g);
	}
	/* 1025 bytes is over the path limit. */
	memset(name, 'n', 1025);
	name[1025] = '\0';
	gen_init(&g);
	gen_file(&g, name, "x");
	gen_finish(&g);
	expect(&g, FSIMG_EUNSUPPORTED, "a 1025-byte path");
	gen_free(&g);
	memset(name, 'n', 1025);
	name[300] = '/';
	name[700] = '/';
	gen_init(&g);
	gen_file(&g, name, "x");
	gen_finish(&g);
	expect(&g, FSIMG_EUNSUPPORTED, "a 1025-byte path of three components");
	gen_free(&g);

	/* Names in the profile: UTF-8, a space, and case kept apart. */
	expect_tree_paths((const char *[]){ "f:caf\xc3\xa9", "f:a b", "f:Case",
	    "f:case", NULL }, (const char *[]){ "/Case", "/a b",
	    "/caf\xc3\xa9", "/case", NULL }, "names in the profile");
}

/*
 * Names outside the profile. The omitted image holds six omitted subtree
 * roots beside delivered entries: a file named with a control byte in the
 * implicit directory mixed, a Latin-1 file, a link named with a control
 * byte, a FIFO named with DEL, an explicit Latin-1 directory with a
 * subtree, and an implicit directory named with a control byte with one.
 * boot_ino[0] names the Latin-1 file.
 */
#define OMITTED_ROOTS	6u

static void
omitted_image(gen_t *g)
{
	size_t latin;

	gen_init(g);
	gen_root(g);
	gen_file(g, "keep", "k");
	gen_file(g, "mixed/ok", "o");
	gen_file(g, "mixed/bad\x02", "b");
	latin = gen_file(g, "caf\xe9", "c");
	gen_link(g, "ctl\x01", "keep");
	gen_fifo(g, "del\x7f");
	gen_dir(g, "dir\xff");
	gen_file(g, "dir\xff/child", "x");
	gen_file(g, "dir\xff/sub/deep", "y");
	gen_file(g, "imp\x01/x/y", "z");
	g->boot[0] = g->members[latin].inode;
	gen_finish(g);
}

/* Extract selection from image into "out" beneath the fixture's scratch
 * directory, through the extractor's internal entry point, which leaves
 * the error naming the failing entry.
 */
static fsimg_status_t
extract_image(fixture_t *f, const char *selection, bool skip,
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
	bzero(result, sizeof(*result));
	status = fsimg_extract_into(f->reader, root, &options, result, error);
	CHECK(close(root) == 0);
	fsimg_reader_free(f->reader);
	f->reader = NULL;
	return (status);
}

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

/* The extracted file at path beneath the fixture holds exactly bytes. */
static void
check_file(fixture_t *f, const char *path, const char *bytes)
{
	char buffer[256];
	ssize_t got;
	int fd;

	fd = openat(f->scratch, path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	CHECK(fd != -1);
	got = read(fd, buffer, sizeof(buffer));
	CHECK(got == (ssize_t)strlen(bytes) &&
	    memcmp(buffer, bytes, (size_t)got) == 0);
	CHECK(close(fd) == 0);
}

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

static void
test_omitted_names(void)
{
	static const char *const paths[] = {
		"/keep", "/mixed", "/mixed/ok", NULL
	};
	fsimg_extract_result_t result;
	fsimg_stats_t stats;
	fsimg_error_t error;
	listing_t l;
	fixture_t f;
	gen_t g;

	/* A listing delivers the entries in the profile and counts each
	 * omitted subtree root once, never its descendants; info agrees,
	 * and counts the boot reference naming an omitted file.
	 */
	omitted_image(&g);
	expect_paths(&g, paths, "the omitted image");
	fixture_open(&f, g.image, g.length, 0);
	fixture_list(&f, false, &l);
	CHECK(l.status == FSIMG_OK);
	CHECK(detail(&l.info, "omitted") == OMITTED_ROOTS);
	CHECK(detail(&l.info, "omitted_references") == 1);
	CHECK(detail(&l.info, "implicit_directories") == 4);
	CHECK(strcmp(l.info.profile, "little-endian, bootable") == 0);
	CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.omitted_entries == OMITTED_ROOTS);
	listing_free(&l);
	fixture_close(&f);

	/* Extracting a scope that holds an omitted entry fails naming the
	 * first in traversal order, rendered, with no name byte in the
	 * message; the skip option omits all six.
	 */
	fixture_open(&f, g.image, g.length, 0);
	bzero(&error, sizeof(error));
	CHECK(extract_image(&f, "/", false, &result, &error) ==
	    FSIMG_EUNSUPPORTED);
	CHECK(strcmp(error.path, "/caf\\xe9") == 0);
	CHECK(strchr(error.message, '\xe9') == NULL);
	remove_tree(f.scratch, "out");
	CHECK(extract_image(&f, "/", true, &result, NULL) == FSIMG_OK);
	CHECK(result.omitted_entries == OMITTED_ROOTS);
	CHECK(result.skipped_entries == 0);
	check_file(&f, "out/keep", "k");
	check_file(&f, "out/mixed/ok", "o");
	CHECK(directory_names(&f, "out") == 2);
	CHECK(directory_names(&f, "out/mixed") == 1);
	remove_tree(f.scratch, "out");
	bzero(&error, sizeof(error));
	CHECK(extract_image(&f, "/mixed", false, &result, &error) ==
	    FSIMG_EUNSUPPORTED);
	CHECK(strcmp(error.path, "/mixed/bad\\x02") == 0);
	remove_tree(f.scratch, "out");
	/* No query spells an omitted name. */
	CHECK(extract_image(&f, "/ctl\x01", true, &result, NULL) ==
	    FSIMG_EINVAL);
	remove_tree(f.scratch, "out");
	fixture_close(&f);

	/* An omitted name is checked like any other: a corrupt range below
	 * the omitted directory fails open.
	 */
	put32(&g, gen_record(&g, 9) + R_FILE_OFFSET, g.dir_offset);
	expect(&g, FSIMG_ECORRUPT, "a corrupt range below an omitted name");
	gen_free(&g);
}

/*
 * Ranges.
 */

static void
test_ranges(void)
{
	static const size_t sizes[] = {
		0, 1, 3, 4, 65535, 65536, 65537, (16u << 20) + 1
	};
	fsimg_extract_result_t result;
	fsimg_error_t error;
	unsigned char *r;
	fixture_t f;
	gen_t g;
	size_t data;

	/* The data region is [hdr_dir_size, image_size - 4). */
	one_record(&g, GEN_FILE);
	r = gen_record(&g, 1);
	put32(&g, r + R_FILE_OFFSET, g.image_size - 4 - 4);
	expect(&g, FSIMG_OK, "a range ending at the trailer");
	put32(&g, r + R_FILE_OFFSET, g.image_size - 4 - 3);
	expect(&g, FSIMG_ECORRUPT, "a range including the trailer");
	put32(&g, r + R_FILE_OFFSET, g.hdr_dir_size);
	expect(&g, FSIMG_OK, "a range starting at hdr_dir_size");
	put32(&g, r + R_FILE_OFFSET, g.hdr_dir_size - 1);
	expect(&g, FSIMG_ECORRUPT, "a range inside the table");
	put32(&g, r + R_FILE_OFFSET, 0x10);
	expect(&g, FSIMG_ECORRUPT, "a range inside the header");
	put32(&g, r + R_FILE_OFFSET, 0xfffffffe);
	expect(&g, FSIMG_ECORRUPT, "a range overflowing 32 bits");
	/* An empty file's offset is not interpreted. */
	put32(&g, r + R_FILE_SIZE, 0);
	put32(&g, r + R_FILE_OFFSET, 0);
	expect(&g, FSIMG_OK, "an empty file at offset 0");
	put32(&g, r + R_FILE_OFFSET, g.dir_offset);
	expect(&g, FSIMG_OK, "an empty file inside the table");
	put32(&g, r + R_FILE_OFFSET, 0xfffffff0);
	expect(&g, FSIMG_OK, "an empty file past the image");
	gen_free(&g);

	/* Sizes around the 64 KiB boundary and above 16 MiB. */
	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		gen_init(&g);
		gen_root(&g);
		data = gen_add_bytes(&g, GEN_FILE, "f", 1, NULL, 0);
		free(g.members[data].bytes);
		g.members[data].bytes = xmalloc(sizes[i]);
		for (size_t b = 0; b < sizes[i]; b++)
			g.members[data].bytes[b] = (unsigned char)(b * 7 + b / 251);
		g.members[data].bytes_length = sizes[i];
		gen_file(&g, "g", "after");
		gen_finish(&g);
		expect(&g, FSIMG_OK, "a file size");
		gen_free(&g);
	}

	/* Shared ranges are separate entries with the same bytes; extracting
	 * a heavily shared image meets the default output ceiling, the
	 * volume's size.
	 */
	gen_init(&g);
	gen_root(&g);
	data = gen_add_bytes(&g, GEN_FILE, "f000", 4, NULL, 0);
	free(g.members[data].bytes);
	g.members[data].bytes = xmalloc(4096);
	memset(g.members[data].bytes, 's', 4096);
	g.members[data].bytes_length = 4096;
	for (unsigned i = 1; i < 64; i++) {
		char name[8];
		size_t shared;

		snprintf(name, sizeof(name), "f%03u", i);
		shared = gen_add_bytes(&g, GEN_FILE, name, 4, NULL, 0);
		free(g.members[shared].bytes);
		g.members[shared].bytes = copy_bytes(g.members[data].bytes, 4096);
		g.members[shared].bytes_length = 4096;
		g.members[shared].share = (int)data;
	}
	gen_finish(&g);
	CHECK(g.image_size < 3 * 4096);
	expect(&g, FSIMG_OK, "shared ranges");
	fixture_open(&f, g.image, g.length, 0);
	bzero(&error, sizeof(error));
	CHECK(extract_image(&f, "/", false, &result, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_OUTPUT);
	remove_tree(f.scratch, "out");
	fixture_close(&f);
	gen_free(&g);
}

/*
 * Symbolic links.
 */

static void
test_links(void)
{
	static const size_t targets[] = { 1, 1023, 1024 };
	static const char *const kinds_of_target[] = {
		"relative/target", "/absolute/target", "no/such/target",
		"../../up", "\xff\x01 bytes"
	};
	char target[1100];
	unsigned char *r;
	gen_t g;

	/* sym_offset is the path length plus one, and nothing else. */
	one_record(&g, GEN_LINK);
	r = gen_record(&g, 1);
	put16(&g, r + R_LINK_OFFSET, 3);
	expect(&g, FSIMG_EUNSUPPORTED, "sym_offset L");
	put16(&g, r + R_LINK_OFFSET, 5);
	expect(&g, FSIMG_EUNSUPPORTED, "sym_offset L + 2");
	put16(&g, r + R_LINK_OFFSET, 4);
	expect(&g, FSIMG_OK, "sym_offset L + 1");
	/* The target's NUL; then targets that do not fit the record. */
	r[0x1c + 4 + 3] = 'x';
	expect(&g, FSIMG_EUNSUPPORTED, "a target without its NUL");
	r[0x1c + 4 + 3] = '\0';
	/* A target holds no NUL: its terminator is the byte after it. */
	r[0x1c + 4 + 1] = '\0';
	expect(&g, FSIMG_EUNSUPPORTED, "a NUL inside the target");
	r[0x1c + 4 + 1] = 't';
	expect(&g, FSIMG_OK, "the target restored");
	put16(&g, r + R_LINK_SIZE, 0);
	expect(&g, FSIMG_EUNSUPPORTED, "an empty target");
	put16(&g, r + R_LINK_SIZE, 4);
	expect(&g, FSIMG_ECORRUPT, "a target crossing the record");
	put16(&g, r + R_LINK_SIZE, 1025);
	expect(&g, FSIMG_ECORRUPT, "a 1025-byte target crossing the record");
	put16(&g, r + R_LINK_SIZE, 3);
	gen_free(&g);

	/* Target lengths 1, 1023 and 1024 read; 1025 does not. */
	for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
		memset(target, 't', targets[i]);
		target[targets[i]] = '\0';
		gen_init(&g);
		gen_root(&g);
		gen_link(&g, "l", target);
		gen_finish(&g);
		expect(&g, FSIMG_OK, "a target length");
		gen_free(&g);
	}
	memset(target, 't', 1025);
	target[1025] = '\0';
	gen_init(&g);
	gen_root(&g);
	gen_link(&g, "l", target);
	gen_finish(&g);
	expect(&g, FSIMG_EUNSUPPORTED, "a 1025-byte target");
	gen_free(&g);

	/* Targets are bytes, never resolved. */
	gen_init(&g);
	gen_root(&g);
	for (size_t i = 0; i < sizeof(kinds_of_target) /
	    sizeof(kinds_of_target[0]); i++) {
		char name[8];

		snprintf(name, sizeof(name), "l%zu", i);
		gen_link(&g, name, kinds_of_target[i]);
	}
	gen_finish(&g);
	expect(&g, FSIMG_OK, "targets of every shape");
	gen_free(&g);
}

/*
 * Metadata and references.
 */

static void
test_metadata(void)
{
	static const uint32_t owners[] = { 0, 65535, 4294967295u };
	static const uint32_t modes[] = { 0000, 0644, 0755, 04755, 07777 };
	static const uint32_t times[] = { 0, 4294967295u };
	listing_t l;
	gen_t g;
	size_t index;

	gen_init(&g);
	gen_root(&g);
	for (size_t i = 0; i < 3; i++) {
		char name[8];

		snprintf(name, sizeof(name), "o%zu", i);
		index = gen_file(&g, name, "x");
		g.members[index].uid = owners[i];
		g.members[index].gid = owners[2 - i];
	}
	for (size_t i = 0; i < 5; i++) {
		char name[8];

		snprintf(name, sizeof(name), "m%zu", i);
		index = gen_dir(&g, name);
		g.members[index].mode = 0040000 | modes[i];
	}
	for (size_t i = 0; i < 2; i++) {
		char name[8];

		snprintf(name, sizeof(name), "t%zu", i);
		index = gen_fifo(&g, name);
		g.members[index].mtime = times[i];
	}
	gen_finish(&g);
	expect(&g, FSIMG_OK, "owners, modes and times");
	list_image(g.image, g.length, 0, &l);
	CHECK(listing_find(&l, "/o2")->uid == 4294967295u &&
	    listing_find(&l, "/o2")->gid == 0);
	CHECK(listing_find(&l, "/m3")->mode == 04755);
	CHECK(listing_find(&l, "/m4")->mode == 07777);
	CHECK(listing_find(&l, "/t1")->mtime == INT64_C(4294967295));
	listing_free(&l);
	gen_free(&g);
}

/* An image of the given members, then a boot program and a script, with
 * boot_ino[0] naming the boot program, script_ino the script and
 * boot_ino[slot] the member at index.
 */
static void
reference_image(gen_t *g, const char *const *members, unsigned slot,
    size_t index)
{
	size_t boot;

	tree_image(g, members);
	boot = gen_file(g, "proc/boot/procnto", "kernel");
	g->members[boot].inode |= 0xe0000000u;
	g->script = g->members[gen_file(g, "proc/boot/.script", "script")].inode;
	g->boot[0] = g->members[boot].inode;
	g->boot[slot] = g->members[index].inode;
	gen_finish(g);
}

static void
expect_reference(const char *const *members, size_t index,
    fsimg_status_t status, const char *label)
{
	gen_t g;

	reference_image(&g, members, 1, index);
	expect_verify(&g, status, label);
	gen_free(&g);
}

static void
test_references(void)
{
	fsimg_volume_info_t info;
	gen_t g;
	size_t boot, script, twin;

	gen_init(&g);
	gen_root(&g);
	boot = gen_file(&g, "proc/boot/procnto", "kernel");
	script = gen_file(&g, "proc/boot/.script", "script");
	g.members[boot].inode |= 0xe0000000u;
	g.boot[0] = g.members[boot].inode;
	g.script = g.members[script].inode;
	gen_finish(&g);
	expect_verify(&g, FSIMG_OK, "a bootable image");
	image_info(&g, &info);
	CHECK(strcmp(info.profile, "little-endian, bootable") == 0);
	CHECK(detail(&info, "boot_ino_0") == g.members[boot].inode);
	CHECK(detail(&info, "script_ino") == g.members[script].inode);
	CHECK(detail(&info, "flags_e") == 1);
	CHECK(detail(&info, "omitted_references") == 0);
	CHECK(detail(&info, "shadowed_references") == 0);
	/* The flag nibble is part of the inode word a reference names. */
	g.boot[0] = g.members[boot].inode & 0x0fffffffu;
	gen_finish(&g);
	expect_verify(&g, FSIMG_ECORRUPT, "a reference without the flag nibble");
	g.boot[0] = g.members[boot].inode;
	/* Two records carrying one word. */
	twin = gen_file(&g, "proc/boot/twin", "twin");
	g.members[twin].inode = g.members[script].inode;
	gen_finish(&g);
	expect_verify(&g, FSIMG_ECORRUPT, "a reference naming two records");
	gen_free(&g);

	/* Any nonzero slot makes the image bootable; open does not judge a
	 * reference, which verification checks.
	 */
	sample(&g);
	expect_verify(&g, FSIMG_OK, "all references zero");
	g.boot[3] = 0x12345;
	gen_finish(&g);
	expect_verify(&g, FSIMG_ECORRUPT, "a reference naming no record");
	image_info(&g, &info);
	CHECK(strcmp(info.profile, "little-endian, bootable") == 0);
	CHECK(detail(&info, "boot_ino_3") == 0x12345);
	g.boot[3] = g.members[1].inode;
	gen_finish(&g);
	expect_verify(&g, FSIMG_OK, "boot_ino[3] naming one file");
	gen_free(&g);

	/* A shadowed record has no entry: a reference naming only an earlier
	 * duplicate or a record below a link names nothing, while the
	 * duplicate's entry is named.
	 */
	expect_reference((const char *[]){ "d:", "f:dup", "f:dup", NULL }, 1,
	    FSIMG_ECORRUPT, "a reference naming an earlier duplicate");
	expect_reference((const char *[]){ "d:", "f:dup", "f:dup", NULL }, 2,
	    FSIMG_OK, "a reference naming the duplicate's entry");
	expect_reference((const char *[]){ "d:", "l:L", "f:L/x", NULL }, 2,
	    FSIMG_ECORRUPT, "a reference naming a record below a link");
	/* Every other kind is refused, the root record's included. */
	expect_reference((const char *[]){ "d:", "d:dir", NULL }, 1,
	    FSIMG_EUNSUPPORTED, "a reference naming a directory");
	expect_reference((const char *[]){ "d:", "f:x", NULL }, 0,
	    FSIMG_EUNSUPPORTED, "a reference naming the root");
	expect_reference((const char *[]){ "d:", "l:link", NULL }, 1,
	    FSIMG_EUNSUPPORTED, "a reference naming a link");
	expect_reference((const char *[]){ "d:", "p:pipe", NULL }, 1,
	    FSIMG_EUNSUPPORTED, "a reference naming a FIFO");

	/* Matching counts nodes only, so a shadowed record carrying the word
	 * of the entry a reference names does not disturb verification; info
	 * counts the reference in shadowed_references, once however many
	 * shadowed records carry its word, and once per reference slot.
	 */
	reference_image(&g, (const char *[]){ "d:", "f:dup", "f:dup", NULL }, 1,
	    2);
	g.members[1].inode = g.members[2].inode;
	gen_finish(&g);
	expect_verify(&g, FSIMG_OK, "a shadowed duplicate carrying the word");
	image_info(&g, &info);
	CHECK(detail(&info, "shadowed_references") == 1);
	g.boot[2] = g.members[2].inode;
	gen_finish(&g);
	expect_verify(&g, FSIMG_OK, "two references to a shadowed word");
	image_info(&g, &info);
	CHECK(detail(&info, "shadowed_references") == 2);
	gen_free(&g);
	reference_image(&g, (const char *[]){ "d:", "f:dup", "f:dup", "f:dup",
	    NULL }, 1, 3);
	g.members[1].inode = g.members[3].inode;
	g.members[2].inode = g.members[3].inode;
	gen_finish(&g);
	expect_verify(&g, FSIMG_OK, "two shadowed duplicates carrying the word");
	image_info(&g, &info);
	CHECK(detail(&info, "shadowed") == 2 &&
	    detail(&info, "shadowed_references") == 1);
	gen_free(&g);
	reference_image(&g, (const char *[]){ "d:", "f:x", "l:L", "f:L/y", NULL },
	    1, 1);
	g.members[3].inode = g.members[1].inode;
	gen_finish(&g);
	expect_verify(&g, FSIMG_OK, "a record below a link carrying the word");
	image_info(&g, &info);
	CHECK(detail(&info, "shadowed_references") == 1);
	gen_free(&g);
}

/*
 * Verification.
 */

/* Flip one byte of a finished image: it still opens, and only the
 * checksum notices; flipped back, it verifies again.
 */
static void
expect_flip(gen_t *g, size_t offset, const char *label)
{
	g->image[offset] ^= 0x10;
	expect_verify(g, FSIMG_ECORRUPT, label);
	g->image[offset] ^= 0x10;
	expect_verify(g, FSIMG_OK, label);
}

/* Fill every byte no decision reads with 0xee: each record's bytes after
 * its path, or its link target, and their NUL, and the gaps in the data
 * region; then recompute the trailer.
 */
static void
fill_padding(gen_t *g)
{
	unsigned char *used;

	for (size_t i = 0; i < g->count; i++) {
		const gen_member_t *m = &g->members[i];
		size_t end = kinds[m->kind].path + m->path_length + 1;

		if (m->kind == GEN_LINK)
			end += m->bytes_length + 1;
		memset(g->image + m->table_offset + end, 0xee, m->length - end);
	}
	used = calloc(1, g->image_size);
	CHECK(used != NULL);
	for (size_t i = 0; i < g->count; i++)
		if (g->members[i].kind == GEN_FILE)
			memset(used + g->members[i].data_offset, 1,
			    g->members[i].bytes_length);
	for (size_t o = g->hdr_dir_size; o < g->image_size - 4; o++)
		if (!used[o])
			g->image[o] = 0xee;
	free(used);
	gen_trailer(g);
}

/* Open image under a work limit and verify it; *opened tells whether open
 * succeeded, and the status is the first failure.
 */
static fsimg_status_t
verify_with_work(const unsigned char *image, size_t length, uint64_t work,
    bool *opened, fsimg_error_t *error)
{
	fsimg_verify_report_t report;
	fsimg_options_t options;
	fsimg_io_ops_t ops;
	fsimg_status_t status;
	fixture_t f;

	bzero(&f, sizeof(f));
	f.image = image;
	f.length = length;
	fsimg_options_init(&options);
	options.max_work = work;
	CHECK(fsimg_ctx_new(&options, &f.ctx, NULL) == FSIMG_OK);
	fsimg_io_ops_init(&ops);
	ops.read_at = image_read;
	ops.get_size = image_size;
	CHECK(fsimg_io_from_callbacks(f.ctx, &ops, &f, &f.io, NULL) ==
	    FSIMG_OK);
	status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, error);
	*opened = status == FSIMG_OK;
	if (*opened)
		status = fsimg_verify(f.reader, 0, &report, error);
	fsimg_reader_free(f.reader);
	fsimg_io_free(f.io);
	fsimg_ctx_free(f.ctx);
	return (status);
}

/* The work verification of a root and one file of length bytes needs
 * beyond what opening it needs: the smallest limits under which each
 * succeeds, found by trying every limit. Just below verification's,
 * opening succeeds and verification runs out of work.
 */
static uint64_t
verify_work(size_t length)
{
	fsimg_error_t error;
	fsimg_status_t status;
	uint64_t work, open_work;
	size_t index;
	bool opened;
	gen_t g;

	gen_init(&g);
	gen_root(&g);
	index = gen_add_bytes(&g, GEN_FILE, "f", 1, NULL, 0);
	free(g.members[index].bytes);
	g.members[index].bytes = xmalloc(length);
	memset(g.members[index].bytes, 'w', length);
	g.members[index].bytes_length = length;
	gen_finish(&g);
	open_work = 0;
	for (work = 1;; work++) {
		CHECK(work < 1000);
		bzero(&error, sizeof(error));
		status = verify_with_work(g.image, g.length, work, &opened,
		    &error);
		if (opened && open_work == 0)
			open_work = work;
		if (status == FSIMG_OK)
			break;
		CHECK(status == FSIMG_ELIMIT &&
		    error.resource == FSIMG_RESOURCE_WORK);
	}
	CHECK(open_work != 0 && open_work < work);
	gen_free(&g);
	return (work - open_work);
}

static void
test_verify(void)
{
	fsimg_verify_report_t report;
	unsigned char trailer[4];
	gen_t g;

	/* The sample's report: five files of 24 bytes, the link target
	 * included, and four directories, the root and the two implicit ones
	 * included; every byte of the image is an allocation unit in use.
	 */
	sample(&g);
	expect_verify(&g, FSIMG_OK, "the sample");
	CHECK(verify_slice(g.image, g.length, FSIMG_VERIFY_PAYLOAD, &report,
	    NULL) == FSIMG_OK);
	CHECK(report.files == 5 && report.directories == 4 &&
	    report.logical_bytes == 24 && report.payload_bytes_read == 24 &&
	    report.allocated_clusters == g.image_size &&
	    report.free_clusters == 0 && report.omitted_entries == 0);

	/* One flipped byte in each region, and a byte-swapped trailer. */
	expect_flip(&g, H_MOUNT_FLAGS, "a flipped header byte");
	expect_flip(&g, H_MOUNTPOINT, "a flipped mountpoint byte");
	expect_flip(&g, H_MOUNTPOINT + 2, "flipped mountpoint padding");
	expect_flip(&g, g.members[1].table_offset + R_UID,
	    "a flipped record byte");
	expect_flip(&g, g.members[1].table_offset + g.members[1].length - 1,
	    "flipped record padding");
	expect_flip(&g, g.members[1].data_offset, "a flipped data byte");
	expect_flip(&g, g.image_size - 1, "a flipped trailer byte");
	bcopy(g.image + g.image_size - 4, trailer, sizeof(trailer));
	for (unsigned i = 0; i < 4; i++)
		g.image[g.image_size - 4 + i] = trailer[3 - i];
	CHECK(bcmp(trailer, g.image + g.image_size - 4, 4) != 0);
	expect_verify(&g, FSIMG_ECORRUPT, "a byte-swapped trailer");
	gen_free(&g);

	/* A big-endian image sums in its own order. */
	gen_init(&g);
	g.big = true;
	gen_root(&g);
	gen_file(&g, "a.txt", "hello");
	gen_link(&g, "l", "a.txt");
	gen_finish(&g);
	expect_verify(&g, FSIMG_OK, "a big-endian image");
	g.big = false;
	gen_trailer(&g);
	g.big = true;
	expect_verify(&g, FSIMG_ECORRUPT,
	    "a big-endian image summed little-endian");
	gen_free(&g);

	/* Nonzero padding in the mountpoint, the records and the data gaps
	 * verifies under a correct trailer.
	 */
	gen_init(&g);
	g.mountpoint_pad = 0x5a;
	g.data_align = 8;
	gen_root(&g);
	g.members[gen_file(&g, "f", "abc")].pad = 8;
	gen_link(&g, "l", "f");
	gen_file(&g, "g", "defgh");
	gen_finish(&g);
	fill_padding(&g);
	CHECK(g.image[g.members[1].data_offset + 3] == 0xee);
	expect_verify(&g, FSIMG_OK, "nonzero padding");
	gen_free(&g);

	/* The volume must be exactly image_size long: one byte and one
	 * sector more are corrupt.
	 */
	for (size_t extra = 1; extra <= 512; extra += 511) {
		sample(&g);
		gen_finish_extra(&g, extra);
		expect_verify(&g, FSIMG_ECORRUPT, "a volume past image_size");
		gen_free(&g);
	}

	/* An invalid mountpoint leaves the volume unverifiable. */
	gen_init(&g);
	gen_mountpoint(&g, "/m\xff", 3);
	gen_root(&g);
	gen_file(&g, "f", "data");
	gen_finish(&g);
	expect_verify(&g, FSIMG_EUNSUPPORTED, "an invalid mountpoint");
	gen_free(&g);

	/* Omitted subtrees are walked and checked, their payload read, and
	 * reported only as omitted_entries; the boot reference naming an
	 * omitted file is satisfied.
	 */
	omitted_image(&g);
	expect_verify(&g, FSIMG_OK, "the omitted image");
	CHECK(verify_slice(g.image, g.length, FSIMG_VERIFY_PAYLOAD, &report,
	    NULL) == FSIMG_OK);
	CHECK(report.omitted_entries == OMITTED_ROOTS && report.files == 2 &&
	    report.directories == 2 && report.logical_bytes == 2 &&
	    report.payload_bytes_read == 11);
	gen_free(&g);

	/* Shadowed records, an earlier duplicate and a record below a link,
	 * were validated at open and are no entries.
	 */
	tree_image(&g, (const char *[]){ "d:", "l:dup", "f:dup", "l:L",
	    "f:L/x", NULL });
	expect_verify(&g, FSIMG_OK, "shadowed records");
	CHECK(verify_slice(g.image, g.length, 0, &report, NULL) == FSIMG_OK);
	CHECK(report.files == 2 && report.directories == 1);
	gen_free(&g);

	/* The checksum pass charges a unit per 64 KiB window, and its read
	 * another: an image holding a 1 MiB file spans 17 windows, one
	 * holding a 4-byte file one, and their trees and walks cost the same.
	 */
	CHECK(verify_work(1u << 20) - verify_work(4) == 2 * 16);

	/* An unsupported record fails open, so no report is complete. */
	one_record(&g, GEN_FILE);
	put32(&g, gen_record(&g, 1) + R_MODE, 0060644);
	gen_trailer(&g);
	CHECK(verify_slice(g.image, g.length, 0, &report, NULL) ==
	    FSIMG_EUNSUPPORTED && !report.complete);
	gen_free(&g);
}

/*
 * The reader against the model on random trees: random paths from a small
 * alphabet that makes duplicates, parents below files and names outside
 * the profile common, in random table order, in both byte orders.
 */
static uint64_t random_state = 0x2545f4914f6cdd1dULL;

static uint32_t
random_below(uint32_t limit)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 7;
	random_state ^= random_state << 17;
	return ((uint32_t)(random_state % limit));
}

/* Records the open pass cannot decode from one small read: a file and a
 * link padded past the decode buffer, a record of the largest length, and a
 * table of about 20 KiB whose records straddle each 8 KiB read. The model
 * predicts every entry.
 */
static void
test_long_records(void)
{
	static const uint32_t pads[] = { 2100, 4000, 65532 - 0x24 };
	char path[16], target[16];
	listing_t l;
	model_t m;
	gen_t g;
	size_t index;

	for (size_t i = 0; i < sizeof(pads) / sizeof(pads[0]); i++) {
		gen_init(&g);
		gen_root(&g);
		g.members[gen_file(&g, "f", "contents")].pad = pads[i];
		g.members[gen_link(&g, "l", "target")].pad = pads[i] - 4;
		gen_dir(&g, "d");
		gen_finish(&g);
		model_build(&m, g.image, g.length);
		CHECK(m.status == FSIMG_OK);
		list_image(g.image, g.length, 0, &l);
		CHECK(listing_matches(&l, &m, "a long record"));
		CHECK(m.status != FSIMG_OK ||
		    verify_matches(g.image, g.length, &m, "a long record"));
		listing_free(&l);
		model_free(&m);
		gen_free(&g);
	}
	/* The file record of 0x20 + 2 bytes rounded to 0x24 is the largest
	 * a 16-bit length holds when padded to 65532.
	 */
	gen_init(&g);
	gen_root(&g);
	index = gen_file(&g, "f", "x");
	g.members[index].pad = 65532 - 0x24;
	gen_finish(&g);
	CHECK(g.members[index].length == 65532);
	expect(&g, FSIMG_OK, "a 65532-byte record");
	gen_free(&g);

	gen_init(&g);
	gen_root(&g);
	for (unsigned i = 0; i < 60; i++) {
		snprintf(path, sizeof(path), "d%u/f%u", i % 7, i);
		snprintf(target, sizeof(target), "t%u", i);
		index = i % 3 == 0 ? gen_link(&g, path, target) :
		    gen_file(&g, path, target);
		g.members[index].pad = 4 * (50 + i * 7 % 61);
	}
	gen_finish(&g);
	CHECK(g.hdr_dir_size - g.dir_offset > 2 * IFS_TEST_WINDOW);
	model_build(&m, g.image, g.length);
	CHECK(m.status == FSIMG_OK);
	list_image(g.image, g.length, 0, &l);
	CHECK(listing_matches(&l, &m, "a table of several windows"));
	CHECK(m.status != FSIMG_OK ||
	    verify_matches(g.image, g.length, &m, "a table of several windows"));
	listing_free(&l);
	model_free(&m);
	gen_free(&g);
}

static void
test_random(void)
{
	static const char *const components[] = {
		"a", "b", "a-b", "B", "caf\xc3\xa9", "x\x01", "z"
	};
	unsigned opened = 0, below = 0;
	char path[64], target[16];

	for (unsigned round = 0; round < 400; round++) {
		gen_t g;
		listing_t l;
		model_t m;
		unsigned count = 1 + random_below(12);

		gen_init(&g);
		g.big = random_below(2) == 0;
		if (random_below(3) != 0)
			gen_root(&g);
		for (unsigned i = 0; i < count; i++) {
			unsigned depth = 1 + random_below(3);
			size_t length = 0, index;

			for (unsigned d = 0; d < depth; d++) {
				const char *c = components[random_below(
				    sizeof(components) / sizeof(components[0]))];

				length += (size_t)snprintf(path + length,
				    sizeof(path) - length, "%s%s",
				    d == 0 ? "" : "/", c);
			}
			snprintf(target, sizeof(target), "t%u", i);
			switch (random_below(6)) {
			case 0:
			case 1:
				index = gen_dir(&g, path);
				break;
			case 2:
				index = gen_link(&g, path, target);
				break;
			case 3:
				index = gen_fifo(&g, path);
				break;
			default:
				index = gen_file(&g, path, target);
				break;
			}
			g.members[index].pad = 4 * random_below(3);
		}
		gen_finish(&g);
		model_build(&m, g.image, g.length);
		list_image(g.image, g.length, 0, &l);
		CHECK(listing_matches(&l, &m, "a random tree"));
		CHECK(m.status != FSIMG_OK ||
		    verify_matches(g.image, g.length, &m, "a random tree"));
		opened += m.status == FSIMG_OK ? 1 : 0;
		below += m.below != 0 ? 1 : 0;
		listing_free(&l);
		model_free(&m);
		gen_free(&g);
	}
	/* Every tree opens, and records below a non-directory are common. */
	CHECK(opened == 400 && below > 50);
}

/* Extraction of a tree of files and directories. */
static void
test_extract(void)
{
	fsimg_extract_result_t result;
	fixture_t f;
	gen_t g;

	tree_image(&g, (const char *[]){ "d:", "f:top", "f:sub/inner",
	    "d:empty", NULL });
	fixture_open(&f, g.image, g.length, 0);
	CHECK(extract_image(&f, "/", false, &result, NULL) == FSIMG_OK);
	CHECK(result.entries == 4);
	check_file(&f, "out/top", "top");
	check_file(&f, "out/sub/inner", "sub/inner");
	CHECK(directory_names(&f, "out/empty") == 0);
	remove_tree(f.scratch, "out");
	CHECK(extract_image(&f, "/sub", false, &result, NULL) == FSIMG_OK);
	check_file(&f, "out/sub/inner", "sub/inner");
	remove_tree(f.scratch, "out");
	fixture_close(&f);
	gen_free(&g);
}

/*
 * Faults. Each case fails one resource or one read during an operation and
 * requires the operation to end with that failure's class; fixture_close
 * then checks that the reader released all charged memory, scratch and
 * descriptors and left the scratch directory empty.
 */

/* What a fault case runs: open and a listing that reads every file and
 * link, which covers the table pass, the hierarchy, traversal and content
 * reads; or open and full verification with payload read-through.
 */
typedef enum operation {
	OPERATION_LIST = 1,
	OPERATION_VERIFY
} operation_t;

/* The fault image: the sample's kinds, explicit and implicit directories,
 * and 200 files in one directory, so that the table spans two of the
 * reader's 8 KiB table reads and a one-page metadata cache spills.
 */
static void
fault_image(gen_t *g)
{
	char path[32], data[32];

	gen_init(g);
	gen_root(g);
	gen_file(g, "a.txt", "hello");
	gen_dir(g, "dir");
	gen_file(g, "dir/inner", "inner data");
	gen_link(g, "dir/up", "../a.txt");
	gen_fifo(g, "pipe");
	gen_file(g, "deep/implicit/file", "x");
	for (unsigned i = 0; i < 200; i++) {
		snprintf(path, sizeof(path), "many/file-%03u", i);
		snprintf(data, sizeof(data), "content %u", i);
		gen_file(g, path, data);
	}
	gen_finish(g);
	/* The last record lies beyond the reader's first table read. */
	CHECK(g->members[g->count - 1].table_offset >= g->dir_offset +
	    IFS_TEST_WINDOW);
}

/* Run operation on the fixture's image, forced to ifs when forced is set,
 * so that the forced format's probe reads only the 88-byte ImageFS header
 * and every later read is the driver's own.
 */
static fsimg_status_t
run_operation(fixture_t *f, operation_t operation, bool forced,
    fsimg_error_t *error)
{
	fsimg_read_options_t options;
	fsimg_verify_report_t report;
	listing_t l;
	fsimg_status_t status;

	if (operation == OPERATION_LIST) {
		fixture_list(f, forced, &l);
		status = l.status;
		if (error != NULL)
			*error = l.error;
		listing_free(&l);
		return (status);
	}
	fsimg_read_options_init(&options);
	if (forced)
		options.format = "ifs";
	status = fsimg_reader_open(f->ctx, f->io, &options, &f->reader, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_verify(f->reader, FSIMG_VERIFY_PAYLOAD, &report, error);
	CHECK(report.complete == (status == FSIMG_OK));
	return (status);
}

/* Cancellation at every poll and the work limit at every value, each until
 * the operation needs no more, end it cleanly with the failure's class.
 * The polls and work units grow with the entries, so these run on a small
 * image.
 */
static void
check_interrupts(const gen_t *g, operation_t operation)
{
	const fixture_spec_t defaults = { 0 };
	fsimg_error_t error;
	fsimg_status_t status;
	uint64_t n, failures;
	fixture_t f;

	failures = 0;
	for (n = 1;; n++) {
		fixture_new(&f, g->image, g->length, &defaults);
		f.cancel_after = n;
		status = run_operation(&f, operation, false, NULL);
		fixture_close(&f);
		if (status == FSIMG_OK)
			break;
		CHECK(status == FSIMG_ECANCELLED);
		failures++;
		CHECK(n < 100000);
	}
	CHECK(failures > 0);

	failures = 0;
	for (n = 1;; n++) {
		const fixture_spec_t limited = { .max_work = n };

		fixture_new(&f, g->image, g->length, &limited);
		bzero(&error, sizeof(error));
		status = run_operation(&f, operation, false, &error);
		fixture_close(&f);
		if (status == FSIMG_OK)
			break;
		CHECK(status == FSIMG_ELIMIT &&
		    error.resource == FSIMG_RESOURCE_WORK);
		failures++;
		CHECK(n < 100000);
	}
	CHECK(failures > 0);
}

/* Allocation at every occurrence and every kind of scratch operation at
 * every occurrence, or at growing steps past the 64th, each until the
 * operation needs no more, end it cleanly with the failure's class. Reads
 * of at most one and seven bytes change nothing: the listing still matches
 * the model.
 */
static void
check_faults(const gen_t *g, operation_t operation)
{
	static const size_t short_reads[] = { 1, 7 };
	const fixture_spec_t defaults = { 0 }, spill = { .cache_pages = 1 };
	fsimg_error_t error;
	fsimg_status_t status;
	uint64_t n, failures;
	fixture_t f;
	listing_t l;
	model_t m;

	for (n = 1;; n++) {
		fixture_new(&f, g->image, g->length, &defaults);
		fsimg_test_fail_alloc_after(f.ctx, n);
		status = run_operation(&f, operation, false, NULL);
		fsimg_test_fail_alloc_after(f.ctx, 0);
		fixture_close(&f);
		if (status == FSIMG_OK)
			break;
		CHECK(status == FSIMG_ENOMEM);
		CHECK(n < 100000);
	}
	CHECK(n > 1);

	/* In a context whose single cache page makes every store spill. */
	for (unsigned point = FSIMG_META_OPEN; point <= FSIMG_META_WRITE;
	    point++) {
		failures = 0;
		for (n = 1;; n += n < 64 ? 1 : n / 8) {
			fixture_new(&f, g->image, g->length, &spill);
			fsimg_test_meta_fail_after(f.ctx, (fsimg_meta_fault_t)point,
			    n, ENOSPC);
			bzero(&error, sizeof(error));
			status = run_operation(&f, operation, false, &error);
			fsimg_test_meta_fail_after(f.ctx, (fsimg_meta_fault_t)point,
			    0, 0);
			fixture_close(&f);
			if (status == FSIMG_OK)
				break;
			CHECK(status == FSIMG_EIO && error.os_error == ENOSPC);
			failures++;
			CHECK(n < 100000);
		}
		CHECK(failures > 0);
	}

	model_build(&m, g->image, g->length);
	for (size_t i = 0; i < sizeof(short_reads) / sizeof(short_reads[0]);
	    i++) {
		fixture_new(&f, g->image, g->length, &defaults);
		f.short_max = short_reads[i];
		if (operation == OPERATION_LIST) {
			fixture_list(&f, false, &l);
			CHECK(listing_matches(&l, &m, "short reads"));
			listing_free(&l);
		} else {
			CHECK(run_operation(&f, operation, false, NULL) ==
			    FSIMG_OK);
		}
		fixture_close(&f);
	}
	model_free(&m);
}

/* The read calls of an operation, forced to ifs, one after another: the
 * n-th call fails, or makes no progress, for n from 1 until the operation
 * makes fewer calls. Every read site of open, replay, content and
 * verification thus fails once, and the operation ends with FSIMG_EIO at
 * that call's offset, an end of the source reported as EOF.
 */
static void
check_read_calls(const gen_t *g, operation_t operation)
{
	const fixture_spec_t defaults = { 0 };
	fsimg_error_t error;
	fsimg_status_t status;
	uint64_t n;
	fixture_t f;

	for (unsigned stall = 0; stall <= 1; stall++) {
		for (n = 1;; n++) {
			fixture_new(&f, g->image, g->length, &defaults);
			if (stall)
				f.stall_call = n;
			else
				f.fail_call = n;
			bzero(&error, sizeof(error));
			status = run_operation(&f, operation, true, &error);
			fixture_close(&f);
			if (status == FSIMG_OK) {
				CHECK(f.calls < n);
				break;
			}
			if (status != FSIMG_EIO || !error.offset_valid ||
			    error.offset != f.injected_offset)
				fprintf(stderr, "read call %" PRIu64 " (operation "
				    "%d): %d at %" PRIu64 " (%s), expected EIO at %"
				    PRIu64 "\n", n, (int)operation, status,
				    error.offset, error.message, f.injected_offset);
			CHECK(status == FSIMG_EIO);
			CHECK(error.offset_valid &&
			    error.offset == f.injected_offset);
			CHECK(!stall || strstr(error.message, "EOF") != NULL);
		}
		CHECK(n > 4);
	}
}

/* A read failure of [first, end), forced to ifs: the listing must end with
 * list_status and verification with verify_status.
 */
static void
check_read_failure(const gen_t *g, uint64_t first, uint64_t end,
    fsimg_status_t list_status, fsimg_status_t verify_status,
    const char *label)
{
	const fixture_spec_t defaults = { 0 };
	fsimg_error_t error;
	fsimg_status_t status, expected;
	fixture_t f;

	for (operation_t operation = OPERATION_LIST;
	    operation <= OPERATION_VERIFY; operation++) {
		expected = operation == OPERATION_LIST ? list_status :
		    verify_status;
		fixture_new(&f, g->image, g->length, &defaults);
		f.fail_first = first;
		f.fail_end = end;
		bzero(&error, sizeof(error));
		status = run_operation(&f, operation, true, &error);
		if (status != expected)
			fprintf(stderr, "%s (operation %d): %d (%s), expected %d\n",
			    label, (int)operation, status, error.message, expected);
		CHECK(status == expected);
		fixture_close(&f);
	}
}

/* When the source ends: during open, or after a successful open, during a
 * listing that reads every file or a verification with payload.
 */
typedef enum eof_stage {
	EOF_OPEN = 1,
	EOF_LIST,
	EOF_VERIFY
} eof_stage_t;

/* One end of the source: no read makes progress in [end, end + span),
 * which isolates one structure from those after it, from stage on, with
 * reads cut to short_max bytes when it is nonzero.
 */
typedef struct eof_case {
	const char	*label;
	uint64_t	 end;
	uint64_t	 span;
	eof_stage_t	 stage;
	size_t		 short_max;
} eof_case_t;

/* Make the source end as the case says. */
static void
eof_arm(fixture_t *f, const eof_case_t *c)
{
	f->eof_at = c->end;
	f->eof_end = c->end + c->span;
}

/* The read that reaches the case's end fails with an I/O error at exactly
 * that offset, whether the source ends before open or after it. The
 * offset is exact because image_read cuts a read that crosses end there,
 * and the bounded I/O layer's exact-read loop then asks for the rest from
 * end and reports the zero progress at that offset. So each case puts end
 * one byte past the start of a structure the operation reads from its
 * start, and span covers the rest of it.
 */
static void
check_eof(const gen_t *g, const eof_case_t *c)
{
	const fixture_spec_t defaults = { 0 };
	fsimg_read_options_t options;
	fsimg_verify_report_t report;
	fsimg_error_t error;
	fsimg_status_t status;
	listing_t l;
	fixture_t f;

	fixture_new(&f, g->image, g->length, &defaults);
	f.short_max = c->short_max;
	if (c->stage == EOF_OPEN)
		eof_arm(&f, c);
	fsimg_read_options_init(&options);
	options.format = "ifs";
	bzero(&error, sizeof(error));
	status = fsimg_reader_open(f.ctx, f.io, &options, &f.reader, &error);
	if (c->stage != EOF_OPEN) {
		CHECK(status == FSIMG_OK);
		eof_arm(&f, c);
	}
	if (c->stage == EOF_LIST) {
		bzero(&l, sizeof(l));
		listing_walk(&f, &l);
		status = l.status;
		error = l.error;
		listing_free(&l);
	} else if (c->stage == EOF_VERIFY) {
		status = fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD, &report,
		    &error);
		CHECK(!report.complete);
	}
	if (status != FSIMG_EIO || error.offset != c->end)
		fprintf(stderr, "%s: expected EIO at %" PRIu64 ", got %d at %"
		    PRIu64 " (%s)\n", c->label, c->end, status, error.offset,
		    error.message);
	CHECK(status == FSIMG_EIO && error.status == FSIMG_EIO);
	CHECK(error.offset_valid && error.offset == c->end);
	CHECK(strstr(error.message, "EOF") != NULL);
	fixture_close(&f);
}

/* The member at path in the generator's table, which must exist. */
static const gen_member_t *
gen_find(const gen_t *g, const char *path)
{
	for (size_t i = 0; i < g->count; i++)
		if (g->members[i].path_length == strlen(path) &&
		    memcmp(g->members[i].path, path, strlen(path)) == 0)
			return (&g->members[i]);
	CHECK(false);
	return (NULL);
}

static void
test_faults(void)
{
	static const size_t short_reads[] = { 0, 3 };
	const gen_member_t *inner, *up, *last;
	eof_case_t c;
	gen_t g;

	sample(&g);
	check_interrupts(&g, OPERATION_LIST);
	check_interrupts(&g, OPERATION_VERIFY);
	check_read_calls(&g, OPERATION_LIST);
	check_read_calls(&g, OPERATION_VERIFY);
	gen_free(&g);

	fault_image(&g);
	check_faults(&g, OPERATION_LIST);
	check_faults(&g, OPERATION_VERIFY);

	/* Where a failed read ends each operation: one of the mountpoint, a
	 * record or the terminator fails the open; of file content the
	 * listing, after open; of the trailer only verification, which also
	 * reads every byte the listing reads.
	 */
	inner = gen_find(&g, "dir/inner");
	check_read_failure(&g, H_MOUNTPOINT, H_MOUNTPOINT + 1, FSIMG_EIO,
	    FSIMG_EIO, "a mountpoint read failure");
	check_read_failure(&g, inner->table_offset, inner->table_offset + 4,
	    FSIMG_EIO, FSIMG_EIO, "a record read failure");
	check_read_failure(&g, g.hdr_dir_size - 4, g.hdr_dir_size, FSIMG_EIO,
	    FSIMG_EIO, "a terminator read failure");
	check_read_failure(&g, inner->data_offset, inner->data_offset + 1,
	    FSIMG_EIO, FSIMG_EIO, "a content read failure");
	check_read_failure(&g, g.image_size - 4, g.image_size, FSIMG_OK,
	    FSIMG_EIO, "a trailer read failure");

	/* The source ends inside each structure open, a listing or
	 * verification reads: the mountpoint, a record of the first and of a
	 * later table read, and the terminator at open; a record replay, a
	 * link's record and file content in a listing, also behind short
	 * reads; and the trailer in verification.
	 */
	up = gen_find(&g, "dir/up");
	last = &g.members[g.count - 1];
	c = (eof_case_t){
		.label = "EOF in the mountpoint",
		.end = H_MOUNTPOINT + 1,
		.span = g.dir_offset - H_MOUNTPOINT - 1,
		.stage = EOF_OPEN
	};
	check_eof(&g, &c);
	c = (eof_case_t){
		.label = "EOF in a record at open",
		.end = inner->table_offset + 1,
		.span = inner->length - 1,
		.stage = EOF_OPEN
	};
	check_eof(&g, &c);
	c = (eof_case_t){
		.label = "EOF in the table's last record at open",
		.end = last->table_offset + 1,
		.span = last->length - 1,
		.stage = EOF_OPEN
	};
	check_eof(&g, &c);
	c = (eof_case_t){
		.label = "EOF in the terminator",
		.end = g.hdr_dir_size - 4 + 1,
		.span = 3,
		.stage = EOF_OPEN
	};
	check_eof(&g, &c);
	for (size_t i = 0; i < sizeof(short_reads) / sizeof(short_reads[0]);
	    i++) {
		size_t short_max = short_reads[i];

		c = (eof_case_t){
			.label = "EOF in a record at replay",
			.end = inner->table_offset + 1,
			.span = inner->length - 1,
			.stage = EOF_LIST,
			.short_max = short_max
		};
		check_eof(&g, &c);
		c = (eof_case_t){
			.label = "EOF in a link's record at replay",
			.end = up->table_offset + 1,
			.span = up->length - 1,
			.stage = EOF_LIST,
			.short_max = short_max
		};
		check_eof(&g, &c);
		c = (eof_case_t){
			.label = "EOF in file content",
			.end = inner->data_offset + 1,
			.span = inner->bytes_length - 1,
			.stage = EOF_LIST,
			.short_max = short_max
		};
		check_eof(&g, &c);
	}
	c = (eof_case_t){
		.label = "EOF in the trailer",
		.end = g.image_size - 4 + 1,
		.span = 3,
		.stage = EOF_VERIFY
	};
	check_eof(&g, &c);
	gen_free(&g);
}

/*
 * Scale. Each case grows one dimension of an image: the body to 2^32 - 4
 * bytes, the record table to a million records, the root to a million
 * files, every path to the 1024-byte limit, or the depth to 512 levels,
 * the most a 1024-byte path holds. The image is never stored: scale_fill
 * synthesizes any range of it from its description, so neither the test
 * process nor the disk holds it, and the growth of the reading process's
 * resident set is the reader's own.
 *
 * Each case is listed, reading every file, and verified with payload, each
 * in a fresh context and a child process of its own. Ordinary runs use each
 * case's small size; --scale the large one, and prints the measurements.
 * The checks restate the driver design's resource bounds for these images:
 *
 * - the listing delivers every member, with its name and bytes, and every
 *   synthesized directory, and verification counts them all;
 * - charged memory stays below SCALE_MEMORY, and at the large sizes the
 *   child's resident set grows by at most SCALE_RSS_GROWTH, below a
 *   resident table, where that is measurable (see rss_measurable);
 * - scratch stays within a budget the context enforces, a small multiple
 *   of the table (see scale_scratch);
 * - the large record-table and root cases spill: their scratch peaks above
 *   the context's memory budget;
 * - open reads the table once, re-reading less than one record per table
 *   read; a listing then reads each delivered record once more, at
 *   replay, and each file's bytes once; verification also reads the whole
 *   image once more, for the checksum.
 */

/* The charged-memory ceiling, half the context's 16 MiB budget: the
 * reader's fixed state, about 3.1 MiB, and the metadata cache, which grows
 * to at most a quarter of the budget before stores spill to scratch.
 */
#define SCALE_MEMORY		(UINT64_C(8) << 20)
/* The most an operation may raise its process's peak resident set: below
 * the 44 MB table of a million records and the scratch its tree takes.
 */
#define SCALE_RSS_GROWTH	(UINT64_C(32) << 20)
/* The context's memory budget, which a spilled case's scratch exceeds. */
#define SCALE_BUDGET		(UINT64_C(16) << 20)
/* dir_offset for the mountpoint "/", and the root record. */
#define SCALE_DIR_OFFSET	0x5cu
#define SCALE_ROOT_RECORD	0x1cu
#define SCALE_PATH_MAX		1024u
/* A member's record: a file's fields and a path of up to 1024 bytes. */
#define SCALE_RECORD_MAX	(0x20u + SCALE_PATH_MAX + 4u)

/* An image of `members` files below an explicit root, little-endian with
 * the mountpoint "/": the header and the root record, then member i's
 * record, record_length bytes at first_record + i * record_length, the
 * terminator, zeros, the data and the trailer. Member i's path is
 * path_of(i), path_length bytes, and its data the four bytes at
 * data_base + 4 i, each a function of its offset (scale_data_byte); when
 * spanning is set member 0's range starts at hdr_dir_size instead, so the
 * zeros before data_base are its content. Every case names its members so
 * that component order is index order: the k-th file a listing delivers
 * is member k.
 */
struct scale_image {
	gen_t		 layout;	/* the header's fields; no members */
	uint64_t	 members;
	uint64_t	 implicit;	/* directories the table omits */
	size_t		 path_length;
	void		(*path_of)(const scale_image_t *, uint64_t, char *);
	bool		 spanning;
	uint32_t	 first_record;
	uint32_t	 record_length;
	uint32_t	 data_base;
	unsigned char	 head[SCALE_DIR_OFFSET + SCALE_ROOT_RECORD];
	unsigned char	 trailer[4];
};

/* The byte at image offset o of the data members own. */
static unsigned char
scale_data_byte(uint64_t o)
{
	return ((unsigned char)(0x5a ^ o ^ o >> 8 ^ o >> 16 ^ o >> 24));
}

/* Member i as the generator describes a member; path receives its path. */
static void
scale_member(const scale_image_t *s, uint64_t i, gen_member_t *m,
    char path[SCALE_PATH_MAX + 1])
{
	bzero(m, sizeof(*m));
	s->path_of(s, i, path);
	m->kind = GEN_FILE;
	m->path = (unsigned char *)path;
	m->path_length = s->path_length;
	m->mode = 0100644;
	m->mtime = GEN_MTIME;
	m->inode = (uint32_t)i + 2;
	m->table_offset = s->first_record + (uint32_t)i * s->record_length;
	m->length = s->record_length;
	m->data_offset = s->data_base + 4 * (uint32_t)i;
	m->bytes_length = 4;
	if (s->spanning && i == 0) {
		m->data_offset = s->layout.hdr_dir_size;
		m->bytes_length = s->data_base + 4 - s->layout.hdr_dir_size;
	}
}

/* Copy the part of [bytes_offset, bytes_offset + bytes_length) that lies
 * in [offset, offset + length) into buffer, which holds the latter.
 */
static void
copy_overlap(unsigned char *buffer, uint64_t offset, size_t length,
    const unsigned char *bytes, uint64_t bytes_offset, size_t bytes_length)
{
	uint64_t first, last;

	first = bytes_offset > offset ? bytes_offset : offset;
	last = bytes_offset + bytes_length < offset + length ?
	    bytes_offset + bytes_length : offset + length;
	if (first < last)
		memcpy(buffer + (first - offset), bytes + (first - bytes_offset),
		    (size_t)(last - first));
}

/* Synthesize [offset, offset + length) of the image. */
static void
scale_fill(const scale_image_t *s, uint64_t offset, unsigned char *buffer,
    size_t length)
{
	unsigned char record[SCALE_RECORD_MAX];
	char path[SCALE_PATH_MAX + 1];
	uint64_t end, table_end, data_end, first;
	gen_member_t m;

	end = offset + length;
	bzero(buffer, length);
	copy_overlap(buffer, offset, length, s->head, 0, s->first_record);
	table_end = s->first_record + s->members * s->record_length;
	if (offset < table_end && end > s->first_record) {
		first = offset > s->first_record ?
		    (offset - s->first_record) / s->record_length : 0;
		for (uint64_t i = first; i < s->members &&
		    s->first_record + i * s->record_length < end; i++) {
			scale_member(s, i, &m, path);
			bzero(record, s->record_length);
			gen_encode_record(&s->layout, &m, record);
			copy_overlap(buffer, offset, length, record,
			    m.table_offset, s->record_length);
		}
	}
	data_end = s->data_base + 4 * s->members;
	for (uint64_t o = offset > s->data_base ? offset : s->data_base;
	    o < end && o < data_end; o++)
		buffer[o - offset] = scale_data_byte(o);
	copy_overlap(buffer, offset, length, s->trailer,
	    s->layout.image_size - 4, 4);
}

/* The 32-bit word sum of [first, last), both multiples of 4. */
static uint32_t
scale_sum(const scale_image_t *s, uint64_t first, uint64_t last)
{
	static unsigned char window[65536];
	uint32_t sum = 0;
	size_t batch;

	for (uint64_t o = first; o < last; o += batch) {
		batch = last - o < sizeof(window) ? (size_t)(last - o) :
		    sizeof(window);
		scale_fill(s, o, window, batch);
		for (size_t i = 0; i < batch; i += 4)
			sum += get32(&s->layout, window + i);
	}
	return (sum);
}

/* Lay the image out once its members and paths are set, for image_size
 * bytes, or as few as the data needs when image_size is zero, and encode
 * the header, the root record and the trailer. The bytes between the
 * terminator and data_base are zero, so the checksum skips them.
 */
static void
scale_layout(scale_image_t *s, uint64_t image_size)
{
	gen_member_t root = {
		.kind = GEN_DIR,
		.path = (unsigned char *)"",
		.mode = 040755,
		.mtime = GEN_MTIME,
		.inode = 1,
		.length = SCALE_ROOT_RECORD
	};
	uint64_t hdr_dir_size, data_end;

	CHECK(s->path_length <= SCALE_PATH_MAX);
	gen_init(&s->layout);
	CHECK(round4(GEN_HEADER + s->layout.mountpoint_length + 1) ==
	    SCALE_DIR_OFFSET);
	CHECK(round4(gen_record_bytes(GEN_DIR, 0, 0)) == SCALE_ROOT_RECORD);
	s->layout.dir_offset = SCALE_DIR_OFFSET;
	s->first_record = SCALE_DIR_OFFSET + SCALE_ROOT_RECORD;
	s->record_length = round4(gen_record_bytes(GEN_FILE, s->path_length, 0));
	hdr_dir_size = s->first_record + s->members * s->record_length + 4;
	if (image_size == 0)
		image_size = hdr_dir_size + 4 * s->members + 4;
	CHECK(image_size % 4 == 0 && image_size <= UINT32_C(0xfffffffc) &&
	    hdr_dir_size + 4 * s->members + 4 <= image_size);
	s->layout.hdr_dir_size = (uint32_t)hdr_dir_size;
	s->layout.image_size = (uint32_t)image_size;
	s->data_base = (uint32_t)(image_size - 4 - 4 * s->members);
	data_end = s->data_base + 4 * s->members;

	bzero(s->head, sizeof(s->head));
	bzero(s->trailer, sizeof(s->trailer));
	gen_encode_header(&s->layout, s->head);
	gen_encode_record(&s->layout, &root, s->head + SCALE_DIR_OFFSET);
	put32(&s->layout, s->trailer, 0 - (scale_sum(s, 0, hdr_dir_size) +
	    scale_sum(s, s->data_base, data_end)));
}

/* "f0000000", "f0000001", ...: one component of eight bytes. */
static void
scale_flat_path(const scale_image_t *s FSIMG_UNUSED, uint64_t i, char *path)
{
	snprintf(path, SCALE_PATH_MAX + 1, "f%07" PRIu64, i);
}

/* "d000/f000" to "d999/f999": a thousand files in each implicit
 * directory.
 */
static void
scale_grouped_path(const scale_image_t *s FSIMG_UNUSED, uint64_t i,
    char *path)
{
	snprintf(path, SCALE_PATH_MAX + 1, "d%03" PRIu64 "/f%03" PRIu64,
	    i / 1000, i % 1000);
}

/* Five components of 204 bytes, 1024 in all: four implicit directories
 * of 'c' and a name of 'n' ending in the index's eight digits.
 */
static void
scale_long_path(const scale_image_t *s FSIMG_UNUSED, uint64_t i, char *path)
{
	char *p = path;

	for (unsigned c = 0; c < 4; c++) {
		memset(p, 'c', 204);
		p[204] = '/';
		p += 205;
	}
	memset(p, 'n', 196);
	snprintf(p + 196, 9, "%08" PRIu64, i);
}

/* "a/a/.../a/i": the index's digit below path_length / 2 directories. */
static void
scale_deep_path(const scale_image_t *s, uint64_t i, char *path)
{
	for (size_t o = 0; o + 1 < s->path_length; o += 2) {
		path[o] = 'a';
		path[o + 1] = '/';
	}
	path[s->path_length - 1] = (char)('0' + i);
}

/* The body grows to size bytes: a file spanning it, whose content is its
 * zeros, and a four-byte file whose range ends at the trailer.
 */
static void
scale_body(scale_image_t *s, uint64_t size)
{
	bzero(s, sizeof(*s));
	s->members = 2;
	s->path_length = 8;
	s->path_of = scale_flat_path;
	s->spanning = true;
	scale_layout(s, size);
}

/* The table grows to n records in implicit directories of a thousand. */
static void
scale_records(scale_image_t *s, uint64_t n)
{
	bzero(s, sizeof(*s));
	s->members = n;
	s->implicit = (n + 999) / 1000;
	s->path_length = 9;
	s->path_of = scale_grouped_path;
	scale_layout(s, 0);
}

/* The root widens to n files. */
static void
scale_width(scale_image_t *s, uint64_t n)
{
	bzero(s, sizeof(*s));
	s->members = n;
	s->path_length = 8;
	s->path_of = scale_flat_path;
	scale_layout(s, 0);
}

/* n records, each with a path of the 1024-byte limit. */
static void
scale_paths(scale_image_t *s, uint64_t n)
{
	bzero(s, sizeof(*s));
	s->members = n;
	s->implicit = 4;
	s->path_length = SCALE_PATH_MAX;
	s->path_of = scale_long_path;
	scale_layout(s, 0);
}

/* Ten files at depth levels, below levels - 1 implicit directories. */
static void
scale_depth(scale_image_t *s, uint64_t levels)
{
	bzero(s, sizeof(*s));
	s->members = 10;
	s->implicit = levels - 1;
	s->path_length = 2 * (levels - 1) + 1;
	s->path_of = scale_deep_path;
	scale_layout(s, 0);
}

/* The bytes [image_offset, image_offset + length) of a member's range
 * must be its data: zero before data_base, the pattern from there.
 */
static bool
scale_bytes_match(const scale_image_t *s, uint64_t image_offset,
    const unsigned char *bytes, size_t length)
{
	static const unsigned char zeros[65536];
	size_t i = 0;

	while (i < length && image_offset + i < s->data_base) {
		size_t run = length - i < sizeof(zeros) ? length - i :
		    sizeof(zeros);

		if (s->data_base - (image_offset + i) < run)
			run = (size_t)(s->data_base - (image_offset + i));
		if (memcmp(bytes + i, zeros, run) != 0)
			return (false);
		i += run;
	}
	for (; i < length; i++)
		if (bytes[i] != scale_data_byte(image_offset + i))
			return (false);
	return (true);
}

/* List every entry, requiring each file's name, size and bytes; *entries
 * counts files and directories.
 */
static fsimg_status_t
scale_list(fixture_t *f, const scale_image_t *s, uint64_t *entries,
    fsimg_error_t *error)
{
	static unsigned char buffer[65536];
	char path[SCALE_PATH_MAX + 1];
	const fsimg_entry_t *entry;
	uint64_t files, offset;
	gen_member_t m;
	size_t count;
	fsimg_status_t status;

	files = 0;
	for (;;) {
		status = fsimg_reader_next(f->reader, &entry, error);
		if (status == FSIMG_DONE)
			break;
		if (status != FSIMG_OK)
			return (status);
		(*entries)++;
		if (entry->kind == FSIMG_ENTRY_DIRECTORY)
			continue;
		CHECK(entry->kind == FSIMG_ENTRY_FILE && files < s->members);
		scale_member(s, files++, &m, path);
		CHECK(entry->path[0] == '/' &&
		    strlen(entry->path + 1) == s->path_length &&
		    memcmp(entry->path + 1, path, s->path_length) == 0);
		CHECK(entry->size == m.bytes_length);
		for (offset = 0; offset < entry->size; offset += count) {
			size_t want = entry->size - offset < sizeof(buffer) ?
			    (size_t)(entry->size - offset) : sizeof(buffer);

			status = fsimg_reader_read_at(f->reader, offset, buffer,
			    want, &count, error);
			if (status != FSIMG_OK)
				return (status);
			CHECK(count != 0 && count <= want);
			CHECK(scale_bytes_match(s, m.data_offset + offset,
			    buffer, count));
		}
	}
	CHECK(files == s->members);
	return (FSIMG_OK);
}

/* What one operation of a case did. */
typedef struct scale_run {
	read_account_t	 opening;	/* the reads of open */
	read_account_t	 reads;		/* every read of the operation */
	uint64_t	 entries;	/* listed, or files and directories verified */
	uint64_t	 io_calls;	/* image and scratch */
	uint64_t	 memory_peak;
	uint64_t	 scratch_peak;
	uint64_t	 rss;		/* the child's peak resident set */
	uint64_t	 rss_growth;	/* how far the operation raised it */
	double		 seconds;
} scale_run_t;

/* The scratch budget the context enforces: the path stream, its sorted
 * copy, the child stream, its sorted copy and the group table, of which
 * the sorts keep at most three at a time, hold a small multiple of the
 * table, as the driver design bounds them; common directory preparation
 * adds 64 bytes per entry, and 1 MiB covers store headers and partly
 * filled pages. A million eight-byte names peak at 3.6 times the table.
 */
static uint64_t
scale_scratch(const scale_image_t *s)
{
	return (4 * (uint64_t)s->layout.hdr_dir_size +
	    64 * (s->members + s->implicit) + (UINT64_C(1) << 20));
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

/* One operation over the image, in the calling process. */
static void
scale_operate(const scale_image_t *s, operation_t operation,
    uint32_t max_depth, scale_run_t *run)
{
	const fixture_spec_t spec = {
		.scale = s,
		.scratch_bytes = scale_scratch(s),
		.max_depth = max_depth
	};
	read_account_t account = {
		.table_start = s->layout.dir_offset,
		.data_start = s->layout.hdr_dir_size,
		.trailer_start = s->layout.image_size - 4
	};
	fsimg_verify_report_t report;
	struct timespec start, end;
	fsimg_stats_t stats;
	fsimg_error_t error;
	fsimg_status_t status;
	uint64_t baseline;
	fixture_t f;

	bzero(run, sizeof(*run));
	baseline = peak_rss();
	fixture_new(&f, NULL, s->layout.image_size, &spec);
	f.account = &account;
	bzero(&error, sizeof(error));
	CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
	status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, &error);
	run->opening = account;
	if (status == FSIMG_OK && operation == OPERATION_VERIFY) {
		status = fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD, &report,
		    &error);
		CHECK(report.complete == (status == FSIMG_OK));
		run->entries = report.files + report.directories;
		if (status == FSIMG_OK) {
			CHECK(report.files == s->members);
			CHECK(report.directories == s->implicit + 1);
			CHECK(report.allocated_clusters == s->layout.image_size);
		}
	} else if (status == FSIMG_OK) {
		status = scale_list(&f, s, &run->entries, &error);
	}
	CHECK(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
	if (status != FSIMG_OK)
		fprintf(stderr, "scale: %d %s\n", status, error.message);
	CHECK(status == FSIMG_OK);

	CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
	run->reads = account;
	run->io_calls = stats.io_calls;
	run->memory_peak = stats.memory_peak;
	run->scratch_peak = stats.scratch_peak;
	run->seconds = (double)(end.tv_sec - start.tv_sec) +
	    (double)(end.tv_nsec - start.tv_nsec) / 1e9;
	fixture_close(&f);
	run->rss = peak_rss();
	run->rss_growth = run->rss - baseline;
}

/* Run one operation in a child process and collect its measurements. A
 * failed check in the child exits it with status 1, which fails here.
 */
static void
scale_measure(const scale_image_t *s, operation_t operation,
    uint32_t max_depth, scale_run_t *run)
{
	int channel[2], status;
	pid_t child;

	CHECK(pipe(channel) == 0);
	CHECK(fflush(stdout) == 0);
	CHECK(fflush(stderr) == 0);
	child = fork();
	CHECK(child != -1);
	if (child == 0) {
		CHECK(close(channel[0]) == 0);
		scale_operate(s, operation, max_depth, run);
		CHECK(write(channel[1], run, sizeof(*run)) ==
		    (ssize_t)sizeof(*run));
		_exit(0);
	}
	CHECK(close(channel[1]) == 0);
	CHECK(waitpid(child, &status, 0) == child);
	if (WIFSIGNALED(status))
		fprintf(stderr, "scale: the child died of signal %d\n",
		    WTERMSIG(status));
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	CHECK(read(channel[0], run, sizeof(*run)) == (ssize_t)sizeof(*run));
	CHECK(close(channel[0]) == 0);
}

static void
scale_print(const char *name, uint64_t size, const char *what,
    const scale_image_t *s, const scale_run_t *run)
{
	uint64_t table = s->layout.hdr_dir_size - s->layout.dir_offset;
	const uint64_t *bytes = run->reads.region_bytes;

	printf("ifs scale %s=%" PRIu64 " %s entries=%" PRIu64 " table=%"
	    PRIu64 " image_calls=%" PRIu64 " image_read=%" PRIu64
	    " table_read=%" PRIu64 " table_passes=%.2f data_read=%" PRIu64
	    " io_calls=%" PRIu64 " memory_peak=%" PRIu64 " scratch_peak=%"
	    PRIu64 " rss=%" PRIu64 " rss_growth=%" PRIu64 " seconds=%.3f\n",
	    name, size, what, run->entries, table, run->reads.calls,
	    run->reads.bytes, bytes[REGION_TABLE],
	    (double)bytes[REGION_TABLE] / (double)table, bytes[REGION_DATA],
	    run->io_calls, run->memory_peak, run->scratch_peak, run->rss,
	    run->rss_growth, run->seconds);
}

/* The bounds every run of a case must hold; rss tells whether to check the
 * growth of the resident set (see rss_measurable).
 *
 * The open pass reads the table once in reads of several KiB. When a
 * record does not fit what a read left, the next read starts at that
 * record, so the bytes read again are less than one record per read; a
 * table read covering at least 4 KiB advances at least 4096 -
 * record_length bytes each time, and the first read and the terminator's
 * add two more. That ceiling restates the reader's access pattern, not a
 * design bound. Replay reads each file's record again, its two-byte length
 * and then the whole record.
 */
static void
scale_check(const scale_image_t *s, const scale_run_t *run,
    operation_t operation, bool rss)
{
	bool verify = operation == OPERATION_VERIFY;
	uint64_t table, data, logical, opened, walked, reread;

	table = s->layout.hdr_dir_size - s->layout.dir_offset;
	data = (uint64_t)s->layout.image_size - 4 - s->layout.hdr_dir_size;
	logical = 4 * s->members + (s->spanning ? s->data_base -
	    s->layout.hdr_dir_size : 0);
	opened = run->opening.region_bytes[REGION_TABLE];
	walked = run->reads.region_bytes[REGION_TABLE] - opened;
	CHECK(s->record_length < 4096);
	reread = (table / (4096 - s->record_length) + 2) * s->record_length;

	CHECK(run->entries == s->members + s->implicit + (verify ? 1 : 0));
	CHECK(run->memory_peak <= SCALE_MEMORY);
	CHECK(!rss || run->rss_growth <= SCALE_RSS_GROWTH);
	CHECK(opened >= table && opened <= table + reread);
	CHECK(run->opening.region_bytes[REGION_DATA] == 0);
	CHECK(walked == s->members * (2 + s->record_length) +
	    (verify ? table : 0));
	CHECK(run->reads.region_bytes[REGION_DATA] == logical +
	    (verify ? data : 0));
}

/* Whether the resident set measures the memory an operation holds, rather
 * than memory its allocator keeps after a free. Only the large sizes are
 * measured: check-scale runs them without sanitizers, whose allocators
 * keep freed memory resident in quarantine, and the small sizes stay
 * below the bound anyway. macOS's default allocator also keeps freed
 * blocks resident: a million-record listing grows the resident set by 18
 * to 73 MB from one run to the next at a 5.4 MB charged peak.
 * MallocSpaceEfficient=1, which check-scale sets, makes it return them,
 * and the growth stays below 8 MB.
 */
static bool
rss_measurable(bool large)
{
#if defined(__APPLE__)
	const char *efficient = getenv("MallocSpaceEfficient");

	if (efficient == NULL || strcmp(efficient, "1") != 0)
		return (false);
#endif
	return (large);
}

typedef struct scale_case {
	const char	*name;
	void		(*build)(scale_image_t *, uint64_t);
	uint64_t	 small;
	uint64_t	 large;
	bool		 deep;		/* the size is a depth past the default */
	bool		 spills;	/* the large size must spill */
} scale_case_t;

static void
test_scale(bool large)
{
	static const scale_case_t cases[] = {
		{
			.name = "body",
			.build = scale_body,
			.small = UINT64_C(1) << 20,
			.large = UINT64_C(0xfffffffc)
		}, {
			.name = "records",
			.build = scale_records,
			.small = 20000,
			.large = 1000000,
			.spills = true
		}, {
			.name = "width",
			.build = scale_width,
			.small = 20000,
			.large = 1000000,
			.spills = true
		}, {
			.name = "paths",
			.build = scale_paths,
			.small = 200,
			.large = 65536
		}, {
			.name = "depth",
			.build = scale_depth,
			.small = 64,
			.large = 512,
			.deep = true
		}
	};
	scale_image_t *s;
	scale_run_t list, verify;
	uint64_t size;
	uint32_t max_depth;
	bool rss;

	rss = rss_measurable(large);
	if (large && !rss)
		printf("SKIP: ifs scale RSS growth is not checked without "
		    "MallocSpaceEfficient=1\n");
	s = xmalloc(sizeof(*s));
	for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		size = large ? cases[c].large : cases[c].small;
		cases[c].build(s, size);
		max_depth = cases[c].deep ? (uint32_t)size : 0;
		scale_measure(s, OPERATION_LIST, max_depth, &list);
		scale_measure(s, OPERATION_VERIFY, max_depth, &verify);
		if (large) {
			scale_print(cases[c].name, size, "list", s, &list);
			scale_print(cases[c].name, size, "verify", s, &verify);
		}
		scale_check(s, &list, OPERATION_LIST, rss);
		scale_check(s, &verify, OPERATION_VERIFY, rss);
		if (large && cases[c].spills)
			CHECK(list.scratch_peak > SCALE_BUDGET &&
			    verify.scratch_peak > SCALE_BUDGET);
	}
	free(s);
	CHECK(fflush(stdout) == 0);
}

/*
 * Native images. FSIMG_IFS_NATIVE names the captures directory: every
 * bare ImageFS in its cases/ subdirectories and in decoded/ is compared
 * with the model. Only counts and image file names are printed, never a
 * member name.
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
	bytes = xmalloc(*length);
	for (size_t done = 0; done < *length; done += (size_t)got) {
		got = read(fd, bytes + done, *length - done);
		CHECK(got > 0);
	}
	CHECK(close(fd) == 0);
	return (bytes);
}

/* What the native images did. Every image the reader opens is verified;
 * verified counts those that pass, rejected those that fail with the
 * model's class.
 */
typedef struct native_counts {
	unsigned	 images;
	unsigned	 opened;
	unsigned	 unsupported;
	unsigned	 corrupt;
	unsigned	 skipped;
	unsigned	 mismatched;
	unsigned	 verified;
	unsigned	 rejected;
} native_counts_t;

/* Compare every ImageFS in directory with the model. In a group of
 * mutations, the E10 copies, every image but the one whose name holds
 * "unchanged" must fail verification; in any other group every image that
 * opens must verify.
 */
static void
native_directory(const char *directory, bool mutations,
    native_counts_t *counts)
{
	struct dirent *entry;
	DIR *dir;

	dir = opendir(directory);
	if (dir == NULL)
		return;
	while ((entry = readdir(dir)) != NULL) {
		char path[4096];
		unsigned char *image;
		size_t length;
		listing_t l;
		model_t m;
		bool verifies, should_verify;

		if (entry->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
		image = read_file(path, &length);
		if (length < 7 || memcmp(image, "imagefs", 7) != 0) {
			counts->skipped++;
			free(image);
			continue;
		}
		counts->images++;
		model_build(&m, image, length);
		list_image(image, length, 600, &l);
		verifies = model_verify(&m) == FSIMG_OK;
		should_verify = mutations ?
		    strstr(entry->d_name, "unchanged") != NULL :
		    m.status == FSIMG_OK;
		if (!listing_matches(&l, &m, entry->d_name) ||
		    (m.status == FSIMG_OK &&
		    !verify_matches(image, length, &m, entry->d_name)))
			counts->mismatched++;
		else if (verifies != should_verify) {
			fprintf(stderr, "%s: verification %s\n", entry->d_name,
			    verifies ? "passes" : "fails");
			counts->mismatched++;
		} else if (l.status == FSIMG_OK)
			counts->opened++;
		else if (l.status == FSIMG_EUNSUPPORTED)
			counts->unsupported++;
		else
			counts->corrupt++;
		if (verifies)
			counts->verified++;
		else
			counts->rejected++;
		listing_free(&l);
		model_free(&m);
		free(image);
	}
	CHECK(closedir(dir) == 0);
}

static int
native_run(void)
{
	static const struct {
		const char	*path;
		bool		 mutations;
	} groups[] = {
		{ "cases/ifs-basic", false }, { "cases/ifs-names", false },
		{ "cases/ifs-extra", false }, { "cases/ifs-data", false },
		{ "cases/ifs-e10-sense", true }, { "decoded", false }
	};
	const char *root = getenv("FSIMG_IFS_NATIVE");
	native_counts_t counts;
	char directory[1024];

	if (root == NULL || root[0] == '\0') {
		printf("SKIP: FSIMG_IFS_NATIVE is not set\n");
		return (77);
	}
	bzero(&counts, sizeof(counts));
	for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
		snprintf(directory, sizeof(directory), "%s/%s", root,
		    groups[i].path);
		native_directory(directory, groups[i].mutations, &counts);
	}
	printf("native: %u images, %u opened, %u unsupported, %u corrupt, "
	    "%u mismatched; %u verified, %u rejected; %u files without an "
	    "ImageFS at offset 0\n", counts.images, counts.opened,
	    counts.unsupported, counts.corrupt, counts.mismatched,
	    counts.verified, counts.rejected, counts.skipped);
	return (counts.mismatched == 0 && counts.images != 0 ? 0 : 1);
}

/*
 * Command modes for the fuzzing seed and test/ifs-integration.sh.
 */

/* An image of shadowed records and implicit parents: a duplicate file
 * path, records below a file and below a link, a link record shadowing a
 * directory record with a child, and a file below two implicit parents.
 */
static void
shadowed_image(gen_t *g)
{
	gen_init(g);
	gen_root(g);
	gen_file(g, "twice", "first");
	gen_file(g, "twice", "second");
	gen_file(g, "file", "f");
	gen_file(g, "file/below", "b");
	gen_link(g, "link", "file");
	gen_file(g, "link/below", "b");
	gen_dir(g, "dir");
	gen_file(g, "dir/child", "c");
	gen_link(g, "dir", "file");
	gen_fifo(g, "pipe");
	gen_file(g, "p/q/r", "deep");
	gen_finish(g);
}

static void
write_image(const char *path, const gen_t *g)
{
	FILE *out;

	out = fopen(path, "wb");
	CHECK(out != NULL);
	CHECK(fwrite(g->image, 1, g->length, out) == g->length);
	CHECK(fclose(out) == 0);
}

/* POSIX cksum(1): CRC-32 with polynomial 0x04C11DB7, most significant bit
 * first; cksum_finish appends the length in as few bytes as hold it, least
 * significant first, and complements the register.
 */
static uint32_t
cksum_update(uint32_t crc, const unsigned char *bytes, size_t length)
{
	for (size_t i = 0; i < length; i++) {
		crc ^= (uint32_t)bytes[i] << 24;
		for (unsigned bit = 0; bit < 8; bit++)
			crc = (crc & 0x80000000u) != 0 ?
			    (crc << 1) ^ 0x04c11db7u : crc << 1;
	}
	return (crc);
}

static uint32_t
cksum_finish(uint32_t crc, uint64_t length)
{
	unsigned char byte;

	for (; length != 0; length >>= 8) {
		byte = (unsigned char)length;
		crc = cksum_update(crc, &byte, 1);
	}
	return (~crc);
}

/* The content read by manifest_file and manifest_link. */
static unsigned char manifest_buffer[65536];

/* Print a file's manifest line, once all of its bytes are read. */
static fsimg_status_t
manifest_file(fsimg_reader_t *reader, const fsimg_entry_t *entry)
{
	fsimg_status_t status;
	uint64_t offset;
	uint32_t crc;
	size_t count;

	crc = 0;
	for (offset = 0; offset < entry->size; offset += count) {
		size_t want = entry->size - offset < sizeof(manifest_buffer) ?
		    (size_t)(entry->size - offset) : sizeof(manifest_buffer);

		status = fsimg_reader_read_at(reader, offset, manifest_buffer,
		    want, &count, NULL);
		if (status != FSIMG_OK)
			return (status);
		CHECK(count != 0 && count <= want);
		crc = cksum_update(crc, manifest_buffer, count);
	}
	printf("F\t%s\t%" PRIu32 "\t%" PRIu64 "\n", entry->path,
	    cksum_finish(crc, entry->size), entry->size);
	return (FSIMG_OK);
}

/* Print a link's manifest line, once its whole target is read. */
static fsimg_status_t
manifest_link(fsimg_reader_t *reader, const fsimg_entry_t *entry)
{
	fsimg_status_t status;
	size_t count, done;

	CHECK(entry->size <= sizeof(manifest_buffer));
	for (done = 0; done < entry->size; done += count) {
		status = fsimg_reader_read_at(reader, done,
		    manifest_buffer + done, (size_t)entry->size - done, &count,
		    NULL);
		if (status != FSIMG_OK)
			return (status);
		CHECK(count != 0 && count <= entry->size - done);
	}
	printf("L\t%s\t", entry->path);
	for (size_t i = 0; i < done; i++)
		printf("%02x", manifest_buffer[i]);
	printf("\n");
	return (FSIMG_OK);
}

/*
 * Print the content of every file and symbolic link of the ImageFS in the
 * image file at path, bare or in a container, which the image layers peel
 * with a spool in the scratch directory, for test/ifs-native.rb to compare
 * with a decoder's manifest: "F\tPATH\tCKSUM\tSIZE" for a file, with its
 * POSIX cksum, and "L\tPATH\tTARGET" for a link, the target in
 * hexadecimal, since it may hold any byte. A delivered path is inside the
 * name profile, so it holds no tab or newline. The names travel through
 * the pipe, never through a command line; diagnostics give the status
 * alone.
 */
static int
manifest(const char *path)
{
	fsimg_selector_t automatic = { .kind = FSIMG_SELECT_AUTOMATIC };
	fsimg_image_options_t image_options;
	fsimg_options_t options;
	fsimg_ctx_t *ctx;
	fsimg_image_t *image;
	fsimg_io_t *io, *volume;
	fsimg_reader_t *reader;
	const fsimg_entry_t *entry;
	fsimg_status_t status;
	char directory[256];
	const char *tmp = getenv("TMPDIR");
	int fd, scratch;

	CHECK((size_t)snprintf(directory, sizeof(directory),
	    "%s/fsimg-ifs-XXXXXX", tmp != NULL && tmp[0] != '\0' ? tmp :
	    "/tmp") < sizeof(directory));
	CHECK(mkdtemp(directory) != NULL);
	scratch = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(scratch != -1);
	fsimg_options_init(&options);
	options.memory_bytes = 64 * 1024 * 1024;
	options.scratch_directory_fd = scratch;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	CHECK(fd != -1);
	CHECK(fsimg_io_from_fd(ctx, fd, FSIMG_IO_READ, &io, NULL) == FSIMG_OK);
	fsimg_image_options_init(&image_options);
	image_options.spool_fd = openat(scratch, "spool", O_RDWR | O_CREAT |
	    O_EXCL | O_CLOEXEC, 0600);
	CHECK(image_options.spool_fd != -1);
	CHECK(unlinkat(scratch, "spool", 0) == 0);
	image = NULL;
	volume = NULL;
	reader = NULL;
	status = fsimg_image_open(ctx, io, &image_options, &image, NULL);
	if (status == FSIMG_OK)
		status = fsimg_image_select(image, &automatic, &volume, NULL);
	if (status == FSIMG_OK)
		status = fsimg_reader_open(ctx, volume, NULL, &reader, NULL);
	while (status == FSIMG_OK) {
		status = fsimg_reader_next(reader, &entry, NULL);
		if (status != FSIMG_OK)
			break;
		if (entry->kind == FSIMG_ENTRY_FILE)
			status = manifest_file(reader, entry);
		else if (entry->kind == FSIMG_ENTRY_SYMLINK)
			status = manifest_link(reader, entry);
	}
	if (status == FSIMG_DONE)
		status = FSIMG_OK;
	if (status != FSIMG_OK)
		fprintf(stderr, "manifest: status %d\n", status);
	fsimg_reader_free(reader);
	fsimg_io_free(volume);
	fsimg_image_free(image);
	fsimg_io_free(io);
	fsimg_ctx_free(ctx);
	CHECK(close(scratch) == 0);
	CHECK(rmdir(directory) == 0);
	CHECK(fflush(stdout) == 0);
	return (status == FSIMG_OK ? 0 : 1);
}

int
main(int argc, char **argv)
{
	gen_t g;

	if (argc == 3 && strcmp(argv[1], "--seed") == 0) {
		omitted_image(&g);
		write_image(argv[2], &g);
		gen_free(&g);
		return (0);
	}
	if (argc == 3 && strcmp(argv[1], "--seed-shadowed") == 0) {
		shadowed_image(&g);
		write_image(argv[2], &g);
		gen_free(&g);
		return (0);
	}
	if (argc == 3 && strcmp(argv[1], "--manifest") == 0)
		return (manifest(argv[2]));
	if (argc == 2 && strcmp(argv[1], "--native") == 0)
		return (native_run());
	if (argc == 2 && strcmp(argv[1], "--scale") == 0) {
		test_scale(true);
		return (0);
	}
	CHECK(argc == 1);
	test_sample();
	test_probe();
	test_header_bounds();
	test_mountpoint();
	test_records();
	test_inode_flags();
	test_hierarchy();
	test_name_lengths();
	test_omitted_names();
	test_ranges();
	test_links();
	test_metadata();
	test_references();
	test_verify();
	test_long_records();
	test_random();
	test_extract();
	test_faults();
	test_scale(false);
	return (0);
}
