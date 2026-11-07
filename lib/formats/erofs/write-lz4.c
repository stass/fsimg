/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <strings.h>

#include "codec.h"
#include "fsimg-endian.h"
#include "meta.h"
#include "write-erofs.h"

/*
 * LZ4 files (specs/design-erofs.md, LZ4 profile; specs/design-erofs-driver.md,
 * LZ4 planning and emission).
 *
 * The cluster former turns a file's content into one-block physical
 * clusters. It keeps a window of the next min(remaining, 64 KiB) input
 * bytes and forms a cluster only when the window is full or the content
 * has ended, so every cluster depends on the file's bytes alone, never on
 * how the measured read batches them. A cluster is the block the LZ4
 * encoder's prefix mode makes from the window, right-aligned behind zero
 * padding, when that block holds at least min(B, window) input bytes;
 * otherwise it is a plain cluster of those bytes at the block's start. So
 * every cluster but the last holds at least B logical bytes, and no two
 * heads fall in one logical cluster.
 *
 * Planning runs the former over a measured read and appends one record per
 * cluster to the plan's cluster records. Emission runs it again over an
 * encoded copy, compares each cluster with its record and stores the
 * block, and writes each compressed inode's full index, or under
 * addressing=48 its encoded extents, from the records, so no payload is
 * staged and the index needs no source.
 */

/* A cluster record, little endian; bytes 13 to 15 are zero. */
#define RECORD_LOGICAL		0
#define RECORD_CONSUMED		8
#define RECORD_PRODUCED		10
#define RECORD_PLAIN		12
#define RECORD_BYTES		16u

_Static_assert(EROFS_LZ4_WINDOW_BYTES - 1 <= UINT16_MAX,
    "a cluster's consumed bytes less one fit a record's 16-bit field");

/* Two windows, so unread input moves to the front at most once a window. */
#define BUFFER_BYTES		(2 * EROFS_LZ4_WINDOW_BYTES)

/* The index buffer fills exactly: header and reserved bytes, then records. */
_Static_assert(FSIMG_EMIT_BUFFER_BYTES % EROFS_FULL_RECORD_BYTES == 0 &&
    EROFS_FULL_INDEX_OFFSET % EROFS_FULL_RECORD_BYTES == 0,
    "index records never straddle a flush of the emission buffer");

/*
 * A cluster former. The window is buffer[start, start + length), the file's
 * logical bytes from logical on; block is the cluster being formed. sink
 * receives every cluster with its block, and clusters counts them. Each
 * cluster charges its consumed bytes to session as work, so a work budget
 * bounds the encoding as it bounds decoding.
 */
struct erofs_lz4_former {
	fsimg_session_t	*session;
	fsimg_lz4_encoder_t *encoder;
	unsigned char	*buffer;
	unsigned char	*block;
	size_t		 start;
	size_t		 length;
	uint64_t	 logical;
	uint64_t	 clusters;
	uint32_t	 block_bytes;
	fsimg_status_t	(*sink)(void *, const erofs_lz4_cluster_t *,
			    const unsigned char *, fsimg_error_t *);
	void		*argument;
};

/*
 * Measurement's sink state: records go to the plan's cluster records from
 * index base on, but only while fewer than limit clusters have formed,
 * since a file with limit clusters stays flat and its records are dropped.
 */
typedef struct measure_run {
	erofs_plan_t	*plan;
	uint64_t	 base;
	uint64_t	 limit;
	uint64_t	 count;
} measure_run_t;

/* The encoded copy's sink state: the row and the next record to match. */
typedef struct copy_run {
	erofs_plan_t	*plan;
	fsimg_emit_t	*emit;
	const erofs_row_t *row;
	uint64_t	 next;
} copy_run_t;

static void former_start(erofs_lz4_former_t *,
    fsimg_status_t (*)(void *, const erofs_lz4_cluster_t *,
    const unsigned char *, fsimg_error_t *), void *);
static fsimg_status_t form_cluster(erofs_lz4_former_t *, fsimg_error_t *);
static fsimg_status_t former_feed(void *, const unsigned char *, size_t,
    fsimg_error_t *);
static fsimg_status_t record_put(erofs_plan_t *, uint64_t,
    const erofs_lz4_cluster_t *, fsimg_error_t *);
static fsimg_status_t record_get(erofs_plan_t *, uint64_t,
    erofs_lz4_cluster_t *, fsimg_error_t *);
