/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include "erofs.h"

/*
 * The mapper: the one function that turns a logical offset of an inode into
 * storage, so every extent the driver reads, for a directory, a symbolic
 * link or file content, is bounded here once.
 *
 * A flat inode stores its size bytes contiguously from its start block, the
 * layout word. A flat-inline inode stores its first floor(size / B) blocks
 * the same way and its last size mod B bytes, the tail, inline at the end of
 * its body. For B = 4096 and size 10000 the stored extent is [0, 8192) from
 * the start block and the inline extent [8192, 10000) from the body's layout
 * offset. The start word addresses something only when a stored extent
 * exists, so it is read only when the requested offset falls in one: never
 * in an empty inode, and never in a flat-inline inode shorter than a block,
 * where producers leave 0xFFFFFFFF.
 *
 * The chunk-based and compressed layouts are not read: a volume using them
 * does not open, and an inode claiming one is refused here as well.
 */

static fsimg_status_t	stored_extent(const fsimg_reader_t *,
			    const erofs_inode_t *, uint64_t, erofs_extent_t *,
			    fsimg_error_t *);

/*
 * The extent of inode holding logical byte offset, which must be below the
 * inode's size: a caller deriving an offset past it is a driver defect,
 * FSIMG_EINVAL. Every extent returned lies inside its storage.
 */
fsimg_status_t
erofs_map(const fsimg_reader_t *reader, const erofs_inode_t *inode,
    uint64_t offset, erofs_extent_t *out, fsimg_error_t *error)
{
	const fsimg_erofs_reader_t *erofs = erofs_state(reader);
	uint64_t stored_bytes;

	if (offset >= inode->size)
		return (erofs_reject(error, FSIMG_EINVAL,
		    "mapping offset past the inode's size"));

	switch (inode->layout) {
	case EROFS_LAYOUT_FLAT:
		return (stored_extent(reader, inode, inode->size, out, error));
	case EROFS_LAYOUT_FLAT_INLINE:
		/* The tail is shorter than a block: the inode decoder refused
		 * a flat-inline size that is a whole number of blocks. */
		stored_bytes = inode->size - inode->size % erofs->block_bytes;
		if (offset < stored_bytes)
			return (stored_extent(reader, inode, stored_bytes, out,
			    error));
		*out = (erofs_extent_t){
			.logical_offset = stored_bytes,
			.storage_offset = inode->layout_offset,
			.length = inode->size - stored_bytes,
			.kind = EROFS_EXTENT_INLINE
		};
		return (FSIMG_OK);
	case EROFS_LAYOUT_CHUNK_BASED:
		return (erofs_reject(error, FSIMG_EUNSUPPORTED,
		    "chunk-based data layout"));
	case EROFS_LAYOUT_COMPRESSED_FULL:
	case EROFS_LAYOUT_COMPRESSED_COMPACT:
		return (erofs_reject(error, FSIMG_EUNSUPPORTED,
		    "compressed data layout"));
	}
	return (erofs_reject(error, FSIMG_ECORRUPT, "inode data layout"));
}

/*
 * The stored extent [0, stored_bytes) of a flat or flat-inline inode, from
 * its start block. The start block has at most 48 bits and B at most 2^12,
 * so its byte offset cannot overflow. The extent occupies whole blocks, and
 * all of them must lie inside the content domain, the domain's whole
 * blocks. A start word of 0xFFFFFFFF, which producers write only where it
 * is not read, is refused outright: a volume declaring no block count has
 * the input's length as its domain, which can reach that block.
 */
static fsimg_status_t
stored_extent(const fsimg_reader_t *reader, const erofs_inode_t *inode,
    uint64_t stored_bytes, erofs_extent_t *out, fsimg_error_t *error)
{
	const fsimg_erofs_reader_t *erofs = erofs_state(reader);
	uint64_t block_mask, start_block, storage_offset, whole_bytes;

	if (inode->layout_high == 0 && inode->layout_word == UINT32_MAX)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "data start block 0xFFFFFFFF"));
	start_block = (uint64_t)inode->layout_high << 32 | inode->layout_word;
	storage_offset = start_block << erofs->super.blkszbits;

	/*
	 * The content domain starts at volume byte 0 and is a whole number
	 * of blocks, so once stored_bytes fits it, rounding it up to whole
	 * blocks stays within the domain's length and cannot overflow.
	 */
	if (stored_bytes > reader->content_domain.length)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "data blocks outside the volume"));
	block_mask = (uint64_t)erofs->block_bytes - 1;
	whole_bytes = (stored_bytes + block_mask) & ~block_mask;
	if (!fsimg_range_fits(storage_offset, whole_bytes,
	    reader->content_domain.length))
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "data blocks outside the volume"));

	*out = (erofs_extent_t){
		.logical_offset = 0,
		.storage_offset = storage_offset,
		.length = stored_bytes,
		.kind = EROFS_EXTENT_STORED
	};
	return (FSIMG_OK);
}
