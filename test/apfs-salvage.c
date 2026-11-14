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

#include "apfs-gen.h"
#include "fsimg.h"
#include "internal.h"
#include "test.h"

enum { BLOCK_BYTES = 4096, FILE_BYTES = 3 * BLOCK_BYTES };

typedef struct image {
	unsigned char *bytes;
	size_t length;
} image_t;

typedef struct extraction {
	const image_t *image;
	fsimg_status_t status;
	fsimg_error_t error;
	uint64_t salvage_losses;
	unsigned loss_count;
	char loss_paths[FSIMG_LOSS_EXAMPLES][128];
	uint32_t loss_kinds[FSIMG_LOSS_EXAMPLES];
	int output;
} extraction_t;

static const char *current_test;
static unsigned failures;
static int fixture_root;
static unsigned output_number;
static unsigned char plain[FILE_BYTES];
static const unsigned char sibling[] = "sibling bytes";

#define EXPECT(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, \
		    current_test, #condition); \
		failures++; \
	} \
} while (0)

static uint64_t load_le(const unsigned char *, unsigned);
static void store_le(unsigned char *, uint64_t, unsigned);
static apfs_gen_volume_t *build_image(image_t *, bool, uint16_t);
static unsigned char *record_value(const apfs_gen_volume_t *, image_t *,
    uint64_t, uint8_t, uint64_t, unsigned char **);
