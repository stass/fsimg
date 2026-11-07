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
 * plain cluster stored shifted in the volume's blocks are mapped volume
 * bytes, a chunk hole a zero extent, and any other cluster, an inline tail
 * included, is decoded or copied into the content window. Only resolve
 * writes that window, and it holds it, so the bytes delivery copies cannot
 * be replaced by another lookup. The driver's own readers, directory
 * preparation, symbolic-link replay, the long-prefix table and payload
 * verification, read a logical range of any inode into a buffer of theirs
 * with erofs_content_read(), copying clusters through the metadata window.
 * The metadata reader reads the metadata box's content the same way through
 * the box window, erofs_content_copy().
 */

static bool		cluster_mapped(const erofs_cluster_t *);
static fsimg_status_t	extent_read(fsimg_reader_t *, erofs_window_t *,
			    const erofs_extent_t *, uint64_t, size_t, void *, size_t *,
			    fsimg_error_t *);

/*
 * Whether delivery maps the cluster's bytes in the volume rather than
 * holding them in the content window: a plain cluster stored shifted in
 * blocks. Of all clusters, resolve maps exactly these and the content
 * reader counts exactly these as payload, so the two cannot disagree.
 */
static bool
cluster_mapped(const erofs_cluster_t *cluster)
{
	return (cluster->source == EROFS_SOURCE_BLOCKS &&
	    cluster->mode == EROFS_CLUSTER_SHIFTED);
}

/*
 * The extent of the current file holding offset, below its initialized
 * length, which replay set to its size. A stored extent is returned mapped:
 * delivery reads it through bounded I/O inside the content domain, and so
 * is a plain cluster stored shifted in blocks, whose extent starts at its
 * first stored byte. A hole is returned as a zero extent, which delivery
 * fills with zeros. Any other cluster is left in the content window and
 * returned held from its cluster_skip, valid until the next resolve or
 * advance.
 */
fsimg_status_t
erofs_resolve(fsimg_reader_t *reader, uint64_t offset, fsimg_extent_t *out,
    fsimg_error_t *error)
{
	fsimg_erofs_reader_t *erofs = erofs_state(reader);
	erofs_extent_t extent;
	fsimg_status_t status;

	status = erofs_map(reader, &erofs->current, offset, &extent, error);
	if (status != FSIMG_OK)
		return (status);

	*out = (fsimg_extent_t){
		.logical_offset = extent.logical_offset,
		.length = extent.length
	};
	switch (extent.kind) {
	case EROFS_EXTENT_STORED:
		out->image_offset = extent.storage_offset;
		return (FSIMG_OK);
	case EROFS_EXTENT_ZERO:
		out->image_offset = FSIMG_EXTENT_ZERO;
		return (FSIMG_OK);
	case EROFS_EXTENT_CLUSTER:
		if (cluster_mapped(&extent.cluster)) {
			out->image_offset = extent.cluster.storage_offset +
			    extent.cluster_skip;
			return (FSIMG_OK);
		}
		status = erofs_window_fill(reader, &erofs->content_window,
		    &extent.cluster, error);
		if (status != FSIMG_OK)
			return (status);
		out->image_offset = 0;
		reader->held = erofs->content_window.output +
		    extent.cluster_skip;
		return (FSIMG_OK);
	}
	return (erofs_reject(error, FSIMG_EINVAL, "extent kind"));
}

/*
 * length bytes of inode's content from logical offset into out, copied
 * through the metadata window: erofs_content_copy() for the driver's own
 * readers.
 */
fsimg_status_t
erofs_content_read(fsimg_reader_t *reader, const erofs_inode_t *inode,
    uint64_t offset, size_t length, void *out, size_t *stored,
    fsimg_error_t *error)
{
	return (erofs_content_copy(reader, &erofs_state(reader)->meta_window,
	    inode, offset, length, out, stored, error));
}

/*
 * length bytes of inode's content from logical offset into out, extent by
 * extent, any cluster decoded into window and copied from it. The range
 * must lie inside the inode's size: callers derive it from the size, so a
 * range past it is a driver defect, FSIMG_EINVAL. When stored is not NULL,
 * the bytes delivery would read from the image are added to it: those of
 * stored extents and of plain clusters stored shifted, never decoded bytes
 * or zeros.
 */
fsimg_status_t
erofs_content_copy(fsimg_reader_t *reader, erofs_window_t *window,
    const erofs_inode_t *inode, uint64_t offset, size_t length, void *out,
    size_t *stored, fsimg_error_t *error)
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
		status = extent_read(reader, window, &extent, offset, piece,
		    cursor, stored, error);
		if (status != FSIMG_OK)
			return (status);
		cursor += piece;
		offset += piece;
		length -= piece;
	}
	return (FSIMG_OK);
}

/*
 * length bytes from logical offset of one extent, which holds them all. A
 * stored extent is read from the volume, where a range past the input's end
 * is a truncated volume; a cluster is left in window and copied from it; a
 * zero extent is filled with zeros without reading.
 */
static fsimg_status_t
extent_read(fsimg_reader_t *reader, erofs_window_t *window,
    const erofs_extent_t *extent, uint64_t offset, size_t length, void *out,
    size_t *stored, fsimg_error_t *error)
{
	uint64_t within = offset - extent->logical_offset;
	size_t transferred;
	fsimg_status_t status;

	switch (extent->kind) {
	case EROFS_EXTENT_STORED:
		status = fsimg_reader_bytes(reader,
		    extent->storage_offset + within, out, length, &transferred,
		    error);
		/* Bytes a failing read confirmed were still read. */
		if (stored != NULL)
			*stored += transferred;
		if (status != FSIMG_OK)
			return (status);
		if (transferred != length)
			return (erofs_reject(error, FSIMG_ECORRUPT,
			    "data read truncated"));
		return (FSIMG_OK);
	case EROFS_EXTENT_CLUSTER:
		status = erofs_window_fill(reader, window, &extent->cluster,
		    error);
		if (status != FSIMG_OK)
			return (status);
		/* The extent lies inside the cluster's decoded bytes. */
		bcopy(window->output + extent->cluster_skip + within, out,
		    length);
		if (stored != NULL && cluster_mapped(&extent->cluster))
			*stored += length;
		return (FSIMG_OK);
	case EROFS_EXTENT_ZERO:
		bzero(out, length);
		return (FSIMG_OK);
	}
	return (erofs_reject(error, FSIMG_EINVAL, "extent kind"));
}
