/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "erofs-gen.h"
#include "test.h"

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
	SB_BUILD_TIME = 0x6c,
	SB_WIDE_ROOT = 0x70,
	SB_BOX_NID = 0x80
};

/* The SB_CHKSUM compatible bit and the metadata start of a default volume. */
enum {
	COMPAT_SB_CHKSUM = 1,
	DEFAULT_META_BYTES = 4096
};

/*
 * CRC-32C from its definition: the reflected Castagnoli polynomial
 * 0x82F63B78, bit by bit, the register seeded with all ones and returned
 * without the final inversion, which is the form the superblock stores.
 */
uint32_t
gen_crc32c_register(const unsigned char *p, size_t n)
{
	uint32_t reg = UINT32_C(0xffffffff);

	for (size_t i = 0; i < n; i++) {
		reg ^= p[i];
		for (int bit = 0; bit < 8; bit++)
			reg = (reg & 1) != 0 ?
			    (reg >> 1) ^ UINT32_C(0x82f63b78) : reg >> 1;
	}
	return (reg);
}

static void
put_le(unsigned char *p, uint64_t value, unsigned width)
{
	for (unsigned i = 0; i < width; i++)
		p[i] = (unsigned char)(value >> (8 * i));
}

void
gen_image_init(gen_image_t *image, size_t length)
{
	image->bytes = calloc(length != 0 ? length : 1, 1);
	CHECK(image->bytes != NULL);
	image->length = length;
	image->meta_base = 0;
}

void
gen_image_free(gen_image_t *image)
{
	free(image->bytes);
	image->bytes = NULL;
}

void
gen_put(gen_image_t *image, uint64_t offset, uint64_t value, unsigned width)
{
	CHECK(offset <= image->length && width <= image->length - offset);
	put_le(image->bytes + offset, value, width);
}

void
gen_put_bytes(gen_image_t *image, uint64_t offset, const void *bytes,
    size_t length)
{
	CHECK(offset <= image->length && length <= image->length - offset);
	bcopy(bytes, image->bytes + offset, length);
}

void
gen_super_put(gen_image_t *image, unsigned field, uint64_t value,
    unsigned width)
{
	gen_put(image, GEN_SUPER_OFFSET + field, value, width);
}

gen_super_t
gen_super_default(unsigned blkszbits, uint32_t blocks)
{
	gen_super_t sb;

	bzero(&sb, sizeof(sb));
	sb.magic = UINT32_C(0xe0f5e1e2);
	sb.compat = COMPAT_SB_CHKSUM;
	sb.blkszbits = (uint8_t)blkszbits;
	sb.blocks = blocks;
	sb.meta_blkaddr = DEFAULT_META_BYTES >> blkszbits;
	sb.inode_count = 1;
	return (sb);
}

void
gen_super_write(gen_image_t *image, const gen_super_t *sb)
{
	gen_super_put(image, SB_MAGIC, sb->magic, 4);
	gen_super_put(image, SB_CHECKSUM, sb->checksum, 4);
	gen_super_put(image, SB_COMPAT, sb->compat, 4);
	gen_super_put(image, SB_BLKSZBITS, sb->blkszbits, 1);
	gen_super_put(image, SB_SLOTS, sb->slots, 1);
	gen_super_put(image, SB_ROOT16, sb->root_nid, 2);
	gen_super_put(image, SB_INODES, sb->inode_count, 8);
	gen_super_put(image, SB_EPOCH, (uint64_t)sb->epoch, 8);
	gen_super_put(image, SB_NSEC, sb->epoch_nsec, 4);
	gen_super_put(image, SB_BLOCKS, sb->blocks, 4);
	gen_super_put(image, SB_META_BLOCK, sb->meta_blkaddr, 4);
	gen_super_put(image, SB_XATTR_BLOCK, sb->xattr_blkaddr, 4);
	gen_put_bytes(image, GEN_SUPER_OFFSET + SB_UUID, sb->uuid,
	    sizeof(sb->uuid));
	gen_put_bytes(image, GEN_SUPER_OFFSET + SB_NAME, sb->name,
	    sizeof(sb->name));
	gen_super_put(image, SB_INCOMPAT, sb->incompat, 4);
	gen_super_put(image, SB_WORD54, sb->compression_word, 2);
	gen_super_put(image, SB_DEVICES, sb->devices, 2);
	gen_super_put(image, SB_DIRBLKBITS, sb->dirblkbits, 1);
	gen_super_put(image, SB_PREFIX_COUNT, sb->prefix_count, 1);
	gen_super_put(image, SB_PREFIX_START, sb->prefix_start, 4);
	gen_super_put(image, SB_PACKED_NID, sb->packed_nid, 8);
	gen_super_put(image, SB_BUILD_TIME, sb->build_time, 4);
	if (sb->box_nid != 0)
		gen_super_put(image, SB_BOX_NID, sb->box_nid, 8);
	image->meta_base = (uint64_t)sb->meta_blkaddr << sb->blkszbits;
}

gen_inode_t
gen_root_default(void)
{
	gen_inode_t root;

	bzero(&root, sizeof(root));
	root.mode = 040755;
	root.nlink = 2;
	root.layout = 2;	/* flat with an inline tail */
	root.size = 24;
	return (root);
}

uint64_t
gen_inode_offset(const gen_image_t *image, uint64_t nid)
{
	if ((nid & GEN_NID_BOX) != 0) {
		CHECK(image->box_base != 0);
		return (image->box_base + 32 * (nid & ~GEN_NID_BOX));
	}
	return (image->meta_base + 32 * nid);
}

