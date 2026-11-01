/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>
#include <time.h>

#include "fsimg-endian.h"
#include "unicode.h"
#include "write-ifs.h"

/* Planning runs a fixed sequence of passes over the frozen catalog
 * (specs/design-ifs-driver.md, Planning): the entries pass checks each
 * entry and fills its row, the records pass sizes every record and places
 * it in the table, the layout pass places every regular file's data after
 * the table, and the summary fills the plan information. Rows are the only
 * per-entry state, and every offset is summed in 64 bits and checked
 * against the largest image before it is stored in 32.
 */

static fsimg_status_t validate_options(fsimg_write_options_t *, unsigned,
    fsimg_error_t *);
static fsimg_status_t make_plan(fsimg_writer_t *, fsimg_error_t *);
static void free_plan(fsimg_plan_t *);
static fsimg_status_t encode_time(ifs_plan_t *, const fsimg_entry_t *,
    uint32_t *, fsimg_error_t *);
static fsimg_status_t plan_entry(ifs_plan_t *, const fsimg_catalog_entry_t *,
    fsimg_error_t *);
static fsimg_status_t plan_entries(ifs_plan_t *, fsimg_error_t *);
static fsimg_status_t plan_records(ifs_plan_t *, fsimg_error_t *);
static fsimg_status_t plan_layout(ifs_plan_t *, fsimg_error_t *);

const fsimg_writer_ops_t fsimg_ifs_writer_ops = {
	.validate = validate_options,
	.plan = make_plan,
	.emit = ifs_write_emit,
	.free = free_plan
};

static fsimg_status_t
too_large(fsimg_error_t *error)
{
	return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER,
	    "ImageFS image exceeds 2^32 - 4 bytes"));
}

static fsimg_status_t
corrupt_row(fsimg_error_t *error)
{
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_WRITER,
	    "ImageFS plan row read back from metadata storage is invalid"));
}

static fsimg_status_t
invalid(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER,
	    message));
}

/* The common options with the profile's rules (Writer settings): byte
 * addressing, so a sector and allocation unit of 1 byte; no FAT root; no
 * volume size or free space, since the image has neither; a 32-bit
 * reproducible epoch; either byte order; a mountpoint of at most 1024
 * bytes under the name profile's byte rules, slashes allowed. The common
 * capability check has refused a label and the settings ImageFS lacks, and
 * the volume serial is a common identity the profile does not store.
 */
static fsimg_status_t
validate_options(fsimg_write_options_t *options, unsigned variant FSIMG_UNUSED,
    fsimg_error_t *error)
{
	size_t length;

	if ((options->sector_bytes != 0 && options->sector_bytes != 1) ||
	    (options->allocation_unit_bytes != 0 &&
	    options->allocation_unit_bytes != 1) ||
	    options->fat_root_entries != 0)
		return (invalid(error, "ImageFS is byte-addressed, with no "
		    "sectors, clusters or FAT root"));
	if (options->volume_bytes != 0 || options->min_free_bytes != 0)
		return (invalid(error, "an ImageFS image has no free space: its "
		    "size is derived from its contents"));
	if ((options->flags & FSIMG_WRITE_REPRODUCIBLE) != 0 &&
	    (options->epoch < 0 || options->epoch > UINT32_MAX))
		return (invalid(error,
		    "reproducible epoch is outside the ImageFS time range"));
	if (options->byte_order != FSIMG_BYTE_ORDER_LITTLE &&
	    options->byte_order != FSIMG_BYTE_ORDER_BIG)
		return (invalid(error, "unknown ImageFS byte order"));
	if (options->mountpoint != NULL) {
		length = strnlen(options->mountpoint, IFS_PATH_MAX + 1);
		if (length > IFS_PATH_MAX)
			return (invalid(error,
			    "ImageFS mountpoint is longer than 1024 bytes"));
		if (!ifs_mountpoint_valid(
		    (const unsigned char *)options->mountpoint, length))
			return (invalid(error, "ImageFS mountpoint is not UTF-8 "
			    "text without control bytes"));
	}
	options->sector_bytes = 1;
	options->allocation_unit_bytes = 1;
	return (FSIMG_OK);
}

/* Read row id back from metadata storage, which is not trusted: a row
 * comes back only with a known kind, a stored path and link target within
 * the profile's limits, and a record length that is zero (records not yet
 * planned) or its kind's minimum rounded up to 4. Those limits keep every
 * record within the emission buffer (write-ifs.h). Emission still checks
 * the stored path length against the catalog's path, and the emission
 * layer bounds the table and data offsets against the volume.
 */
