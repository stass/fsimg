/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "exfat.h"
#include "unicode.h"

/* One prepass validates a complete directory, keeps original-order records,
 * and sorts only namespace keys. No children are visible before ambiguity and
 * entry-set checks succeed. The parent retains only its spillable entry stream.
 */
typedef struct exfat_prepass {
	exfat_directory_cursor_t input;
	fsimg_stream_t	*entries;
	fsimg_stream_t	*keys;
	size_t		 path_length;
	uint32_t	 level;
} exfat_prepass_t;

static fsimg_status_t next_slot(exfat_directory_cursor_t *, unsigned char *, fsimg_error_t *);
static fsimg_status_t benign_allocation(fsimg_reader_t *, const unsigned char *,
    bool, fsimg_error_t *);
static fsimg_status_t decode_set(exfat_prepass_t *, size_t, fsimg_error_t *);
static fsimg_status_t prepare_directory(fsimg_reader_t *, exfat_map_t *, size_t,
    uint32_t, exfat_directory_t *, fsimg_error_t *);
static fsimg_status_t mark_directory(fsimg_reader_t *, exfat_map_t *,
    fsimg_error_t *);
static fsimg_status_t mark_run(fsimg_reader_t *, uint32_t, uint32_t,
    fsimg_error_t *);
static fsimg_status_t start_traversal(fsimg_reader_t *, fsimg_error_t *);

static fsimg_status_t
next_slot(exfat_directory_cursor_t *pass, unsigned char *out, fsimg_error_t *error)
{
	uint64_t remaining;
	fsimg_status_t status;

	if (pass->window_position == pass->window_length) {
		remaining = pass->map->allocated_bytes - pass->offset;
		if (remaining == 0)
			return (FSIMG_DONE);
		pass->window_length = remaining > sizeof(pass->window) ?
		    sizeof(pass->window) : (size_t)remaining;
		status = exfat_map_read(pass->reader, pass->map, pass->offset,
		    pass->window, pass->window_length, error);
		if (status != FSIMG_OK)
			return (status);
		pass->offset += pass->window_length;
		pass->window_position = 0;
	}
	bcopy(pass->window + pass->window_position, out, 32);
	pass->window_position += 32;
	return (fsimg_session_charge(&pass->reader->session, 0, 0, 1, error));
}

fsimg_status_t
exfat_directory_set_next(exfat_directory_cursor_t *cursor, fsimg_error_t *error)
{
	size_t set_length;
	fsimg_status_t status;

	for (;;) {
		status = next_slot(cursor, cursor->set, error);
		if (status == FSIMG_DONE)
			return (FSIMG_DONE);
		if (status != FSIMG_OK)
			return (status);
		if (cursor->set[0] == 0) {
			cursor->ended = true;
			continue;
		}
		if (cursor->set[0] == 0x80)
			return (exfat_corrupt(error, "invalid exFAT directory entry type 0x80"));
		if (cursor->ended) {
			status = exfat_corrupt(error, "invalid entry after exFAT directory terminator");
			return (status);
		}
		if (cursor->set[0] < 0x80)
			continue;
		if ((cursor->set[0] & 0x40) != 0) {
			status = exfat_corrupt(error, "orphaned exFAT secondary directory entry");
			return (status);
		}
		if (cursor->set[0] >= 0x81 && cursor->set[0] <= 0x83) {
			if (!cursor->root) {
				status = exfat_corrupt(error, "exFAT volume metadata outside root");
				return (status);
			}
			cursor->set_length = 32;
			return (FSIMG_OK);
		}
		if (cursor->set[0] != 0x85 && (cursor->set[0] & 0x20) == 0) {
			status = fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
			    FSIMG_OP_READER, "unknown critical exFAT primary entry");
			return (status);
		}
		set_length = ((size_t)cursor->set[1] + 1) * 32;
		for (size_t i = 32; i < set_length; i += 32) {
			status = next_slot(cursor, cursor->set + i, error);
			if (status == FSIMG_DONE)
				status = exfat_corrupt(error, "truncated exFAT directory entry set");
			if (status != FSIMG_OK)
				return (status);
			if ((cursor->set[i] & 0xc0) != 0xc0) {
				status = exfat_corrupt(error, "invalid exFAT secondary entry type");
				return (status);
			}
		}
		if (exfat_set_checksum(cursor->set, set_length) != exfat_u16(cursor->set + 2)) {
			status = exfat_corrupt(error, "invalid exFAT directory entry-set checksum");
			return (status);
		}
		cursor->set_length = set_length;
		return (FSIMG_OK);
	}
}

