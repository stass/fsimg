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