static fsimg_status_t record_cluster(void *, const erofs_lz4_cluster_t *,
    const unsigned char *, fsimg_error_t *);
static fsimg_status_t store_cluster(void *, const erofs_lz4_cluster_t *,
    const unsigned char *, fsimg_error_t *);
static fsimg_status_t write_extents(erofs_plan_t *, const erofs_row_t *,
    uint64_t, unsigned char *, erofs_put_t, void *, fsimg_error_t *);
static fsimg_status_t index_record(unsigned char *, size_t *, uint64_t *,
    const erofs_zrecord_t *, uint16_t, erofs_put_t, void *, fsimg_error_t *);
static fsimg_status_t corrupt_plan(fsimg_error_t *);

/*
 * A former for blocks of block_bytes, with its encoder, window buffer and
 * block allocated through ctx, charged to its memory budget.
 */
fsimg_status_t
erofs_lz4_former_new(fsimg_ctx_t *ctx, fsimg_session_t *session,
    uint32_t block_bytes, erofs_lz4_former_t **out, fsimg_error_t *error)
{
	erofs_lz4_former_t *former;
	fsimg_error_t detail;
	fsimg_status_t status;

	*out = NULL;
	former = fsimg_alloc(ctx, sizeof(*former), &detail);
	if (former == NULL)
		goto fail;
	bzero(former, sizeof(*former));
	former->session = session;
	former->block_bytes = block_bytes;
	former->buffer = fsimg_alloc(ctx, BUFFER_BYTES, &detail);
	if (former->buffer == NULL)
		goto fail;
	former->block = fsimg_alloc(ctx, block_bytes, &detail);
	if (former->block == NULL)
		goto fail;
	status = fsimg_lz4_encoder_new(ctx, &former->encoder, &detail);
	if (status != FSIMG_OK)
		goto fail;
	*out = former;
	return (FSIMG_OK);
fail:
	erofs_lz4_former_free(former);
	if (error != NULL)
		*error = detail;
	return (detail.status);
}

void
erofs_lz4_former_free(erofs_lz4_former_t *former)
{
	if (former == NULL)
		return;
	fsimg_lz4_encoder_free(former->encoder);
	fsimg_dealloc(former->block);
	fsimg_dealloc(former->buffer);
	fsimg_dealloc(former);
}

/* Begin a file: an empty window at logical offset 0. */
static void
former_start(erofs_lz4_former_t *former,
    fsimg_status_t (*sink)(void *, const erofs_lz4_cluster_t *,
    const unsigned char *, fsimg_error_t *), void *argument)
{
	former->start = 0;
	former->length = 0;
	former->logical = 0;
	former->clusters = 0;
	former->sink = sink;
	former->argument = argument;
}

/*
 * Form one cluster from the window, which holds min(remaining, 64 KiB)
 * bytes, and hand it to the sink. The encoder writes only the block's
 * first produced bytes; moving them to the end and zeroing the gap before
 * them, or zeroing the block before copying plain bytes in, defines every
 * byte of the block, so no byte of an earlier cluster or file survives in
 * its padding. Under LZ4_0PADDING the decoder finds the stream's start at
 * the first nonzero byte, which an LZ4 block's leading token always is.
 */
static fsimg_status_t
form_cluster(erofs_lz4_former_t *former, fsimg_error_t *error)
{
	const unsigned char *window = former->buffer + former->start;
	const uint32_t block_bytes = former->block_bytes;
	erofs_lz4_cluster_t cluster;
	fsimg_codec_step_t step;
	size_t least, gap;
	fsimg_status_t status;

	status = fsimg_lz4_encode_prefix(former->encoder, window,
	    former->length, former->block, block_bytes, &step, error);
	if (status != FSIMG_OK)
		return (status);
	least = former->length < block_bytes ? former->length : block_bytes;
	cluster.logical = former->logical;
	if (step.consumed >= least) {
		gap = block_bytes - step.produced;
		bcopy(former->block, former->block + gap, step.produced);
		bzero(former->block, gap);
		cluster.consumed = (uint32_t)step.consumed;
		cluster.produced = (uint32_t)step.produced;
		cluster.plain = false;
	} else {
		bzero(former->block, block_bytes);
		bcopy(window, former->block, least);
		cluster.consumed = (uint32_t)least;
		cluster.produced = (uint32_t)least;
		cluster.plain = true;
	}
	status = fsimg_session_charge(former->session, 0, 0, cluster.consumed,
	    error);
	if (status != FSIMG_OK)
		return (status);
	status = former->sink(former->argument, &cluster, former->block, error);
	if (status != FSIMG_OK)
		return (status);
	former->start += cluster.consumed;
	former->length -= cluster.consumed;
	former->logical += cluster.consumed;
	former->clusters++;
	return (FSIMG_OK);
}