/* The attribute region is 0 bytes for a count of 0, else 12 + 4 * (c - 1). */
size_t
gen_inode_header_bytes(const gen_inode_t *inode)
{
	size_t fixed = inode->extended ? GEN_EXTENDED_BYTES : GEN_COMPACT_BYTES;

	if (inode->xattr_count == 0)
		return (fixed);
	return (fixed + 12 + 4 * (size_t)(inode->xattr_count - 1));
}

void
gen_inode_write(gen_image_t *image, const gen_inode_t *inode)
{
	uint64_t at = gen_inode_offset(image, inode->nid);
	uint16_t format = (uint16_t)((inode->extended ? 1 : 0) |
	    (inode->layout << 1) | inode->format_extra);

	gen_put(image, at + 0x00, format, 2);
	gen_put(image, at + 0x02, inode->xattr_count, 2);
	gen_put(image, at + 0x04, inode->mode, 2);
	if (!inode->extended) {
		gen_put(image, at + 0x06, inode->nlink, 2);
		gen_put(image, at + 0x08, inode->size, 4);
		gen_put(image, at + 0x0c, (uint64_t)inode->time, 4);
		gen_put(image, at + 0x10, inode->layout_word, 4);
		gen_put(image, at + 0x14, inode->serial, 4);
		gen_put(image, at + 0x18, inode->uid, 2);
		gen_put(image, at + 0x1a, inode->gid, 2);
		return;
	}
	gen_put(image, at + 0x08, inode->size, 8);
	gen_put(image, at + 0x10, inode->layout_word, 4);
	gen_put(image, at + 0x14, inode->serial, 4);
	gen_put(image, at + 0x18, inode->uid, 4);
	gen_put(image, at + 0x1c, inode->gid, 4);
	gen_put(image, at + 0x20, (uint64_t)inode->time, 8);
	gen_put(image, at + 0x28, inode->time_nsec, 4);
	gen_put(image, at + 0x2c, inode->nlink, 4);
}

void
gen_seal(gen_image_t *image)
{
	unsigned bits = image->bytes[GEN_SUPER_OFFSET + SB_BLKSZBITS];
	size_t block, covered;

	CHECK(bits >= 9 && bits <= 12);
	block = (size_t)1 << bits;
	covered = block > 1024 ? block - 1024 : block;
	CHECK(GEN_SUPER_OFFSET + covered <= image->length);
	gen_put(image, GEN_SUPER_OFFSET + SB_CHECKSUM, 0, 4);
	gen_put(image, GEN_SUPER_OFFSET + SB_CHECKSUM,
	    gen_crc32c_register(image->bytes + GEN_SUPER_OFFSET, covered), 4);
}

size_t
gen_dir_pack(unsigned char *out, size_t capacity, size_t block,
    const gen_dirent_t *entries, size_t count)
{
	size_t at = 0, used = 0, first = 0;

	while (first < count) {
		size_t n = 0, names = 0, name_at;

		/* Take records while their array and names fit one block. */
		while (first + n < count) {
			const gen_dirent_t *e = &entries[first + n];
			size_t length = strlen(e->name);

			if (n > 0 && (e->type & GEN_STARTS_BLOCK) != 0)
				break;
			if (12 * (n + 1) + names + length > block)
				break;
			names += length;
			n++;
		}
		CHECK(n > 0);
		CHECK(at + block <= capacity);
		name_at = 12 * n;
		for (size_t i = 0; i < n; i++) {
			const gen_dirent_t *e = &entries[first + i];
			unsigned char *record = out + at + 12 * i;
			size_t length = strlen(e->name);

			put_le(record, e->nid, 8);
			put_le(record + 8, name_at, 2);
			record[10] = (unsigned char)e->type;
			bcopy(e->name, out + at + name_at, length);
			name_at += length;
		}
		used = name_at;
		first += n;
		if (first < count)
			at += block;
	}
	return (at + used);
}

uint32_t
gen_tree_first_block(unsigned blkszbits)
{
	return ((uint32_t)(DEFAULT_META_BYTES + GEN_TREE_META_BYTES) >>
	    blkszbits);
}

uint64_t
gen_nid_at(const gen_image_t *image, uint64_t offset)
{
	if (image->box_base != 0 && offset >= image->box_base &&
	    offset < image->box_end) {
		CHECK((offset - image->box_base) % 32 == 0);
		return ((offset - image->box_base) / 32 | GEN_NID_BOX);
	}
	CHECK(offset >= image->meta_base && (offset - image->meta_base) % 32 == 0);
	return ((offset - image->meta_base) / 32);
}

void
gen_tree_init(gen_tree_t *tree, unsigned blkszbits, uint32_t blocks)
{
	gen_super_t sb = gen_super_default(blkszbits, blocks);

	CHECK(blocks >= gen_tree_first_block(blkszbits));
	bzero(tree, sizeof(*tree));
	gen_image_init(&tree->image, (size_t)blocks << blkszbits);
	gen_super_write(&tree->image, &sb);
	tree->blkszbits = blkszbits;
	tree->blocks = blocks;
	tree->meta_next = DEFAULT_META_BYTES;
	tree->data_next = gen_tree_first_block(blkszbits);
	CHECK(gen_tree_slot(tree, GEN_COMPACT_BYTES, 0) == 0);
}

void
gen_tree_free(gen_tree_t *tree)
{
	gen_image_free(&tree->image);
}

void
gen_tree_set_blocks(gen_tree_t *tree, uint32_t blocks)
{
	gen_super_put(&tree->image, SB_BLOCKS, blocks, 4);
}

void
gen_tree_set_compat(gen_tree_t *tree, uint32_t compat)
{
	gen_super_put(&tree->image, SB_COMPAT, compat, 4);
}

void
gen_tree_seal(gen_tree_t *tree)
{
	gen_seal(&tree->image);
}

