/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_QNX4_H
#define FSIMG_QNX4_H

#include "reader.h"

/* The QNX4 filesystem (specs/design-qnx4.md and specs/design-qnx4-driver.md).
 * A volume is N 512-byte blocks numbered from 1; block b starts at byte
 * 512 * (b - 1) of the slice. Offsets and sizes are in bytes.
 */
#define QNX4_BLOCK_BYTES	512u
#define QNX4_RECORD_BYTES	64u
#define QNX4_SLOTS_PER_BLOCK	8u
#define QNX4_PROBE_BYTES	1024u
#define QNX4_ROOT_BLOCK		2u
#define QNX4_BITMAP_FIRST	3u
#define QNX4_BITMAP_OFFSET	1024u
#define QNX4_BITS_PER_BLOCK	4096u
#define QNX4_BLOCKS_MIN		43u
#define QNX4_BLOCKS_MAX		(UINT64_C(1) << 31)
#define QNX4_FILE_MAX		UINT32_C(0x7fffffff)
#define QNX4_WINDOW_BYTES	4096u
#define QNX4_XBLOCK_EXTENTS	60u
#define QNX4_DIRECT_NAME_MAX	16u
#define QNX4_LINK_NAME_MAX	48u
#define QNX4_LONG_NAME_MIN	49u
#define QNX4_NAME_MAX		505u
#define QNX4_LONG_PREFIX	38u	/* name bytes kept in a mangled link */
#define QNX4_LABEL_OFFSET	0x1c0u
#define QNX4_LABEL_BYTES	48u
#define QNX4_ROOT_RECORDS	5u
#define QNX4_ROOT_FIXED_SLOTS	6u	/* ".", "..", .bitmap and three links */

/* Record status bits. A directory slot is free without USED and LINK, an
 * .inodes slot without USED; the bytes of a free slot carry no meaning.
 */
#define QNX4_STATUS_USED	0x01u
#define QNX4_STATUS_MODIFIED	0x02u
#define QNX4_STATUS_HIDDEN	0x04u
#define QNX4_STATUS_LINK	0x08u
#define QNX4_STATUS_TABLE	0x10u
#define QNX4_STATUS_CLEAN	0x20u

#define QNX4_MODE_TYPE		0170000u
#define QNX4_MODE_FIFO		0010000u
#define QNX4_MODE_DIRECTORY	0040000u
#define QNX4_MODE_FILE		0100000u
#define QNX4_MODE_SYMLINK	0120000u

/* The five records of the root block, in order. */
enum {
	QNX4_RECORD_ROOT = 0,
	QNX4_RECORD_INODES,
	QNX4_RECORD_BOOT,
	QNX4_RECORD_ALTBOOT,
	QNX4_RECORD_LOST_FOUND
};

/* A record position is the block holding a 64-byte record and the record's
 * slot in it, packed as block * 8 + slot. It is an object's identity and
 * the target of every link and dot entry.
 */
#define QNX4_POSITION(block, slot) \
	((uint64_t)(block) * QNX4_SLOTS_PER_BLOCK + (slot))
#define QNX4_POSITION_BLOCK(position) \
	((uint32_t)((position) / QNX4_SLOTS_PER_BLOCK))
#define QNX4_POSITION_SLOT(position) \
	((unsigned)((position) % QNX4_SLOTS_PER_BLOCK))

/* One run of blocks: the first block and the count, both zero for none. */
typedef struct qnx4_extent {
	uint32_t	 first;
	uint32_t	 blocks;
} qnx4_extent_t;

/* The 64-byte inode record: a direct directory slot, an .inodes slot or a
 * root-block record. name_length counts the bytes before the first NUL;
 * name_padding_zero is false when a nonzero byte follows that NUL.
 */
