/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_IFS_H
#define FSIMG_IFS_H

#include "reader.h"

/* The QNX image filesystem, ImageFS (specs/design-ifs.md and
 * specs/design-ifs-driver.md). An image is image_size bytes: a fixed
 * 0x58-byte header, the NUL-terminated mountpoint padded to dir_offset, a
 * table of records ending in a four-byte zero terminator at hdr_dir_size,
 * the file data, and a four-byte checksum trailer. Every record carries a
 * full relative path; there is no directory storage, so the hierarchy is
 * derived from the sorted paths. Offsets are bytes from the start of the
 * image, and every multi-byte integer is in the byte order the header's
 * flags byte selects.
 */
#define IFS_HEADER_BYTES	0x58u
#define IFS_SIGNATURE		"imagefs"
#define IFS_SIGNATURE_BYTES	7u
#define IFS_FLAGS_LITTLE	0x04u
#define IFS_FLAGS_BIG		0x05u
#define IFS_BOOT_SLOTS		4u	/* boot_ino[0..3] */
/* The references: the boot slots in order, then script_ino. */
#define IFS_REFERENCES		(IFS_BOOT_SLOTS + 1)
#define IFS_SPARE_WORDS		10u

/* Header field offsets. */
enum {
	IFS_HDR_FLAGS = 0x07,
	IFS_HDR_IMAGE_SIZE = 0x08,
	IFS_HDR_DIR_SIZE = 0x0c,
	IFS_HDR_DIR_OFFSET = 0x10,
	IFS_HDR_BOOT_INO = 0x14,
	IFS_HDR_SCRIPT_INO = 0x24,
	IFS_HDR_CHAIN_PADDR = 0x28,
	IFS_HDR_SPARE = 0x2c,
	IFS_HDR_MOUNT_FLAGS = 0x54
};

/* Record field offsets: the 24-byte prefix every record starts with, then
 * each kind's payload and the offset of its path.
 */
enum {
	IFS_REC_LENGTH = 0x00,
	IFS_REC_EXTATTR = 0x02,
	IFS_REC_INODE = 0x04,
	IFS_REC_MODE = 0x08,
	IFS_REC_GID = 0x0c,
	IFS_REC_UID = 0x10,
	IFS_REC_MTIME = 0x14,
	IFS_REC_PREFIX = 0x18,
	IFS_REC_DIR_PATH = 0x18,
	IFS_REC_FILE_OFFSET = 0x18,
	IFS_REC_FILE_SIZE = 0x1c,
	IFS_REC_FILE_PATH = 0x20,
	IFS_REC_LINK_OFFSET = 0x18,
	IFS_REC_LINK_SIZE = 0x1a,
	IFS_REC_LINK_PATH = 0x1c,
	IFS_REC_FIFO_WORDS = 0x18,
	IFS_REC_FIFO_PATH = 0x20
};

/* The one length limit of every stored path string: QNX's path maximum. */
#define IFS_PATH_MAX		1024u
/* A path of 1024 bytes holds at most 512 components. */
#define IFS_DEPTH_MAX		512u
#define IFS_LINK_TARGET_MAX	1024u
#define IFS_FILE_MAX		UINT32_C(0xffffffff)
#define IFS_VOLUME_MAX		UINT32_C(0xfffffffc)
/* The bytes of a record any decision reads: a link's path of up to 1024
 * bytes and its NUL, then its target of up to 1024 bytes and its NUL,
 * rounded up to whole words. Every byte past them is padding, never
 * interpreted.
 */
#define IFS_RECORD_DECODE_BYTES \
	(IFS_REC_LINK_PATH + 2 * (IFS_PATH_MAX + 1) + 2)
#define IFS_WINDOW_BYTES	8192u

#define IFS_MODE_TYPE		0170000u
#define IFS_MODE_FIFO		0010000u
#define IFS_MODE_DIRECTORY	0040000u
#define IFS_MODE_FILE		0100000u
#define IFS_MODE_SYMLINK	0120000u
#define IFS_MODE_MAX		0177777u

/* The top four bits of an inode word are loader flags, the rest the
 * inode number. Four nibbles occur natively.
 */
#define IFS_INODE_FLAG_SHIFT	28u
#define IFS_FLAG_NIBBLES	4u

/* The permission bits mkifs gives a directory record and fsimg gives a
 * directory the table does not record.
 */
#define IFS_IMPLICIT_MODE	0755u

typedef enum ifs_byte_order {
	IFS_LITTLE_ENDIAN = 0,
	IFS_BIG_ENDIAN = 1
} ifs_byte_order_t;

