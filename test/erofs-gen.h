/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_TEST_EROFS_GEN_H
#define FSIMG_TEST_EROFS_GEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A small EROFS image generator for tests, written from the design's field
 * tables and independent of lib/formats/erofs: it includes none of its
 * headers and calls none of its functions. It builds an image in memory from
 * a zeroed buffer: the base superblock, compact and extended inodes at their
 * NIDs, raw field writers for mutating a single field, and the superblock
 * checksum, which is computed last.
 */

/* Where the superblock starts in the volume. */
enum {
	GEN_SUPER_OFFSET = 1024
};

/* The bytes of the two inode forms. */
enum {
	GEN_COMPACT_BYTES = 32,
	GEN_EXTENDED_BYTES = 64
};

typedef struct gen_image {
	unsigned char	*bytes;
	size_t		 length;
	uint64_t	 meta_base;	/* meta_blkaddr * B, set by gen_super_write */
} gen_image_t;

/* Every base superblock field; gen_super_default() fills a valid volume. */
typedef struct gen_super {
	uint64_t	 inode_count;
	uint64_t	 packed_nid;
	uint64_t	 box_nid;	/* the first extension slot */
	int64_t		 epoch;
	uint32_t	 magic;
	uint32_t	 checksum;	/* stored as given until gen_seal() */
	uint32_t	 compat;
	uint32_t	 epoch_nsec;
	uint32_t	 blocks;
	uint32_t	 meta_blkaddr;
	uint32_t	 xattr_blkaddr;
	uint32_t	 incompat;
	uint32_t	 prefix_start;
	uint32_t	 build_time;
	uint16_t	 root_nid;
	uint16_t	 compression_word;
	uint16_t	 devices;
	uint8_t		 blkszbits;
	uint8_t		 slots;
	uint8_t		 dirblkbits;
	uint8_t		 prefix_count;
	uint8_t		 uuid[16];
	uint8_t		 name[16];
} gen_super_t;

/* An inode of either form. Fields a form lacks are ignored. */
typedef struct gen_inode {
	uint64_t	 nid;
	uint64_t	 size;
	int64_t		 time;		/* extended: seconds; compact: a word */
	uint32_t	 time_nsec;	/* extended only */
	uint32_t	 layout_word;
	uint32_t	 serial;
	uint32_t	 uid;
	uint32_t	 gid;
	uint32_t	 nlink;
	uint16_t	 mode;
	uint16_t	 xattr_count;
	uint16_t	 format_extra;	/* raw bits ORed into the format word */
	uint8_t		 layout;	/* data layout, 0 to 7 */
	bool		 extended;
} gen_inode_t;

/* A zeroed image of length bytes. */
void		gen_image_init(gen_image_t *, size_t);
void		gen_image_free(gen_image_t *);

/*
 * A valid volume of blocks blocks of 2^blkszbits bytes: SB_CHKSUM set, the
 * root NID 0 and the metadata starting at volume byte 4096, past the whole
 * superblock region at every block size.
 */
gen_super_t	gen_super_default(unsigned, uint32_t);
void		gen_super_write(gen_image_t *, const gen_super_t *);

/* The root directory used by the minimal volume: a compact inline directory. */
gen_inode_t	gen_root_default(void);

/* The inode's place and the size of its fixed part plus attribute region. */
uint64_t	gen_inode_offset(const gen_image_t *, uint64_t);
size_t		gen_inode_header_bytes(const gen_inode_t *);
void		gen_inode_write(gen_image_t *, const gen_inode_t *);

/* Little-endian writers: at a volume offset, and at a superblock offset. */
void		gen_put(gen_image_t *, uint64_t, uint64_t, unsigned);
void		gen_put_bytes(gen_image_t *, uint64_t, const void *, size_t);
void		gen_super_put(gen_image_t *, unsigned, uint64_t, unsigned);

/*
 * Compute the superblock checksum over [1024, B) when B > 1024 and
 * [1024, 1024 + B) otherwise, taking the checksum field as zero, and store
 * it. The block size is read from the image's own superblock.
 */
void		gen_seal(gen_image_t *);

/*
 * The CRC-32C register gen_seal() stores: the definition's reflected
 * register seeded with all ones and not inverted at the end.
 */
uint32_t	gen_crc32c_register(const unsigned char *, size_t);

#endif /* !FSIMG_TEST_EROFS_GEN_H */
