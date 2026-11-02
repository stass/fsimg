/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "iso9660.h"

/*
 * ISO 9660's part of directory preparation (specs/design-directories.md and
 * specs/design-iso9660-driver.md#directories-and-names), the only code that
 * walks directory records. A directory is data_length bytes of records at
 * its extent; records never cross a block, and each block's records end at
 * a zero byte, the block's end or the directory's length, whichever comes
 * first (cursor_next). The first two records are the dot records,
 * naming the directory and its parent; every later record, or run of
 * records sharing an identifier through the multi-extent flag, is one
 * entry.
 *
 * Three rules make the hierarchy a tree with no record of visited
 * directories: a directory's parent record names the walker's parent, no
 * child names the root, and each directory child carries an extent key, so
 * namespace validation rejects two records opening one directory.
 *
 * In the rockridge view every record's SUSP stream is decoded, and a run's
 * first record gives its name, kind and metadata. Relocation keeps the
 * same three rules (specs/design-iso9660.md#relocation): a placeholder
 * record carrying CL is a directory entry opening the relocated directory,
 * whose parent record carries PL naming the walker's parent instead; a
 * directory reached through an ordinary record carries no PL; and the
 * relocated directory's own record, carrying RE, is not an entry. Every
 * directory therefore has one way in, and a relocation loop is never
 * reached.
 *
 * The payload of an admitted entry, little-endian, holds the containing
 * directory, the position of the entry's first record, its section count,
 * kind and relocation flag, and the directory a directory entry opens.
 * Replay reads the records again from that position.
 */
#define PAYLOAD_DIRECTORY_EXTENT	0u
#define PAYLOAD_DIRECTORY_LENGTH	4u
#define PAYLOAD_RECORD_BLOCK		8u
#define PAYLOAD_RECORD_OFFSET		12u
#define PAYLOAD_SECTIONS		14u
#define PAYLOAD_KIND			16u
#define PAYLOAD_RELOCATED		17u
#define PAYLOAD_OPENED_EXTENT		18u
#define PAYLOAD_OPENED_LENGTH		22u
#define ISO_PAYLOAD_BYTES		26u

_Static_assert(ISO_PAYLOAD_BYTES <= FSIMG_DIRECTORY_PAYLOAD_BYTES,
    "an ISO 9660 payload fits a prepared record");

/* One entry's records as they are read: the first record and its
 * identifier, the section count, the size so far, and the end block of the
 * last section holding data, which the next one must not precede. open is
 * set while the last record read carried the multi-extent flag, so the run
 * needs another section. kind is the entry's kind in the view, and rr the
 * Rock Ridge entries of the first record, all zero outside the rockridge
 * view.
 */
typedef struct iso_run {
	iso_record_t	 first;
	iso_rr_t	 rr;
	fsimg_entry_kind_t kind;
	uint64_t	 size;
	uint64_t	 data_end;
	uint32_t	 block;			/* the first record's position */
	uint32_t	 offset;
	uint32_t	 sections;
	bool		 has_data;
	bool		 open;
	unsigned char	 identifier[ISO_IDENTIFIER_MAX];
} iso_run_t;

/* Start reading the directory of length bytes at extent: a nonzero
 * length, its blocks within the volume and the readable image.
 */
static fsimg_status_t
cursor_open(const fsimg_iso9660_reader_t *iso, uint32_t extent,
    uint32_t length, iso_cursor_t *cursor, fsimg_error_t *error)
{
	bzero(cursor, sizeof(*cursor));
	if (length == 0)
		return (iso_corrupt(error, "ISO 9660 directory is empty"));
	cursor->extent = extent;
	cursor->length = length;
	cursor->blocks = (uint32_t)ISO_BLOCKS(length);
	return (iso_extent_check(iso, extent, cursor->blocks, error));
}

/* Decode the directory's next record into record, FSIMG_DONE after its
 * last. A block's records end at a zero byte, at the block's end or at the
 * directory's length, whichever comes first, and every byte after them in
 * the block must be zero. *raw points at the record in the reader's window
 * and stays valid until the next read through the window.
 */