static fsimg_status_t
benign_allocation(fsimg_reader_t *reader, const unsigned char *raw,
    bool primary, fsimg_error_t *error)
{
	exfat_stream_t stream;
	exfat_map_t map;
	unsigned flags;
	fsimg_status_t status;

	/* Generic primary flags follow SecondaryCount and SetChecksum; generic
	 * secondary flags occupy byte 1, including Vendor Allocation entries.
	 */
	flags = primary ? exfat_u16(raw + 4) : raw[1];
	if ((flags & 1) == 0) {
		if ((flags & 2) != 0)
			return (exfat_corrupt(error, "exFAT NoFatChain without allocation authority"));
		return (FSIMG_OK);
	}
	stream = (exfat_stream_t){ .first = exfat_u32(raw + 20),
	    .length = fsimg_meta_get_u64(raw + 24), .contiguous = (flags & 2) != 0 };
	stream.valid_length = stream.length;
	status = exfat_map_build(reader, &stream, EXFAT_LENGTH_COVER_DATA, &map, error);
	if (status != FSIMG_OK)
		return (status);
	status = exfat_map_own(reader, &map, error);
	exfat_map_free(&map);
	return (status);
}

static fsimg_status_t
decode_set(exfat_prepass_t *pass, size_t set_length, fsimg_error_t *error)
{
	fsimg_reader_t *reader = pass->input.reader;
	unsigned char *raw = pass->input.set;
	unsigned char record[EXFAT_RECORD_BYTES];
	uint16_t units[255], validated[255];
	fsimg_entry_t entry;
	exfat_stream_t stream;
	size_t count, names, written, validated_count;
	fsimg_status_t status;

	if (raw[0] != 0x85) {
		status = benign_allocation(reader, raw, true, error);
		if (status != FSIMG_OK)
			return (status);
		for (size_t i = 32; i < set_length; i += 32) {
			if ((raw[i] & 0x20) == 0)
				return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
				    FSIMG_OP_READER, "critical secondary in unrecognized exFAT set"));
			status = benign_allocation(reader, raw + i, false, error);
			if (status != FSIMG_OK)
				return (status);
		}
		return (FSIMG_OK);
	}
	if (set_length < 96 || raw[32] != 0xc0)
		return (exfat_corrupt(error, "missing exFAT stream extension"));
	count = raw[35];
	names = (count + 14) / 15;
	if (count == 0 || 64 + names * 32 > set_length)
		return (exfat_corrupt(error, "invalid exFAT name entry count"));
	for (size_t i = 0; i < names; i++) {
		if (raw[64 + i * 32] != 0xc1 || (raw[65 + i * 32] & 3) != 0)
			return (exfat_corrupt(error, "invalid exFAT filename entry order or flags"));
		for (size_t j = 0; j < 15 && i * 15 + j < count; j++)
			units[i * 15 + j] = exfat_u16(raw + 66 + i * 32 + j * 2);
	}
	for (size_t i = 64 + names * 32; i < set_length; i += 32) {
		if ((raw[i] & 0x20) == 0)
			return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
			    FSIMG_OP_READER, "unknown critical exFAT secondary entry"));
		status = benign_allocation(reader, raw + i, false, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (exfat_name_hash(units, count, reader->format.exfat->upcase) !=
	    exfat_u16(raw + 36))
		return (exfat_corrupt(error, "invalid exFAT filename hash"));
	status = fsimg_utf16_to_utf8(units, count, (char *)record + 64,
	    EXFAT_NAME_BYTES - 1, &written, error);
	if (status != FSIMG_OK)
		return (exfat_corrupt(error, "invalid exFAT filename UTF-16"));
	record[64 + written] = 0;
	if (memchr(record + 64, 0, written) != NULL)
		return (exfat_corrupt(error, "embedded zero in exFAT filename"));
	status = exfat_name_units((const char *)record + 64, validated,
	    &validated_count, error);
	if (status != FSIMG_OK)
		return (exfat_corrupt(error, "invalid exFAT filename component"));
	status = exfat_entry_decode(raw, &entry, &stream, error);
	if (status != FSIMG_OK)
		return (status);
	status = exfat_stream_validate(&reader->format.exfat->geometry, &stream,
	    entry.kind == FSIMG_ENTRY_DIRECTORY, error);
	if (status != FSIMG_OK)
		return (status);
	if (pass->path_length + written + 2 > reader->ctx->options.max_path_bytes)
		return (fsimg_limit_error(error, FSIMG_RESOURCE_PATH,
		    reader->ctx->options.max_path_bytes, pass->path_length + written + 2));
	if (pass->level >= reader->ctx->options.max_depth)
		return (fsimg_limit_error(error, FSIMG_RESOURCE_DEPTH,
		    reader->ctx->options.max_depth, (uint64_t)pass->level + 1));
	status = fsimg_session_charge(&reader->session, 1, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	status = exfat_key_append(pass->keys, 0, units, count,
	    reader->format.exfat->upcase, error);
	if (status != FSIMG_OK)
		return (status);
	bcopy(raw, record, 64);
	return (fsimg_stream_append(pass->entries, record, 65 + written, NULL, error));
}

static fsimg_status_t
mark_run(fsimg_reader_t *reader, uint32_t first, uint32_t count,
    fsimg_error_t *error)
{
	bool claimed;
	fsimg_status_t status;

	for (uint32_t i = 0; i < count; i++) {
		status = fsimg_bitset_test_and_set(reader->format.exfat->directory_clusters,
		    (uint64_t)first + i, &claimed, error);
		if (status != FSIMG_OK)
			return (status);
		if (claimed)
			return (exfat_corrupt(error, "shared or cyclic exFAT directory allocation"));
	}
	return (FSIMG_OK);
}

/* Claim every cluster of a directory's chain before decoding it, so
 * re-entering claimed storage through a cycle or a shared chain is
 * corruption. The claim is as good as the chain the map was built from; a
 * damaged scratch record that names other heap clusters is caught only by
 * ownership verification.
 */
static fsimg_status_t
mark_directory(fsimg_reader_t *reader, exfat_map_t *map, fsimg_error_t *error)
{
	return (exfat_map_runs(reader, map, mark_run, error));
}

static fsimg_status_t
prepare_directory(fsimg_reader_t *reader, exfat_map_t *map, size_t path_length,
    uint32_t level, exfat_directory_t *out, fsimg_error_t *error)
{
	exfat_prepass_t *pass;
	fsimg_error_t detail;
	fsimg_status_t status;

	pass = fsimg_alloc(reader->ctx, sizeof(*pass), &detail);
	if (pass == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	pass->input.reader = reader;
	pass->input.map = map;
	pass->input.root = level == 0;
	pass->path_length = path_length;
	pass->level = level;
	status = mark_directory(reader, map, error);
	if (status != FSIMG_OK)
		goto done;
	status = fsimg_stream_new(&reader->session, EXFAT_RECORD_BYTES, &pass->entries, error);
	if (status != FSIMG_OK)
		goto done;
	status = fsimg_stream_new(&reader->session, 518, &pass->keys, error);
	if (status != FSIMG_OK)
		goto done;
	for (;;) {
		status = exfat_directory_set_next(&pass->input, error);
		if (status == FSIMG_DONE)
			break;
		if (status != FSIMG_OK)
			goto done;
		if (pass->input.set[0] >= 0x81 && pass->input.set[0] <= 0x83)
			continue;
		status = decode_set(pass, pass->input.set_length, error);
		if (status != FSIMG_OK)
			goto done;
	}
	status = exfat_keys_unique(pass->keys, true, error);
	if (status != FSIMG_OK)
		goto done;
	status = fsimg_stream_seal(pass->entries, error);
	if (status != FSIMG_OK)
		goto done;
	out->entries = pass->entries;
	out->cursor = 0;
	out->path_length = path_length;
	pass->entries = NULL;
done:
	fsimg_stream_free(pass->keys);
	fsimg_stream_free(pass->entries);
	fsimg_dealloc(pass);
	return (status);
}

static fsimg_status_t
start_traversal(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_exfat_reader_t *exfat = reader->format.exfat;
	uint64_t bytes;
	size_t allocation;
	fsimg_error_t detail;
	fsimg_status_t status;

	if (!fsimg_mul_u64((uint64_t)reader->ctx->options.max_depth + 1,
	    sizeof(*exfat->directories), &bytes) || !fsimg_to_size(bytes, &allocation))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_READER, "exFAT directory stack size overflow"));
	exfat->directories = fsimg_alloc(reader->ctx, allocation, &detail);
	if (exfat->directories == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	status = fsimg_bitset_new(&reader->session, (uint64_t)exfat->geometry.clusters + 2,
	    &exfat->directory_clusters, error);
	if (status != FSIMG_OK)
		return (status);
	status = exfat_map_own(reader, &exfat->root, error);
	if (status != FSIMG_OK)
		return (status);
	status = prepare_directory(reader, &exfat->root, 0, 0,
	    &exfat->directories[0], error);
	if (status == FSIMG_OK)
		exfat->started = true;
	return (status);
}

fsimg_status_t
exfat_next(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_exfat_reader_t *exfat = reader->format.exfat;
	exfat_directory_t *directory;
	exfat_map_t map;
	size_t length, name_length;
	fsimg_status_t status;

	exfat_map_free(&exfat->file);
	if (!exfat->started) {
		status = start_traversal(reader, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (exfat->pending_directory) {
		status = exfat_map_build(reader, &exfat->current, EXFAT_LENGTH_EXACT_ALLOCATION, &map, error);
		if (status != FSIMG_OK)
			return (status);
		status = exfat_map_own(reader, &map, error);
		if (status == FSIMG_OK)
			status = prepare_directory(reader, &map, strlen(reader->path),
			    exfat->depth + 1, &exfat->directories[exfat->depth + 1], error);
		exfat_map_free(&map);
		if (status != FSIMG_OK)
			return (status);
		exfat->depth++;
		exfat->pending_directory = false;
	}
	for (;;) {
		directory = &exfat->directories[exfat->depth];
		status = fsimg_stream_next(directory->entries, &directory->cursor,
		    exfat->record, sizeof(exfat->record), &length, error);
		if (status == FSIMG_DONE) {
			fsimg_stream_free(directory->entries);
			directory->entries = NULL;
			if (exfat->depth == 0)
				return (FSIMG_DONE);
			exfat->depth--;
			continue;
		}
		if (status != FSIMG_OK)
			return (status);
		if (length < 66 || exfat->record[length - 1] != 0 ||
		    memchr(exfat->record + 64, 0, length - 65) != NULL)
			return (exfat_corrupt(error, "invalid cached exFAT entry record"));
		name_length = length - 65;
		status = exfat_entry_decode(exfat->record, &reader->entry,
		    &exfat->current, error);
		if (status != FSIMG_OK)
			return (status);
		reader->path[directory->path_length] = '/';
		bcopy(exfat->record + 64, reader->path + directory->path_length + 1,
		    name_length + 1);
		reader->entry.path = reader->path;
		exfat->pending_directory = reader->entry.kind == FSIMG_ENTRY_DIRECTORY;
		if (!exfat->pending_directory) {
			status = exfat_map_build(reader, &exfat->current, EXFAT_LENGTH_COVER_DATA,
			    &exfat->file, error);
			if (status != FSIMG_OK)
				return (status);
			status = exfat_map_own(reader, &exfat->file, error);
			if (status != FSIMG_OK)
				return (status);
		}
		return (FSIMG_OK);
	}
}
