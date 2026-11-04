/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fsimg.h"
#include "hfsplus.h"
#include "host.h"
#include "selection.h"

/* The complete input is the volume. Public reader operations exercise
 * probing and decoding; private APIs expose name codecs and path selection.
 * No host files or extraction destinations are created by the harness.
 */
typedef struct fuzz_input {
	const uint8_t	*data;
	size_t		 size;
} fuzz_input_t;

static fsimg_status_t fuzz_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t fuzz_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static void exercise_names(const uint8_t *, size_t);
static void exercise(const uint8_t *, size_t, const char *, uint32_t);
static void read_content(fsimg_reader_t *, const fsimg_entry_t *, size_t *);
int LLVMFuzzerTestOneInput(const uint8_t *, size_t);

static fsimg_status_t
fuzz_size(void *argument, uint64_t *out, fsimg_error_t *error FSIMG_UNUSED)
{
	*out = ((fuzz_input_t *)argument)->size;
	return (FSIMG_OK);
}

static fsimg_status_t
fuzz_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *count, fsimg_error_t *error FSIMG_UNUSED)
{
	fuzz_input_t *input = argument;

	if (offset > input->size || length > input->size - offset)
		abort();
	if (length != 0)
		bcopy(input->data + (size_t)offset, buffer, length);
	*count = length;
	return (FSIMG_OK);
}

static void
exercise_names(const uint8_t *data, size_t size)
{
	uint16_t units[HFSPLUS_NAME_UNITS_MAX];
	uint16_t decomposed[HFSPLUS_NAME_UNITS_MAX * HFSPLUS_UNIT_DECOMP_MAX];
	unsigned char key[HFSPLUS_KEY_BYTES_MAX];
	char path[HFSPLUS_NAME_UTF8_MAX];
	size_t count, written, query_bytes;

	count = size / 2;
	if (count > HFSPLUS_NAME_UNITS_MAX)
		count = HFSPLUS_NAME_UNITS_MAX;
	for (size_t i = 0; i < count; i++) {
		units[i] = (uint16_t)((uint16_t)data[2 * i] << 8 |
		    data[2 * i + 1]);
		hfsplus_fold_unit(units[i]);
		hfsplus_combining_class(units[i]);
		written = hfsplus_unit_decompose(units[i], decomposed);
		if (written == 0 || written > HFSPLUS_UNIT_DECOMP_MAX)
			abort();
	}
	hfsplus_decompose(units, count, decomposed,
	    sizeof(decomposed) / sizeof(decomposed[0]), &written);
	hfsplus_name_admit(data, count, NULL);
	if (hfsplus_name_to_path(data, count, path, &written, NULL) == FSIMG_OK &&
	    written > sizeof(path))
		abort();
	query_bytes = size < sizeof(path) ? size : sizeof(path);
	for (unsigned mode = 0; mode < 2; mode++) {
		hfsplus_compare_t compare = (hfsplus_compare_t)mode;

		if (hfsplus_name_compare(compare, data, count, data, count) != 0)
			abort();
		/* Compare independent halves, including unequal lengths. */
		hfsplus_name_compare(compare, data, count / 2,
		    data + 2 * (count / 2), count - count / 2);
		hfsplus_stored_key(compare, data, count, key, &written, NULL);
		hfsplus_query_key(compare, (const char *)data, query_bytes,
		    key, &written, NULL);
	}
}

/* A separate byte allowance bounds content even when extents are shared.
 * Every documented error, including unsupported compression, ends only
 * the attempted operation. Successful reads and ranges must make progress.
 */
