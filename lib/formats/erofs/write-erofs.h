/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_WRITE_EROFS_H
#define FSIMG_WRITE_EROFS_H

#include "emit.h"
#include "erofs.h"

/*
 * The block sizes the writer emits. The kernel's acceptance of the smaller
 * sizes the reader takes is unconfirmed, so the writer emits 4096 only.
 */
#define EROFS_WRITE_BLOCK_BYTES		4096u
#define EROFS_WRITE_BLOCK_SIZES		(1u << 12)
/* The 16-byte volume name field keeps a terminating NUL. */
#define EROFS_WRITE_LABEL_MAX		(EROFS_NAME_FIELD_BYTES - 1)
/*
 * The metadata area starts at the first 32-byte boundary after a
 * superblock without extension slots: byte 1152, NID 36. The root's inode
 * is the first, so its NID fits the superblock's 16-bit field.
 */
#define EROFS_WRITE_METADATA_START	(EROFS_SUPER_OFFSET + EROFS_SUPER_BASE_BYTES)
/* Without 48-bit addressing a block number is 32 bits wide. */
#define EROFS_WRITE_BLOCKS_MAX		UINT64_C(0xffffffff)

/*
 * One plan row per catalog ID, row 0 the root. A row holds everything the
 * inode and its data need apart from the name, which stays in the catalog:
 *   size	content bytes: a file's size, a link's target length or a
 *		directory's packed entries (write-dir.c); zero for a FIFO
 *   nid	the inode's byte offset in the metadata area divided by 32,
 *		zero until metadata placement
 *   start_block
 *		the first block of the inode's data, its tail block included
 *		under the flat layout; zero when the inode stores no block,
 *		before data placement, and for an empty inode or a flat-inline
 *		inode under one block, whose layout word addresses nothing
 *   mtime, mtime_nsec
 *		the stored modification time, already chosen under the
 *		metadata policy (format time when absent or reproducible)
 *   uid, gid	the stored owner, zero when absent
 *   links	1 for a non-directory; a directory's 2 plus its subdirectories
 *   mode	type and permission bits
 *   layout	EROFS_LAYOUT_FLAT or EROFS_LAYOUT_FLAT_INLINE, and extended
 *		the inode form, both fixed by metadata placement
 * The wire layout is little endian at the offsets ROW_* name in
 * write-plan.c, every unused byte zero.
 */
typedef struct erofs_row {
	uint64_t	 size;
	uint64_t	 nid;
	uint64_t	 start_block;
	int64_t		 mtime;
	uint32_t	 mtime_nsec;
	uint32_t	 uid;
	uint32_t	 gid;
	uint32_t	 links;
	uint16_t	 mode;
	erofs_layout_t	 layout;
	bool		 extended;
} erofs_row_t;

#define EROFS_ROW_BYTES		64u

/*
 * The plan: the row array and the volume the passes derive. format_time is
 * the superblock's epoch, with zero fixed nanoseconds; metadata_end is the
 * block-aligned byte offset where the data area begins; used_blocks counts
 * every block up to the end of the last inode's data and block_count the
 * volume's blocks, at least used_blocks. uuid is the stored identifier.
 */
typedef struct erofs_plan {
	fsimg_plan_t	 common;
	fsimg_array_t	*rows;
	uint64_t	 metadata_end;
	uint64_t	 used_blocks;
	uint64_t	 block_count;
	uint64_t	 inode_count;
	int64_t		 format_time;
	uint32_t	 block_bytes;
	unsigned char	 uuid[16];
} erofs_plan_t;

/*
 * One directory entry for packing: name_length bytes of name, and the
 * child's NID and file type, which measurement leaves zero.
 */
typedef struct erofs_dir_entry {
	const char	*name;
	size_t		 name_length;
	uint64_t	 nid;
	uint8_t		 file_type;
} erofs_dir_entry_t;

/*
 * A directory being packed into blocks of block_bytes, at most 4096 so a
 * name offset fits its 16-bit field. closed counts the
 * blocks finished so far; the open block after them holds entries entries
 * whose names take name_bytes. A block's entry count is known only once
 * it is full, and the names follow the whole entry array, so building
 * bytes stages the names apart: block receives the entry array, each
 * entry's name offset relative to the first name until the block closes,
 * and names the names in order. erofs_dir_pack_close() then moves the
 * names behind the array and fixes the offsets. Measuring packs with
 * block and names NULL and produces only the counts.
 */
typedef struct erofs_dir_pack {
	unsigned char	*block;
	unsigned char	*names;
	uint64_t	 closed;
	uint32_t	 block_bytes;
	uint32_t	 entries;
	uint32_t	 name_bytes;
} erofs_dir_pack_t;

/*
 * What erofs_dir_walk() does with each entry and block when it builds
 * bytes: resolve fills an entry's NID and file type from the catalog ID it
 * names (the walk holds its own copy of the name, so resolve may use the
 * catalog), and block receives each finished block, its index in the
 * directory and its valid length; the block_bytes it points to are zero
 * past that length. pack_buffer is twice block_bytes, the block and the
 * name staging area the packer assembles in, owned by the caller.
 */
typedef struct erofs_dir_builder {
	fsimg_status_t	(*resolve)(void *, uint64_t, erofs_dir_entry_t *,
			    fsimg_error_t *);
	fsimg_status_t	(*block)(void *, uint64_t, const unsigned char *,
			    size_t, fsimg_error_t *);
	void		*argument;
	unsigned char	*pack_buffer;
} erofs_dir_builder_t;

extern const fsimg_writer_ops_t fsimg_erofs_writer_ops;

fsimg_status_t erofs_row_get(erofs_plan_t *, uint64_t, erofs_row_t *,
    fsimg_error_t *);
fsimg_status_t erofs_row_set(erofs_plan_t *, uint64_t, const erofs_row_t *,
    fsimg_error_t *);
uint32_t erofs_row_body_bytes(const erofs_row_t *, uint32_t);
uint32_t erofs_row_fixed_bytes(const erofs_row_t *);
uint32_t erofs_row_tail_bytes(const erofs_row_t *, uint32_t);
uint64_t erofs_row_data_blocks(const erofs_row_t *, uint32_t);

void erofs_dir_pack_init(erofs_dir_pack_t *, uint32_t, unsigned char *);
bool erofs_dir_pack_fits(const erofs_dir_pack_t *, size_t);
void erofs_dir_pack_add(erofs_dir_pack_t *, const erofs_dir_entry_t *);
size_t erofs_dir_pack_close(erofs_dir_pack_t *);
uint64_t erofs_dir_pack_size(const erofs_dir_pack_t *);
fsimg_status_t erofs_dir_walk(erofs_plan_t *, uint64_t *, uint64_t,
    uint64_t, const erofs_dir_builder_t *, uint64_t *, fsimg_error_t *);

fsimg_status_t erofs_write_emit(fsimg_writer_t *, fsimg_emit_t *,
    fsimg_error_t *);

#endif