/*
 * The content consumer: append input to the window and form a cluster
 * each time the window fills; on the end call, form clusters until the
 * window is empty. Input moves to the buffer's front only when it reaches
 * the buffer's end, after at least a window of input was consumed.
 */
static fsimg_status_t
former_feed(void *argument, const unsigned char *bytes, size_t length,
    fsimg_error_t *error)
{
	erofs_lz4_former_t *former = argument;
	size_t take, room;
	fsimg_status_t status;

	if (length == 0) {
		while (former->length != 0) {
			status = form_cluster(former, error);
			if (status != FSIMG_OK)
				return (status);
		}
		return (FSIMG_OK);
	}
	while (length != 0) {
		if (former->start + former->length == BUFFER_BYTES) {
			bcopy(former->buffer + former->start, former->buffer,
			    former->length);
			former->start = 0;
		}
		take = EROFS_LZ4_WINDOW_BYTES - former->length;
		room = BUFFER_BYTES - former->start - former->length;
		if (take > room)
			take = room;
		if (take > length)
			take = length;
		bcopy(bytes, former->buffer + former->start + former->length,
		    take);
		former->length += take;
		bytes += take;
		length -= take;
		if (former->length == EROFS_LZ4_WINDOW_BYTES) {
			status = form_cluster(former, error);
			if (status != FSIMG_OK)
				return (status);
		}
	}
	return (FSIMG_OK);
}

/* Store record index, growing the record store as needed. */
static fsimg_status_t
record_put(erofs_plan_t *plan, uint64_t index,
    const erofs_lz4_cluster_t *cluster, fsimg_error_t *error)
{
	unsigned char raw[RECORD_BYTES];
	uint64_t end;
	fsimg_status_t status;

	if (!fsimg_mul_u64(index + 1, RECORD_BYTES, &end))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER,
		    "EROFS cluster records exceed 64 bits"));
	if (fsimg_store_size(plan->cluster_records) < end) {
		status = fsimg_store_grow(plan->cluster_records, end, error);
		if (status != FSIMG_OK)
			return (status);
	}
	bzero(raw, sizeof(raw));
	fsimg_store_le64(raw + RECORD_LOGICAL, cluster->logical);
	fsimg_store_le16(raw + RECORD_CONSUMED,
	    (uint16_t)(cluster->consumed - 1));
	fsimg_store_le16(raw + RECORD_PRODUCED,
	    (uint16_t)(cluster->produced - 1));
	raw[RECORD_PLAIN] = cluster->plain ? 1 : 0;
	return (fsimg_store_write(plan->cluster_records, index * RECORD_BYTES,
	    raw, sizeof(raw), error));
}

/*
 * Load record index. Records come back from scratch storage, so each is
 * checked against what the former can produce: at most a window consumed,
 * at most a block produced, a plain cluster's bytes stored as they are,
 * and zero reserved bytes.
 */
static fsimg_status_t
record_get(erofs_plan_t *plan, uint64_t index, erofs_lz4_cluster_t *cluster,
    fsimg_error_t *error)
{
	unsigned char raw[RECORD_BYTES];
	fsimg_status_t status;

	if (index >= plan->cluster_count)
		return (corrupt_plan(error));
	status = fsimg_store_read(plan->cluster_records, index * RECORD_BYTES,
	    raw, sizeof(raw), error);
	if (status != FSIMG_OK)
		return (status);
	cluster->logical = fsimg_load_le64(raw + RECORD_LOGICAL);
	cluster->consumed = (uint32_t)fsimg_load_le16(raw + RECORD_CONSUMED) + 1;
	cluster->produced = (uint32_t)fsimg_load_le16(raw + RECORD_PRODUCED) + 1;
	cluster->plain = raw[RECORD_PLAIN] != 0;
	if (raw[RECORD_PLAIN] > 1 || raw[13] != 0 || raw[14] != 0 ||
	    raw[15] != 0 || cluster->produced > plan->block_bytes ||
	    (cluster->plain && cluster->consumed != cluster->produced))
		return (corrupt_plan(error));
	return (FSIMG_OK);
}

