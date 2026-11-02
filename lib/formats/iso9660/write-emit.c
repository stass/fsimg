/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "write-iso9660.h"

/*
 * Emission. The common emission object has already zeroed the sink, so
 * the system area, every unused byte of a directory block, a path-table
 * block or a continuation block, and every byte after a file's content
 * in its last block are zero already. The driver writes, in order:
 *   1. the primary descriptor, the Joliet descriptor and the terminator;
 *   2. each hierarchy's type-L and type-M path tables;
 *   3. each hierarchy's directories in path-table order, and with each
 *	primary record its continuation fragments;
 *   4. each file's payload.
 * Every byte comes from a row, a stream, the layout or a profile
 * constant, and records are packed by the same rule planning sized them
 * with, so a directory that does not end where planning said it would is
 * a corrupt plan.
 */

/* Bytes written sequentially from a volume offset through a buffer of
 * capacity bytes, flushed whenever it fills.
 */
typedef struct sequence {
	fsimg_emit_t	*emit;
	unsigned char	*buffer;
	uint64_t	 offset;
	size_t		 capacity;
	size_t		 used;
} sequence_t;

/* One directory being written: its first block, the planned length, the
 * block being filled in plan->block, and the packing position.
 */
typedef struct directory_out {
	iso_plan_t	*plan;
	fsimg_emit_t	*emit;
	uint64_t	 position;
	uint64_t	 current;	/* the block of the directory in block */
	uint32_t	 first_block;
	uint32_t	 bytes;
} directory_out_t;

static fsimg_status_t sequence_flush(sequence_t *, fsimg_error_t *);
static fsimg_status_t sequence_put(sequence_t *, const unsigned char *,
    size_t, fsimg_error_t *);
static fsimg_status_t emit_descriptors(iso_plan_t *, fsimg_emit_t *,
    fsimg_error_t *);
static fsimg_status_t emit_path_tables(iso_plan_t *, iso_hierarchy_t,
    fsimg_emit_t *, fsimg_error_t *);
static fsimg_status_t flush_block(directory_out_t *, fsimg_error_t *);
static fsimg_status_t put_record(directory_out_t *,
    const iso_record_fields_t *, fsimg_error_t *);
static fsimg_status_t split_entries(directory_out_t *, size_t,
    const iso_row_t *, size_t, unsigned char *, size_t *, fsimg_error_t *);
static fsimg_status_t emit_dot(directory_out_t *, iso_hierarchy_t,
    const iso_row_t *, const iso_row_t *, bool, fsimg_error_t *);
static fsimg_status_t emit_child(directory_out_t *, iso_hierarchy_t,
    const iso_name_t *, fsimg_error_t *);
static fsimg_status_t emit_directory(iso_plan_t *, iso_hierarchy_t,
    fsimg_emit_t *, uint64_t, fsimg_error_t *);
static fsimg_status_t emit_directories(iso_plan_t *, iso_hierarchy_t,
    fsimg_emit_t *, fsimg_error_t *);
static fsimg_status_t emit_files(iso_plan_t *, fsimg_emit_t *,
    fsimg_error_t *);

static fsimg_status_t
sequence_flush(sequence_t *sequence, fsimg_error_t *error)
{
	fsimg_status_t status;

	if (sequence->used == 0)
		return (FSIMG_OK);
	status = fsimg_emit_write(sequence->emit, sequence->offset,
	    sequence->buffer, sequence->used, error);
	sequence->offset += sequence->used;
	sequence->used = 0;
	return (status);
}

static fsimg_status_t
sequence_put(sequence_t *sequence, const unsigned char *bytes, size_t length,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	if (sequence->capacity - sequence->used < length) {
		status = sequence_flush(sequence, error);
		if (status != FSIMG_OK)
			return (status);
	}
	bcopy(bytes, sequence->buffer + sequence->used, length);
	sequence->used += length;
	return (FSIMG_OK);
}

/* Step 1: blocks 16 to 18. Both descriptors carry the same geometry and
 * dates; each names its own hierarchy's root and path tables.
 */
