/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/resource.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "baseline.h"
#include "fat.h"
#include "test.h"

/* This fixture builder encodes disk fields directly, independently of the
 * production geometry/name/chain helpers. Real formatter fixtures are checked
 * separately by fat-integration.sh. The image is a fresh sparse ordinary file.
 */
typedef struct fixture {
	fsimg_ctx_t	*ctx;
	fsimg_io_t	*io;
	fsimg_reader_t	*reader;
	uint64_t	 bytes;
	uint64_t	 fat_offset;
	uint64_t	 table_bytes;
	uint64_t	 root_offset;
	uint64_t	 data_offset;
	uint64_t	 calls;
	uint64_t	 payload_calls;
	uint64_t	 fail_call;
	uint64_t	 short_bytes;
	uint64_t	 cancel_after_call;
	bool		 cancelled;
	uint32_t	 clusters;
	uint32_t	 sector_bytes;
	uint32_t	 cluster_bytes;
	uint32_t	 eoc;
	unsigned	 variant;
	int		 fd;
	int		 scratch;
	char		 directory[64];
} fixture_t;

static void put16(unsigned char *, uint16_t);
static void put32(unsigned char *, uint32_t);
static void write_bytes(fixture_t *, uint64_t, const void *, size_t);
static void set_cluster(fixture_t *, uint32_t, uint32_t);
static uint64_t cluster_offset(const fixture_t *, uint32_t);
static void short_entry(unsigned char *, const char *, bool, uint32_t, uint32_t);
static void fixture_init(fixture_t *, unsigned);
static void fixture_geometry(fixture_t *, unsigned, unsigned, unsigned);
static void test_geometry_matrix(void);
static void test_cluster_ceiling(unsigned);
static void fixture_open(fixture_t *);
static void fixture_close(fixture_t *);
static fsimg_status_t image_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static fsimg_status_t image_size(void *, uint64_t *, fsimg_error_t *);
static void test_variants(void);
static void test_names(void);
static void test_geometry(void);
static void test_thresholds(void);
static void test_deep_tree(void);
static void test_detection(void);
static void test_corruption(void);
static void test_limits(void);
static void test_failures(void);
static void test_partial_content(void);
static void test_host_change(void);
static void test_wide(unsigned);
static void test_file_limit(void);
static void test_empty(void);
static void test_edge_cases(void);
static void test_mirror_selector(void);
static void test_index_failures(void);
static void test_cancellation(void);
static void test_slices(void);
static void test_long_names(void);
static void test_volume_labels(void);
static void test_cached_name(void);
static void test_cached_index(void);
static void test_range_reads(void);
static void make_fragmented_file(fixture_t *, uint32_t);
static void baseline_fragmented(void);
static bool cancelled(void *);
static size_t long_records(unsigned char *, const uint16_t *, size_t,
    const unsigned char *);
static void test_report_outputs(void);
static void test_capability_bounds(void);
static void test_external(const char *, const char *);
static void make_payload(const char *, size_t);
static void write_seed(const char *, unsigned);

static void
put16(unsigned char *p, uint16_t value)
{
	p[0] = (unsigned char)value;
	p[1] = (unsigned char)(value >> 8);
}

static void
put32(unsigned char *p, uint32_t value)
{
	for (unsigned i = 0; i < 4; i++)
		p[i] = (unsigned char)(value >> (8 * i));
}

static void
write_bytes(fixture_t *f, uint64_t offset, const void *bytes, size_t length)
{
	CHECK(pwrite(f->fd, bytes, length, (off_t)offset) == (ssize_t)length);
}

static void
set_cluster(fixture_t *f, uint32_t cluster, uint32_t value)
{
	unsigned char bytes[4];
	uint64_t offset;
	uint16_t packed;
	size_t length;

	offset = f->variant == 12 ? (uint64_t)cluster * 3 / 2 :
	    (uint64_t)cluster * (f->variant / 8);
	length = f->variant == 32 ? 4 : 2;
	for (unsigned copy = 0; copy < 2; copy++) {
		uint64_t address = f->fat_offset + copy * f->table_bytes + offset;
		if (f->variant == 12) {
			CHECK(pread(f->fd, bytes, 2, (off_t)address) == 2);
			packed = (uint16_t)(bytes[0] + 256 * bytes[1]);
			if ((cluster & 1) != 0)
				packed = (uint16_t)((packed & 15) | (value << 4));
			else
				packed = (uint16_t)((packed & 0xf000) | value);
			put16(bytes, packed);
		} else {
			put32(bytes, value);
		}
		write_bytes(f, address, bytes, length);
	}
}

static uint64_t
cluster_offset(const fixture_t *f, uint32_t cluster)
{
	return (f->data_offset + (uint64_t)(cluster - 2) * f->cluster_bytes);
}

static void
short_entry(unsigned char *raw, const char *name, bool directory,
    uint32_t cluster, uint32_t size)
{
	bzero(raw, 32);
	bcopy(name, raw, 11);
	raw[11] = directory ? 0x10 : 0x20;
	put16(raw + 20, (uint16_t)(cluster >> 16));
	put16(raw + 26, (uint16_t)cluster);
	put32(raw + 28, size);
}

static fsimg_status_t
image_size(void *argument, uint64_t *out, fsimg_error_t *error FSIMG_UNUSED)
{
	fixture_t *f = argument;

	*out = f->bytes;
	return (FSIMG_OK);
}

static fsimg_status_t
image_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error)
{
	fixture_t *f = argument;
	ssize_t count;
	bool fail;

	f->calls++;
	if (offset >= f->data_offset && offset != f->root_offset &&
	    offset != cluster_offset(f, 7))
		f->payload_calls++;
	fail = f->fail_call != 0 && f->calls == f->fail_call;
	if (f->short_bytes != 0 && length > f->short_bytes)
		length = (size_t)f->short_bytes;
	if (fail && length > 7)
		length = 7;
	count = pread(f->fd, buffer, length, (off_t)offset);
	*transferred = count > 0 ? (size_t)count : 0;
	if (fail || count < 0)
		return (fsimg_error_set(error, FSIMG_EIO, EIO,
		    FSIMG_OP_IO, "injected image read failure"));
	return (FSIMG_OK);
}

static void
fixture_init(fixture_t *f, unsigned variant)
{
	fixture_geometry(f, variant, 512, 1);
}