/*
 * Measurement's sink: record the cluster while the file may still
 * compress. Each cluster's logical offset is the previous one's end, and
 * the window bounds what it consumes, so the record fields hold it.
 */
static fsimg_status_t
record_cluster(void *argument, const erofs_lz4_cluster_t *cluster,
    const unsigned char *block FSIMG_UNUSED, fsimg_error_t *error)
{
	measure_run_t *run = argument;

	if (run->count >= run->limit) {
		run->count++;
		return (FSIMG_OK);
	}
	return (record_put(run->plan, run->base + run->count++, cluster,
	    error));
}

/*
 * Measure catalog entry id, a regular file of size bytes, through a
 * measured read and store its cluster count in *clusters. limit is the
 * number of blocks its flat layout stores: while fewer clusters than that
 * have formed, their records go to the plan's cluster records from
 * cluster_count on. The caller adopts the records of a file with fewer
 * clusters than limit by advancing cluster_count; otherwise the next file
 * overwrites them.
 */
fsimg_status_t
erofs_lz4_measure(erofs_plan_t *plan, erofs_lz4_former_t *former, uint64_t id,
    uint64_t size, uint64_t limit, uint64_t *clusters, fsimg_error_t *error)
{
	measure_run_t run = {
		.plan = plan,
		.base = plan->cluster_count,
		.limit = limit
	};
	fsimg_status_t status;

	*clusters = 0;
	former_start(former, record_cluster, &run);
	status = fsimg_writer_measure(plan->common.writer, id, size,
	    former_feed, former, error);
	if (status != FSIMG_OK)
		return (status);
	*clusters = former->clusters;
	return (FSIMG_OK);
}

/* Encode one index record at buffer + *fill, flushing a full buffer. */
static fsimg_status_t
index_record(unsigned char *buffer, size_t *fill, uint64_t *offset,
    const erofs_zrecord_t *record, uint16_t forward, erofs_put_t put,
    void *argument, fsimg_error_t *error)
{
	fsimg_status_t status;

	if (*fill == FSIMG_EMIT_BUFFER_BYTES) {
		status = put(argument, *offset, buffer, *fill, error);
		if (status != FSIMG_OK)
			return (status);
		*offset += *fill;
		*fill = 0;
	}
	if (!erofs_full_record_encode(record, forward, buffer + *fill))
		return (corrupt_plan(error));
	*fill += EROFS_FULL_RECORD_BYTES;
	return (FSIMG_OK);
}

/*
 * Write a compressed row's map header and full index at offset, the end of
 * its fixed part, staging up to FSIMG_EMIT_BUFFER_BYTES in buffer. The
 * header names LZ4, algorithm 0, for both heads, no advice and a logical
 * cluster of one block, so it is zero, as are the 8 reserved bytes after
 * it.
 *
 * Cluster i, holding logical bytes [h, e), gives logical cluster h / B a
 * head-1 record, or a plain one, at clusterofs h mod B naming block
 * start_block + i, and each logical cluster from h / B + 1 to e / B - 1 a
 * non-head record, back to h / B and forward to e / B, where the next head
 * or the end marker lies. When size mod B is nonzero and no head lies in
 * the last logical cluster, that cluster gets the end marker, a plain
 * record at clusterofs size mod B with block 0, which begins the empty
 * extent at size.
 *
 * records counts the records written, which is also the next logical
 * cluster without one. Each cluster must start where the previous ended
 * and its head must fall in logical cluster records, so the records tile
 * [0, size) in order and never pass ceil(size / B), the count the inode's
 * body was sized for; a plan that breaks either is refused. Under
 * addressing=48, write_extents() writes the encoded extents instead.
 */
