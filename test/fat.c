/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"
#include "force-short.h"

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
#include "fsimg-endian.h"
#include "test.h"

#define BPB_TOTAL_SECTORS16 19
#define BPB_TOTAL_SECTORS32 32
#define BPB_FAT_SECTORS32 36

/* This fixture builder encodes disk fields directly, independently of the
 * production geometry/name/chain helpers. Real formatter fixtures are checked
 * separately by fat-integration.sh. Backing is a sparse file or a memory prefix.
 */
typedef struct fixture {
	unsigned char	*memory;
	size_t		 stored;
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
	unsigned	 tables;
	int		 fd;
	int		 scratch;
	char		 directory[64];
} fixture_t;

typedef struct misplaced_dot_case {
	const char	*name;
	const char	*reason;
	unsigned	 dot_slot;
	unsigned	 dotdot_slot;
	unsigned	 empty_slot;
	bool		 root;
} misplaced_dot_case_t;

typedef struct orphan_case {
	const char	*name;
	const char	*dot_name;
	uint32_t	 dot_cluster;
	bool		 deleted_after_dot;
} orphan_case_t;

typedef struct boot_label_case {
	const char	*name;
	const char	*boot_label;
	const char	*root_label;
	fsimg_status_t	 status;
	unsigned char	 signature;
} boot_label_case_t;

static void put16(unsigned char *, uint16_t);
static void put32(unsigned char *, uint32_t);
static void write_bytes(fixture_t *, uint64_t, const void *, size_t);
static void set_cluster(fixture_t *, uint32_t, uint32_t);
static uint64_t cluster_offset(const fixture_t *, uint32_t);
static void short_entry(unsigned char *, const char *, bool, uint32_t, uint32_t);
static void fixture_init(fixture_t *, unsigned);
static void fixture_geometry(fixture_t *, unsigned, unsigned, unsigned);
static void fixture_layout(fixture_t *, unsigned, unsigned, unsigned, unsigned);
static void fixture_build(fixture_t *, unsigned, unsigned, unsigned, unsigned,
    bool, uint32_t, uint32_t);
static void fixture_add_root_label(fixture_t *, const char *);
static void test_truncated_directory_partial_cluster(void);
static void test_geometry_matrix(void);
static void test_cluster_ceiling(unsigned);
static void fixture_open(fixture_t *);
static void fixture_close(fixture_t *);
static fsimg_status_t image_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static fsimg_status_t image_size(void *, uint64_t *, fsimg_error_t *);
static void test_variants(void);
static void check_fixture_listing(fixture_t *, const char *);
static void test_dot_records_ntres_ignored(unsigned);
static void test_dot_record_attribute_bits(unsigned);
static void test_directory_without_dot_records(unsigned);
static void test_dot_record_clusters_ignored_when_listing(unsigned);
static void test_misplaced_dot_records_skipped_when_listing(unsigned);
static void test_long_name_before_misplaced_dot_record_rejected(unsigned);
static void test_names(void);
static void test_geometry(void);
static void test_thresholds(void);
static void test_deep_tree(void);
static void test_detection(void);
static void test_tot_sec16_overrides_boot_code(void);
static void test_assumed_atari_identity_reads_fragmented_file(void);
static void test_assumed_identity_still_validates_geometry(void);
static void test_assumed_identity_still_rejects_broken_chain(void);
static void test_signed_volume_with_assume_identity_verifies(void);
static void test_assumed_fat16_media_entry_is_reported_by_verify(void);
static void test_assumed_reserved_entry_has_no_status_flags(void);
static void test_assumed_identity_still_compares_reserved_entry_mirrors(void);
static void test_assume_identity_requires_named_format_and_known_flags(void);
static void test_non_pc_boot_records(void);
static void test_unsigned_tiny_fat_open(void);
static void test_pc98_boot_record(void);
static void test_unsigned_media_mismatch_is_not_fat(void);
static void test_identified_unsigned_missing_tail_opens(void);
static void test_unsigned_fat16_head_does_not_check_fourth_byte(void);
static void test_unsigned_fat16_and_fat32_identification(void);
static void test_forced_unsigned_fat12_variant(void);
static void test_geometry_decode_short_boot_record(void);
static void test_boot_voucher_is_the_signature_or_the_media_echo(void);
static void test_unvouched_oversized_regions_are_not_fat(void);
static void test_media_check_precedes_mirror_comparison(void);
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
static void test_boot_label_without_root_label(void);
static void test_cached_name(void);
static void test_cached_index(void);
static void test_range_reads(void);
static void test_trailing_duplicate_claim(void);
static void test_cross_linked_files_name_shared_clusters(void);
static void test_fragmented_verify(void);
static void make_fragmented_file(fixture_t *, uint32_t);
static void baseline_fragmented(void);
static bool cancelled(void *);
static size_t long_records(unsigned char *, const uint16_t *, size_t,
    const unsigned char *);
static void test_report_outputs(void);
static void test_capability_bounds(void);
static void test_external(const char *, const char *, bool);
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
	if (f->memory != NULL) {
		CHECK(offset <= f->stored && length <= f->stored - offset);
		bcopy(bytes, f->memory + offset, length);
	} else
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
	for (unsigned copy = 0; copy < f->tables; copy++) {
		uint64_t address = f->fat_offset + copy * f->table_bytes + offset;
		if (f->variant == 12) {
			if (f->memory != NULL)
				bcopy(f->memory + address, bytes, 2);
			else
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

	CHECK(offset <= f->bytes && length <= f->bytes - offset);
	f->calls++;
	if (offset >= f->data_offset && offset != f->root_offset &&
	    offset != cluster_offset(f, 7))
		f->payload_calls++;
	fail = f->fail_call != 0 && f->calls == f->fail_call;
	if (f->short_bytes != 0 && length > f->short_bytes)
		length = (size_t)f->short_bytes;
	if (fail && length > 7)
		length = 7;
	if (f->memory != NULL) {
		bzero(buffer, length);
		if (offset < f->stored)
			bcopy(f->memory + offset, buffer,
			    length < f->stored - offset ? length :
			    f->stored - (size_t)offset);
		count = (ssize_t)length;
	} else
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
	fixture_layout(f, variant, sector_bytes, sectors_per_cluster, 2);
}

/* Build the fixture volume with the given number of mirrored FATs. */
static void
fixture_layout(fixture_t *f, unsigned variant, unsigned sector_bytes,
    unsigned sectors_per_cluster, unsigned tables)
{
	fixture_build(f, variant, sector_bytes, sectors_per_cluster, tables,
	    false, 0, 0);
}

/* Memory fixtures retain metadata and the first ten clusters; the unused
 * free tail is implicitly zero, independent of the declared volume size.
 */
static void
fixture_build(fixture_t *f, unsigned variant, unsigned sector_bytes,
    unsigned sectors_per_cluster, unsigned tables, bool memory,
    uint32_t clusters, uint32_t total_sectors)
{
	unsigned char boot[512], root[512], directory[512], data[512], info[512];
	uint32_t sectors, reserved, table_sectors, root_sectors, file_clusters;
	char name[] = "fat-image-XXXXXX";

	bzero(f, sizeof(*f));
	f->variant = variant;
	f->sector_bytes = sector_bytes;
	f->cluster_bytes = sector_bytes * sectors_per_cluster;
	f->clusters = variant == 12 ? 3000 : variant == 16 ? 5000 : 70000;
	if (clusters != 0)
		f->clusters = clusters;
	f->eoc = variant == 12 ? 0xfff : variant == 16 ? 0xffff : 0x0fffffff;
	reserved = variant == 32 ? 32 : 1;
	table_sectors = ((f->clusters + 2) * variant + 8 * sector_bytes - 1) /
	    (8 * sector_bytes);
	root_sectors = variant == 32 ? 0 : (512 * 32 + sector_bytes - 1) / sector_bytes;
	f->tables = tables;
	sectors = reserved + tables * table_sectors + root_sectors +
	    f->clusters * sectors_per_cluster;
	if (total_sectors != 0)
		sectors = total_sectors;
	f->bytes = (uint64_t)sectors * sector_bytes;
	f->fat_offset = (uint64_t)reserved * sector_bytes;
	f->table_bytes = (uint64_t)table_sectors * sector_bytes;
	f->data_offset = (reserved + tables * (uint64_t)table_sectors + root_sectors) * sector_bytes;
	f->root_offset = variant == 32 ? f->data_offset :
	    f->fat_offset + tables * f->table_bytes;
	f->fd = -1;
	if (memory) {
		f->stored = (size_t)(f->data_offset + 10 * f->cluster_bytes);
		f->memory = calloc(1, f->stored);
		CHECK(f->memory != NULL);
	} else {
		f->fd = mkstemp(name);
		CHECK(f->fd != -1 && unlink(name) == 0);
		CHECK(ftruncate(f->fd, (off_t)f->bytes) == 0);
	}

	bzero(boot, sizeof(boot));
	boot[0] = 0xeb;
	boot[1] = 0x58;
	boot[2] = 0x90;
	bcopy("FSIMGTST", boot + 3, 8);
	put16(boot + 11, (uint16_t)sector_bytes);
	boot[13] = (unsigned char)sectors_per_cluster;
	put16(boot + 14, (uint16_t)reserved);
	boot[16] = (unsigned char)tables;
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
		put16(boot + 50, 6); /* BPB_BkBootSec */
		/* FAT32 BS_BootSig and BS_VolLab. */
		boot[66] = 0x29;
		bcopy("NO NAME    ", boot + 71, 11);
	} else {
		put16(boot + 22, (uint16_t)table_sectors);
		/* FAT12/16 BS_BootSig and BS_VolLab. */
		boot[38] = 0x29;
		bcopy("NO NAME    ", boot + 43, 11);
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

/* The default fixture root holds exactly two live slots, so shifting them
 * down one slot to insert the label is lossless.
 */
static void
fixture_add_root_label(fixture_t *f, const char *label)
{
	unsigned char root[512];

	CHECK(pread(f->fd, root, sizeof(root), (off_t)f->root_offset) ==
	    sizeof(root));
	bcopy(root, root + 32, 64);
	bzero(root, 32);
	bcopy(label, root, 11);
	root[11] = 0x08;
	write_bytes(f, f->root_offset, root, sizeof(root));
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
	if (f->fd != -1)
		CHECK(close(f->fd) == 0);
	free(f->memory);
	CHECK(close(f->scratch) == 0);
	CHECK(rmdir(f->directory) == 0);
}

/* An exact listing also excludes dot records and any unexpected children. */
static void
check_fixture_listing(fixture_t *f, const char *case_name)
{
	static const char *const paths[] = {
		"/HELLO.TXT", "/SUB", "/SUB/EMPTY"
	};
	const fsimg_entry_t *entry;
	fsimg_status_t status;

	CHECK(fsimg_reader_open(f->ctx, f->io, NULL, &f->reader, NULL) == FSIMG_OK);
	for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		status = fsimg_reader_next(f->reader, &entry, NULL);
		if (status != FSIMG_OK || strcmp(entry->path, paths[i]) != 0)
			fprintf(stderr, "FAT%u %s: expected listing entry %s, got %s (%s)\n",
			    f->variant, case_name, paths[i],
			    entry == NULL ? "NULL" : entry->path, fsimg_status_string(status));
		CHECK(status == FSIMG_OK);
		CHECK(strcmp(entry->path, paths[i]) == 0);
	}
	status = fsimg_reader_next(f->reader, &entry, NULL);
	if (status != FSIMG_DONE || entry != NULL)
		fprintf(stderr, "FAT%u %s: expected end of listing, got %s\n",
		    f->variant, case_name, fsimg_status_string(status));
	CHECK(status == FSIMG_DONE);
	CHECK(entry == NULL);
	fsimg_reader_free(f->reader);
	f->reader = NULL;
}

static void
test_dot_records_ntres_ignored(unsigned variant)
{
	fixture_t f;
	fsimg_verify_report_t report;
	unsigned char ntres = 0x50;

	fixture_init(&f, variant);
	/* KolibriOS sets reserved NTRes bits on both structural records. */
	write_bytes(&f, cluster_offset(&f, 7) + 12, &ntres, 1);
	write_bytes(&f, cluster_offset(&f, 7) + 32 + 12, &ntres, 1);
	fixture_open(&f);
	check_fixture_listing(&f, "reserved NTRes bits on dots");

	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_verify(f.reader, 0, &report, NULL) == FSIMG_OK);
	CHECK(report.complete);
	fixture_close(&f);
}

static void
test_dot_record_attribute_bits(unsigned variant)
{
	typedef struct {
		unsigned char attribute;
		bool valid;
		const char *why;
		unsigned slot;
	} dot_attribute_case_t;
	static const dot_attribute_case_t cases[] = {
		{ .attribute = 0x12, .valid = true,
		    .why = "macOS hidden dots", .slot = 0 },
		{ .attribute = 0x13, .valid = true,
		    .why = "read-only and hidden", .slot = 0 },
		{ .attribute = 0x30, .valid = true,
		    .why = "archive bit", .slot = 0 },
		{ .attribute = 0x37, .valid = true,
		    .why = "every ignorable bit", .slot = 0 },
		{ .attribute = 0x18, .valid = false,
		    .why = "volume-label bit", .slot = 0 },
		{ .attribute = 0x02, .valid = false,
		    .why = "no directory bit", .slot = 1 },
		{ .attribute = 0x0f, .valid = false,
		    .why = "long-name attribute", .slot = 0 },
		{ .attribute = 0x50, .valid = false,
		    .why = "reserved bit 0x40", .slot = 1 },
		{ .attribute = 0x90, .valid = false,
		    .why = "reserved bit 0x80", .slot = 0 }
	};
	fixture_t f;
	fsimg_verify_report_t report;
	fsimg_error_t error;
	fsimg_status_t status;

	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const dot_attribute_case_t *c = &cases[i];

		fixture_init(&f, variant);
		write_bytes(&f, cluster_offset(&f, 7) + c->slot * 32 + 11,
		    &c->attribute, 1);
		if (c->valid)
			write_bytes(&f, cluster_offset(&f, 7) + 32 + 11,
			    &c->attribute, 1);
		fixture_open(&f);
		if (c->valid)
			check_fixture_listing(&f, c->why);

		status = fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL);
		CHECK(status == FSIMG_OK);
		bzero(&error, sizeof(error));
		status = fsimg_verify(f.reader, 0, &report, &error);
		if (status != (c->valid ? FSIMG_OK : FSIMG_ECORRUPT) ||
		    report.complete != c->valid || (!c->valid &&
		    strcmp(error.message, "invalid FAT dot directory record") != 0))
			fprintf(stderr, "FAT%u dot attribute 0x%02x (%s): "
			    "got %s (%s), complete=%d\n", variant,
			    c->attribute, c->why, fsimg_status_string(status),
			    error.message, report.complete);
		if (c->valid) {
			CHECK(status == FSIMG_OK);
			CHECK(report.complete);
		} else {
			CHECK(status == FSIMG_ECORRUPT);
			CHECK(strcmp(error.message,
			    "invalid FAT dot directory record") == 0);
			CHECK(!report.complete);
		}
		fixture_close(&f);
	}
}

