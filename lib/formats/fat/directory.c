/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fat.h"

typedef struct directory_prepass {
	fsimg_reader_t	*reader;
	fsimg_stream_t	*entries;
	fsimg_stream_t	*keys;
	fat_lfn_t	 lfn;
	uint64_t	 slots;
	size_t		 path_length;
	uint32_t	 level;
	uint32_t	 self_cluster;
	uint32_t	 parent_cluster;
	bool		 ended;
} directory_prepass_t;

/* A prepass has two outputs: original-order entry records, and lookup keys
 * carrying their entry ordinal. Only the keys are sorted. Sorting can reject
 * an ambiguous namespace without changing iteration order or keeping a wide
 * directory in RAM. The entry stream is published only after that succeeds.
 */

static int key_compare(const void *, size_t, const void *, size_t, void *);
static fsimg_status_t validate_keys(fsimg_stream_t *, fsimg_error_t *);
static fsimg_status_t append_key(directory_prepass_t *, const char *,
    uint64_t, fsimg_error_t *);
static fsimg_status_t validate_dot(directory_prepass_t *, const unsigned char *,
    fsimg_error_t *);
static fsimg_status_t decode_slot(directory_prepass_t *, const unsigned char *,
    fsimg_error_t *);
static fsimg_status_t read_directory_run(directory_prepass_t *,
    const fat_extent_t *, fsimg_error_t *);
static fsimg_status_t prepare_directory(fsimg_reader_t *, uint32_t, uint32_t,
    size_t, uint32_t, fat_directory_t *, fsimg_error_t *);
static fsimg_status_t traversal_start(fsimg_reader_t *, fsimg_error_t *);

static int
key_compare(const void *a, size_t alen, const void *b, size_t blen,
    void *argument FSIMG_UNUSED)
{
	size_t length;
	int comparison;

	if (alen < 8 || blen < 8)
		return ((alen > blen) - (alen < blen));
	length = alen < blen ? alen - 8 : blen - 8;
	comparison = memcmp((const unsigned char *)a + 8,
	    (const unsigned char *)b + 8, length);
	return (comparison != 0 ? comparison : (alen > blen) - (alen < blen));
}