static fsimg_status_t
cursor_next(fsimg_reader_t *reader, iso_cursor_t *cursor,
    iso_record_t *record, const unsigned char **raw, fsimg_error_t *error)
{
	fsimg_iso9660_reader_t *iso = iso_state(reader);
	uint64_t end;
	fsimg_status_t status;

	*raw = NULL;
	for (;;) {
		if (cursor->block >= cursor->blocks)
			return (FSIMG_DONE);
		status = iso_block_read(reader,
		    (uint64_t)cursor->extent + cursor->block, error);
		if (status != FSIMG_OK)
			return (status);
		end = cursor->length - (uint64_t)cursor->block * ISO_BLOCK_BYTES;
		if (end > ISO_BLOCK_BYTES)
			end = ISO_BLOCK_BYTES;
		if (cursor->offset < end &&
		    iso->window.bytes[cursor->offset] != 0)
			break;
		if (cursor->offset < ISO_BLOCK_BYTES &&
		    !iso_bytes_zero(iso->window.bytes + cursor->offset,
		    ISO_BLOCK_BYTES - cursor->offset))
			return (iso_corrupt(error, "ISO 9660 directory block "
			    "holds bytes after its last record"));
		cursor->block++;
		cursor->offset = 0;
	}
	status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
	if (status == FSIMG_OK)
		status = iso_record_decode(iso->window.bytes, cursor->offset,
		    record, error);
	if (status != FSIMG_OK)
		return (status);
	if (record->length > end - cursor->offset)
		return (iso_corrupt(error,
		    "ISO 9660 directory record extends past its directory"));
	*raw = iso->window.bytes + cursor->offset;
	cursor->record_block = cursor->extent + cursor->block;
	cursor->record_offset = cursor->offset;
	cursor->offset += record->length;
	return (FSIMG_OK);
}

/* A directory child: nonzero, within the volume, never the root. */
static fsimg_status_t
directory_child_check(const fsimg_iso9660_reader_t *iso,
    const iso_record_t *record, fsimg_error_t *error)
{
	if (record->data_length == 0)
		return (iso_corrupt(error, "ISO 9660 directory is empty"));
	if (!iso_in_volume(iso, record->extent,
	    ISO_BLOCKS(record->data_length)))
		return (iso_corrupt(error,
		    "ISO 9660 directory lies outside the volume"));
	if (record->extent == iso->view_root.extent)
		return (iso_corrupt(error,
		    "ISO 9660 directory record names the root"));
	return (FSIMG_OK);
}

/* Check one file section (specs/design-iso9660.md#file-sections) and add
 * it to the run's size: a section before the last is nonzero whole
 * blocks; a section with data has its extended attribute record and data
 * within the volume and starts at or after the end of the previous one
 * with data, so the sizes sum to no more than the volume. A section of
 * length zero holds no data and has no extent to check.
 */
static fsimg_status_t
run_section_add(const fsimg_iso9660_reader_t *iso, iso_run_t *run,
    const iso_record_t *record, fsimg_error_t *error)
{
	uint64_t start, blocks;

	blocks = ISO_BLOCKS(record->data_length);
	if ((record->flags & ISO_FLAG_MULTI_EXTENT) != 0 &&
	    (record->data_length == 0 ||
	    record->data_length % ISO_BLOCK_BYTES != 0))
		return (iso_corrupt(error, "ISO 9660 file section before the "
		    "last is not whole blocks"));
	if (record->data_length == 0)
		return (FSIMG_OK);
	if (!iso_in_volume(iso, record->extent, record->ea_blocks + blocks))
		return (iso_corrupt(error,
		    "ISO 9660 file section lies outside the volume"));
	start = (uint64_t)record->extent + record->ea_blocks;
	if (run->has_data && start < run->data_end)
		return (iso_unsupported(error,
		    "ISO 9660 file sections descend or overlap"));
	run->has_data = true;
	run->data_end = start + blocks;
	run->size += record->data_length;
	return (FSIMG_OK);
}

/* Add the record at the cursor's last position to run: the first record of
 * a new entry, or the next section of an open run, which repeats the
 * identifier and is a file section. A run that is not open is reset by
 * its next record, so one iso_run_t serves every entry of a directory. On
 * return run->open tells whether another section must follow.
 */
