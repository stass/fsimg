/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "baseline.h"
#include "reader.h"
#include "test.h"
#include "writer.h"

/* Every tree fixture uses the same inputs and budgets, so a counter change
 * reflects the library, not the harness. One metadata cache page makes the
 * writer's prepass and directory sort spill to scratch for the wide directory.
 * The descriptor budget stays at its default: the reader retains a spilled
 * entry stream, and so a scratch descriptor, for each ancestor of the deep
 * path.
 */
#define TREE_MEMORY_BYTES	(UINT64_C(16) * 1024 * 1024) /* library minimum */
#define TREE_CACHE_PAGES	1
#define TREE_WIDE_ENTRIES	300	/* enough names to spill one page */
#define TREE_DEEP_LEVELS	24	/* /deep plus 23 nested levels */
#define TREE_MIXED_BYTES	20000
/* The mixed file's zero range spans whole pages, so it covers complete
 * allocation units in every format and the writer can skip them.
 */
#define MIXED_ZERO_START	4096
#define MIXED_ZERO_END		(3 * 4096)
/* Nonzero sink fill: an emitted image must overwrite every byte. */
#define SINK_POISON		0xa5
#define READ_CHUNK_BYTES	4096	/* one read_at request while hashing */
#define SCRIPT_READ_BYTES	16	/* each short read in the access script */

/* Set to a directory to also write each created image there as
 * <fixture>.img, for an independent `shasum -a 256` cross-check.
 */
#define BASELINE_DUMP_ENV	"FSIMG_BASELINE_DUMP"

/* Provider content. A pattern file's provider id is its length, which lets
 * next_range bound its single data range without a lookup table. The mixed
 * file has its own id: data, a zero range from MIXED_ZERO_START to
 * MIXED_ZERO_END, then data to the end.
 */
#define MIXED_SOURCE_ID		UINT64_MAX

typedef struct image_sink {
	unsigned char	*bytes;
	uint64_t	 length;
} image_sink_t;

/* One tree fixture's measurements. The write stats are taken after emit,
 * before teardown, so their memory_bytes is the writer's retained memory.
 * The read stats are taken after traversal; the reader's retained memory is
 * sampled separately while the last file is current.
 */
typedef struct tree_record {
	char		 image_sha256[SHA256_HEX_BYTES];
	uint64_t	 image_bytes;
	fsimg_stats_t	 write_stats;
	uint64_t	 write_work;
	char		 traversal_sha256[SHA256_HEX_BYTES];
	char		 content_sha256[SHA256_HEX_BYTES];
	fsimg_stats_t	 read_stats;
	uint64_t	 read_memory_retained;
	uint64_t	 read_work;
} tree_record_t;

static fsimg_ctx_t *tree_context(int);
static bool mixed_is_zero(uint64_t);
static fsimg_status_t provider_validate(void *, uint64_t, fsimg_error_t *);
static fsimg_status_t provider_read(void *, uint64_t, uint64_t, void *,
    size_t, size_t *, fsimg_error_t *);
static fsimg_status_t provider_range(void *, uint64_t, uint64_t,
    fsimg_range_t *, fsimg_error_t *);
static fsimg_status_t sink_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t sink_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static fsimg_status_t sink_write(void *, uint64_t, const void *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t sink_zero(void *, uint64_t, uint64_t, fsimg_error_t *);
static fsimg_status_t sink_flush(void *, fsimg_error_t *);
static fsimg_io_t *sink_io(fsimg_ctx_t *, image_sink_t *, bool);
static void add_directory(fsimg_writer_t *, const char *);
static void add_file(fsimg_writer_t *, const char *, uint64_t,
    const fsimg_source_t *);
static void add_pattern_file(fsimg_writer_t *, const char *, uint64_t);
static void add_tree(fsimg_writer_t *);
static void write_tree(const char *, int, image_sink_t *, tree_record_t *);
static void dump_image(const char *, const image_sink_t *);
static void read_tree(int, image_sink_t *, tree_record_t *);
static void check_range(fsimg_reader_t *, uint64_t, uint64_t);
static void digest_entry(sha256_t *, const fsimg_entry_t *);
static void digest_timestamp(sha256_t *, const char *,
    const fsimg_timestamp_t *);

static fsimg_ctx_t *
tree_context(int scratch)
{
	fsimg_options_t options;
	fsimg_ctx_t *ctx;

	fsimg_options_init(&options);
	options.memory_bytes = TREE_MEMORY_BYTES;
	options.scratch_directory_fd = scratch;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_test_meta_cache(ctx, TREE_CACHE_PAGES, NULL) == FSIMG_OK);
	return (ctx);
}

