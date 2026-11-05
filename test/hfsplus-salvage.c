/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/stat.h>
#include <sys/wait.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <zlib.h>

#include "fsimg.h"
#include "hfsplus-gen.h"
#include "lzfse-vectors.h"
#include "internal.h"
#include "test.h"

enum { CHUNK = 65536, CHUNKS = 3, FILE_ID = 32 };

static const uint16_t name_f[] = { 'f' };
static const uint16_t name_z[] = { 'z' };
static const uint16_t name_d[] = { 'd' };
static const uint16_t compression_name[] = {
	'c', 'o', 'm', '.', 'a', 'p', 'p', 'l', 'e', '.',
	'd', 'e', 'c', 'm', 'p', 'f', 's'
};
static const unsigned char sibling[] = "plain sibling's own bytes";
static const char *current_test;
static unsigned failures;
static int fixture_root;
static unsigned output_number;

/* Expectations accumulate so every output and sibling is checked on failure. */
#define EXPECT(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, \
		    current_test, #condition); \
		failures++; \
	} \
} while (0)

typedef struct file_fixture {
	gen_plan_t plan;
	gen_object_t objects[6];
	gen_attr_value_t attribute;
	unsigned char header[1024];
	unsigned char *plain;
	unsigned char *resource;
	size_t size;
	size_t resource_size;
} file_fixture_t;

/* Payload bounds exclude framing and tables, so each counted read is one
 * encoded chunk. Cancellation becomes pending only once planning reads data.
 */
typedef struct extraction_control {
	uint64_t output_limit;
	uint64_t work_limit;
	uint64_t payload_start;
	uint64_t payload_end;
	bool cancel_on_payload;
} extraction_control_t;

typedef struct extraction {

	const gen_image_t *image;
	const extraction_control_t *control;
	unsigned payload_reads;
	bool extra_data;
	fsimg_status_t status;
	fsimg_error_t error;
	uint64_t salvage_losses;
	unsigned loss_count;
	char loss_paths[FSIMG_LOSS_EXAMPLES][128];
	uint32_t loss_kinds[FSIMG_LOSS_EXAMPLES];
	int output;
} extraction_t;

static void
store_le(unsigned char *p, uint64_t value, size_t width)
{
	for (size_t i = 0; i < width; i++) {
		p[i] = (unsigned char)value;
		value >>= 8;
	}
}