static void
fixture_geometry(fixture_t *f, unsigned variant, unsigned sector_bytes,
    unsigned sectors_per_cluster)
{
	unsigned char boot[512], root[512], directory[512], data[512], info[512];
	uint32_t sectors, reserved, table_sectors, root_sectors, file_clusters;
	char name[] = "fat-image-XXXXXX";

	bzero(f, sizeof(*f));
	f->variant = variant;
	f->sector_bytes = sector_bytes;
	f->cluster_bytes = sector_bytes * sectors_per_cluster;
	f->clusters = variant == 12 ? 3000 : variant == 16 ? 5000 : 70000;
	f->eoc = variant == 12 ? 0xfff : variant == 16 ? 0xffff : 0x0fffffff;
	reserved = variant == 32 ? 32 : 1;
	table_sectors = ((f->clusters + 2) * variant + 8 * sector_bytes - 1) /
	    (8 * sector_bytes);
	root_sectors = variant == 32 ? 0 : (512 * 32 + sector_bytes - 1) / sector_bytes;
	sectors = reserved + 2 * table_sectors + root_sectors +
	    f->clusters * sectors_per_cluster;
	f->bytes = (uint64_t)sectors * sector_bytes;
	f->fat_offset = (uint64_t)reserved * sector_bytes;
	f->table_bytes = (uint64_t)table_sectors * sector_bytes;
	f->data_offset = (reserved + 2 * (uint64_t)table_sectors + root_sectors) * sector_bytes;
	f->root_offset = variant == 32 ? f->data_offset :
	    f->fat_offset + 2 * f->table_bytes;
	f->fd = mkstemp(name);
	CHECK(f->fd != -1 && unlink(name) == 0);
	CHECK(ftruncate(f->fd, (off_t)f->bytes) == 0);

	bzero(boot, sizeof(boot));
	boot[0] = 0xeb;
	boot[1] = 0x58;
	boot[2] = 0x90;
	bcopy("FSIMGTST", boot + 3, 8);
	put16(boot + 11, (uint16_t)sector_bytes);
	boot[13] = (unsigned char)sectors_per_cluster;
	put16(boot + 14, (uint16_t)reserved);
	boot[16] = 2;
	put16(boot + 17, variant == 32 ? 0 : 512);
	if (sectors < 65536 && variant != 32)
		put16(boot + 19, (uint16_t)sectors);
	else
		put32(boot + 32, sectors);
	boot[21] = 0xf8;
	if (variant == 32) {
		put32(boot + 36, table_sectors);
		put32(boot + 44, 2);
		put16(boot + 48, 1);
		put16(boot + 50, 6);
		boot[66] = 0x29;
		bcopy("FSIMG TEST ", boot + 71, 11);
	} else {
		put16(boot + 22, (uint16_t)table_sectors);
		boot[38] = 0x29;
		bcopy("FSIMG TEST ", boot + 43, 11);
	}
	boot[510] = 0x55;
	boot[511] = 0xaa;
	write_bytes(f, 0, boot, sizeof(boot));
	set_cluster(f, 0, (f->eoc & ~0xffu) | 0xf8);
	set_cluster(f, 1, f->eoc);
	file_clusters = (1100 + f->cluster_bytes - 1) / f->cluster_bytes;
	if (variant == 32) {
		set_cluster(f, 2, f->eoc);
		write_bytes(f, 6 * sector_bytes, boot, sizeof(boot));
		bzero(info, sizeof(info));
		put32(info, 0x41615252);
		put32(info + 484, 0x61417272);
		put32(info + 488, f->clusters - file_clusters - 2);
		put32(info + 492, 10);
		put32(info + 508, 0xaa550000);
		write_bytes(f, sector_bytes, info, sizeof(info));
	}

	/* Nonadjacent clusters carry one predictable file. Larger allocation
	 * units need fewer clusters but never require a larger transfer buffer.
	 */
	set_cluster(f, 7, f->eoc);
	for (unsigned part = 0; part < file_clusters; part++) {
		uint32_t cluster = part == 0 ? 3 : part == 1 ? 9 : 5;
		uint32_t remaining = 1100 - part * f->cluster_bytes;
		if (remaining > f->cluster_bytes)
			remaining = f->cluster_bytes;
		set_cluster(f, cluster, part + 1 == file_clusters ? f->eoc :
		    part == 0 ? 9 : 5);
		for (uint32_t offset = 0; offset < remaining; offset += 512) {
			for (unsigned i = 0; i < sizeof(data); i++)
				data[i] = (unsigned char)((part * f->cluster_bytes + offset + i) * 37);
			write_bytes(f, cluster_offset(f, cluster) + offset, data,
			    remaining - offset > sizeof(data) ? sizeof(data) : remaining - offset);
		}
	}
	bzero(root, sizeof(root));
	short_entry(root, "HELLO   TXT", false, 3, 1100);
	/* Leap day, 23:59:58 plus 1.99 seconds of creation precision. */
	put16(root + 16, (44u << 9) | (2u << 5) | 29u);
	put16(root + 14, (23u << 11) | (59u << 5) | 29u);
	root[13] = 199;
	short_entry(root + 32, "SUB        ", true, 7, 0);
	write_bytes(f, f->root_offset, root, sizeof(root));
	bzero(directory, sizeof(directory));
	short_entry(directory, ".          ", true, 7, 0);
	short_entry(directory + 32, "..         ", true, 0, 0);
	short_entry(directory + 64, "EMPTY      ", false, 0, 0);
	write_bytes(f, cluster_offset(f, 7), directory, sizeof(directory));
	strcpy(f->directory, "fat-scratch-XXXXXX");
	CHECK(mkdtemp(f->directory) != NULL);
	f->scratch = open(f->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(f->scratch != -1);
}

static void
fixture_open(fixture_t *f)
{
	fsimg_options_t options;
	fsimg_io_ops_t ops;

	fsimg_options_init(&options);
	options.scratch_directory_fd = f->scratch;
	options.memory_bytes = 16 * 1024 * 1024;
	options.cancelled = cancelled;
	options.cancel_argument = f;
	CHECK(fsimg_ctx_new(&options, &f->ctx, NULL) == FSIMG_OK);
	fsimg_io_ops_init(&ops);
	ops.read_at = image_read;
	ops.get_size = image_size;
	CHECK(fsimg_io_from_callbacks(f->ctx, &ops, f, &f->io, NULL) == FSIMG_OK);
}

static void
fixture_close(fixture_t *f)
{
	fsimg_stats_t stats;

	fsimg_reader_free(f->reader);
	fsimg_io_free(f->io);
	if (f->ctx != NULL) {
		CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
		CHECK(stats.scratch_bytes == 0 && stats.descriptors == 1);
		CHECK(stats.memory_bytes < 2048);
		fsimg_ctx_free(f->ctx);
	}
	CHECK(close(f->fd) == 0);
	CHECK(close(f->scratch) == 0);
	CHECK(rmdir(f->directory) == 0);
}

static void
test_variants(void)
{
	static const unsigned variants[] = { 16, 12, 32 };
	fixture_t f;
	fsimg_read_options_t options;
	fsimg_volume_info_t info;
	fsimg_verify_report_t report;
	fsimg_format_info_t format;
	const fsimg_entry_t *entry;
	unsigned char data[1100];
	uint64_t calls, work;
	size_t count, cursor;
	fsimg_range_t range;

	cursor = 0;
	for (unsigned i = 0; i < 4; i++) {
		CHECK(fsimg_format_next(&cursor, &format, NULL) == FSIMG_OK);
		CHECK((format.capabilities & FSIMG_CAP_READ) != 0);
		CHECK((format.capabilities & FSIMG_CAP_WRITE) != 0);
	}
	CHECK(fsimg_format_next(&cursor, &format, NULL) == FSIMG_DONE);
	CHECK(fsimg_format_next(&cursor, &format, NULL) == FSIMG_DONE);
	for (unsigned i = 0; i < 3; i++) {
		fixture_init(&f, variants[i]);
		fixture_open(&f);
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
		CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
		CHECK(info.allocation_units == f.clusters && info.volume_bytes == f.bytes);
		CHECK(info.scope == FSIMG_VALIDATION_HEADER && f.payload_calls == 0);
		CHECK(f.calls <= 4);
		CHECK(fsimg_reader_read_at(f.reader, 0, data, 1, &count, NULL) == FSIMG_ESTATE);

		CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
		CHECK(strcmp(entry->path, "/HELLO.TXT") == 0 && entry->size == 1100);
		CHECK(entry->metadata.created.seconds == INT64_C(1709251199));
		CHECK(entry->metadata.created.nanoseconds == 990000000);
		CHECK(entry->metadata.created.basis == FSIMG_TIME_WALL);
		CHECK(fsimg_reader_read_at(f.reader, 1101, data, 1, &count, NULL) == FSIMG_EINVAL);
		CHECK(strcmp(entry->path, "/HELLO.TXT") == 0);
		calls = f.calls;
		work = f.reader->session.progress.work;
		for (unsigned j = 0; j < sizeof(data); j++) {
			CHECK(fsimg_reader_read_at(f.reader, j, data + j, 1, &count, NULL) == FSIMG_OK);
			CHECK(count == 1 && data[j] == (unsigned char)(j * 37));
		}
		CHECK(f.calls - calls <= 1110);
		/* One dispatch and one physical read per byte, plus three chain hops. */
		CHECK(f.reader->session.progress.work - work == 2203);
		CHECK(fsimg_reader_read_at(f.reader, 1100, data, 8, &count, NULL) == FSIMG_OK);
		CHECK(count == 0);
		CHECK(fsimg_reader_read_at(f.reader, 1020, data, 80, &count, NULL) == FSIMG_OK);
		CHECK(count == 80 && fsimg_test_extent_count(fsimg_fat_state(f.reader)->index) == 3);
		for (unsigned j = 0; j < 80; j++)
			CHECK(data[j] == (unsigned char)((1020 + j) * 37));
		CHECK(fsimg_reader_next_range(f.reader, 512, &range, NULL) == FSIMG_OK);
		CHECK(range.offset == 512 && range.length == 512 && range.kind == FSIMG_RANGE_DATA);
		CHECK(fsimg_reader_next_range(f.reader, 1100, &range, NULL) == FSIMG_DONE);
		CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
		CHECK(strcmp(entry->path, "/SUB") == 0);
		CHECK(fsimg_fat_state(f.reader)->index == NULL);
		CHECK(fsimg_reader_read_at(f.reader, 0, data, 1, &count, NULL) == FSIMG_ESTATE);
		CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
		CHECK(strcmp(entry->path, "/SUB/EMPTY") == 0 && entry->size == 0);
		CHECK(fsimg_reader_next_range(f.reader, 0, &range, NULL) == FSIMG_DONE);
		CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_DONE && entry == NULL);
		CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_DONE && entry == NULL);
		CHECK(fsimg_verify(f.reader, 0, &report, NULL) == FSIMG_ESTATE);
		fsimg_reader_free(f.reader);
		f.reader = NULL;

		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
		CHECK(fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD, &report, NULL) == FSIMG_OK);
		CHECK(report.complete && report.scope == FSIMG_VALIDATION_PAYLOAD);
		CHECK(report.files == 2 && report.directories == 2 && report.logical_bytes == 1100);
		CHECK(report.payload_bytes_read == 1100);
		CHECK(report.allocated_clusters == (variants[i] == 32 ? 5 : 4));
		CHECK(report.free_clusters + report.allocated_clusters == f.clusters);
		CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
		CHECK((info.valid & FSIMG_VOLUME_FREE) != 0);
		fsimg_read_options_init(&options);
		options.format = variants[i] == 12 ? "fat16" : "fat12";
		{
			fsimg_reader_t *other;
			CHECK(fsimg_reader_open(f.ctx, f.io, &options, &other, NULL) == FSIMG_EFORMAT);
			CHECK(other == NULL);
			/* A forced decoder never falls back to the detected one. */
			options.format = "exfat";
			CHECK(fsimg_reader_open(f.ctx, f.io, &options, &other, NULL) == FSIMG_EFORMAT);
			CHECK(other == NULL);
			options.format = "qnx6";
			CHECK(fsimg_reader_open(f.ctx, f.io, &options, &other, NULL) == FSIMG_EUNSUPPORTED);
			CHECK(other == NULL);
		}
		fixture_close(&f);
	}
}

static void
test_geometry_matrix(void)
{
	static const unsigned variants[] = { 12, 16, 32 };
	fixture_t f;
	fsimg_verify_report_t report;
	fsimg_error_t error;
	fsimg_status_t status;

	for (unsigned v = 0; v < 3; v++) {
		for (unsigned sector = 512; sector <= 4096; sector *= 2) {
			for (unsigned spc = 1; spc <= 128; spc *= 2) {
				fixture_geometry(&f, variants[v], sector, spc);
				fixture_open(&f);
				status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, &error);
				if (status == FSIMG_OK)
					status = fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD, &report, &error);
				if (status != FSIMG_OK)
					fprintf(stderr, "FAT%u sector=%u spc=%u: %s\n",
					    variants[v], sector, spc, error.message);
				CHECK(status == FSIMG_OK && report.complete);
				CHECK(report.payload_bytes_read == 1100);
				fixture_close(&f);
			}
		}
	}
}

