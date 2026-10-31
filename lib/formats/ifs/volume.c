/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "ifs.h"

/*
 * Opening an image reads it in a fixed order, each step failing before the
 * next one reads:
 *
 * 1. the fixed header, under the probe's rules and the alignment and
 *    spare-word rules the probe leaves to open;
 * 2. the mountpoint, in one bounded read;
 * 3. the record table, in one sequential pass that validates every record
 *    and appends its path to the path stream, from which tree.c derives
 *    the hierarchy.
 *
 * This file is the only reader of the record table. Replay reads single
 * records again by offset through ifs_record_read().
 */

/* A sequential view of the table: bytes [offset, offset + length) of the
 * image are in bytes.
 */
typedef struct table_window {
	uint64_t	 offset;
	size_t		 length;
	unsigned char	 bytes[IFS_WINDOW_BYTES];
} table_window_t;

static fsimg_status_t read_header(fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t read_mountpoint(fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t window_cover(fsimg_reader_t *, table_window_t *,
    uint64_t, size_t, fsimg_error_t *);
static fsimg_status_t check_range(const ifs_header_t *, const ifs_record_t *,
    fsimg_error_t *);
static fsimg_status_t table_record(fsimg_reader_t *, table_window_t *,
    uint32_t, fsimg_stream_t *, uint32_t *, fsimg_error_t *);
static fsimg_status_t table_walk(fsimg_reader_t *, fsimg_stream_t *,
    fsimg_error_t *);

/* Read exactly length bytes at volume offset into buffer; a short read is
 * a truncated image.
 */
fsimg_status_t
ifs_read_exact(fsimg_reader_t *reader, uint64_t offset, void *buffer,
    size_t length, fsimg_error_t *error)
{
	size_t transferred;
	fsimg_status_t status;

	status = fsimg_reader_bytes(reader, offset, buffer, length,
	    &transferred, error);
	if (status != FSIMG_OK)
		return (status);
	if (transferred != length)
		return (ifs_corrupt(error, "ImageFS image is truncated"));
	return (FSIMG_OK);
}

/* The probe claimed the image from these bytes; open reads them again and
 * applies every header rule, so it never trusts the probe. The content
 * domain then covers the record table and the file data and stops before
 * the trailer, so neither the header nor the trailer, nor any byte of a
 * longer volume past image_size, can be delivered as content.
 */
static fsimg_status_t
read_header(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_ifs_reader_t *ifs = ifs_state(reader);
	unsigned char bytes[IFS_HEADER_BYTES];
	ifs_header_t *header = &ifs->header;
	fsimg_status_t status;

	status = fsimg_io_get_size(reader->input, &ifs->volume_bytes, error);
	if (status != FSIMG_OK)
		return (status);
	if (ifs->volume_bytes < IFS_HEADER_BYTES)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0,
		    FSIMG_OP_READER, "no ImageFS header"));
	status = ifs_read_exact(reader, 0, bytes, sizeof(bytes), error);
	if (status != FSIMG_OK)
		return (status);
	if (bcmp(bytes, IFS_SIGNATURE, IFS_SIGNATURE_BYTES) != 0)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0,
		    FSIMG_OP_READER, "no ImageFS signature"));
	status = ifs_header_decode(bytes, header, error);
	if (status != FSIMG_OK)
		return (status);
	status = ifs_header_bounds(header, ifs->volume_bytes, error);
	if (status != FSIMG_OK)
		return (status);
	if (!header->spare_zero)
		return (ifs_unsupported(error,
		    "ImageFS header spare words are nonzero"));
	if (header->dir_offset % 4 != 0 || header->hdr_dir_size % 4 != 0)
		return (ifs_unsupported(error,
		    "ImageFS table offsets are not aligned"));

	reader->content_domain = (fsimg_extent_domain_t){
		.offset = header->dir_offset,
		.length = (uint64_t)header->image_size - 4 - header->dir_offset,
		.unit_bytes = 1
	};
	return (FSIMG_OK);
}

/* The mountpoint is the NUL-terminated string at 0x58, and dir_offset is
 * the offset after its NUL rounded up to a multiple of 4. One read of at
 * most 1025 bytes decides: no NUL before dir_offset contradicts the
 * header, no NUL in 1025 bytes when dir_offset lies further is a
 * mountpoint over the path limit, and a NUL whose rounding is not
 * dir_offset is an unobserved header shape. The bytes between the NUL and
 * dir_offset are padding: the read covers them, and nothing interprets
 * them. A mountpoint outside the byte rules is kept from info but never
 * blocks reading.
 */
