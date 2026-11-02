/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "iso9660.h"

/*
 * The System Use Sharing Protocol (specs/design-iso9660.md, "System Use
 * Sharing Protocol", and specs/design-iso9660-driver.md#the-susp-cursor).
 * A record's entry stream starts in its system use area and continues
 * through at most 16 continuation areas, each named by the one CE entry of
 * the area before it. The cursor returns the entries in order and
 * consumes CE, PD and ST itself. Three rules bound a stream whatever the
 * image holds: an area lies within one block, a stream enters at most 16
 * continuation areas, and no area overlaps one the stream already
 * entered, so a continuation cycle fails instead of looping. A stream therefore decodes at most its
 * system use area and 16 blocks.
 *
 * Continuation blocks are read through the reader's continuation window,
 * which leaves the directory window, and the system use area in it, intact.
 */
#define ENTRY_HEADER	4u	/* signature, length, version */
#define CE_BYTES	28u
#define SP_BYTES	7u
#define ER_HEADER	8u

/* The extension identifiers that announce Rock Ridge, under every version
 * the profile reads.
 */
static const char *const rockridge_identifiers[] = {
	"RRIP_1991A", "IEEE_P1282", "IEEE_1282"
};

static bool
signature_is(const unsigned char *bytes, const char *signature)
{
	return (bytes[0] == (unsigned char)signature[0] &&
	    bytes[1] == (unsigned char)signature[1]);
}

/* Start the stream of the record decoded from raw, which lies at offset of
 * volume block number block: its system use area is the first area.
 */
void
iso_susp_start(iso_susp_t *susp, const unsigned char *raw, uint32_t block,
    uint32_t offset, const iso_record_t *record)
{
	bzero(susp, sizeof(*susp));
	susp->bytes = raw + record->system_use_offset;
	susp->length = record->system_use_length;
	susp->areas[0] = (iso_area_t){
		.block = block,
		.offset = (uint16_t)(offset + record->system_use_offset),
		.length = record->system_use_length
	};
	susp->count = 1;
}

static bool
areas_overlap(const iso_area_t *a, const iso_area_t *b)
{
	return (a->block == b->block &&
	    (uint32_t)a->offset < (uint32_t)b->offset + b->length &&
	    (uint32_t)b->offset < (uint32_t)a->offset + a->length);
}

/* Record the continuation area a CE entry names, entered when the current
 * area ends. The corruption checks come first: the area is not empty,
 * starts in a block of the readable volume, and overlaps no area entered,
 * which includes the current one, so a CE naming its own area fails. Then
 * the profile's: the area ends within its block, and the stream has room
 * for another area.
 */
static fsimg_status_t
continuation_note(fsimg_reader_t *reader, iso_susp_t *susp,
    const unsigned char *entry, fsimg_error_t *error)
{
	iso_area_t area;
	uint32_t block, offset, length;
	fsimg_status_t status;

	if (entry[2] != CE_BYTES || entry[3] != 1)
		return (iso_corrupt(error, "malformed SUSP CE entry"));
	if (susp->has_next)
		return (iso_corrupt(error,
		    "SUSP area holds more than one CE entry"));
	status = iso_u32both(entry + 4, &block, error);
	if (status == FSIMG_OK)
		status = iso_u32both(entry + 12, &offset, error);
	if (status == FSIMG_OK)
		status = iso_u32both(entry + 20, &length, error);
	if (status != FSIMG_OK)
		return (status);
	if (length == 0 || offset >= ISO_BLOCK_BYTES)
		return (iso_corrupt(error,
		    "SUSP continuation area is empty or outside its block"));
	status = iso_extent_check(iso_state(reader), block, 1, error);
	if (status != FSIMG_OK)
		return (status);
	/* Within the block, which is all a crossing area could reach. */
	area = (iso_area_t){
		.block = block,
		.offset = (uint16_t)offset,
		.length = (uint16_t)(length < ISO_BLOCK_BYTES - offset ?
		    length : ISO_BLOCK_BYTES - offset)
	};
	for (unsigned i = 0; i < susp->count; i++) {
		if (areas_overlap(&area, &susp->areas[i]))
			return (iso_corrupt(error,
			    "SUSP continuation area overlaps an earlier area"));
	}
	if (length > ISO_BLOCK_BYTES - offset)
		return (iso_unsupported(error,
		    "SUSP continuation area crosses a block"));
	if (susp->count > ISO_SUSP_AREAS_MAX)
		return (iso_unsupported(error,
		    "SUSP stream has more than 16 continuation areas"));
	susp->next = area;
	susp->has_next = true;
	return (FSIMG_OK);
}

/* Enter the continuation area the current area's CE named, charging one
 * work unit for it.
 */
static fsimg_status_t
continuation_enter(fsimg_reader_t *reader, iso_susp_t *susp,
    fsimg_error_t *error)
{
	fsimg_iso9660_reader_t *iso = iso_state(reader);
	fsimg_status_t status;

	status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
	if (status == FSIMG_OK)
		status = iso_window_read(reader, &iso->continuation,
		    susp->next.block, error);
	if (status != FSIMG_OK)
		return (status);
	susp->areas[susp->count++] = susp->next;
	susp->bytes = iso->continuation.bytes + susp->next.offset;
	susp->length = susp->next.length;
	susp->position = 0;
	susp->has_next = false;
	return (FSIMG_OK);
}

