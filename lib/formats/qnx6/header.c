/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "qnx6.h"

/* Header candidates, validation, pair agreement, snapshot selection and the
 * geometry the selected header establishes. The first header sits at byte
 * 8192 and the second in the last whole 4096-byte unit of the slice, so the
 * slice length V fixes where the second header is, independent of N.
 */

typedef struct qnx6_candidate {
	qnx6_header_t	 header;
	uint32_t	 reservation;
	bool		 big_endian;
	unsigned char	 raw[QNX6_HEADER_BYTES];
} qnx6_candidate_t;

static fsimg_status_t candidate_read(fsimg_reader_t *, uint64_t, uint64_t,
    qnx6_candidate_t *, fsimg_error_t *);
static fsimg_status_t system_tree_check(const qnx6_tree_t *, uint64_t,
    uint32_t, unsigned, uint64_t *, fsimg_error_t *);
static fsimg_status_t pair_select(qnx6_candidate_t *, qnx6_candidate_t *,
    qnx6_candidate_t **, fsimg_error_t *);

/* A system tree must have exactly its native length, the minimal depth for
 * that length and at most depth 2; *blocks receives its block count.
 */
static fsimg_status_t
system_tree_check(const qnx6_tree_t *tree, uint64_t length, uint32_t block_size,
    unsigned max_depth, uint64_t *blocks, fsimg_error_t *error)
{
	uint64_t leaves;
	unsigned depth;

	leaves = qnx6_tree_leaves(tree->length, block_size);
	if (!qnx6_tree_min_depth(leaves, block_size / 4, &depth))
		return (qnx6_corrupt(error, "QNX6 system tree is too large"));
	if (depth != tree->depth || depth > max_depth)
		return (qnx6_unsupported(error,
		    "QNX6 system tree is not in its native minimal shape"));
	/* A longer tree could be valid padding no native volume shows; a
	 * shorter one cannot describe every block or inode.
	 */
	if (tree->length != length)
		return (tree->length > length ? qnx6_unsupported(error,
		    "QNX6 system tree is longer than the native layout") :
		    qnx6_corrupt(error, "QNX6 system tree is too short"));
	if (!qnx6_tree_blocks(leaves, block_size / 4, depth, blocks))
		return (qnx6_corrupt(error, "QNX6 system tree is too large"));
	return (FSIMG_OK);
}

/* Read and completely validate one header. Every field the driver later
 * uses is checked here, so selection compares only valid headers.
 */
static fsimg_status_t
candidate_read(fsimg_reader_t *reader, uint64_t offset, uint64_t second_header,
    qnx6_candidate_t *c, fsimg_error_t *error)
{
	qnx6_header_t *h = &c->header;
	uint64_t end, inode_bytes, bitmap_blocks, inode_blocks, reservation;
	uint64_t leaves;
	uint32_t features;
	unsigned depth;
	size_t transferred;
	fsimg_status_t status;

	status = fsimg_reader_bytes(reader, offset, c->raw, sizeof(c->raw),
	    &transferred, error);
	if (status != FSIMG_OK)
		return (status);
	if (!qnx6_magic_order(c->raw, &c->big_endian))
		return (qnx6_corrupt(error, "QNX6 header magic is missing"));
	if (qnx6_crc32(c->raw + 8, sizeof(c->raw) - 8) !=
	    qnx6_u32(c->raw + 4, c->big_endian))
		return (qnx6_corrupt(error, "QNX6 header checksum mismatch"));
	qnx6_header_decode(c->raw, c->big_endian, h);

	/* Recognized headers outside the classic profile are unsupported. */
	features = h->flags & ~QNX6_FLAG_MOUNTED;
	if (h->version != QNX6_VERSION)
		return (qnx6_unsupported(error, "QNX6 on-disk format is not version 4"));
	if (features != 0 && features != QNX6_FLAG_CHECKSUM)
		return (qnx6_unsupported(error, "QNX6 header has unsupported feature flags"));
	if (h->block_size != 512 && h->block_size != 1024 &&
	    h->block_size != 2048 && h->block_size != 4096)
		return (qnx6_unsupported(error, "QNX6 block size is outside the classic profile"));
	if (!h->descriptors_classic || !h->spare_tree_empty || !h->extension_zero)
		return (qnx6_unsupported(error, "QNX6 header has extension trees or fields"));

	/* Counts and geometry. */
	if (h->reserve > 100 || h->groups == 0 || h->groups > 64)
		return (qnx6_corrupt(error, "QNX6 reserve or group count is out of range"));
	if (h->block_count == 0 ||
	    !fsimg_mul_u64(h->block_count, h->block_size, &end) ||
	    !fsimg_add_u64(end, QNX6_DATA_ORIGIN, &end) || end > second_header)
		return (qnx6_corrupt(error, "QNX6 blocks do not fit before the second header"));
	if (h->inode_count < 2 || h->free_inodes >= h->inode_count ||
	    h->free_blocks >= h->block_count)
		return (qnx6_corrupt(error, "QNX6 inode or free counts are out of range"));
	if (h->names.length % h->block_size != 0)
		return (qnx6_corrupt(error, "QNX6 long-name tree is not whole blocks"));
	leaves = qnx6_tree_leaves(h->names.length, h->block_size);
	if (!qnx6_tree_min_depth(leaves, h->block_size / 4, &depth) ||
	    depth != h->names.depth)
		return (qnx6_unsupported(error,
		    "QNX6 long-name tree is not in its minimal shape"));

	/* The system reservation R is derived from the two system trees, whose
	 * native lengths follow from N and the inode count.
	 */
	inode_bytes = (uint64_t)h->inode_count * QNX6_INODE_BYTES;
	status = system_tree_check(&h->bitmap,
	    (uint64_t)h->block_count / 8 + (h->block_count % 8 != 0),
	    h->block_size, QNX6_SYSTEM_DEPTH_MAX, &bitmap_blocks, error);
	if (status != FSIMG_OK)
		return (status);
	status = system_tree_check(&h->inodes, inode_bytes, h->block_size,
	    QNX6_SYSTEM_DEPTH_MAX, &inode_blocks, error);
	if (status != FSIMG_OK)
		return (status);
	reservation = bitmap_blocks + inode_blocks;
	if (reservation > h->block_count / 2)
		return (qnx6_corrupt(error, "QNX6 system reservation does not fit"));
	c->reservation = (uint32_t)reservation;
	return (FSIMG_OK);
}

