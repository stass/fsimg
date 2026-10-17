/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "fat.h"
#include "io.h"
#include "publish.h"
#include "source.h"
#include "test.h"
#include "write.h"

typedef struct writer_fixture {
	fsimg_ctx_t	*ctx;
	fsimg_writer_t	*writer;
	fsimg_stats_t	 baseline;
	char		 directory[64];
	int		 parent;
	unsigned	 validations;
	unsigned	 losses;
	bool		 changed;
	bool		 leave_staging;
} writer_fixture_t;

typedef struct memory_sink {
	unsigned char	*bytes;
	uint64_t	 length;
	uint64_t	 writes;
	uint64_t	 zeroes;
	bool		 fail_write;
	bool		 fail_flush;
} memory_sink_t;

static const char *const fixture_paths[] = {
	"/HELLO.TXT", "/Nested", "/Nested/long name.txt", "/F0000000",
	"/f0000001", "/F0000002", "/R\303\251sum\303\251 collection.txt", "/EMPTY", "/ZERO"
};
static const char payload[] = "Independent FAT writer fixture.\n";

static void fixture_open(writer_fixture_t *, const char *,
    const fsimg_write_options_t *, uint32_t);
static void fixture_close(writer_fixture_t *);
static void add_fixture(writer_fixture_t *, bool);
static fsimg_status_t provider_validate(void *, uint64_t, fsimg_error_t *);
static fsimg_status_t provider_read(void *, uint64_t, uint64_t, void *,
    size_t, size_t *, fsimg_error_t *);