static fsimg_status_t
run_add(const fsimg_iso9660_reader_t *iso, iso_run_t *run,
    const iso_cursor_t *cursor, const iso_record_t *record,
    const unsigned char *raw, fsimg_error_t *error)
{
	const unsigned char *identifier = raw + 33;
	fsimg_status_t status;

	if (!run->open) {
		bzero(run, sizeof(*run));
		run->first = *record;
		run->block = cursor->record_block;
		run->offset = cursor->record_offset;
		bcopy(identifier, run->identifier, record->identifier_length);
	} else if (record->identifier_length !=
	    run->first.identifier_length || bcmp(identifier, run->identifier,
	    record->identifier_length) != 0 ||
	    (record->flags & ISO_FLAG_DIRECTORY) != 0)
		return (iso_corrupt(error,
		    "ISO 9660 file section run ends without its last section"));
	if (run->sections == ISO_SECTIONS_MAX)
		return (iso_unsupported(error,
		    "ISO 9660 file has more than 65535 sections"));
	run->sections++;
	if ((record->flags & ISO_FLAG_DIRECTORY) != 0)
		status = directory_child_check(iso, record, error);
	else
		status = run_section_add(iso, run, record, error);
	run->open = (record->flags & ISO_FLAG_MULTI_EXTENT) != 0;
	return (status);
}

/* The dot records: the first, at the directory's first byte, names the
 * directory itself with its length; the second, in the same block, names
 * parent, the walker's parent, which is the root itself for the root. In
 * the rockridge view both streams are decoded, and the second carries PL
 * exactly when the directory is relocated: its PL then names parent, and
 * its extent the relocation directory, which only full verification
 * checks against the RE record there.
 */
static fsimg_status_t
dots_check(fsimg_reader_t *reader, iso_cursor_t *cursor, uint32_t length,
    uint32_t parent, bool relocated, fsimg_error_t *error)
{
	fsimg_iso9660_reader_t *iso = iso_state(reader);
	const unsigned char *raw;
	iso_record_t record;
	iso_susp_t susp;
	iso_rr_t rr;
	fsimg_status_t status;

	for (unsigned dot = 0; dot < 2; dot++) {
		status = cursor_next(reader, cursor, &record, &raw, error);
		if (status == FSIMG_DONE)
			return (iso_corrupt(error,
			    "ISO 9660 directory lacks its dot records"));
		if (status != FSIMG_OK)
			return (status);
		if (cursor->record_block != cursor->extent ||
		    (dot == 0 && cursor->record_offset != 0) ||
		    record.identifier_length != 1 || raw[33] != dot ||
		    (record.flags & ISO_FLAG_DIRECTORY) == 0)
			return (iso_corrupt(error,
			    "ISO 9660 directory does not begin with its dot records"));
		if (dot == 0 && (record.extent != cursor->extent ||
		    record.data_length != length))
			return (iso_corrupt(error, "ISO 9660 directory's own "
			    "record does not name it"));
		bzero(&rr, sizeof(rr));
		if (iso->view == FSIMG_ISO_VIEW_ROCKRIDGE) {
			iso_susp_start(&susp, raw, cursor->record_block,
			    cursor->record_offset, &record);
			status = iso_rr_decode(reader, &susp, true, &rr, error);
			if (status != FSIMG_OK)
				return (status);
		}
		if (dot == 0)
			continue;
		if (relocated && ((rr.entries & RR_PL) == 0 ||
		    rr.parent != parent))
			return (iso_corrupt(error, "Rock Ridge relocated "
			    "directory's PL does not name its parent"));
		if (!relocated && (rr.entries & RR_PL) != 0)
			return (iso_corrupt(error, "Rock Ridge PL on a "
			    "directory not reached through CL"));
		if (!relocated && record.extent != parent)
			return (iso_corrupt(error, "ISO 9660 directory's parent "
			    "record does not name its parent"));
	}
	return (FSIMG_OK);
}

/* Add the record at the cursor's last position to run, then in the
 * rockridge view decode its stream: a run's first record gives the
 * entry's name, kind and metadata, a later section's stream is framed and
 * not interpreted. Elsewhere the directory flag gives the kind.
 */