static void
test_directory_without_dot_records(unsigned variant)
{
	fixture_t f;
	fsimg_verify_report_t report;
	fsimg_error_t error;
	unsigned char directory[512];
	struct stat st;

	fixture_init(&f, variant);
	bzero(directory, sizeof(directory));
	short_entry(directory, "EMPTY      ", false, 0, 0);
	write_bytes(&f, cluster_offset(&f, 7), directory, sizeof(directory));
	fixture_open(&f);
	check_fixture_listing(&f, "directory without dot records");

	/* Extraction uses ordinary reading, so EMPTY is still accessible. */
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_extract_at(f.reader, f.scratch, "output", NULL, NULL) == FSIMG_OK);
	CHECK(fstatat(f.scratch, "output/SUB/EMPTY", &st, AT_SYMLINK_NOFOLLOW) == 0);
	CHECK(S_ISREG(st.st_mode) && st.st_size == 0);
	CHECK(unlinkat(f.scratch, "output/SUB/EMPTY", 0) == 0);
	CHECK(unlinkat(f.scratch, "output/SUB", AT_REMOVEDIR) == 0);
	CHECK(unlinkat(f.scratch, "output/HELLO.TXT", 0) == 0);
	CHECK(unlinkat(f.scratch, "output", AT_REMOVEDIR) == 0);
	fsimg_reader_free(f.reader);
	f.reader = NULL;

	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_verify(f.reader, 0, &report, &error) == FSIMG_ECORRUPT);
	CHECK(strcmp(error.message, "FAT directory lacks dot records") == 0);
	CHECK(!report.complete);
	fixture_close(&f);
}

static void
test_dot_record_clusters_ignored_when_listing(unsigned variant)
{
	static const char *const cases[] = { "wrong dot", "wrong dotdot" };
	fixture_t f;
	unsigned char raw[32];

	for (unsigned slot = 0; slot < 2; slot++) {
		fixture_init(&f, variant);
		short_entry(raw, slot == 0 ? ".          " : "..         ", true, 9, 0);
		write_bytes(&f, cluster_offset(&f, 7) + slot * 32, raw, sizeof(raw));
		fixture_open(&f);
		check_fixture_listing(&f, cases[slot]);
		fixture_close(&f);
	}
}

static void
test_misplaced_dot_records_skipped_when_listing(unsigned variant)
{
	static const misplaced_dot_case_t cases[] = {
		{
			.name = "children before both dots",
			.reason = "FAT directory lacks dot records",
			.dot_slot = 2, .dotdot_slot = 1, .empty_slot = 0
		},
		{
			.name = "stray dot after EMPTY",
			.reason = "invalid FAT short-name byte",
			.dot_slot = 3, .dotdot_slot = 1, .empty_slot = 2
		},
		{
			.name = "dot record in the root",
			.reason = "invalid FAT short-name byte",
			.root = true, .dot_slot = 2
		}
	};
	fixture_t f;
	fsimg_verify_report_t report;
	fsimg_error_t error;
	unsigned char directory[512];
	unsigned char raw[32];
	fsimg_status_t status;

	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const misplaced_dot_case_t *c = &cases[i];

		fixture_init(&f, variant);
		if (c->root) {
			short_entry(raw, ".          ", true, 0, 0);
			write_bytes(&f, f.root_offset + c->dot_slot * 32, raw, sizeof(raw));
		} else {
			bzero(directory, sizeof(directory));
			/* Retain the leading dot when adding a stray one. */
			short_entry(directory, ".          ", true, 7, 0);
			short_entry(directory + c->dot_slot * 32, ".          ", true, 7, 0);
			short_entry(directory + c->dotdot_slot * 32, "..         ", true, 0, 0);
			short_entry(directory + c->empty_slot * 32, "EMPTY      ", false, 0, 0);
			write_bytes(&f, cluster_offset(&f, 7), directory, sizeof(directory));
		}
		fixture_open(&f);
		check_fixture_listing(&f, c->name);

		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
		bzero(&error, sizeof(error));
		status = fsimg_verify(f.reader, 0, &report, &error);
		if (status != FSIMG_ECORRUPT || strcmp(error.message, c->reason) != 0 ||
		    report.complete)
			fprintf(stderr, "FAT%u %s: expected incomplete verification with %s, got %s (%s)\n",
			    variant, c->name, c->reason, fsimg_status_string(status), error.message);
		CHECK(status == FSIMG_ECORRUPT);
		CHECK(strcmp(error.message, c->reason) == 0);
		CHECK(!report.complete);
		fixture_close(&f);
	}
}

static void
test_long_name_before_misplaced_dot_record_rejected(unsigned variant)
{
	static const orphan_case_t cases[] = {
		{ .name = "LFN, dot, deleted slot", .dot_name = ".          ",
		    .dot_cluster = 7, .deleted_after_dot = true },
		{ .name = "LFN, dotdot, deleted slot", .dot_name = "..         ",
		    .dot_cluster = 0, .deleted_after_dot = true },
		{ .name = "LFN, dot, matching short entry", .dot_name = ".          ",
		    .dot_cluster = 7 },
		{ .name = "LFN, dotdot, matching short entry", .dot_name = "..         ",
		    .dot_cluster = 0 }
	};
	fixture_t f;
	fsimg_error_t error;
	const fsimg_entry_t *entry;
	unsigned char records[96], raw[32];
	const uint16_t name[] = { 'L', 'o', 'n', 'g' };
	size_t length;
	fsimg_status_t status;

	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const orphan_case_t *c = &cases[i];

		fixture_init(&f, variant);
		short_entry(raw, "SHORT      ", false, 0, 0);
		/* The LFN checksum matches the live entry after the dot, so it
		 * could attach across the skipped dot. A deleted slot instead
		 * discards the LFN, hiding it from the end-marker check.
		 */
		length = long_records(records, name, sizeof(name) / sizeof(name[0]), raw);
		CHECK(length == 64);
		bcopy(raw, records + 64, sizeof(raw));
		if (c->deleted_after_dot)
			records[64] = 0xe5;
		short_entry(records + 32, c->dot_name, true, c->dot_cluster, 0);
		write_bytes(&f, cluster_offset(&f, 7) + 96, records, sizeof(records));
		fixture_open(&f);
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
		CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
		CHECK(strcmp(entry->path, "/HELLO.TXT") == 0);
		CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
		CHECK(strcmp(entry->path, "/SUB") == 0);

		bzero(&error, sizeof(error));
		status = fsimg_reader_next(f.reader, &entry, &error);
		if (status != FSIMG_ECORRUPT ||
		    strcmp(error.message, "orphaned FAT long-name sequence") != 0 || entry != NULL)
			fprintf(stderr, "FAT%u %s: expected orphaned FAT long-name sequence and no entry, got %s (%s)\n",
			    variant, c->name, fsimg_status_string(status), error.message);
		CHECK(status == FSIMG_ECORRUPT);
		CHECK(strcmp(error.message, "orphaned FAT long-name sequence") == 0);
		CHECK(entry == NULL);
		fixture_close(&f);
	}
}

static void
test_variants(void)
{
	static const unsigned variants[] = { 16, 12, 32 };
	static const char *const names[] = {
		"fat12", "fat16", "fat32", "exfat", "qnx6", "qnx4", "ifs",
		"iso9660", "hfsplus", "hfsx"
	};
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

	/* The registry lists every format and its advertised operations. */
	cursor = 0;
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		CHECK(fsimg_format_next(&cursor, &format, NULL) == FSIMG_OK);
		CHECK(strcmp(format.name, names[i]) == 0);
		if (strcmp(format.name, "hfsplus") == 0 ||
		    strcmp(format.name, "hfsx") == 0)
			CHECK(format.capabilities == FSIMG_CAP_READ);
		else
			CHECK(format.capabilities == (FSIMG_CAP_READ |
			    FSIMG_CAP_WRITE | FSIMG_CAP_VERIFY));
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
		/* One DATA range covers the remaining fragments; a range query
		 * resolves no storage and reads nothing.
		 */
		calls = f.calls;
		CHECK(fsimg_reader_next_range(f.reader, 512, &range, NULL) == FSIMG_OK);
		CHECK(range.offset == 512 && range.length == 588 && range.kind == FSIMG_RANGE_DATA);
		CHECK(f.calls == calls);
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
			CHECK(fsimg_reader_open(f.ctx, f.io, &options, &other, NULL) == FSIMG_EFORMAT);
			CHECK(other == NULL);
			options.format = "qnx4";
			CHECK(fsimg_reader_open(f.ctx, f.io, &options, &other, NULL) == FSIMG_EFORMAT);
			CHECK(other == NULL);
			options.format = "ifs";
			CHECK(fsimg_reader_open(f.ctx, f.io, &options, &other, NULL) == FSIMG_EFORMAT);
			CHECK(other == NULL);
			/* An unknown format name is unsupported. */
			options.format = "qnx2";
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
		CHECK(fsimg_fat_geometry_decode(boot, sizeof(boot), UINT64_MAX, &geometry, NULL) == FSIMG_OK);
		CHECK(fsimg_fat_cluster_valid(&geometry, 0x0ffffff6));
		put32(boot + 44, 0x0ffffff7);
		CHECK(fsimg_fat_geometry_decode(boot, sizeof(boot), UINT64_MAX, &geometry, NULL) == FSIMG_ECORRUPT);
		put32(boot + 44, 2);
		put32(boot + 32, 32 + 4194304 + 0x0ffffff6);
		CHECK(fsimg_fat_geometry_decode(boot, sizeof(boot), UINT64_MAX, &geometry, NULL) == FSIMG_ECORRUPT);
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
		CHECK(fsimg_fat_geometry_decode(boot, sizeof(boot), (uint64_t)sectors * 512, &geometry, NULL) == FSIMG_OK);
		CHECK(geometry.variant == variant && geometry.clusters == clusters);
		fixture_close(&f);
	}
}

/* Replace the root's contents with a chain of depth nested directories
 * /D00/D01/..., one cluster each from 20, the deepest holding an empty file
 * EMPTY. depth is at most 100.
 */
static void
make_deep_tree(fixture_t *f, unsigned depth)
{
	unsigned char root[512], directory[512];
	char name[16];

	set_cluster(f, 3, 0);
	set_cluster(f, 5, 0);
	set_cluster(f, 7, 0);
	set_cluster(f, 9, 0);
	bzero(root, sizeof(root));
	short_entry(root, "D00        ", true, 20, 0);
	write_bytes(f, f->root_offset, root, sizeof(root));
	for (unsigned i = 0; i < depth; i++) {
		set_cluster(f, 20 + i, f->eoc);
		bzero(directory, sizeof(directory));
		short_entry(directory, ".          ", true, 20 + i, 0);
		short_entry(directory + 32, "..         ", true, i == 0 ? 0 : 19 + i, 0);
		if (i + 1 == depth) {
			short_entry(directory + 64, "EMPTY      ", false, 0, 0);
		} else {
			CHECK(snprintf(name, sizeof(name), "D%02u        ", i + 1) == 11);
			short_entry(directory + 64, name, true, 21 + i, 0);
		}
		write_bytes(f, cluster_offset(f, 20 + i), directory, sizeof(directory));
	}
}

/* Traverse a deep tree with a one-page metadata cache, so every scratch
 * object spills, and return the peak descriptor count. The tree must be
 * traversed completely within max_open_files.
 */
static uint64_t
deep_tree_descriptors(unsigned depth, uint64_t max_open_files)
{
	fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_stats_t stats;
	unsigned entries;
	fsimg_status_t status;

	fixture_init(&f, 16);
	make_deep_tree(&f, depth);
	fixture_open(&f);
	CHECK(fsimg_test_meta_cache(f.ctx, 1, NULL) == FSIMG_OK);
	f.ctx->options.max_open_files = max_open_files;
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	entries = 0;
	while ((status = fsimg_reader_next(f.reader, &entry, NULL)) == FSIMG_OK)
		entries++;
	CHECK(status == FSIMG_DONE && entries == depth + 1);
	CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
	fixture_close(&f);
	return (stats.descriptors_peak);
}

/* Retained ancestors share one scratch store, so descriptor use does not
 * grow with depth and a deep legal tree fits a small descriptor limit.
 */
static void
test_deep_descriptors(void)
{
	uint64_t shallow, deep;

	shallow = deep_tree_descriptors(4, 16);
	deep = deep_tree_descriptors(60, 16);
	CHECK(deep == shallow);
}

/* A failed advance is terminal for the walker itself, so even a caller that
 * ignores the error cannot replay the partial directory or its parent.
 */
