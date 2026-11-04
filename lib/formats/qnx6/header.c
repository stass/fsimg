/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "qnx6.h"

/* Header candidates, validation, pair agreement, snapshot selection and the
 * geometry the selected header establishes. The first header sits at byte
 * 8192 and the second in the last whole 4096-byte unit of the slice, so the
 * slice length V fixes where the second header is, independent of N. A first
 * header whose N blocks do not fit before that position proves the image was
 * cut, and is then read alone.
 */

typedef struct qnx6_candidate {
	qnx6_header_t	 header;
	uint32_t	 reservation;
	bool		 big_endian;
	unsigned char	 raw[QNX6_HEADER_BYTES];
} qnx6_candidate_t;

static fsimg_status_t candidate_read(fsimg_reader_t *, uint64_t,
    qnx6_candidate_t *, fsimg_error_t *);
static fsimg_status_t system_tree_check(const qnx6_tree_t *, uint64_t,
    uint32_t, unsigned, uint64_t *, fsimg_error_t *);
static uint64_t blocks_end(const qnx6_header_t *);
static uint64_t system_run_bytes(const qnx6_candidate_t *);
static fsimg_status_t pair_select(qnx6_candidate_t *, qnx6_candidate_t *,
    qnx6_candidate_t **, fsimg_error_t *);
static fsimg_status_t truncated_size(const qnx6_candidate_t *, uint64_t,
    uint64_t *, fsimg_error_t *);

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

/* A magic at byte zero selects the bootless or MMI layout, or with one at
 * byte 8192 too an ambiguous one; neither is supported. The probe and
 * open refuse both alike. at_header is whether byte 8192 holds a magic.
 */
fsimg_status_t
qnx6_layout_refused(bool at_header, fsimg_error_t *error)
{
	return (qnx6_unsupported(error, at_header ?
	    "QNX6 magic at byte 0 and 8192 is ambiguous" :
	    "QNX6 bootless or MMI layout is not supported"));
}

/* Refuse a magic at byte zero. An open under an assumed identity reaches this
 * unprobed, and a slice too short to hold a position fails its read.
 */
static fsimg_status_t
layout_check(fsimg_reader_t *reader, fsimg_error_t *error)
{
	unsigned char magic[4];
	size_t transferred;
	fsimg_status_t status;
	bool big;

	status = fsimg_reader_bytes(reader, 0, magic, sizeof(magic), &transferred,
	    error);
	if (status != FSIMG_OK || !qnx6_magic_order(magic, &big))
		return (status);
	status = fsimg_reader_bytes(reader, QNX6_HEADER_OFFSET, magic,
	    sizeof(magic), &transferred, error);
	if (status != FSIMG_OK)
		return (status);
	return (qnx6_layout_refused(qnx6_magic_order(magic, &big), error));
}

/* Read and completely validate one header. Every field the driver later
 * uses is checked here, so selection compares only valid headers. Whether its
 * blocks fit the slice is the caller's question: the answer tells a truncated
 * volume from an intact one, and does not make a header invalid.
 */
