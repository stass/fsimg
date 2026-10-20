/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_QNX6_H
#define FSIMG_QNX6_H

#include "reader.h"

/* The classic QNX6 Power-Safe profile (specs/design-qnx6.md and
 * specs/design-qnx6-driver.md). Offsets and sizes are in bytes.
 */
#define QNX6_MAGIC		UINT32_C(0x68191122)
#define QNX6_HEADER_OFFSET	8192u
#define QNX6_HEADER_BYTES	512u
#define QNX6_HEADER_AREA	4096u
#define QNX6_DATA_ORIGIN	12288u
#define QNX6_PROBE_BYTES	(QNX6_HEADER_OFFSET + QNX6_HEADER_BYTES)
#define QNX6_VERSION		4u
#define QNX6_INODE_BYTES	128u
#define QNX6_SLOT_BYTES		32u
#define QNX6_ROOT_SLOTS		16u
#define QNX6_SENTINEL		UINT32_C(0xffffffff)
#define QNX6_DEPTH_MAX		5u
#define QNX6_SYSTEM_DEPTH_MAX	2u
#define QNX6_INLINE_NAME_MAX	27u
#define QNX6_LONG_NAME_MIN	28u
#define QNX6_NAME_MAX		510u
#define QNX6_LONG_NAME_MARKER	0xffu
#define QNX6_WINDOW_BYTES	4096u
#define QNX6_BUFFER_BYTES	65536u
#define QNX6_ROOT_INODE		1u

/* Header flags. Long-name checksums are a feature of the volume. The
 * mounted bit is snapshot state: a read-write mount sets it, some releases
 * commit it, and the two headers may differ in it.
 */
#define QNX6_FLAG_MOUNTED	UINT32_C(0x1)
#define QNX6_FLAG_CHECKSUM	UINT32_C(0x100)

/* Inode status bits and mode types. */
#define QNX6_STATUS_ALLOCATED	0x1u
#define QNX6_STATUS_CREATED	0x2u
#define QNX6_MODE_TYPE		0170000u
#define QNX6_MODE_FIFO		0010000u
#define QNX6_MODE_DIRECTORY	0040000u
#define QNX6_MODE_FILE		0100000u
#define QNX6_MODE_SYMLINK	0120000u

/* A block tree: the logical length of its object, sixteen root references
 * and the depth of indirection below them.
 */
typedef struct qnx6_tree {
	uint64_t	 length;
	uint32_t	 ref[QNX6_ROOT_SLOTS];
	uint8_t		 depth;
} qnx6_tree_t;

typedef struct qnx6_header {
	uint64_t	 serial;
	qnx6_tree_t	 inodes;
	qnx6_tree_t	 bitmap;
	qnx6_tree_t	 names;
	uint32_t	 format_time;
	uint32_t	 sync_time;
	uint32_t	 flags;
	uint32_t	 block_size;
	uint32_t	 inode_count;
	uint32_t	 free_inodes;
	uint32_t	 block_count;
	uint32_t	 free_blocks;
	uint32_t	 groups;
	uint16_t	 version;
	uint16_t	 reserve;
	unsigned char	 uuid[16];
	/* Header bytes outside the classic layout are unused: the fourth tree
	 * is empty and the extension area is zero. Each check is false when the
	 * stored bytes are something else.
	 */
	bool		 descriptors_classic;
	bool		 spare_tree_empty;
	bool		 extension_zero;
} qnx6_header_t;

typedef struct qnx6_inode {
	uint64_t	 size;
	qnx6_tree_t	 tree;
	uint32_t	 uid;
	uint32_t	 gid;
	uint32_t	 created;
	uint32_t	 modified;
	uint32_t	 accessed;
	uint32_t	 changed;
	uint16_t	 mode;
	uint16_t	 links;
	uint8_t		 status;
	bool		 tail_zero;
} qnx6_inode_t;

/* A directory slot names an inode with an inline name of 1..27 bytes or a
 * long-name record index and checksum. Unused slots are not decoded further.
 */
typedef struct qnx6_slot {
	uint32_t	 inode;
	uint32_t	 long_index;
	uint32_t	 checksum;
	uint8_t		 length;
	bool		 long_name;
	bool		 long_padding_zero;
	unsigned char	 name[QNX6_INLINE_NAME_MAX];
} qnx6_slot_t;

/* Validated volume geometry. Blocks are numbered from the data origin D;
 * the system reservation R and its twin occupy blocks [0, 2R), and every
 * other object lives in [2R, N).
 */
