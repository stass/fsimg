/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fsimg.h"
#include "host.h"
#include "internal.h"
#include "mtree.h"
#include "unicode.h"

/* Arbitrary bytes as an mtree(5) manifest under finite limits. Every entry
 * the parser accepts must keep the parser's promises: a valid image path
 * within the limits, a link target of 1 to 1023 bytes on a link only, a
 * relative contents= path without an empty, "." or ".." component, holes
 * that are ordered, apart and within the size, and valid metadata. Each
 * input is parsed twice, once whole and once in 13-byte reads, and both
 * must reach the same status after the same entries, so read boundaries
 * cannot change what a manifest means. Silently dropped lines are the unit
 * tests' to find, against real mtree -c and bsdtar output.
 */
typedef struct fuzz_input {
	const uint8_t	*data;
	size_t		 size;
	size_t		 offset;
	size_t		 chunk;
} fuzz_input_t;

typedef struct fuzz_outcome {
	fsimg_ctx_t	*ctx;
	uint64_t	 entries;
	uint64_t	 fingerprint;
	fsimg_status_t	 status;
} fuzz_outcome_t;

static fsimg_status_t fuzz_read(void *, unsigned char *, size_t, size_t *,
    fsimg_error_t *);
static fsimg_status_t fuzz_entry(void *, const fsimg_mtree_entry_t *,
    fsimg_error_t *);
static void check_entry(fsimg_ctx_t *, const fsimg_mtree_entry_t *);
static void fold(uint64_t *, const void *, size_t);
static void fold_entry(uint64_t *, const fsimg_mtree_entry_t *);
static bool relative_path_safe(const char *);
static void exercise(const uint8_t *, size_t, size_t, fuzz_outcome_t *);
int LLVMFuzzerTestOneInput(const uint8_t *, size_t);

static fsimg_status_t
fuzz_read(void *argument, unsigned char *buffer, size_t capacity,
    size_t *length, fsimg_error_t *error FSIMG_UNUSED)
{
	fuzz_input_t *input = argument;
	size_t count;

	count = input->size - input->offset;
	if (count > capacity)
		count = capacity;
	if (count > input->chunk)
		count = input->chunk;
	memcpy(buffer, input->data + input->offset, count);
	input->offset += count;
	*length = count;
	return (FSIMG_OK);
}

/* Whether path stays beneath the directory it is resolved in: it is
 * relative and every '/'-separated component is a name, not empty, "."
 * or "..". Written apart from the parser's own check.
 */
static bool
relative_path_safe(const char *path)
{
	const char *component, *end;
	size_t length;

	component = path;
	for (;;) {
		end = strchr(component, '/');
		length = end == NULL ? strlen(component) :
		    (size_t)(end - component);
		if (length == 0 || (length == 1 && component[0] == '.') ||
		    (length == 2 && component[0] == '.' && component[1] == '.'))
			return (false);
		if (end == NULL)
			return (true);
		component = end + 1;
	}
}

static void
check_entry(fsimg_ctx_t *ctx, const fsimg_mtree_entry_t *entry)
{
	const fsimg_mtree_hole_t *hole;
	fsimg_error_t error;
	size_t length;
	uint64_t end;
	uint32_t depth;

	if (fsimg_path_validate(ctx, entry->entry.path, false, &length,
	    &depth, &error) != FSIMG_OK)
		abort();
	if ((entry->link != NULL) != (entry->entry.kind == FSIMG_ENTRY_SYMLINK))
		abort();
	if (entry->link != NULL && (entry->link_length == 0 ||
	    entry->link_length > FSIMG_SYMLINK_TARGET_MAX ||
	    strlen(entry->link) != entry->link_length ||
	    entry->entry.size != entry->link_length))
		abort();
	if (entry->contents != NULL && (entry->entry.kind != FSIMG_ENTRY_FILE ||
	    entry->content != FSIMG_MTREE_CONTENT_HOST ||
	    strlen(entry->contents) != entry->contents_length ||
	    !relative_path_safe(entry->contents)))
		abort();
	if (entry->content != FSIMG_MTREE_CONTENT_HOST && !entry->size_given)
		abort();
	if (entry->hole_count > FSIMG_MTREE_HOLES_MAX)
		abort();
	end = 0;
	for (size_t i = 0; i < entry->hole_count; i++) {
		hole = &entry->holes[i];
		if (hole->length == 0 || (i > 0 && hole->offset <= end) ||
		    !fsimg_add_u64(hole->offset, hole->length, &end) ||
		    end > entry->entry.size)
			abort();
	}
	if ((entry->entry.metadata.valid & FSIMG_META_MODE) != 0 &&
	    entry->entry.metadata.mode > FSIMG_MODE_BITS)
		abort();
	if (fsimg_metadata_validate(&entry->entry.metadata, &error) != FSIMG_OK)
		abort();
}