static fsimg_status_t
run_record(fsimg_reader_t *reader, iso_run_t *run, const iso_cursor_t *cursor,
    const iso_record_t *record, const unsigned char *raw,
    fsimg_error_t *error)
{
	fsimg_iso9660_reader_t *iso = iso_state(reader);
	iso_susp_t susp;
	fsimg_status_t status;

	status = run_add(iso, run, cursor, record, raw, error);
	if (status != FSIMG_OK)
		return (status);
	if (run->sections == 1)
		run->kind = (record->flags & ISO_FLAG_DIRECTORY) != 0 ?
		    FSIMG_ENTRY_DIRECTORY : FSIMG_ENTRY_FILE;
	if (iso->view != FSIMG_ISO_VIEW_ROCKRIDGE)
		return (FSIMG_OK);
	iso_susp_start(&susp, raw, cursor->record_block, cursor->record_offset,
	    record);
	if (run->sections > 1)
		return (iso_susp_frame(reader, &susp, error));
	status = iso_rr_decode(reader, &susp, false, &run->rr, error);
	if (status == FSIMG_OK)
		status = iso_rr_kind(&run->rr, record, &run->kind, error);
	return (status);
}

/* The directory a placeholder in directory relocates, child, which its CL
 * names: not the root, and a directory whose dot records say it is child
 * and carry PL naming directory. Its length is its own record's.
 */
static fsimg_status_t
relocation_target(fsimg_reader_t *reader, uint32_t directory, uint32_t child,
    uint32_t *length, fsimg_error_t *error)
{
	fsimg_iso9660_reader_t *iso = iso_state(reader);
	iso_cursor_t cursor;
	iso_record_t self;
	fsimg_status_t status;

	*length = 0;
	if (child == iso->view_root.extent)
		return (iso_corrupt(error,
		    "Rock Ridge CL entry names the root"));
	status = iso_extent_check(iso, child, 1, error);
	if (status == FSIMG_OK)
		status = iso_block_read(reader, child, error);
	if (status == FSIMG_OK)
		status = iso_record_decode(iso->window.bytes, 0, &self, error);
	if (status == FSIMG_OK)
		status = cursor_open(iso, child, self.data_length, &cursor,
		    error);
	if (status == FSIMG_OK)
		status = dots_check(reader, &cursor, self.data_length,
		    directory, true, error);
	if (status == FSIMG_OK)
		*length = self.data_length;
	return (status);
}

static void
payload_encode(const iso_payload_t *payload, unsigned char *raw)
{
	bzero(raw, ISO_PAYLOAD_BYTES);
	fsimg_store_le32(raw + PAYLOAD_DIRECTORY_EXTENT,
	    payload->directory_extent);
	fsimg_store_le32(raw + PAYLOAD_DIRECTORY_LENGTH,
	    payload->directory_length);
	fsimg_store_le32(raw + PAYLOAD_RECORD_BLOCK, payload->record_block);
	fsimg_store_le16(raw + PAYLOAD_RECORD_OFFSET, payload->record_offset);
	fsimg_store_le16(raw + PAYLOAD_SECTIONS, payload->sections);
	raw[PAYLOAD_KIND] = (unsigned char)payload->kind;
	raw[PAYLOAD_RELOCATED] = payload->relocated ? 1 : 0;
	fsimg_store_le32(raw + PAYLOAD_OPENED_EXTENT, payload->opened_extent);
	fsimg_store_le32(raw + PAYLOAD_OPENED_LENGTH, payload->opened_length);
}

/* Decode a payload read back from scratch storage. It is checked enough
 * that reading the records it locates stays within its directory; the
 * records themselves are decoded and compared again by replay.
 */
