/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "write-ifs.h"

/* Emission (specs/design-ifs-driver.md, Emission). The common emission
 * object has already zeroed the sink, so the mountpoint's padding, the
 * terminator, the gaps after file data and the padding before the trailer
 * are zero already. The driver writes, in order: the header and the
 * mountpoint; for each row in ID order its record and then its content,
 * a regular file's data or a symbolic link's target inside the record; and
 * the trailer. Every byte comes from a row, the plan or a profile constant.
 *
 * The trailer makes the words of the image sum to zero, so emission keeps
 * the sum of every byte it writes as it goes, metadata through
 * emit_metadata() and content through the copy observer, and never reads
 * the sink or a source again. Bytes it never writes are the sink's zeros
 * and add nothing.
 */

/* The running sum of the image's 32-bit words in its byte order. */
typedef struct ifs_sum {
	uint32_t	 sum;
	ifs_byte_order_t order;
} ifs_sum_t;

static void sum_bytes(ifs_sum_t *, const unsigned char *, size_t, uint64_t);
static void observe_content(void *, const unsigned char *, size_t, uint64_t);
static fsimg_status_t emit_metadata(fsimg_emit_t *, ifs_sum_t *, uint64_t,
    const unsigned char *, size_t, fsimg_error_t *);
static fsimg_status_t emit_header(ifs_plan_t *, fsimg_emit_t *, ifs_sum_t *,
    fsimg_error_t *);
static fsimg_status_t emit_record(ifs_plan_t *, fsimg_emit_t *, ifs_sum_t *,
    uint64_t, const ifs_row_t *, const char *, ifs_record_t *,
    fsimg_error_t *);
static fsimg_status_t emit_content(fsimg_emit_t *, ifs_sum_t *, uint64_t,
    const ifs_row_t *, const ifs_record_t *, fsimg_error_t *);

/* Add bytes written at volume offset offset to the sum. A byte at offset o
 * lies in lane o mod 4 of its word, so the sum depends only on the bytes
 * and their offsets, never on how writes split them: whole words are
 * decoded at once and the bytes outside them are shifted into their lanes.
 */
static void
sum_bytes(ifs_sum_t *sum, const unsigned char *bytes, size_t length,
    uint64_t offset)
{
	unsigned lane;

	for (; length != 0 && offset % 4 != 0; bytes++, length--, offset++) {
		lane = (unsigned)(offset % 4);
		sum->sum += (uint32_t)*bytes << 8 *
		    (sum->order == IFS_BIG_ENDIAN ? 3 - lane : lane);
	}
	for (; length >= 4; bytes += 4, length -= 4)
		sum->sum += ifs_u32(bytes, sum->order);
	for (lane = 0; lane < length; lane++)
		sum->sum += (uint32_t)bytes[lane] << 8 *
		    (sum->order == IFS_BIG_ENDIAN ? 3 - lane : lane);
}

/* The copy observer: content bytes join the sum as the copy writes them. */
static void
observe_content(void *argument, const unsigned char *bytes, size_t length,
    uint64_t offset)
{
	sum_bytes(argument, bytes, length, offset);
}

/* Write encoded metadata and add it to the sum. */
static fsimg_status_t
emit_metadata(fsimg_emit_t *emit, ifs_sum_t *sum, uint64_t offset,
    const unsigned char *bytes, size_t length, fsimg_error_t *error)
{
	fsimg_status_t status;

	status = fsimg_emit_write(emit, offset, bytes, length, error);
	if (status == FSIMG_OK)
		sum_bytes(sum, bytes, length, offset);
	return (status);
}

/* Step 1: the header from the plan, with no references, chain_paddr or
 * mount flags, then the mountpoint and its NUL.
 */
static fsimg_status_t
emit_header(ifs_plan_t *plan, fsimg_emit_t *emit, ifs_sum_t *sum,
    fsimg_error_t *error)
{
	unsigned char *buffer = fsimg_emit_buffer(emit);
	ifs_header_t header = {
		.order = plan->order,
		.image_size = plan->image_size,
		.hdr_dir_size = plan->hdr_dir_size,
		.dir_offset = plan->dir_offset
	};

	ifs_header_encode(&header, buffer);
	bcopy(plan->mountpoint, buffer + IFS_HEADER_BYTES,
	    plan->mountpoint_length + 1);
	return (emit_metadata(emit, sum, 0, buffer,
	    IFS_HEADER_BYTES + plan->mountpoint_length + 1, error));
}