/* Provider callbacks. They receive no argument: content is a pure function
 * of (id, offset), so every run supplies identical bytes and ranges.
 */
static bool
mixed_is_zero(uint64_t offset)
{
	return (offset >= MIXED_ZERO_START && offset < MIXED_ZERO_END);
}

static fsimg_status_t
provider_validate(void *argument FSIMG_UNUSED, uint64_t id FSIMG_UNUSED,
    fsimg_error_t *error FSIMG_UNUSED)
{
	return (FSIMG_OK);
}

static fsimg_status_t
provider_read(void *argument FSIMG_UNUSED, uint64_t id, uint64_t offset,
    void *buffer, size_t length, size_t *transferred,
    fsimg_error_t *error FSIMG_UNUSED)
{
	unsigned char *bytes = buffer;
	uint64_t position;

	/* The pattern depends on the byte position and the file's id, so a
	 * misplaced cluster or a swap between files changes the content digest.
	 */
	for (size_t i = 0; i < length; i++) {
		position = offset + i;
		if (id == MIXED_SOURCE_ID && mixed_is_zero(position))
			bytes[i] = 0;
		else
			bytes[i] = (unsigned char)(position * 37 + id);
	}
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
provider_range(void *argument FSIMG_UNUSED, uint64_t id, uint64_t offset,
    fsimg_range_t *range, fsimg_error_t *error FSIMG_UNUSED)
{
	/* The writer only asks for offset < file length, and each range must
	 * end within the file: a pattern file is one data range to its length
	 * (its id); the mixed file is data, zero, data split at the bounds.
	 */
	range->offset = offset;
	if (id != MIXED_SOURCE_ID) {
		range->length = id - offset;
		range->kind = FSIMG_RANGE_DATA;
	} else if (offset < MIXED_ZERO_START) {
		range->length = MIXED_ZERO_START - offset;
		range->kind = FSIMG_RANGE_DATA;
	} else if (mixed_is_zero(offset)) {
		range->length = MIXED_ZERO_END - offset;
		range->kind = FSIMG_RANGE_ZERO;
	} else {
		range->length = TREE_MIXED_BYTES - offset;
		range->kind = FSIMG_RANGE_DATA;
	}
	return (FSIMG_OK);
}

/* Memory sink callbacks. The sink's length is fixed before emission from
 * the plan's volume size; an out-of-range request is a library bug and
 * stops the test.
 */
static fsimg_status_t
sink_size(void *argument, uint64_t *length, fsimg_error_t *error FSIMG_UNUSED)
{
	image_sink_t *sink = argument;

	*length = sink->length;
	return (FSIMG_OK);
}

static fsimg_status_t
sink_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	image_sink_t *sink = argument;

	CHECK(fsimg_range_fits(offset, length, sink->length));
	bcopy(sink->bytes + offset, buffer, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
sink_write(void *argument, uint64_t offset, const void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	image_sink_t *sink = argument;

	CHECK(fsimg_range_fits(offset, length, sink->length));
	bcopy(buffer, sink->bytes + offset, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
sink_zero(void *argument, uint64_t offset, uint64_t length,
    fsimg_error_t *error FSIMG_UNUSED)
{
	image_sink_t *sink = argument;

	CHECK(fsimg_range_fits(offset, length, sink->length));
	bzero(sink->bytes + offset, (size_t)length);
	return (FSIMG_OK);
}

static fsimg_status_t
sink_flush(void *argument FSIMG_UNUSED, fsimg_error_t *error FSIMG_UNUSED)
{
	return (FSIMG_OK);
}

/* The read phase gets a view without write operations, so the reader
 * cannot alter the image it measures.
 */
static fsimg_io_t *
sink_io(fsimg_ctx_t *ctx, image_sink_t *sink, bool writable)
{
	fsimg_io_ops_t ops;
	fsimg_io_t *io;

	fsimg_io_ops_init(&ops);
	ops.get_size = sink_size;
	ops.read_at = sink_read;
	if (writable) {
		ops.write_at = sink_write;
		ops.zero_range = sink_zero;
		ops.flush = sink_flush;
	}
	CHECK(fsimg_io_from_callbacks(ctx, &ops, sink, &io, NULL) == FSIMG_OK);
	return (io);
}

static void
add_directory(fsimg_writer_t *writer, const char *path)
{
	fsimg_entry_t entry = { .path = path, .kind = FSIMG_ENTRY_DIRECTORY };

	CHECK(fsimg_writer_add(writer, &entry, NULL, NULL) == FSIMG_OK);
}

static void
add_file(fsimg_writer_t *writer, const char *path, uint64_t size,
    const fsimg_source_t *source)
{
	fsimg_entry_t entry = { .path = path, .kind = FSIMG_ENTRY_FILE,
	    .size = size };

	CHECK(fsimg_writer_add(writer, &entry, source, NULL) == FSIMG_OK);
}

/* A pattern file of positive size; an empty file takes an explicit zero
 * source because a provider id names the content length.
 */
static void
add_pattern_file(fsimg_writer_t *writer, const char *path, uint64_t size)
{
	fsimg_source_t pattern = { .kind = FSIMG_SOURCE_PROVIDER, .id = size };
	fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };

	add_file(writer, path, size, size == 0 ? &zero : &pattern);
}

/* The shared tree: long, lowercase, mixed-case and non-ASCII names that need
 * generated FAT aliases (the wide directory's names share their first
 * characters), an empty file, zero-only and mixed data/zero sources, a wide
 * directory and a deep path.
 */
static void
add_tree(fsimg_writer_t *writer)
{
	const fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };
	const fsimg_source_t mixed = { .kind = FSIMG_SOURCE_PROVIDER,
	    .id = MIXED_SOURCE_ID };
	char path[TREE_DEEP_LEVELS * 8 + 16];
	size_t used;

	add_pattern_file(writer, "/Long File Name 1.txt", 1000);
	add_pattern_file(writer, "/Long File Name 2.txt", 4097);
	add_pattern_file(writer, "/MixedCase.Txt", 33);
	add_pattern_file(writer, "/R\303\251sum\303\251.txt", 600);
	add_pattern_file(writer, "/empty", 0);
	add_file(writer, "/zero.bin", 70001, &zero);
	add_file(writer, "/mixed.bin", TREE_MIXED_BYTES, &mixed);

	/* Mostly empty entries keep the image small; every fiftieth carries a
	 * distinct amount of data so directory order and payload placement
	 * interleave.
	 */
	add_directory(writer, "/wide");
	for (unsigned i = 0; i < TREE_WIDE_ENTRIES; i++) {
		snprintf(path, sizeof(path), "/wide/entry-%04u.dat", i);
		add_pattern_file(writer, path, i % 50 == 0 ? 100 + i : 0);
	}

	used = (size_t)snprintf(path, sizeof(path), "/deep");
	add_directory(writer, path);
	for (unsigned level = 1; level < TREE_DEEP_LEVELS; level++) {
		used += (size_t)snprintf(path + used, sizeof(path) - used,
		    "/level%02u", level);
		CHECK(used < sizeof(path));
		add_directory(writer, path);
	}
	snprintf(path + used, sizeof(path) - used, "/leaf.txt");
	add_pattern_file(writer, path, 300);
}

/* Plan and emit the tree into a fresh memory sink. The sink starts poisoned,
 * so the image digest also shows that emission covers every byte. The sink
 * is owned by the caller afterwards.
 */
static void
write_tree(const char *format, int scratch, image_sink_t *sink,
    tree_record_t *record)
{
	fsimg_write_options_t options;
	fsimg_source_ops_t source_ops;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_writer_t *writer;
	fsimg_ctx_t *ctx;
	fsimg_io_t *output;
	sha256_t digest;

	ctx = tree_context(scratch);
	fsimg_write_options_init(&options);
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	CHECK(fsimg_writer_new(ctx, format, &options, &writer, NULL) == FSIMG_OK);
	fsimg_source_ops_init(&source_ops);
	source_ops.validate = provider_validate;
	source_ops.read_at = provider_read;
	source_ops.next_range = provider_range;
	CHECK(fsimg_writer_set_sources(writer, &source_ops, NULL, NULL) == FSIMG_OK);
	add_tree(writer);

	CHECK(fsimg_writer_plan(writer, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	sink->length = info.volume_bytes;
	sink->bytes = malloc((size_t)sink->length);
	CHECK(sink->bytes != NULL);
	memset(sink->bytes, SINK_POISON, (size_t)sink->length);
	output = sink_io(ctx, sink, true);
	CHECK(fsimg_writer_emit(writer, output, NULL) == FSIMG_OK);

	sha256_init(&digest);
	sha256_update(&digest, sink->bytes, (size_t)sink->length);
	sha256_final_hex(&digest, record->image_sha256);
	record->image_bytes = sink->length;
	CHECK(fsimg_ctx_get_stats(ctx, &record->write_stats, NULL) == FSIMG_OK);
	record->write_work = writer->session.progress.work;
	/* The fixture exists to measure spilled metadata; keep it spilling. */
	CHECK(record->write_stats.scratch_peak > 0);

	fsimg_io_free(output);
	fsimg_writer_free(writer);
	fsimg_ctx_free(ctx);
}

static void
dump_image(const char *fixture, const image_sink_t *sink)
{
	const char *directory;
	char path[512];
	FILE *file;

	directory = getenv(BASELINE_DUMP_ENV);
	if (directory == NULL || directory[0] == '\0')
		return;
	CHECK(snprintf(path, sizeof(path), "%s/%s.img", directory, fixture) <
	    (int)sizeof(path));
	file = fopen(path, "wbx");
	CHECK(file != NULL);
	CHECK(fwrite(sink->bytes, 1, (size_t)sink->length, file) == sink->length);
	CHECK(fclose(file) == 0);
}

/* List the image and read every file sequentially in iteration order.
 * Charged memory is sampled after each file's content is read, while that
 * file is current, so the last sample is what the reader retains for the
 * final file before the closing next and teardown.
 */
static void
read_tree(int scratch, image_sink_t *sink, tree_record_t *record)
{
	sha256_t traversal, content;
	const fsimg_entry_t *entry;
	fsimg_stats_t current;
	fsimg_reader_t *reader;
	fsimg_status_t status;
	fsimg_ctx_t *ctx;
	fsimg_io_t *input;

	ctx = tree_context(scratch);
	input = sink_io(ctx, sink, false);
	CHECK(fsimg_reader_open(ctx, input, NULL, &reader, NULL) == FSIMG_OK);
	sha256_init(&traversal);
	sha256_init(&content);
	while ((status = fsimg_reader_next(reader, &entry, NULL)) == FSIMG_OK) {
		digest_entry(&traversal, entry);
		if (entry->kind != FSIMG_ENTRY_FILE)
			continue;
		baseline_digest_read(reader, 0, entry->size, &content);
		CHECK(fsimg_ctx_get_stats(ctx, &current, NULL) == FSIMG_OK);
		record->read_memory_retained = current.memory_bytes;
	}
	CHECK(status == FSIMG_DONE);

	sha256_final_hex(&traversal, record->traversal_sha256);
	sha256_final_hex(&content, record->content_sha256);
	CHECK(fsimg_ctx_get_stats(ctx, &record->read_stats, NULL) == FSIMG_OK);
	record->read_work = reader->session.progress.work;

	fsimg_reader_free(reader);
	fsimg_io_free(input);
	fsimg_ctx_free(ctx);
}

/* One text line per entry: path, kind, size, then each valid metadata field. */
static void
digest_entry(sha256_t *digest, const fsimg_entry_t *entry)
{
	const fsimg_metadata_t *metadata = &entry->metadata;
	char line[128];
	int length;

	sha256_update(digest, entry->path, strlen(entry->path));
	length = snprintf(line, sizeof(line), " kind=%d size=%" PRIu64
	    " valid=%#" PRIx32, (int)entry->kind, entry->size, metadata->valid);
	CHECK(length > 0 && (size_t)length < sizeof(line));
	sha256_update(digest, line, (size_t)length);
	if ((metadata->valid & FSIMG_META_ATTRIBUTES) != 0) {
		length = snprintf(line, sizeof(line), " attributes=%#" PRIx32,
		    metadata->attributes);
		CHECK(length > 0 && (size_t)length < sizeof(line));
		sha256_update(digest, line, (size_t)length);
	}
	if ((metadata->valid & FSIMG_META_CREATED) != 0)
		digest_timestamp(digest, "created", &metadata->created);
	if ((metadata->valid & FSIMG_META_MODIFIED) != 0)
		digest_timestamp(digest, "modified", &metadata->modified);
	if ((metadata->valid & FSIMG_META_ACCESSED) != 0)
		digest_timestamp(digest, "accessed", &metadata->accessed);
	sha256_update(digest, "\n", 1);
}

static void
digest_timestamp(sha256_t *digest, const char *name,
    const fsimg_timestamp_t *timestamp)
{
	char text[128];
	int length;

	length = snprintf(text, sizeof(text), " %s=%" PRId64 ".%09" PRIu32
	    "/%" PRIu64 "/%d/%" PRId32, name, timestamp->seconds,
	    timestamp->nanoseconds, timestamp->precision_ns,
	    (int)timestamp->basis, timestamp->offset_minutes);
	CHECK(length > 0 && (size_t)length < sizeof(text));
	sha256_update(digest, text, (size_t)length);
}

void
baseline_digest_read(fsimg_reader_t *reader, uint64_t offset, uint64_t length,
    sha256_t *digest)
{
	unsigned char buffer[READ_CHUNK_BYTES];
	size_t request, count;

	/* read_at may transfer less than requested; continue from where it
	 * stopped. A zero count before the requested end is a premature EOF.
	 */
	while (length > 0) {
		request = length < sizeof(buffer) ? (size_t)length : sizeof(buffer);
		CHECK(fsimg_reader_read_at(reader, offset, buffer, request, &count,
		    NULL) == FSIMG_OK);
		CHECK(count > 0 && count <= request);
		sha256_update(digest, buffer, count);
		offset += count;
		length -= count;
	}
}

/* Query the range at offset and check the answer without recording its
 * granularity: it starts at offset, stays within the file, and a ZERO range
 * really reads as zeros.
 */
static void
check_range(fsimg_reader_t *reader, uint64_t offset, uint64_t size)
{
	unsigned char bytes[SCRIPT_READ_BYTES];
	fsimg_range_t range;
	size_t request, count;

	CHECK(fsimg_reader_next_range(reader, offset, &range, NULL) == FSIMG_OK);
	CHECK(range.offset == offset && range.length > 0 &&
	    range.length <= size - offset);
	CHECK(range.kind == FSIMG_RANGE_DATA || range.kind == FSIMG_RANGE_ZERO);
	if (range.kind != FSIMG_RANGE_ZERO)
		return;
	request = range.length < sizeof(bytes) ? (size_t)range.length :
	    sizeof(bytes);
	CHECK(fsimg_reader_read_at(reader, offset, bytes, request, &count,
	    NULL) == FSIMG_OK);
	CHECK(count > 0);
	for (size_t i = 0; i < count; i++)
		CHECK(bytes[i] == 0);
}

void
baseline_tree(const char *fixture, const char *format)
{
	tree_record_t record;
	image_sink_t sink;
	char directory[] = "baseline-XXXXXX";
	int scratch;

	/* One private scratch directory serves both phases. Each phase has its
	 * own fresh context, so its counters cover that phase alone. The final
	 * rmdir fails if a phase left a scratch file behind.
	 */
	CHECK(mkdtemp(directory) != NULL);
	scratch = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(scratch != -1);
	bzero(&record, sizeof(record));
	write_tree(format, scratch, &sink, &record);
	dump_image(fixture, &sink);
	read_tree(scratch, &sink, &record);
	free(sink.bytes);
	CHECK(close(scratch) == 0);
	CHECK(rmdir(directory) == 0);

	/* Fixed key order: image identity, write counters, read results, read
	 * counters. Key names decide the comparison class (see baseline.h).
	 */
	printf("fixture=%s image_sha256=%s image_bytes=%" PRIu64, fixture,
	    record.image_sha256, record.image_bytes);
	printf(" write_io_calls=%" PRIu64 " write_memory_peak=%" PRIu64
	    " write_memory_retained=%" PRIu64 " write_scratch_peak=%" PRIu64
	    " write_descriptors_peak=%" PRIu64 " write_work=%" PRIu64,
	    record.write_stats.io_calls, record.write_stats.memory_peak,
	    record.write_stats.memory_bytes, record.write_stats.scratch_peak,
	    record.write_stats.descriptors_peak, record.write_work);
	printf(" traversal_sha256=%s content_sha256=%s", record.traversal_sha256,
	    record.content_sha256);
	printf(" read_io_calls=%" PRIu64 " read_bytes=%" PRIu64
	    " read_memory_peak=%" PRIu64 " read_memory_retained=%" PRIu64
	    " read_scratch_peak=%" PRIu64 " read_descriptors_peak=%" PRIu64
	    " read_work=%" PRIu64 "\n", record.read_stats.io_calls,
	    record.read_stats.read_bytes, record.read_stats.memory_peak,
	    record.read_memory_retained, record.read_stats.scratch_peak,
	    record.read_stats.descriptors_peak, record.read_work);
}

/* The fragmented-file access script, identical for every format:
 * 1. one sequential pass over the whole file;
 * 2. short reads at fixed positions, out of order;
 * 3. a range query at each position, followed by a read at offset zero and
 *    a read at the queried position.
 * Step 3 measures how range queries interact with the sequential cursor and
 * any extent index, including costly patterns, without asserting a bound.
 * Only the bytes read enter the digest: the API leaves the granularity of a
 * data range to the implementation, so range answers are checked for
 * validity (see check_range) but not recorded.
 */
void
baseline_read_script(fsimg_reader_t *reader, uint64_t size, sha256_t *digest)
{
	static const unsigned seek_permille[] = { 999, 0, 502, 13, 587 };
	const size_t seeks = sizeof(seek_permille) / sizeof(seek_permille[0]);
	uint64_t offset;

	/* Offsets are fixed per-mille fractions of the size, visited out of
	 * order (end, start, middle). The size bound keeps every short read
	 * inside the file.
	 */
	CHECK(size >= 1000 * SCRIPT_READ_BYTES);
	baseline_digest_read(reader, 0, size, digest);

	for (size_t i = 0; i < seeks; i++) {
		offset = size / 1000 * seek_permille[i];
		baseline_digest_read(reader, offset, SCRIPT_READ_BYTES, digest);
	}

	for (size_t i = 0; i < seeks; i++) {
		offset = size / 1000 * seek_permille[i];
		check_range(reader, offset, size);
		baseline_digest_read(reader, 0, SCRIPT_READ_BYTES, digest);
		baseline_digest_read(reader, offset, SCRIPT_READ_BYTES, digest);
	}
}
