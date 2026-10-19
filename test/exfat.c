/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "baseline.h"
#include "exfat.h"
#include "selection.h"
#include "test.h"
#include "write-exfat.h"

#define IMAGE_BYTES 1048576u
#define HEAP_SECTOR 40u
#define HEAP_CLUSTERS (2048u - HEAP_SECTOR)
#define ROOT_CLUSTER 259u
#define FILE_CLUSTER 260u

typedef struct exfat_fixture {
	fsimg_ctx_t	*ctx;
	fsimg_io_t	*io;
	fsimg_reader_t	*reader;
	unsigned char	*image;
	uint64_t	 length;
	uint64_t	 calls;
	bool		 cancelled;
	bool		 fail_payload;
	int		 scratch;
	char		 directory[64];
} exfat_fixture_t;

static void put16(unsigned char *, uint16_t);
static void put32(unsigned char *, uint32_t);
static void put64(unsigned char *, uint64_t);
static uint32_t rotate_sum(const unsigned char *, size_t, unsigned);
static size_t cluster_offset(unsigned);
static unsigned char *file_set(exfat_fixture_t *);
static void fix_boot(exfat_fixture_t *);
static void fix_set(exfat_fixture_t *);
static void make_image(exfat_fixture_t *);
static void compressed_upcase(exfat_fixture_t *, unsigned);
static bool cancelled(void *);
static fsimg_status_t image_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static fsimg_status_t image_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t image_write(void *, uint64_t, const void *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t image_zero(void *, uint64_t, uint64_t, fsimg_error_t *);
static void fixture_new(exfat_fixture_t *);
static void fixture_free(exfat_fixture_t *);
static const fsimg_entry_t *open_file(exfat_fixture_t *);
static void test_independent(void);
static void test_corruptions(void);
static void test_allocation_profiles(void);
static void test_upcase_profiles(void);
static void test_allocation_failures(void);
static void test_content_guard(const char *);
static void test_metadata_boundary(const char *);
static void test_benign_entries(bool);
static void test_label_validation(void);
static void test_upcase_work_limit(void);
static void test_writer(void);
static void test_writer_names(void);
static void test_timestamp_fidelity(void);
static void test_optional_timestamp_default(void);
static void test_reproducible(void);
static void test_stream_limits(void);
static void test_sequential_fragments(void);
static void test_extract_zero_tail(void);
static uint32_t fragment_cluster(unsigned, unsigned);
static void make_fragmented_file(exfat_fixture_t *, unsigned);
static void baseline_zero_tail(void);
static void baseline_fragmented(void);
static void emit_external(const char *);
static void read_external(const char *);
static void emit_seed(const char *);

/* Fixture geometry, bitmaps and checksums are encoded directly. Neither the
 * production planner nor its codecs construct the independent reader oracle.
 */
static void
put16(unsigned char *p, uint16_t value)
{
	p[0] = (unsigned char)value;
	p[1] = (unsigned char)(value / 256);
}

static void
put32(unsigned char *p, uint32_t value)
{
	for (unsigned i = 0; i < 4; i++) {
		p[i] = (unsigned char)value;
		value /= 256;
	}
}

static void
put64(unsigned char *p, uint64_t value)
{
	for (unsigned i = 0; i < 8; i++) {
		p[i] = (unsigned char)value;
		value /= 256;
	}
}

static uint32_t
rotate_sum(const unsigned char *bytes, size_t length, unsigned bits)
{
	uint64_t sum = 0;
	uint64_t mask = bits == 16 ? 65535 : UINT32_MAX;

	for (size_t i = 0; i < length; i++)
		sum = (sum / 2 + (sum % 2) * ((mask + 1) / 2) + bytes[i]) & mask;
	return ((uint32_t)sum);
}

static size_t
cluster_offset(unsigned cluster)
{
	return ((HEAP_SECTOR + cluster - 2) * 512);
}

static unsigned char *
file_set(exfat_fixture_t *f)
{
	return (f->image + cluster_offset(ROOT_CLUSTER) + 96);
}

static void
fix_boot(exfat_fixture_t *f)
{
	unsigned char copy[11 * 512];
	uint32_t checksum;

	bcopy(f->image, copy, sizeof(copy));
	copy[106] = copy[107] = copy[112] = 0;
	/* Excluded bytes do not rotate the sum either. */
	checksum = 0;
	for (size_t i = 0; i < sizeof(copy); i++) {
		if (i == 106 || i == 107 || i == 112)
			continue;
		checksum = checksum / 2 + (checksum % 2) * UINT32_C(0x80000000) + copy[i];
	}
	for (unsigned i = 0; i < 512; i += 4)
		put32(f->image + 11 * 512 + i, checksum);
	bcopy(f->image, f->image + 12 * 512, 12 * 512);
}

static void
fix_set(exfat_fixture_t *f)
{
	unsigned char *set = file_set(f);
	uint16_t checksum = 0;
	size_t length = ((size_t)set[1] + 1) * 32;

	for (size_t i = 0; i < length; i++) {
		if (i != 2 && i != 3)
			checksum = (uint16_t)(checksum / 2 + (checksum % 2) * 32768 + set[i]);
	}
	put16(set + 2, checksum);
}

static void
make_image(exfat_fixture_t *f)
{
	unsigned char *boot, *fat, *bitmap, *upcase, *root, *set;
	unsigned char upper_name[16];
	const char name[] = "Data.bin";
	uint16_t mapped;

	bzero(f->image, IMAGE_BYTES);
	boot = f->image;
	fat = f->image + 24 * 512;
	bitmap = f->image + cluster_offset(2);
	upcase = f->image + cluster_offset(3);
	root = f->image + cluster_offset(ROOT_CLUSTER);
	set = file_set(f);
	bcopy("\xeb\x76\x90" "EXFAT   ", boot, 11);
	put64(boot + 72, 2048);
	put32(boot + 80, 24);
	put32(boot + 84, 16);
	put32(boot + 88, HEAP_SECTOR);
	put32(boot + 92, HEAP_CLUSTERS);
	put32(boot + 96, ROOT_CLUSTER);
	put16(boot + 104, 0x0100);
	boot[108] = 9;
	boot[110] = 1;
	boot[112] = 255;
	boot[510] = 0x55;
	boot[511] = 0xaa;
	for (unsigned i = 1; i <= 8; i++)
		put32(boot + (i + 1) * 512 - 4, UINT32_C(0xaa550000));
	put32(fat, UINT32_C(0xfffffff8));
	put32(fat + 4, UINT32_MAX);
	put32(fat + 2 * 4, UINT32_MAX);
	for (unsigned i = 3; i < 259; i++)
		put32(fat + i * 4, i == 258 ? UINT32_MAX : i + 1);
	put32(fat + ROOT_CLUSTER * 4, UINT32_MAX);
	put32(fat + FILE_CLUSTER * 4, FILE_CLUSTER + 2);
	put32(fat + (FILE_CLUSTER + 2) * 4, UINT32_MAX);
	for (unsigned i = 2; i <= FILE_CLUSTER + 2; i++) {
		if (i != FILE_CLUSTER + 1)
			bitmap[(i - 2) / 8] |= (unsigned char)(1u << ((i - 2) % 8));
	}
	for (unsigned i = 0; i < 65536; i++) {
		mapped = (uint16_t)(i >= 'a' && i <= 'z' ? i - 32 : i);
		put16(upcase + i * 2, mapped);
	}
	root[0] = 0x81;
	put32(root + 20, 2);
	put64(root + 24, (HEAP_CLUSTERS + 7) / 8);
	root[32] = 0x82;
	put32(root + 36, rotate_sum(upcase, 131072, 32));
	put32(root + 52, 3);
	put64(root + 56, 131072);
	root[64] = 0x83;
	root[65] = 4;
	for (unsigned i = 0; i < 4; i++)
		put16(root + 66 + i * 2, (uint16_t)"TEST"[i]);
	set[0] = 0x85;
	set[1] = 2;
	put16(set + 4, 32);
	for (unsigned i = 0; i < 3; i++) {
		put32(set + 8 + i * 4, UINT32_C(0x00210000));
		set[22 + i] = 0x80;
	}
	set[32] = 0xc0;
	set[33] = 1;
	set[35] = 8;
	put64(set + 40, 513);
	put32(set + 52, FILE_CLUSTER);
	put64(set + 56, 1024);
	set[64] = 0xc1;
	for (unsigned i = 0; i < 8; i++) {
		put16(set + 66 + i * 2, (uint16_t)name[i]);
		upper_name[i * 2] = (unsigned char)(name[i] >= 'a' && name[i] <= 'z' ?
		    name[i] - 32 : name[i]);
		upper_name[i * 2 + 1] = 0;
	}
	put16(set + 36, (uint16_t)rotate_sum(upper_name, sizeof(upper_name), 16));
	fix_set(f);
	fix_boot(f);
	for (unsigned i = 0; i < 512; i++)
		f->image[cluster_offset(FILE_CLUSTER) + i] = (unsigned char)(i * 37);
	memset(f->image + cluster_offset(FILE_CLUSTER + 2), 0xa5, 512);
	f->image[cluster_offset(FILE_CLUSTER + 2)] = 0x7b;
}

static bool
cancelled(void *argument)
{
	return (((exfat_fixture_t *)argument)->cancelled);
}

static fsimg_status_t
image_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error)
{
	exfat_fixture_t *f = argument;

	f->calls++;
	CHECK(fsimg_range_fits(offset, length, f->length));
	if (f->fail_payload && offset >= cluster_offset(FILE_CLUSTER)) {
		length = length > 7 ? 7 : length;
		bcopy(f->image + offset, buffer, length);
		*transferred = length;
		return (fsimg_error_set(error, FSIMG_EIO, EIO,
		    FSIMG_OP_IO, "injected partial exFAT payload read"));
	}
	bcopy(f->image + offset, buffer, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
image_size(void *argument, uint64_t *size, fsimg_error_t *error FSIMG_UNUSED)
{
	*size = ((exfat_fixture_t *)argument)->length;
	return (FSIMG_OK);
}

static fsimg_status_t
image_write(void *argument, uint64_t offset, const void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	exfat_fixture_t *f = argument;

	CHECK(fsimg_range_fits(offset, length, f->length));
	bcopy(buffer, f->image + offset, length);
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
image_zero(void *argument, uint64_t offset, uint64_t length,
    fsimg_error_t *error FSIMG_UNUSED)
{
	exfat_fixture_t *f = argument;

	CHECK(fsimg_range_fits(offset, length, f->length));
	bzero(f->image + offset, (size_t)length);
	return (FSIMG_OK);
}

static void
fixture_new(exfat_fixture_t *f)
{
	fsimg_options_t options;
	fsimg_io_ops_t ops;

	bzero(f, sizeof(*f));
	f->length = IMAGE_BYTES;
	f->image = malloc(IMAGE_BYTES);
	CHECK(f->image != NULL);
	strcpy(f->directory, "/tmp/fsimg-exfat-XXXXXX");
	CHECK(mkdtemp(f->directory) != NULL);
	f->scratch = open(f->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(f->scratch != -1);
	fsimg_options_init(&options);
	options.memory_bytes = 16 * 1024 * 1024;
	options.scratch_directory_fd = f->scratch;
	options.cancelled = cancelled;
	options.cancel_argument = f;
	CHECK(fsimg_ctx_new(&options, &f->ctx, NULL) == FSIMG_OK);
	fsimg_io_ops_init(&ops);
	ops.read_at = image_read;
	ops.write_at = image_write;
	ops.get_size = image_size;
	ops.zero_range = image_zero;
	CHECK(fsimg_io_from_callbacks(f->ctx, &ops, f, &f->io, NULL) == FSIMG_OK);
	make_image(f);
}

static void
fixture_free(exfat_fixture_t *f)
{
	fsimg_stats_t stats;

	f->cancelled = false;
	fsimg_reader_free(f->reader);
	fsimg_io_free(f->io);
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.scratch_bytes == 0 && stats.descriptors == 1);
	fsimg_ctx_free(f->ctx);
	CHECK(close(f->scratch) == 0);
	CHECK(rmdir(f->directory) == 0);
	free(f->image);
}

/* Open the reader and return its first entry, which must be Data.bin. */
static const fsimg_entry_t *
open_file(exfat_fixture_t *f)
{
	const fsimg_entry_t *entry;
	fsimg_error_t error;
	fsimg_status_t status;

	status = fsimg_reader_open(f->ctx, f->io, NULL, &f->reader, &error);
	if (status != FSIMG_OK)
		fprintf(stderr, "exFAT open: %d %s\n", status, error.message);
	CHECK(status == FSIMG_OK);
	status = fsimg_reader_next(f->reader, &entry, &error);
	if (status != FSIMG_OK)
		fprintf(stderr, "exFAT next: %d %s\n", status, error.message);
	CHECK(status == FSIMG_OK);
	CHECK(strcmp(entry->path, "/Data.bin") == 0);
	return (entry);
}

/* A directory with no allocation is valid and empty. It must not be
 * mistaken for the root, whose index is the one preparation already holds.
 */
static void
test_empty_directory(void)
{
	exfat_fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_error_t error;
	unsigned char *set;
	fsimg_status_t status;

	fixture_new(&f);
	set = file_set(&f);
	put16(set + 4, 0x10);
	set[33] = 1;
	put64(set + 40, 0);
	put32(set + 52, 0);
	put64(set + 56, 0);
	fix_set(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(entry->kind == FSIMG_ENTRY_DIRECTORY && strcmp(entry->path, "/Data.bin") == 0);
	status = fsimg_reader_next(f.reader, &entry, &error);
	if (status != FSIMG_DONE)
		fprintf(stderr, "empty directory: %d %s\n", status, error.message);
	CHECK(status == FSIMG_DONE);
	fixture_free(&f);
}

static void
test_independent(void)
{
	exfat_fixture_t f;
	fsimg_read_options_t options;
	fsimg_volume_info_t info;
	fsimg_verify_report_t report;
	fsimg_range_t range;
	const fsimg_entry_t *entry;
	unsigned char bytes[1024];
	size_t count;
	uint64_t calls;
	bool matches;

	fixture_new(&f);
	open_file(&f);
	CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
	CHECK(info.time_basis == FSIMG_TIME_PER_TIMESTAMP);
	CHECK(strcmp(info.label, "TEST") == 0 && info.allocation_units == HEAP_CLUSTERS);
	CHECK(fsimg_reader_match_name(f.reader, "data.BIN", &matches, NULL) == FSIMG_OK && matches);
	CHECK(fsimg_reader_read_at(f.reader, 0, bytes, sizeof(bytes), &count, NULL) == FSIMG_OK);
	CHECK(count == sizeof(bytes));
	for (unsigned i = 0; i < 512; i++)
		CHECK(bytes[i] == (unsigned char)(i * 37));
	CHECK(bytes[512] == 0x7b);
	for (unsigned i = 513; i < 1024; i++)
		CHECK(bytes[i] == 0);
	CHECK(fsimg_reader_next_range(f.reader, 0, &range, NULL) == FSIMG_OK);
	CHECK(range.kind == FSIMG_RANGE_DATA && range.length == 513);
	calls = f.calls;
	CHECK(fsimg_reader_next_range(f.reader, 513, &range, NULL) == FSIMG_OK);
	CHECK(range.kind == FSIMG_RANGE_ZERO && range.length == 511 && f.calls == calls);
	CHECK(fsimg_reader_read_at(f.reader, 900, bytes, 10, &count, NULL) == FSIMG_OK);
	CHECK(count == 10 && f.calls == calls);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_DONE);
	fsimg_reader_free(f.reader);
	f.reader = NULL;
	/* A forced decoder runs only its own probe and never falls back. */
	fsimg_read_options_init(&options);
	options.format = "fat32";
	CHECK(fsimg_reader_open(f.ctx, f.io, &options, &f.reader, NULL) == FSIMG_EFORMAT);
	CHECK(f.reader == NULL);
	options.format = "exfat";
	CHECK(fsimg_reader_open(f.ctx, f.io, &options, &f.reader, NULL) == FSIMG_OK);
	fsimg_reader_free(f.reader);
	f.reader = NULL;
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD, &report, NULL) == FSIMG_OK);
	CHECK(report.complete && report.files == 1 && report.directories == 1);
	CHECK(report.payload_bytes_read == 513 && report.logical_bytes == 1024);
	CHECK(report.allocated_clusters == 260 && report.free_clusters == HEAP_CLUSTERS - 260);
	fixture_free(&f);
}

static void
test_content_guard(const char *mode)
{
	exfat_fixture_t f;
	unsigned char bytes[1024], byte;
	fsimg_range_t range;
	fsimg_status_t status;
	size_t count;
	int fd, read_fd;

	fixture_new(&f);
	if (strcmp(mode, "changed") == 0 || strcmp(mode, "changed-range") == 0) {
		fd = openat(f.scratch, "image", O_CREAT | O_EXCL | O_RDWR, 0600);
		CHECK(fd != -1 && write(fd, f.image, IMAGE_BYTES) == IMAGE_BYTES);
		read_fd = openat(f.scratch, "image", O_RDONLY | O_CLOEXEC);
		CHECK(read_fd != -1);
		fsimg_io_free(f.io);
		CHECK(fsimg_io_from_fd(f.ctx, read_fd, FSIMG_IO_READ, &f.io, NULL) == FSIMG_OK);
		CHECK(close(read_fd) == 0);
		open_file(&f);
		/* A size change makes the identity mismatch independent of clock ticks. */
		CHECK(ftruncate(fd, IMAGE_BYTES + 1) == 0);
		if (strcmp(mode, "changed-range") == 0)
			status = fsimg_reader_next_range(f.reader, 900, &range, NULL);
		else
			status = fsimg_reader_read_at(f.reader, 900, &byte, 1, &count, NULL);
		CHECK(status == FSIMG_ESOURCE_CHANGED);
		CHECK(close(fd) == 0 && unlinkat(f.scratch, "image", 0) == 0);
	} else {
		open_file(&f);
		if (strcmp(mode, "partial") == 0) {
			f.fail_payload = true;
			status = fsimg_reader_read_at(f.reader, 0, bytes, sizeof(bytes), &count, NULL);
			CHECK(status == FSIMG_EIO && count == 7);
		} else if (strcmp(mode, "work") == 0) {
			f.ctx->options.max_work = f.reader->session.progress.work;
			status = fsimg_reader_read_at(f.reader, 900, &byte, 1, &count, NULL);
			CHECK(status == FSIMG_ELIMIT && count == 0);
		} else {
			f.cancelled = true;
			if (strcmp(mode, "cancel-range") == 0)
				status = fsimg_reader_next_range(f.reader, 900, &range, NULL);
			else
				status = fsimg_reader_read_at(f.reader, 900, &byte, 1, &count, NULL);
			CHECK(status == FSIMG_ECANCELLED);
		}
	}
	fixture_free(&f);
}

static void
test_metadata_boundary(const char *mode)
{
	exfat_fixture_t f;
	unsigned char metadata[64], *root;
	const fsimg_entry_t *entry;
	fsimg_status_t status;

	fixture_new(&f);
	root = f.image + cluster_offset(ROOT_CLUSTER);
	if (strcmp(mode, "after-end") == 0) {
		bcopy(root, metadata, 64);
		bzero(root, 512);
		bcopy(metadata, root + 32, 64);
		status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL);
	} else if (strcmp(mode, "inside-set") == 0) {
		bcopy(root, metadata, 64);
		bzero(root, 512);
		root[0] = 0x85;
		root[1] = 2;
		bcopy(metadata, root + 32, 64);
		status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL);
	} else if (strcmp(mode, "revision-zero") == 0 ||
	    strcmp(mode, "revision-large") == 0) {
		put16(f.image + 104, strcmp(mode, "revision-zero") == 0 ? 0 : 0x6400);
		fix_boot(&f);
		status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL);
	} else {
		file_set(&f)[22] = 1;
		fix_set(&f);
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
		status = fsimg_reader_next(f.reader, &entry, NULL);
	}
	CHECK(status == FSIMG_ECORRUPT);
	fixture_free(&f);
}