static void
test_cluster_ceiling(unsigned variant)
{
	fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_verify_report_t report;
	unsigned char boot[512], zeroes[4096], root[512], byte;
	uint64_t position;
	uint32_t sectors;
	size_t batch, count;

	fixture_init(&f, variant);
	if (variant == 32) {
		fat_geometry_t geometry;

		/* This is a logical BPB boundary, without a large-file traversal. */
		CHECK(pread(f.fd, boot, sizeof(boot), 0) == sizeof(boot));
		put32(boot + 36, 2097152);
		put32(boot + 32, 32 + 4194304 + 0x0ffffff5);
		put32(boot + 44, 0x0ffffff6);
		CHECK(fsimg_fat_geometry_decode(boot, UINT64_MAX, &geometry, NULL) == FSIMG_OK);
		CHECK(fsimg_fat_cluster_valid(&geometry, 0x0ffffff6));
		put32(boot + 44, 0x0ffffff7);
		CHECK(fsimg_fat_geometry_decode(boot, UINT64_MAX, &geometry, NULL) == FSIMG_ECORRUPT);
		put32(boot + 44, 2);
		put32(boot + 32, 32 + 4194304 + 0x0ffffff6);
		CHECK(fsimg_fat_geometry_decode(boot, UINT64_MAX, &geometry, NULL) == FSIMG_ECORRUPT);
		fixture_close(&f);
		return;
	}
	f.clusters = variant == 12 ? 4084 : 65524;
	f.table_bytes = ((f.clusters + 2) * variant + 4095) / 4096 * UINT64_C(512);
	f.root_offset = f.fat_offset + 2 * f.table_bytes;
	f.data_offset = f.root_offset + 32 * 512;
	f.bytes = f.data_offset + (uint64_t)f.clusters * 512;
	CHECK(ftruncate(f.fd, (off_t)f.bytes) == 0);
	CHECK(pread(f.fd, boot, sizeof(boot), 0) == sizeof(boot));
	sectors = (uint32_t)(f.bytes / 512);
	put16(boot + 22, (uint16_t)(f.table_bytes / 512));
	put16(boot + 19, sectors < 65536 ? (uint16_t)sectors : 0);
	put32(boot + 32, sectors < 65536 ? 0 : sectors);
	write_bytes(&f, 0, boot, sizeof(boot));

	bzero(zeroes, sizeof(zeroes));
	for (position = f.fat_offset; position < f.root_offset; position += batch) {
		batch = f.root_offset - position > sizeof(zeroes) ? sizeof(zeroes) :
		    (size_t)(f.root_offset - position);
		write_bytes(&f, position, zeroes, batch);
	}
	set_cluster(&f, 0, (f.eoc & ~0xffu) | 0xf8);
	set_cluster(&f, 1, f.eoc);
	set_cluster(&f, f.clusters + 1, f.eoc);
	bzero(root, sizeof(root));
	short_entry(root, "CEILING BIN", false, f.clusters + 1, 1);
	write_bytes(&f, f.root_offset, root, sizeof(root));
	write_bytes(&f, cluster_offset(&f, f.clusters + 1), "Q", 1);

	/* The last cluster is MAX = CountOfClusters + 1. Values between MAX+1
	 * and the bad-cluster marker are reserved; a fixed 0xff0/0xfff0 cutoff
	 * would incorrectly reject these legal classification-boundary images.
	 */
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_read_at(f.reader, 0, &byte, 1, &count, NULL) == FSIMG_OK);
	CHECK(count == 1 && byte == 'Q');
	fsimg_reader_free(f.reader);
	f.reader = NULL;
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_verify(f.reader, 0, &report, NULL) == FSIMG_OK);
	CHECK(report.complete && report.allocated_clusters == 1);
	fixture_close(&f);
}

static void
test_names(void)
{
	unsigned char raw[32], slot[32], key1[FAT_KEY_BYTES], key2[FAT_KEY_BYTES];
	fat_lfn_t lfn;
	char name[FAT_NAME_BYTES];
	size_t len1, len2;
	static const unsigned positions[] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
	unsigned char checksum;

	short_entry(raw, "CAFE    TXT", false, 0, 0);
	raw[3] = 0x90; /* CP437 capital E with acute. */
	CHECK(fsimg_fat_short_name(raw, name, NULL) == FSIMG_OK);
	CHECK(strcmp(name, "CAF\xc3\x89.TXT") == 0);
	raw[12] = 0x18;
	CHECK(fsimg_fat_short_name(raw, name, NULL) == FSIMG_OK);
	CHECK(strcmp(name, "caf\xc3\xa9.txt") == 0);
	raw[0] = 5;
	CHECK(fsimg_fat_short_name(raw, name, NULL) == FSIMG_OK);
	CHECK((unsigned char)name[0] == 0xcf); /* CP437 0xe5 is sigma. */

	CHECK(fsimg_fat_name_key("caf\xc3\xa9.txt", key1, &len1, NULL) == FSIMG_OK);
	CHECK(fsimg_fat_name_key("CAF\xc3\x89.TXT", key2, &len2, NULL) == FSIMG_OK);
	CHECK(len1 == len2 && bcmp(key1, key2, len1) == 0);
	/* Deseret case mapping crosses a UTF-16 surrogate pair. */
	CHECK(fsimg_fat_name_key("\xf0\x90\x90\xa8", key1, &len1, NULL) == FSIMG_OK);
	CHECK(fsimg_fat_name_key("\xf0\x90\x90\x80", key2, &len2, NULL) == FSIMG_OK);
	CHECK(len1 == 4 && len2 == 4 && bcmp(key1, key2, 4) == 0);
	CHECK(fsimg_fat_name_key("\xc3\x9f", key1, &len1, NULL) == FSIMG_OK);
	CHECK(fsimg_fat_name_key("SS", key2, &len2, NULL) == FSIMG_OK);
	CHECK(len1 != len2);

	short_entry(raw, "LONGNA~1TXT", false, 0, 0);
	checksum = 0;
	for (unsigned i = 0; i < 11; i++)
		checksum = (unsigned char)((checksum / 2) + (checksum % 2) * 128 + raw[i]);
	memset(slot, 0xff, sizeof(slot));
	slot[0] = 0x41;
	slot[11] = 0x0f;
	slot[12] = 0;
	slot[13] = checksum;
	put16(slot + 26, 0);
	for (unsigned i = 0; i < 10; i++)
		put16(slot + positions[i], (uint16_t)"Long name!"[i]);
	put16(slot + positions[10], 0);
	bzero(&lfn, sizeof(lfn));
	CHECK(fsimg_fat_lfn_slot(&lfn, slot, NULL) == FSIMG_OK);
	CHECK(fsimg_fat_entry_name(&lfn, raw, name, NULL) == FSIMG_OK);
	CHECK(strcmp(name, "Long name!") == 0);
	bzero(&lfn, sizeof(lfn));
	slot[0] = 0x42;
	CHECK(fsimg_fat_lfn_slot(&lfn, slot, NULL) == FSIMG_OK);
	CHECK(fsimg_fat_entry_name(&lfn, raw, name, NULL) == FSIMG_ECORRUPT);
	bzero(&lfn, sizeof(lfn));
	slot[0] = 0x41;
	put16(slot + positions[0], 0xd800);
	CHECK(fsimg_fat_lfn_slot(&lfn, slot, NULL) == FSIMG_OK);
	CHECK(fsimg_fat_entry_name(&lfn, raw, name, NULL) == FSIMG_ECORRUPT);
	/* DIR_Name[0] cannot be a space, even with later nonspace characters. */
	short_entry(raw, " SPACE  TXT", false, 0, 0);
	CHECK(fsimg_fat_short_name(raw, name, NULL) == FSIMG_ECORRUPT);
	CHECK(fsimg_fat_name_validate("\xc0\x80", NULL) == FSIMG_ECORRUPT);
}

static void
test_thresholds(void)
{
	fixture_t f;
	fat_geometry_t geometry;
	unsigned char boot[512];
	uint32_t clusters, table_sectors, sectors;

	for (unsigned variant = 16; variant <= 32; variant += 16) {
		fixture_init(&f, variant);
		CHECK(pread(f.fd, boot, sizeof(boot), 0) == sizeof(boot));
		clusters = variant == 16 ? 65524 : 65525;
		table_sectors = ((clusters + 2) * variant + 4095) / 4096;
		sectors = (variant == 16 ? 1 + 32 : 32) + 2 * table_sectors + clusters;
		put16(boot + 19, 0);
		put32(boot + 32, sectors);
		if (variant == 16)
			put16(boot + 22, (uint16_t)table_sectors);
		else
			put32(boot + 36, table_sectors);
		CHECK(fsimg_fat_geometry_decode(boot, (uint64_t)sectors * 512, &geometry, NULL) == FSIMG_OK);
		CHECK(geometry.variant == variant && geometry.clusters == clusters);
		fixture_close(&f);
	}
}

static void
test_deep_tree(void)
{
	fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_verify_report_t report;
	unsigned char root[512], directory[512];
	char name[12], expected[256];

	fixture_init(&f, 16);
	set_cluster(&f, 3, 0);
	set_cluster(&f, 5, 0);
	set_cluster(&f, 7, 0);
	set_cluster(&f, 9, 0);
	bzero(root, sizeof(root));
	short_entry(root, "D00        ", true, 20, 0);
	write_bytes(&f, f.root_offset, root, sizeof(root));
	for (unsigned i = 0; i < 20; i++) {
		set_cluster(&f, 20 + i, f.eoc);
		bzero(directory, sizeof(directory));
		short_entry(directory, ".          ", true, 20 + i, 0);
		short_entry(directory + 32, "..         ", true, i == 0 ? 0 : 19 + i, 0);
		if (i == 19) {
			short_entry(directory + 64, "EMPTY      ", false, 0, 0);
		} else {
			snprintf(name, sizeof(name), "D%02u        ", i + 1);
			short_entry(directory + 64, name, true, 21 + i, 0);
		}
		write_bytes(&f, cluster_offset(&f, 20 + i), directory, sizeof(directory));
	}

	/* Every descent retains its parent's path prefix and cursor. Each
	 * non-root parent has a real cluster; only the first '..' names zero.
	 */
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	expected[0] = '\0';
	for (unsigned i = 0; i < 20; i++) {
		snprintf(name, sizeof(name), "/D%02u", i);
		strcat(expected, name);
		CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
		CHECK(entry->kind == FSIMG_ENTRY_DIRECTORY && strcmp(entry->path, expected) == 0);
	}
	strcat(expected, "/EMPTY");
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(entry->kind == FSIMG_ENTRY_FILE && strcmp(entry->path, expected) == 0);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_DONE);
	fsimg_reader_free(f.reader);
	f.reader = NULL;
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_verify(f.reader, 0, &report, NULL) == FSIMG_OK);
	CHECK(report.complete && report.directories == 21 && report.files == 1);
	CHECK(report.allocated_clusters == 20);
	fixture_close(&f);
}