static void
test_walker_poisoned(void)
{
	fixture_t f;
	unsigned char child[32];

	fixture_init(&f, 16);
	make_deep_tree(&f, 4);
	/* Replace the D02 entry in /D00/D01 with a file whose name
	 * has a forbidden leading space.
	 */
	short_entry(child, " SPACE  TXT", false, 0, 0);
	write_bytes(&f, cluster_offset(&f, 21) + 64, child, sizeof(child));
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	f.reader->state = FSIMG_READER_ITERATING;
	CHECK(fsimg_walk_next(f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_walk_next(f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_walk_next(f.reader, NULL) == FSIMG_ECORRUPT);
	CHECK(fsimg_walk_next(f.reader, NULL) == FSIMG_ESTATE);
	CHECK(fsimg_walk_skip(f.reader, NULL) == FSIMG_ESTATE);
	fixture_close(&f);
}

static void
test_deep_tree(void)
{
	fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_verify_report_t report;
	char name[12], expected[256];

	fixture_init(&f, 16);
	make_deep_tree(&f, 20);

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
	static const unsigned variants[] = { 12, 16, 32 };
	static const unsigned char no_tables = 0;
	fsimg_verify_report_t report;
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

	/* The specification allows any nonzero table count, and the supported
	 * profile reads one or two. One table reads and verifies without a
	 * mirror. A coherent third table is a recognized FAT volume this reader
	 * does not support, while a zero count is not a legal BPB at all.
	 */
	for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); i++) {
		fixture_layout(&f, variants[i], 512, 1, 1);
		fixture_open(&f);
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
		CHECK(fsimg_verify(f.reader, 0, &report, NULL) == FSIMG_OK);
		CHECK(report.complete && report.files == 2);
		fixture_close(&f);

		fixture_layout(&f, variants[i], 512, 1, 3);
		fixture_open(&f);
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) ==
		    FSIMG_EUNSUPPORTED);
		fixture_close(&f);

		fixture_init(&f, variants[i]);
		write_bytes(&f, 16, &no_tables, 1);
		fixture_open(&f);
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) ==
		    FSIMG_EFORMAT);
		fixture_close(&f);
	}
}

static void
test_tot_sec16_overrides_boot_code(void)
{
	fixture_t f;
	fsimg_verify_report_t report;
	fsimg_volume_info_t info;
	const fsimg_entry_t *entry;
	uint32_t tot_sec16;
	unsigned char boot[512];
	static const unsigned char boot_code[] = { 0xfa, 0x33, 0xc0, 0x8e };

	/* Before DOS 3.31, boot code starts at offset 30 without an extended BPB.
	 * TotSec16 takes precedence over boot code in the TotSec32 position.
	 */
	fixture_init(&f, 12);
	CHECK(pread(f.fd, boot, sizeof(boot), 0) == sizeof(boot));
	tot_sec16 = fsimg_load_le16(boot + 19);
	CHECK(tot_sec16 != 0);
	CHECK((uint64_t)tot_sec16 * f.sector_bytes == f.bytes);
	memset(boot + 30, 0xcc, 32);
	bcopy(boot_code, boot + 32, sizeof(boot_code));
	write_bytes(&f, 0, boot, sizeof(boot));
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
	CHECK(info.volume_bytes == (uint64_t)tot_sec16 * f.sector_bytes);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(strcmp(entry->path, "/HELLO.TXT") == 0);
	fsimg_reader_free(f.reader);
	f.reader = NULL;
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_verify(f.reader, 0, &report, NULL) == FSIMG_OK);
	CHECK(report.complete);
	fixture_close(&f);
}

/* Remove all three FAT identity vouchers while retaining the file chains. */
static void
fixture_assume_identity(fixture_t *f)
{
	static const unsigned char jump[3] = { 0x60, 0x38, 0 };
	static const unsigned char signature[2] = { 0, 0 };
	static const unsigned char head[3] = { 0, 0, 3 };

	write_bytes(f, 0, jump, sizeof(jump));
	write_bytes(f, 510, signature, sizeof(signature));
	for (unsigned table = 0; table < f->tables; table++)
		write_bytes(f, f->fat_offset + table * f->table_bytes,
		    head, sizeof(head));
}

static void
test_assumed_atari_identity_reads_fragmented_file(void)
{
	fixture_t f;
	fsimg_read_options_t options;
	fsimg_verify_report_t report;
	fsimg_error_t error;
	const fsimg_entry_t *entry;
	unsigned char data[1100], expected[1100];
	size_t count;
	static const char *const paths[] = { "/SUB", "/SUB/EMPTY" };

	fixture_init(&f, 12);
	fixture_assume_identity(&f);
	fixture_open(&f);
	fsimg_read_options_init(&options);
	options.format = "fat12";
	CHECK(fsimg_reader_open(f.ctx, f.io, &options, &f.reader, &error) ==
	    FSIMG_EFORMAT);
	options.flags = FSIMG_READ_ASSUME_IDENTITY;
	CHECK(fsimg_reader_open(f.ctx, f.io, &options, &f.reader, &error) ==
	    FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(strcmp(entry->path, "/HELLO.TXT") == 0 && entry->size == sizeof(data));
	CHECK(fsimg_reader_read_at(f.reader, 0, data, sizeof(data), &count,
	    NULL) == FSIMG_OK);
	CHECK(count == sizeof(data));
	for (size_t i = 0; i < sizeof(expected); i++)
		expected[i] = (unsigned char)(i * 37);
	CHECK(bcmp(data, expected, sizeof(data)) == 0);
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
		CHECK(strcmp(entry->path, paths[i]) == 0);
	}
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_DONE);
	CHECK(entry == NULL);
	fsimg_reader_free(f.reader);
	f.reader = NULL;
	CHECK(fsimg_reader_open(f.ctx, f.io, &options, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_verify(f.reader, 0, &report, &error) == FSIMG_ECORRUPT);
	CHECK(!report.complete);
	CHECK(strcmp(error.message, "fat12 identity was assumed: the boot record "
	    "has no PC boot signature and the first FAT does not repeat the media byte") == 0);
	fixture_close(&f);
}

static void
test_assumed_identity_still_validates_geometry(void)
{
	fixture_t f;
	fsimg_read_options_t options;
	fsimg_error_t error;
	fsimg_volume_info_t info;
	unsigned char field[2];

	fsimg_read_options_init(&options);
	options.format = "fat12";
	options.flags = FSIMG_READ_ASSUME_IDENTITY;
	fixture_init(&f, 12);
	fixture_assume_identity(&f);
	put16(field, 300);
	write_bytes(&f, 11, field, sizeof(field));
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, &options, &f.reader, &error) ==
	    FSIMG_EFORMAT);
	CHECK(strcmp(error.message, "boot record has no legal FAT BPB") == 0);
	fixture_close(&f);

	fixture_init(&f, 12);
	fixture_assume_identity(&f);
	CHECK(f.bytes / f.sector_bytes < UINT16_MAX);
	put16(field, (uint16_t)(f.bytes / f.sector_bytes + 1));
	write_bytes(&f, 19, field, sizeof(field));
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, &options, &f.reader, &error) ==
	    FSIMG_OK);
	CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
	CHECK(info.missing_bytes == f.sector_bytes);
	fixture_close(&f);
}

static void
test_assumed_identity_still_rejects_broken_chain(void)
{
	fixture_t f;
	fsimg_read_options_t options;
	const fsimg_entry_t *entry;
	unsigned char data[1100];
	size_t count;

	fixture_init(&f, 12);
	fixture_assume_identity(&f);
	set_cluster(&f, 3, 0); /* The first data cluster points to free storage. */
	fixture_open(&f);
	fsimg_read_options_init(&options);
	options.format = "fat12";
	options.flags = FSIMG_READ_ASSUME_IDENTITY;
	CHECK(fsimg_reader_open(f.ctx, f.io, &options, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(strcmp(entry->path, "/HELLO.TXT") == 0);
	CHECK(fsimg_reader_read_at(f.reader, 0, data, sizeof(data), &count,
	    NULL) == FSIMG_ECORRUPT);
	fixture_close(&f);
}

static void
test_signed_volume_with_assume_identity_verifies(void)
{
	static const unsigned variants[] = { 12, 16, 32 };
	fixture_t f;
	fsimg_read_options_t options;
	fsimg_verify_report_t report;

	fsimg_read_options_init(&options);
	options.flags = FSIMG_READ_ASSUME_IDENTITY;
	for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); i++) {
		unsigned variant = variants[i];

		fixture_init(&f, variant);
		options.format = variant == 12 ? "fat12" : variant == 16 ? "fat16" : "fat32";
		fixture_open(&f);
		CHECK(fsimg_reader_open(f.ctx, f.io, &options, &f.reader, NULL) == FSIMG_OK);
		CHECK(fsimg_verify(f.reader, 0, &report, NULL) == FSIMG_OK);
		CHECK(report.complete);
		fixture_close(&f);
	}
}

static void
test_assumed_fat16_media_entry_is_reported_by_verify(void)
{
	fixture_t f;
	fsimg_read_options_t options;
	fsimg_verify_report_t report;
	fsimg_error_t error;

	fixture_init(&f, 16);
	set_cluster(&f, 0, 0xff00);
	fixture_open(&f);
	fsimg_read_options_init(&options);
	options.format = "fat16";
	CHECK(fsimg_reader_open(f.ctx, f.io, &options, &f.reader, &error) ==
	    FSIMG_ECORRUPT);
	CHECK(strcmp(error.message, "FAT media descriptor does not match the BPB") == 0);
	options.flags = FSIMG_READ_ASSUME_IDENTITY;
	CHECK(fsimg_reader_open(f.ctx, f.io, &options, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_verify(f.reader, 0, &report, &error) == FSIMG_ECORRUPT);
	CHECK(!report.complete);
	CHECK(strcmp(error.message, "fat16 identity was assumed: "
	    "FAT[0] does not repeat the media byte") == 0);
	fixture_close(&f);
}

static void
test_assumed_reserved_entry_has_no_status_flags(void)
{
	fixture_t f;
	fsimg_read_options_t options;
	fsimg_volume_info_t info;
	fsimg_verify_report_t report;
	fsimg_error_t error;
	char message[128];

	for (unsigned variant = 16; variant <= 32; variant += 16) {
		fixture_init(&f, variant);
		set_cluster(&f, 1, 0);
		fixture_open(&f);
		fsimg_read_options_init(&options);
		options.format = variant == 16 ? "fat16" : "fat32";
		CHECK(fsimg_reader_open(f.ctx, f.io, &options, &f.reader, &error) ==
		    FSIMG_ECORRUPT);
		CHECK(strcmp(error.message, "invalid FAT reserved entries") == 0);
		options.flags = FSIMG_READ_ASSUME_IDENTITY;
		CHECK(fsimg_reader_open(f.ctx, f.io, &options, &f.reader, NULL) == FSIMG_OK);
		CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
		CHECK(info.clean && !info.io_error);
		CHECK(fsimg_verify(f.reader, 0, &report, &error) == FSIMG_ECORRUPT);
		CHECK(!report.complete);
		snprintf(message, sizeof(message), "fat%u identity was assumed: "
		    "FAT[1] is not a reserved end-of-chain entry", variant);
		CHECK(strcmp(error.message, message) == 0);
		fixture_close(&f);
	}
}

static void
test_assumed_identity_still_compares_reserved_entry_mirrors(void)
{
	fixture_t f;
	fsimg_read_options_t options;
	fsimg_error_t error;
	unsigned char bad_entry[2] = { 0, 0 };

	fsimg_read_options_init(&options);
	options.format = "fat16";
	options.flags = FSIMG_READ_ASSUME_IDENTITY;
	for (unsigned entry = 0; entry <= 1; entry++) {
		fixture_init(&f, 16);
		write_bytes(&f, f.fat_offset + entry * 2, bad_entry, sizeof(bad_entry));
		fixture_open(&f);
		CHECK(fsimg_reader_open(f.ctx, f.io, &options, &f.reader, &error) ==
		    FSIMG_ECORRUPT);
		CHECK(strcmp(error.message, "mirrored FAT entries disagree") == 0);
		fixture_close(&f);
	}
}

static void
test_assume_identity_requires_named_format_and_known_flags(void)
{
	fixture_t f;
	fsimg_read_options_t options;

	fixture_init(&f, 12);
	fixture_open(&f);
	fsimg_read_options_init(&options);
	options.flags = FSIMG_READ_ASSUME_IDENTITY;
	CHECK(fsimg_reader_open(f.ctx, f.io, &options, &f.reader, NULL) == FSIMG_EINVAL);
	options.format = "fat12";
	options.flags = FSIMG_READ_ASSUME_IDENTITY | UINT32_C(0x80000000);
	CHECK(fsimg_reader_open(f.ctx, f.io, &options, &f.reader, NULL) == FSIMG_EINVAL);
	fixture_close(&f);
}

/* Non-PC boot records identify FAT by the media echo, regardless of jump. */
static void
test_non_pc_boot_records(void)
{
	static const unsigned char jumps[][3] = {
		{ 0x60, 0x38, 0x00 }, /* Atari ST BRA.S */
		{ 0x00, 0x00, 0x00 }, /* Atari disk without a jump */
		{ 0xeb, 0xfe, 0x90 }  /* MSX-DOS jump to self */
	};
	fixture_t f;
	fsimg_verify_report_t report;
	fsimg_volume_info_t info;
	const fsimg_entry_t *entry;
	static const unsigned char no_signature[2] = { 0, 0 };

	for (size_t i = 0; i < sizeof(jumps) / sizeof(jumps[0]); i++) {
		fixture_init(&f, 12);
		CHECK(f.fat_offset + 3 <= 8192);
		write_bytes(&f, 0, jumps[i], sizeof(jumps[i]));
		write_bytes(&f, 510, no_signature, sizeof(no_signature));
		fixture_open(&f);
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) ==
		    FSIMG_OK);
		CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
		CHECK(strcmp(info.format, "fat12") == 0);
		CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
		CHECK(strcmp(entry->path, "/HELLO.TXT") == 0);
		fsimg_reader_free(f.reader);
		f.reader = NULL;
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) ==
		    FSIMG_OK);
		CHECK(fsimg_verify(f.reader, 0, &report, NULL) == FSIMG_OK);
		CHECK(report.complete);
		fixture_close(&f);
	}
}

/* Opening reads the whole volume when unsigned FAT12 is shorter than 8 KiB. */
static void
test_unsigned_tiny_fat_open(void)
{
	fixture_t f;
	fsimg_volume_info_t info;
	const fsimg_entry_t *entry;
	unsigned char volume[4096] = { 0 };

	volume[0] = 0x60, volume[1] = 0x38; /* Atari ST BRA.S */
	put16(volume + 11, 512);
	volume[13] = 1;
	put16(volume + 14, 1);
	volume[16] = 1;
	put16(volume + 17, 16);
	put16(volume + 19, 8);
	volume[21] = 0xf8;
	put16(volume + 22, 1);
	volume[512] = 0xf8, volume[513] = 0xff, volume[514] = 0xff;

	fixture_init(&f, 12);
	CHECK(ftruncate(f.fd, sizeof(volume)) == 0);
	f.bytes = sizeof(volume);
	write_bytes(&f, 0, volume, sizeof(volume));
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.format, "fat12") == 0);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_DONE);
	CHECK(entry == NULL);
	fixture_close(&f);
}

