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
	/* A metadata box's bytes in the metadata area; see gen_tree_box_begin. */
	uint64_t	 box_base;	/* zero: no box */
	uint64_t	 box_end;
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

/* The superblock's incompatible feature word. */
void		gen_tree_set_incompat(gen_tree_t *, uint32_t);

/* The superblock's packed NID. */
void		gen_tree_set_packed(gen_tree_t *, uint64_t);

/*
 * Copy size bytes into the next free data blocks, rounded up to whole blocks
 * and zero after size, and return the first block's number. A chunk's data
 * lives here, named by the chunk index of an inode.
 */
uint32_t	gen_tree_data(gen_tree_t *, const void *, size_t);

/*
 * Chunk-based files (layout 4). The chunk format is the low 16 bits of the
 * layout word: chunkbits in bits 0 to 4, GEN_CHUNK_INDEXES for 8-byte index
 * entries and GEN_CHUNK_WIDE for 48-bit block numbers; any other bit may be
 * ORed in to build a corrupt one. The high 16 bits of the word are stored as
 * given, since producers leave them uninitialised.
 *
 * A gen_chunk_t is one index entry as written. A 4-byte entry is just block.
 * An 8-byte entry is high (16 bits), device (16 bits), then block (32 bits),
 * all little-endian. gen_chunk_block() names a block with the high part the
 * block's bits 32 to 47 need; gen_chunk_hole(format) is the all-ones block
 * number at the width the format has: block 0xFFFFFFFF, and under
 * GEN_CHUNK_WIDE also high 0xFFFF. Fields can be set directly to build a
 * corrupt entry.
 */
enum {
	GEN_CHUNK_INDEXES = 0x20,
	GEN_CHUNK_WIDE = 0x40
};

typedef struct gen_chunk {
	uint32_t	 block;
	uint16_t	 high;
	uint16_t	 device;
} gen_chunk_t;

gen_chunk_t	gen_chunk_block(uint64_t);
gen_chunk_t	gen_chunk_hole(uint16_t);

/*
 * Overwrite index entry `index` of the chunk-based inode at nid, which was
 * added from the same prototype and chunk format.
 */
void		gen_chunk_put(gen_tree_t *, uint64_t, const gen_inode_t *,
		    uint16_t, size_t, gen_chunk_t);

/*
 * Add a chunk-based inode shaped by the prototype (size, mode and the rest
 * as given, layout forced to 4) with the layout word given, its chunk
 * format in the low half and anything in the high half, and the index of
 * count entries at the attribute region's end rounded up to the entry
 * width. The size and the entry count are
 * independent, so a test can claim more chunks than the index holds.
 * Returns the NID.
 */
uint64_t	gen_tree_add_chunked(gen_tree_t *, const gen_inode_t *,
		    uint32_t, const gen_chunk_t *, size_t);

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

/*
 * A test-only LZ4 block encoder. A block is the raw sequence stream with no
 * frame. Each function returns the block's length, or 0 when it does not fit
 * the capacity (or, for gen_lz4_sequence(), when an argument has no encoding).
 *
 * gen_lz4_literals(): the input as one literal-only sequence.
 *
 * gen_lz4_periodic(): for input whose bytes repeat with `period`
 * (in[i] == in[i - period], checked), one sequence of in[0, period) literals
 * and a match of offset `period` up to length - 5, then a last sequence of
 * the final 5 literals. It needs length - period >= 12, the LZ4 end rules: the
 * last sequence carries at least 5 literals and the last match starts at
 * least 12 bytes before the end.
 *
 * gen_lz4_sequence(): one sequence appended at out, for hostile blocks: any
 * 16-bit offset, including 0 or one reaching before the output's start. A
 * match_length of 0 writes a literal-only (final) sequence, otherwise it is at
 * least 4. Returns the bytes written.
 */
size_t		gen_lz4_literals(unsigned char *, size_t, const void *, size_t);
size_t		gen_lz4_periodic(unsigned char *, size_t, const void *, size_t,
		    size_t);
