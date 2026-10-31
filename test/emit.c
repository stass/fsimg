/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "emit.h"
#include "format.h"
#include "test.h"
#include "write-exfat.h"
#include "write.h"

/* Common emission tests. Every driver copies payloads through the one
 * emission object, so each scenario runs against FAT and exFAT: content with
 * empty, zero-only and mixed data/zero sources on clean and stale sinks, and
 * every failure path, which must leave the output incomplete, the source
 * closed and no resources charged.
 */

enum {
	FILE_EMPTY = 1,		/* provider source, no bytes */
	FILE_ZEROS,		/* zero source */
	FILE_SPARSE,		/* provider source that is one ZERO range */
	FILE_MIXED,		/* provider source: DATA, ZERO, DATA */
	FILE_DIRECTORY,
	FILE_LARGE,		/* provider source, DATA spanning many batches */
	FILE_COUNT = FILE_LARGE
};

#define MIXED_ZERO_START 70000u
#define MIXED_ZERO_END 140000u
#define EXFAT_VOLUME_BYTES (UINT64_C(4) * 1024 * 1024)

/* Indexed by provider source ID; index 0 is unused. */
static const struct {
	const char	*path;
	uint64_t	 size;
} tree[] = {
	[FILE_EMPTY] = { "/empty", 0 },
	[FILE_ZEROS] = { "/zeros", 70001 },
	[FILE_SPARSE] = { "/sparse", 50000 },
	[FILE_MIXED] = { "/mixed", 200000 },
	[FILE_DIRECTORY] = { "/dir", 0 },
	[FILE_LARGE] = { "/dir/large", 300000 },
};

typedef struct emit_fixture {
	fsimg_ctx_t	*ctx;
	fsimg_writer_t	*writer;
	fsimg_stats_t	 baseline;
	char		 directory[64];
	int		 parent;
	/* Provider observations. */
	uint64_t	 zero_range_reads;
	bool		 read_seen[FILE_COUNT + 1];
	/* Faults: 0 disables each. */
	uint64_t	 changed_id;
	bool		 changed_after_read;
	uint64_t	 short_read_id;
	uint64_t	 polls;
	uint64_t	 cancel_at_poll;
	uint64_t	 fail_alloc_at;
} emit_fixture_t;

typedef struct memory_sink {
	unsigned char	*bytes;
	uint64_t	 length;
	uint64_t	 writes;
	/* Faults: 0 disables each. The failing write confirms half its bytes. */
	uint64_t	 fail_write;
	uint64_t	 confirmed_end;
	size_t		 max_transfer;
	bool		 fail_flush;
	bool		 no_zero_range;
} memory_sink_t;

static unsigned char file_byte(uint64_t, uint64_t);
static bool file_zero_at(uint64_t, uint64_t);
static fsimg_status_t provider_validate(void *, uint64_t, fsimg_error_t *);
static fsimg_status_t provider_read(void *, uint64_t, uint64_t, void *,
    size_t, size_t *, fsimg_error_t *);
static fsimg_status_t provider_range(void *, uint64_t, uint64_t,
    fsimg_range_t *, fsimg_error_t *);
static bool cancelled(void *);
static fsimg_status_t sink_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t sink_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static fsimg_status_t sink_write(void *, uint64_t, const void *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t sink_zero(void *, uint64_t, uint64_t, fsimg_error_t *);
static fsimg_status_t sink_flush(void *, fsimg_error_t *);
static void fixture_open(emit_fixture_t *, const char *);
static void fixture_close(emit_fixture_t *);
static void sink_open(emit_fixture_t *, memory_sink_t *, unsigned char);
static fsimg_io_t *sink_io(emit_fixture_t *, memory_sink_t *, bool);
static fsimg_status_t emit_to(emit_fixture_t *, memory_sink_t *,
    fsimg_error_t *);
static void check_image(emit_fixture_t *, memory_sink_t *);
static void check_failed(emit_fixture_t *, memory_sink_t *, fsimg_status_t,
    fsimg_status_t, const fsimg_error_t *, const char *);
static void test_content(const char *);
static void test_source_faults(const char *);
static void test_sink_faults(const char *);
static void test_allocation_failures(const char *);
static uint64_t catalog_id(emit_fixture_t *, const char *);
static void test_corrupt_plans(const char *);
static fsimg_status_t copy_case_emit(fsimg_writer_t *, fsimg_emit_t *,
    fsimg_error_t *);
static void place_regions(const uint64_t *, size_t, uint64_t,
    fsimg_emit_region_t *);
static void check_placed(const memory_sink_t *, uint64_t,
    const fsimg_emit_payload_t *);
static void observe_copy(void *, const unsigned char *, size_t, uint64_t);
static void check_observed(const memory_sink_t *, uint64_t,
    const fsimg_emit_payload_t *);
static void test_direct_copies(void);

/* The expected payload: a byte pattern that differs per file, with zeros
 * wherever the file's ranges are ZERO.
 */
static bool
file_zero_at(uint64_t id, uint64_t offset)
{
	return (id == FILE_SPARSE || (id == FILE_MIXED &&
	    offset >= MIXED_ZERO_START && offset < MIXED_ZERO_END));
}

static unsigned char
file_byte(uint64_t id, uint64_t offset)
{
	if (id == FILE_ZEROS || file_zero_at(id, offset))
		return (0);
	return ((unsigned char)(offset * 31 + id * 7 + 1));
}

static fsimg_status_t
provider_validate(void *argument, uint64_t id, fsimg_error_t *error)
{
	emit_fixture_t *fixture = argument;

	if (id != fixture->changed_id)
		return (FSIMG_OK);
	if (fixture->changed_after_read && !fixture->read_seen[id])
		return (FSIMG_OK);
	return (fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
	    FSIMG_OP_SOURCE, "test source changed"));
}