static fsimg_status_t
emit_descriptors(iso_plan_t *plan, fsimg_emit_t *emit, fsimg_error_t *error)
{
	unsigned char *raw = fsimg_emit_buffer(emit);
	iso_volume_fields_t fields;
	iso_row_t root;
	fsimg_status_t status;

	status = iso_row_get(plan, 0, &root, error);
	if (status != FSIMG_OK)
		return (status);
	for (unsigned h = 0; h < ISO_HIERARCHIES; h++) {
		bzero(&fields, sizeof(fields));
		fields.joliet = h == ISO_JOLIET;
		fields.volume_space_size = plan->volume_blocks;
		fields.path_table_bytes = plan->path_bytes[h];
		fields.path_table_l = plan->path_l[h];
		fields.path_table_m = plan->path_m[h];
		fields.root_extent = root.block[h];
		fields.root_length = root.bytes[h];
		bcopy(plan->label[h], fields.label, ISO_LABEL_BYTES);
		bcopy(root.modified, fields.root_date, ISO_DATE7_BYTES);
		bcopy(plan->volume_date, fields.date, ISO_DATE17_BYTES);
		iso_volume_descriptor_encode(&fields,
		    raw + h * ISO_BLOCK_BYTES);
	}
	iso_terminator_encode(raw + 2 * ISO_BLOCK_BYTES);
	return (fsimg_emit_write(emit,
	    (uint64_t)ISO_SYSTEM_BLOCKS * ISO_BLOCK_BYTES, raw,
	    3 * ISO_BLOCK_BYTES, error));
}

/* Step 2: a hierarchy's two path tables, written together from its path
 * stream, each record with its directory's extent and its parent's
 * number, through the two halves of the emission buffer.
 */
static fsimg_status_t
emit_path_tables(iso_plan_t *plan, iso_hierarchy_t hierarchy,
    fsimg_emit_t *emit, fsimg_error_t *error)
{
	unsigned char *buffer = fsimg_emit_buffer(emit);
	unsigned char path[ISO_PATH_STREAM_MAX];
	unsigned char encoded[ISO_PATH_HEADER_BYTES + ISO_JOLIET_IDENTIFIER_MAX];
	iso_path_record_t record;
	sequence_t tables[2];
	iso_row_t row;
	uint64_t cursor, written;
	size_t length, encoded_length;
	fsimg_status_t status;

	for (unsigned t = 0; t < 2; t++) {
		tables[t] = (sequence_t){ .emit = emit,
		    .buffer = buffer + t * (FSIMG_EMIT_BUFFER_BYTES / 2),
		    .capacity = FSIMG_EMIT_BUFFER_BYTES / 2,
		    .offset = (uint64_t)(t == 0 ? plan->path_l[hierarchy] :
		    plan->path_m[hierarchy]) * ISO_BLOCK_BYTES };
	}
	cursor = 0;
	written = 0;
	for (;;) {
		status = fsimg_stream_next(plan->paths[hierarchy], &cursor,
		    path, sizeof(path), &length, error);
		if (status == FSIMG_DONE)
			break;
		if (status != FSIMG_OK)
			return (status);
		if (length <= ISO_PATH_STREAM_HEADER)
			return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
			    FSIMG_OP_WRITER, "invalid ISO 9660 path record"));
		status = iso_row_get(plan, fsimg_load_le64(path), &row, error);
		if (status != FSIMG_OK)
			return (status);
		bzero(&record, sizeof(record));
		record.extent = row.block[hierarchy];
		record.parent = fsimg_load_le16(path + 8);
		record.length = (uint8_t)(length - ISO_PATH_STREAM_HEADER);
		bcopy(path + ISO_PATH_STREAM_HEADER, record.identifier,
		    record.length);
		for (unsigned t = 0; t < 2; t++) {
			encoded_length = iso_path_record_encode(&record, t == 1,
			    encoded);
			status = sequence_put(&tables[t], encoded,
			    encoded_length, error);
			if (status != FSIMG_OK)
				return (status);
		}
		written += encoded_length;
	}
	if (written != plan->path_bytes[hierarchy])
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_WRITER, "ISO 9660 path table disagrees with its plan"));
	status = sequence_flush(&tables[0], error);
	if (status == FSIMG_OK)
		status = sequence_flush(&tables[1], error);
	return (status);
}

/* Write the directory block being filled. */
static fsimg_status_t
flush_block(directory_out_t *out, fsimg_error_t *error)
{
	if ((out->current + 1) * ISO_BLOCK_BYTES > out->bytes)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_WRITER, "ISO 9660 directory outgrows its plan"));
	return (fsimg_emit_write(out->emit,
	    ((uint64_t)out->first_block + out->current) * ISO_BLOCK_BYTES,
	    out->plan->block, ISO_BLOCK_BYTES, error));
}