static void
test_detection(void)
{
	fixture_t f;
	unsigned char boot[512];

	/* OEM text is arbitrary on FAT. A formatter name alone cannot override
	 * valid BPB geometry or identify a different filesystem family.
	 */
	fixture_init(&f, 16);
	write_bytes(&f, 3, "EXFAT   ", 8);
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	fixture_close(&f);

	fixture_init(&f, 16);
	bzero(boot, sizeof(boot));
	bcopy("EXFAT   ", boot + 3, 8);
	write_bytes(&f, 0, boot, sizeof(boot));
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_EFORMAT);
	fixture_close(&f);

	fixture_init(&f, 16);
	boot[0] = 0xeb;
	boot[1] = 0x76;
	boot[2] = 0x90;
	boot[510] = 0x55;
	boot[511] = 0xaa;
	write_bytes(&f, 0, boot, sizeof(boot));
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_ECORRUPT);
	fixture_close(&f);
}

static void
test_geometry(void)
{
	fixture_t f;
	fat_geometry_t geometry;
	unsigned char boot[512];
	uint32_t sectors;
	uint32_t value;

	fixture_init(&f, 16);
	CHECK(pread(f.fd, boot, sizeof(boot), 0) == sizeof(boot));
	for (unsigned clusters = 4084; clusters <= 4085; clusters++) {
		/* A 32-sector FAT has room for either threshold's packed entries. */
		put16(boot + 22, 32);
		sectors = 1 + 64 + 32 + clusters;
		put16(boot + 19, (uint16_t)sectors);
		CHECK(fsimg_fat_geometry_decode(boot, (uint64_t)sectors * 512, &geometry, NULL) == FSIMG_OK);
		CHECK(geometry.variant == (clusters == 4084 ? 12 : 16));
	}
	/* An illegal fixed BPB field means the record is not FAT at all. */
	boot[13] = 3;
	CHECK(fsimg_fat_geometry_decode(boot, UINT64_MAX, &geometry, NULL) == FSIMG_EFORMAT);
	fixture_close(&f);

	/* Cluster 2730 starts at byte 4095 of a packed FAT12 table. Its second
	 * byte is in the next window; the neighboring odd entry shares its bits.
	 */
	fixture_init(&f, 12);
	set_cluster(&f, 2730, 1234);
	set_cluster(&f, 2731, 2345);
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_fat_table_get(f.reader, 2730, &value, NULL) == FSIMG_OK && value == 1234);
	CHECK(fsimg_fat_table_get(f.reader, 2731, &value, NULL) == FSIMG_OK && value == 2345);
	fixture_close(&f);
}

static void
test_corruption(void)
{
	fixture_t f;
	fsimg_verify_report_t report;
	const fsimg_entry_t *entry;
	unsigned char raw[64], byte, data[1100];
	size_t count;

	/* Each mutation starts from an independent fresh valid image. */
	for (unsigned scenario = 0; scenario < 16; scenario++) {
		fixture_init(&f, 16);
		CHECK(pread(f.fd, raw, sizeof(raw), (off_t)f.root_offset) == sizeof(raw));
		switch (scenario) {
		case 0: set_cluster(&f, 9, 3); break; /* file cycle */
		case 1: set_cluster(&f, 3, 0); break; /* free link */
		case 2: set_cluster(&f, 3, 0xfff7); break; /* bad link */
		case 3: set_cluster(&f, 3, 5002); break; /* outside data */
		case 4: set_cluster(&f, 3, f.eoc); break; /* short allocation */
		case 5: set_cluster(&f, 5, 10); set_cluster(&f, 10, f.eoc); break;
		case 6: set_cluster(&f, 7, 7); break; /* directory cycle */
		case 7: short_entry(raw + 32, "HELLO   TXT", false, 0, 0); break;
		case 8: short_entry(raw + 32, "hello   txt", false, 0, 0); break;
		case 9: short_entry(raw + 32, "OTHER   TXT", false, 3, 1100); break;
		case 10: set_cluster(&f, 12, f.eoc); break; /* orphan */
		case 11: raw[0] = '/'; break;
		case 12: put16(raw + 16, (44u << 9) | (2u << 5) | 30u); break;
		case 13: raw[11] = 0x80; break;
		case 14: raw[32 + 26] = 3; break; /* directory shares file chain */
		case 15: raw[0] = ' '; break; /* forbidden first short-name byte */
		}
		write_bytes(&f, f.root_offset, raw, sizeof(raw));
		fixture_open(&f);
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
		if (scenario == 7 || scenario == 8 || scenario == 11 ||
		    scenario == 12 || scenario == 13 || scenario == 15) {
			CHECK(fsimg_reader_next(f.reader, &entry, NULL) ==
			    (scenario == 13 ? FSIMG_EUNSUPPORTED : FSIMG_ECORRUPT));
			CHECK(entry == NULL);
		} else {
			CHECK(fsimg_verify(f.reader, 0, &report, NULL) == FSIMG_ECORRUPT);
			CHECK(!report.complete);
		}
		CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_ESTATE);
		fixture_close(&f);
	}

	fixture_init(&f, 16);
	byte = 0;
	write_bytes(&f, f.fat_offset + f.table_bytes + 3 * 2, &byte, 1);
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_read_at(f.reader, 0, data, sizeof(data), &count, NULL) == FSIMG_ECORRUPT);
	fixture_close(&f);

	/* Nonmirrored FAT32 must ignore an inactive damaged table. */
	fixture_init(&f, 32);
	byte = 0x81;
	write_bytes(&f, 40, &byte, 1);
	byte = 0;
	write_bytes(&f, f.fat_offset, &byte, 1);
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_read_at(f.reader, 0, data, sizeof(data), &count, NULL) == FSIMG_OK);
	fixture_close(&f);
}

static void
test_limits(void)
{
	fixture_t f;
	fsimg_error_t error;
	const fsimg_entry_t *entry;
	unsigned char data[1100];
	size_t count;

	fixture_init(&f, 16);
	fixture_open(&f);
	f.ctx->options.max_entries = 1;
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_ENTRIES && entry == NULL);
	fixture_close(&f);

	fixture_init(&f, 16);
	fixture_open(&f);
	f.ctx->options.max_depth = 1;
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_DEPTH);
	fixture_close(&f);

	fixture_init(&f, 16);
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	f.ctx->options.max_work = f.reader->session.progress.work + 3;
	CHECK(fsimg_reader_read_at(f.reader, 0, data, 1, &count, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_read_at(f.reader, 1, data, 1, &count, &error) == FSIMG_ELIMIT);
	CHECK(error.resource == FSIMG_RESOURCE_WORK);
	fixture_close(&f);
}

static void
test_failures(void)
{
	fixture_t f;
	fsimg_verify_report_t report;
	fsimg_status_t status;
	uint64_t attempts;
	bool completed;

	/* Exhaust every constructor, prepass, sort and verifier allocation site.
	 * Each failure must release all charged private state by destruction.
	 */
	completed = false;
	for (attempts = 1; attempts < 150; attempts++) {
		fixture_init(&f, 16);
		fixture_open(&f);
		fsimg_test_fail_alloc_after(f.ctx, attempts);
		status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL);
		if (status == FSIMG_OK)
			status = fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD, &report, NULL);
		CHECK(status == FSIMG_OK || status == FSIMG_ENOMEM);
		completed = status == FSIMG_OK;
		fsimg_test_fail_alloc_after(f.ctx, 0);
		fixture_close(&f);
		if (completed)
			break;
	}
	CHECK(completed);

	fixture_init(&f, 16);
	fixture_open(&f);
	f.short_bytes = 7;
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD, &report, NULL) == FSIMG_OK);
	fixture_close(&f);
}

static void
test_partial_content(void)
{
	fixture_t f;
	const fsimg_entry_t *entry;
	unsigned char data[64];
	size_t count;

	fixture_init(&f, 16);
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	f.fail_call = f.calls + 1;
	CHECK(fsimg_reader_read_at(f.reader, 0, data, sizeof(data), &count, NULL) == FSIMG_EIO);
	CHECK(count == 7);
	for (unsigned i = 0; i < count; i++)
		CHECK(data[i] == (unsigned char)(i * 37));
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_ESTATE);
	fixture_close(&f);

	/* After a seek builds the extent index, a read crossing from one indexed
	 * extent into the next still reports the bytes confirmed before failing.
	 */
	fixture_init(&f, 16);
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_read_at(f.reader, 600, data, 1, &count, NULL) == FSIMG_OK);
	CHECK(fsimg_test_extent_count(fsimg_fat_state(f.reader)->index) == 3);
	f.fail_call = f.calls + 2;
	CHECK(fsimg_reader_read_at(f.reader, 1020, data, sizeof(data), &count, NULL) ==
	    FSIMG_EIO);
	CHECK(count == 4 + 7);
	for (unsigned i = 0; i < count; i++)
		CHECK(data[i] == (unsigned char)((1020 + i) * 37));
	fixture_close(&f);
}