/* PC-98's 1024-byte sectors put the identifying echo in the second sector. */
static void
test_pc98_boot_record(void)
{
	fixture_t f;
	fsimg_verify_report_t report;
	fsimg_volume_info_t info;
	const fsimg_entry_t *entry;
	static const unsigned char no_signature[2] = { 0, 0 };

	fixture_geometry(&f, 12, 1024, 1);
	CHECK(f.fat_offset + 3 <= 8192);
	write_bytes(&f, 510, no_signature, sizeof(no_signature));
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.format, "fat12") == 0);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(strcmp(entry->path, "/HELLO.TXT") == 0);
	fsimg_reader_free(f.reader);
	f.reader = NULL;
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_verify(f.reader, 0, &report, NULL) == FSIMG_OK);
	CHECK(report.complete);
	fixture_close(&f);
}

/* A bad echo is corruption only when the PC signature identifies FAT. */
static void
test_unsigned_media_mismatch_is_not_fat(void)
{
	fixture_t f;
	fsimg_error_t error;
	static const unsigned char unsigned_jump[3] = { 0x60, 0x38, 0x00 };
	static const unsigned char no_signature[2] = { 0, 0 };
	static const unsigned char bad_media = 0x55;

	for (unsigned pc_signature = 0; pc_signature < 2; pc_signature++) {
		fixture_init(&f, 12);
		if (pc_signature == 0) {
			write_bytes(&f, 0, unsigned_jump, sizeof(unsigned_jump));
			write_bytes(&f, 510, no_signature, sizeof(no_signature));
		}
		/* Change every table so the failure cannot be mirror disagreement. */
		for (unsigned table = 0; table < f.tables; table++)
			write_bytes(&f, f.fat_offset + table * f.table_bytes,
			    &bad_media, 1);
		fixture_open(&f);
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, &error) ==
		    (pc_signature != 0 ? FSIMG_ECORRUPT : FSIMG_EFORMAT));
		if (pc_signature != 0)
			CHECK(strcmp(error.message,
			    "FAT media descriptor does not match the BPB") == 0);
		else
			CHECK(strcmp(error.message,
			    "unrecognized image format") == 0);
		fixture_close(&f);
	}
}

/* The media echo identifies FAT before geometry validation. */
static void
test_identified_unsigned_missing_tail_opens(void)
{
	fixture_t f;
	fsimg_error_t error;
	fsimg_volume_info_t info;
	unsigned char total_sectors[2];
	static const unsigned char unsigned_jump[3] = { 0x60, 0x38, 0x00 };
	static const unsigned char no_signature[2] = { 0, 0 };

	for (unsigned pc_signature = 0; pc_signature < 2; pc_signature++) {
		fixture_init(&f, 12);
		CHECK(f.bytes / f.sector_bytes < UINT16_MAX);
		put16(total_sectors, (uint16_t)(f.bytes / f.sector_bytes + 1));
		write_bytes(&f, 19, total_sectors, sizeof(total_sectors));
		if (pc_signature == 0) {
			write_bytes(&f, 0, unsigned_jump, sizeof(unsigned_jump));
			write_bytes(&f, 510, no_signature, sizeof(no_signature));
		}
		fixture_open(&f);
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, &error) ==
		    FSIMG_OK);
		CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
		CHECK(info.missing_bytes == f.sector_bytes);
		fixture_close(&f);
	}
}

/* FAT16's third head byte belongs to FAT[1], not FAT[0]. */
static void
test_unsigned_fat16_head_does_not_check_fourth_byte(void)
{
	fixture_t f;
	static const unsigned char no_signature[2] = { 0, 0 };
	/* Clear the dirty/error flags, retaining the reserved low six bits. */
	static const unsigned char fourth = 0x3f;

	fixture_init(&f, 16);
	write_bytes(&f, 510, no_signature, sizeof(no_signature));
	for (unsigned table = 0; table < f.tables; table++)
		write_bytes(&f, f.fat_offset + table * f.table_bytes + 3,
		    &fourth, 1);
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	fixture_close(&f);
}

/* Unsigned identification requires the first FAT head inside the prefix. */
static void
test_unsigned_fat16_and_fat32_identification(void)
{
	fixture_t f;
	fsimg_error_t error;
	fsimg_verify_report_t report;
	const fsimg_entry_t *entry;
	static const unsigned char no_signature[2] = { 0, 0 };

	fixture_init(&f, 16);
	CHECK(f.fat_offset + 3 <= 8192);
	write_bytes(&f, 510, no_signature, sizeof(no_signature));
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(strcmp(entry->path, "/HELLO.TXT") == 0);
	fsimg_reader_free(f.reader);
	f.reader = NULL;
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_verify(f.reader, 0, &report, NULL) == FSIMG_OK);
	CHECK(report.complete);
	fixture_close(&f);

	/* This FAT32 fixture's 32 reserved sectors put table 0 beyond 8 KiB.
	 * Identification rejects it before reading its backup boot record.
	 */
	fixture_init(&f, 32);
	CHECK(f.fat_offset >= 8192);
	write_bytes(&f, 510, no_signature, sizeof(no_signature));
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, &error) == FSIMG_EFORMAT);
	CHECK(strcmp(error.message, "unrecognized image format") == 0);
	fixture_close(&f);
}

/* Forcing a variant still checks an unsigned volume's cluster-count type. */
static void
test_forced_unsigned_fat12_variant(void)
{
	fixture_t f;
	fsimg_read_options_t options;
	fsimg_error_t error;
	static const unsigned char no_signature[2] = { 0, 0 };

	fixture_init(&f, 12);
	write_bytes(&f, 510, no_signature, sizeof(no_signature));
	fixture_open(&f);
	fsimg_read_options_init(&options);
	options.format = "fat12";
	CHECK(fsimg_reader_open(f.ctx, f.io, &options, &f.reader, NULL) == FSIMG_OK);
	fsimg_reader_free(f.reader);
	f.reader = NULL;
	options.format = "fat16";
	CHECK(fsimg_reader_open(f.ctx, f.io, &options, &f.reader, &error) == FSIMG_EFORMAT);
	CHECK(strcmp(error.message, "forced format differs from FAT cluster-count type") == 0);
	fixture_close(&f);
}

static void
test_boot_voucher_is_the_signature_or_the_media_echo(void)
{
	fixture_t f;
	unsigned char boot[8192];
	size_t echo_offset;

	fixture_init(&f, 12);
	CHECK(pread(f.fd, boot, sizeof(boot), 0) == sizeof(boot));
	echo_offset = (size_t)fsimg_load_le16(boot + 14) *
	    fsimg_load_le16(boot + 11);
	CHECK(echo_offset >= 512 && echo_offset + 3 <= sizeof(boot));

	/* Only the PC boot signature vouches. */
	bzero(boot + echo_offset, 3);
	CHECK(fsimg_fat_boot_vouched(boot, sizeof(boot)) == true);
	CHECK(fsimg_fat_boot_vouched(boot, 511) == false);

	/* Only the media echo vouches. */
	boot[510] = 0;
	boot[511] = 0;
	boot[echo_offset] = boot[21];
	boot[echo_offset + 1] = 0xff;
	boot[echo_offset + 2] = 0xff;
	CHECK(fsimg_fat_boot_vouched(boot, sizeof(boot)) == true);
	CHECK(fsimg_fat_boot_vouched(boot, echo_offset) == false);
	CHECK(fsimg_fat_boot_vouched(boot, echo_offset + 2) == false);
	CHECK(fsimg_fat_boot_vouched(boot, echo_offset + 3) == true);

	/* Neither voucher is live. */
	bzero(boot + echo_offset, 3);
	CHECK(fsimg_fat_boot_vouched(boot, sizeof(boot)) == false);
	fixture_close(&f);
}

static void
test_unvouched_oversized_regions_are_not_fat(void)
{
	fixture_t f;
	fsimg_read_options_t options;
	fsimg_error_t error;
	unsigned char total_sectors[2];

	fixture_init(&f, 12);
	fixture_assume_identity(&f);
	/* The fixed BPB fields are legal, but metadata exceeds one sector. */
	put16(total_sectors, 1);
	write_bytes(&f, 19, total_sectors, sizeof(total_sectors));
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, &error) ==
	    FSIMG_EFORMAT);
	CHECK(strcmp(error.message, "unrecognized image format") == 0);

	fsimg_read_options_init(&options);
	options.format = "fat12";
	CHECK(fsimg_reader_open(f.ctx, f.io, &options, &f.reader, &error) ==
	    FSIMG_EFORMAT);
	CHECK(strcmp(error.message, "unrecognized volume boot record") == 0);
	fixture_close(&f);
}

/* Geometry decoding rejects a buffer shorter than the boot record. */
static void
test_geometry_decode_short_boot_record(void)
{
	unsigned char boot[511] = { 0 };
	fat_geometry_t geometry;
	fsimg_error_t error;

	CHECK(fsimg_fat_geometry_decode(boot, sizeof(boot), sizeof(boot),
	    &geometry, &error) == FSIMG_EFORMAT);
	CHECK(strcmp(error.message, "volume is smaller than a boot record") == 0);
}