uint64_t
gen_tree_slot(gen_tree_t *tree, size_t header, size_t tail)
{
	uint64_t block = (uint64_t)1 << tree->blkszbits;
	uint64_t at = tree->meta_next;
	uint64_t end;

	CHECK(tail < block);
	if (tail > 0 && (at + header) / block !=
	    (at + header + tail - 1) / block) {
		/* Start the tail at the next block, the header just before. */
		uint64_t boundary = (at + header) / block * block + block;

		at = (boundary - header + 31) & ~(uint64_t)31;
		CHECK((at + header) / block == (at + header + tail - 1) / block);
	}
	end = (at + header + tail + 31) & ~(uint64_t)31;
	CHECK(end <= DEFAULT_META_BYTES + GEN_TREE_META_BYTES);
	tree->meta_next = end;
	return (gen_nid_at(&tree->image, at));
}

void
gen_tree_fill(gen_tree_t *tree, uint64_t nid, const gen_inode_t *proto,
    const void *content, size_t size)
{
	size_t block = (size_t)1 << tree->blkszbits;
	size_t stored = proto->layout == 2 ? size / block * block : size;
	gen_inode_t inode = *proto;

	CHECK(proto->layout == 0 || proto->layout == 2);
	if (stored > 0) {
		size_t blocks = (stored + block - 1) / block;

		CHECK(tree->data_next + blocks <= tree->blocks);
		inode.layout_word = tree->data_next;
		gen_put_bytes(&tree->image, (uint64_t)tree->data_next * block,
		    content, stored);
		tree->data_next += (uint32_t)blocks;
	}
	if (size > stored) {
		gen_put_bytes(&tree->image,
		    gen_inode_offset(&tree->image, nid) +
		    gen_inode_header_bytes(proto),
		    (const unsigned char *)content + stored, size - stored);
	}
	inode.nid = nid;
	inode.size = size;
	gen_inode_write(&tree->image, &inode);
}

uint64_t
gen_tree_add(gen_tree_t *tree, const gen_inode_t *proto, const void *content,
    size_t size)
{
	size_t block = (size_t)1 << tree->blkszbits;
	size_t tail = proto->layout == 2 ? size & (block - 1) : 0;
	uint64_t nid = gen_tree_slot(tree, gen_inode_header_bytes(proto), tail);

	gen_tree_fill(tree, nid, proto, content, size);
	return (nid);
}

void
gen_tree_set_incompat(gen_tree_t *tree, uint32_t incompat)
{
	gen_super_put(&tree->image, SB_INCOMPAT, incompat, 4);
}

void
gen_tree_set_packed(gen_tree_t *tree, uint64_t nid)
{
	gen_super_put(&tree->image, SB_PACKED_NID, nid, 8);
}

uint32_t
gen_tree_data(gen_tree_t *tree, const void *content, size_t size)
{
	size_t block = (size_t)1 << tree->blkszbits;
	size_t blocks = (size + block - 1) / block;
	uint32_t first = tree->data_next;

	CHECK(blocks > 0 && first + blocks <= tree->blocks);
	gen_put_bytes(&tree->image, (uint64_t)first * block, content, size);
	tree->data_next += (uint32_t)blocks;
	return (first);
}

gen_chunk_t
gen_chunk_block(uint64_t block)
{
	CHECK(block < ((uint64_t)1 << 48));
	return ((gen_chunk_t){ .block = (uint32_t)block,
	    .high = (uint16_t)(block >> 32) });
}

gen_chunk_t
gen_chunk_hole(uint16_t format)
{
	return ((gen_chunk_t){ .block = UINT32_MAX,
	    .high = (format & GEN_CHUNK_WIDE) != 0 ? UINT16_MAX : 0 });
}

/* The index starts at the attribute region's end rounded up to the width. */
static size_t
chunk_index_start(const gen_inode_t *proto, uint16_t format)
{
	size_t width = (format & GEN_CHUNK_INDEXES) != 0 ? 8 : 4;

	return ((gen_inode_header_bytes(proto) + width - 1) / width * width);
}

void
gen_chunk_put(gen_tree_t *tree, uint64_t nid, const gen_inode_t *proto,
    uint16_t format, size_t index, gen_chunk_t chunk)
{
	size_t width = (format & GEN_CHUNK_INDEXES) != 0 ? 8 : 4;
	uint64_t at = gen_inode_offset(&tree->image, nid) +
	    chunk_index_start(proto, format) + index * width;

	if (width == 4) {
		gen_put(&tree->image, at, chunk.block, 4);
		return;
	}
	gen_put(&tree->image, at, chunk.high, 2);
	gen_put(&tree->image, at + 2, chunk.device, 2);
	gen_put(&tree->image, at + 4, chunk.block, 4);
}

uint64_t
gen_tree_add_chunked(gen_tree_t *tree, const gen_inode_t *proto,
    uint32_t layout_word, const gen_chunk_t *chunks, size_t count)
{
	uint16_t format = (uint16_t)layout_word;
	size_t width = (format & GEN_CHUNK_INDEXES) != 0 ? 8 : 4;
	size_t header = gen_inode_header_bytes(proto);
	size_t start = chunk_index_start(proto, format);
	uint64_t nid = gen_tree_slot(tree, header, start - header + count * width);
	gen_inode_t inode = *proto;

	inode.nid = nid;
	inode.layout = 4;
	inode.layout_word = layout_word;
	gen_inode_write(&tree->image, &inode);
	for (size_t i = 0; i < count; i++)
		gen_chunk_put(tree, nid, proto, format, i, chunks[i]);
	return (nid);
}