typedef struct qnx4_inode {
	unsigned char	 name[QNX4_DIRECT_NAME_MAX];
	qnx4_extent_t	 first_extent;
	uint32_t	 size;
	uint32_t	 chain;		/* first extent block, or 0 */
	uint32_t	 created;
	uint32_t	 modified;
	uint32_t	 accessed;
	uint32_t	 changed;
	uint16_t	 extents;	/* extent count, the record's own included */
	uint16_t	 mode;
	uint16_t	 uid;
	uint16_t	 gid;
	uint16_t	 links;
	uint8_t		 name_length;
	uint8_t		 type;		/* byte 0x3e */
	uint8_t		 status;
	bool		 name_padding_zero;
	bool		 reserved_zero;	/* bytes 0x3a to 0x3d */
} qnx4_inode_t;

/* The 64-byte link record: a name and the position of the record it names.
 * long_block is the .longfilenames block of a name over 48 bytes, else 0.
 */
typedef struct qnx4_link {
	unsigned char	 name[QNX4_LINK_NAME_MAX];
	uint32_t	 target_block;
	uint32_t	 long_block;
	uint8_t		 target_slot;
	uint8_t		 name_length;
	uint8_t		 status;
	bool		 name_padding_zero;
	bool		 spare_zero;	/* bytes 0x39 to 0x3e */
} qnx4_link_t;

/* A 512-byte extent block of an object's overflow chain. */
typedef struct qnx4_xblock {
	qnx4_extent_t	 extent[QNX4_XBLOCK_EXTENTS];
	qnx4_extent_t	 first_copy;	/* bytes 0x1f8 to 0x1ff */
	uint32_t	 next;
	uint32_t	 previous;
	uint32_t	 covered;	/* sum of the used descriptors' lengths */
	uint8_t		 used;
	bool		 spare_zero;	/* bytes 0x009 to 0x00b */
	bool		 signature_ok;	/* "IamXblk" and a NUL at 0x1f0 */
} qnx4_xblock_t;

/* A 512-byte .longfilenames record naming one link of over 48 bytes. */
typedef struct qnx4_longname {
	unsigned char	 name[QNX4_NAME_MAX];
	uint32_t	 block;		/* back-reference: the link's block */
	uint16_t	 name_length;
	uint8_t		 slot;		/* and its slot */
	uint8_t		 state;
	bool		 name_padding_zero;
	bool		 tail_zero;	/* byte 0x1ff */
} qnx4_longname_t;

/* The geometry derived from the slice length: N blocks, of which blocks 3
 * to 2 + B hold the bitmap's ceil(N / 8) bytes.
 */
typedef struct qnx4_geometry {
	uint64_t	 volume_bytes;
	uint32_t	 blocks;		/* N */
	uint32_t	 bitmap_blocks;		/* B = ceil(N / 4096) */
	uint32_t	 bitmap_bytes;		/* ceil(N / 8) */
} qnx4_geometry_t;

/* How an extent walk treats each block it visits, beyond the range checks
 * it always applies. A volume uses every block once, so the first walk of
 * each object claims its blocks, extent blocks included, and a block
 * claimed twice is corruption whichever object came first.
 * ALLOCATED	check that the bitmap marks every block in use: the root's
 *		slots at open, a later name of an .inodes object
 * EXCLUSIVE	check allocation and claim every block: .inodes,
 *		.longfilenames and lost+found at open, every directory as it
 *		is entered, the first name of every other object
 */
typedef enum qnx4_walk_policy {
	QNX4_WALK_ALLOCATED = 1,
	QNX4_WALK_EXCLUSIVE
} qnx4_walk_policy_t;

/* Where a record lives, which selects its closed status table. */
typedef enum qnx4_place {
	QNX4_PLACE_DIRECT = 1,	/* a directory slot */
	QNX4_PLACE_TABLE,	/* an .inodes slot */
	QNX4_PLACE_ROOT		/* a root-block record */
} qnx4_place_t;

/* A sorted map from physical blocks to an object's logical blocks, for
 * finding a record by its physical position: one entry per extent of the
 * object's index, ordered by first block. Objects claim their blocks, so
 * the entries never overlap.
 */
typedef struct qnx4_locator {
	fsimg_array_t	*runs;
	uint64_t	 count;
} qnx4_locator_t;

