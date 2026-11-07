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

/*
 * Directory blocks. A gen_dirent_t is one record: the name is a C string
 * (the record's bytes, so "" is the empty name) and the type the file type
 * byte, to which GEN_STARTS_BLOCK may be ORed. gen_dir_pack() writes the
 * records in the order given into out, which the caller zeroed and sized for
 * capacity bytes: as many records to a block as fit, a new block also
 * starting at a record whose type has GEN_STARTS_BLOCK set. It returns the
 * directory's size: every block but the last is full length and the last
 * ends after its last name. It sorts and checks no order, but a
 * block that would not fit in the capacity stops the test.
 */
enum {
	GEN_STARTS_BLOCK = 0x100
};

typedef struct gen_dirent {
	const char	*name;
	uint64_t	 nid;
	uint16_t	 type;
} gen_dirent_t;

size_t		gen_dir_pack(unsigned char *, size_t, size_t,
		    const gen_dirent_t *, size_t);

/*
 * A tree builder over an image of `blocks` blocks of 2^blkszbits bytes. The
 * superblock is default; NID 0 is reserved for the root. Inodes are placed
 * in a metadata area of GEN_TREE_META_BYTES that starts at byte 4096, and
 * data blocks come after it, so a file's first block is
 * gen_tree_first_block(). gen_tree_seal() stores the checksum last.
 */
enum {
	GEN_TREE_META_BYTES = 65536
};

typedef struct gen_tree {
	gen_image_t	 image;
	unsigned	 blkszbits;
	uint32_t	 blocks;
	uint64_t	 meta_next;	/* the next free metadata byte */
	uint32_t	 data_next;	/* the next free data block */
} gen_tree_t;

uint32_t	gen_tree_first_block(unsigned);
void		gen_tree_init(gen_tree_t *, unsigned, uint32_t);
void		gen_tree_free(gen_tree_t *);

/* The superblock rewritten with another block count; the image is kept. */
void		gen_tree_set_blocks(gen_tree_t *, uint32_t);
void		gen_tree_seal(gen_tree_t *);

/* The superblock's compatible feature word, SB_CHKSUM kept as given. */
void		gen_tree_set_compat(gen_tree_t *, uint32_t);

/* The NID of the 32-byte slot at a volume offset in the metadata area. */
uint64_t	gen_nid_at(const gen_image_t *, uint64_t);

/*
 * Reserve room for an inode: its fixed part and attribute region, then an
 * inline tail. The tail lies in one block; the part before it may lie in
 * the block before.
 */
uint64_t	gen_tree_slot(gen_tree_t *, size_t, size_t);

/*
 * Write the inode at nid with the content stored under its layout: layout 0
 * keeps all of it in data blocks, layout 2 keeps floor(size / B) blocks
 * there and the rest inline after the inode's fixed part and attribute
 * region. The layout word becomes the first data block when there is one and
 * is the prototype's otherwise. size is the inode's size.
 */
void		gen_tree_fill(gen_tree_t *, uint64_t, const gen_inode_t *,
		    const void *, size_t);

/* A slot of the right size followed by gen_tree_fill(); returns the NID. */
uint64_t	gen_tree_add(gen_tree_t *, const gen_inode_t *, const void *,
		    size_t);

/*
 * A compact directory (mode 040755) at nid holding the children and the "."
 * and ".." records, all sorted by name bytes, stored flat in data blocks.
 */
void		gen_tree_dir(gen_tree_t *, uint64_t, uint64_t,
		    const gen_dirent_t *, size_t);

/* Name the inode at nid as the root in the superblock. */
void		gen_tree_set_root(gen_tree_t *, uint64_t);

/*
 * One inline attribute entry at a volume offset: name index, suffix and
 * value, padded to 4 bytes. Returns the padded length.
 */
size_t		gen_xattr_entry(gen_image_t *, uint64_t, unsigned,
		    const char *, const void *, size_t);

#endif /* !FSIMG_TEST_EROFS_GEN_H */