static int
dirent_compare(const void *a, const void *b)
{
	const char *x = ((const gen_dirent_t *)a)->name;
	const char *y = ((const gen_dirent_t *)b)->name;

	return (strcmp(x, y));
}

/* The children with "." and ".." added, sorted by name bytes. */
static gen_dirent_t *
dir_records(uint64_t nid, uint64_t parent, const gen_dirent_t *children,
    size_t count)
{
	gen_dirent_t *all = calloc(count + 2, sizeof(*all));

	CHECK(all != NULL);
	all[0] = (gen_dirent_t){ ".", nid, 2 };
	all[1] = (gen_dirent_t){ "..", parent, 2 };
	if (count > 0)
		bcopy(children, all + 2, count * sizeof(*all));
	qsort(all, count + 2, sizeof(*all), dirent_compare);
	return (all);
}

void
gen_tree_dir(gen_tree_t *tree, uint64_t nid, uint64_t parent,
    const gen_dirent_t *children, size_t count)
{
	size_t block = (size_t)1 << tree->blkszbits;
	gen_dirent_t *all = dir_records(nid, parent, children, count);
	/* Every record fits one block, so a block holds at least one. */
	size_t capacity = (count + 2) * block;
	unsigned char *blocks = calloc(capacity, 1);
	gen_inode_t dir;
	size_t size;

	CHECK(blocks != NULL);
	size = gen_dir_pack(blocks, capacity, block, all, count + 2);
	bzero(&dir, sizeof(dir));
	dir.mode = 040755;
	dir.nlink = 2;
	gen_tree_fill(tree, nid, &dir, blocks, size);
	free(blocks);
	free(all);
}

void
gen_tree_set_root(gen_tree_t *tree, uint64_t nid)
{
	gen_super_put(&tree->image, SB_ROOT16, nid, 2);
}

size_t
gen_xattr_entry(gen_image_t *image, uint64_t at, unsigned index,
    const char *suffix, const void *value, size_t value_length)
{
	size_t suffix_length = strlen(suffix);

	gen_put(image, at, suffix_length, 1);
	gen_put(image, at + 1, index, 1);
	gen_put(image, at + 2, value_length, 2);
	gen_put_bytes(image, at + 4, suffix, suffix_length);
	gen_put_bytes(image, at + 4 + suffix_length, value, value_length);
	return ((4 + suffix_length + value_length + 3) & ~(size_t)3);
}

/*
 * The LZ4 encoder writes through a cursor that keeps counting past the
 * capacity, so a block that does not fit is known by its length alone.
 */
typedef struct lz4_out {
	unsigned char	*bytes;
	size_t		 capacity;
	size_t		 at;
} lz4_out_t;

static void
lz4_byte(lz4_out_t *out, unsigned value)
{
	if (out->at < out->capacity)
		out->bytes[out->at] = (unsigned char)value;
	out->at++;
}

/* A length too large for its nibble: 255-valued bytes and a final remainder. */
static void
lz4_extension(lz4_out_t *out, size_t remaining)
{
	for (; remaining >= 255; remaining -= 255)
		lz4_byte(out, 255);
	lz4_byte(out, (unsigned)remaining);
}

/*
 * One sequence: token, literal length extension, literals, and for a match
 * the 16-bit offset and the match length extension (its nibble holds the
 * length less 4).
 */
static void
lz4_emit(lz4_out_t *out, const void *literals, size_t literal_length,
    unsigned offset, size_t match_length)
{
	size_t match_extra = match_length != 0 ? match_length - 4 : 0;
	unsigned literal_nibble = literal_length < 15 ?
	    (unsigned)literal_length : 15;
	unsigned match_nibble = match_extra < 15 ? (unsigned)match_extra : 15;

	lz4_byte(out, literal_nibble << 4 | match_nibble);
	if (literal_length >= 15)
		lz4_extension(out, literal_length - 15);
	for (size_t i = 0; i < literal_length; i++)
		lz4_byte(out, ((const unsigned char *)literals)[i]);
	if (match_length == 0)
		return;
	lz4_byte(out, offset & 0xff);
	lz4_byte(out, offset >> 8);
	if (match_extra >= 15)
		lz4_extension(out, match_extra - 15);
}

static size_t
lz4_finish(const lz4_out_t *out)
{
	return (out->at <= out->capacity ? out->at : 0);
}

size_t
gen_lz4_literals(unsigned char *out, size_t capacity, const void *in,
    size_t length)
{
	lz4_out_t o = { out, capacity, 0 };

	lz4_emit(&o, in, length, 0, 0);
	return (lz4_finish(&o));
}

size_t
gen_lz4_periodic(unsigned char *out, size_t capacity, const void *in,
    size_t length, size_t period)
{
	const unsigned char *p = in;
	lz4_out_t o = { out, capacity, 0 };

	CHECK(period >= 1 && period <= 0xffff && length >= period + 12);
	for (size_t i = period; i < length; i++)
		CHECK(p[i] == p[i - period]);
	/* The match stops 5 bytes short; the last sequence holds the rest. */
	lz4_emit(&o, p, period, (unsigned)period, length - 5 - period);
	lz4_emit(&o, p + length - 5, 5, 0, 0);
	return (lz4_finish(&o));
}

size_t
gen_lz4_sequence(unsigned char *out, size_t capacity, const void *literals,
    size_t literal_length, unsigned offset, size_t match_length)
{
	lz4_out_t o = { out, capacity, 0 };

	if (offset > 0xffff || (match_length != 0 && match_length < 4))
		return (0);
	lz4_emit(&o, literals, literal_length, offset, match_length);
	return (lz4_finish(&o));
}