typedef struct qnx6_geometry {
	uint64_t	 volume_bytes;
	uint64_t	 second_header;
	uint32_t	 block_size;
	uint32_t	 pointers;
	uint32_t	 blocks;
	uint32_t	 inode_count;
	uint32_t	 reservation;
	bool		 big_endian;
} qnx6_geometry_t;

/* The layout of a tree the writer emits (tree.c): n leaves at minimal
 * depth L, count[j] indirect blocks at level j (count[0] = 1 for the root
 * descriptor, count[L + 1] = n) and their indirect total.
 */
typedef struct qnx6_shape {
	uint64_t	 leaves;
	uint64_t	 count[QNX6_DEPTH_MAX + 2];
	uint64_t	 indirect;
	uint32_t	 pointers;
	unsigned	 depth;
} qnx6_shape_t;

/* The two block domains a tree may occupy. */
typedef enum qnx6_domain {
	QNX6_DOMAIN_SYSTEM = 1,
	QNX6_DOMAIN_CONTENT
} qnx6_domain_t;

/* The role of an owned block run in verification. */
typedef enum qnx6_role {
	QNX6_ROLE_INDIRECT = 1,
	QNX6_ROLE_BITMAP,
	QNX6_ROLE_INODES,
	QNX6_ROLE_NAMES,
	QNX6_ROLE_DIRECTORY,
	QNX6_ROLE_PAYLOAD,
	QNX6_ROLE_SYMLINK
} qnx6_role_t;

/* Owner identities of the system trees, above the 32-bit inode space. */
#define QNX6_OWNER_BITMAP	(UINT64_C(1) << 32)
#define QNX6_OWNER_INODES	(QNX6_OWNER_BITMAP + 1)
#define QNX6_OWNER_NAMES	(QNX6_OWNER_BITMAP + 2)

/* How a tree walk treats each block it visits, beyond the range, sentinel,
 * domain and cycle checks it always applies. Outside the system trees one
 * snapshot uses every block once, so the first walk of each directory,
 * long-name tree and inode claims every block it visits, and a block
 * claimed twice is corruption whichever object came first. A hard link's
 * later names walk a tree its first name already claimed.
 * BOOTSTRAP	record every block, before the bitmap exists
 * ALLOCATED	check allocation only: the system trees, a later name of an
 *		inode, verification's second walks
 * EXCLUSIVE	check allocation and claim the block
 */
typedef enum qnx6_walk_policy {
	QNX6_WALK_BOOTSTRAP = 1,
	QNX6_WALK_ALLOCATED,
	QNX6_WALK_EXCLUSIVE
} qnx6_walk_policy_t;

/* One object a tree walk indexes. When records is set and verification
 * collects ownership, the walk records the runs owner owns; each tree is
 * recorded by exactly one walk, so the merge can treat any overlap as
 * corruption. The recording walks are verification's walks of the system
 * and long-name trees, each directory's preparation and each inode's first
 * name; every other walk, a hard link's later names included, leaves
 * records clear. A missing recording walk shows as an allocated block
 * without an owner, an extra one as an overlap. role names the
 * object's leaves in diagnostics; indirect blocks have the indirect role.
 */
typedef struct qnx6_object {
	const qnx6_tree_t *tree;
	uint64_t	 owner;
	qnx6_domain_t	 domain;
	qnx6_role_t	 role;
	qnx6_walk_policy_t policy;
	bool		 records;
} qnx6_object_t;

/* Driver state behind the reader's private pointer, from open to close.
 * The three system indexes come from the selected header. current is the
 * entry replay made current, with its parent directory's inode number, and
 * file indexes its content when it is a file or symbolic link, built at
 * replay for an inode's first name and at the first read for a later one
 * (later_name); a first name with an empty tree has none.
 * claimed holds the blocks exclusive walks claimed (see qnx6_walk_policy_t)
 * and exposed the non-directory inodes whose first name was replayed.
 * owners and references exist only during full verification.
 */
typedef struct fsimg_qnx6_reader fsimg_qnx6_reader_t;

struct fsimg_qnx6_reader {
	qnx6_geometry_t	 geometry;
	qnx6_header_t	 header;
	qnx6_inode_t	 current;
	fsimg_extent_domain_t system_domain;
	fsimg_extent_index_t *bitmap;
	fsimg_extent_index_t *inodes;
	fsimg_extent_index_t *names;
	fsimg_extent_index_t *file;
	fsimg_bitset_t	*claimed;
	fsimg_bitset_t	*exposed;
	fsimg_stream_t	*record;
	fsimg_stream_t	*owners;
	fsimg_stream_t	*references;
	uint64_t	 bitmap_window_offset;
	uint32_t	 current_inode;
	uint32_t	 current_parent;
	bool		 later_name;
	size_t		 bitmap_window_length;
	bool		 bitmap_ready;
	unsigned char	 bitmap_window[QNX6_WINDOW_BYTES];
};