static void
test_media_check_precedes_mirror_comparison(void)
{
	fixture_t f;
	fsimg_error_t error;
	static const unsigned char bad_media = 0x55;

	/* A bad first-table media byte takes precedence over a mirror mismatch. */
	fixture_init(&f, 12);
	CHECK(f.tables == 2);
	write_bytes(&f, f.fat_offset, &bad_media, 1);
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, &error) ==
	    FSIMG_ECORRUPT);
	CHECK(strcmp(error.message,
	    "FAT media descriptor does not match the BPB") == 0);
	fixture_close(&f);

	/* Matching tables still must echo the BPB media descriptor. */
	fixture_init(&f, 12);
	CHECK(f.tables == 2);
	write_bytes(&f, f.fat_offset, &bad_media, 1);
	write_bytes(&f, f.fat_offset + f.table_bytes, &bad_media, 1);
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, &error) ==
	    FSIMG_ECORRUPT);
	CHECK(strcmp(error.message,
	    "FAT media descriptor does not match the BPB") == 0);
	fixture_close(&f);

	/* A bad second-table media byte remains a mirror disagreement. */
	fixture_init(&f, 12);
	CHECK(f.tables == 2);
	write_bytes(&f, f.fat_offset + f.table_bytes, &bad_media, 1);
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, &error) ==
	    FSIMG_ECORRUPT);
	CHECK(strcmp(error.message, "mirrored FAT entries disagree") == 0);
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
		CHECK(fsimg_fat_geometry_decode(boot, sizeof(boot), (uint64_t)sectors * 512, &geometry, NULL) == FSIMG_OK);
		CHECK(geometry.variant == (clusters == 4084 ? 12 : 16));
	}
	/* An illegal fixed BPB field means the record is not FAT at all. */
	boot[13] = 3;
	CHECK(fsimg_fat_geometry_decode(boot, sizeof(boot), UINT64_MAX, &geometry, NULL) == FSIMG_EFORMAT);
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

	for (unsigned scenario = 0; scenario < 3; scenario++) {
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
		if (scenario != 0) {
			CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
			CHECK(fsimg_reader_next_range(f.reader, 0, &range, NULL) == FSIMG_OK);
		}

		if (scenario == 2) {
			CHECK(ftruncate(output, (off_t)f.data_offset) == 0);
			CHECK(fsimg_reader_bytes(f.reader, f.bytes + 1, &byte, 1,
			    &count, NULL) == FSIMG_ESOURCE_CHANGED);
			CHECK(count == 0);
		} else
			CHECK(pwrite(output, "X", 1, (off_t)cluster_offset(&f, 3)) == 1);
		/* Make the metadata change visible even on coarse timestamp clocks. */
		{
			const struct timespec changed[2] = {
				{ .tv_nsec = UTIME_OMIT }, { .tv_sec = 1 }
			};

			CHECK(futimens(output, changed) == 0);
		}
		if (scenario == 2)
			CHECK(fsimg_reader_read_at(f.reader, 0, &byte, 1, &count, NULL) == FSIMG_ESOURCE_CHANGED);
		else if (scenario == 0)
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
	char name[16];
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
		/* entries is at most 65520, so the 8.3 name has 11 characters. */
		CHECK(snprintf(name, sizeof(name), "%08uTXT", i) == 11);
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
		if (scenario == 3 || scenario == 4) {
			if (strcmp(error.message, "FAT dot record has wrong directory cluster") != 0)
				fprintf(stderr, "fixture %s: expected wrong directory cluster, got %s\n",
				    cases[scenario], error.message);
			CHECK(strcmp(error.message, "FAT dot record has wrong directory cluster") == 0);
		}
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
	CHECK(fsimg_reader_open(f.ctx, short_slice, NULL, &other, NULL) == FSIMG_OK);
	{
		fsimg_volume_info_t info;

		CHECK(fsimg_reader_get_info(other, &info, NULL) == FSIMG_OK);
		CHECK(info.volume_bytes == old_length && info.missing_bytes == 1);
	}
	CHECK(fsimg_verify(other, 0, &report, NULL) == FSIMG_ECORRUPT);
	CHECK(!report.complete);
	fsimg_reader_free(other);
	other = NULL;
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
test_boot_label_without_root_label(void)
{
	static const unsigned variants[] = { 12, 16, 32 };
	static const boot_label_case_t cases[] = {
		{
			.name = "named boot label without root label",
			.boot_label = "FSIMG TEST ", .signature = 0x29,
			.root_label = NULL,
			.status = FSIMG_ECORRUPT
		},
		{
			.name = "blank boot label without root label",
			.boot_label = "           ", .signature = 0x29,
			.root_label = NULL,
			.status = FSIMG_ECORRUPT
		},
		{
			.name = "NO NAME boot label without root label",
			.boot_label = "NO NAME    ", .signature = 0x29,
			.root_label = NULL,
			.status = FSIMG_OK
		},
		{
			.name = "matching boot and root labels",
			.boot_label = "FSIMG TEST ", .signature = 0x29,
			.root_label = "FSIMG TEST ", .status = FSIMG_OK
		},
		{
			.name = "matching NO NAME boot and root labels",
			.boot_label = "NO NAME    ", .signature = 0x29,
			.root_label = "NO NAME    ", .status = FSIMG_OK
		},
		{
			.name = "NO NAME boot label with named root label",
			.boot_label = "NO NAME    ", .signature = 0x29,
			.root_label = "FSIMG TEST ", .status = FSIMG_ECORRUPT
		},
		{
			.name = "named boot bytes without extended signature",
			.boot_label = "FSIMG TEST ", .signature = 0,
			.root_label = NULL,
			.status = FSIMG_OK
		}
	};
	fixture_t f;
	fsimg_verify_report_t report;
	fsimg_error_t error;
	unsigned label_offset, signature_offset;
	fsimg_status_t status;

	for (unsigned v = 0; v < sizeof(variants) / sizeof(variants[0]); v++) {
		/* BS_VolLab and BS_BootSig differ between FAT32 and FAT12/16. */
		label_offset = variants[v] == 32 ? 71 : 43;
		signature_offset = variants[v] == 32 ? 66 : 38;
		for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
			const boot_label_case_t *c = &cases[i];
			const char *message = c->status == FSIMG_OK ? "" :
			    "FAT boot and root volume labels disagree";
			bool complete = c->status == FSIMG_OK;

			fixture_init(&f, variants[v]);
			write_bytes(&f, label_offset, c->boot_label, 11);
			write_bytes(&f, signature_offset, &c->signature, 1);
			/* BPB_BkBootSec selects sector 6 in the fixture. Keep its
			 * copy identical to the primary boot sector.
			 */
			if (variants[v] == 32) {
				write_bytes(&f, 6 * f.sector_bytes + label_offset,
				    c->boot_label, 11);
				write_bytes(&f, 6 * f.sector_bytes + signature_offset,
				    &c->signature, 1);
			}
			if (c->root_label != NULL)
				fixture_add_root_label(&f, c->root_label);
			fixture_open(&f);
			check_fixture_listing(&f, c->name);

			CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader,
			    NULL) == FSIMG_OK);
			bzero(&error, sizeof(error));
			status = fsimg_verify(f.reader, 0, &report, &error);
			if (status != c->status)
				fprintf(stderr, "FAT%u %s: expected status %s, got %s\n",
				    variants[v], c->name, fsimg_status_string(c->status),
				    fsimg_status_string(status));
			CHECK(status == c->status);
			if (strcmp(error.message, message) != 0)
				fprintf(stderr, "FAT%u %s: expected error.message \"%s\", got \"%s\"\n",
				    variants[v], c->name, message, error.message);
			CHECK(strcmp(error.message, message) == 0);
			if (report.complete != complete)
				fprintf(stderr, "FAT%u %s: expected report.complete=%d, got %d\n",
				    variants[v], c->name, complete, report.complete);
			CHECK(report.complete == complete);
			fixture_close(&f);
		}
	}
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
			/* This test only lists; boot/root label agreement is not checked. */
			fixture_add_root_label(&f, "FSIMG TEST ");
			CHECK(pread(f.fd, root, sizeof(root), (off_t)f.root_offset) == sizeof(root));
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
	fsimg_error_t error;
	unsigned char records[1024], raw[32];
	uint16_t name[255];
	size_t length;
	fsimg_status_t status;

	for (unsigned scenario = 0; scenario < 9; scenario++) {
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
		} else if (scenario == 8) {
			/* Two equal 120-character long names overflow the message. */
			length = long_records(records, name, 120, raw);
			short_entry(raw, "LONGNA~2TXT", false, 0, 0);
			long_records(records + length, name, 120, raw);
		}
		write_bytes(&f, f.root_offset, records, sizeof(records));
		fixture_open(&f);
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
		status = fsimg_reader_next(f.reader, &entry, &error);
		if (scenario == 0) {
			CHECK(status == FSIMG_OK && strlen(entry->path) == 256);
			CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_DONE);
		} else if (scenario == 1) {
			/* A collision names the second entry by its path and the
			 * first by its position, so no name enters the message.
			 */
			CHECK(status == FSIMG_ECORRUPT && entry == NULL);
			CHECK(strcmp(error.message, "ambiguous FAT long-name or alias "
			    "namespace: collides with entry 1 of its directory") == 0);
			CHECK(strcmp(error.path, "/HELLO.TXT") == 0);
			CHECK(!error.message_truncated && !error.path_truncated);
		} else if (scenario == 8) {
			CHECK(status == FSIMG_ECORRUPT && entry == NULL);
			CHECK(strcmp(error.message, "ambiguous FAT long-name or alias "
			    "namespace: collides with entry 1 of its directory") == 0);
			CHECK(error.path[0] == '/' && strlen(error.path) == 121 &&
			    strspn(error.path + 1, "a") == 120);
			CHECK(!error.message_truncated && !error.path_truncated);
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
	fsimg_store_t *store;
	unsigned char invalid[2] = { 0xc0, 0x80 };
	static const unsigned char classes[] = { 1, 2 };
	uint64_t cursor;

	/* A record starts with its class byte, after the 8-byte frame, and
	 * then its name. Replay checks both again: an invalid name, and a
	 * stored class other than the one its name has.
	 */
	for (unsigned scenario = 0; scenario < 3; scenario++) {
		fixture_init(&f, 16);
		fixture_open(&f);
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) ==
		    FSIMG_OK);
		CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
		fsimg_test_walk_cursor(f.reader, &store, &cursor);
		CHECK(store != NULL);
		if (scenario == 0)
			CHECK(fsimg_store_write(store, cursor + 9, invalid,
			    sizeof(invalid), NULL) == FSIMG_OK);
		else
			CHECK(fsimg_store_write(store, cursor + 8,
			    &classes[scenario - 1], 1, NULL) == FSIMG_OK);
		CHECK(fsimg_reader_next(f.reader, &entry, NULL) ==
		    FSIMG_ECORRUPT);
		CHECK(entry == NULL);
		fixture_close(&f);
	}
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
	fsimg_sha256_t digest;
	unsigned char data[512];
	char hex[FSIMG_SHA256_DIGEST_STRING_LENGTH];
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
	fsimg_sha256_init(&digest);
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
	fsimg_store_le64(record, 1);
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
		CHECK(fsimg_fat_geometry_decode(boot, sizeof(boot), sectors * 4096, &geometry, NULL) == FSIMG_OK);
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
test_external(const char *path, const char *format, bool expect_unicode)
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
				parent_cluster = fsimg_load_le16(dotdot + 26) |
				    (uint32_t)fsimg_load_le16(dotdot + 20) << 16;
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
	CHECK(status == FSIMG_DONE && files >= (expect_unicode ? 3u : 2u) && directories == 1);
	if (expect_unicode)
		CHECK(saw_unicode);
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

/* Extraction asks for a range and then reads it. Interleaved range queries
 * and sequential reads walk the chain once, with no scratch index.
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

/* Two files share the heap's last cluster. The allocation scan reaches it
 * inside the first sorted claim and never advances past it, so only the
 * final drain reads the second claim and rejects the overlap.
 */
static void
test_trailing_duplicate_claim(void)
{
	fixture_t f;
	fsimg_verify_report_t report;
	fsimg_error_t error;
	char expected[128];
	unsigned char raw[64];
	uint32_t last;

	fixture_init(&f, 16);
	last = f.clusters + 1;
	set_cluster(&f, last, f.eoc);
	short_entry(raw, "LASTA   BIN", false, last, 1);
	short_entry(raw + 32, "LASTB   BIN", false, last, 1);
	write_bytes(&f, f.root_offset + 64, raw, sizeof(raw));
	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_verify(f.reader, 0, &report, &error) == FSIMG_ECORRUPT);
	snprintf(expected, sizeof(expected),
	    "FAT cluster %" PRIu32 " is claimed by more than one owner", last);
	if (strcmp(error.message, expected) != 0)
		fprintf(stderr, "trailing duplicate: expected: %s\nactual: %s\n",
		    expected, error.message);
	CHECK(strcmp(error.message, expected) == 0);
	CHECK(!report.complete);
	fixture_close(&f);
}

/* Like dosfstools' check-chain_to_other_file, two distinct chain prefixes
 * merge into a shared tail: 3->4->7->8 and 5->6->7->8.
 */
static void
test_cross_linked_files_name_shared_clusters(void)
{
	const char expected[] =
	    "FAT clusters 7-8 are claimed by more than one owner";
	fixture_t f;
	fsimg_verify_report_t report;
	fsimg_error_t error;
	unsigned char root[512];

	fixture_init(&f, 16);
	set_cluster(&f, 3, 4);
	set_cluster(&f, 4, 7);
	set_cluster(&f, 5, 6);
	set_cluster(&f, 6, 7);
	set_cluster(&f, 7, 8);
	set_cluster(&f, 8, f.eoc);
	/* Replace HELLO.TXT and SUB; cluster 9 is no longer owned. */
	set_cluster(&f, 9, 0);
	bzero(root, sizeof(root));
	short_entry(root, "FILEA   BIN", false, 3, 4 * f.cluster_bytes);
	short_entry(root + 32, "FILEB   BIN", false, 5, 4 * f.cluster_bytes);
	write_bytes(&f, f.root_offset, root, sizeof(root));

	fixture_open(&f);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_verify(f.reader, 0, &report, &error) == FSIMG_ECORRUPT);
	if (strcmp(error.message, expected) != 0)
		fprintf(stderr, "cross-linked files: expected: %s\nactual: %s\n",
		    expected, error.message);
	CHECK(strcmp(error.message, expected) == 0);
	CHECK(!report.complete);
	fixture_close(&f);
}

/* Verification walks each chain once, fused with its ownership claims and
 * payload reads, and builds no extent index. Structural and payload modes
 * make the same metadata reads; payload mode adds one read per one-cluster
 * fragment, never touching the slack after the file's last byte.
 */
static void
test_fragmented_verify(void)
{
	fixture_t f;
	fsimg_verify_report_t report;
	unsigned char raw[32];
	uint64_t metadata_calls[2];
	const uint32_t fragments = 200;
	const uint32_t size = fragments * 512 - 100;

	for (unsigned payload = 0; payload < 2; payload++) {
		fixture_init(&f, 16);
		make_fragmented_file(&f, fragments);
		short_entry(raw, "INDEX   BIN", false, 20, size);
		write_bytes(&f, f.root_offset, raw, sizeof(raw));
		/* Free the replaced HELLO.TXT's chain. */
		set_cluster(&f, 3, 0);
		set_cluster(&f, 5, 0);
		set_cluster(&f, 9, 0);
		fixture_open(&f);
		/* A one-page cache makes the run collection spill to scratch. */
		CHECK(fsimg_test_meta_cache(f.ctx, 1, NULL) == FSIMG_OK);
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_OK);
		f.calls = f.payload_calls = 0;
		CHECK(fsimg_verify(f.reader, payload != 0 ? FSIMG_VERIFY_PAYLOAD : 0,
		    &report, NULL) == FSIMG_OK);
		CHECK(report.complete && fsimg_fat_state(f.reader)->index == NULL);
		metadata_calls[payload] = f.calls - f.payload_calls;
		if (payload != 0) {
			CHECK(f.payload_calls == fragments);
			CHECK(report.payload_bytes_read == size);
		} else {
			CHECK(f.payload_calls == 0 && report.payload_bytes_read == 0);
		}
		fixture_close(&f);
	}
	CHECK(metadata_calls[0] == metadata_calls[1]);
}

static void
truncated_fixture(fixture_t *f, unsigned variant)
{
	const unsigned sector_bytes = 512, tables = 2;
	const unsigned sectors_per_cluster = variant == 16 ? 8 : 1;
	const bool memory_backing = true;
	/* FAT16 exceeds 65535 sectors; the other variants use builder defaults. */
	const uint32_t clusters = variant == 16 ? 33243 : 0;
	const uint32_t total_sectors = variant == 16 ? 266240 : 0;

	fixture_build(f, variant, sector_bytes, sectors_per_cluster, tables,
	    memory_backing, clusters, total_sectors);
}

static void
truncated_open(fixture_t *f)
{
	fsimg_error_t error = {0};
	fsimg_status_t status;

	status = fsimg_reader_open(f->ctx, f->io, NULL, &f->reader, &error);
	if (status != FSIMG_OK)
		fprintf(stderr, "FAT%u slice=%" PRIu64 ": %s (%s)\n",
		    f->variant, f->bytes, fsimg_status_string(status), error.message);
	CHECK(status == FSIMG_OK);
}

static void
check_truncated_verify(fixture_t *f, uint64_t missing)
{
	fsimg_verify_report_t report;
	fsimg_error_t error;
	uint64_t calls;
	char decimal[32];

	CHECK(snprintf(decimal, sizeof(decimal), "%" PRIu64, missing) > 0);
	for (unsigned payload = 0; payload < 2; payload++) {
		fsimg_reader_free(f->reader);
		f->reader = NULL;
		truncated_open(f);
		calls = f->calls;
		bzero(&error, sizeof(error));
		CHECK(fsimg_verify(f->reader, payload ? FSIMG_VERIFY_PAYLOAD : 0,
		    &report, &error) == FSIMG_ECORRUPT);
		CHECK(strstr(error.message, "truncated") != NULL);
		CHECK(strstr(error.message, decimal) != NULL);
		CHECK(!report.complete && report.files == 0 && report.directories == 0);
		CHECK(report.payload_bytes_read == 0 && f->calls == calls);
	}
}

static void
check_truncated_extract(fixture_t *f)
{
	unsigned char bytes[1101];
	ssize_t count;
	int fd;

	CHECK(fsimg_extract_at(f->reader, f->scratch, "output", NULL, NULL) == FSIMG_OK);
	fd = openat(f->scratch, "output/HELLO.TXT", O_RDONLY | O_NOFOLLOW);
	CHECK(fd != -1);
	count = read(fd, bytes, sizeof(bytes));
	CHECK(count == 1100);
	for (unsigned i = 0; i < 1100; i++)
		CHECK(bytes[i] == (unsigned char)(i * 37));
	CHECK(close(fd) == 0);
	CHECK(unlinkat(f->scratch, "output/HELLO.TXT", 0) == 0);
	CHECK(unlinkat(f->scratch, "output/SUB/EMPTY", 0) == 0);
	CHECK(unlinkat(f->scratch, "output/SUB", AT_REMOVEDIR) == 0);
	CHECK(unlinkat(f->scratch, "output", AT_REMOVEDIR) == 0);
}