static fsimg_status_t
candidate_read(fsimg_reader_t *reader, uint64_t offset, qnx6_candidate_t *c,
    fsimg_error_t *error)
{
	qnx6_header_t *h = &c->header;
	uint64_t inode_bytes, bitmap_blocks, inode_blocks, reservation;
	uint64_t leaves;
	uint32_t features;
	unsigned depth;
	size_t transferred;
	fsimg_status_t status;

	status = fsimg_reader_bytes(reader, offset, c->raw, sizeof(c->raw),
	    &transferred, error);
	if (status != FSIMG_OK)
		return (status);
	/* The magic is the identity and also gives the byte order. A caller
	 * assuming the identity waives a missing magic, and the CRC, which
	 * never covers the magic, then decides the order; either way the CRC
	 * must match.
	 */
	if (qnx6_magic_order(c->raw, &c->big_endian)) {
		if (qnx6_crc32(c->raw + 8, sizeof(c->raw) - 8) !=
		    qnx6_u32(c->raw + 4, c->big_endian))
			return (qnx6_corrupt(error,
			    "QNX6 header checksum mismatch"));
	} else {
		status = fsimg_reader_identity(reader, false,
		    "QNX6 header magic is missing",
		    offset == QNX6_HEADER_OFFSET ?
		    "the first superblock lacks the QNX6 magic" :
		    "the second superblock lacks the QNX6 magic", error);
		if (status != FSIMG_OK)
			return (status);
		if (!qnx6_crc_order(c->raw, &c->big_endian))
			return (qnx6_corrupt(error,
			    "QNX6 header checksum mismatch"));
	}
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
	/* A free count below N also requires N >= 1. */
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

/* The byte past the N blocks. N has 32 bits and a block is at most 4096 bytes,
 * so this cannot overflow.
 */
static uint64_t
blocks_end(const qnx6_header_t *h)
{
	return (QNX6_DATA_ORIGIN + (uint64_t)h->block_count * h->block_size);
}

/* The bytes of the system run: R blocks and their twins. 2R <= N, so this
 * cannot overflow either.
 */
static uint64_t
system_run_bytes(const qnx6_candidate_t *c)
{
	return ((uint64_t)c->reservation * 2 * c->header.block_size);
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
	/* Equal serials compare from the CRC on: the magic is identity, and
	 * with the byte orders equal two present magics are equal anyway.
	 */
	if (a->serial == b->serial) {
		if (bcmp(first->raw + 4, second->raw + 4,
		    sizeof(first->raw) - 4) != 0)
			return (qnx6_corrupt(error,
			    "QNX6 headers have equal serials but different snapshots"));
		*selected = first;
	} else {
		*selected = a->serial > b->serial ? first : second;
	}
	return (FSIMG_OK);
}

/* The declared size of a volume whose first header proved it truncated, by its
 * own N rather than by any failure of the other header. N sizes the block and
 * inode bitsets at open, so the system run holding both system trees must be in
 * the slice, which bounds N / 8 and the inode capacity by what the file holds.
 * An intact volume has this from 2R <= N.
 *
 * *volume_bytes receives the smallest volume holding the blocks and a second
 * header. The caller's test was D + N * B > S2, so its round-up is at least
 * S2 + 4096, the slice's last whole unit, and the result exceeds the slice: the
 * reader core reports bytes missing and refuses verification by construction.
 */
static fsimg_status_t
truncated_size(const qnx6_candidate_t *first, uint64_t slice_bytes,
    uint64_t *volume_bytes, fsimg_error_t *error)
{
	uint64_t end = blocks_end(&first->header);

	if (QNX6_DATA_ORIGIN + system_run_bytes(first) > slice_bytes)
		return (qnx6_corrupt(error,
		    "image is truncated inside the QNX6 system trees"));
	*volume_bytes = (end + QNX6_HEADER_AREA - 1) / QNX6_HEADER_AREA *
	    QNX6_HEADER_AREA + QNX6_HEADER_AREA;
	return (FSIMG_OK);
}

/* Read the headers, select the snapshot and establish the geometry, the
 * system domain and the content domain. Nothing is read from the blocks yet.
 */
fsimg_status_t
qnx6_headers_read(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_qnx6_reader_t *qnx6 = qnx6_state(reader);
	qnx6_geometry_t *g = &qnx6->geometry;
	qnx6_candidate_t *candidates, *selected = NULL;
	uint64_t slice_bytes, volume_bytes, second_header, system_bytes;
	fsimg_error_t detail;
	fsimg_status_t status;

	status = fsimg_io_get_size(reader->input, &slice_bytes, error);
	if (status != FSIMG_OK)
		return (status);
	if (slice_bytes > INT64_MAX)
		return (qnx6_corrupt(error, "QNX6 slice is too large"));
	status = layout_check(reader, error);
	if (status != FSIMG_OK)
		return (status);

	candidates = fsimg_alloc(reader->ctx, 2 * sizeof(*candidates), &detail);
	if (candidates == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	status = candidate_read(reader, QNX6_HEADER_OFFSET, &candidates[0], error);
	if (status != FSIMG_OK)
		goto done;
	/* The first header's read holds the slice to at least 8704 bytes, so
	 * this cannot wrap. With D + N * B <= S2 and N >= 1, S2 lies past D;
	 * the second header must then agree on N, so it fits as well.
	 */
	second_header = slice_bytes / QNX6_HEADER_AREA * QNX6_HEADER_AREA -
	    QNX6_HEADER_AREA;
	volume_bytes = slice_bytes;
	if (blocks_end(&candidates[0].header) > second_header) {
		g->second_header_missing = true;
		selected = &candidates[0];
		status = truncated_size(selected, slice_bytes, &volume_bytes,
		    error);
	} else {
		status = candidate_read(reader, second_header, &candidates[1],
		    error);
		if (status == FSIMG_OK)
			status = pair_select(&candidates[0], &candidates[1],
			    &selected, error);
	}
	if (status != FSIMG_OK)
		goto done;

	qnx6->header = selected->header;
	g->volume_bytes = volume_bytes;
	g->block_size = selected->header.block_size;
	g->pointers = g->block_size / 4;
	g->blocks = selected->header.block_count;
	g->inode_count = selected->header.inode_count;
	g->reservation = selected->reservation;
	g->groups = selected->header.groups;
	g->big_endian = selected->big_endian;
	system_bytes = system_run_bytes(selected);
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