static void
test_benign_entries(bool critical)
{
	exfat_fixture_t f;
	unsigned char *set, *bitmap;
	uint16_t sum;
	const fsimg_entry_t *entry;
	fsimg_verify_report_t report;
	fsimg_status_t status;

	fixture_new(&f);
	set = file_set(&f) + 96;
	set[0] = 0xa4;
	set[1] = 1;
	put16(set + 4, 1);
	put32(set + 20, FILE_CLUSTER + 1);
	put64(set + 24, 512);
	set[32] = critical ? 0xc2 : 0xe0;
	sum = 0;
	for (unsigned i = 0; i < 64; i++) {
		if (i != 2 && i != 3)
			sum = (uint16_t)(sum / 2 + (sum % 2) * 32768 + set[i]);
	}
	put16(set + 2, sum);
	put32(f.image + 24 * 512 + (FILE_CLUSTER + 1) * 4, UINT32_MAX);
	bitmap = f.image + cluster_offset(2);
	bitmap[(FILE_CLUSTER - 1) / 8] |=
	    (unsigned char)(1u << ((FILE_CLUSTER - 1) % 8));
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	status = fsimg_reader_next(f.reader, &entry, NULL);
	if (critical) {
		CHECK(status == FSIMG_EUNSUPPORTED && entry == NULL);
	} else {
		CHECK(status == FSIMG_OK);
		fsimg_reader_free(f.reader);
		f.reader = NULL;
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
		CHECK(fsimg_verify(f.reader, 0, &report, NULL) == FSIMG_OK);
		CHECK(report.complete && report.allocated_clusters == 261);
	}
	fixture_free(&f);
}

