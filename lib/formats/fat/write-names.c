/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "fat.h"
#include "unicode.h"
#include "write.h"

/* Both catalog children and reservations are grouped by increasing parent
 * ID. Within a parent reservations are ordered by alias number. This cursor
 * is the merge's sole reservation state; every advance consumes one record.
 */
typedef struct fat_alias_cursor {
	fsimg_stream_t	*stream;
	uint64_t	 offset;
	uint64_t	 parent_id;
	uint64_t	 number;
	bool		 available;
} fat_alias_cursor_t;

static fsimg_status_t next_reservation(fat_alias_cursor_t *, fsimg_error_t *);
static fsimg_status_t skip_reserved_aliases(fat_alias_cursor_t *, uint64_t,
    uint64_t *, fsimg_error_t *);
static int key_compare(const void *, size_t, const void *, size_t, void *);
static int reservation_compare(const void *, size_t, const void *, size_t,
    void *);
static bool alias_reservation(const unsigned char *, size_t, uint64_t *);
static fsimg_status_t collect_names(fsimg_plan_t *, fsimg_stream_t *,
    fsimg_stream_t *, fsimg_error_t *);
static fsimg_status_t check_names(fsimg_stream_t *, fsimg_error_t *);
static fsimg_status_t assign_aliases(fsimg_plan_t *, fsimg_stream_t *,
    fsimg_error_t *);

bool
fsimg_fat_write_short_name(const char *name, unsigned char *raw)
{
	const char *dot;
	size_t base, extension;

	dot = strchr(name, '.');
	base = dot == NULL ? strlen(name) : (size_t)(dot - name);
	extension = dot == NULL ? 0 : strlen(dot + 1);
	if (base == 0 || base > 8 || extension > 3 ||
	    (dot != NULL && extension == 0))
		return (false);
	memset(raw, ' ', 11);
	for (size_t i = 0; name[i] != '\0'; i++) {
		if (dot != NULL && name + i == dot)
			continue;
		if (!((name[i] >= 'A' && name[i] <= 'Z') ||
		    (name[i] >= '0' && name[i] <= '9') ||
		    strchr("$%'-_@~`!(){}^#&", name[i]) != NULL))
			return (false);
	}
	bcopy(name, raw, base);
	if (dot != NULL)
		bcopy(dot + 1, raw + 8, extension);
	return (true);
}

static int
key_compare(const void *a, size_t an, const void *b, size_t bn,
    void *argument FSIMG_UNUSED)
{
	uint64_t ap, bp;
	size_t common;
	int order;

	ap = fsimg_meta_get_u64(a);
	bp = fsimg_meta_get_u64(b);
	if (ap != bp)
		return (ap < bp ? -1 : 1);
	common = an < bn ? an : bn;
	order = memcmp((const unsigned char *)a + 8,
	    (const unsigned char *)b + 8, common - 8);
	if (order != 0)
		return (order);
	return (an < bn ? -1 : an > bn ? 1 : 0);
}

static int
reservation_compare(const void *a, size_t an FSIMG_UNUSED, const void *b,
    size_t bn FSIMG_UNUSED, void *argument FSIMG_UNUSED)
{
	uint64_t av, bv;

	for (unsigned offset = 0; offset < 16; offset += 8) {
		av = fsimg_meta_get_u64((const unsigned char *)a + offset);
		bv = fsimg_meta_get_u64((const unsigned char *)b + offset);
		if (av != bv)
			return (av < bv ? -1 : 1);
	}
	return (0);
}

/* Case keys are big-endian scalars. Reserving after case mapping also covers
 * a lowercase spelling of Fhhhhhhh that will itself receive a long name.
 */
static bool
alias_reservation(const unsigned char *key, size_t length, uint64_t *number)
{
	unsigned char c;
	uint64_t value;

	if (length != 32)
		return (false);
	value = 0;
	for (unsigned i = 0; i < 8; i++) {
		if (key[i * 4] != 0 || key[i * 4 + 1] != 0 || key[i * 4 + 2] != 0)
			return (false);
		c = key[i * 4 + 3];
		if (i == 0) {
			if (c != 'F')
				return (false);
		} else if (c >= '0' && c <= '9')
			value = value * 16 + c - '0';
		else if (c >= 'A' && c <= 'F')
			value = value * 16 + c - 'A' + 10;
		else
			return (false);
	}
	*number = value;
	return (true);
}