size_t		gen_lz4_sequence(unsigned char *, size_t, const void *, size_t,
		    unsigned, size_t);

/*
 * Compressed files. A record is one logical cluster's index entry as the test
 * wants it read. Its type is one of the GEN_Z_* values; a head or plain
 * record names its cluster's first block (absolute) and clusterofs; a
 * non-head one carries the backward distance (or GEN_Z_BIG_MARKER | blocks)
 * and the forward distance. `partial` is bit 15 of a full record's advice
 * word and has no place in the compact form.
 */
enum {
	GEN_Z_PLAIN = 0,
	GEN_Z_HEAD1 = 1,
	GEN_Z_NONHEAD = 2,
	GEN_Z_HEAD2 = 3,
	GEN_Z_BIG_MARKER = 0x800
};

/* The map header's advice flags this generator knows. */
enum {
	GEN_ZA_COMPACT_2B = 0x01,
	GEN_ZA_BIG1 = 0x02,
	GEN_ZA_BIG2 = 0x04,
	GEN_ZA_INTERLACED = 0x10
};

typedef struct gen_zrecord {
	uint32_t	 block;
	uint16_t	 clusterofs;
	uint16_t	 backward;
	uint16_t	 forward;
	uint8_t		 type;
	bool		 partial;
} gen_zrecord_t;

/* The 8-byte map header, written as given. */
typedef struct gen_zheader {
	uint32_t	 word0;		/* fragment offset low bits, or tail size */
	uint16_t	 advice;
	uint8_t		 algorithms;	/* head-1 in bits 0-3, head-2 in 4-7 */
	uint8_t		 cluster_bits;	/* lclusterbits - blkszbits, bit 7 flag */
} gen_zheader_t;

void		gen_zheader_put(gen_image_t *, uint64_t, const gen_zheader_t *);

/* Where an inode's map header lies: its attribute region's end rounded to 8. */
uint64_t	gen_zheader_offset(const gen_image_t *, const gen_inode_t *,
		    uint64_t);

/*
 * The full index of count records, 8 bytes each, from 16 bytes after the map
 * header's start at header_offset.
 */
void		gen_full_index_put(gen_image_t *, uint64_t,
		    const gen_zrecord_t *, size_t);

/*
 * The compact index of count records from the map header's end, packed in
 * groups as the design states for the given advice flags. A group's anchor
 * is computed from the records' blocks: without big clusters (advice 0x06
 * clear) the block of the last head or plain record before the group, or the
 * first head's block less one for the first group; with them, the start block
 * of the first physical cluster not yet accounted for when the group begins
 * (a cluster is accounted for once its count marker, or its head when it has
 * none, lies before the group's first record), or the block after the last
 * cluster when all are. Returns the index length in bytes.
 */
size_t		gen_compact_index_put(gen_image_t *, uint64_t,
		    const gen_zrecord_t *, size_t, uint16_t);

/*
 * The LZ4 configuration record of a COMPR_CFGS volume at the first record
 * position after the superblock and `slots` 16-byte extension slots. The
 * caller sets the superblock's mask and feature bits.
 */
void		gen_lz4_config_put(gen_image_t *, unsigned, uint16_t, uint16_t);

/*
 * One physical cluster of a gen_zfile_t. `start` is the logical offset of
 * its extent, which runs to the next cluster's start or the file's size. The
 * clusters must start in distinct, increasing logical clusters, the first at
 * 0. The stream is written right-aligned in `blocks` blocks after zero
 * padding:
 *   PLAIN        the extent at block byte 0 (shifted), at most B bytes;
 *   INTERLACED   extent byte j at block byte (start mod B + j) mod B;
 *   LZ4_LITERAL  gen_lz4_literals() of the extent;
 *   LZ4_PERIODIC gen_lz4_periodic() of the extent with `period`;
 *   RAW          the caller's bytes (raw, raw_length), whatever they are.
 * A compressed cluster is head-1, or head-2 when head2 is set; a plain one is
 * a plain record.
 */