fsimg_status_t
iso_payload_decode(const fsimg_iso9660_reader_t *iso,
    const unsigned char *raw, size_t length, iso_payload_t *out,
    fsimg_error_t *error)
{
	bzero(out, sizeof(*out));
	if (length != ISO_PAYLOAD_BYTES)
		return (iso_corrupt(error, "invalid prepared ISO 9660 payload"));
	out->directory_extent = fsimg_load_le32(raw + PAYLOAD_DIRECTORY_EXTENT);
	out->directory_length = fsimg_load_le32(raw + PAYLOAD_DIRECTORY_LENGTH);
	out->record_block = fsimg_load_le32(raw + PAYLOAD_RECORD_BLOCK);
	out->record_offset = fsimg_load_le16(raw + PAYLOAD_RECORD_OFFSET);
	out->sections = fsimg_load_le16(raw + PAYLOAD_SECTIONS);
	out->kind = (fsimg_entry_kind_t)raw[PAYLOAD_KIND];
	out->relocated = raw[PAYLOAD_RELOCATED] != 0;
	out->opened_extent = fsimg_load_le32(raw + PAYLOAD_OPENED_EXTENT);
	out->opened_length = fsimg_load_le32(raw + PAYLOAD_OPENED_LENGTH);
	if (out->record_block < out->directory_extent ||
	    (uint64_t)out->record_block - out->directory_extent >=
	    ISO_BLOCKS(out->directory_length) ||
	    out->record_offset >= ISO_BLOCK_BYTES || out->sections == 0 ||
	    raw[PAYLOAD_RELOCATED] > 1 ||
	    (out->relocated && iso->view != FSIMG_ISO_VIEW_ROCKRIDGE))
		return (iso_corrupt(error, "invalid prepared ISO 9660 payload"));
	switch (out->kind) {
	case FSIMG_ENTRY_SYMLINK:
	case FSIMG_ENTRY_FIFO:
		if (iso->view != FSIMG_ISO_VIEW_ROCKRIDGE)
			break;
		/* FALLTHROUGH */
	case FSIMG_ENTRY_FILE:
		if (out->opened_extent != 0 || out->opened_length != 0 ||
		    out->relocated)
			break;
		return (FSIMG_OK);
	case FSIMG_ENTRY_DIRECTORY:
		if (out->sections != 1)
			break;
		return (FSIMG_OK);
	default:
		break;
	}
	return (iso_corrupt(error, "invalid prepared ISO 9660 payload"));
}

/* The displayed name of a complete run in the view, the one derivation
 * preparation admits and replay compares, into out of ISO_NAME_MAX + 1
 * bytes: the Rock Ridge name when the run's first record has NM,
 * otherwise the primary displayed name.
 */
static fsimg_status_t
run_name(const iso_run_t *run, char *out, size_t *length,
    fsimg_error_t *error)
{
	if ((run->rr.entries & RR_NM) != 0) {
		bcopy(run->rr.name, out, run->rr.name_length + 1);
		*length = run->rr.name_length;
		return (FSIMG_OK);
	}
	return (iso_primary_name(run->identifier, run->first.identifier_length,
	    (run->first.flags & ISO_FLAG_DIRECTORY) != 0, out, length, error));
}

/* Admit a complete run under its displayed name. A Rock Ridge record
 * carrying RE is not an entry: its directory is reached through its
 * placeholder. A placeholder opens the directory its CL names.
 */
static fsimg_status_t
run_admit(fsimg_reader_t *reader, fsimg_directory_build_t *build,
    const iso_cursor_t *directory, const iso_run_t *run, fsimg_error_t *error)
{
	unsigned char raw[ISO_PAYLOAD_BYTES];
	char name[ISO_NAME_MAX + 1];
	iso_payload_t payload;
	size_t length;
	fsimg_status_t status;

	if ((run->rr.entries & RR_RE) != 0)
		return (FSIMG_OK);
	bzero(&payload, sizeof(payload));
	payload.kind = run->kind;
	status = run_name(run, name, &length, error);
	if (status != FSIMG_OK)
		return (status);
	payload.directory_extent = directory->extent;
	payload.directory_length = directory->length;
	payload.record_block = run->block;
	payload.record_offset = (uint16_t)run->offset;
	payload.sections = (uint16_t)run->sections;
	if ((run->rr.entries & RR_CL) != 0) {
		if (run->sections != 1)
			return (iso_corrupt(error, "Rock Ridge relocation "
			    "placeholder has several sections"));
		payload.relocated = true;
		payload.opened_extent = run->rr.child;
		status = relocation_target(reader, directory->extent,
		    run->rr.child, &payload.opened_length, error);
		if (status != FSIMG_OK)
			return (status);
	} else if (payload.kind == FSIMG_ENTRY_DIRECTORY) {
		payload.opened_extent = run->first.extent;
		payload.opened_length = run->first.data_length;
	}
	payload_encode(&payload, raw);
	return (fsimg_directory_admit(build, name, raw, sizeof(raw), error));
}