/* Step 2, for one row: its record, encoded with zero padding, at its table
 * offset, and returned in record. path is the stored path, the catalog's
 * image path without its leading slash, or the root's empty path. The
 * inode word is the catalog ID plus 1, below 2^28 because the table holds
 * a record of at least 28 bytes per ID within 2^32 bytes.
 */
static fsimg_status_t
emit_record(ifs_plan_t *plan, fsimg_emit_t *emit, ifs_sum_t *sum, uint64_t id,
    const ifs_row_t *row, const char *path, ifs_record_t *record,
    fsimg_error_t *error)
{
	unsigned char *buffer = fsimg_emit_buffer(emit);

	/* ifs_row_get() has bounded the row's lengths; the planned record
	 * length is nonzero only once records are planned.
	 */
	if (strlen(path) != row->path_length || row->record_length == 0)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_WRITER,
		    "ImageFS plan row disagrees with its catalog entry"));
	if (id + 1 >= UINT32_C(1) << IFS_INODE_FLAG_SHIFT)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "ImageFS catalog ID exceeds the inode range"));
	*record = (ifs_record_t){
		.kind = row->kind,
		.length = row->record_length,
		.inode = (uint32_t)(id + 1),
		.permissions = row->permissions,
		.uid = row->uid,
		.gid = row->gid,
		.mtime = row->mtime,
		.data_offset = row->data_offset,
		.data_length = row->kind == IFS_KIND_FILE ? row->length : 0,
		.path_length = row->path_length,
		.target_length = row->kind == IFS_KIND_SYMLINK ? row->length : 0
	};
	ifs_record_place(record);
	ifs_record_encode(record, (const unsigned char *)path, plan->order,
	    buffer);
	return (emit_metadata(emit, sum, row->table_offset, buffer,
	    row->record_length, error));
}

/* Step 2, for a regular file or a symbolic link: its content through the
 * copy, which opens, validates and closes its source, an empty file's
 * included. A file's data is one region at its data offset; a link's
 * target is one region at the record's target offset, over the zeros
 * emit_record() wrote there, and the NUL after the target is the record's
 * zero padding.
 */
static fsimg_status_t
emit_content(fsimg_emit_t *emit, ifs_sum_t *sum, uint64_t id,
    const ifs_row_t *row, const ifs_record_t *record, fsimg_error_t *error)
{
	fsimg_emit_region_t region = {
		.image_offset = row->kind == IFS_KIND_FILE ? row->data_offset :
		    (uint64_t)row->table_offset + record->target_offset,
		.capacity = row->length
	};
	/* An empty file's payload has no region. */
	fsimg_emit_payload_t payload = {
		.regions = row->length != 0 ? &region : NULL,
		.region_count = row->length != 0 ? 1 : 0,
		.length = row->length,
		.initialized = row->length,
		.observe = observe_content,
		.observer_argument = sum
	};

	return (fsimg_emit_copy(emit, id, &payload, error));
}

fsimg_status_t
ifs_write_emit(fsimg_writer_t *writer, fsimg_emit_t *emit, fsimg_error_t *error)
{
	ifs_plan_t *plan = (ifs_plan_t *)writer->plan;
	const fsimg_catalog_entry_t *catalog;
	unsigned char trailer[4];
	ifs_sum_t sum = { .sum = 0, .order = plan->order };
	ifs_record_t record;
	ifs_row_t row;
	fsimg_status_t status;

	status = emit_header(plan, emit, &sum, error);
	if (status == FSIMG_OK)
		status = ifs_row_get(plan, 0, &row, error);
	if (status == FSIMG_OK)
		status = emit_record(plan, emit, &sum, 0, &row, "", &record,
		    error);
	for (uint64_t id = 1; status == FSIMG_OK && id <= writer->count; id++) {
		status = fsimg_catalog_get(writer, id, &catalog, error);
		if (status == FSIMG_OK)
			status = ifs_row_get(plan, id, &row, error);
		if (status == FSIMG_OK)
			status = emit_record(plan, emit, &sum, id, &row,
			    catalog->entry.path + 1, &record, error);
		if (status == FSIMG_OK && (row.kind == IFS_KIND_FILE ||
		    row.kind == IFS_KIND_SYMLINK))
			status = emit_content(emit, &sum, id, &row, &record,
			    error);
	}
	if (status != FSIMG_OK)
		return (status);

	/* Step 3: the trailer, the additive inverse of every word before it. */
	ifs_put32(trailer, (uint32_t)0 - sum.sum, plan->order);
	return (fsimg_emit_write(emit, (uint64_t)plan->image_size - 4, trailer,
	    sizeof(trailer), error));
}