/* FNV-1a over bytes, so the two parses can be compared value for value. */
static void
fold(uint64_t *hash, const void *bytes, size_t length)
{
	const unsigned char *byte = bytes;

	for (size_t i = 0; i < length; i++) {
		*hash ^= byte[i];
		*hash *= UINT64_C(0x100000001b3);
	}
}

/* Every value the entry carries, field by field, never padding. */
static void
fold_entry(uint64_t *hash, const fsimg_mtree_entry_t *entry)
{
	const fsimg_metadata_t *metadata = &entry->entry.metadata;

	fold(hash, entry->entry.path, strlen(entry->entry.path) + 1);
	fold(hash, &entry->entry.kind, sizeof(entry->entry.kind));
	fold(hash, &entry->entry.size, sizeof(entry->entry.size));
	fold(hash, &metadata->valid, sizeof(metadata->valid));
	fold(hash, &metadata->uid, sizeof(metadata->uid));
	fold(hash, &metadata->gid, sizeof(metadata->gid));
	fold(hash, &metadata->mode, sizeof(metadata->mode));
	fold(hash, &metadata->modified.seconds,
	    sizeof(metadata->modified.seconds));
	fold(hash, &metadata->modified.nanoseconds,
	    sizeof(metadata->modified.nanoseconds));
	if (entry->link != NULL)
		fold(hash, entry->link, entry->link_length + 1);
	if (entry->contents != NULL)
		fold(hash, entry->contents, entry->contents_length + 1);
	fold(hash, &entry->content, sizeof(entry->content));
	fold(hash, &entry->seed, sizeof(entry->seed));
	for (size_t i = 0; i < entry->hole_count; i++) {
		fold(hash, &entry->holes[i].offset, sizeof(uint64_t));
		fold(hash, &entry->holes[i].length, sizeof(uint64_t));
	}
	fold(hash, &entry->size_given, sizeof(entry->size_given));
	fold(hash, &entry->digest_given, sizeof(entry->digest_given));
	fold(hash, entry->digest, sizeof(entry->digest));
}

static fsimg_status_t
fuzz_entry(void *argument, const fsimg_mtree_entry_t *entry,
    fsimg_error_t *error FSIMG_UNUSED)
{
	fuzz_outcome_t *outcome = argument;

	check_entry(outcome->ctx, entry);
	outcome->entries++;
	fold_entry(&outcome->fingerprint, entry);
	return (FSIMG_OK);
}

static void
exercise(const uint8_t *data, size_t size, size_t chunk,
    fuzz_outcome_t *outcome)
{
	fuzz_input_t input = { .data = data, .size = size, .chunk = chunk };
	fsimg_mtree_input_t callbacks = { .read = fuzz_read,
	    .read_argument = &input, .entry = fuzz_entry,
	    .entry_argument = outcome };
	fsimg_options_t options;
	fsimg_session_t session;
	fsimg_stats_t baseline, stats;
	fsimg_error_t error;

	fsimg_options_init(&options);
	options.memory_bytes = 16 * 1024 * 1024;
	options.max_entries = 1000;
	options.max_depth = 8;
	options.max_path_bytes = 256;
	options.max_work = 100000;
	bzero(outcome, sizeof(*outcome));
	outcome->fingerprint = UINT64_C(0xcbf29ce484222325);
	if (fsimg_ctx_new(&options, &outcome->ctx, &error) != FSIMG_OK)
		abort();
	if (fsimg_ctx_get_stats(outcome->ctx, &baseline, &error) != FSIMG_OK)
		abort();
	fsimg_session_init(&session, outcome->ctx, "fuzz", 0);
	outcome->status = fsimg_mtree_parse(&session, &callbacks, &error);
	if (outcome->status != FSIMG_OK && (error.status != outcome->status ||
	    !error.offset_valid || error.offset > size))
		abort();
	if (outcome->status != FSIMG_OK)
		fold(&outcome->fingerprint, &error.offset, sizeof(error.offset));
	if (fsimg_ctx_get_stats(outcome->ctx, &stats, &error) != FSIMG_OK ||
	    stats.memory_bytes != baseline.memory_bytes)
		abort();
	fsimg_ctx_free(outcome->ctx);
	outcome->ctx = NULL;
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	fuzz_outcome_t whole, pieces;

	exercise(data, size, SIZE_MAX, &whole);
	exercise(data, size, 13, &pieces);
	if (whole.status != pieces.status || whole.entries != pieces.entries ||
	    whole.fingerprint != pieces.fingerprint)
		abort();
	return (0);
}