static fsimg_status_t
provider_read(void *argument, uint64_t id, uint64_t offset, void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	emit_fixture_t *fixture = argument;
	unsigned char *bytes = buffer;

	*transferred = 0;
	CHECK(id >= 1 && id <= FILE_COUNT);
	fixture->read_seen[id] = true;
	/* A source that ends early reports EOF partway through the file. */
	if (id == fixture->short_read_id && offset >= 65536)
		return (FSIMG_OK);
	for (size_t i = 0; i < length; i++) {
		if (file_zero_at(id, offset + i))
			fixture->zero_range_reads++;
		bytes[i] = file_byte(id, offset + i);
	}
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
provider_range(void *argument FSIMG_UNUSED, uint64_t id, uint64_t offset,
    fsimg_range_t *range, fsimg_error_t *error FSIMG_UNUSED)
{
	uint64_t end;

	CHECK(id >= 1 && id <= FILE_COUNT);
	end = tree[id].size;
	range->offset = offset;
	range->kind = file_zero_at(id, offset) ? FSIMG_RANGE_ZERO :
	    FSIMG_RANGE_DATA;
	if (id == FILE_MIXED && offset < MIXED_ZERO_START)
		end = MIXED_ZERO_START;
	else if (id == FILE_MIXED && offset < MIXED_ZERO_END)
		end = MIXED_ZERO_END;
	range->length = end - offset;
	return (FSIMG_OK);
}

static bool
cancelled(void *argument)
{
	emit_fixture_t *fixture = argument;

	fixture->polls++;
	return (fixture->cancel_at_poll != 0 &&
	    fixture->polls >= fixture->cancel_at_poll);
}

static fsimg_status_t
sink_size(void *argument, uint64_t *length, fsimg_error_t *error FSIMG_UNUSED)
{
	*length = ((memory_sink_t *)argument)->length;
	return (FSIMG_OK);
}

static fsimg_status_t
sink_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_sink_t *sink = argument;

	CHECK(fsimg_range_fits(offset, length, sink->length));
	bcopy(sink->bytes + offset, buffer, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
sink_write(void *argument, uint64_t offset, const void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_sink_t *sink = argument;

	CHECK(fsimg_range_fits(offset, length, sink->length));
	sink->writes++;
	if (sink->max_transfer != 0 && length > sink->max_transfer)
		length = sink->max_transfer;
	if (sink->writes == sink->fail_write)
		length /= 2;
	bcopy(buffer, sink->bytes + offset, length);
	*transferred = length;
	if (sink->writes == sink->fail_write) {
		sink->confirmed_end = offset + length;
		return (FSIMG_EIO);
	}
	return (FSIMG_OK);
}

static fsimg_status_t
sink_zero(void *argument, uint64_t offset, uint64_t length,
    fsimg_error_t *error FSIMG_UNUSED)
{
	memory_sink_t *sink = argument;

	CHECK(fsimg_range_fits(offset, length, sink->length));
	bzero(sink->bytes + offset, (size_t)length);
	return (FSIMG_OK);
}

static fsimg_status_t
sink_flush(void *argument, fsimg_error_t *error FSIMG_UNUSED)
{
	return (((memory_sink_t *)argument)->fail_flush ? FSIMG_EIO : FSIMG_OK);
}

/* Open a context and a reproducible writer collecting the test tree, with
 * the provider and cancellation hooks bound to the fixture.
 */
static void
fixture_open(emit_fixture_t *fixture, const char *format)
{
	fsimg_options_t options;
	fsimg_write_options_t write_options;
	fsimg_source_ops_t source_ops;
	fsimg_entry_t entry;
	fsimg_source_t source;

	bzero(fixture, sizeof(*fixture));
	snprintf(fixture->directory, sizeof(fixture->directory), "emit-XXXXXX");
	CHECK(mkdtemp(fixture->directory) != NULL);
	fixture->parent = open(fixture->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fixture->parent != -1);
	fsimg_options_init(&options);
	options.memory_bytes = UINT64_C(16) * 1024 * 1024;
	options.scratch_directory_fd = fixture->parent;
	options.max_open_files = 16;
	options.cancelled = cancelled;
	options.cancel_argument = fixture;
	CHECK(fsimg_ctx_new(&options, &fixture->ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &fixture->baseline, NULL) == FSIMG_OK);

	fsimg_write_options_init(&write_options);
	write_options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	if (strcmp(format, "exfat") == 0)
		write_options.volume_bytes = EXFAT_VOLUME_BYTES;
	CHECK(fsimg_writer_new(fixture->ctx, format, &write_options,
	    &fixture->writer, NULL) == FSIMG_OK);
	fsimg_source_ops_init(&source_ops);
	source_ops.validate = provider_validate;
	source_ops.read_at = provider_read;
	source_ops.next_range = provider_range;
	CHECK(fsimg_writer_set_sources(fixture->writer, &source_ops, fixture,
	    NULL) == FSIMG_OK);

	for (uint64_t id = 1; id <= FILE_COUNT; id++) {
		bzero(&entry, sizeof(entry));
		entry.path = tree[id].path;
		entry.size = tree[id].size;
		if (id == FILE_DIRECTORY) {
			entry.kind = FSIMG_ENTRY_DIRECTORY;
			CHECK(fsimg_writer_add(fixture->writer, &entry, NULL,
			    NULL) == FSIMG_OK);
			continue;
		}
		entry.kind = FSIMG_ENTRY_FILE;
		source.kind = id == FILE_ZEROS ? FSIMG_SOURCE_ZERO :
		    FSIMG_SOURCE_PROVIDER;
		source.id = id == FILE_ZEROS ? 0 : id;
		CHECK(fsimg_writer_add(fixture->writer, &entry, &source,
		    NULL) == FSIMG_OK);
	}
}

/* Every outcome, including failure, must return all memory, scratch and
 * descriptors charged to the context.
 */
static void
fixture_close(emit_fixture_t *fixture)
{
	fsimg_stats_t stats;

	fsimg_writer_free(fixture->writer);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == fixture->baseline.memory_bytes);
	CHECK(stats.scratch_bytes == 0);
	CHECK(stats.descriptors == fixture->baseline.descriptors);
	fsimg_ctx_free(fixture->ctx);
	CHECK(close(fixture->parent) == 0);
	CHECK(rmdir(fixture->directory) == 0);
}

/* Plan the image and allocate a sink of its exact size filled with fill;
 * a nonzero fill stands for stale bytes in a reused sink.
 */
static void
sink_open(emit_fixture_t *fixture, memory_sink_t *sink, unsigned char fill)
{
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;

	CHECK(fsimg_writer_plan(fixture->writer, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	sink->length = info.volume_bytes;
	sink->bytes = malloc((size_t)sink->length);
	CHECK(sink->bytes != NULL);
	memset(sink->bytes, fill, (size_t)sink->length);
}

static fsimg_io_t *
sink_io(emit_fixture_t *fixture, memory_sink_t *sink, bool writable)
{
	fsimg_io_ops_t ops;
	fsimg_io_t *io;

	fsimg_io_ops_init(&ops);
	ops.get_size = sink_size;
	ops.read_at = sink_read;
	if (writable) {
		ops.write_at = sink_write;
		ops.flush = sink_flush;
		if (!sink->no_zero_range)
			ops.zero_range = sink_zero;
	}
	CHECK(fsimg_io_from_callbacks(fixture->ctx, &ops, sink, &io, NULL) == FSIMG_OK);
	return (io);
}

/* Emit the planned image into sink. Poll counting and allocation failure
 * apply to the emission only, not to planning or the test's sink adapter.
 */
static fsimg_status_t
emit_to(emit_fixture_t *fixture, memory_sink_t *sink, fsimg_error_t *error)
{
	fsimg_io_t *output;
	fsimg_status_t status;

	output = sink_io(fixture, sink, true);
	fixture->polls = 0;
	fsimg_test_fail_alloc_after(fixture->ctx, fixture->fail_alloc_at);
	status = fsimg_writer_emit(fixture->writer, output, error);
	fsimg_test_fail_alloc_after(fixture->ctx, 0);
	fsimg_io_free(output);
	return (status);
}

/* Read every file back through the reader and compare it with file_byte(),
 * an oracle independent of the copy loop; then verify the payload.
 */
static void
check_image(emit_fixture_t *fixture, memory_sink_t *sink)
{
	fsimg_reader_t *reader;
	fsimg_io_t *input;
	const fsimg_entry_t *entry;
	fsimg_verify_report_t report;
	unsigned char buffer[8192];
	uint64_t id, offset, files;
	size_t length;
	fsimg_status_t status;

	input = sink_io(fixture, sink, false);
	CHECK(fsimg_reader_open(fixture->ctx, input, NULL, &reader, NULL) == FSIMG_OK);
	files = 0;
	while ((status = fsimg_reader_next(reader, &entry, NULL)) == FSIMG_OK) {
		if (entry->kind != FSIMG_ENTRY_FILE)
			continue;
		for (id = 1; id <= FILE_COUNT; id++) {
			if (strcmp(entry->path, tree[id].path) == 0)
				break;
		}
		CHECK(id <= FILE_COUNT && entry->size == tree[id].size);
		files++;
		for (offset = 0; offset < entry->size; offset += length) {
			CHECK(fsimg_reader_read_at(reader, offset, buffer,
			    sizeof(buffer), &length, NULL) == FSIMG_OK);
			CHECK(length > 0);
			for (size_t i = 0; i < length; i++)
				CHECK(buffer[i] == file_byte(id, offset + i));
		}
	}
	CHECK(status == FSIMG_DONE && files == FILE_COUNT - 1);
	fsimg_reader_free(reader);
	CHECK(fsimg_reader_open(fixture->ctx, input, NULL, &reader, NULL) == FSIMG_OK);
	CHECK(fsimg_verify(reader, FSIMG_VERIFY_PAYLOAD, &report, NULL) == FSIMG_OK);
	CHECK(report.complete && report.files == FILE_COUNT - 1);
	fsimg_reader_free(reader);
	fsimg_io_free(input);
}

/* A failed emission reports the expected status and an incomplete output,
 * leaves no source open, and refuses a second attempt.
 */
static void
check_failed(emit_fixture_t *fixture, memory_sink_t *sink,
    fsimg_status_t status, fsimg_status_t expected, const fsimg_error_t *error,
    const char *scenario)
{
	if (status != expected)
		fprintf(stderr, "%s: status %d, expected %d: %s\n", scenario,
		    status, expected, error->message);
	CHECK(status == expected);
	CHECK(error->output_state == FSIMG_OUTPUT_INCOMPLETE);
	CHECK(!fixture->writer->source.active);
	CHECK(fixture->writer->state == FSIMG_WRITER_FAILED);
	fixture->fail_alloc_at = 0;
	CHECK(emit_to(fixture, sink, NULL) == FSIMG_ESTATE);
}

/* The same tree emitted into a clean sink, stale sinks with and without a
 * zero-range capability, and a sink that accepts short writes must produce
 * identical images whose files read back exactly. ZERO ranges are never read.
 */
static void
test_content(const char *format)
{
	static const struct {
		const char	*name;
		unsigned char	 fill;
		bool		 no_zero_range;
		size_t		 max_transfer;
	} sinks[] = {
		{ .name = "clean", .fill = 0 },
		{ .name = "stale", .fill = 0xa5 },
		{ .name = "stale without zero_range", .fill = 0x5a,
		    .no_zero_range = true },
		{ .name = "short writes", .fill = 0xa5, .max_transfer = 1000 },
	};
	emit_fixture_t fixture;
	memory_sink_t first, sink;
	fsimg_error_t error;
	fsimg_status_t status;

	bzero(&first, sizeof(first));
	for (size_t i = 0; i < sizeof(sinks) / sizeof(sinks[0]); i++) {
		fixture_open(&fixture, format);
		bzero(&sink, sizeof(sink));
		sink.no_zero_range = sinks[i].no_zero_range;
		sink.max_transfer = sinks[i].max_transfer;
		sink_open(&fixture, &sink, sinks[i].fill);
		status = emit_to(&fixture, &sink, &error);
		if (status != FSIMG_OK)
			fprintf(stderr, "%s %s: %d %s\n", format, sinks[i].name,
			    status, error.message);
		CHECK(status == FSIMG_OK);
		CHECK(fixture.zero_range_reads == 0);
		CHECK(!fixture.writer->source.active);
		check_image(&fixture, &sink);
		fixture_close(&fixture);
		if (i == 0) {
			first = sink;
			continue;
		}
		CHECK(sink.length == first.length);
		CHECK(bcmp(sink.bytes, first.bytes, (size_t)sink.length) == 0);
		free(sink.bytes);
	}
	free(first.bytes);
}

/* Source changes are detected even where no bytes move: an empty file and a
 * zero-only file are still opened and validated.
 */
static void
test_source_faults(const char *format)
{
	static const struct {
		const char	*name;
		uint64_t	 changed_id;
		bool		 changed_after_read;
		uint64_t	 short_read_id;
		fsimg_status_t	 expected;
	} cases[] = {
		{ .name = "changed empty source", .changed_id = FILE_EMPTY,
		    .expected = FSIMG_ESOURCE_CHANGED },
		{ .name = "changed zero-only source", .changed_id = FILE_SPARSE,
		    .expected = FSIMG_ESOURCE_CHANGED },
		{ .name = "source changed mid-copy", .changed_id = FILE_LARGE,
		    .changed_after_read = true, .expected = FSIMG_ESOURCE_CHANGED },
		{ .name = "source ends early", .short_read_id = FILE_LARGE,
		    .expected = FSIMG_EIO },
	};
	emit_fixture_t fixture;
	memory_sink_t sink;
	fsimg_error_t error;
	fsimg_status_t status;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		fixture_open(&fixture, format);
		bzero(&sink, sizeof(sink));
		sink_open(&fixture, &sink, 0xa5);
		fixture.changed_id = cases[i].changed_id;
		fixture.changed_after_read = cases[i].changed_after_read;
		fixture.short_read_id = cases[i].short_read_id;
		status = emit_to(&fixture, &sink, &error);
		check_failed(&fixture, &sink, status, cases[i].expected, &error,
		    cases[i].name);
		CHECK(fixture.zero_range_reads == 0);
		fixture_close(&fixture);
		free(sink.bytes);
	}
}

/* Write failures at the first, a middle and the last write; cancellation at
 * the first, a middle and the last poll; and a failed flush. A failed write
 * confirms half its bytes, and the error must report that confirmed end.
 */
static void
test_sink_faults(const char *format)
{
	emit_fixture_t fixture;
	memory_sink_t sink;
	fsimg_error_t error;
	fsimg_status_t status;
	uint64_t writes, polls, points[3];

	fixture_open(&fixture, format);
	bzero(&sink, sizeof(sink));
	sink_open(&fixture, &sink, 0);
	CHECK(emit_to(&fixture, &sink, NULL) == FSIMG_OK);
	writes = sink.writes;
	polls = fixture.polls;
	fixture_close(&fixture);
	free(sink.bytes);
	CHECK(writes > 2 && polls > 2);

	points[0] = 1;
	points[1] = writes / 2;
	points[2] = writes;
	for (size_t i = 0; i < 3; i++) {
		fixture_open(&fixture, format);
		bzero(&sink, sizeof(sink));
		sink_open(&fixture, &sink, 0xa5);
		sink.fail_write = points[i];
		status = emit_to(&fixture, &sink, &error);
		check_failed(&fixture, &sink, status, FSIMG_EIO, &error, "write failure");
		CHECK(error.offset_valid && error.offset == sink.confirmed_end);
		fixture_close(&fixture);
		free(sink.bytes);
	}

	points[0] = 1;
	points[1] = polls / 2;
	points[2] = polls;
	for (size_t i = 0; i < 3; i++) {
		fixture_open(&fixture, format);
		bzero(&sink, sizeof(sink));
		sink_open(&fixture, &sink, 0xa5);
		fixture.cancel_at_poll = points[i];
		status = emit_to(&fixture, &sink, &error);
		check_failed(&fixture, &sink, status, FSIMG_ECANCELLED, &error,
		    "cancellation");
		fixture_close(&fixture);
		free(sink.bytes);
	}

	fixture_open(&fixture, format);
	bzero(&sink, sizeof(sink));
	sink_open(&fixture, &sink, 0xa5);
	sink.fail_flush = true;
	status = emit_to(&fixture, &sink, &error);
	check_failed(&fixture, &sink, status, FSIMG_EIO, &error, "flush failure");
	fixture_close(&fixture);
	free(sink.bytes);
}

/* Fail each allocation made during emission in turn until one succeeds. */
static void
test_allocation_failures(const char *format)
{
	emit_fixture_t fixture;
	memory_sink_t sink;
	fsimg_error_t error;
	fsimg_status_t status;
	bool succeeded;

	succeeded = false;
	for (uint64_t attempt = 1; attempt < 1024 && !succeeded; attempt++) {
		fixture_open(&fixture, format);
		bzero(&sink, sizeof(sink));
		sink_open(&fixture, &sink, 0xa5);
		fixture.fail_alloc_at = attempt;
		status = emit_to(&fixture, &sink, &error);
		if (status == FSIMG_OK) {
			succeeded = true;
			check_image(&fixture, &sink);
		} else
			check_failed(&fixture, &sink, status, FSIMG_ENOMEM, &error,
			    "allocation failure");
		fixture_close(&fixture);
		free(sink.bytes);
	}
	CHECK(succeeded);
}

static uint64_t
catalog_id(emit_fixture_t *fixture, const char *path)
{
	const fsimg_catalog_entry_t *catalog;

	for (uint64_t id = 1; id <= fixture->writer->count; id++) {
		CHECK(fsimg_catalog_get(fixture->writer, id, &catalog, NULL) == FSIMG_OK);
		if (strcmp(catalog->entry.path, path) == 0)
			return (id);
	}
	CHECK(!"catalog path not found");
	return (0);
}

/* Plan records whose payload no longer fits its allocation or the volume are
 * rejected by the copy operation instead of being written. Metadata emission
 * encodes these fields but does not validate them, so the copy is the check
 * that fires.
 */
static void
test_corrupt_plans(const char *format)
{
	enum { LENGTH_PAST_ALLOCATION, OUTSIDE_VOLUME, INITIALIZED_PAST_LENGTH };
	static const char *const names[] = { "length past allocation",
	    "outside volume", "initialized past length" };
	emit_fixture_t fixture;
	memory_sink_t sink;
	fat_layout_t fat;
	exfat_layout_t exfat;
	fsimg_error_t error;
	fsimg_status_t status;
	uint64_t id;
	bool is_exfat;

	is_exfat = strcmp(format, "exfat") == 0;
	for (unsigned c = LENGTH_PAST_ALLOCATION; c <= INITIALIZED_PAST_LENGTH; c++) {
		/* FAT cluster numbers also drive its allocation table, and FAT
		 * records no initialized length separate from the file length.
		 */
		if (c != LENGTH_PAST_ALLOCATION && !is_exfat)
			continue;
		fixture_open(&fixture, format);
		bzero(&sink, sizeof(sink));
		sink_open(&fixture, &sink, 0xa5);
		id = catalog_id(&fixture, tree[FILE_LARGE].path);
		if (is_exfat) {
			exfat_plan_t *plan = (exfat_plan_t *)fixture.writer->plan;

			CHECK(exfat_layout_get(plan, id, &exfat, NULL) == FSIMG_OK);
			if (c == LENGTH_PAST_ALLOCATION)
				exfat.length = (uint64_t)exfat.clusters *
				    plan->geometry.cluster_bytes + 1;
			else if (c == OUTSIDE_VOLUME)
				exfat.first = plan->geometry.clusters;
			else
				exfat.valid_length = exfat.length + 1;
			CHECK(exfat_layout_set(plan, id, &exfat, NULL) == FSIMG_OK);
		} else {
			fat_plan_t *plan = (fat_plan_t *)fixture.writer->plan;

			CHECK(fsimg_fat_layout_get(plan, id, &fat, NULL) == FSIMG_OK);
			fat.length = (uint64_t)fat.clusters *
			    plan->common.info.allocation_unit_bytes + 1;
			CHECK(fsimg_fat_layout_set(plan, id, &fat, NULL) == FSIMG_OK);
		}
		status = emit_to(&fixture, &sink, &error);
		check_failed(&fixture, &sink, status, FSIMG_ECORRUPT, &error, names[c]);
		fixture_close(&fixture);
		free(sink.bytes);
	}
}

/* Direct copy scenarios. A test emit callback replaces the driver's, so
 * fsimg_emit_copy() receives placements no FAT or exFAT plan produces:
 * several regions in non-ascending image order with boundaries inside a
 * batch, inside a ZERO range and inside a DATA range; zero-capacity regions;
 * and an initialized length that ends inside DATA. Nothing but the payload is
 * written, so the expected image is zero outside the regions.
 */
#define COPY_REGIONS_MAX 5
#define COPY_BASE 4096u
#define COPY_GAP 512u

typedef struct copy_case {
	const char	*name;
	uint64_t	 file;
	uint64_t	 capacities[COPY_REGIONS_MAX];
	size_t		 region_count;
	uint64_t	 length;
	uint64_t	 initialized;
	bool		 last_region_outside_volume;
	uint64_t	 fail_write;		/* nth write inside the copy fails */
	uint64_t	 short_read_id;
	fsimg_status_t	 expected;
} copy_case_t;

/* State shared with copy_case_emit(), which runs inside fsimg_writer_emit(). */
static struct {
	const copy_case_t *test;
	emit_fixture_t	*fixture;
	memory_sink_t	*sink;
	fsimg_emit_region_t regions[COPY_REGIONS_MAX];
	fsimg_emit_payload_t payload;
	uint64_t	 writes;
	bool		 source_active;
	bool		 writer_failed;
} copy_run;

/* Lay out regions in descending logical order at ascending image offsets,
 * separated by gaps, so a copy that assumed contiguous or ascending regions
 * would write the wrong bytes.
 */
static void
place_regions(const uint64_t *capacities, size_t count, uint64_t volume_bytes,
    fsimg_emit_region_t *regions)
{
	uint64_t image_offset = COPY_BASE;

	for (size_t i = count; i-- > 0;) {
		regions[i].image_offset = image_offset;
		regions[i].capacity = capacities[i];
		image_offset += capacities[i] + COPY_GAP;
	}
	CHECK(image_offset <= volume_bytes);
}

static fsimg_status_t
copy_case_emit(fsimg_writer_t *writer, fsimg_emit_t *emit, fsimg_error_t *error)
{
	uint64_t writes_before;
	fsimg_status_t status;

	writes_before = copy_run.sink->writes;
	if (copy_run.test->fail_write != 0)
		copy_run.sink->fail_write = writes_before + copy_run.test->fail_write;
	status = fsimg_emit_copy(emit, catalog_id(copy_run.fixture,
	    tree[copy_run.test->file].path), &copy_run.payload, error);
	/* Observed before fsimg_writer_emit() fails the writer, so a closed
	 * source here was closed by the copy itself.
	 */
	copy_run.writes = copy_run.sink->writes - writes_before;
	copy_run.source_active = writer->source.active;
	copy_run.writer_failed = writer->state == FSIMG_WRITER_FAILED;
	return (status);
}

/* The oracle maps each region's bytes back to logical offsets on its own:
 * logical bytes below initialized come from file_byte(), everything else,
 * including region slack and the gaps between regions, is zero.
 */
static void
check_placed(const memory_sink_t *sink, uint64_t file,
    const fsimg_emit_payload_t *payload)
{
	unsigned char *expected;
	const fsimg_emit_region_t *region;
	uint64_t logical;

	expected = calloc(1, (size_t)sink->length);
	CHECK(expected != NULL);
	logical = 0;
	for (size_t i = 0; i < payload->region_count; i++) {
		region = &payload->regions[i];
		for (uint64_t k = 0; k < region->capacity; k++, logical++) {
			if (logical < payload->initialized)
				expected[region->image_offset + k] =
				    file_byte(file, logical);
		}
	}
	CHECK(bcmp(expected, sink->bytes, (size_t)sink->length) == 0);
	free(expected);
}

/* What the copy observer saw: each observed byte at its volume offset, which
 * offsets were observed, and the calls and bytes in total.
 */
static struct {
	unsigned char	*bytes;
	bool		*seen;
	uint64_t	 length;
	uint64_t	 calls;
	uint64_t	 total;
} observed;

/* Every observed byte lies in the volume and is observed once. */
static void
observe_copy(void *argument, const unsigned char *bytes, size_t length,
    uint64_t offset)
{
	CHECK(argument == &observed);
	CHECK(length != 0 && offset <= observed.length &&
	    length <= observed.length - offset);
	for (size_t i = 0; i < length; i++) {
		CHECK(!observed.seen[offset + i]);
		observed.seen[offset + i] = true;
		observed.bytes[offset + i] = bytes[i];
	}
	observed.calls++;
	observed.total += length;
}

/* The observer saw exactly the bytes the copy wrote: every DATA byte below
 * the initialized length, with its value and volume offset, and nothing of a
 * ZERO range, of region slack or of the gaps.
 */
static void
check_observed(const memory_sink_t *sink, uint64_t file,
    const fsimg_emit_payload_t *payload)
{
	const fsimg_emit_region_t *region;
	uint64_t logical, offset, expected;
	bool data;

	logical = 0;
	expected = 0;
	for (size_t i = 0; i < payload->region_count; i++) {
		region = &payload->regions[i];
		for (uint64_t k = 0; k < region->capacity; k++, logical++) {
			offset = region->image_offset + k;
			data = logical < payload->initialized &&
			    file != FILE_ZEROS && !file_zero_at(file, logical);
			CHECK(observed.seen[offset] == data);
			if (data)
				CHECK(observed.bytes[offset] == sink->bytes[offset]);
			expected += data;
		}
	}
	CHECK(observed.total == expected);
	CHECK(expected == 0 || observed.calls != 0);
}

static void
test_direct_copies(void)
{
	static const copy_case_t cases[] = {
		{ .name = "fragmented mixed source", .file = FILE_MIXED,
		    .capacities = { 1000, 0, 100000, 50000, 49500 },
		    .region_count = 5, .length = 200000, .initialized = 200000,
		    .expected = FSIMG_OK },
		{ .name = "zero source", .file = FILE_ZEROS,
		    .capacities = { 70004 }, .region_count = 1,
		    .length = 70001, .initialized = 70001,
		    .expected = FSIMG_OK },
		{ .name = "source of one ZERO range", .file = FILE_SPARSE,
		    .capacities = { 50000 }, .region_count = 1,
		    .length = 50000, .initialized = 50000,
		    .expected = FSIMG_OK },
		{ .name = "initialized ends inside data", .file = FILE_LARGE,
		    .capacities = { 301000 }, .region_count = 1,
		    .length = 300000, .initialized = 123457,
		    .expected = FSIMG_OK },
		{ .name = "plan length below catalog", .file = FILE_LARGE,
		    .capacities = { 301000 }, .region_count = 1,
		    .length = 299999, .initialized = 299999,
		    .expected = FSIMG_ECORRUPT },
		{ .name = "allocation smaller than length", .file = FILE_LARGE,
		    .capacities = { 299999 }, .region_count = 1,
		    .length = 300000, .initialized = 300000,
		    .expected = FSIMG_ECORRUPT },
		{ .name = "region outside volume", .file = FILE_LARGE,
		    .capacities = { 100000, 200000 }, .region_count = 2,
		    .length = 300000, .initialized = 300000,
		    .last_region_outside_volume = true,
		    .expected = FSIMG_ECORRUPT },
		{ .name = "sink failure inside copy", .file = FILE_LARGE,
		    .capacities = { 300000 }, .region_count = 1,
		    .length = 300000, .initialized = 300000, .fail_write = 2,
		    .expected = FSIMG_EIO },
		{ .name = "source failure inside copy", .file = FILE_LARGE,
		    .capacities = { 300000 }, .region_count = 1,
		    .length = 300000, .initialized = 300000,
		    .short_read_id = FILE_LARGE, .expected = FSIMG_EIO },
	};
	const copy_case_t *test;
	emit_fixture_t fixture;
	memory_sink_t sink;
	fsimg_writer_ops_t ops;
	fsimg_format_t format;
	fsimg_error_t error;
	fsimg_status_t status;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		test = &cases[i];
		fixture_open(&fixture, "fat12");
		bzero(&sink, sizeof(sink));
		sink_open(&fixture, &sink, 0xa5);
		fixture.short_read_id = test->short_read_id;
		format = *fixture.writer->format;
		ops = *format.writer;
		ops.emit = copy_case_emit;
		format.writer = &ops;
		fixture.writer->format = &format;

		bzero(&copy_run, sizeof(copy_run));
		copy_run.test = test;
		copy_run.fixture = &fixture;
		copy_run.sink = &sink;
		place_regions(test->capacities, test->region_count, sink.length,
		    copy_run.regions);
		if (test->last_region_outside_volume)
			copy_run.regions[test->region_count - 1].image_offset =
			    sink.length - 10;
		copy_run.payload.regions = copy_run.regions;
		copy_run.payload.region_count = test->region_count;
		copy_run.payload.length = test->length;
		copy_run.payload.initialized = test->initialized;
		copy_run.payload.observe = observe_copy;
		copy_run.payload.observer_argument = &observed;
		bzero(&observed, sizeof(observed));
		observed.length = sink.length;
		observed.bytes = calloc(1, (size_t)sink.length);
		observed.seen = calloc((size_t)sink.length, sizeof(bool));
		CHECK(observed.bytes != NULL && observed.seen != NULL);

		status = emit_to(&fixture, &sink, &error);
		/* The copy closes its source on every outcome. Only an operational
		 * source failure fails the writer at once (design-sources.md); any
		 * other failure does so when fsimg_writer_emit() returns.
		 */
		CHECK(!copy_run.source_active);
		CHECK(copy_run.writer_failed == (test->short_read_id != 0));
		if (test->expected == FSIMG_OK) {
			if (status != FSIMG_OK)
				fprintf(stderr, "%s: %d %s\n", test->name, status,
				    error.message);
			CHECK(status == FSIMG_OK);
			check_placed(&sink, test->file, &copy_run.payload);
			check_observed(&sink, test->file, &copy_run.payload);
		} else {
			check_failed(&fixture, &sink, status, test->expected,
			    &error, test->name);
			/* Placement and length errors are found before any write. */
			if (test->expected == FSIMG_ECORRUPT)
				CHECK(copy_run.writes == 0);
		}
		fixture_close(&fixture);
		free(sink.bytes);
		free(observed.bytes);
		free(observed.seen);
	}
}

int
main(void)
{
	static const char *const formats[] = { "fat12", "exfat" };

	for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		test_content(formats[i]);
		test_source_faults(formats[i]);
		test_sink_faults(formats[i]);
		test_allocation_failures(formats[i]);
		test_corrupt_plans(formats[i]);
	}
	test_direct_copies();
	return (0);
}
