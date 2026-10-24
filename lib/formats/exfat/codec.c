/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "exfat.h"
#include "fsimg-endian.h"
#include "unicode.h"

static fsimg_status_t decode_time(const unsigned char *, unsigned,
    fsimg_timestamp_t *, fsimg_error_t *);
static bool encode_timestamp(const fsimg_timestamp_t *, unsigned, unsigned char *);
static fsimg_status_t encode_time(fsimg_plan_t *, const fsimg_entry_t *,
    uint32_t, const fsimg_timestamp_t *, unsigned, unsigned char *,
    fsimg_error_t *);
uint32_t
exfat_checksum32(uint32_t sum, unsigned char byte)
{
	return ((sum >> 1 | sum << 31) + byte);
}

uint16_t
exfat_checksum16(uint16_t sum, unsigned char byte)
{
	return ((uint16_t)((sum >> 1 | (uint32_t)sum << 15) + byte));
}

uint16_t
exfat_set_checksum(const unsigned char *set, size_t length)
{
	uint16_t sum = 0;

	for (size_t i = 0; i < length; i++) {
		if (i != 2 && i != 3)
			sum = exfat_checksum16(sum, set[i]);
	}
	return (sum);
}

uint16_t
exfat_name_hash(const uint16_t *units, size_t count, const uint16_t *upcase)
{
	uint16_t mapped, hash = 0;

	for (size_t i = 0; i < count; i++) {
		mapped = upcase == NULL ? (uint16_t)fsimg_unicode_upper(units[i]) :
		    upcase[units[i]];
		hash = exfat_checksum16(hash, (unsigned char)mapped);
		hash = exfat_checksum16(hash, (unsigned char)(mapped >> 8));
	}
	return (hash);
}

fsimg_status_t
exfat_corrupt(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
	    FSIMG_OP_READER, message));
}

bool
exfat_cluster_valid(const exfat_geometry_t *g, uint32_t cluster)
{
	return (cluster >= 2 && cluster - 2 < g->clusters);
}

uint64_t
exfat_cluster_offset(const exfat_geometry_t *g, uint32_t cluster)
{
	return (g->heap_offset + (uint64_t)(cluster - 2) * g->cluster_bytes);
}

bool
exfat_character_valid(uint16_t unit)
{
	return (unit >= 0x20 && (unit >= 0x80 ||
	    strchr("\"*/:<>?\\|", (int)unit) == NULL));
}

fsimg_status_t
exfat_name_units(const char *name, uint16_t *units, size_t *count,
    fsimg_error_t *error)
{
	fsimg_status_t status;
	size_t length;

	length = strlen(name);
	status = fsimg_utf8_to_utf16(name, length, units, 255, count, error);
	if (status != FSIMG_OK)
		return (status);
	if (*count == 0 || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "invalid exFAT name length or dot component"));
	for (size_t i = 0; i < *count; i++) {
		if (!exfat_character_valid(units[i]))
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_WRITER, "forbidden exFAT filename character"));
	}
	return (FSIMG_OK);
}

fsimg_status_t
exfat_stream_validate(const exfat_geometry_t *g, const exfat_stream_t *stream,
    bool directory, fsimg_error_t *error)
{
	uint64_t capacity, clusters;

	capacity = (uint64_t)g->clusters * g->cluster_bytes;
	if (stream->length > capacity || stream->valid_length > stream->length ||
	    (stream->first != 0 && !exfat_cluster_valid(g, stream->first)) ||
	    (stream->first == 0 && stream->length != 0) ||
	    (stream->contiguous && (stream->first == 0 || stream->length == 0)))
		return (exfat_corrupt(error, "invalid exFAT stream lengths or first cluster"));
	clusters = stream->length / g->cluster_bytes +
	    (stream->length % g->cluster_bytes != 0);
	if (stream->contiguous && clusters > (uint64_t)g->clusters + 2 - stream->first)
		return (exfat_corrupt(error, "exFAT contiguous stream exceeds heap"));
	if (directory && (stream->length > EXFAT_DIRECTORY_BYTES ||
	    stream->length % g->cluster_bytes != 0 ||
	    stream->valid_length != stream->length))
		return (exfat_corrupt(error, "invalid exFAT directory allocation length"));
	return (FSIMG_OK);
}

static fsimg_status_t
decode_time(const unsigned char *raw, unsigned index, fsimg_timestamp_t *out,
    fsimg_error_t *error)
{
	uint32_t packed;
	unsigned increment, offset_byte;
	int offset;

	packed = fsimg_load_le32(raw + 8 + index * 4);
	increment = index < 2 ? raw[20 + index] : 0;
	if (!fsimg_dos_time_decode((uint16_t)(packed >> 16), (uint16_t)packed,
	    increment, index < 2 ? 10000000 : 2000000000, out))
		return (exfat_corrupt(error, "invalid exFAT calendar timestamp"));
	offset_byte = raw[22 + index];
	if ((offset_byte & 0x80) == 0 && offset_byte != 0)
		return (exfat_corrupt(error, "nonzero exFAT offset without OffsetValid"));
	if ((offset_byte & 0x80) != 0) {
		offset = (int)(offset_byte & 0x7f);
		if (offset >= 64)
			offset -= 128;
		out->seconds -= offset * 900;
		out->basis = offset == 0 ? FSIMG_TIME_UTC : FSIMG_TIME_OFFSET;
		out->offset_minutes = offset * 15;
	}
	return (FSIMG_OK);
}