static fsimg_status_t
validate_keys(fsimg_stream_t *keys, fsimg_error_t *error)
{
	fsimg_stream_t *sorted;
	unsigned char current[FAT_KEY_BYTES], previous[FAT_KEY_BYTES];
	uint64_t cursor;
	size_t length, previous_length;
	fsimg_status_t status;

	sorted = NULL;
	status = fsimg_stream_seal(keys, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_stream_sort(keys, key_compare, NULL, NULL, &sorted, error);
	if (status != FSIMG_OK)
		return (status);
	cursor = 0;
	previous_length = 0;
	for (;;) {
		status = fsimg_stream_next(sorted, &cursor, current, sizeof(current),
		    &length, error);
		if (status == FSIMG_DONE) {
			status = FSIMG_OK;
			break;
		}
		if (status != FSIMG_OK)
			break;
		if (length <= 8 || (length - 8) % 4 != 0) {
			status = fsimg_fat_corrupt(error, "invalid FAT namespace key record");
			break;
		}
		if (previous_length == length &&
		    bcmp(previous + 8, current + 8, length - 8) == 0 &&
		    fsimg_meta_get_u64(previous) != fsimg_meta_get_u64(current)) {
			status = fsimg_fat_corrupt(error, "ambiguous FAT long-name or alias namespace");
			break;
		}
		bcopy(current, previous, length);
		previous_length = length;
	}
	fsimg_stream_free(sorted);
	return (status);
}

static fsimg_status_t
append_key(directory_prepass_t *pass, const char *name, uint64_t ordinal,
    fsimg_error_t *error)
{
	unsigned char record[FAT_KEY_BYTES];
	size_t length;
	fsimg_status_t status;

	fsimg_meta_put_u64(record, ordinal);
	status = fsimg_fat_name_key(name, record + 8, &length, error);
	if (status != FSIMG_OK)
		return (status);
	return (fsimg_stream_append(pass->keys, record, length + 8, NULL, error));
}

static fsimg_status_t
validate_dot(directory_prepass_t *pass, const unsigned char *raw,
    fsimg_error_t *error)
{
	const fat_geometry_t *g = &pass->reader->fat->geometry;
	fsimg_entry_t entry;
	unsigned char copy[32];
	uint32_t cluster, expected;
	fsimg_status_t status;

	expected = pass->slots == 0 ? pass->self_cluster : pass->parent_cluster;
	if (bcmp(raw, pass->slots == 0 ? ".          " : "..         ", 11) != 0 ||
	    raw[11] != 0x10 || raw[12] != 0 || fsimg_fat_u32(raw + 28) != 0)
		return (fsimg_fat_corrupt(error, "missing or invalid FAT dot directory record"));
	cluster = fsimg_fat_u16(raw + 26);
	if (g->variant == 32)
		cluster |= (uint32_t)fsimg_fat_u16(raw + 20) << 16;
	else if (fsimg_fat_u16(raw + 20) != 0)
		return (fsimg_fat_corrupt(error, "extended attributes in FAT dot record"));
	if (cluster != expected)
		return (fsimg_fat_corrupt(error, "FAT dot record has wrong directory cluster"));
	/* A root parent is encoded as cluster zero, including on FAT32. Substitute
	 * a valid cluster solely for the common attribute/timestamp validator,
	 * never for traversal.
	 */
	bcopy(raw, copy, sizeof(copy));
	copy[20] = copy[21] = copy[27] = 0;
	copy[26] = 2;
	status = fsimg_fat_entry_decode(g, copy, &entry, &cluster, error);
	return (status);
}

static fsimg_status_t
decode_slot(directory_prepass_t *pass, const unsigned char *raw,
    fsimg_error_t *error)
{
	fsimg_reader_t *reader = pass->reader;
	fsimg_fat_reader_t *fat = reader->fat;
	fsimg_entry_t entry;
	char name[FAT_NAME_BYTES], alias[FAT_NAME_BYTES];
	size_t length;
	uint32_t cluster;
	uint64_t ordinal;
	fsimg_status_t status;

	if (pass->level != 0 && pass->slots < 2) {
		status = validate_dot(pass, raw, error);
		if (status == FSIMG_OK)
			pass->slots++;
		return (status);
	}
	pass->slots++;
	/* End/deletion markers and structural records never become children.
	 * Deleted slots discard a preceding long name; a live orphan is corruption.
	 */
	if (raw[0] == 0) {
		if (pass->lfn.active)
			return (fsimg_fat_corrupt(error, "orphaned FAT long-name sequence"));
		pass->ended = true;
		return (FSIMG_OK);
	}
	if (raw[0] == 0xe5) {
		bzero(&pass->lfn, sizeof(pass->lfn));
		return (FSIMG_OK);
	}
	if (raw[11] == 0x0f)
		return (fsimg_fat_lfn_slot(&pass->lfn, raw, error));
	if ((raw[11] & 0x08) != 0) {
		/* Native formatters can retain the archive bit on a volume label.
		 * It still has no data allocation and never becomes a child entry.
		 */
		if (pass->level != 0 || (raw[11] & ~0x20u) != 0x08 || pass->lfn.active ||
		    fat->root_label_seen || fsimg_fat_u16(raw + 20) != 0 ||
		    fsimg_fat_u16(raw + 26) != 0 || fsimg_fat_u32(raw + 28) != 0)
			return (fsimg_fat_corrupt(error, "invalid or duplicate FAT volume-label entry"));
		status = fsimg_fat_label(raw, fat->root_label, error);
		if (status != FSIMG_OK)
			return (status);
		fat->root_label_seen = true;
		return (FSIMG_OK);
	}
	/* Both spellings participate in lookup, even when the long name hides the
	 * alias in a listing. Validate metadata before charging/admitting an entry.
	 */
	status = fsimg_fat_short_name(raw, alias, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_fat_entry_name(&pass->lfn, raw, name, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_fat_entry_decode(&fat->geometry, raw, &entry, &cluster, error);
	if (status != FSIMG_OK)
		return (status);
	length = strlen(name);
	if (pass->path_length + 1 + length + 1 > reader->ctx->options.max_path_bytes)
		return (fsimg_limit_error(error, FSIMG_RESOURCE_PATH,
		    reader->ctx->options.max_path_bytes, pass->path_length + length + 2));
	if (pass->level >= reader->ctx->options.max_depth)
		return (fsimg_limit_error(error, FSIMG_RESOURCE_DEPTH,
		    reader->ctx->options.max_depth, (uint64_t)pass->level + 1));
	status = fsimg_session_charge(&reader->session, 1, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	ordinal = pass->entries->count;
	/* The private record is the 32-byte short entry followed by one bounded,
	 * NUL-terminated UTF-8 component. Replay decodes the short entry again.
	 */
	bcopy(raw, fat->record, 32);
	bcopy(name, fat->record + 32, length + 1);
	status = fsimg_stream_append(pass->entries, fat->record, 32 + length + 1,
	    NULL, error);
	if (status != FSIMG_OK)
		return (status);
	status = append_key(pass, name, ordinal, error);
	if (status != FSIMG_OK)
		return (status);
	return (append_key(pass, alias, ordinal, error));
}

static fsimg_status_t
read_directory_run(directory_prepass_t *pass, const fat_extent_t *run,
    fsimg_error_t *error)
{
	fsimg_reader_t *reader = pass->reader;
	fsimg_fat_reader_t *fat = reader->fat;
	uint64_t offset;
	size_t batch, transferred;
	fsimg_status_t status;

	offset = 0;
	while (offset < run->length && !pass->ended) {
		batch = sizeof(fat->directory_window);
		if (batch > run->length - offset)
			batch = (size_t)(run->length - offset);
		status = fsimg_reader_bytes(reader, run->image_offset + offset,
		    fat->directory_window, batch, &transferred, error);
		if (status != FSIMG_OK)
			return (status);
		for (size_t i = 0; i < batch && !pass->ended; i += 32) {
			status = decode_slot(pass, fat->directory_window + i, error);
			if (status != FSIMG_OK)
				return (status);
		}
		offset += batch;
	}
	return (FSIMG_OK);
}

static fsimg_status_t
prepare_directory(fsimg_reader_t *reader, uint32_t cluster, uint32_t parent,
    size_t path_length, uint32_t level, fat_directory_t *out,
    fsimg_error_t *error)
{
	fsimg_fat_reader_t *fat = reader->fat;
	const fat_geometry_t *g = &fat->geometry;
	directory_prepass_t pass;
	fat_chain_t chain;
	fat_extent_t run;
	uint64_t first, count;
	fsimg_status_t status;
	bool claimed;

	bzero(&pass, sizeof(pass));
	pass.reader = reader;
	pass.self_cluster = cluster;
	pass.parent_cluster = parent;
	pass.path_length = path_length;
	pass.level = level;
	status = fsimg_stream_new(&reader->session, FAT_RECORD_BYTES,
	    &pass.entries, error);
	if (status != FSIMG_OK)
		goto done;
	status = fsimg_stream_new(&reader->session, FAT_KEY_BYTES, &pass.keys, error);
	if (status != FSIMG_OK)
		goto done;
	if (cluster == 0) {
		run.logical_offset = 0;
		run.image_offset = g->root_offset;
		run.length = (uint64_t)g->root_entries * 32;
		status = read_directory_run(&pass, &run, error);
		if (status != FSIMG_OK)
			goto done;
	} else {
		fsimg_fat_chain_init(&chain, cluster, 0);
		for (;;) {
			status = fsimg_fat_chain_run(reader, &chain, &run, error);
			if (status == FSIMG_DONE)
				break;
			if (status != FSIMG_OK)
				goto done;
			first = (run.image_offset - g->data_offset) / g->cluster_bytes + 2;
			/* Claim allocation even after the directory's end marker. Unused
			 * tail clusters still belong to this chain and cannot hide sharing.
			 */
			count = run.length / g->cluster_bytes;
			for (uint64_t i = 0; i < count; i++) {
				status = fsimg_bitset_test_and_set(fat->directory_clusters,
				    first + i, &claimed, error);
				if (status != FSIMG_OK)
					goto done;
				if (claimed) {
					status = fsimg_fat_corrupt(error, "shared or cyclic FAT directory chain");
					goto done;
				}
			}
			status = fsimg_fat_owner_append(reader, &run, error);
			if (status != FSIMG_OK)
				goto done;
			status = read_directory_run(&pass, &run, error);
			if (status != FSIMG_OK)
				goto done;
		}
	}
	if (pass.lfn.active) {
		status = fsimg_fat_corrupt(error, "orphaned FAT long name at directory end");
		goto done;
	}
	status = validate_keys(pass.keys, error);
	if (status != FSIMG_OK)
		goto done;
	status = fsimg_stream_seal(pass.entries, error);
	if (status != FSIMG_OK)
		goto done;
	out->entries = pass.entries;
	out->cluster = cluster;
	out->path_length = path_length;
	out->cursor = 0;
	pass.entries = NULL;
done:
	fsimg_stream_free(pass.keys);
	fsimg_stream_free(pass.entries);
	return (status);
}

static fsimg_status_t
traversal_start(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_fat_reader_t *fat = reader->fat;
	fsimg_error_t detail;
	uint64_t bytes;
	size_t allocation;
	fsimg_status_t status;

	if (!fsimg_mul_u64((uint64_t)reader->ctx->options.max_depth + 1,
	    sizeof(*fat->directories), &bytes) || !fsimg_to_size(bytes, &allocation))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_READER, "directory cursor storage overflow"));
	fat->directories = fsimg_alloc(reader->ctx, allocation, &detail);
	if (fat->directories == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	fat->directory_capacity = (uint64_t)reader->ctx->options.max_depth + 1;
	status = fsimg_bitset_new(&reader->session,
	    (uint64_t)fat->geometry.clusters + 2, &fat->directory_clusters, error);
	if (status != FSIMG_OK)
		return (status);
	status = prepare_directory(reader, fat->geometry.root_cluster, 0, 0, 0,
	    &fat->directories[0], error);
	if (status != FSIMG_OK)
		return (status);
	fat->started = true;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_fat_next(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_fat_reader_t *fat = reader->fat;
	fat_directory_t *directory;
	uint32_t parent;
	size_t length, name_length;
	fsimg_status_t status;

	fsimg_store_free(fat->index);
	fat->index = NULL;
	fat->index_count = 0;
	fat->next_offset = 0;
	fat->file_started = false;
	bzero(&fat->extent, sizeof(fat->extent));
	if (!fat->started) {
		status = traversal_start(reader, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (fat->pending_directory) {
		/* Descent happens on the call after returning the directory itself.
		 * Parent cursors retain only their spillable streams and path lengths;
		 * the shared path buffer keeps every ancestor prefix intact.
		 */
		if (fat->depth >= reader->ctx->options.max_depth)
			return (fsimg_limit_error(error, FSIMG_RESOURCE_DEPTH,
			    reader->ctx->options.max_depth, (uint64_t)fat->depth + 1));
		parent = fat->depth == 0 ? 0 : fat->directories[fat->depth].cluster;
		status = prepare_directory(reader, fat->current_cluster, parent,
		    strlen(reader->path), fat->depth + 1,
		    &fat->directories[fat->depth + 1], error);
		if (status != FSIMG_OK)
			return (status);
		fat->depth++;
		fat->pending_directory = false;
	}
	for (;;) {
		directory = &fat->directories[fat->depth];
		status = fsimg_stream_next(directory->entries, &directory->cursor,
		    fat->record, sizeof(fat->record), &length, error);
		if (status == FSIMG_DONE) {
			fsimg_stream_free(directory->entries);
			directory->entries = NULL;
			if (fat->depth == 0)
				return (FSIMG_DONE);
			fat->depth--;
			continue;
		}
		if (status != FSIMG_OK)
			return (status);
		if (length < 34 || fat->record[length - 1] != 0 ||
		    memchr(fat->record + 32, 0, length - 33) != NULL)
			return (fsimg_fat_corrupt(error, "invalid cached FAT entry record"));
		name_length = length - 33;
		status = fsimg_fat_name_validate((const char *)fat->record + 32, error);
		if (status != FSIMG_OK)
			return (status);
		if (directory->path_length + name_length + 2 >
		    reader->ctx->options.max_path_bytes)
			return (fsimg_fat_corrupt(error, "cached FAT entry exceeds path limit"));
		status = fsimg_fat_entry_decode(&fat->geometry, fat->record, &reader->entry,
		    &fat->current_cluster, error);
		if (status != FSIMG_OK)
			return (status);
		reader->path[directory->path_length] = '/';
		bcopy(fat->record + 32, reader->path + directory->path_length + 1,
		    name_length + 1);
		reader->entry.path = reader->path;
		fat->pending_directory = reader->entry.kind == FSIMG_ENTRY_DIRECTORY;
		return (FSIMG_OK);
	}
}

fsimg_status_t
fsimg_fat_match_name(fsimg_reader_t *reader, const char *component, bool *matches,
    fsimg_error_t *error)
{
	fsimg_fat_reader_t *fat = reader->fat;
	unsigned char requested[FAT_KEY_BYTES], candidate[FAT_KEY_BYTES];
	char alias[FAT_NAME_BYTES];
	size_t requested_length, candidate_length;
	fsimg_status_t status;

	*matches = false;
	status = fsimg_fat_name_key(component, requested, &requested_length, error);
	if (status != FSIMG_OK)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_READER, "selection component is not representable in FAT"));
	status = fsimg_fat_name_key((const char *)fat->record + 32,
	    candidate, &candidate_length, error);
	if (status != FSIMG_OK)
		return (status);
	if (requested_length == candidate_length &&
	    bcmp(requested, candidate, requested_length) == 0) {
		*matches = true;
		return (FSIMG_OK);
	}
	/* The directory prepass validated both spellings against every sibling.
	 * Only this current entry's alias needs decoding during lookup.
	 */
	status = fsimg_fat_short_name(fat->record, alias, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_fat_name_key(alias, candidate, &candidate_length, error);
	if (status != FSIMG_OK)
		return (status);
	*matches = requested_length == candidate_length &&
	    bcmp(requested, candidate, requested_length) == 0;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_fat_skip_directory(fsimg_reader_t *reader, fsimg_error_t *error)
{
	if (!reader->fat->pending_directory)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_READER, "FAT directory descent is not pending"));
	reader->fat->pending_directory = false;
	return (FSIMG_OK);
}