/* Encode one record at the directory's next position: in the block being
 * filled when it fits there, else at the start of the next, as planning
 * packed it.
 */
static fsimg_status_t
put_record(directory_out_t *out, const iso_record_fields_t *fields,
    fsimg_error_t *error)
{
	unsigned char raw[ISO_RECORD_MAX];
	uint64_t position;
	size_t length;
	fsimg_status_t status;

	length = iso_record_encode(fields, raw);
	position = iso_place(&out->position, length);
	if (position / ISO_BLOCK_BYTES != out->current) {
		status = flush_block(out, error);
		if (status != FSIMG_OK)
			return (status);
		bzero(out->plan->block, ISO_BLOCK_BYTES);
		out->current = position / ISO_BLOCK_BYTES;
	}
	bcopy(raw, out->plan->block + position % ISO_BLOCK_BYTES, length);
	return (FSIMG_OK);
}

/* Split length bytes of entries in plan->entries for a record with an
 * identifier of identifier_length bytes, require the split row planned,
 * and assemble it: the record's system use bytes into system_use, their
 * length in *system_use_length, and each fragment written at its place
 * in the continuation region.
 */
static fsimg_status_t
split_entries(directory_out_t *out, size_t length, const iso_row_t *row,
    size_t identifier_length, unsigned char *system_use,
    size_t *system_use_length, fsimg_error_t *error)
{
	iso_plan_t *plan = out->plan;
	iso_split_t split;
	uint64_t positions[ISO_FRAGMENTS_MAX];
	uint64_t region_bytes;
	size_t at;
	fsimg_status_t status;

	status = iso_susp_split(plan->entries, length,
	    iso_record_room(identifier_length), &split, error);
	if (status != FSIMG_OK)
		return (status);
	if (split.record_bytes != row->record_entries ||
	    split.fragments != row->fragments)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_WRITER, "ISO 9660 Rock Ridge entries disagree "
		    "with their plan"));
	for (unsigned i = 0; i < split.fragments; i++) {
		if (split.fragment_bytes[i] != row->fragment_bytes[i])
			return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
			    FSIMG_OP_WRITER, "ISO 9660 Rock Ridge entries "
			    "disagree with their plan"));
	}
	/* Bound the stored position before assembly rounds it to a block.
	 * A position read from corrupt scratch must not wrap during packing.
	 */
	region_bytes = (uint64_t)plan->continuation_blocks * ISO_BLOCK_BYTES;
	if (split.fragments != 0 && row->fragment_position >= region_bytes)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_WRITER, "ISO 9660 continuation position "
		    "lies outside its region"));
	iso_susp_assemble(plan->entries, &split, plan->continuation_block,
	    row->fragment_position, system_use, plan->fragments, positions);
	*system_use_length = split.record_bytes;
	for (unsigned i = 0; i < split.fragments; i++) {
		if (positions[i] > region_bytes ||
		    split.fragment_bytes[i] > region_bytes - positions[i])
			return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
			    FSIMG_OP_WRITER, "ISO 9660 continuation fragment "
			    "lies outside its region"));
	}
	at = 0;
	for (unsigned i = 0; i < split.fragments; i++) {
		status = fsimg_emit_write(out->emit,
		    (uint64_t)plan->continuation_block * ISO_BLOCK_BYTES +
		    positions[i], plan->fragments + at,
		    split.fragment_bytes[i], error);
		if (status != FSIMG_OK)
			return (status);
		at += split.fragment_bytes[i];
	}
	return (FSIMG_OK);
}

/* A dot record: the directory itself (self) or its parent, named by row,
 * the root's own record for both of the root's. In the primary hierarchy
 * it carries PX and TF of the directory it names; the root's first one
 * also SP and ER, continued as planned in the root's row, which is self.
 */