fsimg_status_t
ifs_row_get(ifs_plan_t *plan, uint64_t id, ifs_row_t *row,
    fsimg_error_t *error)
{
	unsigned char raw[IFS_ROW_BYTES];
	uint64_t minimum;
	uint32_t kind;
	fsimg_status_t status;

	status = fsimg_array_get(plan->rows, id, raw, error);
	if (status != FSIMG_OK)
		return (status);
	kind = fsimg_load_le32(raw);
	if (kind < IFS_KIND_DIRECTORY || kind > IFS_KIND_FIFO)
		return (corrupt_row(error));
	row->kind = (ifs_kind_t)kind;
	row->path_length = fsimg_load_le32(raw + 4);
	row->length = fsimg_load_le32(raw + 8);
	row->table_offset = fsimg_load_le32(raw + 12);
	row->record_length = fsimg_load_le32(raw + 16);
	row->data_offset = fsimg_load_le32(raw + 20);
	row->uid = fsimg_load_le32(raw + 24);
	row->gid = fsimg_load_le32(raw + 28);
	row->permissions = fsimg_load_le32(raw + 32);
	row->mtime = fsimg_load_le32(raw + 36);
	if (row->path_length > IFS_PATH_MAX)
		return (corrupt_row(error));
	if (row->kind == IFS_KIND_SYMLINK && row->length > IFS_LINK_TARGET_MAX)
		return (corrupt_row(error));
	minimum = ifs_record_minimum(row->kind, row->path_length,
	    row->kind == IFS_KIND_SYMLINK ? row->length : 0);
	if (row->record_length != 0 &&
	    row->record_length != ifs_round4(minimum))
		return (corrupt_row(error));
	return (FSIMG_OK);
}

fsimg_status_t
ifs_row_set(ifs_plan_t *plan, uint64_t id, const ifs_row_t *row,
    fsimg_error_t *error)
{
	unsigned char raw[IFS_ROW_BYTES];

	fsimg_store_le32(raw, (uint32_t)row->kind);
	fsimg_store_le32(raw + 4, row->path_length);
	fsimg_store_le32(raw + 8, row->length);
	fsimg_store_le32(raw + 12, row->table_offset);
	fsimg_store_le32(raw + 16, row->record_length);
	fsimg_store_le32(raw + 20, row->data_offset);
	fsimg_store_le32(raw + 24, row->uid);
	fsimg_store_le32(raw + 28, row->gid);
	fsimg_store_le32(raw + 32, row->permissions);
	fsimg_store_le32(raw + 36, row->mtime);
	return (fsimg_array_set(plan->rows, id, raw, error));
}

/* Encode an entry's modification time as unsigned UTC seconds through the
 * shared selector: an absent time, or one outside 0 to 2^32 - 1, takes the
 * fallback, which reproducible mode makes the epoch for every entry. A
 * present time the stored value does not preserve is a loss.
 */
static fsimg_status_t
encode_time(ifs_plan_t *plan, const fsimg_entry_t *entry, uint32_t *out,
    fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	bool exact, present, reproducible;

	present = (entry->metadata.valid & FSIMG_META_MODIFIED) != 0;
	reproducible = (writer->options.flags & FSIMG_WRITE_REPRODUCIBLE) != 0;
	exact = fsimg_time_select_u32(present, &entry->metadata.modified,
	    UINT32_MAX, plan->fallback_time, reproducible, out);
	if (exact)
		return (FSIMG_OK);
	return (fsimg_writer_loss(writer, entry, FSIMG_META_MODIFIED,
	    "ImageFS cannot preserve the requested modification time", error));
}

/* Check one catalog entry against the profile and fill its row: its name
 * under the QNX name profile, its stored path within 1024 bytes, a file
 * within 32 bits, and its owner, mode and time. A catalog entry's parent
 * has a lower ID, so checking every entry's own name checks every
 * component of every path.
 */