fsimg_status_t
exfat_entry_decode(const unsigned char *raw, fsimg_entry_t *entry,
    exfat_stream_t *stream, fsimg_error_t *error)
{
	fsimg_timestamp_t *times[3];
	fsimg_status_t status;
	uint16_t attributes;

	bzero(entry, sizeof(*entry));
	bzero(stream, sizeof(*stream));
	if (raw[0] != 0x85 || raw[32] != 0xc0 || (raw[33] & 1) == 0)
		return (exfat_corrupt(error, "invalid exFAT file or stream entry"));
	attributes = fsimg_load_le16(raw + 4);
	entry->kind = (attributes & 16) != 0 ?
	    FSIMG_ENTRY_DIRECTORY : FSIMG_ENTRY_FILE;
	stream->length = fsimg_load_le64(raw + 56);
	stream->valid_length = fsimg_load_le64(raw + 40);
	stream->first = fsimg_load_le32(raw + 52);
	stream->contiguous = (raw[33] & 2) != 0;
	entry->size = entry->kind == FSIMG_ENTRY_FILE ? stream->length : 0;
	entry->metadata.attributes = (attributes & 7) |
	    ((attributes & 32) != 0 ? FSIMG_ATTR_ARCHIVE : 0);
	entry->metadata.valid = FSIMG_META_ATTRIBUTES;
	times[0] = &entry->metadata.created;
	times[1] = &entry->metadata.modified;
	times[2] = &entry->metadata.accessed;
	for (unsigned i = 0; i < 3; i++) {
		status = decode_time(raw, i, times[i], error);
		if (status != FSIMG_OK)
			return (status);
		entry->metadata.valid |= FSIMG_META_CREATED << i;
	}
	return (FSIMG_OK);
}

/* Leave the caller's initialized default untouched if the calendar or zone
 * cannot be encoded. Otherwise emit the selected value and report whether
 * rounding to this field's resolution preserved its complete value.
 */
static bool
encode_timestamp(const fsimg_timestamp_t *selected, unsigned index,
    unsigned char *raw)
{
	int64_t local_seconds;
	uint32_t packed;
	unsigned char odd, offset;

	if (selected->offset_minutes % 15 != 0 ||
	    selected->offset_minutes < -960 || selected->offset_minutes > 945)
		return (false);
	local_seconds = selected->seconds;
	if (selected->basis == FSIMG_TIME_OFFSET) {
		if ((selected->offset_minutes > 0 &&
		    local_seconds > INT64_MAX - selected->offset_minutes * 60) ||
		    (selected->offset_minutes < 0 &&
		    local_seconds < INT64_MIN - selected->offset_minutes * 60))
			return (false);
		local_seconds += selected->offset_minutes * 60;
	}
	if (!fsimg_dos_time_encode(local_seconds, &packed, &odd))
		return (false);
	offset = selected->basis == FSIMG_TIME_WALL ? 0 :
	    (unsigned char)(0x80 | ((selected->offset_minutes / 15) & 0x7f));
	fsimg_store_le32(raw + 8 + index * 4, packed);
	raw[22 + index] = offset;
	if (index < 2) {
		raw[20 + index] = (unsigned char)(odd * 100 +
		    selected->nanoseconds / 10000000);
		return (selected->nanoseconds % 10000000 == 0);
	}
	return (odd == 0 && selected->nanoseconds == 0);
}

static fsimg_status_t
encode_time(fsimg_plan_t *plan, const fsimg_entry_t *entry, uint32_t field,
    const fsimg_timestamp_t *original, unsigned index, unsigned char *raw,
    fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->writer;
	fsimg_timestamp_t selected;
	const fsimg_timestamp_t fallback = { .seconds = 315532800,
	    .basis = FSIMG_TIME_UTC };
	bool exact, reproducible;

	encode_timestamp(&fallback, index, raw);
	selected = fallback;
	if ((entry->metadata.valid & field) != 0)
		selected = *original;
	reproducible = (writer->options.flags & FSIMG_WRITE_REPRODUCIBLE) != 0;
	if (reproducible)
		selected = (fsimg_timestamp_t){ .seconds = writer->options.epoch,
		    .basis = FSIMG_TIME_UTC };

	/* Encoding fidelity and reproducible substitution are separate tests:
	 * the selected value must fit exactly, and a substituted value must also
	 * equal the caller's original instant, fraction and time basis.
	 */
	exact = encode_timestamp(&selected, index, raw);
	if ((entry->metadata.valid & field) == 0)
		return (FSIMG_OK);
	if (reproducible)
		exact = exact && original->seconds == selected.seconds &&
		    original->nanoseconds == 0 && original->basis == FSIMG_TIME_UTC;
	if (exact)
		return (FSIMG_OK);
	return (fsimg_writer_loss(writer, entry, field,
	    "exFAT cannot preserve the requested timestamp", error));
}

fsimg_status_t
exfat_metadata_encode(fsimg_plan_t *plan, const fsimg_entry_t *entry,
    unsigned char *raw, fsimg_error_t *error)
{
	const fsimg_timestamp_t *times[3];
	uint32_t attributes;
	fsimg_status_t status;

	bzero(raw, 32);
	raw[0] = 0x85;
	attributes = (entry->metadata.valid & FSIMG_META_ATTRIBUTES) != 0 ?
	    entry->metadata.attributes : 0;
	fsimg_store_le16(raw + 4, (uint16_t)((attributes & 7) |
	    ((attributes & FSIMG_ATTR_ARCHIVE) != 0 ? 32 : 0) |
	    (entry->kind == FSIMG_ENTRY_DIRECTORY ? 16 : 0)));
	times[0] = &entry->metadata.created;
	times[1] = &entry->metadata.modified;
	times[2] = &entry->metadata.accessed;
	for (unsigned i = 0; i < 3; i++) {
		status = encode_time(plan, entry, FSIMG_META_CREATED << i,
		    times[i], i, raw, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}