/* Both headers must describe one volume. The larger serial selects the
 * snapshot; equal serials, the state of a fresh volume, require identical
 * headers. The mounted bit is snapshot state and may differ.
 */
static fsimg_status_t
pair_select(qnx6_candidate_t *first, qnx6_candidate_t *second,
    qnx6_candidate_t **selected, fsimg_error_t *error)
{
	const qnx6_header_t *a = &first->header, *b = &second->header;

	if (first->big_endian != second->big_endian ||
	    bcmp(a->uuid, b->uuid, sizeof(a->uuid)) != 0 ||
	    a->format_time != b->format_time || a->block_size != b->block_size ||
	    a->block_count != b->block_count || a->inode_count != b->inode_count ||
	    a->groups != b->groups ||
	    ((a->flags ^ b->flags) & ~QNX6_FLAG_MOUNTED) != 0)
		return (qnx6_corrupt(error, "QNX6 headers describe different volumes"));
	if (a->serial == b->serial) {
		if (bcmp(first->raw, second->raw, sizeof(first->raw)) != 0)
			return (qnx6_corrupt(error,
			    "QNX6 headers have equal serials but different snapshots"));
		*selected = first;
	} else {
		*selected = a->serial > b->serial ? first : second;
	}
	return (FSIMG_OK);
}

/* Read both headers, select the snapshot and establish the geometry, the
 * system domain and the content domain. Nothing is read from the blocks yet.
 */
fsimg_status_t
qnx6_headers_read(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_qnx6_reader_t *qnx6 = qnx6_state(reader);
	qnx6_geometry_t *g = &qnx6->geometry;
	qnx6_candidate_t *candidates, *selected;
	uint64_t volume_bytes, second_header, system_bytes;
	fsimg_error_t detail;
	fsimg_status_t status;

	status = fsimg_io_get_size(reader->input, &volume_bytes, error);
	if (status != FSIMG_OK)
		return (status);
	second_header = volume_bytes / QNX6_HEADER_AREA * QNX6_HEADER_AREA;
	if (volume_bytes > INT64_MAX ||
	    second_header < QNX6_DATA_ORIGIN + QNX6_HEADER_AREA * 2)
		return (qnx6_corrupt(error, "QNX6 slice is too small for two headers"));
	second_header -= QNX6_HEADER_AREA;

	candidates = fsimg_alloc(reader->ctx, 2 * sizeof(*candidates), &detail);
	if (candidates == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	status = candidate_read(reader, QNX6_HEADER_OFFSET, second_header,
	    &candidates[0], error);
	if (status == FSIMG_OK)
		status = candidate_read(reader, second_header, second_header,
		    &candidates[1], error);
	if (status == FSIMG_OK)
		status = pair_select(&candidates[0], &candidates[1], &selected,
		    error);
	if (status != FSIMG_OK)
		goto done;

	qnx6->header = selected->header;
	g->volume_bytes = volume_bytes;
	g->second_header = second_header;
	g->block_size = selected->header.block_size;
	g->pointers = g->block_size / 4;
	g->blocks = selected->header.block_count;
	g->inode_count = selected->header.inode_count;
	g->reservation = selected->reservation;
	g->big_endian = selected->big_endian;
	system_bytes = (uint64_t)g->reservation * 2 * g->block_size;
	qnx6->system_domain = (fsimg_extent_domain_t){
		.offset = QNX6_DATA_ORIGIN,
		.length = system_bytes,
		.unit_bytes = g->block_size
	};
	reader->content_domain = (fsimg_extent_domain_t){
		.offset = QNX6_DATA_ORIGIN + system_bytes,
		.length = (uint64_t)g->blocks * g->block_size - system_bytes,
		.unit_bytes = g->block_size
	};
done:
	fsimg_dealloc(candidates);
	return (status);
}