static fsimg_status_t
read_mountpoint(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_ifs_reader_t *ifs = ifs_state(reader);
	unsigned char bytes[IFS_PATH_MAX + 1];
	const unsigned char *nul;
	uint32_t region;
	size_t length, rounded;
	fsimg_status_t status;

	region = ifs->header.dir_offset - IFS_HEADER_BYTES;
	length = region < sizeof(bytes) ? region : sizeof(bytes);
	status = ifs_read_exact(reader, IFS_HEADER_BYTES, bytes, length, error);
	if (status != FSIMG_OK)
		return (status);
	nul = length == 0 ? NULL : memchr(bytes, '\0', length);
	if (nul == NULL && region <= sizeof(bytes))
		return (ifs_corrupt(error,
		    "ImageFS mountpoint has no terminator"));
	if (nul == NULL)
		return (ifs_unsupported(error,
		    "ImageFS mountpoint is longer than 1024 bytes"));
	length = (size_t)(nul - bytes);
	rounded = (IFS_HEADER_BYTES + length + 1 + 3) & ~(size_t)3;
	if (rounded != ifs->header.dir_offset)
		return (ifs_unsupported(error,
		    "ImageFS table does not follow the mountpoint"));

	ifs->mountpoint_valid = ifs_mountpoint_valid(bytes, length);
	if (ifs->mountpoint_valid)
		bcopy(bytes, ifs->mountpoint, length);
	ifs->mountpoint[length] = '\0';
	return (FSIMG_OK);
}

/* Make the window hold [offset, offset + length), reading from offset when
 * it does not. length is at most the window's size.
 */
static fsimg_status_t
window_cover(fsimg_reader_t *reader, table_window_t *window, uint64_t offset,
    size_t length, fsimg_error_t *error)
{
	uint64_t end;
	size_t fill;
	fsimg_status_t status;

	if (offset >= window->offset &&
	    offset + length <= window->offset + window->length)
		return (FSIMG_OK);
	end = ifs_state(reader)->header.hdr_dir_size;
	fill = end - offset < sizeof(window->bytes) ?
	    (size_t)(end - offset) : sizeof(window->bytes);
	window->offset = offset;
	window->length = 0;
	if (fill < length)
		return (ifs_corrupt(error, "ImageFS record crosses the table end"));
	status = ifs_read_exact(reader, offset, window->bytes, fill, error);
	if (status != FSIMG_OK)
		return (status);
	window->length = fill;
	return (FSIMG_OK);
}

/* A regular file's data lies in [hdr_dir_size, image_size - 4), with the
 * end computed in 64 bits. An empty file's offset is not interpreted.
 */
static fsimg_status_t
check_range(const ifs_header_t *header, const ifs_record_t *record,
    fsimg_error_t *error)
{
	uint64_t end;

	if (record->kind != IFS_KIND_FILE || record->data_length == 0)
		return (FSIMG_OK);
	end = (uint64_t)record->data_offset + record->data_length;
	if (record->data_offset < header->hdr_dir_size ||
	    end > (uint64_t)header->image_size - 4)
		return (ifs_corrupt(error,
		    "ImageFS file data lies outside the data region"));
	return (FSIMG_OK);
}

/* Validate the record at table offset and append its path entry to paths;
 * *length receives its length. The record must lie within the table before
 * the terminator, hold at least the 24-byte prefix and be whole words.
 */
static fsimg_status_t
table_record(fsimg_reader_t *reader, table_window_t *window, uint32_t offset,
    fsimg_stream_t *paths, uint32_t *length, fsimg_error_t *error)
{
	fsimg_ifs_reader_t *ifs = ifs_state(reader);
	const ifs_header_t *header = &ifs->header;
	unsigned char entry[IFS_PATH_ENTRY_BYTES];
	const unsigned char *bytes;
	ifs_record_t record;
	size_t available;
	fsimg_status_t status;

	status = window_cover(reader, window, offset, 2, error);
	if (status != FSIMG_OK)
		return (status);
	*length = ifs_record_length(window->bytes + (offset - window->offset),
	    header->order);
	if (*length < IFS_REC_PREFIX ||
	    (uint64_t)offset + *length > (uint64_t)header->hdr_dir_size - 4)
		return (ifs_corrupt(error,
		    "ImageFS record length is invalid or crosses the table end"));
	if (*length % 4 != 0)
		return (ifs_unsupported(error,
		    "ImageFS record length is not a multiple of 4"));
	available = *length < IFS_RECORD_DECODE_BYTES ? *length :
	    IFS_RECORD_DECODE_BYTES;
	status = window_cover(reader, window, offset, available, error);
	if (status != FSIMG_OK)
		return (status);
	bytes = window->bytes + (offset - window->offset);
	status = ifs_record_decode(bytes, available, *length, header->order,
	    &record, error);
	if (status != FSIMG_OK)
		return (status);
	status = ifs_path_check(bytes + record.path_offset, record.path_length,
	    error);
	if (status != FSIMG_OK)
		return (status);
	status = check_range(header, &record, error);
	if (status != FSIMG_OK)
		return (status);

	ifs->counts.records_by_flag[ifs_flag_index(record.inode)]++;
	fsimg_store_le32(entry + IFS_PATH_ENTRY_TABLE_OFFSET, offset);
	fsimg_store_le32(entry + IFS_PATH_ENTRY_INODE, record.inode);
	entry[IFS_PATH_ENTRY_KIND] = (unsigned char)record.kind;
	bcopy(bytes + record.path_offset, entry + IFS_PATH_ENTRY_FIXED,
	    record.path_length);
	return (fsimg_stream_append(paths, entry,
	    IFS_PATH_ENTRY_FIXED + record.path_length, NULL, error));
}