static void
test_label_validation(void)
{
	static const uint16_t invalid[] = {
		0, 1, 31, '"', '*', '/', ':', '<', '>', '?', '\\', '|'
	};
	exfat_fixture_t f;
	fsimg_write_options_t options;
	fsimg_writer_t *writer;
	char label[2];
	fsimg_status_t status;

	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		fixture_new(&f);
		put16(f.image + cluster_offset(ROOT_CLUSTER) + 66, invalid[i]);
		status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL);
		if (status != FSIMG_ECORRUPT)
			fprintf(stderr, "invalid label unit=%u status=%d\n", invalid[i], status);
		CHECK(status == FSIMG_ECORRUPT);
		if (invalid[i] != 0) {
			label[0] = (char)invalid[i];
			label[1] = '\0';
			fsimg_write_options_init(&options);
			options.label = label;
			CHECK(fsimg_writer_new(f.ctx, "exfat", &options, &writer, NULL) == FSIMG_EINVAL);
			CHECK(writer == NULL);
		}
		fixture_free(&f);
	}
}

static void
test_upcase_work_limit(void)
{
	exfat_fixture_t f;
	fsimg_error_t error;
	fsimg_status_t status;

	for (unsigned compact = 0; compact < 2; compact++) {
		fixture_new(&f);
		if (compact != 0)
			compressed_upcase(&f, 0);
		/* Raw words exceed this budget directly. The compact table's words
		 * fit, but expanding its 65,408 identity mappings must not fit.
		 */
		f.ctx->options.max_work = 5000;
		status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, &error);
		CHECK(status == FSIMG_ELIMIT && error.resource == FSIMG_RESOURCE_WORK);
		fixture_free(&f);
	}
}