static fsimg_status_t
plan_entry(ifs_plan_t *plan, const fsimg_catalog_entry_t *catalog,
    fsimg_error_t *error)
{
	const fsimg_entry_t *entry = &catalog->entry;
	const fsimg_metadata_t *m = &entry->metadata;
	const char *name;
	size_t path_length;
	ifs_row_t row;
	fsimg_status_t status;

	name = strrchr(entry->path, '/') + 1;
	if (!fsimg_name_in_qnx_profile(name, strlen(name))) {
		fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER,
		    "name is outside the ImageFS name profile");
		fsimg_error_path(error, entry->path);
		return (FSIMG_EINVAL);
	}
	path_length = strlen(entry->path) - 1;
	if (path_length > IFS_PATH_MAX) {
		fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0, FSIMG_OP_WRITER,
		    "ImageFS path is longer than 1024 bytes");
		fsimg_error_path(error, entry->path);
		return (FSIMG_EUNSUPPORTED);
	}
	bzero(&row, sizeof(row));
	row.path_length = (uint32_t)path_length;
	switch (entry->kind) {
	case FSIMG_ENTRY_FILE:
		if (entry->size > IFS_FILE_MAX) {
			fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
			    FSIMG_OP_WRITER,
			    "ImageFS file is larger than 2^32 - 1 bytes");
			fsimg_error_path(error, entry->path);
			return (FSIMG_EUNSUPPORTED);
		}
		row.kind = IFS_KIND_FILE;
		row.length = (uint32_t)entry->size;
		row.permissions = 0644;
		plan->common.info.initialized_bytes += entry->size;
		break;
	case FSIMG_ENTRY_DIRECTORY:
		row.kind = IFS_KIND_DIRECTORY;
		row.permissions = 0755;
		break;
	case FSIMG_ENTRY_SYMLINK:
		/* The common capability check holds targets to the format's
		 * limit.
		 */
		if (entry->size == 0 || entry->size > IFS_LINK_TARGET_MAX)
			return (fsimg_error_set(error, FSIMG_ESTATE, 0,
			    FSIMG_OP_WRITER,
			    "ImageFS planning received an unchecked link target"));
		row.kind = IFS_KIND_SYMLINK;
		row.length = (uint32_t)entry->size;
		row.permissions = 0777;
		break;
	case FSIMG_ENTRY_FIFO:
		row.kind = IFS_KIND_FIFO;
		row.permissions = 0777;
		break;
	default:
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "ImageFS planning received an unknown entry kind"));
	}
	if ((m->valid & FSIMG_META_MODE) != 0)
		row.permissions = m->mode & FSIMG_MODE_BITS;
	if ((m->valid & FSIMG_META_OWNER) != 0) {
		row.uid = m->uid;
		row.gid = m->gid;
	}
	status = encode_time(plan, entry, &row.mtime, error);
	if (status != FSIMG_OK)
		return (status);
	return (ifs_row_set(plan, catalog->id, &row, error));
}

/* Pass 1: the root's row, which carries no catalog metadata, then one row
 * per entry in catalog ID order.
 */
static fsimg_status_t
plan_entries(ifs_plan_t *plan, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	const fsimg_catalog_entry_t *catalog;
	ifs_row_t root;
	fsimg_status_t status;

	root = (ifs_row_t){
		.kind = IFS_KIND_DIRECTORY,
		.permissions = IFS_IMPLICIT_MODE,
		.mtime = plan->fallback_time
	};
	status = ifs_row_set(plan, 0, &root, error);
	for (uint64_t id = 1; status == FSIMG_OK && id <= writer->count; id++) {
		status = fsimg_session_charge(&writer->session, 0, 0, 1, error);
		if (status == FSIMG_OK)
			status = fsimg_catalog_get(writer, id, &catalog, error);
		if (status == FSIMG_OK)
			status = plan_entry(plan, catalog, error);
	}
	return (status);
}

/* Pass 2: dir_offset is the mountpoint's NUL rounded up to 4; each record
 * is its kind's minimum rounded up to 4, and the records follow each other
 * from dir_offset in ID order, the root first. hdr_dir_size adds the
 * terminator. The table must leave room for the trailer below the largest
 * image.
 */