typedef enum ifs_kind {
	IFS_KIND_DIRECTORY = 1,
	IFS_KIND_FILE,
	IFS_KIND_SYMLINK,
	IFS_KIND_FIFO
} ifs_kind_t;

/* The fixed header, decoded. spare_zero is false when any of the ten spare
 * words is nonzero.
 */
typedef struct ifs_header {
	ifs_byte_order_t order;
	uint32_t	 image_size;
	uint32_t	 hdr_dir_size;
	uint32_t	 dir_offset;
	uint32_t	 boot_ino[IFS_BOOT_SLOTS];
	uint32_t	 script_ino;
	uint32_t	 chain_paddr;
	uint32_t	 mount_flags;
	uint8_t		 flags;
	bool		 spare_zero;
} ifs_header_t;

/* One validated record. path_offset and target_offset are relative to the
 * record's start; path_length counts the bytes before the path's NUL.
 * data_offset and data_length describe a regular file's range;
 * target_length is a symbolic link's sym_size.
 */
typedef struct ifs_record {
	ifs_kind_t	 kind;
	uint32_t	 length;
	uint32_t	 inode;
	uint32_t	 permissions;
	uint32_t	 uid;
	uint32_t	 gid;
	uint32_t	 mtime;
	uint32_t	 data_offset;
	uint32_t	 data_length;
	uint32_t	 path_offset;
	uint32_t	 path_length;
	uint32_t	 target_offset;
	uint32_t	 target_length;
} ifs_record_t;

/* The counts the table pass and the child pass collect, which info
 * reports. records_by_flag counts every record, shadowed and omitted ones
 * included, by its inode flag nibble 0x0, 0x8, 0xc and 0xe in that order.
 * shadowed counts the records the tree gives no node: each earlier record
 * of a duplicate path and each record below a non-directory. omitted
 * counts the roots of omitted subtrees, omitted_references the boot and
 * script references naming an omitted node. reference_matches and
 * reference_kind record, per reference slot, how many nodes carry its
 * inode word and the kind of the last one, for verification;
 * reference_shadowed whether a shadowed record carries it too, which info
 * counts as shadowed_references.
 */
typedef struct ifs_counts {
	uint64_t	 records_by_flag[IFS_FLAG_NIBBLES];
	uint64_t	 implicit_directories;
	uint64_t	 shadowed;
	uint64_t	 omitted;
	uint64_t	 omitted_references;
	uint64_t	 reference_matches[IFS_REFERENCES];
	ifs_kind_t	 reference_kind[IFS_REFERENCES];
	bool		 reference_shadowed[IFS_REFERENCES];
} ifs_counts_t;

/* The node replay made current: its index, kind, the table offset of its
 * record (zero for a synthesized directory) and its record when it has one.
 */
typedef struct ifs_current {
	ifs_record_t	 record;
	uint64_t	 node;
	uint32_t	 table_offset;
	ifs_kind_t	 kind;
} ifs_current_t;

/* Driver state behind the reader's private pointer, from open to close.
 *
 * The hierarchy lives in scratch: children is the sealed child stream,
 * one record per node other than the root, ordered by parent node and
 * then by component, and groups is a paged array giving each node the
 * stream offset and count of its children. node_count counts the root.
 * file is the current regular file's or link's one-extent index, NULL for
 * every other entry and for an empty file. record holds the bytes of the
 * record replay last read.
 */
typedef struct fsimg_ifs_reader fsimg_ifs_reader_t;

struct fsimg_ifs_reader {
	ifs_header_t	 header;
	ifs_counts_t	 counts;
	ifs_current_t	 current;
	fsimg_stream_t	*children;
	fsimg_array_t	*groups;
	fsimg_extent_index_t *file;
	uint64_t	 volume_bytes;
	uint64_t	 node_count;
	bool		 mountpoint_valid;
	char		 mountpoint[IFS_PATH_MAX + 1];
	unsigned char	 record[IFS_RECORD_DECODE_BYTES];
};

extern const fsimg_reader_ops_t fsimg_ifs_reader_ops;

fsimg_ifs_reader_t *ifs_state(const fsimg_reader_t *);
fsimg_status_t ifs_corrupt(fsimg_error_t *, const char *);
fsimg_status_t ifs_unsupported(fsimg_error_t *, const char *);

/* records.c: pure codecs over byte buffers, without I/O. */
uint16_t ifs_u16(const unsigned char *, ifs_byte_order_t);
uint32_t ifs_u32(const unsigned char *, ifs_byte_order_t);
void ifs_put16(unsigned char *, uint16_t, ifs_byte_order_t);
void ifs_put32(unsigned char *, uint32_t, ifs_byte_order_t);
uint32_t ifs_path_offset(ifs_kind_t);
void ifs_record_place(ifs_record_t *);
uint64_t ifs_record_minimum(ifs_kind_t, uint32_t, uint32_t);
void ifs_header_encode(const ifs_header_t *, unsigned char *);
void ifs_record_encode(const ifs_record_t *, const unsigned char *,
    ifs_byte_order_t, unsigned char *);