static void
test_corruptions(void)
{
	exfat_fixture_t f;
	fsimg_verify_report_t report;
	const fsimg_entry_t *entry;
	fsimg_status_t status;
	unsigned char *set;

	for (unsigned scenario = 0; scenario < 14; scenario++) {
		fixture_new(&f);
		set = file_set(&f);
		switch (scenario) {
		case 0: f.image[120] ^= 1; break;
		case 1: f.image[12 * 512 + 120] ^= 1; break;
		case 2: set[66] ^= 1; break;
		case 3: set[36] ^= 1; fix_set(&f); break;
		case 4: put64(set + 40, 1025); fix_set(&f); break;
		case 5: put32(set + 52, HEAP_CLUSTERS + 2); fix_set(&f); break;
		case 6: set[33] = 3; put64(set + 56, UINT64_MAX); fix_set(&f); break;
		case 7: put32(f.image + 24 * 512 + FILE_CLUSTER * 4, FILE_CLUSTER); break;
		case 8: f.image[cluster_offset(2) + (FILE_CLUSTER - 2) / 8] &=
		    (unsigned char)~(1u << ((FILE_CLUSTER - 2) % 8)); break;
		case 9: f.image[cluster_offset(3) + 'a' * 2] ^= 1; break;
		case 10: f.image[110] = 2; fix_boot(&f); break;
		case 11: set[64] = 0xc2; fix_set(&f); break;
		case 12: set[96] = 0x85; break;
		case 13: f.image[cluster_offset(2) + 70] |= 1; break;
		}
		status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL);
		if (status == FSIMG_OK)
			status = fsimg_reader_next(f.reader, &entry, NULL);
		if (status == FSIMG_OK && scenario == 13) {
			fsimg_reader_free(f.reader);
			f.reader = NULL;
			CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
			status = fsimg_verify(f.reader, 0, &report, NULL);
		}
		if (status != (scenario == 10 ? FSIMG_EUNSUPPORTED : FSIMG_ECORRUPT))
			fprintf(stderr, "corruption scenario=%u status=%d\n", scenario, status);
		CHECK(status == (scenario == 10 ? FSIMG_EUNSUPPORTED : FSIMG_ECORRUPT));
		fixture_free(&f);
	}
}

static void
test_allocation_profiles(void)
{
	exfat_fixture_t f;
	fsimg_verify_report_t report;
	const fsimg_entry_t *entry;
	unsigned char *set, *fat, *bitmap, *root;
	uint64_t expected_allocated;
	bool matches;

	for (unsigned scenario = 0; scenario < 7; scenario++) {
		fixture_new(&f);
		set = file_set(&f);
		fat = f.image + 24 * 512;
		bitmap = f.image + cluster_offset(2);
		root = f.image + cluster_offset(ROOT_CLUSTER);
		expected_allocated = 260;
		switch (scenario) {
		case 0:
			/* Extra chained allocation remains owned beyond a short file. */
			put64(set + 40, 1);
			put64(set + 56, 1);
			fix_set(&f);
			break;
		case 1:
			put64(set + 40, 0);
			put64(set + 56, 0);
			fix_set(&f);
			break;
		case 2:
			/* NoFatChain entries must ignore even bad/reserved FAT values. */
			set[33] = 3;
			put32(fat + FILE_CLUSTER * 4, UINT32_C(0xfffffff7));
			put32(fat + (FILE_CLUSTER + 1) * 4, 1);
			bitmap[(FILE_CLUSTER - 1) / 8] |=
			    (unsigned char)(1u << ((FILE_CLUSTER - 1) % 8));
			bitmap[FILE_CLUSTER / 8] &=
			    (unsigned char)~(1u << (FILE_CLUSTER % 8));
			fix_set(&f);
			break;
		case 3:
			put64(root + 24, 512);
			break;
		case 4:
			/* A fragmented bitmap includes trailing reserved bytes. */
			put64(root + 24, 1024);
			put32(fat + 2 * 4, FILE_CLUSTER + 1);
			put32(fat + (FILE_CLUSTER + 1) * 4, UINT32_MAX);
			bitmap[(FILE_CLUSTER - 1) / 8] |=
			    (unsigned char)(1u << ((FILE_CLUSTER - 1) % 8));
			expected_allocated++;
			break;
		case 5:
			/* Replace one upcase cluster without changing logical table bytes. */
			bcopy(f.image + cluster_offset(100),
			    f.image + cluster_offset(FILE_CLUSTER + 1), 512);
			put32(fat + 99 * 4, FILE_CLUSTER + 1);
			put32(fat + (FILE_CLUSTER + 1) * 4, 101);
			bitmap[(100 - 2) / 8] &= (unsigned char)~(1u << ((100 - 2) % 8));
			bitmap[(FILE_CLUSTER - 1) / 8] |=
			    (unsigned char)(1u << ((FILE_CLUSTER - 1) % 8));
			break;
		case 6:
			/* A legal custom table, rather than host Unicode, controls lookup. */
			put16(f.image + cluster_offset(3) + 0xe9 * 2, 'D');
			put32(root + 36, rotate_sum(f.image + cluster_offset(3), 131072, 32));
			put16(set + 66, 0xe9);
			fix_set(&f);
			break;
		}
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
		CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
		if (scenario == 6) {
			CHECK(strcmp(entry->path, "/\xc3\xa9" "ata.bin") == 0);
			CHECK(fsimg_reader_match_name(f.reader, "DATA.BIN", &matches, NULL) == FSIMG_OK);
			CHECK(matches);
		}
		fsimg_reader_free(f.reader);
		f.reader = NULL;
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
		CHECK(fsimg_verify(f.reader, 0, &report, NULL) == FSIMG_OK && report.complete);
		CHECK(report.allocated_clusters == expected_allocated);
		fixture_free(&f);
	}
}