typedef enum gen_zkind {
	GEN_ZK_PLAIN,
	GEN_ZK_INTERLACED,
	GEN_ZK_LZ4_LITERAL,
	GEN_ZK_LZ4_PERIODIC,
	GEN_ZK_RAW
} gen_zkind_t;

typedef struct gen_zcluster {
	uint64_t	 start;
	uint32_t	 blocks;
	gen_zkind_t	 kind;
	bool		 head2;
	size_t		 period;
	const void	*raw;
	size_t		 raw_length;
} gen_zcluster_t;

/*
 * Where a compressed file's last cluster is stored. GEN_ZL_BLOCKS keeps it
 * in blocks like the others. GEN_ZL_INLINE stores its stream, or a plain
 * cluster's extent, after the index as a compressed inline tail (advice
 * 0x08, h_idata_size in the header). GEN_ZL_FRAGMENT makes it a fragment at
 * fragment_offset of the packed inode (advice 0x20; the header holds the
 * offset's low 32 bits and a full index's head record the high 32). Either
 * way the last cluster's `blocks` is 0, and the index ends with no end
 * marker. GEN_ZL_WHOLE makes the whole file one fragment: the header is the
 * offset with bit 63 set and no index follows; the layout must be 1. The
 * caller places a fragment's bytes in the packed inode.
 */
typedef enum gen_zlast {
	GEN_ZL_BLOCKS,
	GEN_ZL_INLINE,
	GEN_ZL_FRAGMENT,
	GEN_ZL_WHOLE
} gen_zlast_t;

/*
 * A compressed file: the content, and how it is stored. index_layout is 1
 * (full index) or 3 (compact); `advice` and `algorithms` fill the map header
 * (the cluster bits are 0, so L = B), with the bit `last` implies added.
 */
typedef struct gen_zfile {
	const void		*content;
	size_t			 size;
	unsigned		 index_layout;
	uint16_t		 advice;
	uint8_t			 algorithms;
	const gen_zcluster_t	*clusters;
	size_t			 cluster_count;
	gen_zlast_t		 last;
	uint64_t		 fragment_offset;
} gen_zfile_t;

/*
 * The records the file's clusters imply, ceil(size / B) of them, allocating
 * their blocks from first_block on in cluster order. Heads and plain
 * records sit where clusters start; a non-head has its backward distance to
 * the cluster's head and its forward distance to the next head or plain
 * record, or to the record count when none follows. The first non-head of a
 * compressed cluster carries the count marker when the advice permits big
 * clusters for its head type. When the last cluster starts before the last
 * logical cluster and size mod B is nonzero, the last record is the end
 * marker, a plain record with clusterofs = size mod B (block 0 in a full
 * index, the block after the last cluster in a compact one). Returns the
 * count; the caller frees *records.
 */
size_t		gen_zfile_records(const gen_zfile_t *, unsigned, uint32_t,
		    gen_zrecord_t **);

/*
 * Add a compressed inode for the file: prototype fields as gen_tree_add(), but
 * the layout is index_layout, the size is the file's and the layout word the
 * total of the clusters' blocks. Cluster blocks are allocated from
 * tree->data_next. The body is the fixed part and attribute region, the map
 * header and the index; the caller writes any attributes. Returns the NID.
 */
uint64_t	gen_tree_add_zfile(gen_tree_t *, const gen_inode_t *,
		    const gen_zfile_t *);

/*
 * The 48-bit layout's superblock, incompatible bit 0x080 ORed into the
 * feature word, with a block count of up to 48 bits whose low 32 bits are
 * stored at 0x24. The wide form, a nonzero root NID, stores the root NID at
 * 0x70 and the count's bits 32 to 47 at 0x0E. The narrow form, root NID 0,
 * leaves the 16-bit root NID at 0x0E as it is and needs a count below 2^32.
 * The image is not resized: a count beyond it describes a sparse volume.
 */
