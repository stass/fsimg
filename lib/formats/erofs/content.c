/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <strings.h>

#include "erofs.h"

/*
 * Content: the logical bytes of an inode, located by erofs_map(). Two
 * readers use them. Common delivery asks resolve for the extent holding one
 * offset of the current file and reads it itself: a stored extent and a
 * plain cluster stored shifted are mapped volume bytes, a chunk hole a zero
 * extent, an inline tail is copied into the tail window, and any other
 * cluster is decoded into the content window. Only resolve writes those two
 * windows, and it holds the one it filled, so the bytes delivery copies
 * cannot be replaced by another lookup. The driver's own readers, directory
 * preparation and symbolic-link replay, read a logical range of any inode
 * into a buffer of theirs with erofs_content_read(); they do not read
 * compressed inodes.
 */

static fsimg_status_t	extent_read(fsimg_reader_t *, const erofs_inode_t *,
			    const erofs_extent_t *, uint64_t, size_t, void *,
			    fsimg_error_t *);

/*
 * The extent of the current file holding offset, below its initialized
 * length, which replay set to its size. A stored extent is returned mapped:
 * delivery reads it through bounded I/O inside the content domain, and so
 * is a plain cluster stored shifted, whose extent starts at its first stored
 * byte. A hole is returned as a zero extent, which delivery fills with
 * zeros. An inline tail is read from the inode's body into the tail window,
 * and any other cluster decoded into the content window; either is returned
 * held, valid until the next resolve or advance.
 */
fsimg_status_t
erofs_resolve(fsimg_reader_t *reader, uint64_t offset, fsimg_extent_t *out,
    fsimg_error_t *error)
{
	fsimg_erofs_reader_t *erofs = erofs_state(reader);
	const erofs_inode_t *file = &erofs->current;
	erofs_extent_t extent;
	fsimg_status_t status;

	status = erofs_map(reader, file, offset, &extent, error);
	if (status != FSIMG_OK)
		return (status);

	switch (extent.kind) {
	case EROFS_EXTENT_STORED:
		*out = (fsimg_extent_t){
			.logical_offset = extent.logical_offset,
			.image_offset = extent.storage_offset,
			.length = extent.length
		};
		return (FSIMG_OK);
	case EROFS_EXTENT_ZERO:
		*out = (fsimg_extent_t){
			.logical_offset = extent.logical_offset,
			.image_offset = FSIMG_EXTENT_ZERO,
			.length = extent.length
		};
		return (FSIMG_OK);
	case EROFS_EXTENT_INLINE:
		/* A tail is shorter than a block, the window's size. */
		status = erofs_body_read(reader, file, extent.storage_offset,
		    (size_t)extent.length, erofs->tail_window, error);
		if (status != FSIMG_OK)
			return (status);
		*out = (fsimg_extent_t){
			.logical_offset = extent.logical_offset,
			.image_offset = 0,
			.length = extent.length
		};
		reader->held = erofs->tail_window;
		return (FSIMG_OK);
	case EROFS_EXTENT_CLUSTER:
		*out = (fsimg_extent_t){
			.logical_offset = extent.logical_offset,
			.image_offset = extent.storage_offset,
			.length = extent.length
		};
		if (extent.cluster.mode == EROFS_CLUSTER_SHIFTED)
			return (FSIMG_OK);
		status = erofs_window_fill(reader, &erofs->content_window,
		    &extent.cluster, error);
		if (status != FSIMG_OK)
			return (status);
		out->image_offset = 0;
		reader->held = erofs->content_window.output;
		return (FSIMG_OK);
	}
	return (erofs_reject(error, FSIMG_EINVAL, "extent kind"));
}

/*
 * length bytes of inode's content from logical offset into out, extent by
 * extent. The range must lie inside the inode's size: callers derive it
 * from the size, so a range past it is a driver defect, FSIMG_EINVAL.
 */
fsimg_status_t
erofs_content_read(fsimg_reader_t *reader, const erofs_inode_t *inode,
    uint64_t offset, size_t length, void *out, fsimg_error_t *error)
{
	unsigned char *cursor = out;
	erofs_extent_t extent;
	uint64_t extent_end;
	size_t piece;
	fsimg_status_t status;

	if (!fsimg_range_fits(offset, length, inode->size))
		return (erofs_reject(error, FSIMG_EINVAL,
		    "content read past the inode's size"));

	while (length > 0) {
		status = erofs_map(reader, inode, offset, &extent, error);
		if (status != FSIMG_OK)
			return (status);
		extent_end = extent.logical_offset + extent.length;
		piece = extent_end - offset < length ?
		    (size_t)(extent_end - offset) : length;
		status = extent_read(reader, inode, &extent, offset, piece,
		    cursor, error);
		if (status != FSIMG_OK)
			return (status);
		cursor += piece;
		offset += piece;
		length -= piece;
	}
	return (FSIMG_OK);
}

/*
 * length bytes from logical offset of one extent of inode, which holds them
 * all. A stored extent is read from the volume, where a range past the
 * input's end is a truncated volume; an inline one from the inode's body;
 * a zero extent is filled with zeros without reading. The driver's readers do not read compressed
 * directories and symbolic links, which are unsupported.
 */
static fsimg_status_t
extent_read(fsimg_reader_t *reader, const erofs_inode_t *inode,
    const erofs_extent_t *extent, uint64_t offset, size_t length, void *out,
    fsimg_error_t *error)
{
	uint64_t within = offset - extent->logical_offset;
	size_t transferred;
	fsimg_status_t status;

	switch (extent->kind) {
	case EROFS_EXTENT_STORED:
		status = fsimg_reader_bytes(reader,
		    extent->storage_offset + within, out, length, &transferred,
		    error);
		if (status != FSIMG_OK)
			return (status);
		if (transferred != length)
			return (erofs_reject(error, FSIMG_ECORRUPT,
			    "data read truncated"));
		return (FSIMG_OK);
	case EROFS_EXTENT_INLINE:
		return (erofs_body_read(reader, inode,
		    extent->storage_offset + within, length, out, error));
	case EROFS_EXTENT_CLUSTER:
		return (erofs_reject(error, FSIMG_EUNSUPPORTED,
		    "compressed directory or symbolic link"));
	case EROFS_EXTENT_ZERO:
		bzero(out, length);
		return (FSIMG_OK);
	}
	return (erofs_reject(error, FSIMG_EINVAL, "extent kind"));
}