void
gen_zheader_put(gen_image_t *image, uint64_t at, const gen_zheader_t *header)
{
	gen_put(image, at + 0, header->word0, 4);
	gen_put(image, at + 4, header->advice, 2);
	gen_put(image, at + 6, header->algorithms, 1);
	gen_put(image, at + 7, header->cluster_bits, 1);
}

static size_t
round_up(size_t value, size_t unit)
{
	return ((value + unit - 1) / unit * unit);
}

uint64_t
gen_zheader_offset(const gen_image_t *image, const gen_inode_t *inode,
    uint64_t nid)
{
	return (gen_inode_offset(image, nid) +
	    round_up(gen_inode_header_bytes(inode), 8));
}

void
gen_lz4_config_put(gen_image_t *image, unsigned slots, uint16_t max_distance,
    uint16_t max_blocks)
{
	uint64_t at = GEN_SUPER_OFFSET + 128 + 16 * (uint64_t)slots;

	gen_put(image, at, 14, 2);
	gen_put(image, at + 2, max_distance, 2);
	gen_put(image, at + 4, max_blocks, 2);
}

static bool
record_is_head(const gen_zrecord_t *record)
{
	return (record->type != GEN_Z_NONHEAD);
}

void
gen_full_index_put(gen_image_t *image, uint64_t header_offset,
    const gen_zrecord_t *records, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		const gen_zrecord_t *r = &records[i];
		uint64_t at = header_offset + 16 + 8 * (uint64_t)i;

		gen_put(image, at, (r->partial ? 0x8000 : 0) | r->type, 2);
		gen_put(image, at + 2, r->clusterofs, 2);
		if (record_is_head(r)) {
			gen_put(image, at + 4, r->block, 4);
		} else {
			gen_put(image, at + 4, r->backward, 2);
			gen_put(image, at + 6, r->forward, 2);
		}
	}
}

/* OR the width low bits of value in at bit bitpos; the bytes start zero. */
static void
put_bits(unsigned char *p, unsigned bitpos, unsigned width, uint32_t value)
{
	for (unsigned bit = 0; bit < width; bit++) {
		if (((value >> bit) & 1) != 0)
			p[(bitpos + bit) / 8] |=
			    (unsigned char)(1u << ((bitpos + bit) % 8));
	}
}

/*
 * The anchor without big clusters: the block of the last head or plain record
 * before the group, or, for the first group, one less than the first head's.
 */
static uint32_t
anchor_single(const gen_zrecord_t *records, size_t count, size_t group)
{
	for (size_t i = group; i-- > 0;) {
		if (record_is_head(&records[i]))
			return (records[i].block);
	}
	for (size_t i = 0; i < count; i++) {
		if (record_is_head(&records[i]))
			return (records[i].block - 1);
	}
	return (0);
}

/*
 * The anchor with big clusters: the start block of the first physical cluster
 * whose marker, or head when it has none, is at or after the group's first
 * record; the block after the last cluster when every cluster is behind it.
 */
static uint32_t
anchor_big(const gen_zrecord_t *records, size_t count, size_t group)
{
	uint32_t after_last = 0;

	for (size_t i = 0; i < count; i++) {
		bool marked;
		size_t accounted;

		if (!record_is_head(&records[i]))
			continue;
		marked = i + 1 < count &&
		    records[i + 1].type == GEN_Z_NONHEAD &&
		    (records[i + 1].backward & GEN_Z_BIG_MARKER) != 0;
		accounted = marked ? i + 1 : i;
		if (accounted >= group)
			return (records[i].block);
		after_last = records[i].block + (marked ?
		    (uint32_t)(records[i + 1].backward &
		    (GEN_Z_BIG_MARKER - 1)) : 1);
	}
	return (after_last);
}

/*
 * One group of `slots` record slots starting at record `first`, in `bytes`
 * bytes at p: n fields of fieldbits each from bit 0, the anchor in the last
 * 4. A slot past the records stays zero.
 */
static void
compact_group(unsigned char *p, size_t bytes, unsigned slots,
    unsigned fieldbits, const gen_zrecord_t *records, size_t count,
    size_t first, uint16_t advice)
{
	uint32_t anchor = (advice & (GEN_ZA_BIG1 | GEN_ZA_BIG2)) != 0 ?
	    anchor_big(records, count, first) :
	    anchor_single(records, count, first);

	for (unsigned slot = 0; slot < slots && first + slot < count; slot++) {
		const gen_zrecord_t *r = &records[first + slot];
		uint32_t value;

		if (record_is_head(r))
			value = r->clusterofs;
		else if (slot == slots - 1 &&
		    (r->backward & GEN_Z_BIG_MARKER) == 0)
			value = r->forward;	/* the last slot: forward */
		else
			value = r->backward;
		CHECK(value < 0x1000);
		put_bits(p, slot * fieldbits, fieldbits,
		    value | (uint32_t)r->type << 12);
	}
	put_le(p + bytes - 4, anchor, 4);
}

/*
 * Pack the records into out, a zeroed buffer of at least 8 * count + 32
 * bytes, for a map header ending at header_end. Without advice 0x01 every
 * group is 8 bytes. With it, and more records than the initial run that
 * brings the offset to a multiple of 32, that run of 8-byte groups is
 * followed by 32-byte groups of 16 records while 16 remain, then 8-byte
 * groups. Returns the length.
 */
static size_t
compact_pack(unsigned char *out, uint64_t header_end,
    const gen_zrecord_t *records, size_t count, uint16_t advice)
{
	size_t initial = (size_t)(((32 - header_end % 32) / 4) & 7);
	bool wide = (advice & GEN_ZA_COMPACT_2B) != 0 && count > initial;
	size_t at = 0, i = 0;

	CHECK(header_end % 8 == 0);
	while (i < count) {
		if (wide && i >= initial && count - i >= 16) {
			compact_group(out + at, 32, 16, 14, records, count, i,
			    advice);
			at += 32;
			i += 16;
		} else {
			compact_group(out + at, 8, 2, 16, records, count, i,
			    advice);
			at += 8;
			i += 2;
		}
	}
	return (at);
}