static fsimg_status_t
collect_names(fsimg_plan_t *plan, fsimg_stream_t *names,
    fsimg_stream_t *reservations, fsimg_error_t *error)
{
	const fsimg_catalog_entry_t *catalog;
	fat_layout_t row;
	unsigned char key[8 + FAT_KEY_BYTES], reservation[16];
	const char *name;
	uint64_t number;
	size_t length;
	fsimg_status_t status;

	bzero(&row, sizeof(row));
	row.slots = 2; /* Volume label and terminating zero. */
	status = fsimg_fat_layout_set(plan, 0, &row, error);
	if (status != FSIMG_OK)
		return (status);
	for (uint64_t id = 1; id <= plan->writer->count; id++) {
		status = fsimg_catalog_get(plan->writer, id, &catalog, error);
		if (status != FSIMG_OK)
			return (status);
		name = strrchr(catalog->entry.path, '/') + 1;
		status = fsimg_fat_name_validate(name, error);
		if (status != FSIMG_OK)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_WRITER, "invalid FAT target filename"));
		if (catalog->entry.size > UINT32_MAX)
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_WRITER, "FAT file exceeds its 32-bit size field"));
		bzero(&row, sizeof(row));
		row.length = catalog->entry.size;
		if (catalog->entry.kind == FSIMG_ENTRY_DIRECTORY)
			row.slots = 3; /* Dot, dot-dot, terminator. */
		status = fsimg_fat_write_metadata(plan, &catalog->entry, row.entry, error);
		if (status != FSIMG_OK)
			return (status);
		status = fsimg_fat_layout_set(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);

		fsimg_meta_put_u64(key, catalog->parent_id);
		status = fsimg_fat_name_key(name, key + 8, &length, error);
		if (status != FSIMG_OK)
			return (status);
		status = fsimg_stream_append(names, key, length + 8, NULL, error);
		if (status != FSIMG_OK)
			return (status);
		if (alias_reservation(key + 8, length, &number)) {
			fsimg_meta_put_u64(reservation, catalog->parent_id);
			fsimg_meta_put_u64(reservation + 8, number);
			status = fsimg_stream_append(reservations, reservation,
			    sizeof(reservation), NULL, error);
			if (status != FSIMG_OK)
				return (status);
		}
	}
	return (FSIMG_OK);
}

static fsimg_status_t
check_names(fsimg_stream_t *names, fsimg_error_t *error)
{
	unsigned char current[8 + FAT_KEY_BYTES], previous[8 + FAT_KEY_BYTES];
	uint64_t cursor;
	size_t length, previous_length;
	fsimg_status_t status;

	cursor = 0;
	previous_length = 0;
	for (;;) {
		status = fsimg_stream_next(names, &cursor, current, sizeof(current),
		    &length, error);
		if (status == FSIMG_DONE)
			return (FSIMG_OK);
		if (status != FSIMG_OK)
			return (status);
		if (previous_length == length && bcmp(previous, current, length) == 0)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_WRITER, "case-equivalent FAT sibling names"));
		bcopy(current, previous, length);
		previous_length = length;
	}
}

static fsimg_status_t
next_reservation(fat_alias_cursor_t *cursor, fsimg_error_t *error)
{
	unsigned char raw[16];
	size_t length;
	fsimg_status_t status;

	status = fsimg_stream_next(cursor->stream, &cursor->offset, raw,
	    sizeof(raw), &length, error);
	cursor->available = status == FSIMG_OK;
	if (status == FSIMG_DONE)
		return (FSIMG_OK);
	if (status != FSIMG_OK)
		return (status);
	if (length != sizeof(raw) || fsimg_meta_get_u64(raw + 8) > 0x0fffffff)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_WRITER, "invalid FAT alias reservation"));
	cursor->parent_id = fsimg_meta_get_u64(raw);
	cursor->number = fsimg_meta_get_u64(raw + 8);
	return (FSIMG_OK);
}

/* Discard earlier parents and values below the candidate. Equality reserves
 * that number, so advance the candidate and continue. Future reservations stay
 * at the cursor for the next child. Neither cursor nor candidate moves back.
 */