fsimg_status_t ifs_header_decode(const unsigned char *, ifs_header_t *,
    fsimg_error_t *);
fsimg_status_t ifs_header_bounds(const ifs_header_t *, uint64_t,
    fsimg_error_t *);
uint32_t ifs_record_length(const unsigned char *, ifs_byte_order_t);
fsimg_status_t ifs_record_decode(const unsigned char *, size_t, uint32_t,
    ifs_byte_order_t, ifs_record_t *, fsimg_error_t *);
unsigned ifs_flag_index(uint32_t);
uint32_t ifs_reference(const ifs_header_t *, unsigned);

/* names.c: path components and the profile. */
fsimg_status_t ifs_path_check(const unsigned char *, size_t,
    fsimg_error_t *);
size_t ifs_component_end(const unsigned char *, size_t, size_t);
bool ifs_mountpoint_valid(const unsigned char *, size_t);

/* volume.c: the header, the mountpoint and the table pass. Each path
 * stream record holds the record's table offset, its inode word and its
 * kind, then its path bytes; tree.c decodes them.
 */
enum {
	IFS_PATH_ENTRY_TABLE_OFFSET = 0,	/* u32 */
	IFS_PATH_ENTRY_INODE = 4,		/* u32 */
	IFS_PATH_ENTRY_KIND = 8,		/* u8 */
	IFS_PATH_ENTRY_FIXED = 9		/* the path follows */
};
#define IFS_PATH_ENTRY_BYTES	(IFS_PATH_ENTRY_FIXED + IFS_PATH_MAX)
fsimg_status_t ifs_read_exact(fsimg_reader_t *, uint64_t, void *, size_t,
    fsimg_error_t *);
fsimg_status_t ifs_volume_open(fsimg_reader_t *, fsimg_stream_t **,
    fsimg_error_t *);
fsimg_status_t ifs_record_read(fsimg_reader_t *, uint32_t, ifs_record_t *,
    fsimg_error_t *);

/* tree.c: the derived hierarchy. The child stream's records are the
 * parent's and the node's indexes, the kind, the table offset and the
 * record's path length (both zero for a synthesized directory), then the
 * component bytes. A node's index exceeds its parent's: nodes are numbered
 * in preorder.
 */
enum {
	IFS_CHILD_PARENT = 0,		/* u64 */
	IFS_CHILD_NODE = 8,		/* u64 */
	IFS_CHILD_KIND = 16,		/* u8 */
	IFS_CHILD_TABLE_OFFSET = 17,	/* u32 */
	IFS_CHILD_PATH_LENGTH = 21,	/* u16, little-endian */
	IFS_CHILD_FIXED = 23		/* the component follows */
};
#define IFS_CHILD_BYTES		(IFS_CHILD_FIXED + IFS_PATH_MAX)
#define IFS_GROUP_BYTES		16u

typedef struct ifs_child {
	uint64_t	 parent;
	uint64_t	 node;
	uint32_t	 table_offset;
	uint32_t	 path_length;
	ifs_kind_t	 kind;
	size_t		 name_length;
	const unsigned char *name;
} ifs_child_t;

fsimg_status_t ifs_tree_build(fsimg_reader_t *, fsimg_stream_t *,
    fsimg_error_t *);
fsimg_status_t ifs_child_decode(const unsigned char *, size_t,
    ifs_child_t *, fsimg_error_t *);
fsimg_status_t ifs_group(fsimg_reader_t *, uint64_t, uint64_t *, uint64_t *,
    fsimg_error_t *);

/* directory.c */
fsimg_status_t ifs_prepare(fsimg_reader_t *, fsimg_directory_build_t *,
    fsimg_error_t *);
fsimg_status_t ifs_replay(fsimg_reader_t *, const char *,
    const unsigned char *, size_t, fsimg_error_t *);
fsimg_status_t ifs_keys(fsimg_reader_t *, const char *,
    const unsigned char *, size_t, fsimg_directory_keys_t *, fsimg_error_t *);
fsimg_status_t ifs_key(fsimg_reader_t *, const char *, unsigned char *,
    size_t *, fsimg_error_t *);

/* verify.c */
fsimg_status_t ifs_verify(fsimg_reader_t *, unsigned, fsimg_verify_report_t *,
    fsimg_error_t *);

#endif