size_t
gen_compact_index_put(gen_image_t *image, uint64_t header_offset,
    const gen_zrecord_t *records, size_t count, uint16_t advice)
{
	unsigned char *packed = calloc(8 * count + 32, 1);
	size_t length;

	CHECK(packed != NULL);
	length = compact_pack(packed, header_offset + 8, records, count,
	    advice);
	gen_put_bytes(image, header_offset + 8, packed, length);
	free(packed);
	return (length);
}

static bool
cluster_is_plain(const gen_zcluster_t *cluster)
{
	return (cluster->kind == GEN_ZK_PLAIN ||
	    cluster->kind == GEN_ZK_INTERLACED);
}

size_t
gen_zfile_records(const gen_zfile_t *zf, unsigned blkszbits,
    uint32_t first_block, gen_zrecord_t **records_out)
{
	uint64_t block = (uint64_t)1 << blkszbits;
	size_t count = (size_t)((zf->size + block - 1) / block);
	gen_zrecord_t *records = calloc(count != 0 ? count : 1, sizeof(*records));
	size_t *cluster_at = calloc(count != 0 ? count : 1, sizeof(*cluster_at));
	uint32_t *first = calloc(zf->cluster_count != 0 ? zf->cluster_count : 1,
	    sizeof(*first));
	uint32_t next_block = first_block;
	size_t prev_head = SIZE_MAX, next_head = count;

	CHECK(records != NULL && cluster_at != NULL && first != NULL);
	for (size_t i = 0; i < count; i++)
		cluster_at[i] = SIZE_MAX;
	CHECK(zf->cluster_count == 0 || zf->clusters[0].start == 0);
	for (size_t k = 0; k < zf->cluster_count; k++) {
		size_t logical = (size_t)(zf->clusters[k].start / block);

		CHECK(zf->clusters[k].start < zf->size && logical < count);
		CHECK(cluster_at[logical] == SIZE_MAX);
		CHECK(k == 0 || zf->clusters[k].start > zf->clusters[k - 1].start);
		cluster_at[logical] = k;
		first[k] = next_block;
		next_block += zf->clusters[k].blocks;
	}
	for (size_t i = 0; i < count; i++) {
		gen_zrecord_t *r = &records[i];
		size_t k = cluster_at[i];

		if (k != SIZE_MAX) {
			const gen_zcluster_t *c = &zf->clusters[k];

			r->type = cluster_is_plain(c) ? GEN_Z_PLAIN :
			    c->head2 ? GEN_Z_HEAD2 : GEN_Z_HEAD1;
			r->clusterofs = (uint16_t)(c->start % block);
			r->block = first[k];
			if (zf->last == GEN_ZL_FRAGMENT && k + 1 == zf->cluster_count &&
			    zf->index_layout == 1)
				r->block = (uint32_t)(zf->fragment_offset >> 32);
			prev_head = i;
		} else if (i == count - 1 && zf->size % block != 0 &&
		    zf->last == GEN_ZL_BLOCKS) {
			/* The end marker begins an empty cluster at size. */
			r->type = GEN_Z_PLAIN;
			r->clusterofs = (uint16_t)(zf->size % block);
			r->block = zf->index_layout == 1 ? 0 : next_block;
		} else {
			const gen_zrecord_t *head;
			uint16_t big;

			CHECK(prev_head != SIZE_MAX);
			head = &records[prev_head];
			big = head->type == GEN_Z_HEAD2 ? GEN_ZA_BIG2 : GEN_ZA_BIG1;
			r->type = GEN_Z_NONHEAD;
			r->backward = (uint16_t)(i - prev_head);
			if (prev_head == i - 1 && head->type != GEN_Z_PLAIN &&
			    (zf->advice & big) != 0) {
				size_t k_head = cluster_at[prev_head];

				r->backward = (uint16_t)(GEN_Z_BIG_MARKER |
				    zf->clusters[k_head].blocks);
			}
		}
	}
	for (size_t i = count; i-- > 0;) {
		if (record_is_head(&records[i]))
			next_head = i;
		else
			records[i].forward = (uint16_t)(next_head - i);
	}
	free(cluster_at);
	free(first);
	*records_out = records;
	return (count);
}

/*
 * Write one cluster's stream right-aligned in `blocks` blocks at volume
 * offset `at`: leading zero bytes, then the stream. The extent is the file's
 * bytes the cluster covers.
 */
static size_t	zcluster_encode(size_t, size_t, const gen_zcluster_t *,
		    const unsigned char *, size_t, unsigned char *);

static void
zcluster_write(gen_image_t *image, uint64_t at, size_t block,
    const gen_zcluster_t *cluster, const unsigned char *extent, size_t length)
{
	size_t span = (size_t)cluster->blocks * block;
	unsigned char *stream = calloc(span != 0 ? span : 1, 1);
	size_t stream_length;

	CHECK(stream != NULL);
	stream_length = zcluster_encode(block, span, cluster, extent, length,
	    stream);
	gen_put_bytes(image, at + span - stream_length, stream, stream_length);
	free(stream);
}

/*
 * The stored bytes of one cluster of at most span bytes into stream: a
 * compressed or raw stream, or a plain cluster's span of block bytes with
 * the extent in place. Returns their length.
 */