static void
read_content(fsimg_reader_t *reader, const fsimg_entry_t *entry, size_t *budget)
{
	unsigned char bytes[4096];
	fsimg_range_t range;
	uint64_t offset = entry->size > 65536 ? 65536 - 32 : 0;
	bool seek_back = offset != 0;
	size_t count, length;

	if (entry->kind != FSIMG_ENTRY_FILE)
		return;
	while (offset < entry->size && *budget != 0) {
		if (fsimg_reader_next_range(reader, offset, &range, NULL) != FSIMG_OK)
			break;
		if (range.offset != offset || range.length == 0 ||
		    range.length > entry->size - offset)
			abort();
		length = *budget < sizeof(bytes) ? *budget : sizeof(bytes);
		if (fsimg_reader_read_at(reader, offset, bytes, length, &count,
		    NULL) != FSIMG_OK)
			break;
		if (count == 0 || count > length || count > entry->size - offset)
			abort();
		offset += count;
		*budget -= count;
		if (seek_back && *budget != 0) {
			offset = 0;
			seek_back = false;
		}
	}
}

static void
exercise(const uint8_t *data, size_t size, const char *format, uint32_t flags)
{
	static const char *const queries[] = { "/dir/f00", "/a", "/missing" };
	fuzz_input_t input = { .data = data, .size = size };
	fsimg_options_t options;
	fsimg_read_options_t read_options;
	fsimg_io_ops_t ops;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	fsimg_reader_t *reader;
	fsimg_selection_t *selection;
	fsimg_stats_t baseline, stats;
	const fsimg_entry_t *entry;
	char paths[4][4096];
	const char *member;
	size_t path_count = 0, budget = 16 * 1024 * 1024;

	fsimg_options_init(&options);
	options.memory_bytes = 16 * 1024 * 1024;
	options.max_entries = 1000;
	options.max_depth = 16;
	options.max_path_bytes = sizeof(paths[0]);
	options.max_work = 250000;
	if (fsimg_ctx_new(&options, &ctx, NULL) != FSIMG_OK)
		return;
	fsimg_ctx_get_stats(ctx, &baseline, NULL);
	fsimg_io_ops_init(&ops);
	ops.get_size = fuzz_size;
	ops.read_at = fuzz_read;
	if (fsimg_io_from_callbacks(ctx, &ops, &input, &io, NULL) != FSIMG_OK) {
		fsimg_ctx_free(ctx);
		return;
	}
	fsimg_read_options_init(&read_options);
	read_options.format = format;
	read_options.flags = flags;
	if (fsimg_reader_open(ctx, io, &read_options, &reader, NULL) == FSIMG_OK) {
		while (fsimg_reader_next(reader, &entry, NULL) == FSIMG_OK) {
			if (path_count < 4) {
				size_t length = strlen(entry->path);

				if (length >= sizeof(paths[0]))
					abort();
				bcopy(entry->path, paths[path_count], length + 1);
				path_count++;
			}
			read_content(reader, entry, &budget);
		}
		fsimg_reader_free(reader);
	}
	/* Selection resolves components using the driver's validated namespace.
	 * Each lookup needs a fresh reader because selection owns traversal.
	 */
	for (size_t i = 0; i < path_count + 3; i++) {
		member = i < path_count ? paths[i] : queries[i - path_count];
		if (fsimg_reader_open(ctx, io, &read_options, &reader, NULL) != FSIMG_OK)
			continue;
		if (fsimg_selection_new(reader, &member, 1, FSIMG_SELECTION_TREE,
		    &selection, NULL) == FSIMG_OK) {
			while (fsimg_selection_next(selection, &entry, NULL) == FSIMG_OK)
				read_content(reader, entry, &budget);
			fsimg_selection_free(selection);
		}
		fsimg_reader_free(reader);
	}
	fsimg_io_free(io);
	fsimg_ctx_get_stats(ctx, &stats, NULL);
	if (stats.memory_bytes != baseline.memory_bytes ||
	    stats.scratch_bytes != 0 || stats.descriptors != 0)
		abort();
	fsimg_ctx_free(ctx);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static const char *const variants[] = { "hfsplus", "hfsx" };

	exercise_names(data, size);
	exercise(data, size, NULL, 0);
	for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); i++) {
		exercise(data, size, variants[i], 0);
		exercise(data, size, variants[i], FSIMG_READ_ASSUME_IDENTITY);
	}
	return (0);
}