/* The stream's next entry into entry, FSIMG_DONE after its last. An area
 * ends at a zero byte where a signature would start or with fewer than
 * four bytes left; an entry must end within its area. CE, PD and ST are
 * consumed here, every other entry is returned whatever its signature.
 * entry->data points into a window and stays valid until the next call.
 */
fsimg_status_t
iso_susp_next(fsimg_reader_t *reader, iso_susp_t *susp, iso_entry_t *entry,
    fsimg_error_t *error)
{
	const unsigned char *raw;
	size_t left;
	fsimg_status_t status;

	bzero(entry, sizeof(*entry));
	while (!susp->ended) {
		left = susp->length - susp->position;
		raw = susp->bytes + susp->position;
		if (left < ENTRY_HEADER || raw[0] == 0) {
			if (!susp->has_next) {
				susp->ended = true;
				break;
			}
			status = continuation_enter(reader, susp, error);
			if (status != FSIMG_OK)
				return (status);
			continue;
		}
		if (raw[2] < ENTRY_HEADER || raw[2] > left)
			return (iso_corrupt(error,
			    "SUSP entry extends past its area"));
		susp->position += raw[2];
		if (signature_is(raw, "CE")) {
			status = continuation_note(reader, susp, raw, error);
			if (status != FSIMG_OK)
				return (status);
		} else if (signature_is(raw, "ST")) {
			susp->ended = true;
		} else if (!signature_is(raw, "PD")) {
			entry->signature[0] = (char)raw[0];
			entry->signature[1] = (char)raw[1];
			entry->version = raw[3];
			entry->data = raw + ENTRY_HEADER;
			entry->length = raw[2] - ENTRY_HEADER;
			return (FSIMG_OK);
		}
	}
	return (FSIMG_DONE);
}

/* Walk a stream for its framing only, as for a file's later sections,
 * whose entries are not interpreted.
 */
fsimg_status_t
iso_susp_frame(fsimg_reader_t *reader, iso_susp_t *susp, fsimg_error_t *error)
{
	iso_entry_t entry;
	fsimg_status_t status;

	do
		status = iso_susp_next(reader, susp, &entry, error);
	while (status == FSIMG_OK);
	return (status == FSIMG_DONE ? FSIMG_OK : status);
}

/* Whether an ER entry announces Rock Ridge: its identifier, description
 * and source lie within it, the identifier is in the profile's table and
 * the extension version is 1. A malformed ER is corrupt.
 */
static fsimg_status_t
extension_check(const iso_entry_t *entry, bool *rockridge,
    fsimg_error_t *error)
{
	size_t identifier;

	*rockridge = false;
	if (entry->version != 1 || entry->length < ER_HEADER - ENTRY_HEADER ||
	    (size_t)entry->data[0] + entry->data[1] + entry->data[2] >
	    entry->length - (ER_HEADER - ENTRY_HEADER))
		return (iso_corrupt(error, "malformed SUSP ER entry"));
	identifier = entry->data[0];
	if (entry->data[3] != 1)
		return (FSIMG_OK);
	for (size_t i = 0; i < sizeof(rockridge_identifiers) /
	    sizeof(rockridge_identifiers[0]); i++) {
		if (strlen(rockridge_identifiers[i]) == identifier &&
		    bcmp(rockridge_identifiers[i], entry->data + 4,
		    identifier) == 0)
			*rockridge = true;
	}
	return (FSIMG_OK);
}

/* The handshake (specs/design-iso9660.md#the-handshake): SUSP is in use
 * when the primary root's first record's system use area begins with a
 * well-formed SP entry, otherwise the areas are opaque and Rock Ridge is
 * absent. Rock Ridge is present when the root record's stream, its
 * continuation areas included, holds an ER entry the profile lists. A
 * nonzero SP skip count makes a present Rock Ridge unreadable. Once SP is
 * well formed the whole stream must frame, so a damaged one is corrupt.
 * The root's dot records themselves are checked when the root is prepared.
 */
fsimg_status_t
iso_handshake(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_iso9660_reader_t *iso = iso_state(reader);
	const unsigned char *area;
	iso_record_t record;
	iso_entry_t entry;
	iso_susp_t susp;
	bool found, listed;
	uint8_t skip;
	fsimg_status_t status;

	iso->rockridge = ISO_ROCKRIDGE_ABSENT;
	status = iso_block_read(reader, iso->primary.root.extent, error);
	if (status == FSIMG_OK)
		status = iso_record_decode(iso->window.bytes, 0, &record,
		    error);
	if (status != FSIMG_OK)
		return (status);
	area = iso->window.bytes + record.system_use_offset;
	if (record.system_use_length < SP_BYTES || !signature_is(area, "SP") ||
	    area[2] != SP_BYTES || area[3] != 1 || area[4] != 0xbe ||
	    area[5] != 0xef)
		return (FSIMG_OK);
	skip = area[6];

	found = false;
	iso_susp_start(&susp, iso->window.bytes, iso->primary.root.extent, 0,
	    &record);
	while ((status = iso_susp_next(reader, &susp, &entry, error)) ==
	    FSIMG_OK) {
		if (entry.signature[0] != 'E' || entry.signature[1] != 'R')
			continue;
		status = extension_check(&entry, &listed, error);
		if (status != FSIMG_OK)
			return (status);
		found = found || listed;
	}
	if (status != FSIMG_DONE)
		return (status);
	if (found)
		iso->rockridge = skip == 0 ? ISO_ROCKRIDGE_PRESENT :
		    ISO_ROCKRIDGE_SKIP;
	return (FSIMG_OK);
}