static size_t
zcluster_encode(size_t block, size_t span, const gen_zcluster_t *cluster,
    const unsigned char *extent, size_t length, unsigned char *stream)
{
	size_t stream_length = 0;
	size_t ofs = (size_t)(cluster->start % block);

	switch (cluster->kind) {
	case GEN_ZK_PLAIN:
		CHECK(length <= span);
		bcopy(extent, stream, length);
		stream_length = span;	/* the block's bytes in place, no shift */
		break;
	case GEN_ZK_INTERLACED:
		CHECK(length <= block && span == block);
		for (size_t j = 0; j < length; j++)
			stream[(ofs + j) % block] = extent[j];
		stream_length = span;
		break;
	case GEN_ZK_LZ4_LITERAL:
		stream_length = gen_lz4_literals(stream, span, extent, length);
		CHECK(stream_length != 0 && stream[0] != 0);
		break;
	case GEN_ZK_LZ4_PERIODIC:
		stream_length = gen_lz4_periodic(stream, span, extent, length,
		    cluster->period);
		CHECK(stream_length != 0 && stream[0] != 0);
		break;
	case GEN_ZK_RAW:
		CHECK(cluster->raw_length <= span);
		bcopy(cluster->raw, stream, cluster->raw_length);
		stream_length = cluster->raw_length;
		break;
	}
	return (stream_length);
}

/*
 * The inline tail of the file's last cluster into tail, at most a block:
 * a plain cluster's extent, or its stream. Returns the tail's length.
 */
static size_t
ztail_encode(size_t block, const gen_zfile_t *zf, unsigned char *tail)
{
	const gen_zcluster_t *c = &zf->clusters[zf->cluster_count - 1];
	size_t length = (size_t)(zf->size - c->start);
	size_t stored;

	CHECK(c->blocks == 0);
	stored = zcluster_encode(block, block, c,
	    (const unsigned char *)zf->content + c->start, length, tail);
	return (c->kind == GEN_ZK_PLAIN || c->kind == GEN_ZK_INTERLACED ?
	    length : stored);
}

uint64_t
gen_tree_add_zfile(gen_tree_t *tree, const gen_inode_t *proto,
    const gen_zfile_t *zf)
{
	size_t block = (size_t)1 << tree->blkszbits;
	size_t header_start = round_up(gen_inode_header_bytes(proto), 8);
	gen_zrecord_t *records;
	size_t count, index_bytes, total_blocks = 0, tail_bytes = 0;
	gen_inode_t inode = *proto;
	gen_zheader_t header = { 0, zf->advice, zf->algorithms, 0 };
	unsigned char *tail = calloc(block, 1);
	uint64_t nid, header_offset;

	CHECK(tail != NULL);
	CHECK(zf->index_layout == 1 || zf->index_layout == 3);
	CHECK(zf->last != GEN_ZL_WHOLE || zf->index_layout == 1);
	count = gen_zfile_records(zf, tree->blkszbits, tree->data_next, &records);
	if (zf->last == GEN_ZL_INLINE) {
		tail_bytes = ztail_encode(block, zf, tail);
		header.word0 = (uint32_t)tail_bytes << 16;
		header.advice |= 0x08;
	} else if (zf->last == GEN_ZL_FRAGMENT) {
		CHECK(zf->clusters[zf->cluster_count - 1].blocks == 0);
		header.word0 = (uint32_t)zf->fragment_offset;
		header.advice |= 0x20;
	}
	if (zf->last == GEN_ZL_WHOLE) {
		index_bytes = 8;
	} else if (zf->index_layout == 1) {
		index_bytes = 16 + 8 * count;
	} else {
		/* The index length depends on the header's place modulo 32 only. */
		unsigned char *scratch = calloc(8 * count + 32, 1);

		CHECK(scratch != NULL);
		index_bytes = 8 + compact_pack(scratch, header_start + 8, records,
		    count, zf->advice);
		free(scratch);
	}
	nid = gen_tree_slot(tree, header_start + index_bytes, tail_bytes);
	/* An inline tail or a fragment is stored elsewhere, not in blocks. */
	for (size_t k = 0; k < zf->cluster_count - (zf->last != GEN_ZL_BLOCKS);
	    k++) {
		const gen_zcluster_t *c = &zf->clusters[k];
		uint64_t end = k + 1 < zf->cluster_count ?
		    zf->clusters[k + 1].start : zf->size;

		CHECK(tree->data_next + c->blocks <= tree->blocks);
		zcluster_write(&tree->image, (uint64_t)tree->data_next * block, block,
		    c, (const unsigned char *)zf->content + c->start,
		    (size_t)(end - c->start));
		tree->data_next += c->blocks;
		total_blocks += c->blocks;
	}
	inode.nid = nid;
	inode.size = zf->size;
	inode.layout = (uint8_t)zf->index_layout;
	inode.layout_word = (uint32_t)total_blocks;
	gen_inode_write(&tree->image, &inode);
	header_offset = gen_zheader_offset(&tree->image, proto, nid);
	if (zf->last == GEN_ZL_WHOLE) {
		gen_put(&tree->image, header_offset,
		    zf->fragment_offset | UINT64_C(1) << 63, 8);
	} else {
		gen_zheader_put(&tree->image, header_offset, &header);
		if (zf->index_layout == 1)
			gen_full_index_put(&tree->image, header_offset, records,
			    count);
		else
			(void)gen_compact_index_put(&tree->image, header_offset,
			    records, count, header.advice);
		gen_put_bytes(&tree->image, header_offset + index_bytes, tail,
		    tail_bytes);
	}
	free(records);
	free(tail);
	return (nid);
}