static void
compressed_upcase(exfat_fixture_t *f, unsigned scenario)
{
	unsigned char *table, *root, *bitmap, *fat;
	size_t offset;

	table = f->image + cluster_offset(3);
	root = f->image + cluster_offset(ROOT_CLUSTER);
	bitmap = f->image + cluster_offset(2);
	fat = f->image + 24 * 512;
	offset = 0;
	if (scenario == 1) {
		put16(table, 0xffff);
		put16(table + 2, 0);
		offset = 4;
	}
	for (unsigned i = 0; i < 128; i++)
		put16(table + offset + i * 2,
		    (uint16_t)(i >= 'a' && i <= 'z' ? i - 32 : i));
	offset += 256;
	put16(table + offset, 0xffff);
	put16(table + offset + 2, scenario == 2 ? 65535 : 65408);
	offset += 4;
	if (scenario == 3)
		offset -= 2;
	if (scenario == 4)
		put16(table + 'a' * 2, 'a');
	put32(root + 36, rotate_sum(table, offset, 32));
	put64(root + 56, offset);
	put32(fat + 3 * 4, UINT32_MAX);
	for (unsigned cluster = 4; cluster < ROOT_CLUSTER; cluster++)
		bitmap[(cluster - 2) / 8] &=
		    (unsigned char)~(1u << ((cluster - 2) % 8));
}

static void
test_upcase_profiles(void)
{
	exfat_fixture_t f;
	fsimg_verify_report_t report;
	fsimg_status_t status;

	for (unsigned scenario = 0; scenario < 5; scenario++) {
		fixture_new(&f);
		compressed_upcase(&f, scenario);
		status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL);
		if (scenario < 2) {
			CHECK(status == FSIMG_OK);
			CHECK(fsimg_verify(f.reader, 0, &report, NULL) == FSIMG_OK);
			CHECK(report.allocated_clusters == 5);
		} else {
			CHECK(status == FSIMG_ECORRUPT);
		}
		fixture_free(&f);
	}
}

static void
test_allocation_failures(void)
{
	exfat_fixture_t f;
	fsimg_verify_report_t report;
	fsimg_status_t status;
	unsigned failures = 0, successes = 0;

	/* Injection spans opening, directory streams, extent indexes and verification
	 * sorting. Every failure must release format state before the next session.
	 */
	for (unsigned attempt = 1; attempt < 100; attempt++) {
		fixture_new(&f);
		fsimg_test_fail_alloc_after(f.ctx, attempt);
		status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL);
		if (status == FSIMG_OK)
			status = fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD, &report, NULL);
		CHECK(status == FSIMG_OK || status == FSIMG_ENOMEM);
		if (status == FSIMG_ENOMEM)
			failures++;
		else
			successes++;
		fsimg_test_fail_alloc_after(f.ctx, 0);
		fixture_free(&f);
	}
	CHECK(failures > 10 && successes > 0);
}