fsimg_status_t
erofs_lz4_write_index(erofs_plan_t *plan, const erofs_row_t *row,
    uint64_t offset, unsigned char *buffer, erofs_put_t put, void *argument,
    fsimg_error_t *error)
{
	const uint32_t block_bytes = plan->block_bytes;
	const erofs_zheader_t header = { .head1_algorithm = EROFS_ALGORITHM_LZ4,
	    .head2_algorithm = EROFS_ALGORITHM_LZ4 };
	erofs_lz4_cluster_t cluster;
	erofs_zrecord_t record;
	uint64_t expected, records, head, end_cluster;
	size_t fill;
	fsimg_status_t status;

	if (plan->addressing48)
		return (write_extents(plan, row, offset, buffer, put, argument,
		    error));
	if (!erofs_zheader_encode(&header, buffer))
		return (corrupt_plan(error));
	bzero(buffer + EROFS_ZHEADER_BYTES,
	    EROFS_FULL_INDEX_OFFSET - EROFS_ZHEADER_BYTES);
	fill = EROFS_FULL_INDEX_OFFSET;
	expected = 0;
	records = 0;
	for (uint64_t i = 0; i < row->clusters; i++) {
		status = record_get(plan, row->first_cluster + i, &cluster,
		    error);
		if (status != FSIMG_OK)
			return (status);
		head = cluster.logical / block_bytes;
		if (cluster.logical != expected || head != records ||
		    cluster.consumed > row->size - cluster.logical)
			return (corrupt_plan(error));
		expected += cluster.consumed;
		end_cluster = expected / block_bytes;

		record = (erofs_zrecord_t){
			.type = cluster.plain ? EROFS_RECORD_PLAIN :
			    EROFS_RECORD_HEAD1,
			.clusterofs = (uint16_t)(cluster.logical % block_bytes),
			.block = row->start_block + i
		};
		status = index_record(buffer, &fill, &offset, &record, 0, put,
		    argument, error);
		if (status != FSIMG_OK)
			return (status);
		records++;
		while (records < end_cluster) {
			record = (erofs_zrecord_t){
				.type = EROFS_RECORD_NONHEAD,
				.backward = (uint16_t)(records - head)
			};
			status = index_record(buffer, &fill, &offset, &record,
			    (uint16_t)(end_cluster - records), put, argument,
			    error);
			if (status != FSIMG_OK)
				return (status);
			records++;
		}
	}
	if (expected != row->size)
		return (corrupt_plan(error));
	if (records < erofs_row_index_records(row, block_bytes)) {
		record = (erofs_zrecord_t){
			.type = EROFS_RECORD_PLAIN,
			.clusterofs = (uint16_t)(row->size % block_bytes)
		};
		status = index_record(buffer, &fill, &offset, &record, 0, put,
		    argument, error);
		if (status != FSIMG_OK)
			return (status);
		records++;
	}
	if (records != erofs_row_index_records(row, block_bytes))
		return (corrupt_plan(error));
	return (put(argument, offset, buffer, fill, error));
}

/*
 * Under addressing=48, write a compressed row's map header and 32-byte
 * encoded extents at offset, the end of its fixed part, staging up to
 * FSIMG_EMIT_BUFFER_BYTES, a multiple of the record, in buffer
 * (specs/design-erofs.md, 48-bit profile), each through its codec.c
 * encoder. The header holds the extent count, one per cluster, which fits
 * 32 bits, and the advice that selects 32-byte records; the 24 bytes
 * between the header and the first record are zero.
 *
 * Cluster i, holding logical bytes [h, e), gives one record: B stored
 * bytes, format LZ4 (the algorithm plus one) or plain (0), at the byte
 * offset of block start_block + i, from logical offset h. A record runs to the next one's logical offset,
 * or to size, so e need not be stored. Each cluster must start where the
 * previous ended and lie within size, so the logical offsets start at 0,
 * increase strictly and tile [0, size); a plan that breaks that is
 * refused. The count is at most ceil(size / B), the bound readers enforce:
 * erofs_row_get() refuses a row with more clusters than that, which the
 * former never forms, since every cluster but the last holds B bytes or
 * more. Block start_block + i is a placed
 * block, so its byte offset stays below 2^60.
 */