static uint32_t
load_le32(const unsigned char *p)
{
	return ((uint32_t)p[0] | (uint32_t)p[1] << 8 |
	    (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

/* Type 4 uses resource-file framing and offset/length pairs, unlike the
 * offset-only table gen_compressed_resource supplies for type 8.
 */
static unsigned char *
zlib_resource(const gen_compressed_chunk_t *chunks, size_t count, size_t *size)
{
	size_t cursor = 264 + 8 * count;
	unsigned char *out;

	*size = cursor + 50;
	for (size_t i = 0; i < count; i++)
		*size += chunks[i].length;
	out = calloc(1, *size);
	CHECK(out != NULL);
	store_le(out + 260, count, 4);
	for (size_t i = 0; i < count; i++) {
		store_le(out + 264 + 8 * i, cursor - 260, 4);
		store_le(out + 268 + 8 * i, chunks[i].length, 4);
		bcopy(chunks[i].bytes, out + cursor, chunks[i].length);
		cursor += chunks[i].length;
	}
	gen_store(out, 256, 4);
	gen_store(out + 4, cursor, 4);
	gen_store(out + 8, cursor - 256, 4);
	gen_store(out + 12, 50, 4);
	gen_store(out + 256, cursor - 260, 4);
	return (out);
}

/* Payload bytes vary by both chunk and position; the sibling has unrelated
 * bytes. No expected output is obtained through the library's decoder.
 */
static void
file_fixture_chunks(file_fixture_t *f, unsigned type, bool stored, size_t count)
{
	gen_compressed_chunk_t chunks[64];
	unsigned char *encoded[64];

	CHECK(count <= 64);
	bzero(f, sizeof(*f));
	f->size = type == 3 ? 127 : count * CHUNK;
	f->plain = malloc(f->size);
	CHECK(f->plain != NULL);
	for (size_t i = 0; i < f->size; i++)
		f->plain[i] = (unsigned char)(17 + i / CHUNK * 53 + i % 31);
	gen_plan_init(&f->plan);
	/* Allocate z first so it remains readable when f's storage is sliced. */
	f->objects[0] = (gen_object_t){ .name = name_z, .units = 1,
	    .id = 33, .type = 2, .data = sibling,
	    .data_size = sizeof(sibling) - 1 };
	f->objects[1] = (gen_object_t){ .name = name_f, .units = 1,
	    .id = FILE_ID, .type = 2 };
	f->plan.objects = f->objects;
	f->plan.object_count = 2;
	if (type == 0) {
		f->objects[1].data = f->plain;
		f->objects[1].data_size = f->size;
		return;
	}
	f->objects[1].flags = 4;
	f->objects[1].owner_flags = 0x20;
	bcopy("fpmc", f->header, 4);
	store_le(f->header + 4, type, 4);
	store_le(f->header + 8, f->size, 8);
	f->attribute = (gen_attr_value_t){ .id = FILE_ID, .type = 0x10,
	    .name = compression_name, .units = 17, .value = f->header,
	    .size = 16 };
	f->plan.values = &f->attribute;
	f->plan.value_count = 1;
	f->plan.attributes = true;
	if (type == 3) {
		uLongf length = sizeof(f->header) - 16;

		CHECK(compress2(f->header + 16, &length, f->plain,
		    f->size, Z_BEST_COMPRESSION) == Z_OK);
		f->attribute.size += length;
		return;
	}
	for (size_t i = 0; i < count; i++) {
		uLongf length = compressBound(CHUNK);

		encoded[i] = malloc(length + 1);
		CHECK(encoded[i] != NULL);
		if (stored || type == 8) {
			encoded[i][0] = type == 8 ? 6 : 0xff;
			bcopy(f->plain + i * CHUNK, encoded[i] + 1, CHUNK);
			length = CHUNK + 1;
		} else
			CHECK(compress2(encoded[i], &length,
			    f->plain + i * CHUNK, CHUNK,
			    Z_BEST_COMPRESSION) == Z_OK);
		chunks[i] = (gen_compressed_chunk_t){ .bytes = encoded[i],
		    .length = length };
	}
	f->resource = type == 8 ?
	    gen_compressed_resource(chunks, count, &f->resource_size) :
	    zlib_resource(chunks, count, &f->resource_size);
	f->objects[1].resource = f->resource;
	f->objects[1].resource_size = f->resource_size;
	for (size_t i = 0; i < count; i++)
		free(encoded[i]);
}

static void
file_fixture_init(file_fixture_t *f, unsigned type, bool stored)
{
	file_fixture_chunks(f, type, stored, CHUNKS);
}

static void
file_fixture_free(file_fixture_t *f)
{
	free(f->plain);
	free(f->resource);
}

static fsimg_status_t
memory_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *count, fsimg_error_t *error)
{
	extraction_t *result = argument;
	const gen_image_t *image = result->image;

	(void)error;
	if (result->control != NULL &&
	    offset >= result->control->payload_start &&
	    offset < result->control->payload_end)
		result->payload_reads++;
	CHECK(offset <= image->length && length <= image->length - offset);
	bcopy(image->bytes + offset, buffer, length);
	*count = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_size(void *argument, uint64_t *size, fsimg_error_t *error)
{
	(void)error;
	*size = ((extraction_t *)argument)->image->length;
	return (FSIMG_OK);
}

static void
record_loss(void *argument, const char *path, uint32_t kind)
{
	extraction_t *result = argument;
	unsigned i;

	if ((kind & ~FSIMG_META_KNOWN) == 0)
		return;
	i = result->loss_count++;
	CHECK(i < FSIMG_LOSS_EXAMPLES);
	CHECK(strlen(path) < sizeof(result->loss_paths[i]));
	strcpy(result->loss_paths[i], path);
	result->loss_kinds[i] = kind;
	fprintf(stderr, "%s: loss %s kind %#x\n", current_test, path, kind);
}

static bool
cancel_after_payload(void *argument)
{
	extraction_t *result = argument;

	return (result->control->cancel_on_payload && result->payload_reads != 0);
}

static void
record_entry(void *argument, const fsimg_entry_t *entry)
{
	extraction_t *result = argument;

	if (strcmp(entry->path, "/f") == 0)
		result->extra_data =
		    (entry->metadata.valid & FSIMG_META_EXTRA_DATA) != 0;
}

/* All scratch and extracted output live beneath the runner's private root. */
static fsimg_status_t
extract_image_controlled(const gen_image_t *image, extraction_t *result,
    const extraction_control_t *control)
{
	fsimg_options_t options;
	fsimg_extract_options_t extract;
	fsimg_io_ops_t ops;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	fsimg_reader_t *reader;
	fsimg_stats_t stats;
	char name[32];

	bzero(result, sizeof(*result));
	result->image = image;
	result->control = control;
	result->output = -1;
	fsimg_options_init(&options);
	options.memory_bytes = 16 * 1024 * 1024;
	options.scratch_directory_fd = fixture_root;
	options.loss = record_loss;
	options.loss_argument = result;
	options.entry = record_entry;
	options.entry_argument = result;
	if (control != NULL) {
		if (control->output_limit != 0)
			options.max_output_bytes = control->output_limit;
		options.max_work = control->work_limit;
		options.cancelled = cancel_after_payload;
		options.cancel_argument = result;
	}
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	fsimg_io_ops_init(&ops);
	ops.read_at = memory_read;
	ops.get_size = memory_size;
	CHECK(fsimg_io_from_callbacks(ctx, &ops, result, &io, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_open(ctx, io, NULL, &reader, NULL) == FSIMG_OK);
	fsimg_extract_options_init(&extract);
	extract.flags |= FSIMG_EXTRACT_SALVAGE;
	snprintf(name, sizeof(name), "out-%u", output_number++);
	result->status = fsimg_extract_at(reader, fixture_root, name, &extract,
	    &result->error);
	CHECK(fsimg_ctx_get_stats(ctx, &stats, NULL) == FSIMG_OK);
	result->salvage_losses = stats.salvage_losses;
	fsimg_reader_free(reader);
	fsimg_io_free(io);
	fsimg_ctx_free(ctx);
	result->output = openat(fixture_root, name,
	    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	EXPECT(result->output != -1);
	fprintf(stderr, "%s: extraction %s, losses %llu (%s)\n",
	    current_test, fsimg_status_string(result->status),
	    (unsigned long long)result->salvage_losses, result->error.message);
	return (result->status);
}

static fsimg_status_t
extract_image(const gen_image_t *image, extraction_t *result)
{
	return (extract_image_controlled(image, result, NULL));
}

static void
expect_file(int root, const char *name, const unsigned char *expected,
    size_t size)
{
	unsigned char bytes[4096];
	struct stat st;
	size_t offset = 0;
	ssize_t count;
	int fd;

	fd = openat(root, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd == -1)
		fprintf(stderr, "%s: missing output %s\n", current_test, name);
	EXPECT(fd != -1);
	if (fd == -1)
		return;
	CHECK(fstat(fd, &st) == 0);
	if (st.st_size != (off_t)size)
		fprintf(stderr, "%s: %s: expected %zu bytes, got %lld\n",
		    current_test, name, size, (long long)st.st_size);
	EXPECT(st.st_size == (off_t)size);
	while (offset < size) {
		size_t piece = size - offset;

		if (piece > sizeof(bytes))
			piece = sizeof(bytes);
		count = read(fd, bytes, piece);
		EXPECT(count == (ssize_t)piece);
		if (count != (ssize_t)piece)
			break;
		if (bcmp(bytes, expected + offset, piece) != 0)
			fprintf(stderr, "%s: %s: bytes differ at offset %zu\n",
			    current_test, name, offset);
		EXPECT(bcmp(bytes, expected + offset, piece) == 0);
		offset += piece;
	}
	EXPECT(read(fd, bytes, 1) == 0);
	CHECK(close(fd) == 0);
}

static void
expect_absent(int root, const char *name)
{
	struct stat st;
	int status = fstatat(root, name, &st, AT_SYMLINK_NOFOLLOW);

	if (status != -1 || errno != ENOENT)
		fprintf(stderr, "%s: expected %s absent\n", current_test, name);
	EXPECT(status == -1 && errno == ENOENT);
}

static void
expect_losses(const extraction_t *result, uint32_t kind, const char *path)
{
	unsigned count = kind == 0 ? 0 : 1;

	EXPECT(result->status == (count == 0 ? FSIMG_OK : FSIMG_EPARTIAL));
	EXPECT(result->loss_count == count);
	EXPECT(result->salvage_losses == count);
	if (count != 0 && result->loss_count != 0) {
		EXPECT(result->loss_kinds[0] == kind);
		EXPECT(strcmp(result->loss_paths[0], path) == 0);
	}
}

static void
expect_file_and_sibling(extraction_t *result, const file_fixture_t *f,
    const char *output_name, size_t prefix)
{
	expect_file(result->output, "z", sibling, sizeof(sibling) - 1);
	if (output_name != NULL)
		expect_file(result->output, output_name, f->plain, prefix);
	if (output_name == NULL || strcmp(output_name, "f") != 0)
		expect_absent(result->output, "f");
	if (output_name == NULL || strcmp(output_name, "f.partial") != 0)
		expect_absent(result->output, "f.partial");
	if (result->output != -1)
		CHECK(close(result->output) == 0);
}

static void
test_clean_compressed_fork_keeps_its_name(void)
{
	static const unsigned types[] = { 4, 8 };

	for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		file_fixture_t f;
		gen_image_t image;
		extraction_t result;

		file_fixture_init(&f, types[i], false);
		gen_build(&f.plan, &image);
		fprintf(stderr, "%s: type %u\n", current_test, types[i]);
		extract_image(&image, &result);
		expect_losses(&result, 0, NULL);
		expect_file_and_sibling(&result, &f, "f", f.size);
		gen_free(&image);
		file_fixture_free(&f);
	}
}

static void
test_clean_compressed_inline_keeps_its_name(void)
{
	file_fixture_t f;
	gen_image_t image;
	extraction_t result;

	file_fixture_init(&f, 3, false);
	gen_build(&f.plan, &image);
	extract_image(&image, &result);
	expect_losses(&result, 0, NULL);
	expect_file_and_sibling(&result, &f, "f", f.size);
	gen_free(&image);
	file_fixture_free(&f);
}

static void
test_corrupt_middle_chunk_keeps_chunks_before_it(void)
{
	file_fixture_t f;
	gen_image_t image;
	extraction_t result;
	size_t chunk;

	file_fixture_init(&f, 4, false);
	chunk = 260 + load_le32(f.resource + 264 + 8);
	f.resource[chunk] = 0; /* Invalid zlib header in logical chunk 1. */
	gen_build(&f.plan, &image);
	extract_image(&image, &result);
	expect_losses(&result, FSIMG_LOSS_SHORTENED, "/f");
	expect_file_and_sibling(&result, &f, "f.partial", CHUNK);
	gen_free(&image);
	file_fixture_free(&f);
}

static void
test_bad_table_entry_for_last_chunk_keeps_chunks_before_it(void)
{
	file_fixture_t f;
	gen_image_t image;
	extraction_t result;

	file_fixture_init(&f, 4, false);
	store_le(f.resource + 264 + 16, f.resource_size + 1, 4);
	gen_build(&f.plan, &image);
	extract_image(&image, &result);
	expect_losses(&result, FSIMG_LOSS_SHORTENED, "/f");
	expect_file_and_sibling(&result, &f, "f.partial", 2 * CHUNK);
	gen_free(&image);
	file_fixture_free(&f);
}

static void
test_corrupt_first_chunk_is_skipped_and_sibling_gets_its_own_bytes(void)
{
	file_fixture_t f;
	gen_image_t image;
	extraction_t result;

	file_fixture_init(&f, 4, false);
	f.resource[260 + load_le32(f.resource + 264)] = 0;
	gen_build(&f.plan, &image);
	extract_image(&image, &result);
	expect_losses(&result, FSIMG_LOSS_SKIPPED, "/f");
	expect_file_and_sibling(&result, &f, NULL, 0);
	gen_free(&image);
	file_fixture_free(&f);
}

static void
test_corrupt_inline_payload_is_skipped(void)
{
	file_fixture_t f;
	gen_image_t image;
	extraction_t result;

	file_fixture_init(&f, 3, false);
	f.header[16] = 0; /* Attribute is valid; its zlib payload is not. */
	gen_build(&f.plan, &image);
	extract_image(&image, &result);
	expect_losses(&result, FSIMG_LOSS_SKIPPED, "/f");
	expect_file_and_sibling(&result, &f, NULL, 0);
	gen_free(&image);
	file_fixture_free(&f);
}

static void
test_bad_resource_header_is_skipped(void)
{
	file_fixture_t f;
	gen_image_t image;
	extraction_t result;

	file_fixture_init(&f, 4, false);
	gen_store(f.resource + 12, 49, 4); /* Resource map must span 50 bytes. */
	gen_build(&f.plan, &image);
	extract_image(&image, &result);
	expect_losses(&result, FSIMG_LOSS_SKIPPED, "/f");
	expect_file_and_sibling(&result, &f, NULL, 0);
	gen_free(&image);
	file_fixture_free(&f);
}

/* Reject the first overflow record's first extent, keeping its key searchable.
 * The inline descriptor and all payload bytes remain intact.
 */
static void
break_overflow(gen_image_t *image, unsigned fork_type)
{
	gen_tree_t *tree = &image->trees[GEN_EXTENTS];

	for (size_t i = 0; i < tree->record_count; i++) {
		gen_record_t *record = &tree->records[i];

		if (gen_load(record->bytes + 4, 4) == FILE_ID &&
		    record->bytes[2] == fork_type &&
		    gen_load(record->bytes + 8, 4) == 8) {
			gen_store(image->bytes + record->offset + 16, 0, 4);
			return;
		}
	}
	CHECK(false);
}

static void
test_plain_file_keeps_extents_before_broken_overflow(void)
{
	file_fixture_t f;
	gen_image_t image;
	extraction_t result;

	file_fixture_init(&f, 0, false);
	f.objects[1].fragmented = true;
	gen_build(&f.plan, &image);
	break_overflow(&image, 0);
	extract_image(&image, &result);
	expect_losses(&result, FSIMG_LOSS_SHORTENED, "/f");
	expect_file_and_sibling(&result, &f, "f.partial", 8 * f.plan.block_size);
	gen_free(&image);
	file_fixture_free(&f);
}

static void
test_compressed_file_keeps_chunks_before_broken_resource_overflow(void)
{
	file_fixture_t f;
	gen_image_t image;
	extraction_t result;

	file_fixture_init(&f, 4, true);
	/* Eight inline blocks cover the table and chunk 0, but not chunk 1.
	 * Stored streams keep that boundary independent of zlib's encoder.
	 */
	f.plan.block_size = 16384;
	f.objects[1].fragmented = true;
	CHECK(288 + CHUNK + 1 < 8 * f.plan.block_size);
	CHECK(288 + 2 * (CHUNK + 1) > 8 * f.plan.block_size);
	gen_build(&f.plan, &image);
	break_overflow(&image, 255);
	extract_image(&image, &result);
	expect_losses(&result, FSIMG_LOSS_SHORTENED, "/f");
	expect_file_and_sibling(&result, &f, "f.partial", CHUNK);
	gen_free(&image);
	file_fixture_free(&f);
}

static unsigned char *
file_fork(gen_image_t *image, unsigned fork_offset)
{
	const gen_tree_t *tree = &image->trees[GEN_CATALOG];

	for (size_t i = 0; i < tree->record_count; i++) {
		const gen_record_t *record = &tree->records[i];
		const unsigned char *body = record->bytes + record->key_length;

		if (gen_load(body, 2) == 2 &&
		    gen_load(body + 8, 4) == FILE_ID)
			return (image->bytes + record->offset +
			    record->key_length + fork_offset);
	}
	CHECK(false);
	return (NULL);
}

static void
test_truncated_volume_shortens_plain_and_compressed(void)
{
	static const unsigned types[] = { 0, 4 };

	for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		file_fixture_t f;
		gen_image_t image, slice;
		extraction_t result;
		size_t prefix = types[i] == 0 ? CHUNK + 137 : CHUNK;
		uint64_t start;
		unsigned char *fork;

		file_fixture_init(&f, types[i], true);
		gen_build(&f.plan, &image);
		/* Metadata is packed after payload by the generator. Move f to
		 * unused tail blocks so slicing its content preserves every tree
		 * and z. Allocation bitmap verification is not an extraction step.
		 */
		start = (uint64_t)(f.plan.total_blocks - 64) * f.plan.block_size;
		for (size_t tree = 0; tree < GEN_TREE_COUNT; tree++)
			CHECK(image.trees[tree].offset +
			    (uint64_t)image.trees[tree].nodes *
			    image.trees[tree].node_size < start);
		CHECK(start + (types[i] == 0 ? f.size : f.resource_size) <
		    image.length - 1024);
		fork = file_fork(&image, types[i] == 0 ? 88 : 168);
		gen_store(fork + 16, start / f.plan.block_size, 4);
		bcopy(types[i] == 0 ? f.plain : f.resource, image.bytes + start,
		    types[i] == 0 ? f.size : f.resource_size);
		slice = image;
		/* Plain bytes stop at the cut itself. A stored compressed chunk
		 * crossing the cut contributes no decoded bytes to the prefix.
		 */
		slice.length = (size_t)start + CHUNK + 137 +
		    (types[i] == 0 ? 0 : 288 + 1);
		CHECK(slice.length < image.length);
		fprintf(stderr, "%s: %s\n", current_test,
		    types[i] == 0 ? "plain" : "compressed");
		extract_image(&slice, &result);
		expect_losses(&result, FSIMG_LOSS_SHORTENED, "/f");
		expect_file_and_sibling(&result, &f, "f.partial", prefix);
		gen_free(&image);
		file_fixture_free(&f);
	}
}

static void
test_rejected_compressed_child_loses_entry_and_keeps_siblings(void)
{
	file_fixture_t f;
	gen_image_t image;
	extraction_t result;
	struct stat st = { 0 };

	file_fixture_init(&f, 3, false);
	f.objects[0].parent = 34;
	f.objects[1].parent = 34;
	f.objects[2] = (gen_object_t){ .name = name_d, .units = 1,
	    .id = 34, .type = 1 };
	f.plan.object_count = 3;
	f.header[0] = 'x'; /* prepare_named rejects /d/f's attribute. */
	gen_build(&f.plan, &image);
	extract_image(&image, &result);
	expect_losses(&result, FSIMG_LOSS_ENTRY, "/d/f");
	expect_file(result.output, "d/z", sibling, sizeof(sibling) - 1);
	EXPECT(fstatat(result.output, "d", &st, AT_SYMLINK_NOFOLLOW) == 0);
	EXPECT(S_ISDIR(st.st_mode));
	expect_absent(result.output, "d/f");
	expect_absent(result.output, "d/f.partial");
	if (result.output != -1)
		CHECK(close(result.output) == 0);
	gen_free(&image);
	file_fixture_free(&f);
}

/* Only reads within the encoded payload count; table/header reads do not
 * stand in for decoding. The catalog fork is contiguous in these fixtures.
 */
static extraction_control_t
payload_control(gen_image_t *image, const file_fixture_t *f)
{
	unsigned char *fork = file_fork(image, 168);
	uint64_t start = gen_load(fork + 16, 4) * f->plan.block_size;

	return ((extraction_control_t){
	    .payload_start = start + 260 + load_le32(f->resource + 264),
	    .payload_end = start + f->resource_size - 50 });
}

static void
test_output_limit_stops_decoding_after_crossing_chunk(void)
{
	file_fixture_t f;
	gen_image_t image;
	extraction_t result;
	extraction_control_t control;

	file_fixture_chunks(&f, 4, false, 64);
	gen_build(&f.plan, &image);
	control = payload_control(&image, &f);
	control.output_limit = CHUNK;
	extract_image_controlled(&image, &result, &control);
	EXPECT(result.status == FSIMG_ELIMIT);
	EXPECT(result.error.resource == FSIMG_RESOURCE_OUTPUT);
	EXPECT(result.loss_count == 0 && result.salvage_losses == 0);
	fprintf(stderr, "%s: payload reads %u; expected 1..2 of 64\n",
	    current_test, result.payload_reads);
	EXPECT(result.payload_reads > 0 && result.payload_reads <= 2);
	expect_absent(result.output, "f");
	expect_absent(result.output, "f.partial");
	CHECK(close(result.output) == 0);
	gen_free(&image);
	file_fixture_free(&f);
}

static void
test_exact_output_limit_keeps_clean_compressed_name(void)
{
	file_fixture_t f;
	gen_image_t image;
	extraction_t result;
	extraction_control_t control;

	file_fixture_chunks(&f, 4, false, 64);
	gen_build(&f.plan, &image);
	control = payload_control(&image, &f);
	control.output_limit = f.size + sizeof(sibling) - 1;
	extract_image_controlled(&image, &result, &control);
	expect_losses(&result, 0, NULL);
	expect_file_and_sibling(&result, &f, "f", f.size);
	gen_free(&image);
	file_fixture_free(&f);
}

static void
test_bad_compression_attributes_keep_directory_children(void)
{
	static const uint16_t name_u[] = { 'u' };
	static const uint16_t name_s[] = { 's' };
	file_fixture_t f;
	gen_image_t image;
	extraction_t result;
	gen_attr_value_t attributes[2];
	unsigned char unknown[16];

	file_fixture_init(&f, 3, false);
	f.header[0] = 'x'; /* /d/f has a malformed decmpfs magic. */
	bcopy(f.header, unknown, sizeof(unknown));
	unknown[0] = 'f';
	store_le(unknown + 4, 99, 4); /* /d/u is well formed, unsupported. */
	attributes[0] = f.attribute;
	attributes[1] = f.attribute;
	attributes[1].id = 35;
	attributes[1].value = unknown;
	attributes[1].size = sizeof(unknown);
	f.plan.values = attributes;
	f.plan.value_count = 2;
	f.objects[0].parent = 34;
	f.objects[1].parent = 34;
	f.objects[2] = (gen_object_t){ .name = name_d, .units = 1,
	    .id = 34, .type = 1 };
	f.objects[3] = (gen_object_t){ .name = name_u, .units = 1,
	    .id = 35, .parent = 34, .type = 2, .flags = 4,
	    .owner_flags = 0x20 };
	f.objects[4] = (gen_object_t){ .name = name_s, .units = 1,
	    .id = 36, .parent = 34, .type = 1 };
	f.objects[5] = (gen_object_t){ .name = name_z, .units = 1,
	    .id = 37, .parent = 36, .type = 2, .data = sibling,
	    .data_size = sizeof(sibling) - 1 };
	f.plan.object_count = 6;
	gen_build(&f.plan, &image);
	extract_image(&image, &result);
	EXPECT(result.status == FSIMG_EPARTIAL);
	EXPECT(result.loss_count == 2 && result.salvage_losses == 2);
	for (unsigned i = 0; i < result.loss_count; i++) {
		EXPECT(result.loss_kinds[i] == FSIMG_LOSS_ENTRY);
		EXPECT(strcmp(result.loss_paths[i], i == 0 ? "/d/f" :
		    "/d/u") == 0);
	}
	expect_file(result.output, "d/z", sibling, sizeof(sibling) - 1);
	expect_file(result.output, "d/s/z", sibling, sizeof(sibling) - 1);
	expect_absent(result.output, "d/f");
	expect_absent(result.output, "d/u");
	CHECK(close(result.output) == 0);
	gen_free(&image);
	file_fixture_free(&f);
}

static void
test_plain_child_with_broken_attribute_scan_keeps_bytes_and_extra_data(void)
{
	file_fixture_t f;
	gen_image_t image;
	extraction_t result;
	const gen_record_t *record;

	file_fixture_init(&f, 3, false);
	f.objects[1].owner_flags = 0;
	f.objects[1].data = f.plain;
	f.objects[1].data_size = f.size;
	gen_build(&f.plan, &image);
	/* The inline value claims more bytes than its record contains. */
	record = &image.trees[GEN_ATTRIBUTES].records[0];
	gen_store(image.bytes + record->offset + record->key_length + 12,
	    4096, 4);
	extract_image(&image, &result);
	expect_losses(&result, 0, NULL);
	EXPECT(result.extra_data);
	expect_file_and_sibling(&result, &f, "f", f.size);
	gen_free(&image);
	file_fixture_free(&f);
}

static void
test_directory_hard_link_rejects_its_parent_listing(void)
{
	file_fixture_t f;
	gen_image_t image;
	extraction_t result;

	file_fixture_init(&f, 0, false);
	f.objects[1].parent = 34;
	f.objects[1].flags = 0x20;
	bcopy("fdrpMACS", f.objects[1].finder, 8);
	f.objects[2] = (gen_object_t){ .name = name_d, .units = 1,
	    .id = 34, .type = 1 };
	f.plan.object_count = 3;
	gen_build(&f.plan, &image);
	extract_image(&image, &result);
	expect_losses(&result, FSIMG_LOSS_DIRECTORY, "/d");
	expect_file(result.output, "z", sibling, sizeof(sibling) - 1);
	expect_absent(result.output, "d/f");
	CHECK(close(result.output) == 0);
	gen_free(&image);
	file_fixture_free(&f);
}

static void
test_bad_middle_adler_trailer_keeps_only_validated_chunk(void)
{
	file_fixture_t f;
	gen_image_t image;
	extraction_t result;
	size_t start, length;

	file_fixture_init(&f, 4, false);
	start = 260 + load_le32(f.resource + 272);
	length = load_le32(f.resource + 276);
	/* Inflate emits chunk 1 before discovering its wrong Adler-32. Those
	 * bytes must never replace the validated chunk 0 in the held window.
	 */
	f.resource[start + length - 1] ^= 1;
	gen_build(&f.plan, &image);
	extract_image(&image, &result);
	expect_losses(&result, FSIMG_LOSS_SHORTENED, "/f");
	expect_file_and_sibling(&result, &f, "f.partial", CHUNK);
	gen_free(&image);
	file_fixture_free(&f);
}

static void
test_low_middle_table_offset_rejects_previous_chunk(void)
{
	file_fixture_t f;
	gen_image_t image;
	extraction_t result;

	file_fixture_init(&f, 4, false);
	/* chunk_locate checks chunk 0's successor too. Moving chunk 1 back
	 * one byte overlaps chunk 0, so no chunk belongs to the prefix.
	 */
	store_le(f.resource + 272, load_le32(f.resource + 272) - 1, 4);
	gen_build(&f.plan, &image);
	extract_image(&image, &result);
	expect_losses(&result, FSIMG_LOSS_SKIPPED, "/f");
	expect_file_and_sibling(&result, &f, NULL, 0);
	gen_free(&image);
	file_fixture_free(&f);
}

/* Type 8 takes bare LZVN, without the vector's 12-byte LZFSE block
 * header or four-byte stream terminator. Type 12 takes the whole stream.
 * Chunk 0 is real LZFSE; LZVN uses the real short vector as chunk 0.
 */
static void
real_codec_fixture(file_fixture_t *f, unsigned type)
{
	const lzfse_vector_t *v = &lzfse_vectors[type == 8 ? 1 : 3];
	const unsigned char *encoded = v->encoded;
	size_t length = v->encoded_size;
	gen_compressed_chunk_t chunks[2];

	file_fixture_init(f, 8, false);
	free(f->resource);
	free(f->plain);
	f->size = v->plain_size * (type == 8 ? 1 : 2);
	f->plain = malloc(f->size);
	CHECK(f->plain != NULL);
	for (size_t i = 0; i < f->size; i++)
		f->plain[i] = lzfse_plain_byte(v, i % v->plain_size);
	if (type == 8) {
		encoded += 12;
		length -= 16;
	}
	chunks[0] = (gen_compressed_chunk_t){ encoded, length };
	chunks[1] = chunks[0];
	f->resource = gen_compressed_resource(chunks, type == 8 ? 1 : 2,
	    &f->resource_size);
	store_le(f->header + 4, type, 4);
	store_le(f->header + 8, f->size, 8);
	f->objects[1].resource = f->resource;
	f->objects[1].resource_size = f->resource_size;
}

static void
test_real_lzvn_bad_word_table_skips_first_chunk(void)
{
	file_fixture_t f;
	gen_image_t image;
	extraction_t result;

	real_codec_fixture(&f, 8);
	/* The ending word lies past the fork; chunk 0 cannot be located. */
	store_le(f.resource + 4, f.resource_size + 1, 4);
	gen_build(&f.plan, &image);
	extract_image(&image, &result);
	expect_losses(&result, FSIMG_LOSS_SKIPPED, "/f");
	expect_file_and_sibling(&result, &f, NULL, 0);
	gen_free(&image);
	file_fixture_free(&f);
}

static void
test_real_lzvn_bad_stream_skips_first_chunk(void)
{
	file_fixture_t f;
	gen_image_t image;
	extraction_t result;

	real_codec_fixture(&f, 8);
	/* 0x1e is an undefined LZVN opcode, replacing the first literal. */
	f.resource[load_le32(f.resource)] = 0x1e;
	gen_build(&f.plan, &image);
	extract_image(&image, &result);
	expect_losses(&result, FSIMG_LOSS_SKIPPED, "/f");
	expect_file_and_sibling(&result, &f, NULL, 0);
	gen_free(&image);
	file_fixture_free(&f);
}

static void
test_real_lzfse_bad_middle_stream_keeps_first_chunk(void)
{
	file_fixture_t f;
	gen_image_t image;
	extraction_t result;

	real_codec_fixture(&f, 12);
	/* Destroy chunk 1's bvx2 block magic; chunk 0 stays encoded. */
	f.resource[load_le32(f.resource + 4)] = 0;
	gen_build(&f.plan, &image);
	extract_image(&image, &result);
	expect_losses(&result, FSIMG_LOSS_SHORTENED, "/f");
	expect_file_and_sibling(&result, &f, "f.partial", CHUNK);
	gen_free(&image);
	file_fixture_free(&f);
}

static void
test_damaged_compressed_file_does_not_contaminate_next_compressed_file(void)
{
	file_fixture_t f, next;
	gen_image_t image;
	extraction_t result;
	gen_attr_value_t attributes[2];
	uLongf length;

	file_fixture_init(&f, 4, false);
	file_fixture_init(&next, 3, false);
	f.resource[260 + load_le32(f.resource + 272)] = 0;
	/* Distinct bytes expose a held window left over from the first file. */
	for (size_t i = 0; i < next.size; i++)
		next.plain[i] = (unsigned char)(201 + i % 17);
	length = sizeof(next.header) - 16;
	CHECK(compress2(next.header + 16, &length, next.plain, next.size,
	    Z_BEST_COMPRESSION) == Z_OK);
	next.attribute.size = 16 + length;
	f.objects[0] = next.objects[1];
	f.objects[0].name = name_z;
	f.objects[0].id = 33;
	attributes[0] = f.attribute;
	attributes[1] = next.attribute;
	attributes[1].id = 33;
	f.plan.values = attributes;
	f.plan.value_count = 2;
	gen_build(&f.plan, &image);
	extract_image(&image, &result);
	expect_losses(&result, FSIMG_LOSS_SHORTENED, "/f");
	expect_file(result.output, "f.partial", f.plain, CHUNK);
	expect_file(result.output, "z", next.plain, next.size);
	expect_absent(result.output, "f");
	expect_absent(result.output, "z.partial");
	CHECK(close(result.output) == 0);
	gen_free(&image);
	file_fixture_free(&next);
	file_fixture_free(&f);
}

static void
test_work_limit_during_compressed_plan_is_terminal(void)
{
	file_fixture_t f;
	gen_image_t image;
	extraction_t result;
	extraction_control_t control;

	file_fixture_init(&f, 4, false);
	gen_build(&f.plan, &image);
	control = payload_control(&image, &f);
	control.work_limit = 2 * CHUNK;
	extract_image_controlled(&image, &result, &control);
	EXPECT(result.status == FSIMG_ELIMIT);
	EXPECT(result.error.resource == FSIMG_RESOURCE_WORK);
	EXPECT(result.payload_reads != 0);
	EXPECT(result.loss_count == 0 && result.salvage_losses == 0);
	expect_absent(result.output, "f");
	expect_absent(result.output, "f.partial");
	CHECK(close(result.output) == 0);
	gen_free(&image);
	file_fixture_free(&f);
}

static void
test_cancellation_during_compressed_plan_is_terminal(void)
{
	file_fixture_t f;
	gen_image_t image;
	extraction_t result;
	extraction_control_t control;

	file_fixture_init(&f, 4, false);
	gen_build(&f.plan, &image);
	control = payload_control(&image, &f);
	control.cancel_on_payload = true;
	extract_image_controlled(&image, &result, &control);
	EXPECT(result.status == FSIMG_ECANCELLED);
	EXPECT(result.payload_reads == 1);
	EXPECT(result.loss_count == 0 && result.salvage_losses == 0);
	expect_absent(result.output, "f");
	expect_absent(result.output, "f.partial");
	CHECK(close(result.output) == 0);
	gen_free(&image);
	file_fixture_free(&f);
}

static void
test_broken_overflow_past_file_end_keeps_clean_name(void)
{
	file_fixture_t f;
	gen_image_t image;
	extraction_t result;
	unsigned char *fork;

	file_fixture_init(&f, 0, false);
	f.objects[1].fragmented = true;
	gen_build(&f.plan, &image);
	break_overflow(&image, 0);
	/* Retain allocation and its broken overflow but end the logical file
	 * in its first block. No unavailable byte belongs to this file.
	 */
	fork = file_fork(&image, 88);
	gen_store(fork, 137, 8);
	extract_image(&image, &result);
	expect_losses(&result, 0, NULL);
	expect_file_and_sibling(&result, &f, "f", 137);
	gen_free(&image);
	file_fixture_free(&f);
}

static void
remove_children(int root)
{
	DIR *directory;
	struct dirent *entry;
	struct stat st;
	int child;

	directory = fdopendir(dup(root));
	CHECK(directory != NULL);
	while ((entry = readdir(directory)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0)
			continue;
		CHECK(fstatat(root, entry->d_name, &st,
		    AT_SYMLINK_NOFOLLOW) == 0);
		if (S_ISDIR(st.st_mode)) {
			child = openat(root, entry->d_name,
			    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			CHECK(child != -1);
			remove_children(child);
			CHECK(close(child) == 0);
			CHECK(unlinkat(root, entry->d_name, AT_REMOVEDIR) == 0);
		} else
			CHECK(unlinkat(root, entry->d_name, 0) == 0);
	}
	CHECK(closedir(directory) == 0);
}

/* Isolate setup failures as well as assertions; the parent always removes
 * the private fixture, even when a child exits at a failed CHECK.
 */
static unsigned
run_case(const char *name, void (*test)(void))
{
	char temporary[] = "/tmp/fsimg-hfsplus-salvage-XXXXXX";
	pid_t child;
	int status;
	bool passed;

	CHECK(mkdtemp(temporary) != NULL);
	fixture_root = open(temporary, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fixture_root != -1);
	current_test = name;
	fflush(NULL);
	child = fork();
	CHECK(child != -1);
	if (child == 0) {
		failures = 0;
		output_number = 0;
		test();
		exit(failures == 0 ? 0 : 1);
	}
	CHECK(waitpid(child, &status, 0) == child);
	remove_children(fixture_root);
	CHECK(close(fixture_root) == 0);
	CHECK(rmdir(temporary) == 0);
	passed = WIFEXITED(status) && WEXITSTATUS(status) == 0;
	fprintf(stderr, "%s: %s\n", name, passed ? "PASS" : "FAIL");
	return (passed ? 0 : 1);
}

#define RUN(test) (failed += run_case(#test, test))

int
main(void)
{
	unsigned failed = 0;

	RUN(test_clean_compressed_fork_keeps_its_name);
	RUN(test_clean_compressed_inline_keeps_its_name);
	RUN(test_corrupt_middle_chunk_keeps_chunks_before_it);
	RUN(test_bad_table_entry_for_last_chunk_keeps_chunks_before_it);
	RUN(test_corrupt_first_chunk_is_skipped_and_sibling_gets_its_own_bytes);
	RUN(test_corrupt_inline_payload_is_skipped);
	RUN(test_bad_resource_header_is_skipped);
	RUN(test_plain_file_keeps_extents_before_broken_overflow);
	RUN(test_compressed_file_keeps_chunks_before_broken_resource_overflow);
	RUN(test_truncated_volume_shortens_plain_and_compressed);
	RUN(test_rejected_compressed_child_loses_entry_and_keeps_siblings);
	RUN(test_output_limit_stops_decoding_after_crossing_chunk);
	RUN(test_exact_output_limit_keeps_clean_compressed_name);
	RUN(test_bad_compression_attributes_keep_directory_children);
	RUN(test_plain_child_with_broken_attribute_scan_keeps_bytes_and_extra_data);
	RUN(test_directory_hard_link_rejects_its_parent_listing);
	RUN(test_bad_middle_adler_trailer_keeps_only_validated_chunk);
	RUN(test_low_middle_table_offset_rejects_previous_chunk);
	RUN(test_real_lzvn_bad_word_table_skips_first_chunk);
	RUN(test_real_lzvn_bad_stream_skips_first_chunk);
	RUN(test_real_lzfse_bad_middle_stream_keeps_first_chunk);
	RUN(test_damaged_compressed_file_does_not_contaminate_next_compressed_file);
	RUN(test_work_limit_during_compressed_plan_is_terminal);
	RUN(test_cancellation_during_compressed_plan_is_terminal);
	RUN(test_broken_overflow_past_file_end_keeps_clean_name);
	fprintf(stderr, "%u of 25 cases failed\n", failed);
	return (failed == 0 ? 0 : 1);
}