extern const fsimg_reader_ops_t fsimg_qnx6_reader_ops;

fsimg_qnx6_reader_t *qnx6_state(const fsimg_reader_t *);
fsimg_status_t qnx6_corrupt(fsimg_error_t *, const char *);
fsimg_status_t qnx6_unsupported(fsimg_error_t *, const char *);

/* codec.c: pure decoders and encoders over byte buffers. */
uint16_t qnx6_u16(const unsigned char *, bool);
uint32_t qnx6_u32(const unsigned char *, bool);
uint64_t qnx6_u64(const unsigned char *, bool);
uint32_t qnx6_crc32(const unsigned char *, size_t);
uint32_t qnx6_name_checksum(const unsigned char *, size_t);
bool qnx6_magic_order(const unsigned char *, bool *);
void qnx6_header_decode(const unsigned char *, bool, qnx6_header_t *);
void qnx6_inode_decode(const unsigned char *, bool, qnx6_inode_t *);
void qnx6_slot_decode(const unsigned char *, bool, qnx6_slot_t *);
void qnx6_put16(unsigned char *, uint16_t, bool);
void qnx6_put32(unsigned char *, uint32_t, bool);
void qnx6_put64(unsigned char *, uint64_t, bool);
void qnx6_header_encode(const qnx6_header_t *, bool, unsigned char *);
void qnx6_inode_encode(const qnx6_inode_t *, bool, unsigned char *);
void qnx6_slot_encode(const qnx6_slot_t *, bool, unsigned char *);

/* header.c */
fsimg_status_t qnx6_headers_read(fsimg_reader_t *, fsimg_error_t *);

/* tree.c: shape arithmetic and the one interpreter of block references. */
uint64_t qnx6_tree_leaves(uint64_t, uint32_t);
bool qnx6_tree_min_depth(uint64_t, uint32_t, unsigned *);
bool qnx6_tree_blocks(uint64_t, uint32_t, unsigned, uint64_t *);
bool qnx6_shape(uint64_t, uint32_t, qnx6_shape_t *);
uint64_t qnx6_shape_position(const qnx6_shape_t *, unsigned, uint64_t);
void qnx6_shape_refs(const qnx6_shape_t *, uint32_t, unsigned, uint64_t,
    uint32_t *);
fsimg_status_t qnx6_index_build(fsimg_reader_t *, const qnx6_object_t *,
    fsimg_extent_index_t **, fsimg_error_t *);
fsimg_status_t qnx6_index_read(fsimg_reader_t *, fsimg_extent_index_t *,
    uint64_t, void *, size_t, fsimg_error_t *);

/* bitmap.c */
fsimg_status_t qnx6_bitmap_open(fsimg_reader_t *, fsimg_error_t *);
fsimg_status_t qnx6_block_allocated(fsimg_reader_t *, uint32_t, bool *,
    fsimg_error_t *);

/* inode.c */
fsimg_status_t qnx6_inode_read(fsimg_reader_t *, uint32_t, qnx6_inode_t *,
    fsimg_error_t *);
fsimg_status_t qnx6_inode_entry(const qnx6_inode_t *, fsimg_entry_t *,
    fsimg_error_t *);

/* names.c */
fsimg_status_t qnx6_name_profile(const unsigned char *, size_t,
    fsimg_error_t *);
fsimg_status_t qnx6_slot_name(fsimg_reader_t *, const qnx6_slot_t *, char *,
    fsimg_error_t *);

/* directory.c */
fsimg_status_t qnx6_prepare(fsimg_reader_t *, fsimg_directory_build_t *,
    fsimg_error_t *);
fsimg_status_t qnx6_replay(fsimg_reader_t *, const char *,
    const unsigned char *, size_t, fsimg_error_t *);
fsimg_status_t qnx6_index_current(fsimg_reader_t *, qnx6_walk_policy_t,
    fsimg_error_t *);
fsimg_status_t qnx6_keys(fsimg_reader_t *, const char *,
    const unsigned char *, size_t, fsimg_directory_keys_t *, fsimg_error_t *);
fsimg_status_t qnx6_key(fsimg_reader_t *, const char *, unsigned char *,
    size_t *, fsimg_error_t *);

/* verify.c */
fsimg_status_t qnx6_own(fsimg_reader_t *, uint32_t, uint32_t, uint64_t,
    fsimg_error_t *);
fsimg_status_t qnx6_verify(fsimg_reader_t *, unsigned,
    fsimg_verify_report_t *, fsimg_error_t *);

#endif