static void
test_writer(void)
{
	exfat_fixture_t f;
	fsimg_writer_t *writer;
	fsimg_write_options_t options;
	fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };
	fsimg_entry_t source = { .path = "/empty", .kind = FSIMG_ENTRY_FILE };
	const fsimg_entry_t *entry;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_verify_report_t report;
	fsimg_error_t error;
	fsimg_status_t status;
	unsigned entries;

	fixture_new(&f);
	fsimg_write_options_init(&options);
	options.volume_bytes = IMAGE_BYTES;
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	options.label = "R\xc3\xa9sum\xc3\xa9";
	CHECK(fsimg_writer_new(f.ctx, "exfat", &options, &writer, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_add(writer, &source, &zero, NULL) == FSIMG_OK);
	source = (fsimg_entry_t){ .path = "/Directory", .kind = FSIMG_ENTRY_DIRECTORY };
	CHECK(fsimg_writer_add(writer, &source, NULL, NULL) == FSIMG_OK);
	source = (fsimg_entry_t){ .path = "/Directory/Zero \xf0\x9f\x8c\x8d.bin",
	    .kind = FSIMG_ENTRY_FILE, .size = 100003 };
	CHECK(fsimg_writer_add(writer, &source, &zero, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_plan(writer, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	CHECK(info.payload_bytes == 100003 && info.initialized_bytes == 0 && info.entries == 3);
	status = fsimg_writer_emit(writer, f.io, &error);
	if (status != FSIMG_OK)
		fprintf(stderr, "writer emit: %d %s\n", status, error.message);
	CHECK(status == FSIMG_OK);
	{
		const exfat_geometry_t *geometry = &((const exfat_plan_t *)plan)->geometry;
		const unsigned char *root = f.image + geometry->heap_offset +
		    (uint64_t)(geometry->root_cluster - 2) * geometry->cluster_bytes;

		/* Compact identity runs keep the complete mapping interoperable with
		 * independent checkers that accept a 16-bit encoded byte count.
		 */
		CHECK(fsimg_meta_get_u64(root + 56) < 65536);
	}
	fsimg_writer_free(writer);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	entries = 0;
	while ((status = fsimg_reader_next(f.reader, &entry, &error)) == FSIMG_OK)
		entries++;
	if (status != FSIMG_DONE)
		fprintf(stderr, "writer reread: %d %s\n", status, error.message);
	CHECK(status == FSIMG_DONE && entries == 3);
	fsimg_reader_free(f.reader);
	f.reader = NULL;
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD, &report, &error) == FSIMG_OK);
	CHECK(report.complete && report.files == 2 && report.directories == 2);
	fixture_free(&f);
}

static void
test_writer_names(void)
{
	exfat_fixture_t f;
	fsimg_writer_t *writer;
	fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };
	fsimg_entry_t entry = { .path = "/Case", .kind = FSIMG_ENTRY_FILE };
	const fsimg_plan_t *plan;

	fixture_new(&f);
	CHECK(fsimg_writer_new(f.ctx, "exfat", NULL, &writer, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_add(writer, &entry, &zero, NULL) == FSIMG_OK);
	entry.path = "/cASE";
	CHECK(fsimg_writer_add(writer, &entry, &zero, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_plan(writer, &plan, NULL) == FSIMG_EINVAL);
	fsimg_writer_free(writer);
	fixture_free(&f);
}

static void
test_timestamp_fidelity(void)
{
	exfat_fixture_t f;
	fsimg_writer_t *writer;
	fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };
	fsimg_entry_t source;
	const fsimg_entry_t *entry;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;

	fixture_new(&f);
	source = (fsimg_entry_t){ .path = "/times", .kind = FSIMG_ENTRY_FILE };
	source.metadata.valid = FSIMG_META_CREATED | FSIMG_META_MODIFIED | FSIMG_META_ACCESSED;
	source.metadata.required = source.metadata.valid;
	source.metadata.created = (fsimg_timestamp_t){ .seconds = 315529200,
	    .basis = FSIMG_TIME_OFFSET, .offset_minutes = 60, .precision_ns = 10000000 };
	source.metadata.modified = (fsimg_timestamp_t){ .seconds = 315532801,
	    .basis = FSIMG_TIME_WALL, .nanoseconds = 10000000, .precision_ns = 10000000 };
	source.metadata.accessed = (fsimg_timestamp_t){ .seconds = 315532802,
	    .basis = FSIMG_TIME_UTC, .precision_ns = 2000000000 };
	CHECK(fsimg_writer_new(f.ctx, "exfat", NULL, &writer, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_add(writer, &source, &zero, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_emit(writer, f.io, NULL) == FSIMG_OK);
	fsimg_writer_free(writer);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(entry->metadata.created.seconds == source.metadata.created.seconds);
	CHECK(entry->metadata.created.basis == FSIMG_TIME_OFFSET);
	CHECK(entry->metadata.created.offset_minutes == 60);
	CHECK(entry->metadata.modified.seconds == source.metadata.modified.seconds);
	CHECK(entry->metadata.modified.nanoseconds == 10000000);
	CHECK(entry->metadata.modified.basis == FSIMG_TIME_WALL);
	CHECK(entry->metadata.accessed.seconds == source.metadata.accessed.seconds);
	CHECK(entry->metadata.accessed.basis == FSIMG_TIME_UTC);
	fsimg_reader_free(f.reader);
	f.reader = NULL;

	/* A required sub-quarter-hour offset cannot be silently approximated. */
	source.metadata.created.offset_minutes = 1;
	CHECK(fsimg_writer_new(f.ctx, "exfat", NULL, &writer, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_add(writer, &source, &zero, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_plan(writer, &plan, NULL) == FSIMG_EUNSUPPORTED);
	fsimg_writer_free(writer);

	/* Optional loss is reported once by planning, not again during emission. */
	source.metadata.required &= ~FSIMG_META_CREATED;
	CHECK(fsimg_writer_new(f.ctx, "exfat", NULL, &writer, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_add(writer, &source, &zero, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_plan(writer, &plan, NULL) == FSIMG_OK);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	CHECK(info.metadata_losses == 1);
	CHECK(fsimg_writer_emit(writer, f.io, NULL) == FSIMG_OK);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	CHECK(info.metadata_losses == 1);
	fsimg_writer_free(writer);
	fixture_free(&f);
}

static void
test_optional_timestamp_default(void)
{
	exfat_fixture_t f;
	fsimg_writer_t *writer;
	fsimg_entry_t source = { .path = "/default-time", .kind = FSIMG_ENTRY_FILE };
	fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };
	const fsimg_entry_t *entry;

	fixture_new(&f);
	source.metadata.valid = FSIMG_META_CREATED;
	source.metadata.created = (fsimg_timestamp_t){ .seconds = INT64_MAX,
	    .nanoseconds = 990000000, .precision_ns = 10000000, .basis = FSIMG_TIME_UTC };
	CHECK(fsimg_writer_new(f.ctx, "exfat", NULL, &writer, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_add(writer, &source, &zero, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_emit(writer, f.io, NULL) == FSIMG_OK);
	fsimg_writer_free(writer);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(entry->metadata.created.seconds == 315532800);
	CHECK(entry->metadata.created.nanoseconds == 0);
	CHECK(entry->metadata.created.basis == FSIMG_TIME_UTC);
	fixture_free(&f);
}

static void
test_reproducible(void)
{
	static const fsimg_entry_t entries[] = {
		{ .path = "/a", .kind = FSIMG_ENTRY_FILE, .size = 3 },
		{ .path = "/b", .kind = FSIMG_ENTRY_DIRECTORY },
		{ .path = "/b/Caf\xc3\xa9", .kind = FSIMG_ENTRY_FILE, .size = 5 }
	};
	exfat_fixture_t f;
	fsimg_writer_t *writer;
	fsimg_write_options_t options;
	fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };
	unsigned char *baseline;
	unsigned index;

	fixture_new(&f);
	baseline = malloc(IMAGE_BYTES);
	CHECK(baseline != NULL);
	fsimg_write_options_init(&options);
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	options.volume_bytes = IMAGE_BYTES;
	options.volume_serial = 42;
	for (unsigned reverse = 0; reverse < 2; reverse++) {
		CHECK(fsimg_test_meta_cache(f.ctx, reverse == 0 ? 1 : 32, NULL) == FSIMG_OK);
		memset(f.image, reverse == 0 ? 0xa5 : 0x6b, IMAGE_BYTES);
		CHECK(fsimg_writer_new(f.ctx, "exfat", &options, &writer, NULL) == FSIMG_OK);
		for (unsigned i = 0; i < 3; i++) {
			index = reverse == 0 ? i : 2 - i;
			CHECK(fsimg_writer_add(writer, &entries[index],
			    entries[index].kind == FSIMG_ENTRY_FILE ? &zero : NULL, NULL) == FSIMG_OK);
		}
		CHECK(fsimg_writer_emit(writer, f.io, NULL) == FSIMG_OK);
		fsimg_writer_free(writer);
		if (reverse == 0)
			bcopy(f.image, baseline, IMAGE_BYTES);
		else
			CHECK(bcmp(f.image, baseline, IMAGE_BYTES) == 0);
	}
	free(baseline);
	fixture_free(&f);
}

static void
test_stream_limits(void)
{
	exfat_geometry_t geometry = { .cluster_bytes = 512, .clusters = 1000000 };
	exfat_stream_t stream = { .first = 2, .length = UINT64_C(256) << 20,
	    .valid_length = UINT64_C(256) << 20 };

	/* The directory field limit is narrower than a regular file's length
	 * range, independently of the larger heap used for this boundary case.
	 */
	CHECK(exfat_stream_validate(&geometry, &stream, true, NULL) == FSIMG_OK);
	stream.length += 512;
	stream.valid_length = stream.length;
	CHECK(exfat_stream_validate(&geometry, &stream, true, NULL) == FSIMG_ECORRUPT);
	CHECK(exfat_stream_validate(&geometry, &stream, false, NULL) == FSIMG_OK);
	stream.length -= 512;
	stream.valid_length = stream.length - 1;
	CHECK(exfat_stream_validate(&geometry, &stream, true, NULL) == FSIMG_ECORRUPT);
	CHECK(exfat_stream_validate(&geometry, &stream, false, NULL) == FSIMG_OK);
	stream = (exfat_stream_t){ .first = 2, .contiguous = true };
	CHECK(exfat_stream_validate(&geometry, &stream, false, NULL) == FSIMG_ECORRUPT);
	stream = (exfat_stream_t){ 0 };
	CHECK(exfat_stream_validate(&geometry, &stream, true, NULL) == FSIMG_OK);
}

static uint32_t
fragment_cluster(unsigned ordinal, unsigned count)
{
	if (ordinal < count / 2)
		return (FILE_CLUSTER + ordinal * 2);
	return (FILE_CLUSTER + (ordinal - count / 2) * 2 + 1);
}

/* Make Data.bin a fully initialized file of the given number of 512-byte
 * clusters. Visit even clusters, then odd clusters, within one allocated
 * interval. This creates one physical extent per logical cluster without
 * holes or extra payload storage. Data follows logical position, not image
 * order: each cluster starts with its logical ordinal.
 */
static void
make_fragmented_file(exfat_fixture_t *f, unsigned clusters)
{
	unsigned char *fat, *bitmap, *set;
	uint32_t cluster, next;
	const uint64_t length = (uint64_t)clusters * 512;

	fat = f->image + 24 * 512;
	bitmap = f->image + cluster_offset(2);
	set = file_set(f);
	/* Valid data length and data length are equal: no zero tail. The set
	 * checksum covers both fields, so it is recomputed.
	 */
	put64(set + 40, length);
	put64(set + 56, length);
	fix_set(f);
	/* For each logical cluster: link the FAT entry of its physical cluster
	 * to the next logical one (end of chain for the last), mark it in the
	 * allocation bitmap, and fill it with the position pattern.
	 */
	for (unsigned i = 0; i < clusters; i++) {
		cluster = fragment_cluster(i, clusters);
		next = i + 1 == clusters ? UINT32_MAX : fragment_cluster(i + 1, clusters);
		put32(fat + cluster * 4, next);
		bitmap[(cluster - 2) / 8] |= (unsigned char)(1u << ((cluster - 2) % 8));
		for (unsigned j = 0; j < 512; j++)
			f->image[cluster_offset(cluster) + j] = (unsigned char)((i * 512 + j) * 37);
		put32(f->image + cluster_offset(cluster), i);
	}
}

static void
test_sequential_fragments(void)
{
	static const unsigned seeks[] = { 511, 0, 257, 7, 300 };
	exfat_fixture_t f;
	unsigned char *bytes;
	unsigned char marker[4], marker8[8], saved8[8];
	fsimg_store_t *records;
	uint64_t before, work;
	size_t count;
	const unsigned clusters = 512;
	const size_t length = 512u * 512;

	fixture_new(&f);
	CHECK(fsimg_test_meta_cache(f.ctx, 1, NULL) == FSIMG_OK);
	make_fragmented_file(&f, clusters);
	open_file(&f);
	bytes = malloc(length);
	CHECK(bytes != NULL);
	before = f.reader->session.progress.work;
	CHECK(fsimg_reader_read_at(f.reader, 0, bytes, length, &count, NULL) == FSIMG_OK);
	CHECK(count == length);
	work = f.reader->session.progress.work - before;
	if (work > (uint64_t)clusters * 4 + 64)
		fprintf(stderr, "sequential fragmented read work=%" PRIu64 " clusters=%u\n", work, clusters);
	CHECK(work <= (uint64_t)clusters * 4 + 64);
	for (unsigned i = 0; i < clusters; i++) {
		for (unsigned j = 0; j < 4; j++)
			CHECK(bytes[i * 512 + j] == (unsigned char)(i >> (j * 8)));
		for (unsigned j = 4; j < 512; j++)
			CHECK(bytes[i * 512 + j] == (unsigned char)(j * 37));
	}
	for (size_t i = 0; i < sizeof(seeks) / sizeof(seeks[0]); i++) {
		CHECK(fsimg_reader_read_at(f.reader, (uint64_t)seeks[i] * 512,
		    marker, sizeof(marker), &count, NULL) == FSIMG_OK);
		CHECK(count == sizeof(marker));
		for (unsigned j = 0; j < 4; j++)
			CHECK(marker[j] == (unsigned char)(seeks[i] >> (j * 8)));
	}
	/* Records must tile the allocation in order. A record repeating an
	 * earlier logical offset passes per-record checks but not the index walk.
	 */
	records = fsimg_test_extent_records(exfat_state(f.reader)->file);
	CHECK(fsimg_store_read(records, 1 * 24,
	    marker8, sizeof(marker8), NULL) == FSIMG_OK);
	CHECK(fsimg_store_read(records, 2 * 24,
	    saved8, sizeof(saved8), NULL) == FSIMG_OK);
	CHECK(fsimg_store_write(records, 2 * 24,
	    marker8, sizeof(marker8), NULL) == FSIMG_OK);
	CHECK(exfat_index_check(f.reader, exfat_state(f.reader)->file, NULL) ==
	    FSIMG_ECORRUPT);
	CHECK(fsimg_store_write(records, 2 * 24,
	    saved8, sizeof(saved8), NULL) == FSIMG_OK);
	CHECK(exfat_index_check(f.reader, exfat_state(f.reader)->file, NULL) ==
	    FSIMG_OK);
	/* Scratch is not trusted. A record redirected to other in-volume metadata,
	 * here the FAT, must fail rather than disclose that object's bytes.
	 */
	put64(marker8, 24 * 512);
	CHECK(fsimg_store_write(records, 3 * 24 + 8,
	    marker8, sizeof(marker8), NULL) == FSIMG_OK);
	CHECK(fsimg_reader_read_at(f.reader, 3 * 512, marker, sizeof(marker), &count,
	    NULL) == FSIMG_ECORRUPT);
	CHECK(count == 0);
	free(bytes);
	fixture_free(&f);
}

static void
test_extract_zero_tail(void)
{
	exfat_fixture_t f;
	unsigned char bytes[1024];
	char directory[] = "/tmp/fsimg-exfat-extract-XXXXXX";
	int parent, output, fd;

	fixture_new(&f);
	CHECK(mkdtemp(directory) != NULL);
	parent = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(parent != -1);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_extract_at(f.reader, parent, "output", NULL, NULL) == FSIMG_OK);
	output = openat(parent, "output", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(output != -1);
	fd = openat(output, "Data.bin", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	CHECK(fd != -1 && read(fd, bytes, sizeof(bytes)) == sizeof(bytes));
	for (unsigned i = 0; i < 512; i++)
		CHECK(bytes[i] == (unsigned char)(i * 37));
	CHECK(bytes[512] == 0x7b);
	for (unsigned i = 513; i < sizeof(bytes); i++)
		CHECK(bytes[i] == 0);
	CHECK(close(fd) == 0 && unlinkat(output, "Data.bin", 0) == 0);
	CHECK(close(output) == 0 && unlinkat(parent, "output", AT_REMOVEDIR) == 0);
	CHECK(close(parent) == 0 && rmdir(directory) == 0);
	fixture_free(&f);
}

/* Comparison-baseline record (see baseline.h) for the default image, whose
 * Data.bin has 513 initialized bytes of 1024 and a poisoned tail cluster.
 * The content digest shows that the tail reads as zeros.
 */
static void
baseline_zero_tail(void)
{
	exfat_fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_stats_t stats;
	sha256_t digest;
	char hex[SHA256_HEX_BYTES];

	fixture_new(&f);
	entry = open_file(&f);
	sha256_init(&digest);
	baseline_digest_read(f.reader, 0, entry->size, &digest);
	sha256_final_hex(&digest, hex);
	/* Sampled while Data.bin is still current, before teardown, so
	 * memory_bytes is what the reader keeps for the file.
	 */
	CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
	printf("fixture=exfat-zero-tail content_sha256=%s read_bytes=%" PRIu64
	    " read_io_calls=%" PRIu64 " read_memory_retained=%" PRIu64
	    " read_descriptors_peak=%" PRIu64 "\n", hex, stats.read_bytes,
	    stats.io_calls, stats.memory_bytes, stats.descriptors_peak);
	fixture_free(&f);
}

/* Comparison-baseline record (see baseline.h): the 512-fragment file under
 * the common access script with a one-page metadata cache.
 */
static void
baseline_fragmented(void)
{
	exfat_fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_stats_t stats;
	sha256_t digest;
	char hex[SHA256_HEX_BYTES];

	fixture_new(&f);
	CHECK(fsimg_test_meta_cache(f.ctx, 1, NULL) == FSIMG_OK);
	make_fragmented_file(&f, 512);
	entry = open_file(&f);
	sha256_init(&digest);
	baseline_read_script(f.reader, entry->size, &digest);
	sha256_final_hex(&digest, hex);
	/* Sampled while Data.bin is still current, before teardown, so
	 * memory_bytes is what the reader keeps for the file.
	 */
	CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
	printf("fixture=exfat-fragmented content_sha256=%s read_io_calls=%" PRIu64
	    " read_work=%" PRIu64 " read_scratch_peak=%" PRIu64
	    " read_memory_retained=%" PRIu64 " read_descriptors_peak=%" PRIu64
	    "\n", hex, stats.io_calls, f.reader->session.progress.work,
	    stats.scratch_peak, stats.memory_bytes, stats.descriptors_peak);
	fixture_free(&f);
}

static void
read_external(const char *path)
{
	exfat_fixture_t f;
	fsimg_verify_report_t report;
	fsimg_error_t error;
	fsimg_status_t status;
	int fd;

	fixture_new(&f);
	fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	CHECK(fd != -1);
	fsimg_io_free(f.io);
	CHECK(fsimg_io_from_fd(f.ctx, fd, FSIMG_IO_READ, &f.io, NULL) == FSIMG_OK);
	CHECK(close(fd) == 0);
	status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, &error);
	if (status != FSIMG_OK)
		fprintf(stderr, "external exFAT open: %d %s\n", status, error.message);
	CHECK(status == FSIMG_OK);
	status = fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD, &report, &error);
	if (status != FSIMG_OK)
		fprintf(stderr, "external exFAT verify: %d %s\n", status, error.message);
	CHECK(status == FSIMG_OK && report.complete);
	printf("external exfat files=%" PRIu64 " directories=%" PRIu64
	    " initialized_read=%" PRIu64 "\n", report.files, report.directories,
	    report.payload_bytes_read);
	fixture_free(&f);
}

static void
emit_seed(const char *path)
{
	exfat_fixture_t f;
	int fd;

	fixture_new(&f);
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	CHECK(fd != -1);
	CHECK(write(fd, f.image, IMAGE_BYTES) == IMAGE_BYTES);
	CHECK(close(fd) == 0);
	fixture_free(&f);
}

static void
emit_external(const char *path)
{
	exfat_fixture_t f;
	fsimg_writer_t *writer;
	fsimg_write_options_t options;
	fsimg_entry_t entry = { .path = "/ZERO.BIN", .kind = FSIMG_ENTRY_FILE,
	    .size = 70003 };
	fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };
	int fd;

	fixture_new(&f);
	fsimg_write_options_init(&options);
	options.volume_bytes = IMAGE_BYTES;
	options.flags |= FSIMG_WRITE_REPRODUCIBLE;
	CHECK(fsimg_writer_new(f.ctx, "exfat", &options, &writer, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_add(writer, &entry, &zero, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_emit(writer, f.io, NULL) == FSIMG_OK);
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	CHECK(fd != -1);
	CHECK(write(fd, f.image, IMAGE_BYTES) == IMAGE_BYTES);
	CHECK(close(fd) == 0);
	fsimg_writer_free(writer);
	fixture_free(&f);
}

int
main(int argc, char **argv)
{
	/* Measurement records only on stdout; test/baseline.sh parses them. */
	if (argc == 2 && strcmp(argv[1], "--baseline") == 0) {
		baseline_tree("exfat-tree", "exfat");
		baseline_zero_tail();
		baseline_fragmented();
		return (0);
	}
	if (argc == 3 && strcmp(argv[1], "--guard") == 0) {
		test_content_guard(argv[2]);
		return (0);
	}
	if (argc == 3 && strcmp(argv[1], "--emit") == 0) {
		emit_external(argv[2]);
		return (0);
	}
	if (argc == 3 && strcmp(argv[1], "--read-external") == 0) {
		read_external(argv[2]);
		return (0);
	}
	if (argc == 3 && strcmp(argv[1], "--seed") == 0) {
		emit_seed(argv[2]);
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--compact-upcase") == 0) {
		test_writer();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--benign-critical") == 0) {
		test_benign_entries(true);
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--labels") == 0) {
		test_label_validation();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--upcase-work") == 0) {
		test_upcase_work_limit();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--optional-default") == 0) {
		test_optional_timestamp_default();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--sequential-fragments") == 0) {
		test_sequential_fragments();
		return (0);
	}
	if (argc == 3 && strcmp(argv[1], "--metadata-boundary") == 0) {
		test_metadata_boundary(argv[2]);
		return (0);
	}
	CHECK(argc == 1);
	test_empty_directory();
	test_independent();
	test_corruptions();
	test_allocation_profiles();
	test_upcase_profiles();
	test_allocation_failures();
	test_writer();
	test_writer_names();
	test_timestamp_fidelity();
	test_optional_timestamp_default();
	test_reproducible();
	test_stream_limits();
	test_sequential_fragments();
	test_extract_zero_tail();
	test_content_guard("changed");
	test_content_guard("changed-range");
	test_content_guard("cancel");
	test_content_guard("cancel-range");
	test_content_guard("work");
	test_content_guard("partial");
	test_metadata_boundary("after-end");
	test_metadata_boundary("inside-set");
	test_metadata_boundary("unknown-offset");
	test_metadata_boundary("revision-zero");
	test_metadata_boundary("revision-large");
	test_benign_entries(false);
	test_benign_entries(true);
	test_label_validation();
	test_upcase_work_limit();
	return (0);
}