/* Prepare the view's root at level 0, otherwise the directory replay made
 * current: check its dot records, then group every later record into runs
 * and admit each complete one. Preparation reads the directory's blocks
 * once and never its subtree.
 */
fsimg_status_t
iso_prepare(fsimg_reader_t *reader, fsimg_directory_build_t *build,
    fsimg_error_t *error)
{
	fsimg_iso9660_reader_t *iso = iso_state(reader);
	const unsigned char *raw;
	iso_cursor_t cursor;
	iso_record_t record;
	iso_run_t run;
	uint32_t extent, length, parent;
	bool relocated;
	fsimg_status_t status;

	relocated = false;
	if (fsimg_directory_level(build) == 0) {
		extent = iso->view_root.extent;
		length = iso->view_root.data_length;
		parent = extent;
	} else {
		if (iso->current.kind != FSIMG_ENTRY_DIRECTORY)
			return (iso_corrupt(error,
			    "ISO 9660 entry to prepare is not a directory"));
		extent = iso->current.opened_extent;
		length = iso->current.opened_length;
		parent = iso->current.directory_extent;
		relocated = iso->current.relocated;
	}
	status = cursor_open(iso, extent, length, &cursor, error);
	if (status == FSIMG_OK)
		status = dots_check(reader, &cursor, length, parent, relocated,
		    error);
	if (status != FSIMG_OK)
		return (status);

	bzero(&run, sizeof(run));
	for (;;) {
		status = cursor_next(reader, &cursor, &record, &raw, error);
		if (status == FSIMG_DONE)
			break;
		if (status == FSIMG_OK)
			status = run_record(reader, &run, &cursor, &record, raw,
			    error);
		if (status == FSIMG_OK && !run.open)
			status = run_admit(reader, build, &cursor, &run, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (run.open)
		return (iso_corrupt(error, "ISO 9660 file section run continues "
		    "past the end of its directory"));
	return (FSIMG_OK);
}

/* Append a file section with data to the current file's index: its data,
 * past the extended attribute record, at logical offset, rounded up to
 * whole blocks as the index requires. Only the final section can gain
 * padding, and delivery clips it to the file size.
 */
static fsimg_status_t
section_index(fsimg_reader_t *reader, const iso_record_t *record,
    uint64_t logical, fsimg_error_t *error)
{
	fsimg_iso9660_reader_t *iso = iso_state(reader);
	fsimg_extent_t extent;
	fsimg_status_t status;

	if (record->data_length == 0)
		return (FSIMG_OK);
	if (iso->file == NULL) {
		status = fsimg_extent_index_new(&reader->session,
		    &reader->content_domain, &iso->file, error);
		if (status != FSIMG_OK)
			return (status);
	}
	extent = (fsimg_extent_t){
		.logical_offset = logical,
		.image_offset = ((uint64_t)record->extent + record->ea_blocks) *
		    ISO_BLOCK_BYTES,
		.length = ISO_BLOCKS(record->data_length) * ISO_BLOCK_BYTES
	};
	return (fsimg_extent_index_append(iso->file, &extent, error));
}

/* Read the run a payload locates again, checking and decoding every
 * record as preparation did, and index a regular file's sections.
 */
static fsimg_status_t
run_reread(fsimg_reader_t *reader, const iso_payload_t *payload,
    iso_run_t *run, fsimg_error_t *error)
{
	const unsigned char *raw;
	iso_cursor_t cursor;
	iso_record_t record;
	uint64_t logical;
	fsimg_status_t status;

	status = cursor_open(iso_state(reader), payload->directory_extent,
	    payload->directory_length, &cursor, error);
	if (status != FSIMG_OK)
		return (status);
	cursor.block = payload->record_block - payload->directory_extent;
	cursor.offset = payload->record_offset;
	bzero(run, sizeof(*run));
	do {
		status = cursor_next(reader, &cursor, &record, &raw, error);
		if (status == FSIMG_DONE)
			return (iso_corrupt(error, "ISO 9660 file section run "
			    "continues past the end of its directory"));
		if (status == FSIMG_OK && run->sections == 0 &&
		    (cursor.record_block != payload->record_block ||
		    cursor.record_offset != payload->record_offset))
			return (iso_corrupt(error,
			    "prepared ISO 9660 record is not at its position"));
		/* The section's logical offset is the size before it. */
		logical = run->size;
		if (status == FSIMG_OK)
			status = run_record(reader, run, &cursor, &record, raw,
			    error);
		if (status == FSIMG_OK && run->kind == FSIMG_ENTRY_FILE)
			status = section_index(reader, &record, logical, error);
		if (status != FSIMG_OK)
			return (status);
	} while (run->open);
	return (FSIMG_OK);
}

/* Make a replayed child current. Its payload came from scratch storage, so
 * the run is read again from the directory and must agree with it: the
 * same kind, section count, displayed name and relocation, and for a
 * directory the directory it opens. That the payload's directory is the
 * one being listed is taken from the walker's private scratch, which
 * preparation wrote; whatever it names, every read stays within the
 * volume. A regular file's sections become the content index, and a
 * symbolic link's decoded target its content; a directory or FIFO has no
 * content.
 */
fsimg_status_t
iso_replay(fsimg_reader_t *reader, const char *name,
    const unsigned char *raw, size_t length, fsimg_error_t *error)
{
	fsimg_iso9660_reader_t *iso = iso_state(reader);
	char shown[ISO_NAME_MAX + 1];
	iso_payload_t payload;
	iso_run_t run;
	fsimg_metadata_t *metadata = &reader->entry.metadata;
	size_t shown_length;
	bool relocated, opens;
	fsimg_status_t status;

	fsimg_extent_index_free(iso->file);
	iso->file = NULL;
	bzero(&iso->current, sizeof(iso->current));
	status = iso_payload_decode(iso, raw, length, &payload, error);
	if (status == FSIMG_OK)
		status = run_reread(reader, &payload, &run, error);
	if (status != FSIMG_OK)
		return (status);
	relocated = (run.rr.entries & RR_CL) != 0;
	if (relocated)
		opens = run.rr.child == payload.opened_extent;
	else
		opens = run.first.extent == payload.opened_extent &&
		    run.first.data_length == payload.opened_length;
	if (run.kind != payload.kind || run.sections != payload.sections ||
	    relocated != payload.relocated ||
	    (run.rr.entries & RR_RE) != 0 ||
	    (run.kind == FSIMG_ENTRY_DIRECTORY && !opens))
		return (iso_corrupt(error,
		    "prepared ISO 9660 entry disagrees with its records"));
	status = run_name(&run, shown, &shown_length, error);
	if (status != FSIMG_OK)
		return (status);
	if (strcmp(shown, name) != 0)
		return (iso_corrupt(error,
		    "prepared ISO 9660 name disagrees with its record"));

	iso->current = payload;
	reader->entry.kind = payload.kind;
	reader->entry.size = 0;
	*metadata = run.rr.metadata;
	metadata->valid |= FSIMG_META_ATTRIBUTES;
	if ((run.first.flags & ISO_FLAG_HIDDEN) != 0)
		metadata->attributes = FSIMG_ATTR_HIDDEN;
	if ((metadata->valid & FSIMG_META_MODIFIED) == 0 &&
	    run.first.date_present) {
		metadata->valid |= FSIMG_META_MODIFIED;
		metadata->modified = run.first.date;
	}
	switch (payload.kind) {
	case FSIMG_ENTRY_FILE:
		reader->entry.size = run.size;
		reader->initialized = run.size;
		return (FSIMG_OK);
	case FSIMG_ENTRY_SYMLINK:
		return (fsimg_reader_set_link_target(reader, run.rr.target,
		    run.rr.target_length, error));
	default:
		return (FSIMG_OK);
	}
}
