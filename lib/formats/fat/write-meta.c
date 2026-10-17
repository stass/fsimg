/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <strings.h>

#include "write.h"

static bool encode_timestamp(const fsimg_timestamp_t *, uint32_t,
    unsigned char *);
static fsimg_status_t encode_field(fat_plan_t *, const fsimg_entry_t *,
    uint32_t, const fsimg_timestamp_t *, unsigned char *, fsimg_error_t *);

void
fsimg_fat_put16(unsigned char *p, uint16_t value)
{
	p[0] = (unsigned char)value;
	p[1] = (unsigned char)(value >> 8);
}

void
fsimg_fat_put32(unsigned char *p, uint32_t value)
{
	fsimg_fat_put16(p, (uint16_t)value);
	fsimg_fat_put16(p + 2, (uint16_t)(value >> 16));
}

/* Count complete Gregorian years/months from the Unix epoch. The supported
 * interval has only 128 years, so this bounded calendar walk is transparent
 * and avoids host timezones, time_t ranges, and signed division conventions.
 */
static bool
encode_timestamp(const fsimg_timestamp_t *time, uint32_t field,
    unsigned char *raw)
{
	static const unsigned month_days[] = {
		31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
	};
	uint64_t days, seconds;
	unsigned year, month, span, day_seconds;
	uint16_t date, clock;
	bool leap, exact;

	if (time->seconds < 315532800 || time->seconds > 4354819199LL)
		return (false);
	seconds = (uint64_t)time->seconds;
	days = seconds / 86400;
	day_seconds = (unsigned)(seconds % 86400);
	year = 1970;
	for (;;) {
		leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
		span = leap ? 366 : 365;
		if (days < span)
			break;
		days -= span;
		year++;
	}
	month = 0;
	for (;;) {
		span = month_days[month] + (month == 1 && leap ? 1 : 0);
		if (days < span)
			break;
		days -= span;
		month++;
	}
	date = (uint16_t)((year - 1980) << 9 | (month + 1) << 5 | (days + 1));
	clock = (uint16_t)((day_seconds / 3600) << 11 |
	    (day_seconds / 60 % 60) << 5 | (day_seconds % 60 / 2));
	exact = time->basis == FSIMG_TIME_WALL;
	if (field == FSIMG_META_CREATED) {
		raw[13] = (unsigned char)((seconds % 2) * 100 +
		    time->nanoseconds / 10000000);
		fsimg_fat_put16(raw + 14, clock);
		fsimg_fat_put16(raw + 16, date);
		exact = exact && time->nanoseconds % 10000000 == 0;
	} else if (field == FSIMG_META_MODIFIED) {
		fsimg_fat_put16(raw + 22, clock);
		fsimg_fat_put16(raw + 24, date);
		exact = exact && seconds % 2 == 0 && time->nanoseconds == 0;
	} else {
		fsimg_fat_put16(raw + 18, date);
		exact = exact && day_seconds == 0 && time->nanoseconds == 0;
	}
	return (exact);
}

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
	if ((entry->metadata.required & field) != 0 ||
	    (writer->options.flags & FSIMG_WRITE_ALLOW_LOSS) == 0)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_WRITER, "FAT cannot preserve the requested timestamp"));
	plan->common.info.metadata_losses++;
	fsimg_report_loss(writer->ctx, entry->path, field);
	return (FSIMG_OK);
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
		fsimg_fat_put32(raw + 28, (uint32_t)entry->size);

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