/* Driver state behind the reader's private pointer, from open to close.
 * root holds the five decoded root-block records. inodes and names index
 * .inodes and .longfilenames (names is NULL without .longfilenames), each
 * with its locator. current is the record replay made current, at
 * current_position, with its parent directory's position. file indexes
 * the current object's content: replay builds it for an object's first
 * name, and the first read builds it for a later one (later_name).
 * claimed holds the blocks exclusive walks claimed, bit b - 1 for block b
 * as in the bitmap's own bits, so verification compares the two byte for
 * byte. exposed has one bit per logical .inodes slot, set when replay
 * reached the slot's first name.
 *
 * references and long_named exist only during full verification, and
 * references being set is what makes directory preparation account names
 * (see verify.c): references receives the logical .inodes slot of every
 * link record's target, and long_named has one bit per .longfilenames
 * record, set when a link's long name is decoded from it.
 */
typedef struct fsimg_qnx4_reader fsimg_qnx4_reader_t;

struct fsimg_qnx4_reader {
	qnx4_geometry_t	 geometry;
	qnx4_inode_t	 root[QNX4_ROOT_RECORDS];
	qnx4_inode_t	 current;
	qnx4_locator_t	 inodes_locator;
	qnx4_locator_t	 names_locator;
	fsimg_extent_index_t *inodes;
	fsimg_extent_index_t *names;
	fsimg_extent_index_t *file;
	fsimg_bitset_t	*claimed;
	fsimg_bitset_t	*exposed;
	fsimg_stream_t	*references;
	fsimg_bitset_t	*long_named;
	uint64_t	 current_position;
	uint64_t	 current_parent;
	uint64_t	 inode_slots;		/* .inodes size / 64 */
	uint64_t	 names_slot_offset;	/* root offset of .longfilenames */
	uint64_t	 bitmap_window_offset;
	size_t		 bitmap_window_length;
	bool		 later_name;
	bool		 has_names;
	bool		 label_valid;
	bool		 lost_found_used;
	char		 label[QNX4_LABEL_BYTES + 1];
	unsigned char	 root_block[QNX4_BLOCK_BYTES];
	unsigned char	 bitmap_window[QNX4_WINDOW_BYTES];
};

extern const fsimg_reader_ops_t fsimg_qnx4_reader_ops;

fsimg_qnx4_reader_t *qnx4_state(const fsimg_reader_t *);
fsimg_status_t qnx4_corrupt(fsimg_error_t *, const char *);
fsimg_status_t qnx4_unsupported(fsimg_error_t *, const char *);
uint64_t qnx4_block_offset(uint32_t);

/* codec.c: pure decoders over byte buffers. */
uint16_t qnx4_u16(const unsigned char *);
uint32_t qnx4_u32(const unsigned char *);
bool qnx4_slot_free(const unsigned char *);
bool qnx4_slot_link(const unsigned char *);
void qnx4_inode_decode(const unsigned char *, qnx4_inode_t *);
void qnx4_link_decode(const unsigned char *, qnx4_link_t *);
void qnx4_xblock_decode(const unsigned char *, qnx4_xblock_t *);
void qnx4_longname_decode(const unsigned char *, qnx4_longname_t *);
bool qnx4_bytes_zero(const unsigned char *, size_t);

/* volume.c: geometry, the root block and the format-owned objects. */
fsimg_status_t qnx4_volume_open(fsimg_reader_t *, fsimg_error_t *);
/* descriptor.c: the root block's identity, for the probe and for open. */
fsimg_status_t qnx4_root_block_identity(const unsigned char *, uint64_t,
    fsimg_error_t *);
fsimg_status_t qnx4_root_fixed_slot(fsimg_reader_t *, unsigned,
    const unsigned char *, fsimg_error_t *);

/* extent.c: the only interpreter of extents and extent blocks. */
bool qnx4_extents_consistent(const qnx4_inode_t *);
fsimg_status_t qnx4_index_build(fsimg_reader_t *, const qnx4_inode_t *,
    qnx4_walk_policy_t, fsimg_extent_index_t **, fsimg_error_t *);