static void
test_host_change(void)
{
	fixture_t f;
	fsimg_io_t *host;
	const fsimg_entry_t *entry;
	fsimg_range_t range;
	unsigned char byte, bytes[4096];
	uint64_t offset;
	size_t count;
	char path[32];
	int readonly, output;

	for (unsigned scenario = 0; scenario < 2; scenario++) {
		fixture_init(&f, 16);
		fixture_open(&f);
		strcpy(path, "fat-host-XXXXXX");
		output = mkstemp(path);
		CHECK(output != -1);
		for (offset = 0; offset < f.bytes; offset += count) {
			count = f.bytes - offset > sizeof(bytes) ? sizeof(bytes) :
			    (size_t)(f.bytes - offset);
			CHECK(pread(f.fd, bytes, count, (off_t)offset) == (ssize_t)count);
			CHECK(write(output, bytes, count) == (ssize_t)count);
		}
		readonly = open(path, O_RDONLY | O_CLOEXEC);
		CHECK(readonly != -1 && unlink(path) == 0);
		CHECK(fsimg_io_from_fd(f.ctx, readonly, FSIMG_IO_READ, &host, NULL) == FSIMG_OK);
		CHECK(fsimg_reader_open(f.ctx, host, NULL, &f.reader, NULL) == FSIMG_OK);
		if (scenario == 1) {
			CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
			CHECK(fsimg_reader_next_range(f.reader, 0, &range, NULL) == FSIMG_OK);
		}

		CHECK(pwrite(output, "X", 1, (off_t)cluster_offset(&f, 3)) == 1);
		/* Make the metadata change visible even on coarse timestamp clocks. */
		{
			const struct timespec changed[2] = {
				{ .tv_nsec = UTIME_OMIT }, { .tv_sec = 1 }
			};

			CHECK(futimens(output, changed) == 0);
		}
		if (scenario == 0)
			CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_ESOURCE_CHANGED);
		else
			CHECK(fsimg_reader_next_range(f.reader, 0, &range, NULL) == FSIMG_ESOURCE_CHANGED);
		CHECK(fsimg_reader_read_at(f.reader, 0, &byte, 1, &count, NULL) == FSIMG_ESTATE);
		fsimg_reader_free(f.reader);
		f.reader = NULL;
		fsimg_io_free(host);
		CHECK(close(readonly) == 0 && close(output) == 0);
		fixture_close(&f);
	}
}

static void
test_wide(unsigned entries)
{
	fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_stats_t stats;
	unsigned char record[32];
	char name[12];
	unsigned count;
	fsimg_verify_report_t report;
	struct rusage usage;
	struct timespec started, finished;
	double elapsed;
	unsigned char boot[512];

	CHECK(clock_gettime(CLOCK_MONOTONIC, &started) == 0);
	fixture_init(&f, 16);
	if (entries > 512) {
		CHECK(entries == 65520);
		f.data_offset += (4095 - 32) * 512;
		f.bytes = f.data_offset + (uint64_t)f.clusters * 512;
		CHECK(ftruncate(f.fd, (off_t)f.bytes) == 0);
		CHECK(pread(f.fd, boot, sizeof(boot), 0) == sizeof(boot));
		put16(boot + 17, (uint16_t)entries);
		put16(boot + 19, (uint16_t)(f.bytes / 512));
		write_bytes(&f, 0, boot, sizeof(boot));
		set_cluster(&f, 3, 0);
		set_cluster(&f, 5, 0);
		set_cluster(&f, 7, 0);
		set_cluster(&f, 9, 0);
	}
	for (unsigned i = 0; i < entries; i++) {
		snprintf(name, sizeof(name), "%08uTXT", i);
		short_entry(record, name, false, 0, 0);
		write_bytes(&f, f.root_offset + i * 32, record, sizeof(record));
	}
	fixture_open(&f);
	CHECK(fsimg_test_meta_cache(f.ctx, 1, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	count = 0;
	while (fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK) {
		char path[16];
		snprintf(path, sizeof(path), "/%08u.TXT", count);
		CHECK(strcmp(path, entry->path) == 0);
		count++;
	}
	CHECK(count == entries);
	CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.scratch_peak > 32768 && stats.memory_peak < 16 * 1024 * 1024);
	if (entries > 512) {
		CHECK(stats.scratch_peak > 16 * 1024 * 1024);
		fsimg_reader_free(f.reader);
		f.reader = NULL;
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
		CHECK(fsimg_verify(f.reader, 0, &report, NULL) == FSIMG_OK);
		CHECK(report.complete && report.files == entries && report.allocated_clusters == 0);
		CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
		CHECK(getrusage(RUSAGE_SELF, &usage) == 0);
		CHECK(clock_gettime(CLOCK_MONOTONIC, &finished) == 0);
		elapsed = (double)(finished.tv_sec - started.tv_sec) +
		    (double)(finished.tv_nsec - started.tv_nsec) / 1000000000.0;
		printf("FAT entries=%u memory_peak=%llu scratch_peak=%llu descriptors_peak=%llu io_calls=%llu physical_scratch_peak=%llu elapsed=%.3f maxrss=%ld\n",
		    entries, (unsigned long long)stats.memory_peak,
		    (unsigned long long)stats.scratch_peak,
		    (unsigned long long)stats.descriptors_peak,
		    (unsigned long long)stats.io_calls,
		    (unsigned long long)stats.scratch_physical_peak, elapsed, usage.ru_maxrss);
	}
	fixture_close(&f);
}

static void
make_payload(const char *path, size_t length)
{
	unsigned char buffer[4096];
	size_t batch, done;
	int fd;

	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	CHECK(fd != -1);
	for (done = 0; done < length; done += batch) {
		batch = length - done > sizeof(buffer) ? sizeof(buffer) : length - done;
		for (size_t i = 0; i < batch; i++)
			buffer[i] = (unsigned char)((done + i) * 37);
		CHECK(write(fd, buffer, batch) == (ssize_t)batch);
	}
	CHECK(close(fd) == 0);
}

static void
test_file_limit(void)
{
	fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_verify_report_t report;
	unsigned char raw[32], bytes[4], byte;
	size_t count;

	/* The maximum FAT file has 2^32-1 logical bytes. Only two boundary
	 * markers are transferred; its contiguous allocation and large offsets
	 * are real sparse-file geometry, not a full-payload throughput test.
	 */
	fixture_geometry(&f, 32, 4096, 128);
	set_cluster(&f, 3, 0);
	for (uint32_t i = 0; i < 8192; i++)
		set_cluster(&f, 20 + i, i == 8191 ? f.eoc : 21 + i);
	short_entry(raw, "MAXFILE BIN", false, 20, UINT32_MAX);
	write_bytes(&f, f.root_offset, raw, sizeof(raw));
	write_bytes(&f, cluster_offset(&f, 20), "A", 1);
	write_bytes(&f, cluster_offset(&f, 8211) + f.cluster_bytes - 2, "B", 1);
	put32(bytes, f.clusters - 8192 - 2);
	write_bytes(&f, f.sector_bytes + 488, bytes, sizeof(bytes));
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(entry->size == UINT32_MAX);
	/* A first read at the far end is a seek, so it indexes the maximum-size
	 * chain; the start is then found through that index.
	 */
	CHECK(fsimg_reader_read_at(f.reader, (uint64_t)UINT32_MAX - 1, &byte, 1, &count, NULL) == FSIMG_OK);
	CHECK(count == 1 && byte == 'B' && fsimg_test_extent_count(fsimg_fat_state(f.reader)->index) == 1);
	CHECK(fsimg_reader_read_at(f.reader, 0, &byte, 1, &count, NULL) == FSIMG_OK);
	CHECK(count == 1 && byte == 'A');
	CHECK(fsimg_reader_read_at(f.reader, UINT32_MAX, &byte, 1, &count, NULL) == FSIMG_OK);
	CHECK(count == 0);
	CHECK(fsimg_reader_read_at(f.reader, (uint64_t)UINT32_MAX + 1, &byte, 1, &count, NULL) == FSIMG_EINVAL);
	fsimg_reader_free(f.reader);
	f.reader = NULL;
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_verify(f.reader, 0, &report, NULL) == FSIMG_OK);
	CHECK(report.logical_bytes == UINT32_MAX && report.payload_bytes_read == 0);
	fixture_close(&f);
}

static void
test_empty(void)
{
	static const unsigned variants[] = { 12, 16, 32 };
	fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_verify_report_t report;
	unsigned char zeroes[512], bytes[4];

	bzero(zeroes, sizeof(zeroes));
	for (unsigned v = 0; v < 3; v++) {
		fixture_init(&f, variants[v]);
		write_bytes(&f, f.root_offset, zeroes, sizeof(zeroes));
		set_cluster(&f, 3, 0);
		set_cluster(&f, 5, 0);
		set_cluster(&f, 7, 0);
		set_cluster(&f, 9, 0);
		if (variants[v] == 32) {
			put32(bytes, f.clusters - 1);
			write_bytes(&f, 512 + 488, bytes, sizeof(bytes));
		}
		fixture_open(&f);
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
		CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_DONE && entry == NULL);
		CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_DONE && entry == NULL);
		fsimg_reader_free(f.reader);
		f.reader = NULL;
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
		CHECK(fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD, &report, NULL) == FSIMG_OK);
		CHECK(report.complete && report.files == 0 && report.directories == 1);
		CHECK(report.payload_bytes_read == 0);
		fixture_close(&f);
	}
}

static bool
cancelled(void *argument)
{
	fixture_t *f = argument;

	return (f->cancelled ||
	    (f->cancel_after_call != 0 && f->calls >= f->cancel_after_call));
}