static fsimg_status_t
emit_dot(directory_out_t *out, iso_hierarchy_t hierarchy,
    const iso_row_t *self, const iso_row_t *row, bool parent,
    fsimg_error_t *error)
{
	static const unsigned char identifiers[2] = { 0x00, 0x01 };
	unsigned char system_use[ISO_RECORD_MAX];
	iso_record_fields_t fields;
	iso_rr_input_t input = { .row = row, .record = ISO_RR_DOT };
	size_t length;
	fsimg_status_t status;

	bzero(&fields, sizeof(fields));
	fields.identifier = identifiers + (parent ? 1 : 0);
	fields.identifier_length = 1;
	fields.extent = row->block[hierarchy];
	fields.data_length = row->bytes[hierarchy];
	fields.flags = ISO_FLAG_DIRECTORY;
	bcopy(row->modified, fields.date, ISO_DATE7_BYTES);
	if (hierarchy == ISO_PRIMARY) {
		if (!parent && self->number[ISO_PRIMARY] == 1)
			input.record = ISO_RR_ROOT_SELF;
		length = iso_rr_encode(&input, out->plan->entries);
		if (input.record == ISO_RR_ROOT_SELF) {
			status = split_entries(out, length, self, 1, system_use,
			    &fields.system_use_length, error);
			if (status != FSIMG_OK)
				return (status);
		} else {
			if (length > iso_record_room(1))
				return (fsimg_error_set(error, FSIMG_ECORRUPT,
				    0, FSIMG_OP_WRITER, "ISO 9660 dot record "
				    "entries outgrow their record"));
			bcopy(out->plan->entries, system_use, length);
			fields.system_use_length = length;
		}
		fields.system_use = system_use;
	}
	return (put_record(out, &fields, error));
}

/* One child's records: a directory's one record naming its extent in
 * this hierarchy; a file's one record per section, contiguous from its
 * first block, every one but the last flagged multi-extent; a link's or
 * FIFO's empty record. In the primary hierarchy the first record carries
 * the child's Rock Ridge entries.
 */
static fsimg_status_t
emit_child(directory_out_t *out, iso_hierarchy_t hierarchy,
    const iso_name_t *name, fsimg_error_t *error)
{
	iso_plan_t *plan = out->plan;
	unsigned char system_use[ISO_RECORD_MAX];
	iso_record_fields_t fields;
	iso_row_t row;
	size_t length;
	uint32_t sections;
	fsimg_status_t status;

	status = iso_row_get(plan, name->id, &row, error);
	if (status != FSIMG_OK)
		return (status);
	if (name->length != row.identifier[hierarchy])
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_WRITER, "ISO 9660 name record disagrees with its row"));
	bzero(&fields, sizeof(fields));
	fields.identifier = name->identifier;
	fields.identifier_length = name->length;
	bcopy(row.modified, fields.date, ISO_DATE7_BYTES);
	if (hierarchy == ISO_PRIMARY) {
		status = iso_child_entries(plan, name->id, &row, false, &length,
		    error);
		if (status == FSIMG_OK)
			status = split_entries(out, length, &row, name->length,
			    system_use, &fields.system_use_length, error);
		if (status != FSIMG_OK)
			return (status);
		fields.system_use = system_use;
	}

	if (row.kind == FSIMG_ENTRY_DIRECTORY) {
		fields.flags = ISO_FLAG_DIRECTORY;
		fields.extent = row.block[hierarchy];
		fields.data_length = row.bytes[hierarchy];
		return (put_record(out, &fields, error));
	}
	if (row.kind != FSIMG_ENTRY_FILE)
		return (put_record(out, &fields, error));
	sections = iso_sections(row.size);
	for (uint32_t i = 0; i < sections; i++) {
		fields.extent = row.size == 0 ? 0 : row.first_block +
		    (uint32_t)(i * (ISO_SECTION_MAX / ISO_BLOCK_BYTES));
		fields.data_length = iso_section_bytes(row.size, i);
		fields.flags = i + 1 < sections ? ISO_FLAG_MULTI_EXTENT : 0;
		status = put_record(out, &fields, error);
		if (status != FSIMG_OK)
			return (status);
		fields.system_use_length = 0;
	}
	return (FSIMG_OK);
}

/* Step 3 for directory id: its dot records, then its children in
 * name-stream order, block by block.
 */