/* The records tile [dir_offset, hdr_dir_size - 4) and four zero bytes end
 * the table. Each step either reaches the terminator or advances by a
 * record of at least 24 bytes that ends before it, so the walk ends; it
 * charges one work unit per record.
 */
static fsimg_status_t
table_walk(fsimg_reader_t *reader, fsimg_stream_t *paths,
    fsimg_error_t *error)
{
	fsimg_ifs_reader_t *ifs = ifs_state(reader);
	uint32_t terminator = ifs->header.hdr_dir_size - 4;
	table_window_t *window;
	fsimg_error_t detail;
	uint32_t offset, length;
	fsimg_status_t status;

	window = fsimg_alloc(reader->ctx, sizeof(*window), &detail);
	if (window == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	window->offset = 0;
	window->length = 0;
	status = FSIMG_OK;
	for (offset = ifs->header.dir_offset; offset < terminator;
	    offset += length) {
		status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
		if (status != FSIMG_OK)
			break;
		status = table_record(reader, window, offset, paths, &length,
		    error);
		if (status != FSIMG_OK)
			break;
	}
	if (status == FSIMG_OK)
		status = window_cover(reader, window, terminator, 4, error);
	if (status == FSIMG_OK &&
	    ifs_u32(window->bytes + (terminator - window->offset),
	    IFS_LITTLE_ENDIAN) != 0)
		status = ifs_corrupt(error, "ImageFS table terminator is nonzero");
	fsimg_dealloc(window);
	return (status);
}

/* Open the image through the table pass. *paths receives the sealed path
 * stream, which the caller frees; on failure it is NULL.
 */
fsimg_status_t
ifs_volume_open(fsimg_reader_t *reader, fsimg_stream_t **paths,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	*paths = NULL;
	status = read_header(reader, error);
	if (status != FSIMG_OK)
		return (status);
	status = read_mountpoint(reader, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_stream_new(&reader->session, IFS_PATH_ENTRY_BYTES, paths,
	    error);
	if (status == FSIMG_OK)
		status = table_walk(reader, *paths, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_seal(*paths, error);
	if (status != FSIMG_OK) {
		fsimg_stream_free(*paths);
		*paths = NULL;
	}
	return (status);
}

/* Read and decode the record at table offset again, for replay. The table
 * pass validated it, but scratch storage is not trusted: the offset must
 * name a record inside the table, and the record is decoded afresh. Its
 * bytes stay in the driver's record buffer.
 */
fsimg_status_t
ifs_record_read(fsimg_reader_t *reader, uint32_t offset, ifs_record_t *record,
    fsimg_error_t *error)
{
	fsimg_ifs_reader_t *ifs = ifs_state(reader);
	const ifs_header_t *header = &ifs->header;
	uint32_t length;
	size_t available;
	fsimg_status_t status;

	if (offset < header->dir_offset ||
	    (uint64_t)offset + IFS_REC_PREFIX > (uint64_t)header->hdr_dir_size - 4)
		return (ifs_corrupt(error, "invalid prepared ImageFS record offset"));
	status = ifs_read_exact(reader, offset, ifs->record, 2, error);
	if (status != FSIMG_OK)
		return (status);
	length = ifs_record_length(ifs->record, header->order);
	if (length < IFS_REC_PREFIX ||
	    (uint64_t)offset + length > (uint64_t)header->hdr_dir_size - 4)
		return (ifs_corrupt(error,
		    "ImageFS record length is invalid or crosses the table end"));
	if (length % 4 != 0)
		return (ifs_unsupported(error,
		    "ImageFS record length is not a multiple of 4"));
	available = length < IFS_RECORD_DECODE_BYTES ? length :
	    IFS_RECORD_DECODE_BYTES;
	status = ifs_read_exact(reader, offset, ifs->record, available, error);
	if (status != FSIMG_OK)
		return (status);
	status = ifs_record_decode(ifs->record, available, length,
	    header->order, record, error);
	if (status != FSIMG_OK)
		return (status);
	return (check_range(header, record, error));
}