static void
check_truncated_tail(fixture_t *f, uint64_t declared)
{
	fsimg_volume_info_t info;

	fixture_open(f);
	truncated_open(f);
	CHECK(fsimg_reader_get_info(f->reader, &info, NULL) == FSIMG_OK);
	CHECK(info.volume_bytes == declared);
	CHECK(info.missing_bytes == declared - f->bytes);
	{
		fat_geometry_t geometry;

		CHECK(fsimg_fat_geometry_decode(f->memory, 512, f->bytes,
		    &geometry, NULL) == FSIMG_OK);
		CHECK(geometry.variant == f->variant && geometry.clusters == f->clusters);
		CHECK(geometry.volume_bytes == declared);
	}
	fsimg_reader_free(f->reader);
	f->reader = NULL;
	check_fixture_listing(f, "truncated free tail");
	truncated_open(f);
	check_truncated_extract(f);
	check_truncated_verify(f, declared - f->bytes);
}

/* A table-heavy prefix has many more addressable clusters than stored ones.
 * Alternate low and high cluster numbers so absent tails cannot coalesce.
 * Directory chains stay below FAT's two-MiB directory limit individually.
 */
static void
truncated_resource_fixture(fixture_t *f, bool directory)
{
	const uint32_t clusters = 262142, directories = 64, chain_length = 4090;
	unsigned char *root, *raw, *memory;
	size_t stored;
	uint32_t first, low, high, count, root_clusters;

	fixture_build(f, 32, 512, 1, 1, true, clusters, 0);
	stored = (size_t)f->data_offset + 256 * 512;
	memory = realloc(f->memory, stored);
	CHECK(memory != NULL);
	f->memory = memory;
	f->stored = stored;
	bzero(f->memory + f->data_offset, stored - (size_t)f->data_offset);
	bzero(f->memory + f->fat_offset, (size_t)f->table_bytes);
	set_cluster(f, 0, 0x0ffffff8);
	set_cluster(f, 1, f->eoc);
	root_clusters = directory ? directories / 16 : 1;
	for (uint32_t c = 2; c < 2 + root_clusters; c++)
		set_cluster(f, c, c + 1 < 2 + root_clusters ? c + 1 : f->eoc);
	first = 2 + root_clusters;
	count = directory ? directories : 1;
	low = first + count;
	high = clusters + 1;
	root = f->memory + f->root_offset;
	for (uint32_t d = 0; d < count; d++) {
		char name[12];
		uint32_t c = first + d;
		uint32_t length = directory ? chain_length : clusters - root_clusters;

		CHECK(snprintf(name, sizeof(name), "F%07uBIN", d) == 11);
		short_entry(root + d * 32, name, directory, c,
		    directory ? 0 : length * 512);
		if (directory) {
			raw = f->memory + cluster_offset(f, c);
			short_entry(raw, ".          ", true, c, 0);
			short_entry(raw + 32, "..         ", true, 0, 0);
		}
		for (uint32_t k = 1; k < length; k++) {
			uint32_t next = (k & 1) != 0 ? high-- : low++;

			set_cluster(f, c, next);
			c = next;
		}
		set_cluster(f, c, f->eoc);
	}
	f->bytes = f->stored;
}

static void
check_truncated_chain_resource(bool directory, bool limited)
{
	fixture_t f;
	fsimg_options_t options;
	fsimg_io_ops_t ops;
	fsimg_stats_t before, after;
	fsimg_error_t error = {0};
	const fsimg_entry_t *entry;
	fsimg_status_t status, listing_status;
	uint64_t stored, work_limit, storage_bound;
	unsigned entries = 0;
	unsigned char byte;
	size_t count;

	truncated_resource_fixture(&f, directory);
	stored = (f.bytes - f.data_offset) / 512;
	/* Work scales with file bytes, including its table. Storage allows one
	 * extent per surviving cluster, the directory bitset, and 64 pages for
	 * reader state, store headers, page rounding and directory sorting.
	 */
	work_limit = f.bytes / 512 * 8;
	storage_bound = stored * 32 + (f.clusters + 9) / 8 + 64 * 4096;
	fsimg_options_init(&options);
	options.scratch_directory_fd = f.scratch;
	options.max_work = limited ? work_limit : 0;
	CHECK(fsimg_ctx_new(&options, &f.ctx, NULL) == FSIMG_OK);
	/* A small shared cache makes the storage charge include spilled pages. */
	CHECK(fsimg_test_meta_cache(f.ctx, 4, NULL) == FSIMG_OK);
	fsimg_io_ops_init(&ops);
	ops.read_at = image_read;
	ops.get_size = image_size;
	CHECK(fsimg_io_from_callbacks(f.ctx, &ops, &f, &f.io, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(f.ctx, &before, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, &error) == FSIMG_OK);
	while ((status = fsimg_reader_next(f.reader, &entry, &error)) == FSIMG_OK)
		entries++;
	if (status != FSIMG_DONE)
		fprintf(stderr, "FAT resource listing: %s (limit=%" PRIu64 ")\n",
		    error.message, work_limit);
	listing_status = status;
	if (status == FSIMG_DONE && !directory) {
		fsimg_reader_free(f.reader);
		f.reader = NULL;
		truncated_open(&f);
		CHECK(fsimg_reader_next(f.reader, &entry, &error) == FSIMG_OK);
		status = fsimg_reader_read_at(f.reader, 512, &byte, 1, &count, &error);
		if (status != FSIMG_ECORRUPT)
			fprintf(stderr, "FAT resource seek: %s\n", error.message);
		CHECK(status == FSIMG_ECORRUPT);
		CHECK(count == 0 && strstr(error.message, "truncated") != NULL);
	}
	CHECK(fsimg_ctx_get_stats(f.ctx, &after, NULL) == FSIMG_OK);
	if (!limited) {
		uint64_t growth = after.scratch_peak - before.scratch_peak +
		    after.memory_peak - before.memory_peak;

		if (growth >= storage_bound)
			fprintf(stderr, "FAT storage growth=%" PRIu64 ", bound=%" PRIu64 "\n",
			    growth, storage_bound);
		CHECK(growth < storage_bound);
	}
	CHECK(listing_status == FSIMG_DONE && entries == (directory ? 64u : 1u));
	fixture_close(&f);
}

static void
test_truncated_directory_storage_bound(void)
{
	check_truncated_chain_resource(true, false);
}

static void
test_truncated_directory_work_bound(void)
{
	check_truncated_chain_resource(true, true);
}

static void
test_truncated_file_storage_bound(void)
{
	check_truncated_chain_resource(false, false);
}

static void
test_truncated_file_work_bound(void)
{
	check_truncated_chain_resource(false, true);
}

/* A contiguous run ends before the first wholly missing cluster, even
 * when the FAT continues through more adjacent clusters.
 */
static void
test_truncated_contiguous_run(void)
{
	fixture_t f;
	fat_chain_t chain;
	fsimg_extent_t run;

	truncated_fixture(&f, 12);
	set_cluster(&f, 3, 4);
	set_cluster(&f, 4, 5);
	set_cluster(&f, 5, f.eoc);
	f.bytes = cluster_offset(&f, 4);
	fixture_open(&f);
	truncated_open(&f);
	fsimg_fat_chain_init(&chain, 3, 3);
	CHECK(fsimg_fat_chain_run(f.reader, &chain, &run, NULL) == FSIMG_OK);
	CHECK(run.image_offset == cluster_offset(&f, 3) && run.length == 512);
	fixture_close(&f);
}

/* A missing cluster can link back to stored data. Neither a sequential
 * run nor a seek may expose those later bytes, even if the file size fits.
 * A short chain must report truncation rather than judging its length.
 */
static void
check_truncated_stopped_file(bool contiguous, bool short_chain, uint64_t offset)
{
	fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_error_t error = {0};
	unsigned char byte = 0xa5;
	uint32_t missing = contiguous ? 4 : 10;
	size_t count;

	truncated_fixture(&f, 12);
	short_entry(f.memory + f.root_offset, "HELLO   TXT", false, 3, 1536);
	set_cluster(&f, 3, missing);
	set_cluster(&f, missing, short_chain ? f.eoc : 5);
	set_cluster(&f, 5, f.eoc);
	f.bytes = cluster_offset(&f, missing);
	fixture_open(&f);
	truncated_open(&f);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	if (contiguous) {
		/* Start sequential delivery before crossing the contiguous EOF. */
		CHECK(fsimg_reader_read_at(f.reader, 0, &byte, 1, &count, NULL) == FSIMG_OK);
		CHECK(count == 1 && byte == 0);
		byte = 0xa5;
	}
	CHECK(fsimg_reader_read_at(f.reader, offset, &byte, 1, &count,
	    &error) == FSIMG_ECORRUPT);
	CHECK(count == 0 && byte == 0xa5);
	CHECK(strstr(error.message, "truncated") != NULL);
	fixture_close(&f);
}

static void
test_truncated_contiguous_file_read(void)
{
	check_truncated_stopped_file(true, false, 512);
}

static void
test_truncated_file_seek_after_stop(void)
{
	check_truncated_stopped_file(false, false, 1024);
}

static void
test_truncated_file_length_after_stop(void)
{
	check_truncated_stopped_file(false, true, 1024);
}

/* A directory end marker permits listing without following an absent run.
 * Without that marker the next slot must fail as truncated. A contiguous
 * run must end at the same boundary rather than reading beyond the image.
 */
static void
check_truncated_directory_stop(bool marker, bool contiguous)
{
	fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_error_t error = {0};
	fsimg_status_t status;
	unsigned entries = 0;

	truncated_fixture(&f, 12);
	set_cluster(&f, 7, contiguous ? 8 : 10);
	set_cluster(&f, contiguous ? 8 : 10, 0);
	f.bytes = cluster_offset(&f, contiguous ? 8 : 10);
	/* Remove the unrelated payload from this directory-only fixture. */
	short_entry(f.memory + f.root_offset, "GOOD    BIN", false, 3, 4);
	set_cluster(&f, 3, f.eoc);
	bcopy("good", f.memory + cluster_offset(&f, 3), 4);
	if (!marker) {
		for (unsigned slot = 3; slot < 16; slot++)
			f.memory[cluster_offset(&f, 7) + slot * 32] = 0xe5;
	}
	fixture_open(&f);
	truncated_open(&f);
	while ((status = fsimg_reader_next(f.reader, &entry, &error)) == FSIMG_OK) {
		entries++;
		if (strcmp(entry->path, "/GOOD.BIN") == 0) {
			unsigned char bytes[4];
			size_t count;

			CHECK(fsimg_reader_read_at(f.reader, 0, bytes, 4, &count, NULL) == FSIMG_OK);
			CHECK(count == 4 && bcmp(bytes, "good", 4) == 0);
		}
	}
	CHECK(status == (marker ? FSIMG_DONE : FSIMG_ECORRUPT));
	if (marker)
		CHECK(entries == 3);
	else
		CHECK(strstr(error.message, "truncated") != NULL);
	fixture_close(&f);
}

static void
test_truncated_directory_partial_cluster(void)
{
	fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_error_t error = {0};
	fsimg_status_t status;

	truncated_fixture(&f, 12);
	/* Clusters 7 and 8 coalesce into a 1024-byte run. The first 4 KiB
	 * batch includes both the marker in cluster 7 and the cut in cluster 8.
	 */
	set_cluster(&f, 7, 8);
	set_cluster(&f, 8, f.eoc);
	f.bytes = cluster_offset(&f, 8) + 100;
	short_entry(f.memory + f.root_offset, "GOOD    BIN", false, 3, 4);
	set_cluster(&f, 3, f.eoc);
	bcopy("good", f.memory + cluster_offset(&f, 3), 4);
	fixture_open(&f);
	truncated_open(&f);
	while ((status = fsimg_reader_next(f.reader, &entry, &error)) == FSIMG_OK)
		continue;
	CHECK(status == FSIMG_ECORRUPT);
	fixture_close(&f);
}

static void
test_truncated_directory_end_marker(void)
{
	check_truncated_directory_stop(true, false);
}

static void
test_truncated_contiguous_directory_end_marker(void)
{
	check_truncated_directory_stop(true, true);
}

static void
test_truncated_directory_slots_after_stop(void)
{
	check_truncated_directory_stop(false, false);
}

/* The same fragmented allocation is valid when complete. In a short view,
 * its surviving prefix and independent sibling remain readable, but a link
 * through missing storage must never return to its stored final cluster.
 */
static void
check_truncated_chain_control(bool complete)
{
	fixture_t f;
	fsimg_verify_report_t report;
	const fsimg_entry_t *entry;
	fsimg_status_t status;
	unsigned char bytes[1536];
	unsigned entries = 0;
	size_t count;

	truncated_fixture(&f, 12);
	if (complete) {
		unsigned char *memory = calloc(1, (size_t)f.bytes);

		CHECK(memory != NULL);
		bcopy(f.memory, memory, f.stored);
		free(f.memory);
		f.memory = memory;
		f.stored = (size_t)f.bytes;
	}
	short_entry(f.memory + f.root_offset, "HELLO   TXT", false, 3, 1536);
	short_entry(f.memory + f.root_offset + 32, "SIBLING BIN", false, 4, 4);
	set_cluster(&f, 3, 10);
	set_cluster(&f, 10, 5);
	set_cluster(&f, 5, f.eoc);
	set_cluster(&f, 4, f.eoc);
	set_cluster(&f, 7, 0);
	set_cluster(&f, 9, 0);
	bcopy("good", f.memory + cluster_offset(&f, 4), 4);
	for (unsigned part = 0; part < 3; part++) {
		const uint32_t chain[] = { 3, 10, 5 };

		memset(f.memory + cluster_offset(&f, chain[part]), (int)('a' + part), 512);
	}
	if (!complete)
		f.bytes = cluster_offset(&f, 10);
	fixture_open(&f);
	truncated_open(&f);
	while ((status = fsimg_reader_next(f.reader, &entry, NULL)) == FSIMG_OK) {
		size_t length = entry->size == 4 ? 4 : sizeof(bytes);

		if (!complete && length > 512)
			length = 512;
		entries++;
		CHECK(fsimg_reader_read_at(f.reader, 0, bytes,
		    length, &count, NULL) == FSIMG_OK);
		if (entry->size == 4)
			CHECK(count == 4 && bcmp(bytes, "good", 4) == 0);
		else {
			CHECK(count == (complete ? 1536u : 512u));
			for (size_t i = 0; i < count; i++)
				CHECK(bytes[i] == (unsigned char)('a' + i / 512));
		}
	}
	CHECK(status == FSIMG_DONE && entries == 2);
	if (complete) {
		fsimg_reader_free(f.reader);
		f.reader = NULL;
		truncated_open(&f);
		CHECK(fsimg_verify(f.reader, FSIMG_VERIFY_PAYLOAD, &report, NULL) == FSIMG_OK);
		CHECK(report.complete && report.files == 2);
	}
	fixture_close(&f);
}

static void
test_truncated_chain_surviving_sibling(void)
{
	check_truncated_chain_control(false);
}

static void
test_complete_chain_control(void)
{
	check_truncated_chain_control(true);
}

static void
test_truncated_tail(unsigned variant)
{
	fixture_t f;
	uint64_t declared;
	const uint32_t first_free_cluster = variant == 16 ? 8 : 10;

	truncated_fixture(&f, variant);
	declared = f.bytes;
	if (variant == 16)
		CHECK(declared == UINT64_C(136314880));
	f.bytes = cluster_offset(&f, first_free_cluster);
	check_truncated_tail(&f, declared);
	fixture_close(&f);
}

static void
test_truncated_file_slack(void)
{
	fixture_t f;
	uint64_t declared;

	truncated_fixture(&f, 12);
	declared = f.bytes;
	/* Only the last cluster's unused slack is missing. */
	set_cluster(&f, 3, 5);
	set_cluster(&f, 5, 9);
	set_cluster(&f, 9, f.eoc);
	for (unsigned i = 0; i < 512; i++)
		f.memory[cluster_offset(&f, 5) + i] = (unsigned char)((512 + i) * 37);
	for (unsigned i = 0; i < 76; i++)
		f.memory[cluster_offset(&f, 9) + i] = (unsigned char)((1024 + i) * 37);
	f.bytes = cluster_offset(&f, 9) + 76;
	check_truncated_tail(&f, declared);
	fixture_close(&f);
}

static void
test_truncated_file_reference(void)
{
	fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_error_t error = {0};
	unsigned char bytes[1100];
	size_t count;

	truncated_fixture(&f, 12);
	f.bytes = cluster_offset(&f, 9);
	fixture_open(&f);
	check_fixture_listing(&f, "missing file payload does not affect listing");
	truncated_open(&f);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(strcmp(entry->path, "/HELLO.TXT") == 0);
	CHECK(fsimg_reader_read_at(f.reader, 0, bytes, sizeof(bytes),
	    &count, &error) == FSIMG_ECORRUPT);
	CHECK(count < sizeof(bytes));
	CHECK(strstr(error.message, "truncated") != NULL);
	fsimg_reader_free(f.reader);
	f.reader = NULL;
	truncated_open(&f);
	CHECK(fsimg_extract_at(f.reader, f.scratch, "output", NULL,
	    &error) == FSIMG_ECORRUPT);
	CHECK(strstr(error.message, "truncated") != NULL);
	if (unlinkat(f.scratch, "output/HELLO.TXT", 0) != 0)
		CHECK(errno == ENOENT);
	CHECK(unlinkat(f.scratch, "output", AT_REMOVEDIR) == 0);
	fixture_close(&f);
}

static void
test_truncated_directory_reference(void)
{
	fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_error_t error = {0};
	fsimg_status_t status;

	truncated_fixture(&f, 12);
	short_entry(f.memory + f.root_offset + 32, "SUB        ", true, 11, 0);
	set_cluster(&f, 11, f.eoc);
	f.bytes = cluster_offset(&f, 10);
	fixture_open(&f);
	truncated_open(&f);
	do {
		status = fsimg_reader_next(f.reader, &entry, &error);
	} while (status == FSIMG_OK);
	CHECK(status == FSIMG_ECORRUPT);
	CHECK(strstr(error.message, "truncated") != NULL);
	fixture_close(&f);
}

typedef enum open_mode { DETECT, NAMED, ASSUMED } open_mode_t;
typedef enum metadata_boundary {
	FIRST_TABLE, SECOND_TABLE, FIXED_ROOT, METADATA_END, DATA_START
} metadata_boundary_t;
typedef enum metadata_result { FAIL_OPEN, FAIL_WALK, OPEN_OK } metadata_result_t;
typedef struct metadata_cut {
	const char *name;
	metadata_boundary_t boundary;
	bool fixed_root_only;
	bool fat32_only;
	metadata_result_t result;
} metadata_cut_t;

static void
check_metadata_cut(unsigned variant, const metadata_cut_t *cut, open_mode_t mode)
{
	fixture_t f;
	fsimg_read_options_t options;
	fsimg_volume_info_t info;
	fsimg_status_t status, expected;
	const fsimg_entry_t *entry;
	const char *format, *mode_name;

	truncated_fixture(&f, variant);
	switch (cut->boundary) {
	case FIRST_TABLE:
		f.bytes = f.fat_offset + 1;
		break;
	case SECOND_TABLE:
		f.bytes = f.fat_offset + f.table_bytes + 1;
		break;
	case FIXED_ROOT:
		f.bytes = f.root_offset + 1;
		break;
	case METADATA_END:
		f.bytes = f.data_offset - 1;
		break;
	case DATA_START:
		f.bytes = f.data_offset;
		break;
	}
	switch (variant) {
	case 12: format = "fat12"; break;
	case 16: format = "fat16"; break;
	default: format = "fat32"; break;
	}
	switch (mode) {
	case DETECT: mode_name = "detect"; break;
	case NAMED: mode_name = "named"; break;
	default: mode_name = "assumed"; break;
	}
	fixture_open(&f);
	fsimg_read_options_init(&options);
	if (mode != DETECT)
		options.format = format;
	if (mode == ASSUMED)
		options.flags = FSIMG_READ_ASSUME_IDENTITY;
	status = fsimg_reader_open(f.ctx, f.io, &options, &f.reader, NULL);
	expected = cut->result == FAIL_OPEN ? FSIMG_ECORRUPT : FSIMG_OK;
	if (status != expected)
		fprintf(stderr, "FAT%u cut=%s mode=%s open: expected %d, got %d\n",
		    variant, cut->name, mode_name, expected, status);
	CHECK(status == expected);
	if (cut->result == FAIL_OPEN)
		CHECK(f.reader == NULL);
	else if (cut->result == FAIL_WALK) {
		status = fsimg_reader_next(f.reader, &entry, NULL);
		if (status != FSIMG_ECORRUPT)
			fprintf(stderr, "FAT%u cut=%s mode=%s walk: expected %d, got %d\n",
			    variant, cut->name, mode_name, FSIMG_ECORRUPT, status);
		CHECK(status == FSIMG_ECORRUPT);
	} else {
		status = fsimg_reader_get_info(f.reader, &info, NULL);
		if (status != FSIMG_OK)
			fprintf(stderr, "FAT%u cut=%s mode=%s info: expected %d, got %d\n",
			    variant, cut->name, mode_name, FSIMG_OK, status);
		CHECK(status == FSIMG_OK);
		if (info.missing_bytes != info.volume_bytes - f.bytes)
			fprintf(stderr, "FAT%u cut=%s mode=%s missing bytes: expected %" PRIu64 ", got %" PRIu64 "\n",
			    variant, cut->name, mode_name,
			    info.volume_bytes - f.bytes, info.missing_bytes);
		CHECK(info.missing_bytes == info.volume_bytes - f.bytes);
	}
	fixture_close(&f);
}

static void
test_truncated_metadata(void)
{
	const unsigned variants[] = {12, 16, 32};
	const open_mode_t modes[] = {DETECT, NAMED, ASSUMED};
	const metadata_cut_t cuts[] = {
		{.name = "first table", .boundary = FIRST_TABLE, .result = FAIL_OPEN},
		{.name = "second table", .boundary = SECOND_TABLE, .result = FAIL_OPEN},
		{.name = "fixed root", .boundary = FIXED_ROOT,
		 .fixed_root_only = true, .result = FAIL_OPEN},
		{.name = "metadata end", .boundary = METADATA_END, .result = FAIL_OPEN},
		{.name = "data start", .boundary = DATA_START,
		 .fixed_root_only = true, .result = OPEN_OK},
		{.name = "data start", .boundary = DATA_START,
		 .fat32_only = true, .result = FAIL_WALK}
	};

	for (unsigned v = 0; v < sizeof(variants) / sizeof(variants[0]); v++) {
		unsigned variant = variants[v];

		for (unsigned c = 0; c < sizeof(cuts) / sizeof(cuts[0]); c++) {
			const metadata_cut_t *cut = &cuts[c];

			if ((cut->fixed_root_only && variant == 32) ||
			    (cut->fat32_only && variant != 32))
				continue;
			for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); m++)
				check_metadata_cut(variant, cut, modes[m]);
		}
	}
}