static fsimg_status_t
plan_records(ifs_plan_t *plan, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	ifs_row_t row;
	uint64_t next, length;
	fsimg_status_t status;

	plan->dir_offset = (uint32_t)ifs_round4((uint64_t)IFS_HEADER_BYTES +
	    plan->mountpoint_length + 1);
	next = plan->dir_offset;
	for (uint64_t id = 0; id <= writer->count; id++) {
		status = fsimg_session_charge(&writer->session, 0, 0, 1, error);
		if (status == FSIMG_OK)
			status = ifs_row_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		length = ifs_round4(ifs_record_minimum(row.kind, row.path_length,
		    row.kind == IFS_KIND_SYMLINK ? row.length : 0));
		if (next + length + 4 > IFS_TRAILER_MAX)
			return (too_large(error));
		row.table_offset = (uint32_t)next;
		row.record_length = (uint32_t)length;
		next += length;
		status = ifs_row_set(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
	}
	plan->hdr_dir_size = (uint32_t)(next + 4);
	return (FSIMG_OK);
}

/* Pass 3: each regular file in ID order takes the next multiple of 4 from
 * hdr_dir_size, an empty one included; the data end, rounded up to 4, is
 * where the trailer starts.
 */
static fsimg_status_t
plan_layout(ifs_plan_t *plan, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	ifs_row_t row;
	uint64_t next;
	fsimg_status_t status;

	next = plan->hdr_dir_size;
	for (uint64_t id = 1; id <= writer->count; id++) {
		status = fsimg_session_charge(&writer->session, 0, 0, 1, error);
		if (status == FSIMG_OK)
			status = ifs_row_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		if (row.kind != IFS_KIND_FILE)
			continue;
		if (ifs_round4(next + row.length) > IFS_TRAILER_MAX)
			return (too_large(error));
		row.data_offset = (uint32_t)next;
		next = ifs_round4(next + row.length);
		status = ifs_row_set(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
	}
	plan->image_size = (uint32_t)(next + 4);
	return (FSIMG_OK);
}

static fsimg_status_t
make_plan(fsimg_writer_t *writer, fsimg_error_t *error)
{
	const char *mountpoint;
	fsimg_plan_info_t *info;
	ifs_plan_t *plan;
	fsimg_error_t detail;
	time_t now;
	fsimg_status_t status;

	plan = fsimg_alloc(writer->ctx, sizeof(*plan), &detail);
	if (plan == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	info = &plan->common.info;
	plan->common.writer = writer;
	strcpy(info->format, "ifs");
	info->entries = writer->count;
	info->payload_bytes = writer->payload_bytes;
	info->required_io_flags = FSIMG_IO_WRITE;
	if ((writer->options.flags & FSIMG_WRITE_DURABLE) != 0)
		info->required_io_flags |= FSIMG_IO_DURABLE;
	plan->order = writer->options.byte_order == FSIMG_BYTE_ORDER_BIG ?
	    IFS_BIG_ENDIAN : IFS_LITTLE_ENDIAN;
	/* Validation bounded the mountpoint, and the writer copied it. */
	mountpoint = writer->options.mountpoint == NULL ? "/" :
	    writer->options.mountpoint;
	plan->mountpoint_length = (uint32_t)strlen(mountpoint);
	bcopy(mountpoint, plan->mountpoint, plan->mountpoint_length + 1);

	/* The root's time and that of every entry without one: the epoch when
	 * reproducible, otherwise the host's UTC clock at planning.
	 */
	if ((writer->options.flags & FSIMG_WRITE_REPRODUCIBLE) != 0) {
		plan->fallback_time = (uint32_t)writer->options.epoch;
	} else {
		now = time(NULL);
		if (now < 0 || (uint64_t)now > UINT32_MAX) {
			status = fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_WRITER,
			    "host clock is outside the ImageFS time range");
			goto fail;
		}
		plan->fallback_time = (uint32_t)now;
	}
	/* Every record holds at least a 24-byte prefix and a NUL padded to 4,
	 * and the header with the shortest mountpoint's NUL padded to 4, the
	 * terminator and the trailer take 0x58 + 4 + 4 + 4 bytes, so a catalog
	 * whose bare records would not fit fails before its rows are made.
	 * The bound only refuses catalogs that cannot fit.
	 */
	if ((writer->count + 1) > (IFS_TRAILER_MAX - IFS_HEADER_BYTES - 8) /
	    (IFS_REC_PREFIX + 4)) {
		status = too_large(error);
		goto fail;
	}
	status = fsimg_array_new(&writer->session, IFS_ROW_BYTES,
	    writer->count + 1, &plan->rows, error);
	if (status == FSIMG_OK)
		status = plan_entries(plan, error);
	if (status == FSIMG_OK)
		status = plan_records(plan, error);
	if (status == FSIMG_OK)
		status = plan_layout(plan, error);
	if (status != FSIMG_OK)
		goto fail;

	/* Pass 4: the summary. The image has no free space: everything but
	 * the file data and its alignment is metadata, from the header
	 * through the terminator, and the trailer.
	 */
	info->volume_bytes = plan->image_size;
	info->sector_bytes = 1;
	info->allocation_unit_bytes = 1;
	info->allocation_units = plan->image_size;
	info->allocated_bytes = plan->image_size;
	info->free_bytes = 0;
	info->metadata_bytes = (uint64_t)plan->hdr_dir_size + 4;
	info->scratch_bytes = writer->ctx->stats.scratch_bytes;
	writer->plan = &plan->common;
	return (FSIMG_OK);
fail:
	free_plan(&plan->common);
	return (status);
}

static void
free_plan(fsimg_plan_t *base)
{
	ifs_plan_t *plan = (ifs_plan_t *)base;

	if (plan == NULL)
		return;
	fsimg_array_free(plan->rows);
	fsimg_dealloc(plan);
}