static fsimg_status_t memory_read(void *, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t memory_size(void *, uint64_t *, fsimg_error_t *);
static void record_loss(void *, const char *, uint32_t);
static void extract_image(const image_t *, extraction_t *, bool);
static void expect_file(int, const char *, const unsigned char *, size_t);
static void expect_absent(int, const char *);
static void expect_losses(const extraction_t *, uint32_t, const char *);
static void expect_siblings(const extraction_t *);
static void test_bad_middle_extent_keeps_only_prefix(void);
static void test_bad_child_leaf_checksum_keeps_other_leaves(void);
static void test_bad_inode_skips_only_named_child(void);
static void test_bad_attribute_skips_only_named_child(void);
static void test_overlapping_successor_shortens_to_the_overlapped_extent(void);
static void test_bad_leaf_holding_file_extents_keeps_prefix(void);
static bool remove_children(int);
static unsigned run_case(const char *, void (*)(void), unsigned);

static uint64_t
load_le(const unsigned char *p, unsigned width)
{
	uint64_t value = 0;

	for (unsigned i = 0; i < width; i++)
		value |= (uint64_t)p[i] << (8 * i);
	return (value);
}

static void
store_le(unsigned char *p, uint64_t value, unsigned width)
{
	for (unsigned i = 0; i < width; i++)
		p[i] = (unsigned char)(value >> (8 * i));
}

static apfs_gen_volume_t *
build_image(image_t *image, bool attribute, uint16_t leaf_records)
{
	apfs_gen_volume_t *volume;
	extraction_t clean;
	uint64_t a, f, z;

	volume = apfs_gen_volume_new();
	CHECK(volume != NULL);
	CHECK(apfs_gen_volume_layout(volume, leaf_records, 1));
	CHECK(apfs_gen_volume_file(volume, 100, 2, "a", sibling,
	    sizeof(sibling) - 1));
	CHECK(apfs_gen_volume_file(volume, 200, 2, "f", plain, sizeof(plain)));
	/* Put both sibling streams before f, outside its damaged leaves. */
	CHECK(apfs_gen_volume_file(volume, 150, 2, "z", sibling,
	    sizeof(sibling) - 1));
	if (attribute)
		CHECK(apfs_gen_volume_attribute(volume, 200));
	CHECK(apfs_gen_volume_build(volume, &image->bytes, &image->length));
	CHECK(apfs_gen_volume_record_block(volume, 100, APFS_GEN_KIND_INODE, 0,
	    &a));
	CHECK(apfs_gen_volume_record_block(volume, 200, APFS_GEN_KIND_INODE, 0,
	    &f));
	CHECK(apfs_gen_volume_record_block(volume, 150, APFS_GEN_KIND_INODE, 0,
	    &z));
	CHECK(a != f && f != z && a != z);
	/* Prove the same image extracts intact before introducing damage. */
	extract_image(image, &clean, true);
	expect_losses(&clean, 0, NULL);
	expect_file(clean.output, "f", plain, sizeof(plain));
	expect_siblings(&clean);
	CHECK(close(clean.output) == 0);
	return (volume);
}

/* Decode the generator's variable TOC independently of the reader. Keys
 * start after the table and value offsets count backward from the node end.
 */
static unsigned char *
record_value(const apfs_gen_volume_t *volume, image_t *image, uint64_t oid,
    uint8_t kind, uint64_t suffix, unsigned char **node)
{
	uint64_t block, key_word;
	unsigned char *toc, *key;
	size_t key_start, value_end;

	CHECK(apfs_gen_volume_record_block(volume, oid, kind, suffix, &block));
	CHECK(block < image->length / BLOCK_BYTES);
	*node = image->bytes + block * BLOCK_BYTES;
	key_start = GEN_APFS_NODE_HEADER + load_le(*node + 42, 2);
	value_end = BLOCK_BYTES - ((load_le(*node + 32, 2) &
	    GEN_APFS_ROOT) != 0 ? GEN_APFS_FOOTER : 0);
	for (unsigned i = 0; i < load_le(*node + 36, 4); i++) {
		toc = *node + GEN_APFS_NODE_HEADER + i * 8;
		key = *node + key_start + load_le(toc, 2);
		key_word = load_le(key, 8);
		if ((key_word & GEN_JKEY_OID_MASK) != oid ||
		    key_word >> 60 != kind)
			continue;
		if (kind == APFS_GEN_KIND_FILE_EXTENT &&
		    load_le(key + 8, 8) != suffix)
			continue;
		return (*node + value_end - load_le(toc + 4, 2));
	}
	CHECK(false);
	return (NULL);
}

static fsimg_status_t
memory_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *count, fsimg_error_t *error)
{
	extraction_t *result = argument;
	const image_t *image = result->image;

	(void)error;
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

static void
extract_image(const image_t *image, extraction_t *result, bool salvage)
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
	fsimg_options_init(&options);
	options.scratch_directory_fd = fixture_root;
	options.loss = record_loss;
	options.loss_argument = result;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	fsimg_io_ops_init(&ops);
	ops.read_at = memory_read;
	ops.get_size = memory_size;
	CHECK(fsimg_io_from_callbacks(ctx, &ops, result, &io, NULL) ==
	    FSIMG_OK);
	CHECK(fsimg_reader_open(ctx, io, NULL, &reader, NULL) == FSIMG_OK);
	fsimg_extract_options_init(&extract);
	if (salvage)
		extract.flags |= FSIMG_EXTRACT_SALVAGE;
	CHECK(snprintf(name, sizeof(name), "out-%u", output_number++) > 0);
	result->status = fsimg_extract_at(reader, fixture_root, name, &extract,
	    &result->error);
	CHECK(fsimg_ctx_get_stats(ctx, &stats, NULL) == FSIMG_OK);
	result->salvage_losses = stats.salvage_losses;
	fsimg_reader_free(reader);
	fsimg_io_free(io);
	fsimg_ctx_free(ctx);
	result->output = openat(fixture_root, name,
	    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	CHECK(result->output != -1);
	fprintf(stderr, "%s: extraction %s, losses %llu (%s)\n",
	    current_test, fsimg_status_string(result->status),
	    (unsigned long long)result->salvage_losses, result->error.message);
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
		if (bcmp(bytes, expected + offset, piece) != 0) {
			size_t different = 0;

			while (different < piece &&
			    bytes[different] == expected[offset + different])
				different++;
			fprintf(stderr,
			    "%s: %s: offset %zu expected byte %u got %u\n",
			    current_test, name, offset + different,
			    expected[offset + different], bytes[different]);
		}
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
	fprintf(stderr,
	    "%s: expected %u losses, observed %u callbacks/%llu total\n",
	    current_test, count, result->loss_count,
	    (unsigned long long)result->salvage_losses);
	EXPECT(result->loss_count == count);
	EXPECT(result->salvage_losses == count);
	if (count != 0 && result->loss_count != 0) {
		EXPECT(result->loss_kinds[0] == kind);
		EXPECT(strcmp(result->loss_paths[0], path) == 0);
	}
}

static void
expect_siblings(const extraction_t *result)
{
	expect_file(result->output, "a", sibling, sizeof(sibling) - 1);
	expect_file(result->output, "z", sibling, sizeof(sibling) - 1);
}

static void
test_bad_middle_extent_keeps_only_prefix(void)
{
	apfs_gen_volume_t *volume;
	image_t image;
	extraction_t result;
	unsigned char *node, *value;

	volume = build_image(&image, false, 1);
	value = record_value(volume, &image, 200, APFS_GEN_KIND_FILE_EXTENT,
	    BLOCK_BYTES, &node);
	/* Point the extent value's phys_block beyond the image. */
	store_le(value + 8, image.length / BLOCK_BYTES, 8);
	CHECK(apfs_gen_seal(node, BLOCK_BYTES));

	extract_image(&image, &result, true);
	expect_losses(&result, FSIMG_LOSS_SHORTENED, "/f");
	expect_file(result.output, "f.partial", plain, BLOCK_BYTES);
	expect_absent(result.output, "f");
	expect_siblings(&result);
	CHECK(close(result.output) == 0);

	extract_image(&image, &result, false);
	EXPECT(result.status == FSIMG_ECORRUPT);
	EXPECT(strstr(result.error.message, "file extent") != NULL);
	EXPECT(strcmp(result.error.path, "/f") == 0);
	EXPECT(result.loss_count == 0 && result.salvage_losses == 0);
	CHECK(close(result.output) == 0);
	free(image.bytes);
	apfs_gen_volume_free(volume);
}

static void
test_bad_child_leaf_checksum_keeps_other_leaves(void)
{
	apfs_gen_volume_t *volume;
	image_t image;
	extraction_t result;
	unsigned char *node;

	volume = build_image(&image, false, 1);
	(void)record_value(volume, &image, 200, APFS_GEN_KIND_INODE,
	    0, &node);
	node[0] ^= 1;

	extract_image(&image, &result, true);
	expect_losses(&result, FSIMG_LOSS_ENTRY, "/f");
	expect_absent(result.output, "f");
	expect_absent(result.output, "f.partial");
	expect_siblings(&result);
	CHECK(close(result.output) == 0);

	extract_image(&image, &result, false);
	EXPECT(result.status == FSIMG_ECORRUPT);
	CHECK(close(result.output) == 0);
	free(image.bytes);
	apfs_gen_volume_free(volume);
}

static void
test_bad_inode_skips_only_named_child(void)
{
	apfs_gen_volume_t *volume;
	image_t image;
	extraction_t result;
	unsigned char *node, *value;

	volume = build_image(&image, false, 1);
	value = record_value(volume, &image, 200, APFS_GEN_KIND_INODE,
	    0, &node);
	/* The extended-field count cannot fit in the inode value. */
	store_le(value + GEN_INODE_VAL_BYTES, UINT16_MAX, 2);
	CHECK(apfs_gen_seal(node, BLOCK_BYTES));

	extract_image(&image, &result, true);
	expect_losses(&result, FSIMG_LOSS_ENTRY, "/f");
	expect_absent(result.output, "f");
	expect_absent(result.output, "f.partial");
	expect_siblings(&result);
	CHECK(close(result.output) == 0);
	free(image.bytes);
	apfs_gen_volume_free(volume);
}

static void
test_bad_attribute_skips_only_named_child(void)
{
	apfs_gen_volume_t *volume;
	image_t image;
	extraction_t result;
	unsigned char *node, *value;

	volume = build_image(&image, true, 1);
	value = record_value(volume, &image, 200, APFS_GEN_KIND_XATTR,
	    0, &node);
	/* The inline length exceeds the bytes in the attribute value. */
	store_le(value + 2, UINT16_MAX, 2);
	CHECK(apfs_gen_seal(node, BLOCK_BYTES));

	/* design-salvage.md, "Directories and reader state": a decoder failure
	 * during entry replay skips the named entry. APFS validates every
	 * attribute header while preparing the inode, even unserved attributes.
	 */
	extract_image(&image, &result, true);
	expect_losses(&result, FSIMG_LOSS_ENTRY, "/f");
	expect_absent(result.output, "f");
	expect_absent(result.output, "f.partial");
	expect_siblings(&result);
	CHECK(close(result.output) == 0);
	free(image.bytes);
	apfs_gen_volume_free(volume);
}

/* The final extent's valid successor overlaps bytes it would deliver. */
static void
test_overlapping_successor_shortens_to_the_overlapped_extent(void)
{
	apfs_gen_volume_t *volume;
	image_t image;
	extraction_t result;
	unsigned char *node, *value;

	volume = build_image(&image, false, 1);
	value = record_value(volume, &image, 200, APFS_GEN_KIND_FILE_EXTENT,
	    BLOCK_BYTES, &node);
	/* Keep the third record at 8 KiB; extend the second over [4K, 12K). */
	store_le(value, 2 * BLOCK_BYTES, 8);
	CHECK(apfs_gen_seal(node, BLOCK_BYTES));

	extract_image(&image, &result, false);
	EXPECT(result.status == FSIMG_ECORRUPT);
	CHECK(close(result.output) == 0);

	extract_image(&image, &result, true);
	expect_losses(&result, FSIMG_LOSS_SHORTENED, "/f");
	expect_file(result.output, "f.partial", plain, BLOCK_BYTES);
	expect_absent(result.output, "f");
	expect_siblings(&result);
	CHECK(close(result.output) == 0);
	free(image.bytes);
	apfs_gen_volume_free(volume);
}

static void
test_bad_leaf_holding_file_extents_keeps_prefix(void)
{
	apfs_gen_volume_t *volume;
	image_t image;
	extraction_t result;
	uint64_t first, later, last;

	/* The first extent ends one leaf; the next two fill the next leaf. */
	volume = build_image(&image, false, 2);
	CHECK(apfs_gen_volume_record_block(volume, 200,
	    APFS_GEN_KIND_FILE_EXTENT, 0, &first));
	CHECK(apfs_gen_volume_record_block(volume, 200,
	    APFS_GEN_KIND_FILE_EXTENT, BLOCK_BYTES, &later));
	CHECK(apfs_gen_volume_record_block(volume, 200,
	    APFS_GEN_KIND_FILE_EXTENT, 2 * BLOCK_BYTES, &last));
	CHECK(first < later && later == last);
	image.bytes[later * BLOCK_BYTES] ^= 1;

	extract_image(&image, &result, false);
	EXPECT(result.status == FSIMG_ECORRUPT);
	CHECK(close(result.output) == 0);

	extract_image(&image, &result, true);
	expect_losses(&result, FSIMG_LOSS_SHORTENED, "/f");
	expect_file(result.output, "f.partial", plain, BLOCK_BYTES);
	expect_absent(result.output, "f");
	expect_siblings(&result);
	CHECK(close(result.output) == 0);
	free(image.bytes);
	apfs_gen_volume_free(volume);
}

/* Keep removing reachable children after an error; never assert in cleanup. */
static bool
remove_children(int root)
{
	DIR *directory;
	struct dirent *entry;
	struct stat st;
	int child, fd;
	bool removed = true;

	fd = openat(root, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd == -1)
		return (false);
	directory = fdopendir(fd);
	if (directory == NULL) {
		close(fd);
		return (false);
	}
	for (;;) {
		errno = 0;
		entry = readdir(directory);
		if (entry == NULL) {
			if (errno != 0)
				removed = false;
			break;
		}
		if (strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0)
			continue;
		if (fstatat(root, entry->d_name, &st,
		    AT_SYMLINK_NOFOLLOW) != 0) {
			removed = false;
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			child = openat(root, entry->d_name,
			    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			if (child == -1) {
				removed = false;
				continue;
			}
			if (!remove_children(child))
				removed = false;
			if (close(child) != 0)
				removed = false;
			if (unlinkat(root, entry->d_name, AT_REMOVEDIR) != 0)
				removed = false;
		} else if (unlinkat(root, entry->d_name, 0) != 0)
			removed = false;
	}
	if (closedir(directory) != 0)
		removed = false;
	return (removed);
}

/* Isolate setup failures as well as assertions; record parent validation
 * failures without exiting, then remove the private fixture before failing.
 */
static unsigned
run_case(const char *name, void (*test)(void), unsigned expected_outputs)
{
	char temporary[] = "apfs-salvage-XXXXXX";
	char output_name[32];
	DIR *directory;
	struct dirent *entry;
	pid_t child, waited;
	unsigned count = 0;
	int fd, status = 0;
	bool known;

	current_test = name;
	failures = 0;
	/* Use the build test directory for scratch, like apfs-faults. */
	CHECK(mkdtemp(temporary) != NULL);
	fixture_root = open(temporary, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	EXPECT(fixture_root != -1);
	if (fixture_root == -1)
		goto remove_fixture;
	fflush(NULL);
	child = fork();
	EXPECT(child != -1);
	if (child == -1)
		goto cleanup;
	if (child == 0) {
		failures = 0;
		output_number = 0;
		test();
		exit(failures == 0 ? 0 : 1);
	}
	do {
		waited = waitpid(child, &status, 0);
	} while (waited == -1 && errno == EINTR);
	EXPECT(waited == child);
	if (waited == child)
		EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);

	fd = dup(fixture_root);
	directory = fd == -1 ? NULL : fdopendir(fd);
	EXPECT(directory != NULL);
	if (directory == NULL) {
		if (fd != -1)
			EXPECT(close(fd) == 0);
		goto cleanup;
	}
	for (;;) {
		errno = 0;
		entry = readdir(directory);
		if (entry == NULL) {
			EXPECT(errno == 0);
			break;
		}
		if (strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0)
			continue;
		known = false;
		for (unsigned i = 0; i < expected_outputs; i++) {
			snprintf(output_name, sizeof(output_name), "out-%u", i);
			if (strcmp(entry->d_name, output_name) == 0)
				known = true;
		}
		EXPECT(known);
		count++;
	}
	EXPECT(count == expected_outputs);
	EXPECT(closedir(directory) == 0);
 cleanup:
	EXPECT(remove_children(fixture_root));
	EXPECT(close(fixture_root) == 0);
 remove_fixture:
	EXPECT(rmdir(temporary) == 0);
	fprintf(stderr, "%s: %s\n", name, failures == 0 ? "PASS" : "FAIL");
	return (failures == 0 ? 0 : 1);
}

#define RUN(test, outputs) (failed += run_case(#test, test, outputs))

int
main(void)
{
	unsigned failed = 0;

	for (size_t i = 0; i < sizeof(plain); i++)
		plain[i] = (unsigned char)(i * 17 + 3);
	RUN(test_bad_middle_extent_keeps_only_prefix, 3);
	RUN(test_bad_child_leaf_checksum_keeps_other_leaves, 3);
	RUN(test_bad_inode_skips_only_named_child, 2);
	RUN(test_bad_attribute_skips_only_named_child, 2);
	RUN(test_overlapping_successor_shortens_to_the_overlapped_extent, 3);
	RUN(test_bad_leaf_holding_file_extents_keeps_prefix, 3);
	fprintf(stderr, "%u of 6 cases failed\n", failed);
	return (failed == 0 ? 0 : 1);
}