static void
test_whole_and_longer_slices(void)
{
	fixture_t f;
	fsimg_volume_info_t info;
	uint64_t declared;

	for (unsigned longer = 0; longer < 2; longer++) {
		truncated_fixture(&f, 12);
		declared = f.bytes;
		f.bytes += longer * 123;
		fixture_open(&f);
		truncated_open(&f);
		CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
		CHECK(info.volume_bytes == declared && info.missing_bytes == 0);
		for (unsigned payload = 0; payload < 2; payload++) {
			fsimg_verify_report_t report;

			CHECK(fsimg_verify(f.reader, payload ? FSIMG_VERIFY_PAYLOAD : 0,
			    &report, NULL) == FSIMG_OK);
			CHECK(report.complete);
			fsimg_reader_free(f.reader);
			f.reader = NULL;
			truncated_open(&f);
		}

		fixture_close(&f);
	}
}

static void
test_reader_byte_bounds(void)
{
	fixture_t f;
	fsimg_error_t error = {0};
	fsimg_status_t status;
	uint64_t calls;
	unsigned char bytes[2];
	size_t count;

	truncated_fixture(&f, 12);
	fixture_open(&f);
	truncated_open(&f);
	for (unsigned overflow = 0; overflow < 2; overflow++) {
		calls = f.calls;
		bytes[0] = bytes[1] = 0xa5;
		count = 99;
		status = fsimg_reader_bytes(f.reader,
		    overflow ? UINT64_MAX : f.bytes - 1, bytes,
		    sizeof(bytes), &count, &error);
		if (status != FSIMG_ECORRUPT)
			fprintf(stderr, "read beyond slice: %s (%s), count=%zu\n",
			    fsimg_status_string(status), error.message, count);
		CHECK(status == FSIMG_ECORRUPT);
		CHECK(strcmp(error.message, "image is truncated") == 0);
		CHECK(count == 0 && f.calls == calls);
		CHECK(bytes[0] == 0xa5 && bytes[1] == 0xa5);
	}
	fixture_close(&f);
}

typedef enum truncated_operation {
	TRUNCATED_READ,
	TRUNCATED_VERIFY,
	TRUNCATED_VERIFY_PAYLOAD
} truncated_operation_t;

static bool
truncated_status_allowed(fsimg_status_t status, uint64_t slice)
{
	return (status == FSIMG_OK || status == FSIMG_ECORRUPT ||
	    (slice < 512 && status == FSIMG_EFORMAT));
}

static fsimg_status_t
read_truncated_files(fsimg_reader_t *reader)
{
	const fsimg_entry_t *entry;
	fsimg_status_t status;
	unsigned char bytes[1100];
	size_t count;

	while ((status = fsimg_reader_next(reader, &entry, NULL)) == FSIMG_OK) {
		if (entry->kind != FSIMG_ENTRY_FILE)
			continue;
		status = fsimg_reader_read_at(reader, 0, bytes, sizeof(bytes), &count, NULL);
		CHECK(status == FSIMG_OK || status == FSIMG_ECORRUPT);
		if (status != FSIMG_OK)
			return (status);
	}
	return (status == FSIMG_DONE ? FSIMG_OK : status);
}

static void
check_truncated_cut(fixture_t *f, uint64_t whole)
{
	const truncated_operation_t operations[] = {
		TRUNCATED_READ, TRUNCATED_VERIFY, TRUNCATED_VERIFY_PAYLOAD
	};
	fsimg_verify_report_t report;
	fsimg_status_t status;

	fixture_open(f);
	for (unsigned i = 0; i < sizeof(operations) / sizeof(operations[0]); i++) {
		truncated_operation_t operation = operations[i];

		status = fsimg_reader_open(f->ctx, f->io, NULL, &f->reader, NULL);
		if (f->bytes == whole)
			CHECK(status == FSIMG_OK);
		if (status == FSIMG_OK) {
			if (operation == TRUNCATED_READ)
				status = read_truncated_files(f->reader);
			else {
				status = fsimg_verify(f->reader,
				    operation == TRUNCATED_VERIFY_PAYLOAD ? FSIMG_VERIFY_PAYLOAD : 0,
				    &report, NULL);
				CHECK(status == (f->bytes == whole ? FSIMG_OK : FSIMG_ECORRUPT));
			}
		}
		if (!truncated_status_allowed(status, f->bytes))
			fprintf(stderr, "FAT sweep cut=%" PRIu64 " operation=%u status=%d\n",
			    f->bytes, operation, status);
		CHECK(truncated_status_allowed(status, f->bytes));
		fsimg_reader_free(f->reader);
		f->reader = NULL;
	}
	fsimg_io_free(f->io);
	f->io = NULL;
	fsimg_ctx_free(f->ctx);
	f->ctx = NULL;
}

