/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "exfat.h"
#include "host.h"
#include "unicode.h"

static fsimg_status_t exfat_open(fsimg_reader_t *,
    const fsimg_format_variant_t *, fsimg_error_t *);
static void exfat_close(fsimg_reader_t *);
static fsimg_status_t discover_metadata(fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t load_upcase(fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t exfat_match_name(fsimg_reader_t *, const char *, bool *,
    fsimg_error_t *);
static fsimg_status_t exfat_skip_directory(fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t exfat_read(fsimg_reader_t *, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t exfat_range(fsimg_reader_t *, uint64_t, fsimg_range_t *,
    fsimg_error_t *);

const fsimg_reader_ops_t fsimg_exfat_reader_ops = {
	.open = exfat_open,
	.close = exfat_close,
	.next = exfat_next,
	.match_name = exfat_match_name,
	.skip_directory = exfat_skip_directory,
	.read = exfat_read,
	.range = exfat_range,
	.verify = exfat_verify
};

fsimg_exfat_reader_t *
exfat_state(const fsimg_reader_t *reader)
{
	return (reader->driver);
}

static fsimg_status_t
discover_metadata(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_exfat_reader_t *exfat = exfat_state(reader);
	exfat_directory_cursor_t *cursor;
	exfat_stream_t bitmap = { 0 }, upcase = { 0 };
	const unsigned char *raw;
	uint16_t label[11];
	size_t written;
	unsigned bitmap_count, upcase_count, label_count;
	fsimg_error_t detail;
	fsimg_status_t status;

	cursor = fsimg_alloc(reader->ctx, sizeof(*cursor), &detail);
	if (cursor == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	cursor->reader = reader;
	cursor->map = &exfat->root;
	cursor->root = true;
	bitmap_count = upcase_count = label_count = 0;
	for (;;) {
		status = exfat_directory_set_next(cursor, error);
		if (status == FSIMG_DONE)
			break;
		if (status != FSIMG_OK)
			goto done;
		raw = cursor->set;
		/* The shared cursor establishes framing and checksums. File names
		 * remain uninterpreted until their volume's upcase table is validated.
		 */
		if (raw[0] == 0x81) {
			unsigned char bitmap_flags = raw[1];

			if (++bitmap_count != 1 || (bitmap_flags & 1) != 0) {
				status = exfat_corrupt(error, "duplicate or inactive exFAT bitmap");
				goto done;
			}
			bitmap.first = exfat_u32(raw + 20);
			bitmap.length = fsimg_meta_get_u64(raw + 24);
		} else if (raw[0] == 0x82) {
			if (++upcase_count != 1) {
				status = exfat_corrupt(error, "duplicate exFAT upcase table");
				goto done;
			}
			upcase.first = exfat_u32(raw + 20);
			upcase.length = fsimg_meta_get_u64(raw + 24);
			exfat->upcase_checksum = exfat_u32(raw + 4);
		} else if (raw[0] == 0x83) {
			if (++label_count != 1 || raw[1] > 11) {
				status = exfat_corrupt(error, "invalid or duplicate exFAT volume label");
				goto done;
			}
			for (unsigned j = 0; j < raw[1]; j++) {
				label[j] = exfat_u16(raw + 2 + j * 2);
				if (!exfat_character_valid(label[j])) {
					status = exfat_corrupt(error, "forbidden character in exFAT label");
					goto done;
				}
			}
			status = fsimg_utf16_to_utf8(label, raw[1], reader->info.label,
			    sizeof(reader->info.label) - 1, &written, error);
			if (status != FSIMG_OK) {
				status = exfat_corrupt(error, "invalid exFAT label encoding");
				goto done;
			}
			reader->info.label[written] = '\0';
			reader->info.valid |= FSIMG_VOLUME_LABEL;
		}
	}
	status = FSIMG_OK;
done:
	fsimg_dealloc(cursor);
	if (status != FSIMG_OK)
		return (status);
	if (bitmap_count != 1 || upcase_count != 1 ||
	    bitmap.length < ((uint64_t)exfat->geometry.clusters + 7) / 8 ||
	    upcase.length == 0 || upcase.length % 2 != 0)
		return (exfat_corrupt(error, "missing or undersized exFAT metadata"));
	bitmap.valid_length = bitmap.length;
	upcase.valid_length = upcase.length;
	status = exfat_map_build(reader, &bitmap, EXFAT_LENGTH_COVER_DATA, &exfat->bitmap, error);
	if (status != FSIMG_OK)
		return (status);
	exfat->bitmap_length = bitmap.length;
	exfat->bitmap_ready = true;
	/* Root and bitmap maps predate the allocation bitmap. Every later map
	 * validates its allocation inside map_build; these bootstrap maps cannot.
	 */
	status = exfat_map_check(reader, &exfat->bitmap, error);
	if (status != FSIMG_OK)
		return (status);
	status = exfat_map_check(reader, &exfat->root, error);
	if (status != FSIMG_OK)
		return (status);
	status = exfat_map_build(reader, &upcase, EXFAT_LENGTH_COVER_DATA, &exfat->upcase_map, error);
	if (status != FSIMG_OK)
		return (status);
	exfat->upcase_length = upcase.length;
	return (FSIMG_OK);
}

static fsimg_status_t
load_upcase(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_exfat_reader_t *exfat = exfat_state(reader);
	unsigned char *buffer;
	uint64_t offset;
	uint32_t decoded, checksum;
	uint16_t word;
	size_t batch;
	bool run;
	fsimg_error_t detail;
	fsimg_status_t status;

	exfat->upcase = fsimg_alloc(reader->ctx, EXFAT_UPCASE_BYTES, &detail);
	if (exfat->upcase == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	buffer = fsimg_alloc(reader->ctx, EXFAT_BUFFER_BYTES, &detail);
	if (buffer == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	decoded = checksum = 0;
	run = false;
	for (offset = 0; offset < exfat->upcase_length; offset += batch) {
		batch = exfat->upcase_length - offset > EXFAT_BUFFER_BYTES ?
		    EXFAT_BUFFER_BYTES : (size_t)(exfat->upcase_length - offset);
		status = exfat_map_read(reader, &exfat->upcase_map, offset, buffer, batch, error);
		if (status != FSIMG_OK)
			goto done;
		/* Empty identity runs still consume encoded words. Charge input
		 * interpretation independently of how many mappings it produces.
		 */
		status = fsimg_session_charge(&reader->session, 0, 0, batch / 2, error);
		if (status != FSIMG_OK)
			goto done;
		for (size_t i = 0; i < batch; i += 2) {
			checksum = exfat_checksum32(checksum, buffer[i]);
			checksum = exfat_checksum32(checksum, buffer[i + 1]);
			word = exfat_u16(buffer + i);
			if (run) {
				if (word > 65536 - decoded) {
					status = exfat_corrupt(error, "invalid exFAT upcase identity run");
					goto done;
				}
				status = fsimg_session_charge(&reader->session, 0, 0, word, error);
				if (status != FSIMG_OK)
					goto done;
				for (unsigned j = 0; j < word; j++) {
					exfat->upcase[decoded] = (uint16_t)decoded;
					decoded++;
				}
				run = false;
			} else if (word == 0xffff &&
			    !(decoded == 65535 && offset + i + 2 == exfat->upcase_length)) {
				run = true;
			} else {
				if (decoded == 65536) {
					status = exfat_corrupt(error, "exFAT upcase table has excess mappings");
					goto done;
				}
				exfat->upcase[decoded++] = word;
			}
		}
	}
	if (run || decoded != 65536 || checksum != exfat->upcase_checksum) {
		status = exfat_corrupt(error, "incomplete exFAT upcase table or checksum mismatch");
		goto done;
	}
	for (unsigned i = 0; i < 128; i++) {
		if (exfat->upcase[i] != (i >= 'a' && i <= 'z' ? i - 32 : i)) {
			status = exfat_corrupt(error, "invalid mandatory exFAT upcase mapping");
			goto done;
		}
	}
	status = FSIMG_OK;
done:
	fsimg_dealloc(buffer);
	return (status);
}

static fsimg_status_t
exfat_open(fsimg_reader_t *reader,
    const fsimg_format_variant_t *forced FSIMG_UNUSED, fsimg_error_t *error)
{
	fsimg_exfat_reader_t *exfat;
	exfat_stream_t root;
	uint32_t value;
	fsimg_error_t detail;
	fsimg_status_t status;

	exfat = fsimg_alloc(reader->ctx, sizeof(*exfat), &detail);
	if (exfat == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	reader->driver = exfat;
	status = exfat_boot_read(reader, error);
	if (status != FSIMG_OK)
		return (status);
	status = exfat_table_get(reader, 0, &value, error);
	if (status != FSIMG_OK)
		return (status);
	if ((value & UINT32_C(0xffffff00)) != UINT32_C(0xffffff00))
		return (exfat_corrupt(error, "invalid exFAT media descriptor"));
	root = (exfat_stream_t){ .first = exfat->geometry.root_cluster };
	status = exfat_map_build(reader, &root, EXFAT_LENGTH_COVER_DATA, &exfat->root, error);
	if (status != FSIMG_OK)
		return (status);
	status = discover_metadata(reader, error);
	if (status != FSIMG_OK)
		return (status);
	status = load_upcase(reader, error);
	if (status != FSIMG_OK)
		return (status);
	strcpy(reader->info.format, "exfat");
	strcpy(reader->info.profile, "embedded-upcase");
	reader->info.valid |= FSIMG_VOLUME_GEOMETRY;
	reader->info.volume_bytes = exfat->geometry.volume_bytes;
	reader->info.sector_bytes = exfat->geometry.sector_bytes;
	reader->info.allocation_unit_bytes = exfat->geometry.cluster_bytes;
	reader->info.allocation_units = exfat->geometry.clusters;
	reader->info.time_basis = FSIMG_TIME_PER_TIMESTAMP;
	reader->info.scope = FSIMG_VALIDATION_HEADER;
	reader->info.clean = (exfat->geometry.flags & 2) == 0;
	reader->info.io_error = (exfat->geometry.flags & 4) != 0;
	return (FSIMG_OK);
}

static void
exfat_close(fsimg_reader_t *reader)
{
	fsimg_exfat_reader_t *exfat = exfat_state(reader);

	if (exfat == NULL)
		return;
	if (exfat->directories != NULL) {
		for (uint32_t i = 0; i <= exfat->depth; i++)
			fsimg_stream_free(exfat->directories[i].entries);
	}
	exfat_map_free(&exfat->root);
	exfat_map_free(&exfat->bitmap);
	exfat_map_free(&exfat->upcase_map);
	exfat_map_free(&exfat->file);
	fsimg_bitset_free(exfat->directory_clusters);
	fsimg_stream_free(exfat->owners);
	fsimg_dealloc(exfat->directories);
	fsimg_dealloc(exfat->upcase);
	fsimg_dealloc(exfat);
	reader->driver = NULL;
}

static fsimg_status_t
exfat_match_name(fsimg_reader_t *reader, const char *component, bool *matches,
    fsimg_error_t *error)
{
	const uint16_t *upcase = exfat_state(reader)->upcase;
	uint16_t query[255], current[255];
	size_t query_count, current_count;
	const char *name;
	fsimg_status_t status;

	*matches = false;
	status = exfat_name_units(component, query, &query_count, error);
	if (status != FSIMG_OK)
		return (status);
	name = strrchr(reader->entry.path, '/') + 1;
	status = exfat_name_units(name, current, &current_count, error);
	if (status != FSIMG_OK)
		return (status);
	if (query_count != current_count)
		return (FSIMG_OK);
	for (size_t i = 0; i < query_count; i++) {
		if (upcase[query[i]] != upcase[current[i]])
			return (FSIMG_OK);
	}
	*matches = true;
	return (FSIMG_OK);
}

static fsimg_status_t
exfat_skip_directory(fsimg_reader_t *reader, fsimg_error_t *error FSIMG_UNUSED)
{
	exfat_state(reader)->pending_directory = false;
	return (FSIMG_OK);
}

static fsimg_status_t
exfat_read(fsimg_reader_t *reader, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error)
{
	fsimg_exfat_reader_t *exfat = exfat_state(reader);
	uint64_t image_offset, extent_length;
	size_t initialized, batch, count;
	fsimg_status_t status;

	initialized = length;
	if (offset >= exfat->current.valid_length)
		initialized = 0;
	else if (initialized > exfat->current.valid_length - offset)
		initialized = (size_t)(exfat->current.valid_length - offset);
	/* Metadata map reads require complete records. Content instead preserves
	 * each confirmed byte count when a later extent or physical read fails.
	 */
	while (*transferred < initialized) {
		status = exfat_map_extent(&exfat->geometry, &exfat->file,
		    offset + *transferred, &image_offset, &extent_length, error);
		if (status != FSIMG_OK)
			return (status);
		batch = initialized - *transferred;
		if (batch > extent_length)
			batch = (size_t)extent_length;
		status = fsimg_reader_bytes(reader, image_offset,
		    (unsigned char *)buffer + *transferred, batch, &count, error);
		*transferred += count;
		if (status != FSIMG_OK)
			return (status);
	}
	if (length > initialized)
		bzero((unsigned char *)buffer + initialized, length - initialized);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
exfat_range(fsimg_reader_t *reader, uint64_t offset, fsimg_range_t *out,
    fsimg_error_t *error FSIMG_UNUSED)
{
	const exfat_stream_t *stream = &exfat_state(reader)->current;

	out->offset = offset;
	out->kind = offset < stream->valid_length ? FSIMG_RANGE_DATA : FSIMG_RANGE_ZERO;
	out->length = (offset < stream->valid_length ? stream->valid_length :
	    stream->length) - offset;
	return (FSIMG_OK);
}
