/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <strings.h>

#include "fsimg-endian.h"
#include "unicode.h"
#include "write.h"

static bool encode_timestamp(const fsimg_timestamp_t *, uint32_t,
    unsigned char *);
static fsimg_status_t encode_field(fat_plan_t *, const fsimg_entry_t *,
    uint32_t, const fsimg_timestamp_t *, unsigned char *, fsimg_error_t *);

/* Place a time's seconds, read as calendar seconds, as a DOS date and time
 * in field's slots of raw: creation keeps 10 ms increments including the
 * odd second, modification two-second steps, and access only the date.
 * Return whether the stored fields keep the whole time, which needs
 * wall-clock basis; leave raw untouched and return false outside the DOS
 * calendar.
 */
static bool
encode_timestamp(const fsimg_timestamp_t *time, uint32_t field,
    unsigned char *raw)
{
	uint32_t packed;
	uint16_t date, clock;
	unsigned char odd;
	bool exact;

	if (!fsimg_dos_time_encode(time->seconds, &packed, &odd))
		return (false);
	date = (uint16_t)(packed >> 16);
	clock = (uint16_t)packed;
	exact = time->basis == FSIMG_TIME_WALL;
	if (field == FSIMG_META_CREATED) {
		raw[13] = (unsigned char)(odd * 100 + time->nanoseconds / 10000000);
		fsimg_store_le16(raw + 14, clock);
		fsimg_store_le16(raw + 16, date);
		exact = exact && time->nanoseconds % 10000000 == 0;
	} else if (field == FSIMG_META_MODIFIED) {
		fsimg_store_le16(raw + 22, clock);
		fsimg_store_le16(raw + 24, date);
		exact = exact && odd == 0 && time->nanoseconds == 0;
	} else {
		fsimg_store_le16(raw + 18, date);
		exact = exact && clock == 0 && odd == 0 && time->nanoseconds == 0;
	}
	return (exact);
}

/* Encode one timestamp field over the initialized fallback: the caller's
 * time, or in reproducible mode the epoch read as wall-clock seconds. A
 * present field is preserved when the encoding is exact and, when
 * reproducible, the epoch is the original wall-clock whole second; any
 * other present field is a loss.
 */
static fsimg_status_t
encode_field(fat_plan_t *plan, const fsimg_entry_t *entry, uint32_t field,
    const fsimg_timestamp_t *original, unsigned char *raw, fsimg_error_t *error)
{
	fsimg_writer_t *writer;
	fsimg_timestamp_t selected;
	bool exact;

	writer = plan->common.writer;
	selected = (fsimg_timestamp_t){ .seconds = 315532800,
	    .basis = FSIMG_TIME_WALL };
	if ((entry->metadata.valid & field) != 0)
		selected = *original;
	if ((writer->options.flags & FSIMG_WRITE_REPRODUCIBLE) != 0) {
		selected.seconds = writer->options.epoch;
		selected.nanoseconds = 0;
		selected.basis = FSIMG_TIME_WALL;
		selected.offset_minutes = 0;
	}
	exact = encode_timestamp(&selected, field, raw);
	if ((entry->metadata.valid & field) == 0)
		return (FSIMG_OK);
	if ((writer->options.flags & FSIMG_WRITE_REPRODUCIBLE) != 0)
		exact = exact && original->seconds == selected.seconds &&
		    original->nanoseconds == 0 && original->basis == FSIMG_TIME_WALL;
	if (exact)
		return (FSIMG_OK);
	return (fsimg_writer_loss(writer, entry, field,
	    "FAT cannot preserve the requested timestamp", error));
}

fsimg_status_t
fsimg_fat_write_metadata(fat_plan_t *plan, const fsimg_entry_t *entry,
    unsigned char *raw, fsimg_error_t *error)
{
	fsimg_timestamp_t fallback = { .seconds = 315532800,
	    .basis = FSIMG_TIME_WALL };
	fsimg_status_t status;
	uint32_t attributes;

	bzero(raw, 32);
	attributes = (entry->metadata.valid & FSIMG_META_ATTRIBUTES) != 0 ?
	    entry->metadata.attributes : 0;
	raw[11] = (unsigned char)(((attributes & FSIMG_ATTR_READONLY) != 0 ? 1 : 0) |
	    ((attributes & FSIMG_ATTR_HIDDEN) != 0 ? 2 : 0) |
	    ((attributes & FSIMG_ATTR_SYSTEM) != 0 ? 4 : 0) |
	    ((attributes & FSIMG_ATTR_ARCHIVE) != 0 ? 32 : 0));
	if (entry->kind == FSIMG_ENTRY_DIRECTORY)
		raw[11] |= 16;
	else
		fsimg_store_le32(raw + 28, (uint32_t)entry->size);

	/* Defaults remain in place when an optional out-of-range field is lost. */
	encode_timestamp(&fallback, FSIMG_META_CREATED, raw);
	encode_timestamp(&fallback, FSIMG_META_MODIFIED, raw);
	encode_timestamp(&fallback, FSIMG_META_ACCESSED, raw);
	status = encode_field(plan, entry, FSIMG_META_CREATED,
	    &entry->metadata.created, raw, error);
	if (status != FSIMG_OK)
		return (status);
	status = encode_field(plan, entry, FSIMG_META_MODIFIED,
	    &entry->metadata.modified, raw, error);
	if (status != FSIMG_OK)
		return (status);
	return (encode_field(plan, entry, FSIMG_META_ACCESSED,
	    &entry->metadata.accessed, raw, error));
}