static void
test_edge_cases(void)
{
	static const char *const cases[] = {
		"shared directory", "ancestor directory", "tail cycle", "wrong dot",
		"wrong dotdot", "unvisited mirror", "premature EOF", "short slice",
		"FSInfo signature", "FSInfo free count", "FSInfo next hint",
		"backup BPB", "dirty volume", "FAT32 revision", "root cluster",
		"active FAT index", "reserved FAT flag bit4", "reserved FAT flags"
	};
	fixture_t f;
	fsimg_verify_report_t report;
	fsimg_error_t error;
	unsigned char raw[96], bytes[4];
	fsimg_status_t status, expected;

	for (unsigned scenario = 0; scenario < sizeof(cases) / sizeof(cases[0]); scenario++) {
		fixture_init(&f, scenario < 8 ? 16 : 32);
		expected = scenario == 6 ? FSIMG_EIO :
		    scenario == 13 ? FSIMG_EUNSUPPORTED : FSIMG_ECORRUPT;
		CHECK(pread(f.fd, raw, sizeof(raw), (off_t)f.root_offset) == sizeof(raw));
		switch (scenario) {
		case 0:
			short_entry(raw + 64, "OTHER      ", true, 7, 0);
			write_bytes(&f, f.root_offset, raw, sizeof(raw));
			break;
		case 1:
			short_entry(raw, "BACK       ", true, 7, 0);
			write_bytes(&f, cluster_offset(&f, 7) + 64, raw, 32);
			break;
		case 2:
			set_cluster(&f, 7, 8);
			set_cluster(&f, 8, 7);
			break;
		case 3:
		case 4:
			put16(bytes, 9);
			write_bytes(&f, cluster_offset(&f, 7) + (scenario == 3 ? 26 : 58), bytes, 2);
			break;
		case 5:
			put16(bytes, 0xffff);
			write_bytes(&f, f.fat_offset + f.table_bytes + 99 * 2, bytes, 2);
			break;
		case 6:
			CHECK(ftruncate(f.fd, (off_t)f.root_offset + 8) == 0);
			break;
		case 7:
			f.bytes--;
			break;
		case 8:
			bytes[0] = 0;
			write_bytes(&f, 512, bytes, 1);
			break;
		case 9:
		case 10:
			put32(bytes, scenario == 9 ? 0 : 1);
			write_bytes(&f, 512 + (scenario == 9 ? 488 : 492), bytes, 4);
			break;
		case 11:
			bytes[0] = 3;
			write_bytes(&f, 6 * 512 + 13, bytes, 1);
			break;
		case 12:
			set_cluster(&f, 1, 0x07ffffff);
			break;
		case 13:
			bytes[0] = 1;
			write_bytes(&f, 42, bytes, 1);
			break;
		case 14:
			put32(bytes, 0);
			write_bytes(&f, 44, bytes, 4);
			break;
		case 15:
		case 16:
		case 17:
			put16(bytes, scenario == 15 ? 0x82 : scenario == 16 ? 0x10 : 0x100);
			write_bytes(&f, 40, bytes, 2);
			break;
		}
		fixture_open(&f);
		status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, &error);
		if (status == FSIMG_OK) {
			status = fsimg_verify(f.reader, 0, &report, &error);
			CHECK(!report.complete);
		}
		if (status != expected)
			fprintf(stderr, "fixture %s: %s (%s)\n", cases[scenario],
			    fsimg_status_string(status), error.message);
		CHECK(status == expected);
		fixture_close(&f);
	}
}

static void
test_mirror_selector(void)
{
	fixture_t f;
	fsimg_verify_report_t report;
	fsimg_error_t error;
	const fsimg_entry_t *entry;
	unsigned char bytes[4];
	fsimg_status_t status;

	for (unsigned scenario = 0; scenario < 3; scenario++) {
		fixture_init(&f, 32);
		put16(bytes, 0x000f);
		write_bytes(&f, 40, bytes, 2);
		if (scenario != 2)
			write_bytes(&f, 6 * 512 + 40, bytes, 2);
		if (scenario == 1) {
			put32(bytes, f.eoc);
			write_bytes(&f, f.fat_offset + f.table_bytes + 99 * 4, bytes, 4);
		}

		/* Bit7 enables selector interpretation only in nonmirrored mode.
		 * Mirrored mode ignores even an out-of-range nibble, compares both
		 * FATs, and keeps backup-copy consistency as a separate verify check.
		 */
		fixture_open(&f);
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
		CHECK(fsimg_fat_state(f.reader)->geometry.mirrored && fsimg_fat_state(f.reader)->geometry.active_table == 0);
		CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
		fsimg_reader_free(f.reader);
		f.reader = NULL;
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
		status = fsimg_verify(f.reader, 0, &report, &error);
		CHECK(status == (scenario == 0 ? FSIMG_OK : FSIMG_ECORRUPT));
		if (scenario == 2)
			CHECK(!report.complete && strstr(error.message, "backup") != NULL);
		fixture_close(&f);
	}
}

static void
test_index_failures(void)
{
	fixture_t f;
	const fsimg_entry_t *entry;
	unsigned char byte;
	fsimg_status_t status;
	size_t count;
	bool complete;

	complete = false;
	for (uint64_t attempt = 1; attempt < 16; attempt++) {
		fixture_init(&f, 16);
		fixture_open(&f);
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
		CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
		fsimg_test_fail_alloc_after(f.ctx, attempt);
		status = fsimg_reader_read_at(f.reader, 1099, &byte, 1, &count, NULL);
		CHECK(status == FSIMG_OK || status == FSIMG_ENOMEM);
		complete = status == FSIMG_OK;
		fsimg_test_fail_alloc_after(f.ctx, 0);
		fixture_close(&f);
		if (complete)
			break;
	}
	CHECK(complete);

	/* A thousand one-cluster fragments force the current-file index through
	 * the one-page cache into scratch. Fail each backing operation separately.
	 */
	for (unsigned fault = 0; fault <= FSIMG_META_WRITE; fault++) {
		fixture_init(&f, 16);
		make_fragmented_file(&f, 1000);
		fixture_open(&f);
		CHECK(fsimg_test_meta_cache(f.ctx, 1, NULL) == FSIMG_OK);
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
		CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
		if (fault != 0)
			fsimg_test_meta_fail_after(f.ctx, (fsimg_meta_fault_t)fault, 1, EIO);
		status = fsimg_reader_read_at(f.reader, 1000 * 512 - 1, &byte, 1, &count, NULL);
		if (fault == 0) {
			uint64_t work = f.reader->session.progress.work;

			CHECK(status == FSIMG_OK && fsimg_test_extent_count(fsimg_fat_state(f.reader)->index) == 1000);
			for (unsigned i = 0; i < 100; i++) {
				CHECK(fsimg_reader_read_at(f.reader, (i * 619u % 1000) * 512,
				    &byte, 1, &count, NULL) == FSIMG_OK);
				CHECK(count == 1 && byte == 0);
			}
			CHECK(f.reader->session.progress.work - work < 10000);
		} else {
			CHECK(status == FSIMG_EIO);
			fsimg_test_meta_fail_after(f.ctx, (fsimg_meta_fault_t)fault, 0, EIO);
		}
		fixture_close(&f);
	}
}

static void
test_cancellation(void)
{
	fixture_t f;
	const fsimg_entry_t *entry;
	unsigned char byte;
	size_t count;

	for (unsigned scenario = 0; scenario < 2; scenario++) {
		fixture_init(&f, 16);
		fixture_open(&f);
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
		if (scenario == 0) {
			f.cancel_after_call = f.calls + 1;
			CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_ECANCELLED);
			CHECK(entry == NULL);
		} else {
			CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
			f.cancelled = true;
			CHECK(fsimg_reader_read_at(f.reader, 0, &byte, 1, &count, NULL) == FSIMG_ECANCELLED);
			CHECK(count == 0);
		}
		f.cancelled = false;
		f.cancel_after_call = 0;
		fixture_close(&f);
	}
}