static fsimg_status_t
emit_directory(iso_plan_t *plan, iso_hierarchy_t hierarchy,
    fsimg_emit_t *emit, uint64_t id, fsimg_error_t *error)
{
	const fsimg_catalog_entry_t *catalog;
	unsigned char raw[ISO_NAME_RECORD_MAX];
	directory_out_t out;
	iso_name_t name;
	iso_row_t row, parent;
	uint64_t cursor;
	fsimg_status_t status;

	status = iso_row_get(plan, id, &row, error);
	if (status != FSIMG_OK)
		return (status);
	parent = row;
	if (id != 0) {
		status = fsimg_catalog_get(plan->common.writer, id, &catalog,
		    error);
		if (status == FSIMG_OK)
			status = iso_row_get(plan, catalog->parent_id, &parent,
			    error);
		if (status != FSIMG_OK)
			return (status);
	}
	out = (directory_out_t){ .plan = plan, .emit = emit,
	    .first_block = row.block[hierarchy], .bytes = row.bytes[hierarchy] };
	bzero(plan->block, ISO_BLOCK_BYTES);
	status = emit_dot(&out, hierarchy, &row, &row, false, error);
	if (status == FSIMG_OK)
		status = emit_dot(&out, hierarchy, &row, &parent, true, error);
	if (status != FSIMG_OK)
		return (status);

	cursor = row.group[hierarchy];
	for (uint32_t i = 0; i < row.children[hierarchy]; i++) {
		status = iso_name_next(plan->names[hierarchy], &cursor, raw,
		    &name, error);
		if (status == FSIMG_OK)
			status = fsimg_session_charge(
			    &plan->common.writer->session, 0, 0, 1, error);
		if (status == FSIMG_OK)
			status = emit_child(&out, hierarchy, &name, error);
		if (status != FSIMG_OK)
			return (status == FSIMG_DONE ? FSIMG_ECORRUPT : status);
	}
	status = flush_block(&out, error);
	if (status != FSIMG_OK)
		return (status);
	if ((out.current + 1) * ISO_BLOCK_BYTES != out.bytes)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_WRITER, "ISO 9660 directory ends before its plan"));
	return (FSIMG_OK);
}

/* Step 3: a hierarchy's directories in path-table order. */
static fsimg_status_t
emit_directories(iso_plan_t *plan, iso_hierarchy_t hierarchy,
    fsimg_emit_t *emit, fsimg_error_t *error)
{
	unsigned char path[ISO_PATH_STREAM_MAX];
	uint64_t cursor;
	size_t length;
	fsimg_status_t status;

	cursor = 0;
	for (;;) {
		status = fsimg_stream_next(plan->paths[hierarchy], &cursor,
		    path, sizeof(path), &length, error);
		if (status == FSIMG_DONE)
			return (FSIMG_OK);
		if (status != FSIMG_OK)
			return (status);
		status = emit_directory(plan, hierarchy, emit,
		    fsimg_load_le64(path), error);
		if (status != FSIMG_OK)
			return (status);
	}
}

/* Step 4: each file's payload into its contiguous sections. An empty
 * file is a copy without regions, which still opens and validates its
 * source.
 */
static fsimg_status_t
emit_files(iso_plan_t *plan, fsimg_emit_t *emit, fsimg_error_t *error)
{
	fsimg_emit_region_t region;
	fsimg_emit_payload_t payload;
	iso_row_t row;
	fsimg_status_t status;

	for (uint64_t id = 1; id <= plan->common.writer->count; id++) {
		status = iso_row_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		if (row.kind != FSIMG_ENTRY_FILE)
			continue;
		bzero(&payload, sizeof(payload));
		payload.length = row.size;
		payload.initialized = row.size;
		if (row.size != 0) {
			region.image_offset = (uint64_t)row.first_block *
			    ISO_BLOCK_BYTES;
			region.capacity = ISO_BLOCKS(row.size) *
			    ISO_BLOCK_BYTES;
			payload.regions = &region;
			payload.region_count = 1;
		}
		status = fsimg_emit_copy(emit, id, &payload, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

fsimg_status_t
iso_write_emit(fsimg_writer_t *writer, fsimg_emit_t *emit,
    fsimg_error_t *error)
{
	iso_plan_t *plan = (iso_plan_t *)writer->plan;
	fsimg_status_t status;

	status = emit_descriptors(plan, emit, error);
	for (unsigned h = 0; status == FSIMG_OK && h < ISO_HIERARCHIES; h++)
		status = emit_path_tables(plan, (iso_hierarchy_t)h, emit,
		    error);
	for (unsigned h = 0; status == FSIMG_OK && h < ISO_HIERARCHIES; h++)
		status = emit_directories(plan, (iso_hierarchy_t)h, emit,
		    error);
	if (status == FSIMG_OK)
		status = emit_files(plan, emit, error);
	return (status);
}