fsimg_status_t qnx4_index_read(fsimg_reader_t *, fsimg_extent_index_t *,
    uint64_t, void *, size_t, fsimg_error_t *);
fsimg_status_t qnx4_locator_build(fsimg_reader_t *, fsimg_extent_index_t *,
    qnx4_locator_t *, fsimg_error_t *);
void qnx4_locator_free(qnx4_locator_t *);
fsimg_status_t qnx4_locator_find(const qnx4_locator_t *, uint32_t, bool *,
    uint64_t *, fsimg_error_t *);

/* bitmap.c */
fsimg_status_t qnx4_bitmap_open(fsimg_reader_t *, fsimg_error_t *);
fsimg_status_t qnx4_blocks_allocated(fsimg_reader_t *, uint32_t, uint32_t,
    fsimg_error_t *);

/* inode.c */
fsimg_status_t qnx4_record_read(fsimg_reader_t *, uint64_t, unsigned char *,
    fsimg_error_t *);
fsimg_status_t qnx4_inode_validate(const qnx4_inode_t *, qnx4_place_t,
    fsimg_error_t *);
fsimg_status_t qnx4_link_resolve(fsimg_reader_t *, const qnx4_link_t *,
    uint64_t *, uint64_t *, qnx4_inode_t *, fsimg_error_t *);
fsimg_status_t qnx4_object_read(fsimg_reader_t *, uint64_t, qnx4_inode_t *,
    qnx4_place_t *, uint64_t *, fsimg_error_t *);
fsimg_status_t qnx4_inode_entry(const qnx4_inode_t *, fsimg_entry_t *,
    fsimg_error_t *);

/* names.c */
fsimg_status_t qnx4_name_profile(const unsigned char *, size_t,
    fsimg_error_t *);
bool qnx4_label_valid(const unsigned char *, size_t);
fsimg_status_t qnx4_direct_name(const qnx4_inode_t *, char *, fsimg_error_t *);
fsimg_status_t qnx4_link_name(fsimg_reader_t *, const qnx4_link_t *, uint64_t,
    char *, fsimg_error_t *);

/* directory.c. A slot visitor receives the slot's index in its directory,
 * its record position and its 64 bytes.
 */
typedef fsimg_status_t (*qnx4_slot_fn_t)(void *, uint64_t, uint64_t,
    const unsigned char *, fsimg_error_t *);

fsimg_status_t qnx4_slots_scan(fsimg_reader_t *, fsimg_extent_index_t *,
    uint64_t, qnx4_slot_fn_t, void *, fsimg_error_t *);
fsimg_status_t qnx4_dot_target(const unsigned char *, bool, uint64_t *,
    fsimg_error_t *);
fsimg_status_t qnx4_prepare(fsimg_reader_t *, fsimg_directory_build_t *,
    fsimg_error_t *);
fsimg_status_t qnx4_replay(fsimg_reader_t *, const char *,
    const unsigned char *, size_t, fsimg_error_t *);
fsimg_status_t qnx4_index_current(fsimg_reader_t *, qnx4_walk_policy_t,
    fsimg_error_t *);
fsimg_status_t qnx4_keys(fsimg_reader_t *, const char *,
    const unsigned char *, size_t, fsimg_directory_keys_t *, fsimg_error_t *);
fsimg_status_t qnx4_key(fsimg_reader_t *, const char *, unsigned char *,
    size_t *, fsimg_error_t *);

/* verify.c. The two accounting calls do nothing outside verification. */
fsimg_status_t qnx4_verify(fsimg_reader_t *, unsigned,
    fsimg_verify_report_t *, fsimg_error_t *);
fsimg_status_t qnx4_verify_name(fsimg_reader_t *, const qnx4_inode_t *,
    qnx4_place_t, uint64_t, fsimg_error_t *);
fsimg_status_t qnx4_verify_directory(fsimg_reader_t *, const qnx4_inode_t *,
    uint64_t, fsimg_error_t *);

#endif