static fsimg_status_t
write_extents(erofs_plan_t *plan, const erofs_row_t *row, uint64_t offset,
    unsigned char *buffer, erofs_put_t put, void *argument,
    fsimg_error_t *error)
{
	const uint32_t block_bytes = plan->block_bytes;
	const erofs_zheader_t header = {
		.extents_form = EROFS_EXTENTS_SEARCHED,
		.extent_bytes = EROFS_EXTENT32_BYTES,
		.advice = EROFS_EXTENT32_ADVICE,
		.extent_count = row->clusters
	};
	erofs_lz4_cluster_t cluster;
	erofs_xlength_t length;
	erofs_xrecord_t record;
	uint64_t expected;
	size_t fill;
	fsimg_status_t status;

	_Static_assert(FSIMG_EMIT_BUFFER_BYTES % EROFS_EXTENT32_BYTES == 0,
	    "the staging buffer holds whole encoded extents");
	if (!erofs_zheader_encode(&header, buffer))
		return (corrupt_plan(error));
	bzero(buffer + EROFS_ZHEADER_BYTES,
	    EROFS_EXTENT32_OFFSET - EROFS_ZHEADER_BYTES);
	fill = EROFS_EXTENT32_OFFSET;
	expected = 0;
	for (uint64_t i = 0; i < row->clusters; i++) {
		status = record_get(plan, row->first_cluster + i, &cluster,
		    error);
		if (status != FSIMG_OK)
			return (status);
		if (cluster.logical != expected ||
		    cluster.consumed > row->size - cluster.logical)
			return (corrupt_plan(error));
		expected += cluster.consumed;

		if (fill == FSIMG_EMIT_BUFFER_BYTES) {
			status = put(argument, offset, buffer, fill, error);
			if (status != FSIMG_OK)
				return (status);
			offset += fill;
			fill = 0;
		}
		length = (erofs_xlength_t){ .stored_bytes = block_bytes,
		    .format = cluster.plain ? 0 : EROFS_ALGORITHM_LZ4 + 1 };
		record = (erofs_xrecord_t){
			.physical = (row->start_block + i) * block_bytes,
			.logical = cluster.logical
		};
		if (!erofs_xlength_encode(&length, &record.length_word) ||
		    !erofs_xrecord_encode(&record, EROFS_EXTENT32_BYTES,
		    buffer + fill))
			return (corrupt_plan(error));
		fill += EROFS_EXTENT32_BYTES;
	}
	if (expected != row->size)
		return (corrupt_plan(error));
	return (put(argument, offset, buffer, fill, error));
}

/*
 * The encoded copy's sink: the cluster must be the one planning recorded,
 * or the source's bytes changed since they were measured; then its whole
 * block is stored.
 */
static fsimg_status_t
store_cluster(void *argument, const erofs_lz4_cluster_t *cluster,
    const unsigned char *block, fsimg_error_t *error)
{
	copy_run_t *run = argument;
	erofs_lz4_cluster_t planned;
	fsimg_status_t status;

	if (run->next >= run->row->clusters)
		return (fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
		    FSIMG_OP_SOURCE, "EROFS file has more clusters than planned"));
	status = record_get(run->plan, run->row->first_cluster + run->next,
	    &planned, error);
	if (status != FSIMG_OK)
		return (status);
	if (planned.logical != cluster->logical ||
	    planned.consumed != cluster->consumed ||
	    planned.produced != cluster->produced ||
	    planned.plain != cluster->plain)
		return (fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
		    FSIMG_OP_SOURCE, "EROFS file clusters differ from the plan"));
	run->next++;
	return (fsimg_emit_put(run->emit, block, run->plan->block_bytes, error));
}

/*
 * Store a compressed row's clusters through an encoded copy into its
 * planned blocks, with observe seeing every stored byte. The copy refuses
 * output beyond or short of clusters blocks, and store_cluster() any
 * cluster planning did not record. The caller writes the row's index
 * first: erofs_lz4_write_index() checks that the records tile the file,
 * and record_get() that each is one the former can produce, so a
 * difference found here is reported as the source's. A record damaged in
 * scratch only in a stored length or plain flag the former could also
 * produce is reported the same way; either way nothing outside the file's
 * planned blocks is written.
 */
fsimg_status_t
erofs_lz4_copy(erofs_plan_t *plan, erofs_lz4_former_t *former,
    fsimg_emit_t *emit, uint64_t id, const erofs_row_t *row,
    fsimg_emit_observer_t observe, void *observer_argument,
    fsimg_error_t *error)
{
	const uint32_t block_bytes = plan->block_bytes;
	const fsimg_emit_region_t region = {
		.image_offset = row->start_block * block_bytes,
		.capacity = (uint64_t)row->clusters * block_bytes
	};
	copy_run_t run = { .plan = plan, .emit = emit, .row = row };
	const fsimg_emit_encoding_t encoding = {
		.regions = &region,
		.region_count = 1,
		.length = row->size,
		.stored = region.capacity,
		.encode = former_feed,
		.encoder_argument = former,
		.observe = observe,
		.observer_argument = observer_argument
	};

	former_start(former, store_cluster, &run);
	return (fsimg_emit_encoded_copy(emit, id, &encoding, error));
}

static fsimg_status_t
corrupt_plan(fsimg_error_t *error)
{
	return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
	    "invalid EROFS cluster plan"));
}