void		gen_tree_set_48bit(gen_tree_t *, uint64_t, uint64_t);

/*
 * Encoded extents: a full-index inode (layout 1) of a 48-bit volume whose
 * advice has bit 0x01, with records in place of an index. A gen_xrecord_t is
 * one record as written, each width keeping the fields it has: 4 bytes the
 * length word alone; 8 bytes it and the physical offset's low 32 bits; 16
 * bytes it, the 64-bit physical offset in two halves and the logical
 * offset's low 32 bits; 32 bytes it, both offsets in two halves each and 12
 * zero bytes.
 *
 * A gen_xfile_t is the inode's map. The advice gets 0x01 and the width's
 * bits 1 and 2 added. For 16- and 32-byte records the map header holds
 * `count`, its low 32 bits in bytes 0 to 3 and its high 16 in bytes 6 and 7;
 * for 4- and 8-byte records bytes 0 to 3 and 6 are zero and byte 7 is
 * `shift`. The records start at the map header's end rounded up to the
 * width; 4-byte records follow the 64-bit physical `start` placed there.
 * The size and the records are independent, so a test can write any count.
 */
typedef struct gen_xrecord {
	uint64_t	 physical;
	uint64_t	 logical;
	uint32_t	 length;	/* the length word */
} gen_xrecord_t;

typedef struct gen_xfile {
	uint64_t		 size;
	uint64_t		 count;		/* 16- and 32-byte records */
	uint64_t		 start;		/* 4-byte records */
	const gen_xrecord_t	*records;
	size_t			 record_count;
	unsigned		 width;		/* 4, 8, 16 or 32 */
	uint16_t		 advice;
	uint8_t			 shift;		/* 4- and 8-byte records */
} gen_xfile_t;

/*
 * Add an encoded-extent inode for the map: prototype fields as
 * gen_tree_add(), but the layout is 1, the size is the map's and the layout
 * word 0. The extents' bytes are the caller's to place. Returns the NID.
 */
uint64_t	gen_tree_add_extents(gen_tree_t *, const gen_inode_t *,
		    const gen_xfile_t *);

/*
 * Metadata box volumes. gen_tree_box_begin() opens the box at the next block
 * boundary of the metadata area: every slot gen_tree_slot() hands out until
 * gen_tree_box_end() lies in the box, and its NID is a box NID, bit 63 set
 * over a count of 32-byte slots from the box's first byte, which
 * gen_inode_offset() and gen_nid_at() map to the box's bytes in the area.
 * So every builder works on inodes in the box unchanged, and a test writes
 * a shared pool or a prefix table there at gen_box_offset(). A box NID
 * makes a root only through the 48-bit layout's wide root field.
 *
 * gen_tree_box_end() closes the box at the next block boundary, stores it
 * by kind, adds the box inode in the primary space after it and sets
 * METABOX, one extension slot and the box NID; it returns the box inode's
 * NID. GEN_BOX_PLAIN keeps the box's blocks in place as a flat inode's
 * content. GEN_BOX_INDEXED copies them into data blocks as plain clusters
 * behind a full index and zeroes them in the area, so the box reads only
 * through its index. Inodes in an indexed box are complete before the end.
 */
typedef enum gen_box_kind {
	GEN_BOX_PLAIN,
	GEN_BOX_INDEXED
} gen_box_kind_t;

#define GEN_NID_BOX		(UINT64_C(1) << 63)

void		gen_tree_box_begin(gen_tree_t *);
uint64_t	gen_tree_box_end(gen_tree_t *, gen_box_kind_t);
/* The volume offset of byte `offset` of an open or plain box's content. */
uint64_t	gen_box_offset(const gen_tree_t *, uint64_t);

#endif /* !FSIMG_TEST_EROFS_GEN_H */