static fsimg_status_t
skip_reserved_aliases(fat_alias_cursor_t *cursor, uint64_t parent_id,
    uint64_t *candidate, fsimg_error_t *error)
{
	fsimg_status_t status;

	while (cursor->available) {
		if (cursor->parent_id > parent_id ||
		    (cursor->parent_id == parent_id && cursor->number > *candidate))
			break;
		if (cursor->parent_id == parent_id && cursor->number == *candidate)
			(*candidate)++;
		status = next_reservation(cursor, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

static fsimg_status_t
assign_aliases(fsimg_plan_t *plan, fsimg_stream_t *reservations,
    fsimg_error_t *error)
{
	fsimg_writer_t *writer;
	fsimg_catalog_child_t child;
	fat_alias_cursor_t reservation = { .stream = reservations };
	fat_layout_t row, parent;
	unsigned char short_name[11];
	char alias[9];
	uint64_t cursor, parent_id, candidate, slots;
	size_t length, units;
	fsimg_status_t status;

	writer = plan->writer;
	cursor = candidate = 0;
	parent_id = UINT64_MAX;
	status = next_reservation(&reservation, error);
	if (status != FSIMG_OK)
		return (status);
	for (;;) {
		status = fsimg_stream_next(writer->children, &cursor, writer->record,
		    writer->record_capacity, &length, error);
		if (status == FSIMG_DONE)
			return (FSIMG_OK);
		if (status != FSIMG_OK)
			return (status);
		status = fsimg_catalog_decode_child(writer, length, &child, error);
		if (status != FSIMG_OK)
			return (status);
		if (parent_id != child.parent_id) {
			parent_id = child.parent_id;
			candidate = 0;
		}
		status = fsimg_fat_layout_get(plan, child.id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		slots = 1;
		if (!fsimg_fat_write_short_name(child.name, short_name)) {
			status = fsimg_utf8_to_utf16(child.name, child.name_length,
			    NULL, 0, &units, error);
			if (status != FSIMG_OK)
				return (status);
			slots += (units + 12) / 13;

			status = skip_reserved_aliases(&reservation, parent_id, &candidate, error);
			if (status != FSIMG_OK)
				return (status);
			if (candidate > 0x0fffffff)
				return (fsimg_error_set(error, FSIMG_ERANGE, 0,
				    FSIMG_OP_WRITER, "FAT alias namespace exhausted"));
			snprintf(alias, sizeof(alias), "F%07X", (unsigned)candidate);
			memset(short_name, ' ', sizeof(short_name));
			bcopy(alias, short_name, 8);
			candidate++;
		}
		bcopy(short_name, row.entry, sizeof(short_name));
		status = fsimg_fat_layout_get(plan, parent_id, &parent, error);
		if (status != FSIMG_OK)
			return (status);
		/* slots includes the terminator; insert immediately before it. */
		row.parent_slot = parent.slots - 1;
		parent.slots += slots;
		if (parent.slots > FAT_WRITE_DIRECTORY_MAX / 32)
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_WRITER, "FAT directory exceeds its record limit"));
		status = fsimg_fat_layout_set(plan, child.id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		status = fsimg_fat_layout_set(plan, parent_id, &parent, error);
		if (status != FSIMG_OK)
			return (status);
	}
}

fsimg_status_t
fsimg_fat_write_names(fsimg_plan_t *plan, fsimg_error_t *error)
{
	fsimg_stream_t *names, *reservations, *sorted;
	fsimg_status_t status;

	names = reservations = sorted = NULL;
	status = fsimg_stream_new(&plan->writer->session, 8 + FAT_KEY_BYTES,
	    &names, error);
	if (status != FSIMG_OK)
		goto done;
	status = fsimg_stream_new(&plan->writer->session, 16, &reservations, error);
	if (status != FSIMG_OK)
		goto done;
	status = collect_names(plan, names, reservations, error);
	if (status != FSIMG_OK)
		goto done;
	status = fsimg_stream_seal(names, error);
	if (status != FSIMG_OK)
		goto done;
	status = fsimg_stream_sort(names, key_compare, NULL, NULL, &sorted, error);
	if (status != FSIMG_OK)
		goto done;
	fsimg_stream_free(names);
	names = sorted;
	sorted = NULL;
	status = check_names(names, error);
	if (status != FSIMG_OK)
		goto done;
	fsimg_stream_free(names);
	names = NULL;
	status = fsimg_stream_seal(reservations, error);
	if (status != FSIMG_OK)
		goto done;
	status = fsimg_stream_sort(reservations, reservation_compare, NULL, NULL,
	    &sorted, error);
	if (status != FSIMG_OK)
		goto done;
	status = assign_aliases(plan, sorted, error);
done:
	fsimg_stream_free(names);
	fsimg_stream_free(reservations);
	fsimg_stream_free(sorted);
	return (status);
}
