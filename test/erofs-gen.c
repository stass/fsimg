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