void
gen_tree_set_48bit(gen_tree_t *tree, uint64_t blocks, uint64_t root_nid)
{
	const unsigned char *incompat = tree->image.bytes + GEN_SUPER_OFFSET +
	    SB_INCOMPAT;

	CHECK(blocks >> 48 == 0 && (root_nid != 0 || blocks >> 32 == 0));
	gen_super_put(&tree->image, SB_INCOMPAT, (incompat[0] | 0x80u) |
	    (uint32_t)incompat[1] << 8 | (uint32_t)incompat[2] << 16 |
	    (uint32_t)incompat[3] << 24, 4);
	gen_super_put(&tree->image, SB_BLOCKS, blocks, 4);
	if (root_nid == 0)
		return;
	gen_super_put(&tree->image, SB_WIDE_ROOT, root_nid, 8);
	gen_super_put(&tree->image, SB_ROOT16, blocks >> 32, 2);
}

/* One encoded-extent record at a volume offset, the fields its width has. */
static void
xrecord_put(gen_image_t *image, uint64_t at, unsigned width,
    const gen_xrecord_t *record)
{
	gen_put(image, at, record->length, 4);
	if (width == 8)
		gen_put(image, at + 4, record->physical, 4);
	if (width >= 16)
		gen_put(image, at + 4, record->physical, 8);
	if (width == 16)
		gen_put(image, at + 12, record->logical, 4);
	if (width == 32)
		gen_put(image, at + 12, record->logical, 8);
}

uint64_t
gen_tree_add_extents(gen_tree_t *tree, const gen_inode_t *proto,
    const gen_xfile_t *xf)
{
	unsigned width = xf->width;
	unsigned width_bits = width == 4 ? 0 : width == 8 ? 1 : width == 16 ?
	    2 : 3;
	size_t header_start = round_up(gen_inode_header_bytes(proto), 8);
	size_t records_start = round_up(header_start + 8, width) +
	    (width == 4 ? 8 : 0);
	gen_inode_t inode = *proto;
	uint64_t nid, base, header;

	CHECK(width == 4 || width == 8 || width == 16 || width == 32);
	nid = gen_tree_slot(tree, records_start + xf->record_count * width, 0);
	inode.nid = nid;
	inode.size = xf->size;
	inode.layout = 1;
	inode.layout_word = 0;
	gen_inode_write(&tree->image, &inode);
	base = gen_inode_offset(&tree->image, nid);
	header = base + header_start;
	if (width >= 16) {
		gen_put(&tree->image, header, xf->count, 4);
		gen_put(&tree->image, header + 6, xf->count >> 32, 2);
	} else
		gen_put(&tree->image, header + 7, xf->shift, 1);
	gen_put(&tree->image, header + 4, xf->advice | 0x01u | width_bits << 1,
	    2);
	if (width == 4)
		gen_put(&tree->image, base + records_start - 8, xf->start, 8);
	for (size_t i = 0; i < xf->record_count; i++)
		xrecord_put(&tree->image, base + records_start + i * width,
		    width, &xf->records[i]);
	return (nid);
}

void
gen_tree_box_begin(gen_tree_t *tree)
{
	uint64_t block = (uint64_t)1 << tree->blkszbits;

	CHECK(tree->image.box_base == 0);
	tree->meta_next = round_up(tree->meta_next, block);
	tree->image.box_base = tree->meta_next;
	tree->image.box_end = UINT64_MAX;
}

uint64_t
gen_box_offset(const gen_tree_t *tree, uint64_t offset)
{
	CHECK(tree->image.box_base != 0);
	return (tree->image.box_base + offset);
}

uint64_t
gen_tree_box_end(gen_tree_t *tree, gen_box_kind_t kind)
{
	uint64_t block = (uint64_t)1 << tree->blkszbits;
	uint64_t base = tree->image.box_base, end, nid;
	const unsigned char *word = tree->image.bytes + GEN_SUPER_OFFSET +
	    SB_INCOMPAT;
	gen_inode_t box;

	CHECK(base != 0 && tree->image.box_end == UINT64_MAX);
	end = round_up(tree->meta_next, block);
	tree->meta_next = end;
	tree->image.box_end = end;
	bzero(&box, sizeof(box));
	box.mode = 0100644;
	box.nlink = 1;
	box.size = end - base;
	if (kind == GEN_BOX_PLAIN) {
		box.layout_word = (uint32_t)(base / block);
		nid = gen_tree_slot(tree, GEN_COMPACT_BYTES, 0);
		box.nid = nid;
		gen_inode_write(&tree->image, &box);
	} else {
		size_t count = (size_t)(box.size / block);
		gen_zcluster_t *clusters = calloc(count, sizeof(*clusters));
		unsigned char *content = malloc((size_t)box.size);
		gen_zfile_t zf;

		CHECK(clusters != NULL && content != NULL);
		bcopy(tree->image.bytes + base, content, (size_t)box.size);
		bzero(tree->image.bytes + base, (size_t)box.size);
		for (size_t i = 0; i < count; i++)
			clusters[i] = (gen_zcluster_t){ .start = i * block,
			    .blocks = 1, .kind = GEN_ZK_PLAIN };
		zf = (gen_zfile_t){ .content = content,
		    .size = (size_t)box.size, .index_layout = 1,
		    .clusters = clusters, .cluster_count = count,
		    .last = GEN_ZL_BLOCKS };
		nid = gen_tree_add_zfile(tree, &box, &zf);
		free(content);
		free(clusters);
	}
	gen_super_put(&tree->image, SB_INCOMPAT, (word[0] |
	    (uint32_t)word[1] << 8 | (uint32_t)word[2] << 16 |
	    (uint32_t)word[3] << 24) | 0x100u, 4);
	gen_super_put(&tree->image, SB_SLOTS, 1, 1);
	gen_super_put(&tree->image, SB_BOX_NID, nid, 8);
	return (nid);
}