static void loss_report(void *, const char *, uint32_t);
static fsimg_status_t memory_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t memory_read(void *, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t memory_write(void *, uint64_t, const void *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t memory_zero(void *, uint64_t, uint64_t, fsimg_error_t *);
static fsimg_status_t memory_flush(void *, fsimg_error_t *);
static void memory_io(writer_fixture_t *, memory_sink_t *, bool, fsimg_io_t **);
static void check_contents(writer_fixture_t *, memory_sink_t *);
static void test_roundtrip(const char *);
static void test_geometry(void);
static void test_type_margins(void);
static void test_invalid_names(void);
static void test_metadata(void);
static void test_faults(void);
static void test_aliases(void);
static void test_publication(void);
static void test_allocation_failures(void);
static void test_zero_work(void);
static void test_short_zero_work(void);
static void test_sparse_publication(void);
static void test_layout_boundaries(void);
static void test_slice_emission(void);
static void test_timestamp_boundaries(void);
static void test_retained_staging(void);
static void test_corrupt_payload_plan(void);
static int unsupported_link(void *, int, const char *, int, const char *);
static int publication_sync(void *, int);
static int publication_unlink(void *, int, const char *);
static void create_external(const char *, const char *, bool);
static void test_scale(void);

static void
fixture_open(writer_fixture_t *fixture, const char *format,
    const fsimg_write_options_t *write_options, uint32_t pages)
{
	fsimg_options_t options;
	fsimg_source_ops_t source_ops;

	bzero(fixture, sizeof(*fixture));
	snprintf(fixture->directory, sizeof(fixture->directory), "fat-write-XXXXXX");
	CHECK(mkdtemp(fixture->directory) != NULL);
	fixture->parent = open(fixture->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fixture->parent != -1);
	fsimg_options_init(&options);
	options.memory_bytes = UINT64_C(16) * 1024 * 1024;
	options.scratch_directory_fd = fixture->parent;
	options.max_open_files = 16;
	options.loss = loss_report;
	options.loss_argument = fixture;
	CHECK(fsimg_ctx_new(&options, &fixture->ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_test_meta_cache(fixture->ctx, pages, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &fixture->baseline, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_new(fixture->ctx, format, write_options,
	    &fixture->writer, NULL) == FSIMG_OK);
	fsimg_source_ops_init(&source_ops);
	source_ops.validate = provider_validate;
	source_ops.read_at = provider_read;
	CHECK(fsimg_writer_set_sources(fixture->writer, &source_ops, fixture,
	    NULL) == FSIMG_OK);
}

static void
fixture_close(writer_fixture_t *fixture)
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

static fsimg_status_t
provider_validate(void *argument, uint64_t id FSIMG_UNUSED,
    fsimg_error_t *error FSIMG_UNUSED)
{
	writer_fixture_t *fixture = argument;
	DIR *directory;
	struct dirent *entry;
	int staging, fd;

	fixture->validations++;
	if (fixture->leave_staging) {
		/* Simulate an unexpected extra staging object. The coordinator may
		 * remove only objects it owns, and must report the retained directory.
		 */
		directory = fdopendir(dup(fixture->parent));
		CHECK(directory != NULL);
		while ((entry = readdir(directory)) != NULL) {
			if (strncmp(entry->d_name, ".fsimg-output-", 14) != 0)
				continue;
			staging = openat(fixture->parent, entry->d_name,
			    O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
			CHECK(staging != -1);
			fd = openat(staging, "retained", O_WRONLY | O_CREAT | O_EXCL, 0600);
			CHECK(fd != -1);
			CHECK(close(fd) == 0);
			CHECK(close(staging) == 0);
			break;
		}
		CHECK(entry != NULL);
		CHECK(closedir(directory) == 0);
		return (FSIMG_ESOURCE_CHANGED);
	}
	return (fixture->changed ? FSIMG_ESOURCE_CHANGED : FSIMG_OK);
}

static fsimg_status_t
provider_read(void *argument FSIMG_UNUSED, uint64_t id, uint64_t offset,
    void *buffer, size_t length, size_t *transferred,
    fsimg_error_t *error FSIMG_UNUSED)
{
	unsigned char *bytes = buffer;

	/* Deliberately short reads exercise the source adapter's exact-copy loop. */
	if (length > 137)
		length = 137;
	for (size_t i = 0; i < length; i++)
		bytes[i] = id == 1 ? (unsigned char)payload[(offset + i) % (sizeof(payload) - 1)] :
		    (unsigned char)((offset + i) * 37 + id);
	*transferred = length;
	return (FSIMG_OK);
}

static void
loss_report(void *argument, const char *path, uint32_t field)
{
	writer_fixture_t *fixture = argument;

	CHECK(path[0] == '/');
	CHECK(field == FSIMG_META_CREATED || field == FSIMG_META_MODIFIED ||
	    field == FSIMG_META_ACCESSED);
	fixture->losses++;
}

static void
add_fixture(writer_fixture_t *fixture, bool reverse)
{
	fsimg_entry_t entry;
	fsimg_source_t source = { .kind = FSIMG_SOURCE_PROVIDER, .id = 1 };
	size_t count, index;

	count = sizeof(fixture_paths) / sizeof(fixture_paths[0]);
	for (size_t i = 0; i < count; i++) {
		index = reverse ? count - i - 1 : i;
		bzero(&entry, sizeof(entry));
		entry.path = fixture_paths[index];
		entry.kind = index == 1 ? FSIMG_ENTRY_DIRECTORY : FSIMG_ENTRY_FILE;
		entry.size = index == 1 || index == 7 ? 0 :
		    index == 8 ? 70001 : sizeof(payload) - 1;
		if (index == 8) {
			source.kind = FSIMG_SOURCE_ZERO;
			source.id = 0;
		} else {
			source.kind = FSIMG_SOURCE_PROVIDER;
			source.id = 1;
		}
		CHECK(fsimg_writer_add(fixture->writer, &entry,
		    entry.kind == FSIMG_ENTRY_DIRECTORY ? NULL : &source, NULL) == FSIMG_OK);
	}
}

static fsimg_status_t
memory_size(void *argument, uint64_t *length, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_sink_t *sink = argument;

	*length = sink->length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_sink_t *sink = argument;

	CHECK(fsimg_range_fits(offset, length, sink->length));
	bcopy(sink->bytes + offset, buffer, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_write(void *argument, uint64_t offset, const void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_sink_t *sink = argument;

	*transferred = 0;
	sink->writes++;
	if (sink->fail_write)
		return (FSIMG_EIO);
	CHECK(fsimg_range_fits(offset, length, sink->length));
	bcopy(buffer, sink->bytes + offset, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_zero(void *argument, uint64_t offset, uint64_t length,
    fsimg_error_t *error FSIMG_UNUSED)
{
	memory_sink_t *sink = argument;

	sink->zeroes++;
	CHECK(fsimg_range_fits(offset, length, sink->length));
	bzero(sink->bytes + offset, (size_t)length);
	return (FSIMG_OK);
}

static fsimg_status_t
memory_flush(void *argument, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_sink_t *sink = argument;

	return (sink->fail_flush ? FSIMG_EIO : FSIMG_OK);
}

static void
memory_io(writer_fixture_t *fixture, memory_sink_t *sink, bool writable,
    fsimg_io_t **out)
{
	fsimg_io_ops_t ops;

	fsimg_io_ops_init(&ops);
	ops.get_size = memory_size;
	ops.read_at = memory_read;
	if (writable) {
		ops.write_at = memory_write;
		ops.zero_range = memory_zero;
		ops.flush = memory_flush;
	}
	CHECK(fsimg_io_from_callbacks(fixture->ctx, &ops, sink, out, NULL) == FSIMG_OK);
}

static void
check_contents(writer_fixture_t *fixture, memory_sink_t *sink)
{
	fsimg_reader_t *reader;
	fsimg_io_t *input;
	const fsimg_entry_t *entry;
	fsimg_verify_report_t report;
	unsigned char buffer[4096];
	uint64_t offset, files, directories;
	size_t length;
	fsimg_status_t status;

	memory_io(fixture, sink, false, &input);
	CHECK(fsimg_reader_open(fixture->ctx, input, NULL, &reader, NULL) == FSIMG_OK);
	files = directories = 0;
	while ((status = fsimg_reader_next(reader, &entry, NULL)) == FSIMG_OK) {
		if (entry->kind == FSIMG_ENTRY_DIRECTORY) {
			directories++;
			continue;
		}
		files++;
		offset = 0;
		while (offset < entry->size) {
			CHECK(fsimg_reader_read_at(reader, offset, buffer, sizeof(buffer),
			    &length, NULL) == FSIMG_OK);
			CHECK(length > 0);
			for (size_t i = 0; i < length; i++)
				CHECK(buffer[i] == (strcmp(entry->path, "/ZERO") == 0 ? 0 :
				    (unsigned char)payload[(offset + i) % (sizeof(payload) - 1)]));
			offset += length;
		}
	}
	CHECK(status == FSIMG_DONE);
	CHECK(files == 8 && directories == 1);
	fsimg_reader_free(reader);
	CHECK(fsimg_reader_open(fixture->ctx, input, NULL, &reader, NULL) == FSIMG_OK);
	CHECK(fsimg_verify(reader, FSIMG_VERIFY_PAYLOAD, &report, NULL) == FSIMG_OK);
	CHECK(report.complete && report.files == files && report.directories == 2);
	fsimg_reader_free(reader);
	fsimg_io_free(input);
}

static void
test_roundtrip(const char *format)
{
	writer_fixture_t fixture;
	memory_sink_t sinks[2] = { 0 };
	const fsimg_plan_t *plan, *again;
	fsimg_plan_info_t info;
	fsimg_io_t *output;
	fat_geometry_t geometry;
	uint64_t used_end;

	for (unsigned pass = 0; pass < 2; pass++) {
		fixture_open(&fixture, format, NULL, pass == 0 ? 1 : 16);
		add_fixture(&fixture, pass != 0);
		CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
		CHECK(fsimg_writer_plan(fixture.writer, &again, NULL) == FSIMG_OK);
		CHECK(plan == again);
		CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
		sinks[pass].length = info.volume_bytes;
		sinks[pass].bytes = malloc((size_t)info.volume_bytes);
		CHECK(sinks[pass].bytes != NULL);
		memset(sinks[pass].bytes, 0xa5, (size_t)info.volume_bytes);
		memory_io(&fixture, &sinks[pass], true, &output);
		CHECK(fsimg_writer_emit(fixture.writer, output, NULL) == FSIMG_OK);
		CHECK(fsimg_writer_emit(fixture.writer, output, NULL) == FSIMG_ESTATE);
		CHECK(fixture.validations != 0);
		fsimg_io_free(output);
		check_contents(&fixture, &sinks[pass]);
		CHECK(fsimg_fat_geometry_decode(sinks[pass].bytes, info.volume_bytes,
		    &geometry, NULL) == FSIMG_OK);
		/* Allocations form a prefix. Every free byte must overwrite poison. */
		used_end = geometry.data_offset + info.allocated_bytes;
		for (uint64_t i = used_end; i < info.volume_bytes; i++)
			CHECK(sinks[pass].bytes[i] == 0);
		fixture_close(&fixture);
	}
	CHECK(sinks[0].length == sinks[1].length);
	CHECK(bcmp(sinks[0].bytes, sinks[1].bytes, (size_t)sinks[0].length) == 0);
	free(sinks[0].bytes);
	free(sinks[1].bytes);
}

static void
test_type_margins(void)
{
	static const char *const formats[] = { "fat16", "fat32", "fat12", "fat16" };
	/* Exact boundary geometries include reserved sectors, two adequate FATs,
	 * and the fixed root where present. These counts come from the disk rules.
	 */
	static const uint64_t exact_sectors[] = { 4150, 66581 };
	static const uint64_t exact_clusters[] = { 4085, 65525 };
	writer_fixture_t fixture;
	fsimg_write_options_t options;
	fsimg_entry_t entry = { .path = "/ZERO", .kind = FSIMG_ENTRY_FILE };
	fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	uint64_t clusters;

	for (unsigned scenario = 0; scenario < 4; scenario++) {
		fixture_open(&fixture, formats[scenario], NULL, 1);
		if (scenario >= 2) {
			entry.size = (scenario == 2 ? UINT64_C(4075) : UINT64_C(65515)) * 512;
			CHECK(fsimg_writer_add(fixture.writer, &entry, &zero, NULL) == FSIMG_OK);
		}
		CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
		CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
		clusters = (info.allocated_bytes + info.free_bytes) / info.allocation_unit_bytes;
		if (!((clusters + 16 <= 4085 || clusters >= 4101) &&
		    (clusters + 16 <= 65525 || clusters >= 65541)))
			fprintf(stderr, "%s automatic geometry too close to type cutoff: %llu clusters\n",
			    formats[scenario], (unsigned long long)clusters);
		CHECK(clusters + 16 <= 4085 || clusters >= 4101);
		CHECK(clusters + 16 <= 65525 || clusters >= 65541);
		if (scenario >= 2)
			CHECK(info.allocation_unit_bytes == 1024);
		fixture_close(&fixture);
	}

	for (unsigned variant = 0; variant < 2; variant++) {
		fsimg_write_options_init(&options);
		options.volume_bytes = exact_sectors[variant] * 512;
		options.allocation_unit_bytes = 512;
		fixture_open(&fixture, formats[variant], &options, 1);
		CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
		CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
		clusters = (info.allocated_bytes + info.free_bytes) / info.allocation_unit_bytes;
		CHECK(clusters == exact_clusters[variant]);
		fixture_close(&fixture);
	}
}

static void
test_geometry(void)
{
	static const char *const formats[] = { "fat12", "fat16", "fat32" };
	writer_fixture_t fixture;
	fsimg_write_options_t options;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;

	for (unsigned variant = 0; variant < 3; variant++) {
		for (uint32_t sector = 512; sector <= 4096; sector *= 2) {
			for (uint32_t cluster = sector; cluster <= 32768; cluster *= 2) {
				fsimg_write_options_init(&options);
				options.sector_bytes = sector;
				options.allocation_unit_bytes = cluster;
				options.min_free_bytes = 513;
				fixture_open(&fixture, formats[variant], &options, 1);
				CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
				CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
				CHECK(info.free_bytes >= 513);
				CHECK(info.sector_bytes == sector && info.allocation_unit_bytes == cluster);
				options.volume_bytes = info.volume_bytes;
				fixture_close(&fixture);

				fixture_open(&fixture, formats[variant], &options, 1);
				CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
				CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
				CHECK(info.volume_bytes == options.volume_bytes);
				fixture_close(&fixture);
			}
		}
	}
	fsimg_write_options_init(&options);
	options.volume_bytes = 1024;
	fixture_open(&fixture, "fat32", &options, 1);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_ERANGE);
	CHECK(plan == NULL);
	fixture_close(&fixture);

	options.volume_bytes = 0;
	options.fat_root_entries = 16;
	fixture_open(&fixture, "fat12", &options, 1);
	add_fixture(&fixture, false);
	{
		fsimg_entry_t extra = { .path = "/Extra filename exhausting root slots",
		    .kind = FSIMG_ENTRY_FILE };
		fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };

		CHECK(fsimg_writer_add(fixture.writer, &extra, &zero, NULL) == FSIMG_OK);
	}
	CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_ERANGE);
	fixture_close(&fixture);
}

static void
test_invalid_names(void)
{
	static const char *const paths[] = { "/trailing.", "/a:b", "/ leading" };
	writer_fixture_t fixture;
	fsimg_entry_t entry = { .kind = FSIMG_ENTRY_FILE };
	fsimg_source_t source = { .kind = FSIMG_SOURCE_ZERO };
	const fsimg_plan_t *plan;

	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		fixture_open(&fixture, "fat12", NULL, 1);
		entry.path = paths[i];
		CHECK(fsimg_writer_add(fixture.writer, &entry, &source, NULL) == FSIMG_OK);
		CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_EINVAL);
		fixture_close(&fixture);
	}
	fixture_open(&fixture, "fat12", NULL, 1);
	entry.path = "/same";
	CHECK(fsimg_writer_add(fixture.writer, &entry, &source, NULL) == FSIMG_OK);
	entry.path = "/SAME";
	CHECK(fsimg_writer_add(fixture.writer, &entry, &source, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_EINVAL);
	fixture_close(&fixture);

	fixture_open(&fixture, "fat12", NULL, 1);
	entry.path = "/LARGE";
	entry.size = (uint64_t)UINT32_MAX + 1;
	CHECK(fsimg_writer_add(fixture.writer, &entry, &source, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_ERANGE);
	fixture_close(&fixture);
}

static void
test_metadata(void)
{
	writer_fixture_t fixture;
	fsimg_entry_t entry = { .path = "/TIME", .kind = FSIMG_ENTRY_FILE };
	fsimg_source_t source = { .kind = FSIMG_SOURCE_ZERO };
	fsimg_write_options_t options;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fat_layout_t row;

	entry.metadata.valid = FSIMG_META_MODIFIED;
	entry.metadata.modified = (fsimg_timestamp_t){ .seconds = 1700000001,
	    .precision_ns = 1, .nanoseconds = 123, .basis = FSIMG_TIME_UTC };
	for (unsigned pass = 0; pass < 3; pass++) {
		fsimg_write_options_init(&options);
		if (pass == 1)
			options.flags = 0;
		entry.metadata.required = pass == 2 ? FSIMG_META_MODIFIED : 0;
		fixture_open(&fixture, "fat12", &options, 1);
		CHECK(fsimg_writer_add(fixture.writer, &entry, &source, NULL) == FSIMG_OK);
		if (pass != 0) {
			CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_EUNSUPPORTED);
		} else {
			CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
			CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
			CHECK(info.metadata_losses == 1 && fixture.losses == 1);
			CHECK(fsimg_fat_layout_get((fat_plan_t *)fixture.writer->plan, 1, &row, NULL) == FSIMG_OK);
			CHECK(fsimg_fat_u16(row.entry + 24) == 0x576e); /* 2023-11-14 */
		}
		fixture_close(&fixture);
	}
}

static void
test_faults(void)
{
	writer_fixture_t fixture;
	memory_sink_t sink;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_io_t *output;
	fsimg_error_t error;

	for (unsigned scenario = 0; scenario < 5; scenario++) {
		bzero(&sink, sizeof(sink));
		fixture_open(&fixture, "fat12", NULL, 1);
		add_fixture(&fixture, false);
		CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
		CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
		sink.length = info.volume_bytes + (scenario == 0 ? 1 : 0);
		sink.bytes = calloc(1, (size_t)sink.length);
		CHECK(sink.bytes != NULL);
		memory_io(&fixture, &sink, true, &output);
		if (scenario == 1)
			sink.fail_write = true;
		if (scenario == 2)
			sink.fail_flush = true;
		if (scenario == 3)
			fixture.changed = true;
		if (scenario == 4)
			fixture.writer->options.flags |= FSIMG_WRITE_DURABLE;
		CHECK(fsimg_writer_emit(fixture.writer, output, &error) < FSIMG_OK);
		if (scenario == 0 || scenario == 4)
			CHECK(sink.writes == 0 && sink.zeroes == 0);
		else
			CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
		CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
		CHECK(fsimg_writer_emit(fixture.writer, output, NULL) == FSIMG_ESTATE);
		fsimg_io_free(output);
		free(sink.bytes);
		fixture_close(&fixture);
	}
}

static void
test_aliases(void)
{
	writer_fixture_t fixture;
	fsimg_io_t *output, *slice;
	struct stat before, after;
	char bytes[8];
	fsimg_error_t error;
	fsimg_status_t status;
	int source_dir, fd;

	for (unsigned scenario = 0; scenario < 3; scenario++) {
		fixture_open(&fixture, "fat12", NULL, 1);
		CHECK(mkdirat(fixture.parent, "sources", 0700) == 0);
		source_dir = openat(fixture.parent, "sources", O_RDONLY | O_DIRECTORY);
		CHECK(source_dir != -1);
		fd = openat(source_dir, "file", O_CREAT | O_EXCL | O_RDWR, 0600);
		CHECK(fd != -1);
		CHECK(write(fd, "sentinel", 8) == 8);
		CHECK(linkat(source_dir, "file", fixture.parent, "alias", 0) == 0);
		CHECK(fsimg_writer_add_tree(fixture.writer, source_dir, "/", NULL) == FSIMG_OK);
		if (scenario == 1) {
			CHECK(close(fd) == 0);
			fd = openat(fixture.parent, "alias", O_RDWR);
			CHECK(fd != -1);
		}
		CHECK(fstat(fd, &before) == 0);
		CHECK(fsimg_io_from_fd(fixture.ctx, fd, FSIMG_IO_WRITE, &output, NULL) == FSIMG_OK);
		if (scenario == 2) {
			CHECK(fsimg_io_slice(output, 0, 8, &slice, NULL) == FSIMG_OK);
			CHECK(fsimg_writer_emit(fixture.writer, slice, &error) == FSIMG_EINVAL);
			CHECK(strstr(error.message, "aliases") != NULL);
			fsimg_io_free(slice);
		} else {
			status = fsimg_writer_emit(fixture.writer, output, &error);
			if (status != FSIMG_EINVAL)
				fprintf(stderr, "alias case %u: %d: %s\n", scenario,
				    status, error.message);
			CHECK(status == FSIMG_EINVAL);
		}
		CHECK(fstat(fd, &after) == 0);
		CHECK(before.st_size == after.st_size && before.st_mode == after.st_mode);
		CHECK(pread(fd, bytes, sizeof(bytes), 0) == 8);
		CHECK(bcmp(bytes, "sentinel", 8) == 0);
		fsimg_io_free(output);
		CHECK(close(fd) == 0);
		CHECK(unlinkat(fixture.parent, "alias", 0) == 0);
		CHECK(unlinkat(source_dir, "file", 0) == 0);
		CHECK(close(source_dir) == 0);
		CHECK(unlinkat(fixture.parent, "sources", AT_REMOVEDIR) == 0);
		fixture_close(&fixture);
	}
}

static void
test_publication(void)
{
	writer_fixture_t fixture;
	fsimg_error_t error;
	char bytes[8];
	int fd;

	fixture_open(&fixture, "fat12", NULL, 1);
	add_fixture(&fixture, false);
	fd = openat(fixture.parent, "existing", O_CREAT | O_EXCL | O_RDWR, 0600);
	CHECK(fd != -1);
	CHECK(write(fd, "sentinel", 8) == 8);
	CHECK(fsimg_writer_write_at(fixture.writer, fixture.parent, "existing", &error) == FSIMG_EEXISTS);
	CHECK(error.output_state == FSIMG_OUTPUT_ABSENT);
	CHECK(pread(fd, bytes, sizeof(bytes), 0) == 8);
	CHECK(bcmp(bytes, "sentinel", 8) == 0);
	CHECK(close(fd) == 0);
	CHECK(unlinkat(fixture.parent, "existing", 0) == 0);
	fixture_close(&fixture);

	fixture_open(&fixture, "fat12", NULL, 1);
	add_fixture(&fixture, false);
	CHECK(fsimg_writer_write_at(fixture.writer, fixture.parent, "image", &error) == FSIMG_OK);
	CHECK(unlinkat(fixture.parent, "image", 0) == 0);
	fixture_close(&fixture);
}

static void
test_allocation_failures(void)
{
	writer_fixture_t fixture;
	const fsimg_plan_t *plan;
	fsimg_status_t status;
	bool succeeded;

	succeeded = false;
	for (uint64_t attempt = 1; attempt < 256; attempt++) {
		fixture_open(&fixture, "fat12", NULL, 1);
		add_fixture(&fixture, false);
		fsimg_test_fail_alloc_after(fixture.ctx, attempt);
		status = fsimg_writer_plan(fixture.writer, &plan, NULL);
		CHECK(status == FSIMG_OK || status == FSIMG_ENOMEM);
		fsimg_test_fail_alloc_after(fixture.ctx, 0);
		fixture_close(&fixture);
		if (status == FSIMG_OK) {
			succeeded = true;
			break;
		}
	}
	CHECK(succeeded);
}

static void
test_corrupt_payload_plan(void)
{
	writer_fixture_t fixture;
	memory_sink_t sink = { 0 };
	const fsimg_plan_t *plan;
	const fsimg_catalog_entry_t *catalog;
	fsimg_plan_info_t info;
	fsimg_io_t *output;
	fat_layout_t row;
	uint64_t id;

	fixture_open(&fixture, "fat12", NULL, 1);
	add_fixture(&fixture, false);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
	for (id = 1; id <= fixture.writer->count; id++) {
		CHECK(fsimg_catalog_get(fixture.writer, id, &catalog, NULL) == FSIMG_OK);
		if (strcmp(catalog->entry.path, "/HELLO.TXT") == 0)
			break;
	}
	CHECK(id <= fixture.writer->count);
	CHECK(fsimg_fat_layout_get((fat_plan_t *)fixture.writer->plan, id, &row, NULL) == FSIMG_OK);
	row.length++;
	CHECK(fsimg_fat_layout_set((fat_plan_t *)fixture.writer->plan, id, &row, NULL) == FSIMG_OK);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	sink.length = info.volume_bytes;
	sink.bytes = calloc(1, (size_t)sink.length);
	CHECK(sink.bytes != NULL);
	memory_io(&fixture, &sink, true, &output);
	CHECK(fsimg_writer_emit(fixture.writer, output, NULL) == FSIMG_ECORRUPT);
	fsimg_io_free(output);
	free(sink.bytes);
	fixture_close(&fixture);
}

static void
test_retained_staging(void)
{
	const fsimg_publish_ops_t hooks = { .sync_fd = publication_sync,
	    .link_at = unsupported_link, .unlink_at = publication_unlink };
	writer_fixture_t fixture;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_error_t error;
	char *separator;
	int directory;

	fixture_open(&fixture, "fat12", NULL, 1);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	CHECK(fsimg_output_write(fixture.writer, fixture.parent, "unsupported",
	    info.volume_bytes, &hooks, &error) == FSIMG_EUNSUPPORTED);
	CHECK(error.output_state == FSIMG_OUTPUT_ABSENT);
	separator = strchr(error.path, '/');
	CHECK(separator != NULL && strcmp(separator, "/image") == 0);
	*separator = '\0';
	directory = openat(fixture.parent, error.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(directory != -1);
	CHECK(unlinkat(directory, "image", 0) == 0);
	CHECK(close(directory) == 0);
	CHECK(unlinkat(fixture.parent, error.path, AT_REMOVEDIR) == 0);
	fixture_close(&fixture);

	fixture_open(&fixture, "fat12", NULL, 1);
	add_fixture(&fixture, false);
	fixture.leave_staging = true;
	CHECK(fsimg_writer_write_at(fixture.writer, fixture.parent, "failed",
	    &error) == FSIMG_ESOURCE_CHANGED);
	CHECK(error.cleanup_error == ENOTEMPTY || error.cleanup_error == EEXIST);
	CHECK(error.output_state == FSIMG_OUTPUT_ABSENT);
	CHECK(strncmp(error.path, ".fsimg-output-", 14) == 0);
	directory = openat(fixture.parent, error.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(directory != -1);
	CHECK(unlinkat(directory, "retained", 0) == 0);
	CHECK(close(directory) == 0);
	CHECK(unlinkat(fixture.parent, error.path, AT_REMOVEDIR) == 0);
	fixture_close(&fixture);
}

static int
unsupported_link(void *argument FSIMG_UNUSED, int source FSIMG_UNUSED,
    const char *name FSIMG_UNUSED, int parent FSIMG_UNUSED,
    const char *target FSIMG_UNUSED)
{
	errno = EOPNOTSUPP;
	return (-1);
}

static int
publication_sync(void *argument FSIMG_UNUSED, int fd)
{
	return (fsync(fd));
}

static int
publication_unlink(void *argument FSIMG_UNUSED, int parent, const char *name)
{
	return (unlinkat(parent, name, 0));
}

static void
test_layout_boundaries(void)
{
	writer_fixture_t fixture;
	fsimg_write_options_t options;
	fsimg_entry_t entry = { .path = "/MAXIMUM", .kind = FSIMG_ENTRY_FILE,
	    .size = UINT32_MAX };
	fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;

	fixture_open(&fixture, "fat32", NULL, 1);
	CHECK(fsimg_writer_add(fixture.writer, &entry, &zero, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	CHECK(info.payload_bytes == UINT32_MAX && info.volume_bytes > UINT32_MAX);
	fixture_close(&fixture);

	/* Independent geometry arithmetic: 100 data clusters, two one-sector
	 * FAT12 tables, a reserved sector and a 32-sector fixed root.
	 */
	for (unsigned scenario = 0; scenario < 3; scenario++) {
		fsimg_write_options_init(&options);
		options.volume_bytes = (100 + 2 + 1 + 32) * 512;
		options.allocation_unit_bytes = 512;
		options.min_free_bytes = scenario == 1 ? 1 : 0;
		entry.size = 100 * 512 + (scenario == 2 ? 1 : 0);
		fixture_open(&fixture, "fat12", &options, 1);
		CHECK(fsimg_writer_add(fixture.writer, &entry, &zero, NULL) == FSIMG_OK);
		if (scenario == 0) {
			CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
			CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
			CHECK(info.free_bytes == 0);
		} else
			CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_ERANGE);
		fixture_close(&fixture);
	}
	/* The selected variant is never changed to accommodate a threshold. */
	for (unsigned scenario = 0; scenario < 2; scenario++) {
		fsimg_write_options_init(&options);
		options.allocation_unit_bytes = 512;
		options.volume_bytes = (4084 + 1 + 32 + 2 * 12 + scenario) * 512;
		fixture_open(&fixture, "fat12", &options, 1);
		CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) ==
		    (scenario == 0 ? FSIMG_OK : FSIMG_ERANGE));
		fixture_close(&fixture);
	}
}

static void
test_slice_emission(void)
{
	writer_fixture_t fixture;
	memory_sink_t sink = { 0 };
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_io_t *output, *slice;
	fsimg_reader_t *reader;
	fsimg_verify_report_t report;

	fixture_open(&fixture, "fat12", NULL, 1);
	add_fixture(&fixture, false);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	sink.length = info.volume_bytes + 1024;
	sink.bytes = malloc((size_t)sink.length);
	CHECK(sink.bytes != NULL);
	memset(sink.bytes, 0xa5, (size_t)sink.length);
	memory_io(&fixture, &sink, true, &output);
	CHECK(fsimg_io_slice(output, 512, info.volume_bytes, &slice, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_emit(fixture.writer, slice, NULL) == FSIMG_OK);
	for (unsigned i = 0; i < 512; i++) {
		CHECK(sink.bytes[i] == 0xa5);
		CHECK(sink.bytes[512 + info.volume_bytes + i] == 0xa5);
	}
	CHECK(fsimg_reader_open(fixture.ctx, slice, NULL, &reader, NULL) == FSIMG_OK);
	CHECK(fsimg_verify(reader, FSIMG_VERIFY_PAYLOAD, &report, NULL) == FSIMG_OK);
	fsimg_reader_free(reader);
	fsimg_io_free(slice);
	fsimg_io_free(output);
	free(sink.bytes);
	fixture_close(&fixture);
}

static void
test_timestamp_boundaries(void)
{
	static const int64_t seconds[] = { 315532800, 4354819199LL, 315532799,
	    4354819200LL };
	writer_fixture_t fixture;
	fsimg_entry_t entry = { .path = "/TIME", .kind = FSIMG_ENTRY_FILE };
	fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };
	fsimg_write_options_t options;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fat_layout_t row;

	for (unsigned scenario = 0; scenario < 5; scenario++) {
		fsimg_write_options_init(&options);
		if (scenario == 4)
			options.flags |= FSIMG_WRITE_REPRODUCIBLE;
		entry.metadata.valid = FSIMG_META_CREATED | FSIMG_META_ACCESSED;
		entry.metadata.created = (fsimg_timestamp_t){
		    .seconds = seconds[scenario % 4], .precision_ns = 10000000,
		    .nanoseconds = 990000000, .basis = FSIMG_TIME_WALL };
		entry.metadata.accessed = (fsimg_timestamp_t){
		    .seconds = 315532800, .precision_ns = UINT64_C(86400000000000),
		    .basis = FSIMG_TIME_WALL };
		fixture_open(&fixture, "fat12", &options, 1);
		CHECK(fsimg_writer_add(fixture.writer, &entry, &zero, NULL) == FSIMG_OK);
		CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
		CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
		CHECK(fsimg_fat_layout_get((fat_plan_t *)fixture.writer->plan, 1, &row, NULL) == FSIMG_OK);
		if (scenario == 1) {
			CHECK(fsimg_fat_u16(row.entry + 16) == 0xff9f);
			CHECK(row.entry[13] == 199);
		} else {
			CHECK(fsimg_fat_u16(row.entry + 16) == 0x21);
			CHECK(row.entry[13] == (scenario == 0 ? 99 : 0));
		}
		CHECK(info.metadata_losses == (scenario >= 2 ? 1 : 0));
		fixture_close(&fixture);
	}
}

static void
test_sparse_publication(void)
{
	writer_fixture_t fixture;
	struct stat st;
	int fd;

	fixture_open(&fixture, "fat32", NULL, 1);
	CHECK(fsimg_writer_write_at(fixture.writer, fixture.parent, "sparse", NULL) == FSIMG_OK);
	fd = openat(fixture.parent, "sparse", O_RDONLY);
	CHECK(fd != -1);
	CHECK(fstat(fd, &st) == 0);
	CHECK(st.st_size > 32 * 1024 * 1024);
	CHECK((uint64_t)st.st_blocks * 512 < 1024 * 1024);
	CHECK(close(fd) == 0);
	CHECK(unlinkat(fixture.parent, "sparse", 0) == 0);
	fixture_close(&fixture);
}

static void
test_short_zero_work(void)
{
	writer_fixture_t fixture;
	memory_sink_t sink = { 0 };
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_io_ops_t ops;
	fsimg_io_t *output;
	fsimg_error_t error;

	fixture_open(&fixture, "fat12", NULL, 1);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	sink.length = info.volume_bytes;
	sink.bytes = malloc((size_t)sink.length);
	CHECK(sink.bytes != NULL);
	fsimg_io_ops_init(&ops);
	ops.max_transfer = 1;
	ops.get_size = memory_size;
	ops.write_at = memory_write;
	CHECK(fsimg_io_from_callbacks(fixture.ctx, &ops, &sink, &output, NULL) == FSIMG_OK);
	fixture.ctx->options.max_work = fixture.writer->session.progress.work + 3;
	CHECK(fsimg_writer_emit(fixture.writer, output, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_WORK);
	CHECK(sink.writes == 3);
	fsimg_io_free(output);
	free(sink.bytes);
	fixture_close(&fixture);
}

static void
test_zero_work(void)
{
	writer_fixture_t fixture;
	memory_sink_t sink = { 0 };
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_io_t *output;
	fsimg_error_t error;

	fixture_open(&fixture, "fat16", NULL, 1);
	CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	sink.length = info.volume_bytes;
	sink.bytes = malloc((size_t)sink.length);
	CHECK(sink.bytes != NULL);
	memory_io(&fixture, &sink, true, &output);
	fixture.ctx->options.max_work = fixture.writer->session.progress.work + 3;
	CHECK(fsimg_writer_emit(fixture.writer, output, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_WORK);
	CHECK(sink.zeroes == 3 && sink.writes == 0);
	fsimg_io_free(output);
	free(sink.bytes);
	fixture_close(&fixture);
}

static void
create_external(const char *directory, const char *format, bool empty)
{
	writer_fixture_t fixture;
	int parent;

	fixture_open(&fixture, format, NULL, 1);
	if (!empty)
		add_fixture(&fixture, true);
	parent = open(directory, O_RDONLY | O_DIRECTORY);
	CHECK(parent != -1);
	CHECK(fsimg_writer_write_at(fixture.writer, parent, "writer.img", NULL) == FSIMG_OK);
	CHECK(close(parent) == 0);
	fixture_close(&fixture);
}

static void
test_scale(void)
{
	writer_fixture_t fixture;
	fsimg_entry_t entry = { .kind = FSIMG_ENTRY_FILE };
	fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };
	const fsimg_plan_t *plan;
	fsimg_stats_t stats;
	char path[64];

	fixture_open(&fixture, "fat32", NULL, 1);
	for (unsigned i = 0; i < 30000; i++) {
		snprintf(path, sizeof(path), "/File%07u", 29999 - i);
		entry.path = path;
		CHECK(fsimg_writer_add(fixture.writer, &entry, &zero, NULL) == FSIMG_OK);
	}
	CHECK(fsimg_writer_plan(fixture.writer, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_peak <= UINT64_C(16) * 1024 * 1024);
	CHECK(stats.scratch_peak > UINT64_C(16) * 1024 * 1024);
	printf("FAT writer 30000 names: memory=%" PRIu64 " scratch=%" PRIu64
	    " descriptors=%" PRIu64 " io=%" PRIu64 "\n", stats.memory_peak,
	    stats.scratch_peak, stats.descriptors_peak, stats.io_calls);
	fixture_close(&fixture);
}

int
main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--corrupt-plan") == 0) {
		test_corrupt_payload_plan();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--retained-staging") == 0) {
		test_retained_staging();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--sparse-publication") == 0) {
		test_sparse_publication();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--zero-work") == 0) {
		test_zero_work();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--short-zero-work") == 0) {
		test_short_zero_work();
		return (0);
	}
	if (argc == 4 && (strcmp(argv[1], "--create") == 0 ||
	    strcmp(argv[1], "--create-empty") == 0)) {
		create_external(argv[2], argv[3], strcmp(argv[1], "--create-empty") == 0);
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--type-margins") == 0) {
		test_type_margins();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--scale") == 0) {
		test_scale();
		return (0);
	}
	CHECK(argc == 1);
	test_roundtrip("fat12");
	test_roundtrip("fat16");
	test_roundtrip("fat32");
	test_geometry();
	test_type_margins();
	test_invalid_names();
	test_metadata();
	test_faults();
	test_aliases();
	test_publication();
	test_allocation_failures();
	test_zero_work();
	test_short_zero_work();
	test_sparse_publication();
	test_layout_boundaries();
	test_slice_emission();
	test_timestamp_boundaries();
	test_retained_staging();
	test_corrupt_payload_plan();
	return (0);
}