static void
test_truncated_sweep(void)
{
	fixture_t f;
	uint64_t whole;

	truncated_fixture(&f, 12);
	whole = f.data_offset + 10 * f.cluster_bytes;
	put16(f.memory + BPB_TOTAL_SECTORS16, (uint16_t)(whole / 512));
	for (uint64_t cut = 0; cut <= whole; cut += 512) {
		f.bytes = cut;
		check_truncated_cut(&f, whole);
		if (cut < whole) {
			f.bytes = cut + 257;
			check_truncated_cut(&f, whole);
		}
	}
	fixture_close(&f);
}

static void
huge_geometry_fixture(fixture_t *f, unsigned sectors_per_cluster)
{
	const unsigned variant = 32, sector_bytes = 512, tables = 2;
	const uint32_t clusters = 70014; /* Full capacity of a 547-sector FAT32. */
	const uint32_t default_total_sectors = 0;
	const bool memory_backing = true;

	fixture_build(f, variant, sector_bytes, sectors_per_cluster, tables,
	    memory_backing, clusters, default_total_sectors);
}

static void
test_truncated_memory_cost_independent_of_declared_size(void)
{
	fixture_t f;
	fsimg_stats_t stats[2];
	fsimg_volume_info_t info;
	const fsimg_entry_t *entry;
	fsimg_error_t error = {0};
	uint64_t declared[2];
	const unsigned cluster_sectors[] = {1, 128};

	/* Equal tables and cluster counts isolate the cost of the declared size. */
	for (unsigned i = 0; i < 2; i++) {
		huge_geometry_fixture(&f, cluster_sectors[i]);
		declared[i] = f.bytes;
		f.bytes = f.data_offset; /* Both tables present, all data omitted. */
		CHECK(f.table_bytes == 547 * f.sector_bytes);
		fixture_open(&f);
		truncated_open(&f);
		CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
		CHECK(info.volume_bytes == declared[i]);
		CHECK(info.missing_bytes == declared[i] - f.bytes);
		/* Walking allocates the directory bitset before reading the root. */
		CHECK(fsimg_reader_next(f.reader, &entry, &error) == FSIMG_ECORRUPT);
		CHECK(strstr(error.message, "truncated") != NULL);
		CHECK(fsimg_ctx_get_stats(f.ctx, &stats[i], NULL) == FSIMG_OK);
		check_force_truncated_volume(f.ctx, f.io, "fat32");
		fixture_close(&f);
	}
	CHECK(declared[1] > UINT64_C(4000000000));
	CHECK(stats[1].memory_peak <= stats[0].memory_peak);
	CHECK(stats[1].scratch_peak <= stats[0].scratch_peak);
}

static void
test_truncated_one_extra_cluster_refused(void)
{
	fixture_t f;
	const unsigned cluster_sectors[] = {1, 128};
	uint64_t declared;

	for (unsigned i = 0; i < 2; i++) {
		huge_geometry_fixture(&f, cluster_sectors[i]);
		declared = f.bytes;
		f.bytes = f.data_offset;
		/* One more data cluster cannot fit in the allocation tables. */
		put32(f.memory + BPB_TOTAL_SECTORS32,
		    (uint32_t)(declared / f.sector_bytes) + cluster_sectors[i]);
		fixture_open(&f);
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_ECORRUPT);
		CHECK(f.reader == NULL);
		fixture_close(&f);
	}
}

static void
test_truncated_absent_tables_refused(void)
{
	fixture_t f;
	const unsigned cluster_sectors[] = {1, 128};
	const uint32_t declared_table_sectors = 32768;

	for (unsigned i = 0; i < 2; i++) {
		huge_geometry_fixture(&f, cluster_sectors[i]);
		/* An enormous declaration does not excuse absent tables. */
		f.bytes = f.sector_bytes;
		put32(f.memory + BPB_TOTAL_SECTORS32, UINT32_MAX - 1);
		put32(f.memory + BPB_FAT_SECTORS32, declared_table_sectors);
		fixture_open(&f);
		CHECK(fsimg_reader_open(f.ctx, f.io, NULL, &f.reader, NULL) == FSIMG_ECORRUPT);
		CHECK(f.reader == NULL);
		fixture_close(&f);
	}
}

static void
test_truncated_assumed_identity_verify(void)
{
	fixture_t f;
	fsimg_read_options_t options;
	fsimg_verify_report_t report;
	fsimg_error_t error = {0};

	truncated_fixture(&f, 12);
	fixture_assume_identity(&f);
	f.bytes = cluster_offset(&f, 10);
	fixture_open(&f);
	fsimg_read_options_init(&options);
	options.format = "fat12";
	options.flags = FSIMG_READ_ASSUME_IDENTITY;
	CHECK(fsimg_reader_open(f.ctx, f.io, &options, &f.reader, &error) == FSIMG_OK);
	CHECK(fsimg_verify(f.reader, 0, &report, &error) == FSIMG_ECORRUPT);
	if (strstr(error.message, "truncated") == NULL)
		fprintf(stderr, "truncated assumed-identity verify: %s\n", error.message);
	fixture_close(&f);
	CHECK(strstr(error.message, "truncated") != NULL);
	CHECK(!report.complete);
}

static void
test_truncated_extract_output_budget(void)
{
	fixture_t f;
	fsimg_extract_options_t options;
	fsimg_error_t error = {0};
	fsimg_status_t status;
	const uint32_t file_bytes = 3000000;
	uint32_t file_clusters;

	truncated_fixture(&f, 16);
	short_entry(f.memory + f.root_offset, "A       BIN", false, 3, file_bytes);
	file_clusters = (file_bytes + f.cluster_bytes - 1) / f.cluster_bytes;
	for (uint32_t cluster = 3; cluster < 3 + file_clusters; cluster++)
		set_cluster(&f, cluster, cluster + 1 < 3 + file_clusters ? cluster + 1 : f.eoc);
	bzero(f.memory + f.root_offset + 32, 32);
	f.bytes = 2000000;
	fixture_open(&f);
	truncated_open(&f);
	fsimg_extract_options_init(&options);
	options.max_output_bytes = f.bytes;
	status = fsimg_extract_at(f.reader, f.scratch, "output", &options, &error);
	if (status != FSIMG_ECORRUPT)
		fprintf(stderr, "truncated extraction budget: %s (%s)\n",
		    fsimg_status_string(status), error.message);
	CHECK(unlinkat(f.scratch, "output", AT_REMOVEDIR) == 0);
	fixture_close(&f);
	CHECK(status == FSIMG_ECORRUPT);
	CHECK(strstr(error.message, "truncated") != NULL);
}

static void
test_truncated_present_file_source_change_outranks_output_limit(void)
{
	fixture_t f;
	fsimg_options_t options;
	fsimg_volume_info_t info;
	const fsimg_entry_t *entry;
	fsimg_error_t error = {0};
	fsimg_status_t status;
	const uint64_t output_limit = 1000;
	const struct timespec changed[2] = {
		{ .tv_nsec = UTIME_OMIT }, { .tv_sec = 1 }
	};
	char path[] = "fat-charge-host-XXXXXX";
	int output, readonly;

	truncated_fixture(&f, 16);
	f.bytes = cluster_offset(&f, 8);
	output = mkstemp(path);
	CHECK(output != -1);
	CHECK(write(output, f.memory, (size_t)f.bytes) == (ssize_t)f.bytes);
	readonly = open(path, O_RDONLY | O_CLOEXEC);
	CHECK(readonly != -1 && unlink(path) == 0);
	fsimg_options_init(&options);
	options.scratch_directory_fd = f.scratch;
	options.max_output_bytes = output_limit;
	CHECK(fsimg_ctx_new(&options, &f.ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_io_from_fd(f.ctx, readonly, FSIMG_IO_READ, &f.io, NULL) == FSIMG_OK);
	truncated_open(&f);
	CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
	CHECK(info.missing_bytes > 0);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(entry->kind == FSIMG_ENTRY_FILE);
	CHECK(strcmp(entry->path, "/HELLO.TXT") == 0);
	CHECK(entry->size > output_limit);
	CHECK(cluster_offset(&f, 3) + entry->size <= f.bytes);

	/* Change the payload and force a distinct identity on coarse clocks. */
	CHECK(pwrite(output, "X", 1, (off_t)cluster_offset(&f, 3)) == 1);
	CHECK(futimens(output, changed) == 0);
	status = fsimg_reader_charge_file(f.reader, &error);
	if (status != FSIMG_ESOURCE_CHANGED)
		fprintf(stderr, "truncated present file with changed host and small output budget: expected FSIMG_ESOURCE_CHANGED, got %s (%d): %s\n",
		    fsimg_status_string(status), status, error.message);

	fixture_close(&f);
	CHECK(close(readonly) == 0 && close(output) == 0);
	CHECK(status == FSIMG_ESOURCE_CHANGED);
}

/* Three 128 KiB files exceed the CLI's default budget for a 300000-byte slice.
 * The first two are present; the third extends past the slice. Only metadata
 * needs writing; the CLI fixture helper extends and truncates the slice.
 */
static void
write_output_budget_seed(const char *path)
{
	fixture_t f;
	const unsigned variant = 16, sector_bytes = 512, tables = 2;
	const unsigned sectors_per_cluster = 1;
	const bool memory_backing = true;
	const uint32_t file_bytes = 128 * 1024;
	const uint32_t default_clusters = 0, default_total_sectors = 0;
	const char *names[] = {"A       BIN", "B       BIN", "C       BIN"};
	uint32_t file_clusters, first;
	int fd;

	fixture_build(&f, variant, sector_bytes, sectors_per_cluster, tables,
	    memory_backing, default_clusters, default_total_sectors);
	file_clusters = file_bytes / f.cluster_bytes;
	for (unsigned i = 0; i < 3; i++) {
		first = 2 + i * file_clusters;
		short_entry(f.memory + f.root_offset + i * 32, names[i], false,
		    first, file_bytes);
		for (uint32_t c = first; c < first + file_clusters; c++)
			set_cluster(&f, c, c + 1 < first + file_clusters ? c + 1 : f.eoc);
	}
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	CHECK(fd != -1);
	CHECK(write(fd, f.memory, (size_t)f.data_offset) == (ssize_t)f.data_offset);
	CHECK(close(fd) == 0);
	fixture_close(&f);
}

static void
write_truncated_seed(const char *path)
{
	fixture_t f;
	int fd;

	truncated_fixture(&f, 16);
	f.bytes = cluster_offset(&f, 8);
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	CHECK(fd != -1);
	CHECK(write(fd, f.memory, (size_t)f.bytes) == (ssize_t)f.bytes);
	CHECK(close(fd) == 0);
	fixture_close(&f);
}

int
main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--seed-output-budget") == 0) {
		write_output_budget_seed(argv[2]);
		return (0);
	}
	if (argc == 3 && strcmp(argv[1], "--seed-truncated") == 0) {
		write_truncated_seed(argv[2]);
		return (0);
	}
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
		test_boot_label_without_root_label();
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
	if (argc == 4 && (strcmp(argv[1], "--external") == 0 ||
	    strcmp(argv[1], "--external-ascii") == 0)) {
		test_external(argv[2], argv[3], strcmp(argv[1], "--external") == 0);
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
	check_force_short_volumes("fat12", 8192);
	test_truncated_directory_partial_cluster();
	test_truncated_directory_storage_bound();
	test_truncated_directory_work_bound();
	test_truncated_file_storage_bound();
	test_truncated_file_work_bound();
	test_truncated_contiguous_run();
	test_truncated_contiguous_file_read();
	test_truncated_file_seek_after_stop();
	test_truncated_file_length_after_stop();
	test_truncated_directory_end_marker();
	test_truncated_contiguous_directory_end_marker();
	test_truncated_directory_slots_after_stop();
	test_truncated_chain_surviving_sibling();
	test_complete_chain_control();
	test_truncated_tail(16);
	test_truncated_tail(12);
	test_truncated_tail(32);
	test_truncated_file_slack();
	test_truncated_file_reference();
	test_truncated_directory_reference();
	test_truncated_metadata();
	test_truncated_memory_cost_independent_of_declared_size();
	test_truncated_one_extra_cluster_refused();
	test_truncated_absent_tables_refused();
	test_whole_and_longer_slices();
	test_reader_byte_bounds();
	test_truncated_sweep();
	test_variants();
	for (unsigned variant = 16; variant <= 32; variant += 16) {
		test_dot_records_ntres_ignored(variant);
		test_dot_record_attribute_bits(variant);
		test_directory_without_dot_records(variant);
		test_dot_record_clusters_ignored_when_listing(variant);
		test_misplaced_dot_records_skipped_when_listing(variant);
		test_long_name_before_misplaced_dot_record_rejected(variant);
	}
	test_geometry_matrix();
	test_cluster_ceiling(12);
	test_cluster_ceiling(16);
	test_cluster_ceiling(32);
	test_names();
	test_geometry();
	test_thresholds();
	test_deep_tree();
	test_deep_descriptors();
	test_walker_poisoned();
	test_detection();
	test_tot_sec16_overrides_boot_code();
	test_assumed_atari_identity_reads_fragmented_file();
	test_assumed_identity_still_validates_geometry();
	test_assumed_identity_still_rejects_broken_chain();
	test_signed_volume_with_assume_identity_verifies();
	test_assumed_fat16_media_entry_is_reported_by_verify();
	test_assumed_reserved_entry_has_no_status_flags();
	test_assumed_identity_still_compares_reserved_entry_mirrors();
	test_assume_identity_requires_named_format_and_known_flags();
	test_non_pc_boot_records();
	test_unsigned_tiny_fat_open();
	test_pc98_boot_record();
	test_unsigned_fat16_head_does_not_check_fourth_byte();
	test_unsigned_fat16_and_fat32_identification();
	test_forced_unsigned_fat12_variant();
	test_geometry_decode_short_boot_record();
	test_boot_voucher_is_the_signature_or_the_media_echo();
	test_unvouched_oversized_regions_are_not_fat();
	test_unsigned_media_mismatch_is_not_fat();
	test_identified_unsigned_missing_tail_opens();
	test_media_check_precedes_mirror_comparison();
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
	test_trailing_duplicate_claim();
	test_cross_linked_files_name_shared_clusters();
	test_fragmented_verify();
	test_report_outputs();
	test_capability_bounds();
	test_boot_label_without_root_label();
	test_truncated_extract_output_budget();
	test_truncated_assumed_identity_verify();
	test_truncated_present_file_source_change_outranks_output_limit();
	return (0);
}