static void
test_slices(void)
{
	fixture_t f;
	fsimg_io_t *slice, *nested, *short_slice;
	fsimg_reader_t *other;
	const fsimg_entry_t *entry;
	fsimg_verify_report_t report;
	unsigned char bytes[4096];
	uint64_t old_length, position;
	size_t batch, count;

	fixture_init(&f, 16);
	old_length = f.bytes;
	f.bytes += 256;
	CHECK(ftruncate(f.fd, (off_t)f.bytes) == 0);
	/* Move backwards to install an image at a nonzero byte base without
	 * staging another payload image. Sentinel bytes stay outside both ends.
	 */
	position = old_length;
	while (position != 0) {
		batch = position > sizeof(bytes) ? sizeof(bytes) : (size_t)position;
		position -= batch;
		CHECK(pread(f.fd, bytes, batch, (off_t)position) == (ssize_t)batch);
		write_bytes(&f, position + 128, bytes, batch);
	}
	memset(bytes, 0xa5, 128);
	write_bytes(&f, 0, bytes, 128);
	write_bytes(&f, old_length + 128, bytes, 128);
	fixture_open(&f);
	CHECK(fsimg_io_slice(f.io, 128, old_length, &slice, NULL) == FSIMG_OK);
	CHECK(fsimg_io_slice(slice, 0, old_length, &nested, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_open(f.ctx, nested, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD, &report, NULL) == FSIMG_OK);
	fsimg_reader_free(f.reader);
	f.reader = NULL;

	CHECK(fsimg_io_slice(slice, 0, old_length - 1, &short_slice, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_open(f.ctx, short_slice, NULL, &other, NULL) == FSIMG_ECORRUPT);
	CHECK(other == NULL);
	fsimg_io_free(short_slice);
	CHECK(fsimg_reader_open(f.ctx, nested, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_open(f.ctx, nested, NULL, &other, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(other, &entry, NULL) == FSIMG_OK);
	fsimg_reader_free(f.reader);
	f.reader = NULL;
	CHECK(fsimg_reader_read_at(other, 0, bytes, 1, &count, NULL) == FSIMG_OK);
	CHECK(count == 1 && bytes[0] == 0);
	fsimg_reader_free(other);
	fsimg_io_free(nested);
	fsimg_io_free(slice);
	fixture_close(&f);
}

static size_t
long_records(unsigned char *records, const uint16_t *name, size_t length,
    const unsigned char *short_record)
{
	static const unsigned positions[] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
	unsigned char checksum, *raw;
	size_t slots, ordinal, index;

	checksum = 0;
	for (unsigned i = 0; i < 11; i++)
		checksum = (unsigned char)((checksum / 2) + (checksum % 2) * 128 + short_record[i]);
	slots = (length + 12) / 13;
	for (size_t i = 0; i < slots; i++) {
		ordinal = slots - i;
		raw = records + i * 32;
		bzero(raw, 32);
		raw[0] = (unsigned char)(ordinal | (i == 0 ? 0x40 : 0));
		raw[11] = 0x0f;
		raw[13] = checksum;
		for (unsigned j = 0; j < 13; j++) {
			index = (ordinal - 1) * 13 + j;
			put16(raw + positions[j], index < length ? name[index] :
			    index == length ? 0 : 0xffff);
		}
	}
	bcopy(short_record, records + slots * 32, 32);
	return ((slots + 1) * 32);
}

static void
test_volume_labels(void)
{
	const unsigned variants[] = { 12, 16, 32 };
	fixture_t f;
	const fsimg_entry_t *entry;
	unsigned char root[512];
	fsimg_status_t status;

	for (unsigned variant = 0; variant < 3; variant++) {
		for (unsigned scenario = 0; scenario < 9; scenario++) {
			fixture_init(&f, variants[variant]);
			CHECK(pread(f.fd, root, sizeof(root), (off_t)f.root_offset) == sizeof(root));
			bcopy(root, root + 32, 64);
			bzero(root, 32);
			bcopy("FSIMG TEST ", root, 11);
			root[11] = scenario == 1 ? 0x28 : 0x08;
			if (scenario == 2)
				root[11] |= 0x10;
			else if (scenario == 3)
				bcopy(root, root + 32, 32);
			else if (scenario == 4)
				put16(root + 26, 2);
			else if (scenario == 5)
				put32(root + 28, 1);
			else if (scenario == 6)
				put16(root + 20, 1);
			else if (scenario == 7)
				root[11] |= 0x40;
			else if (scenario == 8) {
				/* A label in a subdirectory is invalid even without a root label. */
				write_bytes(&f, cluster_offset(&f, 7) + 64, root, 32);
				bcopy(root + 32, root, 64);
				bzero(root + 64, sizeof(root) - 64);
			}
			write_bytes(&f, f.root_offset, root, sizeof(root));
			fixture_open(&f);
			CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
			status = fsimg_reader_next(f.reader, &entry, NULL);
			if (scenario < 2) {
				if (status != FSIMG_OK)
					fprintf(stderr, "FAT%u label attributes 0x%02x: expected entry, got %d\n",
					    variants[variant], root[11], status);
				CHECK(status == FSIMG_OK && strcmp(entry->path, "/HELLO.TXT") == 0);
				CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
				CHECK(strcmp(entry->path, "/SUB") == 0);
				CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
				CHECK(strcmp(entry->path, "/SUB/EMPTY") == 0);
				CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_DONE);
			} else {
				while (status == FSIMG_OK)
					status = fsimg_reader_next(f.reader, &entry, NULL);
				CHECK(status == FSIMG_ECORRUPT && entry == NULL);
			}
			fixture_close(&f);
		}
	}
}

static void
test_long_names(void)
{
	fixture_t f;
	const fsimg_entry_t *entry;
	unsigned char records[1024], raw[32];
	uint16_t name[255];
	size_t length;
	fsimg_status_t status;

	for (unsigned scenario = 0; scenario < 8; scenario++) {
		fixture_init(&f, 16);
		bzero(records, sizeof(records));
		short_entry(raw, "LONGNA~1TXT", false, 0, 0);
		for (unsigned i = 0; i < 255; i++)
			name[i] = 'a';
		length = long_records(records, name, scenario == 0 ? 255 : 20, raw);
		if (scenario == 1) {
			/* The alias of a later entry collides with this long name. */
			for (unsigned i = 0; i < 9; i++)
				name[i] = (uint16_t)"HELLO.TXT"[i];
			length = long_records(records, name, 9, raw);
			short_entry(records + length, "HELLO   TXT", false, 0, 0);
		} else if (scenario == 2) {
			records[13]++;
		} else if (scenario == 3) {
			records[32] = 2;
		} else if (scenario == 4) {
			put16(records + 30, 0); /* Padding after the NUL must be 0xffff. */
		} else if (scenario == 5) {
			records[12] = 1;
		} else if (scenario == 7) {
			name[0] = ' ';
			length = long_records(records, name, 20, raw);
		} else if (scenario == 6) {
			/* Live slots without their associated short record are corrupt. */
			bzero(records + length - 32, 32);
		}
		write_bytes(&f, f.root_offset, records, sizeof(records));
		fixture_open(&f);
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
		status = fsimg_reader_next(f.reader, &entry, NULL);
		if (scenario == 0) {
			CHECK(status == FSIMG_OK && strlen(entry->path) == 256);
			CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_DONE);
		} else {
			CHECK(status == FSIMG_ECORRUPT && entry == NULL);
		}
		fixture_close(&f);
	}
}

static void
test_cached_name(void)
{
	fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_stream_t *stream;
	unsigned char invalid[2] = { 0xc0, 0x80 };
	uint64_t cursor;

	fixture_init(&f, 16);
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	stream = fsimg_fat_state(f.reader)->directories[0].entries;
	cursor = fsimg_fat_state(f.reader)->directories[0].cursor;
	CHECK(fsimg_store_write(stream->store, cursor + 8 + 32,
	    invalid, sizeof(invalid), NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_ECORRUPT);
	CHECK(entry == NULL);
	fixture_close(&f);
}

/* Replace the first root entry with INDEX.BIN, a file of the given number
 * of one-cluster fragments on every other cluster from 20. The payload
 * stays zero; callers that need content write it themselves.
 */
static void
make_fragmented_file(fixture_t *f, uint32_t fragments)
{
	unsigned char raw[32];

	/* Each entry links to the cluster two ahead, leaving the odd clusters
	 * between them free, so no two clusters of the chain are adjacent. The
	 * caller's variant must have more than 20 + 2 * fragments clusters.
	 */
	for (uint32_t i = 0; i < fragments; i++)
		set_cluster(f, 20 + i * 2, i + 1 == fragments ? f->eoc : 22 + i * 2);
	short_entry(raw, "INDEX   BIN", false, 20, fragments * 512);
	write_bytes(f, f->root_offset, raw, sizeof(raw));
}

/* Comparison-baseline record (see baseline.h): the thousand-fragment file
 * under the common access script with a one-page metadata cache. Each
 * cluster carries a position pattern led by its logical ordinal, so the
 * content digest detects misordered or misplaced reads.
 */
static void
baseline_fragmented(void)
{
	fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_stats_t stats;
	sha256_t digest;
	unsigned char data[512];
	char hex[SHA256_HEX_BYTES];
	const uint32_t fragments = 1000;

	fixture_init(&f, 16);
	make_fragmented_file(&f, fragments);
	/* The builder leaves payload zero; write the position pattern into the
	 * chain's clusters in logical order.
	 */
	for (uint32_t i = 0; i < fragments; i++) {
		for (unsigned j = 0; j < sizeof(data); j++)
			data[j] = (unsigned char)((i * 512 + j) * 37);
		put32(data, i);
		write_bytes(&f, cluster_offset(&f, 20 + i * 2), data, sizeof(data));
	}
	fixture_open(&f);
	CHECK(fsimg_test_meta_cache(f.ctx, 1, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(strcmp(entry->path, "/INDEX.BIN") == 0);
	sha256_init(&digest);
	baseline_read_script(f.reader, entry->size, &digest);
	sha256_final_hex(&digest, hex);
	/* Sampled while INDEX.BIN is still current, before teardown, so
	 * memory_bytes is what the reader keeps for the file.
	 */
	CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
	printf("fixture=fat-fragmented content_sha256=%s read_io_calls=%" PRIu64
	    " read_work=%" PRIu64 " read_scratch_peak=%" PRIu64
	    " read_memory_retained=%" PRIu64 " read_descriptors_peak=%" PRIu64
	    "\n", hex, stats.io_calls, f.reader->session.progress.work,
	    stats.scratch_peak, stats.memory_bytes, stats.descriptors_peak);
	fixture_close(&f);
}

static void
test_cached_index(void)
{
	fixture_t f;
	const fsimg_entry_t *entry;
	unsigned char record[8], byte;
	size_t count;

	fixture_init(&f, 16);
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_read_at(f.reader, 1024, &byte, 1, &count, NULL) == FSIMG_OK);
	CHECK(fsimg_test_extent_count(fsimg_fat_state(f.reader)->index) == 3);
	fsimg_meta_put_u64(record, 1);
	CHECK(fsimg_store_write(fsimg_test_extent_records(fsimg_fat_state(f.reader)->index),
	    0, record, sizeof(record), NULL) == FSIMG_OK);
	CHECK(fsimg_reader_read_at(f.reader, 1, &byte, 1, &count, NULL) == FSIMG_ECORRUPT);
	CHECK(count == 0);
	fixture_close(&f);
}

static void
test_report_outputs(void)
{
	fixture_t f;
	fsimg_verify_report_t report;

	fixture_init(&f, 16);
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_verify(f.reader, 0, &report, NULL) == FSIMG_OK && report.complete);
	CHECK(fsimg_verify(f.reader, 0, &report, NULL) == FSIMG_ESTATE);
	CHECK(!report.complete);
	report.complete = true;
	CHECK(fsimg_verify(f.reader, 0x80, &report, NULL) == FSIMG_EINVAL);
	CHECK(!report.complete);
	report.complete = true;
	CHECK(fsimg_verify(NULL, 0, &report, NULL) == FSIMG_EINVAL);
	CHECK(!report.complete);
	fixture_close(&f);
}

static void
test_capability_bounds(void)
{
	fixture_t f;
	fsimg_format_info_t format;
	fat_geometry_t geometry;
	unsigned char boot[512];
	uint64_t sectors;
	size_t cursor;

	fixture_init(&f, 16);
	CHECK(pread(f.fd, boot, sizeof(boot), 0) == sizeof(boot));
	put16(boot + 11, 4096);
	boot[13] = 128;
	put16(boot + 14, 65535);
	put16(boot + 17, 65535);
	put16(boot + 19, 0);
	put16(boot + 22, 65535);
	/* Maximize all BPB regions and the partial-cluster tail independently.
	 * This is a logical geometry boundary, not a multi-gigabyte transfer.
	 */
	for (unsigned variant = 0; variant < 2; variant++) {
		uint64_t clusters = variant == 0 ? 4084 : 65524;
		sectors = 65535 + 2 * UINT64_C(65535) + 512 + (clusters + 1) * 128 - 1;
		put32(boot + 32, (uint32_t)sectors);
		CHECK(fsimg_fat_geometry_decode(boot, sectors * 4096, &geometry, NULL) == FSIMG_OK);
		cursor = variant;
		CHECK(fsimg_format_next(&cursor, &format, NULL) == FSIMG_OK);
		CHECK(format.max_volume_bytes >= geometry.volume_bytes);
		if (variant == 0)
			CHECK(format.max_file_bytes == UINT64_C(4084) * 512 * 1024);
	}
	fixture_close(&f);
}

static void
write_seed(const char *path, unsigned variant)
{
	fixture_t f;
	unsigned char buffer[4096];
	uint64_t done, seed_bytes;
	size_t batch;
	int fd;

	fixture_init(&f, variant);
	seed_bytes = variant == 32 ? f.data_offset + 16 * f.cluster_bytes : f.bytes;
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	CHECK(fd != -1);
	for (done = 0; done < seed_bytes; done += batch) {
		batch = seed_bytes - done > sizeof(buffer) ? sizeof(buffer) : (size_t)(seed_bytes - done);
		CHECK(pread(f.fd, buffer, batch, (off_t)done) == (ssize_t)batch);
		CHECK(write(fd, buffer, batch) == (ssize_t)batch);
	}
	CHECK(close(fd) == 0);
	fixture_close(&f);
}

static void
test_external(const char *path, const char *format)
{
	static const char payload[] = "Independent FAT fixture.\n";
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	fsimg_reader_t *reader;
	fsimg_options_t options;
	fsimg_volume_info_t info;
	fsimg_verify_report_t report;
	fsimg_error_t error;
	const fsimg_entry_t *entry;
	unsigned char buffer[4096];
	uint64_t offset;
	size_t count;
	unsigned files, directories;
	int fd, scratch;
	bool fragment, saw_unicode;
	fsimg_status_t status;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	scratch = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fd != -1 && scratch != -1);
	fsimg_options_init(&options);
	options.scratch_directory_fd = scratch;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_io_from_fd(ctx, fd, FSIMG_IO_READ, &io, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_open(ctx, io, NULL, &reader, &error) == FSIMG_OK);
	CHECK(fsimg_reader_get_info(reader, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.format, format) == 0);
	files = directories = 0;
	saw_unicode = false;
	while ((status = fsimg_reader_next(reader, &entry, &error)) == FSIMG_OK) {
		if (entry->kind == FSIMG_ENTRY_DIRECTORY) {
			directories++;
			if (strcmp(format, "fat32") == 0 && strcmp(entry->path, "/Nested") == 0) {
				unsigned char dotdot[32];
				uint32_t parent_cluster;
				uint64_t address;

				address = fsimg_fat_cluster_offset(&fsimg_fat_state(reader)->geometry,
				    fsimg_fat_state(reader)->current_cluster) + 32;
				CHECK(fsimg_io_read_exact(io, address, dotdot, sizeof(dotdot), &count, NULL) == FSIMG_OK);
				CHECK(bcmp(dotdot, "..         ", 11) == 0);
				parent_cluster = fsimg_fat_u16(dotdot + 26) |
				    (uint32_t)fsimg_fat_u16(dotdot + 20) << 16;
				printf("FAT32 external root-parent dotdot cluster=%u\n", parent_cluster);
				CHECK(parent_cluster == 0);
			}
			continue;
		}
		files++;
		if (strstr(entry->path, "R\xc3\xa9sum\xc3\xa9") != NULL)
			saw_unicode = true;
		fragment = strcmp(entry->path, "/FRAG.BIN") == 0;
		if (!fragment)
			CHECK(entry->size == sizeof(payload) - 1);
		offset = 0;
		while (offset < entry->size) {
			CHECK(fsimg_reader_read_at(reader, offset, buffer, sizeof(buffer), &count, &error) == FSIMG_OK);
			CHECK(count > 0);
			if (fragment) {
				for (size_t i = 0; i < count; i++)
					CHECK(buffer[i] == (unsigned char)((offset + i) * 37));
			} else {
				CHECK(count == sizeof(payload) - 1 && bcmp(buffer, payload, count) == 0);
			}
			offset += count;
		}
		if (fragment) {
			CHECK(fsimg_reader_read_at(reader, 0, buffer, 1, &count, NULL) == FSIMG_OK);
			CHECK(fsimg_test_extent_count(fsimg_fat_state(reader)->index) > 1);
		}
	}
	if (status != FSIMG_DONE)
		fprintf(stderr, "external read: %s (%s)\n", fsimg_status_string(status), error.message);
	CHECK(status == FSIMG_DONE && files >= 3 && directories == 1 && saw_unicode);
	fsimg_reader_free(reader);
	CHECK(fsimg_reader_open(ctx, io, NULL, &reader, NULL) == FSIMG_OK);
	status = fsimg_verify(reader, FSIMG_VERIFY_PAYLOAD, &report, &error);
	if (status != FSIMG_OK)
		fprintf(stderr, "external verify: %s (%s)\n", fsimg_status_string(status), error.message);
	CHECK(status == FSIMG_OK && report.complete && report.files == files);
	fsimg_reader_free(reader);
	fsimg_io_free(io);
	fsimg_ctx_free(ctx);
	CHECK(close(fd) == 0 && close(scratch) == 0);
}

/* Extraction asks for a range and then reads it. A range query must not move
 * the sequential cursor past the bytes it describes, or the following read
 * looks like a seek and rebuilds the whole chain into a scratch index.
 */
static void
test_range_reads(void)
{
	fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_range_t range;
	unsigned char data[1100];
	uint64_t offset;
	size_t count;

	/* HELLO.TXT spans three nonadjacent cluster runs. */
	fixture_init(&f, 16);
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(strcmp(entry->path, "/HELLO.TXT") == 0 && entry->size == sizeof(data));
	offset = 0;
	while (fsimg_reader_next_range(f.reader, offset, &range, NULL) == FSIMG_OK) {
		CHECK(range.offset == offset && range.kind == FSIMG_RANGE_DATA);
		CHECK(range.length <= sizeof(data) - offset);
		CHECK(fsimg_reader_read_at(f.reader, offset, data + offset,
		    (size_t)range.length, &count, NULL) == FSIMG_OK);
		CHECK(count == range.length);
		offset += count;
	}
	CHECK(offset == sizeof(data));
	for (unsigned j = 0; j < sizeof(data); j++)
		CHECK(data[j] == (unsigned char)(j * 37));
	CHECK(fsimg_fat_state(f.reader)->index == NULL);
	fsimg_reader_free(f.reader);
	f.reader = NULL;
	fixture_close(&f);
}

int
main(int argc, char **argv)
{
	/* Measurement records only on stdout; test/baseline.sh parses them. */
	if (argc == 2 && strcmp(argv[1], "--baseline") == 0) {
		baseline_fragmented();
		return (0);
	}
	if (argc == 4 && strcmp(argv[1], "--seed-variant") == 0) {
		unsigned variant = (unsigned)strtoul(argv[3], NULL, 10);

		CHECK(variant == 12 || variant == 16 || variant == 32);
		write_seed(argv[2], variant);
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--mirror-selector") == 0) {
		test_mirror_selector();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--cached-index") == 0) {
		test_cached_index();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--detection") == 0) {
		test_detection();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--cached-name") == 0) {
		test_cached_name();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--volume-labels") == 0) {
		test_volume_labels();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--long-names") == 0) {
		test_long_names();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--scale") == 0) {
		test_wide(65520);
		return (0);
	}
	if (argc == 3 && strcmp(argv[1], "--max-cluster") == 0) {
		CHECK(strcmp(argv[2], "12") == 0 || strcmp(argv[2], "16") == 0 ||
		    strcmp(argv[2], "32") == 0);
		test_cluster_ceiling((unsigned)strtoul(argv[2], NULL, 10));
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--geometry-matrix") == 0) {
		test_geometry_matrix();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--host-change") == 0) {
		test_host_change();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--reports") == 0) {
		test_report_outputs();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--capabilities") == 0) {
		test_capability_bounds();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--names") == 0) {
		test_names();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--corruption") == 0) {
		test_corruption();
		return (0);
	}
	if (argc == 4 && strcmp(argv[1], "--external") == 0) {
		test_external(argv[2], argv[3]);
		return (0);
	}
	if (argc == 4 && strcmp(argv[1], "--make-payload") == 0) {
		make_payload(argv[2], (size_t)strtoul(argv[3], NULL, 10));
		return (0);
	}
	if (argc == 3 && strcmp(argv[1], "--seed") == 0) {
		write_seed(argv[2], 12);
		return (0);
	}
	CHECK(argc == 1);
	test_variants();
	test_geometry_matrix();
	test_cluster_ceiling(12);
	test_cluster_ceiling(16);
	test_cluster_ceiling(32);
	test_names();
	test_geometry();
	test_thresholds();
	test_deep_tree();
	test_detection();
	test_corruption();
	test_limits();
	test_failures();
	test_partial_content();
	test_host_change();
	test_wide(400);
	test_file_limit();
	test_empty();
	test_edge_cases();
	test_mirror_selector();
	test_index_failures();
	test_cancellation();
	test_slices();
	test_long_names();
	test_volume_labels();
	test_cached_name();
	test_cached_index();
	test_range_reads();
	test_report_outputs();
	test_capability_bounds();
	return (0);
}
