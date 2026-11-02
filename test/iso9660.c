/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "format.h"
#include "fsimg.h"
#include "internal.h"
#include "test.h"

/* Independent ECMA-119 encoder. These are wire offsets, not driver types.
 * The prefix reserves room for 65 descriptors, path tables, directories
 * and payload. The callback supplies zeros beyond it for large files.
 */
#define BLOCK 2048u
#define BLOCKS 128u
#define ROOT 84u
#define DATA 104u
#define RR_AREA 110u
#define RR_DIRECTORY 85u

enum {
	DOT_BYTES = 34, DOT_PAIR_BYTES = 2 * DOT_BYTES,
	SP_BYTES = 7, PX_BYTES = 36, CE_BYTES = 28, LINK_BYTES = 12,
	SUSP_LENGTH = 2, SUSP_VERSION = 3, SUSP_PAYLOAD = 4,
	ROOT_SP = DOT_BYTES, ROOT_ER = ROOT_SP + SP_BYTES,
	ROOT_DOT_BYTES = 60, ROOT_CE_DOT_BYTES = 70,
	CHILD_SUSP = 40, RELOCATED_CHILD = DOT_PAIR_BYTES + LINK_BYTES,
	XORRISO_PLACEHOLDER = 43
};
/* Producer placeholder extents: docs/build.md, ISO producer observations. */
#define MKISOFS_PLACEHOLDER 0xffffffeeu

enum {
	VD_SPACE = 80, VD_SET = 120, VD_SEQUENCE = 124, VD_BLOCK = 128,
	VD_PATH_SIZE = 132, VD_PATH_L = 140, VD_PATH_M = 148,
	VD_ROOT = 156, VD_DATE = 813, VD_STRUCTURE = 881,
	DR_XATTR = 1, DR_EXTENT = 2, DR_SIZE = 10, DR_DATE = 18,
	DR_FLAGS = 25, DR_UNIT = 26, DR_GAP = 27, DR_SEQUENCE = 28,
	DR_NAME_SIZE = 32, DR_NAME = 33
};

typedef struct image {
	unsigned char	*bytes;
	uint64_t	 length;
	uint64_t	 stored;
	uint64_t	 high_read;
	uint64_t	 reads;
	uint64_t	 continuation_reads;
	uint64_t	 ce_read_index;
	uint64_t	 fail_read;
	bool		 eof;
	bool		 cancel_read;
} image_t;

typedef struct fixture {
	fsimg_ctx_t	*ctx;
	fsimg_io_t	*io;
	fsimg_reader_t	*reader;
	fsimg_stats_t	 baseline;
} fixture_t;

static void put16(unsigned char *p, uint16_t value);
static void put32(unsigned char *p, uint32_t value);
static size_t record(unsigned char *p, const void *name, size_t name_length,
    uint32_t extent, uint32_t length, unsigned flags);
static void directory(unsigned char *p, uint32_t block, uint32_t parent,
    uint32_t length);
static void descriptor(unsigned char *p, unsigned type, unsigned version);
static image_t image_new(void);
static fsimg_status_t image_size(void *arg, uint64_t *size,
    fsimg_error_t *error FSIMG_UNUSED);
static fsimg_status_t image_read(void *arg, uint64_t offset, void *buffer,
    size_t length,
    size_t *count, fsimg_error_t *error FSIMG_UNUSED);
static void fixture_new(fixture_t *f, image_t *image,
    const fsimg_options_t *options);
static void fixture_free(fixture_t *f);
static fsimg_status_t open_image(fixture_t *f, fsimg_iso_view_t view);
static fsimg_status_t walk(fixture_t *f);
static void expect(image_t *image, fsimg_status_t expected, const char *name);
static void expect_open(image_t *image, fsimg_status_t expected,
    const char *name);
static void probe_expect(image_t *image, fsimg_status_t expected,
    const char *name);
static void test_probe(void);
static void test_descriptors(void);
static void test_geometry(void);
static void test_records(void);
static void test_tree(void);
static void check_file(image_t *image, uint64_t size, uint64_t offset,
    const void *expected, size_t bytes);
static void test_sections(void);
static void test_extended_attributes_and_domain(void);
static void test_names(void);
static void test_info_metadata_views(void);
static void remove_tree(int parent, const char *name);
static fsimg_status_t extract(fixture_t *f, int parent, const char *selection);
static void test_extraction_and_selection(void);
static bool cancelled(void *argument);
static void test_image_faults(image_t *image);
static void test_faults(void);
static void test_exclusivity(void);
static void test_smallest_volume(void);
static void permutations(const unsigned char templates[6][BLOCK],
    unsigned *order,
    unsigned used, unsigned depth);
static void test_descriptor_permutations(void);
static void test_section_count_boundary(void);
static fsimg_status_t image_write(void *argument, uint64_t offset,
    const void *buffer, size_t length,
    size_t *count, fsimg_error_t *error FSIMG_UNUSED);
static fsimg_status_t image_resize(void *argument, uint64_t length,
    fsimg_error_t *error FSIMG_UNUSED);
static void test_other_format_probes(void);

static void test_supplementary_info(void);
static void test_displayed_version_suffix(void);
static void test_empty_section_extents(void);
static void test_descriptor_limit_terminator(void);
static void test_label_nul_padding(void);

/* El Torito fixture helpers and cases. */
static void boot_le16(unsigned char *p, uint16_t value);
static void boot_le32(unsigned char *p, uint32_t value);
static void boot_record(unsigned char *p, uint32_t catalog);
static void boot_checksum(unsigned char *p);
static void boot_validation(unsigned char *p, unsigned platform);
static void boot_entry(unsigned char *p, unsigned media, uint16_t sectors,
    uint32_t image_block);
static void boot_header(unsigned char *p, unsigned marker, unsigned platform,
    uint16_t count);
static image_t boot_image(unsigned platform);
static void boot_efi_section(image_t *image);
static void boot_info(image_t *image, const char *name, uint64_t catalog,
    uint64_t entries, uint64_t x86, uint64_t efi);
static void test_boot_platforms_and_sections(void);
static void test_boot_entry_variants(void);
static void test_boot_catalog_capacity(void);
static void test_boot_validation_mutations(void);
static void test_boot_entry_mutations(void);
static void test_boot_extent_bounds(void);
static void test_boot_data_remains_tree_data(void);

static unsigned failures;
static unsigned cases;

static void
put16(unsigned char *p, uint16_t value)
{
	p[0] = value;
	p[1] = value >> 8;
	p[2] = value >> 8;
	p[3] = value;
}

static void
put32(unsigned char *p, uint32_t value)
{
	for (unsigned i = 0; i < 4; i++) {
		p[i] = value >> (8 * i);
		p[7 - i] = value >> (8 * i);
	}
}

static size_t
record(unsigned char *p, const void *name, size_t name_length,
    uint32_t extent, uint32_t length, unsigned flags)
{
	size_t bytes = 33 + name_length + (name_length % 2 == 0);

	CHECK(bytes <= 254);
	bzero(p, bytes);
	p[0] = bytes;
	put32(p + DR_EXTENT, extent);
	put32(p + DR_SIZE, length);
	p[DR_FLAGS] = flags;
	put16(p + DR_SEQUENCE, 1);
	p[DR_NAME_SIZE] = name_length;
	bcopy(name, p + DR_NAME, name_length);
	return (bytes);
}

static void
directory(unsigned char *p, uint32_t block, uint32_t parent, uint32_t length)
{
	unsigned char self = 0, up = 1;

	record(p, &self, 1, block, length, 2);
	record(p + 34, &up, 1, parent, BLOCK, 2);
	if (parent == block)
		put32(p + 34 + DR_SIZE, length);
}

static void
descriptor(unsigned char *p, unsigned type, unsigned version)
{
	bzero(p, BLOCK);
	p[0] = type;
	bcopy("CD001", p + 1, 5);
	p[6] = version;
}

static image_t
image_new(void)
{
	image_t image = { .length = BLOCKS * BLOCK, .stored = BLOCKS * BLOCK };
	unsigned char *p, zero = 0;

	image.bytes = calloc(BLOCKS, BLOCK);
	CHECK(image.bytes != NULL);
	p = image.bytes + 16 * BLOCK;
	descriptor(p, 1, 1);
	memset(p + 40, ' ', 32);
	bcopy("TEST ISO", p + 40, 8);
	put32(p + VD_SPACE, BLOCKS);
	put16(p + VD_SET, 1);
	put16(p + VD_SEQUENCE, 1);
	put16(p + VD_BLOCK, BLOCK);
	put32(p + VD_PATH_SIZE, 10);
	p[VD_PATH_L] = 82;
	p[VD_PATH_M + 3] = 83;
	record(p + VD_ROOT, &zero, 1, ROOT, BLOCK, 2);
	p[VD_STRUCTURE] = 1;
	descriptor(image.bytes + 17 * BLOCK, 255, 1);
	/* Type-L and type-M root path-table records. */
	p = image.bytes + 82 * BLOCK;
	p[0] = 1;
	p[2] = ROOT;
	p[6] = 1;
	p = image.bytes + 83 * BLOCK;
	p[0] = 1;
	p[5] = ROOT;
	p[7] = 1;
	p = image.bytes + ROOT * BLOCK;
	directory(p, ROOT, ROOT, BLOCK);
	record(p + DOT_PAIR_BYTES, "A.TXT;1", 7, DATA, 5, 0);
	memset(image.bytes + DATA * BLOCK, 0xa5, BLOCK);
	bcopy("hello", image.bytes + DATA * BLOCK, 5);
	return (image);
}

static fsimg_status_t
image_size(void *arg, uint64_t *size, fsimg_error_t *error FSIMG_UNUSED)
{
	*size = ((image_t *)arg)->length;
	return (FSIMG_OK);
}

static fsimg_status_t
image_read(void *arg, uint64_t offset, void *buffer, size_t length,
    size_t *count, fsimg_error_t *error FSIMG_UNUSED)
{
	image_t *image = arg;

	CHECK(offset <= image->length && length <= image->length - offset);
	image->reads++;
	if (offset <= RR_AREA * BLOCK && offset + length > RR_AREA * BLOCK) {
		image->continuation_reads++;
		image->ce_read_index = image->reads;
	}
	*count = 0;
	if (image->reads == image->fail_read) {
		if (image->cancel_read)
			return (FSIMG_ECANCELLED);
		return (image->eof ? FSIMG_OK : FSIMG_EIO);
	}
	if (offset + length > image->high_read)
		image->high_read = offset + length;
	bzero(buffer, length);
	if (offset < image->stored) {
		size_t stored = image->stored - offset;

		if (stored > length)
			stored = length;
		bcopy(image->bytes + offset, buffer, stored);
	}
	*count = length;
	return (FSIMG_OK);
}

static void
fixture_new(fixture_t *f, image_t *image, const fsimg_options_t *options)
{
	fsimg_options_t local;
	fsimg_io_ops_t ops;
	int scratch;

	bzero(f, sizeof(*f));
	fsimg_options_init(&local);
	if (options != NULL)
		local = *options;
	scratch = open(".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(scratch >= 0);
	local.scratch_directory_fd = scratch;
	CHECK(fsimg_ctx_new(&local, &f->ctx, NULL) == FSIMG_OK);
	CHECK(close(scratch) == 0);
	CHECK(fsimg_ctx_get_stats(f->ctx, &f->baseline, NULL) == FSIMG_OK);
	fsimg_io_ops_init(&ops);
	ops.read_at = image_read;
	ops.get_size = image_size;
	CHECK(fsimg_io_from_callbacks(f->ctx, &ops, image, &f->io, NULL) ==
	    FSIMG_OK);
}

static void
fixture_free(fixture_t *f)
{
	fsimg_stats_t stats;

	fsimg_reader_free(f->reader);
	fsimg_io_free(f->io);
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == f->baseline.memory_bytes);
	CHECK(stats.scratch_bytes == 0);
	CHECK(stats.descriptors == f->baseline.descriptors);
	fsimg_ctx_free(f->ctx);
}

static fsimg_status_t
open_image(fixture_t *f, fsimg_iso_view_t view)
{
	fsimg_read_options_t options;

	fsimg_read_options_init(&options);
	options.iso_view = view;
	return (fsimg_reader_open(f->ctx, f->io, &options, &f->reader, NULL));
}

static fsimg_status_t
walk(fixture_t *f)
{
	const fsimg_entry_t *entry;
	fsimg_status_t status;

	while ((status = fsimg_reader_next(f->reader, &entry, NULL)) ==
	    FSIMG_OK)
		;
	return (status == FSIMG_DONE ? FSIMG_OK : status);
}

static void
expect(image_t *image, fsimg_status_t expected, const char *name)
{
	fixture_t f;
	fsimg_status_t status;

	fixture_new(&f, image, NULL);
	status = open_image(&f, FSIMG_ISO_VIEW_PRIMARY);
	if (status == FSIMG_OK)
		status = walk(&f);
	cases++;
	if (status != expected) {
		fprintf(stderr, "%s: expected %d, observed %d\n", name,
		    expected, status);
		failures++;
	}
	fixture_free(&f);
}

/* Catalog validation must finish before a reader is published for info. */
static void
expect_open(image_t *image, fsimg_status_t expected, const char *name)
{
	fixture_t f;
	fsimg_status_t status;

	fixture_new(&f, image, NULL);
	status = open_image(&f, FSIMG_ISO_VIEW_PRIMARY);
	cases++;
	if (status != expected) {
		fprintf(stderr, "%s: open expected %d, observed %d\n", name,
		    expected, status);
		failures++;
	}
	if (status != FSIMG_OK)
		CHECK(f.reader == NULL);
	fixture_free(&f);
}

static void
probe_expect(image_t *image, fsimg_status_t expected, const char *name)
{
	const fsimg_format_variant_t *variant;
	fsimg_status_t status;
	size_t prefix = image->length < 17 * BLOCK ? image->length : 17 * BLOCK;

	status = fsimg_iso9660_format.probe(image->bytes, prefix,
	    image->length, &variant, NULL);
	cases++;
	if (status != expected) {
		fprintf(stderr, "probe %s: expected %d, observed %d\n", name,
		    expected, status);
		failures++;
	}
}

static void
test_probe(void)
{
	image_t image = image_new();
	unsigned char *p = image.bytes + 16 * BLOCK;

	probe_expect(&image, FSIMG_OK, "PVD at block 16");
	p[1] = 'X';
	probe_expect(&image, FSIMG_EFORMAT, "missing signature");
	p[1] = 'C';
	image.length = 16 * BLOCK;
	probe_expect(&image, FSIMG_EFORMAT, "16 blocks");
	image.length = 17 * BLOCK;
	probe_expect(&image, FSIMG_OK, "17-block probe prefix");
	image.length = BLOCKS * BLOCK;
	put16(p + VD_BLOCK, 512);
	probe_expect(&image, FSIMG_EUNSUPPORTED, "512-byte blocks");
	p[VD_BLOCK + 3] ^= 1;
	probe_expect(&image, FSIMG_ECORRUPT, "block-size copies");
	put16(p + VD_BLOCK, BLOCK);
	p[VD_SPACE + 7] ^= 1;
	probe_expect(&image, FSIMG_ECORRUPT, "space copies");
	put32(p + VD_SPACE, 0);
	probe_expect(&image, FSIMG_ECORRUPT, "zero space");
	put32(p + VD_SPACE, BLOCKS);
	p[0] = 4;
	probe_expect(&image, FSIMG_EFORMAT, "unlisted first type");
	p[0] = 1;
	bcopy(p, p + BLOCK, BLOCK);
	descriptor(p, 0, 1);
	descriptor(p + 2 * BLOCK, 255, 1);
	expect(&image, FSIMG_OK, "boot before primary");
	free(image.bytes);
}

static void
test_descriptors(void)
{
	image_t image;
	unsigned char primary[BLOCK];
	unsigned char *p, *next;
	static const unsigned kinds[] = { 1, 2, 3, 0, 2 };

	/* Every permutation of opaque descriptors plus the primary. */
	for (unsigned a = 0; a < 5; a++) {
		for (unsigned b = 0; b < 5; b++) {
			if (a == b)
				continue;
			image = image_new();
			p = image.bytes + 16 * BLOCK;
			bcopy(p, primary, BLOCK);
			for (unsigned i = 0; i < 5; i++)
				descriptor(p + i * BLOCK, i == 0 ? 3 : kinds[i],
				    i == 4 ? 2 : 1);
			bcopy(primary, p + a * BLOCK, BLOCK);
			/* A Joliet descriptor in a different position. */
			bcopy(primary, p + b * BLOCK, BLOCK);
			p[b * BLOCK] = 2;
			bcopy("%/E", p + b * BLOCK + 88, 3);
			descriptor(p + 5 * BLOCK, 255, 1);
			bcopy("BEA01", p + 6 * BLOCK + 1, 5);
			expect(&image, FSIMG_OK,
			    "descriptor order with UDF tail");
			free(image.bytes);
		}
	}
	image = image_new();
	p = image.bytes + 16 * BLOCK;
	for (unsigned n = 64; n <= 65; n++) {
		for (unsigned i = 1; i < n; i++)
			descriptor(p + i * BLOCK, 3, 1);
		descriptor(p + n * BLOCK, 255, 1);
		expect(&image, n == 64 ? FSIMG_OK : FSIMG_EUNSUPPORTED,
		    "descriptor count boundary");
	}
	free(image.bytes);
	image = image_new();
	p = image.bytes + 16 * BLOCK;
	next = p + BLOCK;
	next[7] = 1;
	expect(&image, FSIMG_ECORRUPT, "terminator nonzero");
	next[7] = 0;
	p[0] = 3;
	expect(&image, FSIMG_ECORRUPT, "missing PVD");
	p[0] = 1;
	p[6] = 3;
	expect(&image, FSIMG_EUNSUPPORTED, "descriptor version");
	p[6] = 1;
	bcopy(p, next, BLOCK);
	descriptor(next + BLOCK, 255, 1);
	expect(&image, FSIMG_OK, "identical second PVD");
	next[40] ^= 1;
	expect(&image, FSIMG_EUNSUPPORTED, "different second PVD");
	next[1] = 'X';
	expect(&image, FSIMG_ECORRUPT, "missing later CD001");
	bcopy(p, next, BLOCK);
	next[0] = 2;
	bcopy("%/E", next + 88, 3);
	expect(&image, FSIMG_OK, "one Joliet descriptor");
	next[7] = 1;
	expect(&image, FSIMG_ECORRUPT, "Joliet flags");
	next[7] = 0;
	put32(next + VD_SPACE, BLOCKS - 1);
	expect(&image, FSIMG_ECORRUPT, "Joliet geometry");
	put32(next + VD_SPACE, 0);
	expect(&image, FSIMG_ECORRUPT, "Joliet zero volume space size");
	put32(next + VD_SPACE, BLOCKS);
	put16(next + VD_BLOCK, 512);
	expect(&image, FSIMG_ECORRUPT, "Joliet block size 512");
	put16(next + VD_BLOCK, BLOCK);
	put16(next + VD_SET, 2);
	expect(&image, FSIMG_ECORRUPT, "Joliet volume set size 2");
	put16(next + VD_SET, 1);
	put16(next + VD_SEQUENCE, 2);
	expect(&image, FSIMG_ECORRUPT, "Joliet volume sequence number 2");
	put16(next + VD_SEQUENCE, 1);
	bcopy(next, next + BLOCK, BLOCK);
	descriptor(next + 2 * BLOCK, 255, 1);
	expect(&image, FSIMG_EUNSUPPORTED, "two Joliet descriptors");
	descriptor(next, 255, 1);
	for (unsigned field = VD_SET; field <= VD_SEQUENCE; field += 4) {
		put16(p + field, 2);
		expect(&image, FSIMG_EUNSUPPORTED, "volume set or sequence");
		put16(p + field, 1);
	}
	p[VD_STRUCTURE] = 2;
	expect(&image, FSIMG_EUNSUPPORTED, "file structure version");
	p[VD_STRUCTURE] = 1;
	bcopy("2023023000000000", p + VD_DATE, 16);
	expect(&image, FSIMG_ECORRUPT, "invalid descriptor date");
	memset(p + VD_DATE, '0', 16);
	expect(&image, FSIMG_OK, "ASCII absent date");
	bzero(p + VD_DATE, 17);
	expect(&image, FSIMG_OK, "binary absent date");
	put32(p + VD_SPACE, 17);
	expect(&image, FSIMG_ECORRUPT, "descriptor beyond declared space");
	free(image.bytes);
}

static void
test_geometry(void)
{
	image_t image = image_new();
	unsigned char *p = image.bytes + 16 * BLOCK;
	unsigned char *file = image.bytes + ROOT * BLOCK + 68;
	fixture_t f;
	fsimg_volume_info_t info;

	image.length += BLOCK;
	expect(&image, FSIMG_OK, "longer slice");
	CHECK(image.high_read <= BLOCKS * BLOCK);
	image.length = (BLOCKS - 1) * BLOCK;
	fixture_new(&f, &image, NULL);
	CHECK(open_image(&f, FSIMG_ISO_VIEW_PRIMARY) == FSIMG_OK);
	CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.details[8].name, "missing_blocks") == 0);
	CHECK(info.details[8].value == 1);
	CHECK(walk(&f) == FSIMG_OK);
	fixture_free(&f);
	put32(file + DR_EXTENT, BLOCKS);
	expect(&image, FSIMG_ECORRUPT, "file past volume");
	put32(file + DR_EXTENT, DATA);
	put32(file + DR_SIZE, UINT32_MAX);
	expect(&image, FSIMG_ECORRUPT, "large extent arithmetic");
	put32(file + DR_SIZE, 5);
	image.length = DATA * BLOCK;
	expect(&image, FSIMG_ECORRUPT, "file past short slice");
	image.length = ROOT * BLOCK;
	expect(&image, FSIMG_ECORRUPT, "directory past short slice");
	image.length = BLOCKS * BLOCK;
	p[VD_PATH_L] = BLOCKS;
	expect(&image, FSIMG_ECORRUPT, "path table outside volume");
	p[VD_PATH_L] = 82;
	image.length--;
	expect(&image, FSIMG_OK, "partial unreferenced final block");
	free(image.bytes);
}

static void
test_records(void)
{
	image_t image;
	unsigned char *root, *file, *p;
	typedef struct record_mutation {
		unsigned offset;
		unsigned value;
		fsimg_status_t status;
		const char *name;
	} record_mutation_t;
	static const record_mutation_t mutations[] = {
		{ 0, 41, FSIMG_ECORRUPT, "odd record length" },
		{ 0, 33, FSIMG_ECORRUPT, "short record" },
		{ DR_NAME_SIZE, 0, FSIMG_ECORRUPT, "empty identifier" },
		{ DR_NAME_SIZE, 250, FSIMG_ECORRUPT, "identifier past record" },
		{ DR_FLAGS, 0x20, FSIMG_ECORRUPT, "reserved flag 20" },
		{ DR_FLAGS, 0x40, FSIMG_ECORRUPT, "reserved flag 40" },
		{ DR_UNIT, 1, FSIMG_EUNSUPPORTED, "interleave unit" },
		{ DR_GAP, 1, FSIMG_EUNSUPPORTED, "interleave gap" },
		{ DR_FLAGS, 4, FSIMG_EUNSUPPORTED, "associated file" },
		{ DR_FLAGS, 8, FSIMG_ECORRUPT, "record format without xattr" },
		{ DR_FLAGS, 16, FSIMG_ECORRUPT, "protection without xattr" },
		{ DR_DATE + 1, 13, FSIMG_ECORRUPT, "invalid recording date" },
		{ DR_EXTENT + 7, 1, FSIMG_ECORRUPT, "extent copies disagree" },
		{ DR_SIZE + 7, 1, FSIMG_ECORRUPT, "length copies disagree" }
	};

	image = image_new();
	file = image.bytes + ROOT * BLOCK + 68;
	record(file, "AB.TXT;1", 8, DATA, 5, 0);
	expect(&image, FSIMG_OK, "even file identifier with zero pad");
	check_file(&image, 5, 0, "hello", 5);
	file[DR_NAME + 8] = 1;
	expect(&image, FSIMG_ECORRUPT, "nonzero file identifier pad");
	free(image.bytes);

	image = image_new();
	file = image.bytes + ROOT * BLOCK + 68;
	bzero(file, BLOCK - 68);
	record(file, "AB", 2, ROOT + 1, BLOCK, 2);
	directory(image.bytes + (ROOT + 1) * BLOCK, ROOT + 1, ROOT, BLOCK);
	expect(&image, FSIMG_OK, "even directory identifier with zero pad");
	file[DR_NAME + 2] = 1;
	expect(&image, FSIMG_ECORRUPT, "nonzero directory identifier pad");
	free(image.bytes);

	for (size_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
		image = image_new();
		file = image.bytes + ROOT * BLOCK + 68;
		expect(&image, FSIMG_OK, mutations[i].name);
		file[mutations[i].offset] = mutations[i].value;
		expect(&image, mutations[i].status, mutations[i].name);
		free(image.bytes);
	}
	image = image_new();
	root = image.bytes + ROOT * BLOCK;
	file = root + 68;
	put16(file + DR_SEQUENCE, 2);
	expect(&image, FSIMG_EUNSUPPORTED, "record sequence 2");
	put16(file + DR_SEQUENCE, 1);
	root[2000] = 1;
	expect(&image, FSIMG_ECORRUPT, "nonzero block tail");
	root[2000] = 0;
	p = image.bytes + 16 * BLOCK + VD_ROOT;
	put32(p + DR_SIZE, 108);
	put32(root + DR_SIZE, 108);
	put32(root + 34 + DR_SIZE, 108);
	expect(&image, FSIMG_OK, "exact directory length");
	root[2000] = 1;
	expect(&image, FSIMG_ECORRUPT, "padding past directory length");
	root[2000] = 0;
	put32(p + DR_SIZE, 107);
	put32(root + DR_SIZE, 107);
	put32(root + 34 + DR_SIZE, 107);
	expect(&image, FSIMG_ECORRUPT, "record past directory end");
	put32(p + DR_SIZE, 0);
	expect(&image, FSIMG_ECORRUPT, "zero directory size");
	free(image.bytes);

	/* System-use bytes enlarge records so the last ends at the block
	 * end. */
	image = image_new();
	root = image.bytes + ROOT * BLOCK;
	bzero(root + 68, BLOCK - 68);
	for (size_t offset = 68; offset < 1868; offset += 200) {
		char name[16];

		CHECK(snprintf(name, sizeof(name), "F%04zu;1", offset) > 0);
		record(root + offset, name, strlen(name), DATA, 5, 0);
		root[offset] = 200;
	}
	record(root + 1868, "END;1", 5, DATA, 5, 0);
	root[1868] = 180;
	expect(&image, FSIMG_OK, "records exactly fill block");
	root[1868] = 182;
	expect(&image, FSIMG_ECORRUPT, "record crosses block");
	root[1868] = 180;
	put32(image.bytes + 16 * BLOCK + VD_ROOT + DR_SIZE, 3 * BLOCK);
	put32(root + DR_SIZE, 3 * BLOCK);
	put32(root + 34 + DR_SIZE, 3 * BLOCK);
	record(root + 2 * BLOCK, "LATE;1", 6, DATA, 5, 0);
	expect(&image, FSIMG_OK, "directory spans blocks with empty block");
	free(image.bytes);
}

static void
test_tree(void)
{
	image_t image = image_new();
	unsigned char *root = image.bytes + ROOT * BLOCK;
	unsigned char *pvd_root = image.bytes + 16 * BLOCK + VD_ROOT;
	unsigned char saved[34];

	for (unsigned depth = 0; depth < 12; depth++) {
		unsigned block = ROOT + depth;
		unsigned char *p = image.bytes + block * BLOCK;

		bzero(p, BLOCK);
		directory(p, block, depth == 0 ? ROOT : block - 1, BLOCK);
		if (depth < 11)
			record(p + DOT_PAIR_BYTES, "DIR", 3, block + 1, BLOCK, 2);
		else
			record(p + DOT_PAIR_BYTES, "A.TXT;1", 7, DATA, 5, 0);
	}
	expect(&image, FSIMG_OK, "12-level tree");
	root[68 + DR_FLAGS] |= 0x80;
	expect(&image, FSIMG_ECORRUPT, "multi-extent directory");
	root[68 + DR_FLAGS] = 2;
	put32(root + DR_EXTENT, ROOT + 1);
	expect(&image, FSIMG_ECORRUPT, "wrong self extent");
	put32(root + DR_EXTENT, ROOT);
	put32(root + DR_SIZE, BLOCK - 2);
	expect(&image, FSIMG_ECORRUPT, "wrong self length");
	put32(root + DR_SIZE, BLOCK);
	put32(root + BLOCK + 34 + DR_EXTENT, ROOT + 1);
	expect(&image, FSIMG_ECORRUPT, "wrong parent extent");
	put32(root + BLOCK + 34 + DR_EXTENT, ROOT);
	root[DR_NAME] = 1;
	expect(&image, FSIMG_ECORRUPT, "dot records not first");
	root[DR_NAME] = 0;
	put32(root + 68 + DR_EXTENT, ROOT);
	expect(&image, FSIMG_ECORRUPT, "child names root");
	put32(root + 68 + DR_EXTENT, ROOT + 1);
	record(root + 104, "OTHER", 5, ROOT + 1, BLOCK, 2);
	expect(&image, FSIMG_ECORRUPT, "two names open one directory");
	bzero(root + 104, BLOCK - 104);
	record(root + 104, "\0", 1, DATA, 5, 0);
	expect(&image, FSIMG_ECORRUPT, "misplaced self identifier");
	root[104 + DR_NAME] = 1;
	expect(&image, FSIMG_ECORRUPT, "misplaced parent identifier");
	bzero(root + 104, BLOCK - 104);
	put32(root + 2 * BLOCK + 68 + DR_EXTENT, ROOT + 1);
	expect(&image, FSIMG_ECORRUPT, "child points to parent cycle");
	put32(root + 2 * BLOCK + 68 + DR_EXTENT, ROOT + 3);
	bcopy(pvd_root, saved, 34);
	pvd_root[0] = 36;
	expect(&image, FSIMG_ECORRUPT, "PVD root length");
	bcopy(saved, pvd_root, 34);
	pvd_root[DR_NAME] = 1;
	expect(&image, FSIMG_ECORRUPT, "PVD root identifier");
	bcopy(saved, pvd_root, 34);
	pvd_root[DR_FLAGS] = 0;
	expect(&image, FSIMG_ECORRUPT, "PVD root not directory");
	free(image.bytes);
}

static void
check_file(image_t *image, uint64_t size, uint64_t offset,
    const void *expected, size_t bytes)
{
	fixture_t f;
	const fsimg_entry_t *entry;
	unsigned char buffer[4096];
	fsimg_range_t range;
	size_t count;

	CHECK(bytes <= sizeof(buffer));
	fixture_new(&f, image, NULL);
	CHECK(open_image(&f, FSIMG_ISO_VIEW_PRIMARY) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(entry->kind == FSIMG_ENTRY_FILE && entry->size == size);
	CHECK(fsimg_reader_read_at(f.reader, offset, buffer, bytes, &count,
	    NULL) == FSIMG_OK);
	CHECK(count == bytes && bcmp(buffer, expected, bytes) == 0);
	CHECK(fsimg_reader_next_range(f.reader, offset, &range,
	    NULL) == FSIMG_OK);
	CHECK(range.offset == offset && range.length > 0);
	CHECK(walk(&f) == FSIMG_OK);
	fixture_free(&f);
}

static void
test_sections(void)
{
	image_t image = image_new();
	unsigned char *file = image.bytes + ROOT * BLOCK + 68;
	unsigned char zeros[16] = { 0 };

	check_file(&image, 5, 0, "hello", 5);
	put32(file + DR_SIZE, BLOCK);
	file[DR_FLAGS] = 0x80;
	record(file + 40, "A.TXT;1", 7, DATA + 2, 5, 0);
	bcopy("world", image.bytes + (DATA + 2) * BLOCK, 5);
	image.bytes[(DATA + 1) * BLOCK - 2] = 'x';
	image.bytes[(DATA + 1) * BLOCK - 1] = 'y';
	check_file(&image, BLOCK + 5, BLOCK - 2, "xyworld", 7);
	file[40 + DR_FLAGS] = 0x80;
	expect(&image, FSIMG_ECORRUPT, "missing last section");
	file[40 + DR_FLAGS] = 0;
	file[40 + DR_NAME] = 'B';
	expect(&image, FSIMG_ECORRUPT, "changed section identifier");
	file[40 + DR_NAME] = 'A';
	put32(file + DR_SIZE, BLOCK - 1);
	expect(&image, FSIMG_ECORRUPT, "unaligned middle section");
	put32(file + DR_SIZE, BLOCK);
	put32(file + 40 + DR_EXTENT, DATA - 1);
	expect(&image, FSIMG_EUNSUPPORTED, "descending sections");
	put32(file + 40 + DR_EXTENT, DATA);
	expect(&image, FSIMG_EUNSUPPORTED, "overlapping sections");
	put32(file + 40 + DR_EXTENT, BLOCKS);
	expect(&image, FSIMG_ECORRUPT, "section outside volume");
	put32(file + 40 + DR_SIZE, 0);
	check_file(&image, BLOCK, 0, "hello", 5);
	free(image.bytes);

	image = image_new();
	file = image.bytes + ROOT * BLOCK + 68;
	for (unsigned i = 0; i < 20; i++) {
		record(file + i * 40, "A.TXT;1", 7, DATA + i, BLOCK,
		    i == 19 ? 0 : 0x80);
		memset(image.bytes + (DATA + i) * BLOCK, 'a' + i, BLOCK);
	}
	check_file(&image, 20 * BLOCK, BLOCK - 2, "aabb", 4);
	for (unsigned i = 0; i < 20; i++) {
		unsigned char expected[BLOCK];

		memset(expected, 'a' + i, BLOCK);
		check_file(&image, 20 * BLOCK, i * BLOCK, expected, BLOCK);
	}
	free(image.bytes);

	/* Two maximal sections exceed 4 GiB. Only a fixed prefix is stored;
	 * data beyond it is synthesized by image_read, with checked bounds. */
	image = image_new();
	file = image.bytes + ROOT * BLOCK + 68;
	image.length = UINT64_C(5000000) * BLOCK;
	put32(image.bytes + 16 * BLOCK + VD_SPACE, 5000000);
	record(file, "A.TXT;1", 7, 200, UINT32_MAX - 2047, 0x80);
	record(file + 40, "A.TXT;1", 7, 2097351, UINT32_MAX, 0);
	check_file(&image, UINT64_C(8589932543), UINT64_C(8589932527),
	    zeros, sizeof(zeros));
	free(image.bytes);
}

static void
test_extended_attributes_and_domain(void)
{
	image_t image = image_new();
	unsigned char *file = image.bytes + ROOT * BLOCK + 68;

	for (unsigned n = 1; n <= 2; n++) {
		file[DR_XATTR] = n;
		memset(image.bytes + DATA * BLOCK, 0x6d, n * BLOCK);
		bcopy("hello", image.bytes + (DATA + n) * BLOCK, 5);
		check_file(&image, 5, 0, "hello", 5);
		for (unsigned flag = 8; flag <= 16; flag *= 2) {
			file[DR_FLAGS] = flag;
			expect(&image, FSIMG_EUNSUPPORTED,
			    "xattr record format/protection");
		}
		file[DR_FLAGS] = 0;
	}
	put32(file + DR_EXTENT, BLOCKS - 1);
	expect(&image, FSIMG_ECORRUPT, "xattr outside volume");
	put32(file + DR_EXTENT, ROOT + 1);
	file[DR_FLAGS] = 2;
	put32(file + DR_SIZE, BLOCK);
	expect(&image, FSIMG_EUNSUPPORTED, "xattr on directory");
	free(image.bytes);
	for (unsigned block = 0; block <= 17; block += 17) {
		image = image_new();
		file = image.bytes + ROOT * BLOCK + 68;
		put32(file + DR_EXTENT, block);
		expect(&image, FSIMG_ECORRUPT,
		    "file outside content domain at listing");
		free(image.bytes);
	}
}

static void
test_names(void)
{
	static const char *const bad[] = {
		"A;0", "A;32768", "A;01", "A;", "A/B;1", ";1", ".", ".."
	};
	image_t image;
	unsigned char *root;
	fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_stats_t stats;

	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		image = image_new();
		root = image.bytes + ROOT * BLOCK;
		bzero(root + 68, BLOCK - 68);
		record(root + 68, bad[i], strlen(bad[i]), DATA, 5, 0);
		expect(&image, FSIMG_ECORRUPT, bad[i]);
		free(image.bytes);
	}
	image = image_new();
	root = image.bytes + ROOT * BLOCK;
	root[68 + DR_NAME + 1] = 0;
	expect(&image, FSIMG_ECORRUPT, "NUL in identifier");
	root[68 + DR_NAME + 1] = '.';
	record(root + 108, "A.TXT", 5, DATA, 5, 0);
	expect(&image, FSIMG_ECORRUPT, "duplicate displayed name");
	bzero(root + 108, BLOCK - 108);
	record(root + 108, "A.TXT;2", 7, DATA, 5, 0);
	record(root + 148, "README.;1", 9, DATA, 5, 0);
	record(root + 190, "README", 6, ROOT + 1, BLOCK, 2);
	directory(root + BLOCK, ROOT + 1, ROOT, BLOCK);
	record(root + 230, "lowercase", 9, DATA, 5, 0);
	fixture_new(&f, &image, NULL);
	CHECK(open_image(&f, FSIMG_ISO_VIEW_PRIMARY) == FSIMG_OK);
	for (unsigned i = 0; i < 5; i++) {
		static const char *const names[] = {
			"/A.TXT", "/A.TXT;2", "/README.", "/README",
			    "/lowercase"
		};

		CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
		CHECK(strcmp(entry->path, names[i]) == 0);
	}
	CHECK(walk(&f) == FSIMG_OK);
	fixture_free(&f);
	bzero(root + 68, BLOCK - 68);
	record(root + 68, "caf\xe9;1", 6, DATA, 5, 0);
	fixture_new(&f, &image, NULL);
	CHECK(open_image(&f, FSIMG_ISO_VIEW_PRIMARY) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_DONE);
	CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.omitted_entries == 1);
	fixture_free(&f);
	free(image.bytes);
}

static void
test_info_metadata_views(void)
{
	static const char *const details[] = {
		"rockridge", "joliet_level", "enhanced", "descriptors",
		"terminator_block", "root_block", "path_table_bytes",
		"system_area_used", "missing_blocks", "boot_catalog_block",
		"boot_entries", "boot_x86", "boot_efi"
	};
	static const uint64_t values[] = {
		3, 0, 0, 1, 17, ROOT, 10, 0, 0, 0, 0, 0, 0
	};
	typedef struct date_case {
		unsigned char bytes[7];
		int64_t seconds;
		int32_t minutes;
	} date_case_t;
	static const date_case_t dates[] = {
		{ { 70, 1, 1, 1, 0, 0, 4 }, 0, 60 },
		{ { 70, 1, 1, 0, 0, 0, 252 }, 3600, -60 },
		{ { 69, 12, 31, 23, 59, 59, 0 }, -1, 0 }
	};
	image_t image = image_new();
	unsigned char *file = image.bytes + ROOT * BLOCK + 68;
	fsimg_volume_info_t info;
	fsimg_verify_report_t report;
	const fsimg_entry_t *entry;
	fixture_t f;

	fixture_new(&f, &image, NULL);
	CHECK(open_image(&f, FSIMG_ISO_VIEW_PRIMARY) == FSIMG_OK);
	CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.format, "iso9660") == 0);
	CHECK(strcmp(info.profile, "primary") == 0);
	CHECK(strcmp(info.label, "TEST ISO") == 0);
	CHECK((info.valid & FSIMG_VOLUME_LABEL) != 0);
	CHECK(info.volume_bytes == BLOCKS * BLOCK);
	CHECK(info.allocation_units == BLOCKS && info.sector_bytes == BLOCK);
	CHECK(info.allocation_unit_bytes == BLOCK);
	CHECK(info.time_basis == FSIMG_TIME_OFFSET);
	CHECK(info.scope == FSIMG_VALIDATION_HEADER);
	CHECK(info.capabilities == FSIMG_CAP_READ);
	CHECK(info.detail_count == 13);
	for (unsigned i = 0; i < 13; i++) {
		CHECK(strcmp(info.details[i].name, details[i]) == 0);
		CHECK(info.details[i].value == values[i]);
	}
	CHECK(fsimg_verify(f.reader, 0, &report, NULL) == FSIMG_EUNSUPPORTED);
	CHECK(!report.complete);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK((entry->metadata.valid & FSIMG_META_MODIFIED) == 0);
	fixture_free(&f);
	for (unsigned i = 0; i < 3; i++) {
		bcopy(dates[i].bytes, file + DR_DATE, 7);
		file[DR_FLAGS] = 1;
		fixture_new(&f, &image, NULL);
		CHECK(open_image(&f, FSIMG_ISO_VIEW_PRIMARY) == FSIMG_OK);
		CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
		CHECK((entry->metadata.attributes & FSIMG_ATTR_HIDDEN) != 0);
		CHECK((entry->metadata.valid & FSIMG_META_MODIFIED) != 0);
		CHECK(entry->metadata.modified.basis == FSIMG_TIME_OFFSET);
		CHECK(entry->metadata.modified.seconds == dates[i].seconds);
		CHECK(entry->metadata.modified.offset_minutes ==
		    dates[i].minutes);
		CHECK(entry->metadata.modified.precision_ns ==
		    UINT64_C(1000000000));
		fixture_free(&f);
	}
	for (unsigned view = 0; view <= 3; view++) {
		fixture_new(&f, &image, NULL);
		CHECK(open_image(&f, (fsimg_iso_view_t)view) ==
		    (view == 0 || view == 3 ? FSIMG_OK : FSIMG_EUNSUPPORTED));
		fixture_free(&f);
	}
	for (unsigned byte = 1; byte <= 255; byte += 254) {
		image.bytes[16 * BLOCK + 40] = byte;
		fixture_new(&f, &image, NULL);
		CHECK(open_image(&f, FSIMG_ISO_VIEW_PRIMARY) == FSIMG_OK);
		CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
		CHECK((info.valid & FSIMG_VOLUME_LABEL) == 0);
		fixture_free(&f);
	}
	free(image.bytes);
}

/* Only fixture-created private trees reach this cleanup; never follow links. */
static void
remove_tree(int parent, const char *name)
{
	struct dirent *entry;
	struct stat st;
	DIR *dir;
	int fd;

	if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
		return;
	if (!S_ISDIR(st.st_mode)) {
		CHECK(unlinkat(parent, name, 0) == 0);
		return;
	}
	fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(fd >= 0);
	dir = fdopendir(fd);
	CHECK(dir != NULL);
	while ((entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0)
			continue;
		remove_tree(fd, entry->d_name);
	}
	CHECK(closedir(dir) == 0);
	CHECK(unlinkat(parent, name, AT_REMOVEDIR) == 0);
}

static fsimg_status_t
extract(fixture_t *f, int parent, const char *selection)
{
	fsimg_extract_options_t options;

	fsimg_extract_options_init(&options);
	if (selection != NULL) {
		options.selections = &selection;
		options.selection_count = 1;
	}
	return (fsimg_extract_at(f->reader, parent, "out", &options, NULL));
}

static void
test_extraction_and_selection(void)
{
	static const char *const selections[] = {
		NULL, "/A.TXT", "/A.TXT;1", "/a.txt", "/bad\001"
	};
	image_t image = image_new();
	fixture_t f;
	char temporary[] = "./iso-extract-XXXXXX";
	char buffer[2053];
	int parent, fd;

	CHECK(mkdtemp(temporary) != NULL);
	parent = open(temporary, O_RDONLY | O_DIRECTORY);
	CHECK(parent >= 0);
	for (unsigned i = 0; i < 5; i++) {
		fixture_new(&f, &image, NULL);
		CHECK(open_image(&f, FSIMG_ISO_VIEW_PRIMARY) == FSIMG_OK);
		CHECK(extract(&f, parent, selections[i]) ==
		    (i == 3 ? FSIMG_EINVAL : i == 4 ? FSIMG_EINVAL : FSIMG_OK));
		if (i < 3) {
			fd = openat(parent, "out/A.TXT", O_RDONLY | O_NOFOLLOW);
			CHECK(fd >= 0);
			CHECK(read(fd, buffer, sizeof(buffer)) == 5);
			CHECK(bcmp(buffer, "hello", 5) == 0);
			CHECK(close(fd) == 0);
		}
		fixture_free(&f);
		remove_tree(parent, "out");
	}
	/* Extraction must concatenate sections and discard extent padding. */
	put32(image.bytes + ROOT * BLOCK + 68 + DR_SIZE, BLOCK);
	image.bytes[ROOT * BLOCK + 68 + DR_FLAGS] = 0x80;
	record(image.bytes + ROOT * BLOCK + 108, "A.TXT;1", 7, DATA + 2, 5, 0);
	bcopy("world", image.bytes + (DATA + 2) * BLOCK, 5);
	fixture_new(&f, &image, NULL);
	CHECK(open_image(&f, FSIMG_ISO_VIEW_PRIMARY) == FSIMG_OK);
	CHECK(extract(&f, parent, NULL) == FSIMG_OK);
	fd = openat(parent, "out/A.TXT", O_RDONLY | O_NOFOLLOW);
	CHECK(fd >= 0);
	CHECK(read(fd, buffer, sizeof(buffer)) == sizeof(buffer));
	CHECK(bcmp(buffer, image.bytes + DATA * BLOCK, BLOCK) == 0);
	CHECK(bcmp(buffer + BLOCK, "world", 5) == 0);
	CHECK(read(fd, buffer, 1) == 0);
	CHECK(close(fd) == 0);
	fixture_free(&f);
	remove_tree(parent, "out");
	for (unsigned i = 0; i < 20; i++) {
		record(image.bytes + ROOT * BLOCK + 68 + i * 40,
		    "A.TXT;1", 7, DATA + i, BLOCK, i == 19 ? 0 : 0x80);
		memset(image.bytes + (DATA + i) * BLOCK, 'a' + i, BLOCK);
	}
	fixture_new(&f, &image, NULL);
	CHECK(open_image(&f, FSIMG_ISO_VIEW_PRIMARY) == FSIMG_OK);
	CHECK(extract(&f, parent, NULL) == FSIMG_OK);
	fd = openat(parent, "out/A.TXT", O_RDONLY | O_NOFOLLOW);
	CHECK(fd >= 0);
	for (unsigned i = 0; i < 20; i++) {
		CHECK(read(fd, buffer, BLOCK) == BLOCK);
		CHECK(bcmp(buffer, image.bytes + (DATA + i) * BLOCK,
		    BLOCK) == 0);
	}
	CHECK(read(fd, buffer, 1) == 0);
	CHECK(close(fd) == 0);
	fixture_free(&f);
	remove_tree(parent, "out");
	bzero(image.bytes + ROOT * BLOCK + 68, BLOCK - 68);
	record(image.bytes + ROOT * BLOCK + 68, "caf\xe9", 4, DATA, 5, 0);
	fixture_new(&f, &image, NULL);
	CHECK(open_image(&f, FSIMG_ISO_VIEW_PRIMARY) == FSIMG_OK);
	CHECK(extract(&f, parent, NULL) == FSIMG_EUNSUPPORTED);
	fixture_free(&f);
	remove_tree(parent, "out");
	CHECK(close(parent) == 0);
	CHECK(rmdir(temporary) == 0);
	free(image.bytes);
}

static bool
cancelled(void *argument)
{
	unsigned *remaining = argument;

	if (*remaining == 0)
		return (true);
	(*remaining)--;
	return (false);
}

static void
test_image_faults(image_t *image)
{
	fixture_t f;
	fsimg_options_t options;
	fsimg_status_t status;
	const fsimg_entry_t *entry;
	unsigned remaining;
	unsigned char buffer[8];
	uint64_t open_reads, reads;
	bool allocation_hit;
	size_t count;
	char temporary[] = "./iso-fault-XXXXXX";
	int parent;

	CHECK(mkdtemp(temporary) != NULL);
	parent = open(temporary, O_RDONLY | O_DIRECTORY);
	CHECK(parent >= 0);
	/* Each prefix of the allocation sequence fails once; all partially
	 * constructed objects are released before their context is checked. */
	for (unsigned operation = 0; operation < 4; operation++) {
		for (unsigned n = 1;; n++) {
			fixture_new(&f, image, NULL);
			fsimg_test_fail_alloc_after(f.ctx, n);
			status = open_image(&f, FSIMG_ISO_VIEW_PRIMARY);
			if (status == FSIMG_OK && operation == 1)
				status = walk(&f);
			if (status == FSIMG_OK && operation == 2) {
				status = fsimg_reader_next(f.reader, &entry,
				    NULL);
				if (status == FSIMG_OK)
					status = fsimg_reader_read_at(f.reader,
					    0, buffer, sizeof(buffer), &count,
					    NULL);
			}
			if (status == FSIMG_OK && operation == 3)
				status = extract(&f, parent, NULL);
			allocation_hit = f.ctx->allocation_attempts >= n;
			fsimg_test_fail_alloc_after(f.ctx, 0);
			fixture_free(&f);
			remove_tree(parent, "out");
			if (status == FSIMG_OK && !allocation_hit)
				break;
			CHECK(status == FSIMG_OK || status == FSIMG_ENOMEM ||
			    status == FSIMG_ELIMIT);
			CHECK(n < 10000);
		}
	}
	image->reads = 0;
	fixture_new(&f, image, NULL);
	CHECK(open_image(&f, FSIMG_ISO_VIEW_PRIMARY) == FSIMG_OK);
	open_reads = image->reads;
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_read_at(f.reader, 0, buffer, sizeof(buffer),
	    &count, NULL) == FSIMG_OK);
	reads = image->reads;
	fixture_free(&f);
	for (unsigned eof = 0; eof < 2; eof++) {
		for (uint64_t n = 1; n <= reads; n++) {
			image->reads = 0;
			image->fail_read = n;
			image->eof = eof != 0;
			fixture_new(&f, image, NULL);
			status = open_image(&f, FSIMG_ISO_VIEW_PRIMARY);
			if (n <= open_reads) {
				CHECK(status == FSIMG_EIO);
				CHECK(f.reader == NULL);
			}
			if (status == FSIMG_OK)
				status = fsimg_reader_next(f.reader, &entry,
				    NULL);
			if (status == FSIMG_OK)
				status = fsimg_reader_read_at(f.reader, 0,
				    buffer,
				    sizeof(buffer), &count, NULL);
			CHECK(status == FSIMG_EIO);
			fixture_free(&f);
		}
	}
	image->fail_read = 0;
	for (unsigned blocks = 0; blocks < BLOCKS; blocks++) {
		image->length = blocks * BLOCK;
		expect(image, blocks < 17 ? FSIMG_EFORMAT :
		    blocks <= DATA ? FSIMG_ECORRUPT : FSIMG_OK,
		    "truncation at each block boundary");
	}
	image->length = BLOCKS * BLOCK;
	/* Sweep every cancellation checkpoint in open, then in open and walk. */
	for (unsigned operation = 0; operation < 2; operation++) {
		for (remaining = 0;; remaining++) {
			unsigned budget = remaining;

			fsimg_options_init(&options);
			options.cancelled = cancelled;
			options.cancel_argument = &budget;
			fixture_new(&f, image, &options);
			status = open_image(&f, FSIMG_ISO_VIEW_PRIMARY);
			if (status != FSIMG_OK)
				CHECK(f.reader == NULL);
			if (status == FSIMG_OK && operation == 1)
				status = walk(&f);
			if (remaining < 2)
				CHECK(status == FSIMG_ECANCELLED);
			CHECK(status == FSIMG_OK || status == FSIMG_ECANCELLED);
			fixture_free(&f);
			if (status == FSIMG_OK)
				break;
			CHECK(remaining < 10000);
		}
	}
	fsimg_options_init(&options);
	options.max_work = 1;
	fixture_new(&f, image, &options);
	status = open_image(&f, FSIMG_ISO_VIEW_PRIMARY);
	if (status == FSIMG_OK)
		status = walk(&f);
	CHECK(status == FSIMG_ELIMIT);
	fixture_free(&f);
	record(image->bytes + ROOT * BLOCK + 108, "B;1", 3, DATA, 5, 0);
	fsimg_options_init(&options);
	options.max_entries = 1;
	fixture_new(&f, image, &options);
	status = open_image(&f, FSIMG_ISO_VIEW_PRIMARY);
	if (status == FSIMG_OK)
		status = walk(&f);
	CHECK(status == FSIMG_ELIMIT);
	fixture_free(&f);
	CHECK(close(parent) == 0);
	CHECK(rmdir(temporary) == 0);
}

static void
test_faults(void)
{
	image_t image = image_new();

	test_image_faults(&image);
	free(image.bytes);
	image = boot_image(0);
	test_image_faults(&image);
	free(image.bytes);
}

static void
test_exclusivity(void)
{
	static const fsimg_format_t *const formats[] = {
		&fsimg_fat_format, &fsimg_exfat_format, &fsimg_qnx6_format,
		&fsimg_qnx4_format, &fsimg_ifs_format, &fsimg_iso9660_format
	};
	const fsimg_format_variant_t *variant;
	image_t image = image_new();

	for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		CHECK(formats[i]->probe(image.bytes, 17 * BLOCK, image.length,
		    &variant, NULL) == (i == 5 ? FSIMG_OK : FSIMG_EFORMAT));
	}
	free(image.bytes);
}

static void
test_supplementary_info(void)
{
	image_t image = image_new();
	unsigned char *primary = image.bytes + 16 * BLOCK;
	unsigned char *svd = primary + BLOCK;
	fixture_t f;
	fsimg_volume_info_t info;
	static const unsigned char escapes[] = { '@', 'C', 'E' };

	bcopy(primary, svd, BLOCK);
	svd[0] = 2;
	bcopy("other", svd + 88, 5);
	descriptor(svd + BLOCK, 2, 2);
	descriptor(svd + 2 * BLOCK, 255, 1);
	image.bytes[0] = 1;
	for (unsigned level = 0; level <= 3; level++) {
		if (level != 0) {
			bzero(svd + 88, 32);
			bcopy("%/", svd + 88, 2);
			svd[90] = escapes[level - 1];
		}
		fixture_new(&f, &image, NULL);
		CHECK(open_image(&f, FSIMG_ISO_VIEW_PRIMARY) == FSIMG_OK);
		CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
		CHECK(info.details[1].value == level);
		CHECK(info.details[2].value == 1);
		CHECK(info.details[3].value == 3);
		CHECK(info.details[4].value == 19);
		CHECK(info.details[7].value == 1);
		CHECK(walk(&f) == FSIMG_OK);
		fixture_free(&f);
	}
	free(image.bytes);
}

static void
test_smallest_volume(void)
{
	image_t image = image_new();
	unsigned char *p = image.bytes + 16 * BLOCK;

	bcopy(image.bytes + ROOT * BLOCK, image.bytes + 18 * BLOCK, BLOCK);
	bcopy(image.bytes + 82 * BLOCK, image.bytes + 19 * BLOCK, BLOCK);
	bcopy(image.bytes + 83 * BLOCK, image.bytes + 20 * BLOCK, BLOCK);
	bcopy(image.bytes + DATA * BLOCK, image.bytes + 21 * BLOCK, BLOCK);
	put32(p + VD_SPACE, 22);
	put32(p + VD_ROOT + DR_EXTENT, 18);
	p[VD_PATH_L] = 19;
	p[VD_PATH_M + 3] = 20;
	put32(image.bytes + 18 * BLOCK + DR_EXTENT, 18);
	put32(image.bytes + 18 * BLOCK + 34 + DR_EXTENT, 18);
	put32(image.bytes + 18 * BLOCK + 68 + DR_EXTENT, 21);
	image.bytes[19 * BLOCK + 2] = 18;
	image.bytes[20 * BLOCK + 5] = 18;
	image.length = 22 * BLOCK;
	expect(&image, FSIMG_OK, "minimal complete volume");
	check_file(&image, 5, 0, "hello", 5);
	free(image.bytes);
}

static void
permutations(const unsigned char templates[6][BLOCK], unsigned *order,
    unsigned used, unsigned depth)
{
	image_t image;

	if (depth < 6) {
		for (unsigned i = 0; i < 6; i++) {
			if ((used & (1u << i)) != 0)
				continue;
			order[depth] = i;
			permutations(templates, order, used | (1u << i),
			    depth + 1);
		}
		return;
	}
	image = image_new();
	for (unsigned i = 0; i < 6; i++)
		bcopy(templates[order[i]], image.bytes + (16 + i) * BLOCK,
		    BLOCK);
	descriptor(image.bytes + 22 * BLOCK, 255, 1);
	bcopy("BEA01", image.bytes + 23 * BLOCK + 1, 5);
	expect(&image, FSIMG_OK, "all descriptor permutations");
	free(image.bytes);
}

static void
test_descriptor_permutations(void)
{
	unsigned char templates[6][BLOCK];
	unsigned order[6];
	image_t image = image_new();

	bcopy(image.bytes + 16 * BLOCK, templates[0], BLOCK);
	bcopy(templates[0], templates[1], BLOCK);
	templates[1][0] = 2;
	bcopy("%/E", templates[1] + 88, 3);
	descriptor(templates[2], 2, 1);
	descriptor(templates[3], 2, 2);
	descriptor(templates[4], 0, 1);
	descriptor(templates[5], 3, 1);
	permutations(templates, order, 0, 0);
	free(image.bytes);
}

static void
test_section_count_boundary(void)
{
	image_t image = image_new(), prefix;
	unsigned char *root;
	size_t cursor, last = 0;

	image.stored = 4 * 1024 * 1024;
	free(image.bytes);
	/* Preserve the descriptor and path-table prefix of the normal image. */
	prefix = image_new();

	image.bytes = calloc(1, image.stored);
	CHECK(image.bytes != NULL);
	bcopy(prefix.bytes, image.bytes, ROOT * BLOCK);
	free(prefix.bytes);
	root = image.bytes + ROOT * BLOCK;
	image.length = UINT64_C(70000) * BLOCK;
	put32(image.bytes + 16 * BLOCK + VD_SPACE, 70000);
	cursor = 68;
	for (unsigned n = 0; n < 65536; n++) {
		if (cursor % BLOCK + 40 > BLOCK)
			cursor += BLOCK - cursor % BLOCK;
		last = cursor;
		record(root + cursor, "A.TXT;1", 7, 2000 + n, BLOCK, 0x80);
		cursor += 40;
	}
	directory(root, ROOT, ROOT, cursor);
	put32(image.bytes + 16 * BLOCK + VD_ROOT + DR_SIZE, cursor);
	root[last + DR_FLAGS] = 0;
	expect(&image, FSIMG_EUNSUPPORTED, "65536 sections");
	/* Remove the final record, ending the preceding section's run. */
	bzero(root + last, 40);
	last -= 40;
	if (root[last] == 0)
		last -= 8;
	CHECK(root[last] == 40);
	root[last + DR_FLAGS] = 0;
	expect(&image, FSIMG_OK, "65535 sections");
	free(image.bytes);
}

static fsimg_status_t
image_write(void *argument, uint64_t offset, const void *buffer, size_t length,
    size_t *count, fsimg_error_t *error FSIMG_UNUSED)
{
	image_t *image = argument;

	CHECK(offset <= image->stored && length <= image->stored - offset);
	bcopy(buffer, image->bytes + offset, length);
	*count = length;
	return (FSIMG_OK);
}

static fsimg_status_t
image_resize(void *argument, uint64_t length, fsimg_error_t *error FSIMG_UNUSED)
{
	image_t *image = argument;

	CHECK(length <= image->stored);
	image->length = length;
	return (FSIMG_OK);
}

static void
test_other_format_probes(void)
{
	static const char *const names[] = {
		"fat12", "fat16", "fat32", "exfat", "qnx6", "qnx6",
		    "qnx4", "ifs"
	};
	fsimg_ctx_t *ctx;
	fsimg_writer_t *writer;
	fsimg_write_options_t options;
	fsimg_io_ops_t ops;
	fsimg_io_t *io;
	fsimg_plan_info_t info;
	const fsimg_plan_t *plan;
	const fsimg_format_variant_t *variant;
	image_t image;

	for (unsigned n = 0; n < 8; n++) {
		CHECK(fsimg_ctx_new(NULL, &ctx, NULL) == FSIMG_OK);
		fsimg_write_options_init(&options);
		if (n == 5)
			options.byte_order = FSIMG_BYTE_ORDER_BIG;
		CHECK(fsimg_writer_new(ctx, names[n], &options, &writer,
		    NULL) == FSIMG_OK);
		CHECK(fsimg_writer_plan(writer, &plan, NULL) == FSIMG_OK);
		CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
		bzero(&image, sizeof(image));
		image.stored = info.volume_bytes;
		image.length = info.volume_bytes;
		image.bytes = calloc(1, image.stored);
		CHECK(image.bytes != NULL);
		fsimg_io_ops_init(&ops);
		ops.get_size = image_size;
		ops.read_at = image_read;
		ops.write_at = image_write;
		ops.resize = image_resize;
		CHECK(fsimg_io_from_callbacks(ctx, &ops, &image, &io,
		    NULL) == FSIMG_OK);
		CHECK(fsimg_writer_emit(writer, io, NULL) == FSIMG_OK);
		CHECK(fsimg_iso9660_format.probe(image.bytes,
		    image.length < 17 * BLOCK ? image.length : 17 * BLOCK,
		    image.length, &variant, NULL) == FSIMG_EFORMAT);
		fsimg_io_free(io);
		fsimg_writer_free(writer);
		fsimg_ctx_free(ctx);
		free(image.bytes);
	}
}

static void
test_displayed_version_suffix(void)
{
	image_t image = image_new();
	unsigned char *root = image.bytes + ROOT * BLOCK;
	fixture_t f;
	const fsimg_entry_t *entry;
	char temporary[] = "./iso-selection-XXXXXX";
	char buffer[5];
	int parent, fd;

	bzero(root + 68, BLOCK - 68);
	record(root + 68, "A.TXT;1;1", 9, DATA, 5, 0);
	expect(&image, FSIMG_ECORRUPT, "file displayed name ends in ;1");
	bzero(root + 68, BLOCK - 68);
	record(root + 68, "DIR;1;1", 7, ROOT + 1, BLOCK, 2);
	directory(root + BLOCK, ROOT + 1, ROOT, BLOCK);
	expect(&image, FSIMG_ECORRUPT, "directory displayed name ends in ;1");
	bzero(root + 68, BLOCK - 68);
	record(root + 68, "A.TXT;2;1", 9, DATA, 5, 0);
	expect(&image, FSIMG_OK, "displayed version ;2 witness");
	fixture_new(&f, &image, NULL);
	CHECK(open_image(&f, FSIMG_ISO_VIEW_PRIMARY) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(strcmp(entry->path, "/A.TXT;2") == 0);
	CHECK(walk(&f) == FSIMG_OK);
	fixture_free(&f);
	CHECK(mkdtemp(temporary) != NULL);
	parent = open(temporary, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(parent >= 0);
	fixture_new(&f, &image, NULL);
	CHECK(open_image(&f, FSIMG_ISO_VIEW_PRIMARY) == FSIMG_OK);
	CHECK(extract(&f, parent, "/A.TXT;2") == FSIMG_OK);
	fd = openat(parent, "out/A.TXT;2", O_RDONLY | O_NOFOLLOW);
	CHECK(fd >= 0);
	CHECK(read(fd, buffer, sizeof(buffer)) == sizeof(buffer));
	CHECK(bcmp(buffer, "hello", sizeof(buffer)) == 0);
	CHECK(close(fd) == 0);
	fixture_free(&f);
	remove_tree(parent, "out");
	CHECK(close(parent) == 0);
	CHECK(rmdir(temporary) == 0);
	free(image.bytes);
}

static void
test_empty_section_extents(void)
{
	static const char *const names[] = {
		"one-section empty file beyond volume",
		"final empty section beyond volume"
	};

	for (unsigned sections = 1; sections <= 2; sections++) {
		image_t image = image_new();
		unsigned char *file = image.bytes + ROOT * BLOCK + 68;
		fixture_t f;
		const fsimg_entry_t *entry;
		fsimg_status_t status;

		/* A non-final section must contain whole blocks. Its empty
		 * final section contributes no bytes to the listed file
		 * size. */
		if (sections == 2)
			file += record(file, "A.TXT;1", 7, DATA, BLOCK, 0x80);
		record(file, "A.TXT;1", 7, UINT32_MAX, 0, 0);
		file[DR_XATTR] = 255;
		fixture_new(&f, &image, NULL);
		CHECK(open_image(&f, FSIMG_ISO_VIEW_PRIMARY) == FSIMG_OK);
		status = fsimg_reader_next(f.reader, &entry, NULL);
		cases++;
		if (status != FSIMG_OK) {
			fprintf(stderr, "%s: expected %d, observed %d\n",
			    names[sections - 1], FSIMG_OK, status);
			failures++;
		} else {
			CHECK(entry->kind == FSIMG_ENTRY_FILE);
			CHECK(entry->size == (sections == 1 ? 0 : BLOCK));
			CHECK(walk(&f) == FSIMG_OK);
		}
		fixture_free(&f);
		free(image.bytes);
	}
}

static void
test_descriptor_limit_terminator(void)
{
	image_t image = image_new();
	unsigned char *p = image.bytes + 16 * BLOCK;

	for (unsigned i = 1; i < 64; i++)
		descriptor(p + i * BLOCK, 3, 1);
	p += 64 * BLOCK;
	descriptor(p, 255, 1);
	expect(&image, FSIMG_OK, "65th block terminator witness");
	bzero(p, BLOCK);
	expect(&image, FSIMG_EUNSUPPORTED, "65th block lacks CD001");
	image.length = 80 * BLOCK;
	expect(&image, FSIMG_EUNSUPPORTED, "slice ends after 64 descriptors");
	image.length = BLOCKS * BLOCK;
	descriptor(p, 255, 1);
	p[BLOCK - 1] = 1;
	expect(&image, FSIMG_ECORRUPT, "65th block terminator nonzero");
	free(image.bytes);
}

static void
test_label_nul_padding(void)
{
	image_t image = image_new();
	unsigned char *label = image.bytes + 16 * BLOCK + 40;
	fixture_t f;
	fsimg_volume_info_t info;

	for (unsigned interior = 0; interior < 2; interior++) {
		memset(label, interior ? ' ' : 0, 32);
		bcopy(interior ? "CD\0ROM" : "CDROM", label, interior ? 6 : 5);
		fixture_new(&f, &image, NULL);
		CHECK(open_image(&f, FSIMG_ISO_VIEW_PRIMARY) == FSIMG_OK);
		CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
		cases++;
		if (interior ? (info.valid & FSIMG_VOLUME_LABEL) != 0 :
		    (info.valid & FSIMG_VOLUME_LABEL) == 0 ||
		    strcmp(info.label, "CDROM") != 0) {
			fprintf(stderr,
			    "%s: incorrect label presence or value\n",
			    interior ? "interior NUL label" :
			    "NUL-padded label");
			failures++;
		}
		fixture_free(&f);
	}
	free(image.bytes);
}

/* El Torito fixtures: wire fields from the catalog specification, with no
 * driver codecs. Catalog block 100 and image block 104 follow terminator 18.
 */
#define BOOT_CATALOG 100u

static void
boot_le16(unsigned char *p, uint16_t value)
{
	p[0] = value;
	p[1] = value >> 8;
}

static void
boot_le32(unsigned char *p, uint32_t value)
{
	for (unsigned i = 0; i < 4; i++)
		p[i] = value >> (8 * i);
}

static void
boot_record(unsigned char *p, uint32_t catalog)
{
	descriptor(p, 0, 1);
	bcopy("EL TORITO SPECIFICATION", p + 7, 23);
	boot_le32(p + 71, catalog);
}

static void
boot_checksum(unsigned char *p)
{
	uint32_t sum = 0;

	boot_le16(p + 28, 0);
	for (unsigned i = 0; i < 32; i += 2)
		sum += p[i] + 256u * p[i + 1];
	boot_le16(p + 28, (uint16_t)(0u - sum));
}

static void
boot_validation(unsigned char *p, unsigned platform)
{
	bzero(p, 32);
	p[0] = 1;
	p[1] = platform;
	memset(p + 4, 'V', 24);
	p[30] = 0x55;
	p[31] = 0xaa;
	boot_checksum(p);
}

static void
boot_entry(unsigned char *p, unsigned media, uint16_t sectors,
    uint32_t image_block)
{
	bzero(p, 32);
	p[0] = 0x88;
	p[1] = media;
	boot_le16(p + 2, 0x7c0);
	p[4] = 0x83;
	boot_le16(p + 6, sectors);
	boot_le32(p + 8, image_block);
}

static void
boot_header(unsigned char *p, unsigned marker, unsigned platform,
    uint16_t count)
{
	bzero(p, 32);
	p[0] = marker;
	p[1] = platform;
	boot_le16(p + 2, count);
	memset(p + 4, 'S', 28);
}

static image_t
boot_image(unsigned platform)
{
	image_t image = image_new();
	unsigned char *catalog = image.bytes + BOOT_CATALOG * BLOCK;

	boot_record(image.bytes + 17 * BLOCK, BOOT_CATALOG);
	descriptor(image.bytes + 18 * BLOCK, 255, 1);
	boot_validation(catalog, platform);
	boot_entry(catalog + 32, 0, 4, DATA);
	return (image);
}

static void
boot_efi_section(image_t *image)
{
	unsigned char *catalog = image->bytes + BOOT_CATALOG * BLOCK;

	boot_header(catalog + 64, 0x91, 0xef, 1);
	boot_entry(catalog + 96, 0, 4, DATA + 1);
}

/* Assert names, order and values, including zero counters for ignored
 * records. expect() also walks the ordinary hierarchy for each witness.
 */
static void
boot_info(image_t *image, const char *name, uint64_t catalog,
    uint64_t entries, uint64_t x86, uint64_t efi)
{
	static const char *const names[] = {
		"boot_catalog_block", "boot_entries", "boot_x86", "boot_efi"
	};
	const uint64_t values[] = { catalog, entries, x86, efi };
	fsimg_volume_info_t info;
	fixture_t f;

	expect(image, FSIMG_OK, name);
	fixture_new(&f, image, NULL);
	CHECK(open_image(&f, FSIMG_ISO_VIEW_PRIMARY) == FSIMG_OK);
	CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
	CHECK(info.detail_count == 13);
	for (unsigned i = 0; i < 4; i++) {
		if (strcmp(info.details[9 + i].name, names[i]) != 0 ||
		    info.details[9 + i].value != values[i]) {
			fprintf(stderr, "%s: %s expected %" PRIu64
			    ", observed %" PRIu64 "\n", name, names[i],
			    values[i], info.details[9 + i].value);
			failures++;
		}
	}
	fixture_free(&f);
}

static void
test_boot_platforms_and_sections(void)
{
	typedef struct boot_platform_case {
		const char *name;
		unsigned platform;
		unsigned x86;
		unsigned efi;
	} boot_platform_case_t;
	static const boot_platform_case_t platforms[] = {
		{ "BIOS initial entry", 0, 1, 0 },
		{ "EFI initial entry", 0xef, 0, 1 },
		{ "PowerPC initial entry", 1, 0, 0 },
		{ "Mac initial entry", 2, 0, 0 }
	};
	image_t image;
	unsigned char *catalog;

	for (unsigned i = 0; i < 4; i++) {
		image = boot_image(platforms[i].platform);
		boot_info(&image, platforms[i].name, BOOT_CATALOG, 1,
		    platforms[i].x86, platforms[i].efi);
		free(image.bytes);
	}
	image = boot_image(0);
	boot_efi_section(&image);
	boot_info(&image, "BIOS initial and EFI section",
	    BOOT_CATALOG, 2, 1, 1);
	catalog = image.bytes + BOOT_CATALOG * BLOCK;
	boot_header(catalog + 64, 0x90, 1, 1);
	boot_header(catalog + 128, 0x90, 2, 1);
	boot_entry(catalog + 160, 0, 4, DATA);
	boot_header(catalog + 192, 0x91, 0xef, 1);
	boot_entry(catalog + 224, 0, 4, DATA);
	boot_info(&image, "PowerPC Mac and EFI sections",
	    BOOT_CATALOG, 4, 1, 1);
	free(image.bytes);
}

static void
test_boot_entry_variants(void)
{
	image_t image = boot_image(0);
	unsigned char *catalog = image.bytes + BOOT_CATALOG * BLOCK;

	memset(catalog + 44, 0x5a, 20);
	boot_info(&image, "initial entry selection criteria accepted",
	    BOOT_CATALOG, 1, 1, 0);
	for (unsigned media = 0; media <= 4; media++) {
		char name[64];

		CHECK(snprintf(name, sizeof(name), "initial media type %u",
		    media) > 0);
		catalog[33] = media;
		boot_info(&image, name, BOOT_CATALOG, 1, 1, 0);
	}
	catalog[32] = 0;
	boot_info(&image, "non-bootable initial counted",
	    BOOT_CATALOG, 1, 1, 0);
	memset(catalog + 64, 0xa5, BLOCK - 64);
	boot_info(&image, "non-header tail ignored", BOOT_CATALOG, 1, 1, 0);
	bzero(catalog + 64, BLOCK - 64);
	boot_efi_section(&image);
	memset(catalog + 108, 0x5a, 20);
	catalog[96] = 0;
	for (unsigned media = 0; media <= 4; media++) {
		char name[64];

		CHECK(snprintf(name, sizeof(name),
		    "section media %u with flags and criteria", media) > 0);
		catalog[97] = 0xc0 | media;
		boot_info(&image, name, BOOT_CATALOG, 2, 1, 1);
	}
	free(image.bytes);
	image = boot_image(0);
	image.bytes[17 * BLOCK + 7] = 'X';
	boot_info(&image, "different boot system ignored", 0, 0, 0, 0);
	boot_record(image.bytes + 17 * BLOCK, BOOT_CATALOG);
	image.bytes[17 * BLOCK + 38] = 1;
	boot_info(&image, "nonzero boot system padding ignored", 0, 0, 0, 0);
	free(image.bytes);
}

static void
test_boot_catalog_capacity(void)
{
	image_t image = boot_image(0);
	unsigned char *catalog = image.bytes + BOOT_CATALOG * BLOCK;

	boot_header(catalog + 64, 0x91, 0xef, 61);
	for (unsigned i = 3; i < 64; i++)
		boot_entry(catalog + i * 32, 0, 4, DATA);
	boot_info(&image, "62 boot entries fill catalog",
	    BOOT_CATALOG, 62, 1, 61);
	boot_le16(catalog + 66, 62);
	expect_open(&image, FSIMG_EUNSUPPORTED, "62 section entries exceed catalog");
	boot_header(catalog + 64, 0x90, 0xef, 61);
	expect_open(&image, FSIMG_EUNSUPPORTED, "further header exceeds catalog");
	free(image.bytes);
}

static void
test_boot_validation_mutations(void)
{
	typedef struct boot_validation_case {
		const char *name;
		unsigned offset;
		unsigned value;
		bool checksum;
		fsimg_status_t status;
	} boot_validation_case_t;
	static const boot_validation_case_t mutations[] = {
		{ "validation header ID", 0, 2, true, FSIMG_ECORRUPT },
		{ "validation key byte 30", 30, 0, true, FSIMG_ECORRUPT },
		{ "validation key byte 31", 31, 0, true, FSIMG_ECORRUPT },
		{ "validation checksum plus one", 28, 0, false,
		    FSIMG_ECORRUPT },
		{ "validation ID breaks checksum", 4, 'W', false,
		    FSIMG_ECORRUPT },
		{ "validation reserved byte 2", 2, 1, true, FSIMG_ECORRUPT },
		{ "validation reserved byte 3", 3, 1, true, FSIMG_ECORRUPT },
		{ "validation platform unknown", 1, 3, true,
		    FSIMG_EUNSUPPORTED }
	};

	for (unsigned i = 0; i < sizeof(mutations) / sizeof(mutations[0]);
	    i++) {
		image_t image = boot_image(0);
		unsigned char *catalog = image.bytes + BOOT_CATALOG * BLOCK;

		if (mutations[i].offset == 28)
			boot_le16(catalog + 28,
			    (uint16_t)(catalog[28] + 256u * catalog[29] + 1));
		else
			catalog[mutations[i].offset] = mutations[i].value;
		if (mutations[i].checksum)
			boot_checksum(catalog);
		expect_open(&image, mutations[i].status, mutations[i].name);
		free(image.bytes);
	}
}

static void
test_boot_entry_mutations(void)
{
	typedef struct boot_entry_case {
		const char *name;
		unsigned offset;
		unsigned value;
		fsimg_status_t status;
	} boot_entry_case_t;
	static const boot_entry_case_t mutations[] = {
		{ "initial indicator", 32, 0x44, FSIMG_ECORRUPT },
		{ "initial media 5", 33, 5, FSIMG_ECORRUPT },
		{ "initial media 0x10", 33, 0x10, FSIMG_ECORRUPT },
		{ "initial unused byte", 37, 1, FSIMG_ECORRUPT },
		{ "section indicator", 96, 1, FSIMG_ECORRUPT },
		{ "section media nibble 5", 97, 5, FSIMG_ECORRUPT },
		{ "section reserved bit 4", 97, 0x10, FSIMG_ECORRUPT },
		{ "section unused byte", 101, 1, FSIMG_ECORRUPT },
		{ "section extension flag", 97, 0x20, FSIMG_EUNSUPPORTED },
		{ "section count zero", 66, 0, FSIMG_ECORRUPT },
		{ "section platform unknown", 65, 3, FSIMG_EUNSUPPORTED },
		{ "section count past real entries", 66, 2, FSIMG_ECORRUPT }
	};
	image_t image;
	unsigned char *catalog;

	for (unsigned i = 0; i < sizeof(mutations) / sizeof(mutations[0]);
	    i++) {
		image = boot_image(0);
		boot_efi_section(&image);
		catalog = image.bytes + BOOT_CATALOG * BLOCK;
		catalog[mutations[i].offset] = mutations[i].value;
		expect_open(&image, mutations[i].status, mutations[i].name);
		free(image.bytes);
	}
	image = boot_image(0);
	boot_efi_section(&image);
	catalog = image.bytes + BOOT_CATALOG * BLOCK;
	boot_le16(catalog + 66, 65535);
	expect_open(&image, FSIMG_ECORRUPT,
	    "last section count 65535 runs into zero entry");
	boot_header(catalog + 64, 0x90, 0xef, 2);
	boot_header(catalog + 128, 0x91, 0, 1);
	boot_entry(catalog + 160, 0, 4, DATA);
	expect_open(&image, FSIMG_ECORRUPT,
	    "more section count runs over following last header");
	bzero(catalog + 128, 64);
	boot_header(catalog + 64, 0x90, 0xef, 1);
	expect_open(&image, FSIMG_ECORRUPT, "non-header after more section");
	catalog[64] = 0x91;
	image.bytes[17 * BLOCK + 39] = 1;
	expect_open(&image, FSIMG_ECORRUPT, "boot identifier nonzero");
	image.bytes[17 * BLOCK + 39] = 0;
	boot_record(image.bytes + 18 * BLOCK, BOOT_CATALOG);
	descriptor(image.bytes + 19 * BLOCK, 255, 1);
	expect_open(&image, FSIMG_EUNSUPPORTED, "two El Torito boot records");
	free(image.bytes);
}

static void
test_boot_extent_bounds(void)
{
	typedef struct boot_catalog_case {
		const char *name;
		uint32_t block;
	} boot_catalog_case_t;
	static const boot_catalog_case_t catalogs[] = {
		{ "catalog past volume", BLOCKS },
		{ "catalog in system area", 15 },
		{ "catalog on descriptor", 17 },
		{ "catalog on terminator", 18 }
	};
	static const boot_catalog_case_t images[] = {
		{ "image at block zero", 0 },
		{ "image in system area", 15 },
		{ "image on primary descriptor", 16 },
		{ "image on boot record", 17 },
		{ "image on terminator", 18 }
	};
	image_t image = boot_image(0);
	unsigned char *entry = image.bytes + BOOT_CATALOG * BLOCK + 32;

	/* A valid catalog in the system area isolates the data-area rule. */
	bcopy(image.bytes + BOOT_CATALOG * BLOCK, image.bytes + 15 * BLOCK,
	    BLOCK);
	for (unsigned i = 0; i < sizeof(catalogs) / sizeof(catalogs[0]); i++) {
		boot_record(image.bytes + 17 * BLOCK, catalogs[i].block);
		expect_open(&image, FSIMG_ECORRUPT, catalogs[i].name);
	}
	/* Leave the catalog readable in the slice but outside the volume. */
	bcopy(image.bytes + BOOT_CATALOG * BLOCK,
	    image.bytes + (BLOCKS - 1) * BLOCK, BLOCK);
	put32(image.bytes + 16 * BLOCK + VD_SPACE, BLOCKS - 1);
	boot_record(image.bytes + 17 * BLOCK, BLOCKS - 1);
	expect_open(&image, FSIMG_ECORRUPT, "catalog in slice past volume");
	put32(image.bytes + 16 * BLOCK + VD_SPACE, BLOCKS);
	boot_record(image.bytes + 17 * BLOCK, BOOT_CATALOG);
	image.length = BOOT_CATALOG * BLOCK;
	expect_open(&image, FSIMG_ECORRUPT, "catalog past truncated slice");
	image.length = BLOCKS * BLOCK;
	for (unsigned i = 0; i < sizeof(images) / sizeof(images[0]); i++) {
		boot_entry(entry, 0, 4, images[i].block);
		expect_open(&image, FSIMG_ECORRUPT, images[i].name);
	}
	boot_validation(image.bytes + BOOT_CATALOG * BLOCK, 0xef);
	boot_entry(entry, 0, 4, BLOCKS);
	boot_info(&image, "EFI image past volume accepted",
	    BOOT_CATALOG, 1, 0, 1);
	boot_entry(entry, 0, 65535, UINT32_MAX);
	boot_info(&image, "EFI image end past 2^32 blocks accepted",
	    BOOT_CATALOG, 1, 0, 1);
	boot_entry(entry, 0, 5, BLOCKS - 1);
	boot_info(&image, "sector count crossing volume end accepted",
	    BOOT_CATALOG, 1, 0, 1);
	boot_entry(entry, 0, 0, BLOCKS);
	boot_info(&image, "zero sector count past volume accepted",
	    BOOT_CATALOG, 1, 0, 1);
	boot_validation(image.bytes + BOOT_CATALOG * BLOCK, 0);
	boot_entry(entry, 0, 0, BLOCKS - 1);
	boot_info(&image, "zero sectors at last block", BOOT_CATALOG, 1, 1, 0);
	image.length = (BLOCKS - 1) * BLOCK;
	boot_info(&image, "boot image in missing slice tail",
	    BOOT_CATALOG, 1, 1, 0);
	free(image.bytes);
}

static void
test_boot_data_remains_tree_data(void)
{
	image_t image = boot_image(0);
	unsigned char *file = image.bytes + ROOT * BLOCK + 68;
	const fsimg_entry_t *entry;
	fixture_t f;

	/* The only tree file is A.TXT: the boot image is its ordinary data. */
	check_file(&image, 5, 0, "hello", 5);
	fixture_new(&f, &image, NULL);
	CHECK(open_image(&f, FSIMG_ISO_VIEW_PRIMARY) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(strcmp(entry->path, "/A.TXT") == 0);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_DONE);
	fixture_free(&f);
	/* Moving the boot image does not introduce a tree entry. */
	boot_entry(image.bytes + BOOT_CATALOG * BLOCK + 32, 0, 4, DATA + 1);
	check_file(&image, 5, 0, "hello", 5);
	/* Publish the catalog as the sole tree file and compare every byte. */
	bzero(file, BLOCK - 68);
	record(file, "BOOT.CAT;1", 10, BOOT_CATALOG, BLOCK, 0);
	fixture_new(&f, &image, NULL);
	CHECK(open_image(&f, FSIMG_ISO_VIEW_PRIMARY) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(strcmp(entry->path, "/BOOT.CAT") == 0);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_DONE);
	fixture_free(&f);
	check_file(&image, BLOCK, 0, image.bytes + BOOT_CATALOG * BLOCK, BLOCK);
	free(image.bytes);
}
/* End of El Torito fixtures. */

/* SUSP wire encoders are independent of the reader. A child's stream lives
 * in block 110 by default, leaving room for boundary-sized NM and SL data.
 */
#define RR_CHILD 94u
typedef struct susp_area {
	unsigned char bytes[BLOCK];
	size_t length;
} susp_area_t;

static unsigned char *susp_entry(susp_area_t *, const char *, size_t);
static void susp_px(susp_area_t *, unsigned, size_t);
static void susp_nm(susp_area_t *, const void *, size_t, unsigned);
static void susp_ce(susp_area_t *, unsigned, unsigned, unsigned);
static void susp_sl(susp_area_t *, unsigned, unsigned, const void *, size_t);
static void susp_er(susp_area_t *, const char *, unsigned);
static image_t rr_image(const susp_area_t *, unsigned);
static unsigned char *rr_child_directory(image_t *);
static image_t rr_dot_image(const susp_area_t *, bool, bool);
static void rr_status(image_t *, fsimg_iso_view_t, fsimg_status_t,
    const char *);
static void rr_equal(uint64_t, uint64_t, const char *);
static void rr_status_equal(fsimg_status_t, fsimg_status_t, const char *);
static void rr_witness(image_t *, const char *, fsimg_entry_kind_t,
    const void *, size_t, const char *);
static void test_susp_handshake_views(void);
static void test_susp_continuation_bounds(void);
static void test_susp_framing_and_precedence(void);
static void test_rr_kinds_and_singletons(void);
static void test_rr_names_and_continuations(void);
static void test_rr_link_components(void);
static void test_rr_times(void);
static void test_rr_shared_continuation(void);
static void test_rr_omitted_names(void);
static void rr_relocated_chain(unsigned, bool);
static void test_rr_relocation(void);
static void test_rr_dot_entries(void);
static void test_rr_misplaced_dot_names(void);
static void test_rr_faults(void);
static void test_rr_later_sections(void);
static void test_rr_exact_queries(void);
static void test_rr_known_versions(void);
static void susp_seed(const char *);

static unsigned char *
susp_entry(susp_area_t *area, const char *signature, size_t length)
{
	unsigned char *p;

	CHECK(length >= 4 && length <= 255);
	CHECK(area->length + length <= sizeof(area->bytes));
	p = area->bytes + area->length;
	bzero(p, length);
	bcopy(signature, p, 2);
	p[2] = length;
	p[3] = 1;
	area->length += length;
	return (p);
}

static void
susp_px(susp_area_t *area, unsigned mode, size_t length)
{
	unsigned char *p = susp_entry(area, "PX", length);

	put32(p + 4, mode);
	put32(p + 12, 1);
	put32(p + 20, 123);
	put32(p + 28, 456);
}

static void
susp_nm(susp_area_t *area, const void *name, size_t length, unsigned flags)
{
	unsigned char *p = susp_entry(area, "NM", 5 + length);

	p[4] = flags;
	bcopy(name, p + 5, length);
}

static void
susp_ce(susp_area_t *area, unsigned block, unsigned offset, unsigned length)
{
	unsigned char *p = susp_entry(area, "CE", 28);

	put32(p + 4, block);
	put32(p + 12, offset);
	put32(p + 20, length);
}

static void
susp_sl(susp_area_t *area, unsigned flags, unsigned component,
    const void *text, size_t length)
{
	unsigned char *p = susp_entry(area, "SL", 7 + length);

	p[4] = flags;
	p[5] = component;
	p[6] = length;
	bcopy(text, p + 7, length);
}

static void
susp_er(susp_area_t *area, const char *identifier, unsigned version)
{
	size_t length = strlen(identifier);
	unsigned char *p = susp_entry(area, "ER", 8 + length);

	p[4] = length;
	p[7] = version;
	bcopy(identifier, p + 8, length);
}

static image_t
rr_image(const susp_area_t *area, unsigned flags)
{
	image_t image = image_new();
	susp_area_t handshake = {0}, continuation = {0};
	unsigned char *root = image.bytes + ROOT * BLOCK;
	unsigned char *p, self = 0, up = 1;
	size_t length;

	p = susp_entry(&handshake, "SP", 7);
	p[4] = 0xbe;
	p[5] = 0xef;
	susp_er(&handshake, "RRIP_1991A", 1);
	CHECK(handshake.length == 25);
	bzero(root, BLOCK);
	length = record(root, &self, 1, ROOT, BLOCK, 2);
	bcopy(handshake.bytes, root + length, handshake.length);
	root[0] += handshake.length + (handshake.length % 2);
	record(root + root[0], &up, 1, ROOT, BLOCK, 2);
	susp_ce(&continuation, RR_AREA, 0, area->length);
	p = root + RR_CHILD;
	length = record(p, "A.TXT;1", 7, DATA, 5, flags);
	bcopy(continuation.bytes, p + length, continuation.length);
	p[0] += continuation.length;
	bcopy(area->bytes, image.bytes + RR_AREA * BLOCK, area->length);
	return (image);
}

static unsigned char *
rr_child_directory(image_t *image)
{
	unsigned char *child = image->bytes + ROOT * BLOCK + RR_CHILD;
	unsigned char *contents = image->bytes + RR_DIRECTORY * BLOCK;

	put32(child + DR_EXTENT, RR_DIRECTORY);
	put32(child + DR_SIZE, BLOCK);
	directory(contents, RR_DIRECTORY, ROOT, BLOCK);
	return (contents);
}

/* Build a root with one valid child directory, placing the supplied SUSP
 * area on exactly one of the four dot records. The root handshake precedes
 * any supplied entries on its first dot record.
 */
static image_t
rr_dot_image(const susp_area_t *dot, bool root, bool parent)
{
	image_t image = image_new();
	susp_area_t area;
	unsigned char *p;
	size_t length;

	for (unsigned level = 0; level < 2; level++) {
		unsigned extent = level == 0 ? ROOT : RR_DIRECTORY;

		p = image.bytes + extent * BLOCK;
		bzero(p, BLOCK);
		for (unsigned char identifier = 0; identifier < 2; identifier++) {
			bzero(&area, sizeof(area));
			if (level == 0 && identifier == 0) {
				unsigned char *sp = susp_entry(&area, "SP", SP_BYTES);

				sp[4] = 0xbe;
				sp[5] = 0xef;
				susp_er(&area, "RRIP_1991A", 1);
			}
			if (root == (level == 0) && parent == (identifier == 1)) {
				bcopy(dot->bytes, area.bytes + area.length, dot->length);
				area.length += dot->length;
			}
			length = record(p, &identifier, 1,
			    identifier == 0 ? extent : ROOT, BLOCK, 2);
			bcopy(area.bytes, p + length, area.length);
			length += area.length;
			p[0] = length + (length % 2);
			p += p[0];
		}
		if (level == 0)
			record(p, "CHILD", 5, RR_DIRECTORY, BLOCK, 2);
	}
	return (image);
}

static void
rr_equal(uint64_t actual, uint64_t expected, const char *name)
{
	cases++;
	if (actual != expected) {
		fprintf(stderr, "%s: expected %" PRIu64
		    ", observed %" PRIu64 "\n",
		    name, expected, actual);
		failures++;
	}
}

static void
rr_status_equal(fsimg_status_t actual, fsimg_status_t expected,
    const char *name)
{
	cases++;
	if (actual != expected) {
		fprintf(stderr, "%s: expected %d, observed %d\n", name,
		    expected, actual);
		failures++;
	}
}

static void
rr_status(image_t *image, fsimg_iso_view_t view, fsimg_status_t expected,
    const char *name)
{
	fixture_t f;
	fsimg_status_t status;

	fixture_new(&f, image, NULL);
	status = open_image(&f, view);
	if (status == FSIMG_OK)
		status = walk(&f);
	cases++;
	if (status != expected) {
		fprintf(stderr, "%s: expected %d, observed %d\n", name,
		    expected, status);
		failures++;
	}
	fixture_free(&f);
}

static void
rr_witness(image_t *image, const char *path, fsimg_entry_kind_t kind,
    const void *content, size_t size, const char *name)
{
	fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_status_t status;
	unsigned char buffer[2048];
	size_t count;

	fixture_new(&f, image, NULL);
	status = open_image(&f, FSIMG_ISO_VIEW_ROCKRIDGE);
	if (status == FSIMG_OK)
		status = fsimg_reader_next(f.reader, &entry, NULL);
	rr_status_equal(status, FSIMG_OK, name);
	if (status == FSIMG_OK) {
		rr_equal(strcmp(entry->path, path), 0, name);
		rr_equal(entry->kind, kind, name);
		rr_equal(entry->size, size, name);
		if (content != NULL) {
			status = fsimg_reader_read_at(f.reader, 0, buffer,
			    sizeof(buffer), &count, NULL);
			rr_status_equal(status, FSIMG_OK, name);
			if (status == FSIMG_OK) {
				rr_equal(count, size, name);
				rr_equal(count == size &&
				    bcmp(buffer, content, size) == 0, 1, name);
			}
		}
		rr_status_equal(walk(&f), FSIMG_OK, name);
	}
	fixture_free(&f);
}

static void
test_susp_handshake_views(void)
{
	static const struct {
		const char *label;
		const char *identifier;
		unsigned offset, value, detail;
		fsimg_status_t automatic, forced;
	} rows[] = {
		{ "SP alone", NULL, ROOT_ER, 0, 0,
		    FSIMG_OK, FSIMG_EUNSUPPORTED },
		{ "opaque bad SP", NULL, ROOT_SP + 4, 0, 0,
		    FSIMG_OK, FSIMG_EUNSUPPORTED },
		{ "ER length below SUSP header", NULL,
		    ROOT_ER + SUSP_LENGTH, 3, 0, FSIMG_ECORRUPT,
		    FSIMG_ECORRUPT },
		{ "unsupported SP skip", NULL, ROOT_SP + 6, 1, 2,
		    FSIMG_OK, FSIMG_EUNSUPPORTED },
		{ "RRIP identifier", "RRIP_1991A", 0, 0, 1, FSIMG_OK,
		    FSIMG_OK },
		{ "P1282 identifier", "IEEE_P1282", 0, 0, 1, FSIMG_OK,
		    FSIMG_OK },
		{ "1282 identifier", "IEEE_1282", 0, 0, 1, FSIMG_OK, FSIMG_OK },
		{ "unknown identifier", "NOT_RRIP", 0, 0, 0,
		    FSIMG_OK, FSIMG_EUNSUPPORTED }
	};
	static const struct {
		const char *label;
		fsimg_iso_view_t view;
	} views[] = {
		{ "auto", FSIMG_ISO_VIEW_AUTO },
		{ "forced RR", FSIMG_ISO_VIEW_ROCKRIDGE },
		{ "primary", FSIMG_ISO_VIEW_PRIMARY }
	};
	/* Field 3 is the entry version; 4..6 are lengths; 7 is ER version. */
	static const struct {
		const char *label;
		unsigned field, value;
		fsimg_status_t automatic, forced;
	} fields[] = {
		{ "ER entry version", 3, 255, FSIMG_ECORRUPT, FSIMG_ECORRUPT },
		{ "ER identifier bounds", 4, 255, FSIMG_ECORRUPT,
		    FSIMG_ECORRUPT },
		{ "ER description bounds", 5, 255, FSIMG_ECORRUPT,
		    FSIMG_ECORRUPT },
		{ "ER source bounds", 6, 255, FSIMG_ECORRUPT, FSIMG_ECORRUPT },
		{ "ER unknown extension version", 7, 2,
		    FSIMG_OK, FSIMG_EUNSUPPORTED }
	};
	susp_area_t area = {0};
	image_t image;
	fixture_t f;
	fsimg_volume_info_t info;
	unsigned char *root;
	char label[128];

	susp_px(&area, 0100640, PX_BYTES);
	for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		image = rr_image(&area, 0);
		root = image.bytes + ROOT * BLOCK;
		if (rows[i].identifier != NULL) {
			susp_area_t er = {0};

			susp_er(&er, rows[i].identifier, 1);
			bzero(root + ROOT_ER, ROOT_DOT_BYTES - ROOT_ER);
			bcopy(er.bytes, root + ROOT_ER, er.length);
		} else
			root[rows[i].offset] = rows[i].value;
		for (unsigned v = 0; v < sizeof(views) / sizeof(views[0]);
		    v++) {
			fsimg_status_t expected = rows[i].automatic, status;
			unsigned detail = rows[i].detail;

			if (views[v].view == FSIMG_ISO_VIEW_ROCKRIDGE)
				expected = rows[i].forced;
			if (views[v].view == FSIMG_ISO_VIEW_PRIMARY) {
				expected = FSIMG_OK;
				detail = 3;
			}
			snprintf(label, sizeof(label), "%s / %s",
			    rows[i].label, views[v].label);
			fixture_new(&f, &image, NULL);
			status = open_image(&f, views[v].view);
			rr_status_equal(status, expected, label);
			if (status == FSIMG_OK) {
				CHECK(fsimg_reader_get_info(f.reader, &info,
				    NULL) == FSIMG_OK);
				rr_equal(info.details[0].value, detail, label);
				rr_equal(strcmp(info.profile,
				    detail == 1 ? "rockridge" : "primary"),
				    0, label);
				rr_status_equal(walk(&f), FSIMG_OK, label);
			}
			fixture_free(&f);
		}
		free(image.bytes);
	}
	for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		image = rr_image(&area, 0);
		image.bytes[ROOT * BLOCK + ROOT_ER + fields[i].field] =
		    fields[i].value;
		rr_status(&image, FSIMG_ISO_VIEW_AUTO,
		    fields[i].automatic, fields[i].label);
		rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE,
		    fields[i].forced, fields[i].label);
		free(image.bytes);
	}
}

static void
test_susp_continuation_bounds(void)
{
	enum setup {
		CROSS_BLOCK, EMPTY, BAD_LENGTH, BAD_VERSION, SELF, OVERLAP,
		ORIGINAL, DUPLICATE, HANDSHAKE, OFFSET_OUTSIDE
	};
	static const struct {
		const char *label;
		enum setup setup;
		fsimg_status_t expected;
	} rows[] = {
		{ "CE crosses block", CROSS_BLOCK, FSIMG_EUNSUPPORTED },
		{ "CE empty", EMPTY, FSIMG_ECORRUPT },
		{ "CE wrong length", BAD_LENGTH, FSIMG_ECORRUPT },
		{ "CE version", BAD_VERSION, FSIMG_ECORRUPT },
		{ "CE self reference", SELF, FSIMG_ECORRUPT },
		{ "CE overlap", OVERLAP, FSIMG_ECORRUPT },
		{ "CE original area overlap", ORIGINAL, FSIMG_ECORRUPT },
		{ "two CE in area", DUPLICATE, FSIMG_ECORRUPT },
		{ "ER in continuation", HANDSHAKE, FSIMG_OK },
		{ "CE offset at block end", OFFSET_OUTSIDE, FSIMG_ECORRUPT }
	};
	susp_area_t area = {0}, next = {0};
	image_t image;
	unsigned char *p;

	susp_px(&area, 0100644, PX_BYTES);
	for (unsigned count = 16; count <= 17; count++) {
		image = rr_image(&area, 0);
		for (unsigned i = 0; i < count; i++) {
			bzero(&next, sizeof(next));
			if (i + 1 < count)
				susp_ce(&next, 20 + i + 1, 0, CE_BYTES);
			else
				susp_entry(&next, "PD", CE_BYTES);
			bcopy(next.bytes, image.bytes + (20 + i) * BLOCK,
			    CE_BYTES);
		}
		p = image.bytes + ROOT * BLOCK + RR_CHILD + CHILD_SUSP;
		put32(p + 4, 20);
		put32(p + 20, CE_BYTES);
		rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE,
		    count == 16 ? FSIMG_OK : FSIMG_EUNSUPPORTED,
		    count == 16 ? "exactly 16 CE areas" : "17 CE areas");
		if (count == 16)
			rr_witness(&image, "/A.TXT", FSIMG_ENTRY_FILE,
			    "hello", 5,
			    "16 CE areas preserve content");
		free(image.bytes);
	}
	for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		image = rr_image(&area, 0);
		p = image.bytes + ROOT * BLOCK + RR_CHILD + CHILD_SUSP;
		switch (rows[i].setup) {
		case CROSS_BLOCK: put32(p + 12, BLOCK - 1); break;
		case EMPTY: put32(p + 20, 0); break;
		case BAD_LENGTH: p[SUSP_LENGTH] = 26; break;
		case BAD_VERSION: p[SUSP_VERSION] = 2; break;
		case SELF:
		case OVERLAP:
			/* Both ranges have valid framing: CE + PD and its PD
			 * suffix (or the same stream for SELF). Without overlap
			 * rejection, only SELF cycles.
			 */
			bzero(&next, sizeof(next));
			if (rows[i].setup == SELF)
				susp_ce(&next, RR_AREA, 0, PX_BYTES);
			else
				susp_ce(&next, RR_AREA, CE_BYTES, 8);
			susp_entry(&next, "PD", 8);
			bcopy(next.bytes, image.bytes + RR_AREA * BLOCK,
			    next.length);
			break;
		case ORIGINAL:
			put32(p + 4, ROOT);
			put32(p + 12, RR_CHILD + CHILD_SUSP);
			put32(p + 20, CE_BYTES);
			break;
		case DUPLICATE:
			bcopy(p, p + CE_BYTES, CE_BYTES);
			image.bytes[ROOT * BLOCK + RR_CHILD] += CE_BYTES;
			break;
		case HANDSHAKE:
			bzero(&next, sizeof(next));
			susp_er(&next, "RRIP_1991A", 1);
			bcopy(next.bytes, image.bytes + 20 * BLOCK,
			    next.length);
			bzero(&next, sizeof(next));
			susp_ce(&next, 20, 0, 18);
			/* Grow root ER into CE, preserving the siblings. */
			p = image.bytes + ROOT * BLOCK;
			memmove(p + ROOT_CE_DOT_BYTES, p + ROOT_DOT_BYTES,
			    BLOCK - ROOT_CE_DOT_BYTES);
			p[0] = ROOT_CE_DOT_BYTES;
			bcopy(next.bytes, p + ROOT_ER, CE_BYTES);
			break;
		case OFFSET_OUTSIDE: put32(p + 12, BLOCK); break;
		}
		rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE,
		    rows[i].expected, rows[i].label);
		free(image.bytes);
	}
}

static void
test_susp_framing_and_precedence(void)
{
	static const struct {
		const char *label, *prefix;
		unsigned mode, flags, tail_length, tail_version;
		bool stop;
		fsimg_status_t expected;
	} rows[] = {
		{ "unknown and PD skipped", "XY", 0, 0, 4, 1, false,
		    FSIMG_OK },
		{ "ST ends stream", "XY", 0, 0, 3, 1, true, FSIMG_OK },
		{ "entry below 4", "XY", 0, 0, 3, 1, false, FSIMG_ECORRUPT },
		{ "entry past area", "XY", 0, 0, 5, 1, false, FSIMG_ECORRUPT },
		{ "ZF then corrupt PX", "ZF", 0, 0, 36, 2, false,
		    FSIMG_ECORRUPT },
		{ "SF then corrupt PX", "SF", 0, 0, 36, 2, false,
		    FSIMG_ECORRUPT },
		{ "device then corrupt PX", NULL, 0020600, 0, 36, 2, false,
		    FSIMG_ECORRUPT },
		{ "ZF refused", "ZF", 0, 0, 0, 0, false, FSIMG_EUNSUPPORTED },
		{ "SF refused", "SF", 0, 0, 0, 0, false, FSIMG_EUNSUPPORTED },
		{ "ZF with directory PX but clear flag", "ZF", 0040755, 0,
		    0, 0, false, FSIMG_ECORRUPT },
		{ "device PX with directory flag", NULL, 0020600, 2,
		    0, 0, false, FSIMG_ECORRUPT }
	};
	susp_area_t area;
	image_t image;
	unsigned char *p;

	for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		bzero(&area, sizeof(area));
		if (rows[i].mode != 0)
			susp_px(&area, rows[i].mode, PX_BYTES);
		if (rows[i].prefix != NULL)
			susp_entry(&area, rows[i].prefix, 4);
		if (rows[i].tail_length != 0) {
			if (strcmp(rows[i].prefix == NULL ? "" :
			    rows[i].prefix, "XY") == 0) {
				susp_entry(&area, "PD", 4);
				if (rows[i].stop)
					susp_entry(&area, "ST", 4);
				p = susp_entry(&area, "XY", 4);
			} else
				p = susp_entry(&area, "PX", PX_BYTES);
			p[SUSP_LENGTH] = rows[i].tail_length;
			p[SUSP_VERSION] = rows[i].tail_version;
		}
		image = rr_image(&area, rows[i].flags);
		if (rows[i].flags != 0) {
			/* Keep ISO directory geometry valid so only PX's kind
			 * contradicts the directory flag.
			 */
			rr_child_directory(&image);
		}
		rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE,
		    rows[i].expected, rows[i].label);
		free(image.bytes);
	}
}

static void
test_rr_kinds_and_singletons(void)
{
	static const struct {
		const char *label;
		unsigned mode, px_length;
		fsimg_entry_kind_t kind;
		const char *content;
		size_t size;
		bool device;
	} kinds[] = {
		{ "PX regular file", 0100640, 36, FSIMG_ENTRY_FILE,
		    "hello", 5, false },
		{ "PX directory", 0040750, 44, FSIMG_ENTRY_DIRECTORY,
		    NULL, 0, false },
		{ "PX symlink", 0120777, 36, FSIMG_ENTRY_SYMLINK,
		    "a", 1, false },
		{ "PX FIFO", 0010600, 44, FSIMG_ENTRY_FIFO, NULL, 0, false },
		{ "PX character device refused", 0020600, 36, 0, NULL, 0,
		    true },
		{ "PX block device refused", 0060600, 44, 0, NULL, 0, true },
		{ "PX socket refused", 0140600, 36, 0, NULL, 0, false },
		{ "PX unknown type refused", 0030600, 44, 0, NULL, 0, false }
	};
	static const struct {
		const char *label, *signature;
		unsigned length, mode, flags;
		fsimg_status_t single;
	} singletons[] = {
		{ "duplicate PX", "PX", 36, 0, 0, FSIMG_OK },
		{ "duplicate TF", "TF", 5, 0, 0, FSIMG_OK },
		{ "duplicate PL", "PL", 12, 0, 0, FSIMG_OK },
		{ "duplicate RE on directory", "RE", 4, 0040755, 2, FSIMG_OK },
		{ "duplicate PN on device", "PN", 20, 0020600, 0,
		    FSIMG_EUNSUPPORTED }
	};
	static const struct {
		const char *label, *extra;
		unsigned mode, length, version;
	} invalid[] = {
		{ "PX length 40", NULL, 0100644, 40, 1 },
		{ "PX dir flag mismatch", NULL, 0040755, 36, 1 },
		{ "link missing SL", NULL, 0120777, 36, 1 },
		{ "SL on file", "SL", 0100644, 36, 1 },
		{ "PN on file", "PN", 0100644, 36, 1 },
		{ "RE on file", "RE", 0100644, 36, 1 },
		{ "known entry version 2", NULL, 0100644, 36, 2 }
	};
	susp_area_t area;
	image_t image;
	fixture_t f;
	const fsimg_entry_t *entry;
	unsigned char *p;

	for (unsigned i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
		bzero(&area, sizeof(area));
		susp_px(&area, kinds[i].mode, kinds[i].px_length);
		if (kinds[i].kind == FSIMG_ENTRY_SYMLINK)
			susp_sl(&area, 0x00, 0x00, "a", 1);
		if (kinds[i].device)
			susp_entry(&area, "PN", 20);
		image = rr_image(&area,
		    kinds[i].kind == FSIMG_ENTRY_DIRECTORY ? 2 : 0);
		if (kinds[i].kind == FSIMG_ENTRY_DIRECTORY)
			rr_child_directory(&image);
		if (kinds[i].kind != 0) {
			rr_witness(&image, "/A.TXT", kinds[i].kind,
			    kinds[i].content, kinds[i].size, kinds[i].label);
			fixture_new(&f, &image, NULL);
			if (open_image(&f, FSIMG_ISO_VIEW_ROCKRIDGE) ==
			    FSIMG_OK &&
			    fsimg_reader_next(f.reader, &entry, NULL) ==
			    FSIMG_OK) {
				rr_equal(entry->metadata.valid &
				    (FSIMG_META_OWNER | FSIMG_META_MODE),
				    FSIMG_META_OWNER | FSIMG_META_MODE,
				    kinds[i].label);
				rr_equal(entry->metadata.uid, 123,
				    kinds[i].label);
				rr_equal(entry->metadata.gid, 456,
				    kinds[i].label);
				rr_equal(entry->metadata.mode,
				    kinds[i].mode & 07777,
				    kinds[i].label);
			}
			fixture_free(&f);
		} else
			rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE,
			    FSIMG_EUNSUPPORTED, kinds[i].label);
		free(image.bytes);
	}
	for (unsigned i = 0; i < sizeof(singletons) / sizeof(singletons[0]);
	    i++) {
		bzero(&area, sizeof(area));
		if (singletons[i].mode != 0)
			susp_px(&area, singletons[i].mode, PX_BYTES);
		p = susp_entry(&area, singletons[i].signature,
		    singletons[i].length);
		if (strcmp(singletons[i].signature, "PX") == 0) {
			/* A valid ordinary-file PX, not a zero file type. */
			area.length = 0;
			susp_px(&area, 0100644, PX_BYTES);
		}
		image = rr_image(&area, singletons[i].flags);
		/* RE marks this physical alias; it is omitted from the tree. */
		if (singletons[i].flags != 0)
			rr_child_directory(&image);
		rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE,
		    singletons[i].single, singletons[i].label);
		free(image.bytes);
		/* Copy the valid singleton, preserving all its wire fields. */
		p = area.bytes + area.length - singletons[i].length;
		bcopy(p, area.bytes + area.length, singletons[i].length);
		area.length += singletons[i].length;
		image = rr_image(&area, singletons[i].flags);
		if (singletons[i].flags != 0)
			rr_child_directory(&image);
		rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE,
		    FSIMG_ECORRUPT, singletons[i].label);
		free(image.bytes);
	}
	for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		bzero(&area, sizeof(area));
		susp_px(&area, invalid[i].mode, invalid[i].length);
		area.bytes[SUSP_VERSION] = invalid[i].version;
		if (invalid[i].extra != NULL) {
			if (strcmp(invalid[i].extra, "SL") == 0)
				susp_sl(&area, 0x00, 0x00, "a", 1);
			else
				susp_entry(&area, invalid[i].extra,
				    strcmp(invalid[i].extra, "PN") ==
				    0 ? 20 : 4);
		}
		image = rr_image(&area, 0);
		rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE, FSIMG_ECORRUPT,
		    invalid[i].label);
		free(image.bytes);
	}
}

static void
test_rr_names_and_continuations(void)
{
	static const struct {
		const char *label, *text;
		size_t length;
		unsigned flags;
		bool second;
	} invalid[] = {
		{ "empty NM", "", 0, 0, false },
		{ "dot NM", ".", 1, 0, false },
		{ "parent NM", "..", 2, 0, false },
		{ "slash NM", "a/b", 3, 0, false },
		{ "NUL NM", "a\0b", 3, 0, false },
		{ "dangling NM", "a", 1, 1, false },
		{ "current NM outside dot", "a", 1, 2, false },
		{ "parent NM outside dot", "a", 1, 4, false },
		{ "NM after ended name", "a", 1, 0, true }
	};
	susp_area_t area = {0}, next = {0};
	image_t image;
	unsigned char text[1025];
	char path[1027];

	susp_nm(&area, "caf\xc3\xa9", 5, 0);
	image = rr_image(&area, 0);
	rr_witness(&image, "/caf\xc3\xa9", FSIMG_ENTRY_FILE, "hello", 5,
	    "UTF-8 NM witness");
	free(image.bytes);
	memset(text, 'n', sizeof(text));
	for (unsigned size = 1024; size <= 1025; size++) {
		bzero(&area, sizeof(area));
		for (unsigned offset = 0; offset < size;) {
			unsigned count = size - offset > 200 ?
			    200 : size - offset;

			susp_nm(&area, text + offset, count,
			    offset + count < size);
			offset += count;
		}
		image = rr_image(&area, 0);
		path[0] = '/';
		bcopy(text, path + 1, size);
		path[size + 1] = 0;
		if (size == 1024)
			rr_witness(&image, path, FSIMG_ENTRY_FILE, "hello",
			    5, "1024 byte NM");
		else
			rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE,
			    FSIMG_EUNSUPPORTED, "1025 byte NM");
		free(image.bytes);
		if (size == 1025) {
			/* TF selects modify but omits its seven-byte date. */
			susp_entry(&area, "TF", 5)[4] = 2;
			image = rr_image(&area, 0);
			rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE,
			    FSIMG_ECORRUPT, "overlong NM then corrupt TF");
			free(image.bytes);
		}
	}
	for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		bzero(&area, sizeof(area));
		susp_nm(&area, invalid[i].text, invalid[i].length,
		    invalid[i].flags);
		if (invalid[i].second)
			susp_nm(&area, "b", 1, 0);
		image = rr_image(&area, 0);
		rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE, FSIMG_ECORRUPT,
		    invalid[i].label);
		free(image.bytes);
	}
	bzero(&area, sizeof(area));
	susp_nm(&area, "exact;", 6, 1);
	susp_ce(&area, 111, 0, 6);
	susp_nm(&next, "1", 1, 0);
	image = rr_image(&area, 0);
	bcopy(next.bytes, image.bytes + 111 * BLOCK, next.length);
	rr_witness(&image, "/exact;1", FSIMG_ENTRY_FILE, "hello", 5,
	    "NM across CE exact bytes");
	free(image.bytes);
}

static void
test_rr_link_components(void)
{
	static const struct {
		const char *label, *target, *second;
		unsigned flags, length;
		fsimg_status_t expected;
	} components[] = {
		{ "SL ordinary", "a/b", "b", 0x00, 1, FSIMG_OK },
		{ "SL continued component", "ab", "b", 0x01, 1, FSIMG_OK },
		{ "SL current", "./b", "b", 0x02, 0, FSIMG_OK },
		{ "SL parent", "../b", "b", 0x04, 0, FSIMG_OK },
		{ "SL root", "/a", "a", 0x08, 0, FSIMG_OK },
		{ "SL volume root refused", NULL, "b", 0x10, 0,
		    FSIMG_EUNSUPPORTED },
		{ "SL host refused", NULL, "b", 0x20, 0, FSIMG_EUNSUPPORTED },
		{ "SL volume root continued refused", NULL, "b", 0x11, 0,
		    FSIMG_EUNSUPPORTED },
		{ "SL host continued refused", NULL, "b", 0x21, 0,
		    FSIMG_EUNSUPPORTED },
		{ "SL current plus parent", NULL, "b", 0x06, 0, FSIMG_ECORRUPT },
		{ "SL current plus root", NULL, "b", 0x0a, 0, FSIMG_ECORRUPT },
		{ "SL parent plus root", NULL, "b", 0x0c, 0, FSIMG_ECORRUPT },
		{ "SL volume root parent current", NULL, "b", 0x16, 0,
		    FSIMG_ECORRUPT },
		{ "SL volume root plus root", NULL, "b", 0x18, 0,
		    FSIMG_ECORRUPT },
		{ "SL volume root plus host", NULL, "b", 0x30, 0,
		    FSIMG_ECORRUPT }
	};
	static const struct {
		const char *label;
		unsigned length, flags, component, text_length, text;
		bool second;
	} invalid[] = {
		{ .label = "dangling SL entry", .length = 8,
		    .flags = 0x01, .component = 0x00, .text_length = 1,
		    .text = 'a', .second = false },
		{ .label = "dangling SL component", .length = 8,
		    .flags = 0x00, .component = 0x01, .text_length = 1,
		    .text = 'a', .second = false },
		{ .label = "SL component overrun", .length = 8,
		    .flags = 0x00, .component = 0x00, .text_length = 2,
		    .text = 'a', .second = false },
		{ .label = "empty SL target", .length = 5,
		    .flags = 0x00, .component = 0x00, .text_length = 0,
		    .text = 0, .second = false },
		{ .label = "SL component NUL", .length = 8,
		    .flags = 0x00, .component = 0x00, .text_length = 1,
		    .text = 0, .second = false },
		{ .label = "SL after ended target", .length = 8,
		    .flags = 0x00, .component = 0x00, .text_length = 1,
		    .text = 'a', .second = true },
		{ .label = "SL multiple meanings", .length = 8,
		    .flags = 0x00, .component = 0x0a, .text_length = 1,
		    .text = 'a', .second = false },
		{ .label = "SL current NUL", .length = 8,
		    .flags = 0x00, .component = 0x02, .text_length = 1,
		    .text = 0, .second = false },
		{ .label = "SL parent NUL", .length = 8,
		    .flags = 0x00, .component = 0x04, .text_length = 1,
		    .text = 0, .second = false },
		{ .label = "SL root NUL", .length = 8,
		    .flags = 0x00, .component = 0x08, .text_length = 1,
		    .text = 0, .second = false },
		{ .label = "SL volume root NUL", .length = 8,
		    .flags = 0x00, .component = 0x10, .text_length = 1,
		    .text = 0, .second = false },
		{ .label = "SL host NUL", .length = 8,
		    .flags = 0x00, .component = 0x20, .text_length = 1,
		    .text = 0, .second = false },
		{ .label = "SL volume root dangling component", .length = 7,
		    .flags = 0x00, .component = 0x11, .text_length = 0,
		    .text = 0, .second = false },
		{ .label = "SL host dangling component", .length = 7,
		    .flags = 0x00, .component = 0x21, .text_length = 0,
		    .text = 0, .second = false }
	};
	susp_area_t area, next;
	image_t image;
	unsigned char *p, text[1025];

	for (unsigned i = 0; i < sizeof(components) / sizeof(components[0]);
	    i++) {
		bzero(&area, sizeof(area));
		susp_px(&area, 0120777, 44);
		susp_sl(&area, 0x01, components[i].flags, "a",
		    components[i].length);
		susp_sl(&area, 0x00, 0x00, components[i].second, 1);
		image = rr_image(&area, 0);
		if (components[i].expected == FSIMG_OK)
			rr_witness(&image, "/A.TXT", FSIMG_ENTRY_SYMLINK,
			    components[i].target, strlen(components[i].target),
			    components[i].label);
		else
			rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE,
			    components[i].expected, components[i].label);
		free(image.bytes);
		if (components[i].flags == 0x20) {
			susp_entry(&area, "TF", 5)[4] = 2;
			image = rr_image(&area, 0);
			rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE,
			    FSIMG_ECORRUPT, "host SL then corrupt TF");
			free(image.bytes);
		}
	}
	for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		bzero(&area, sizeof(area));
		susp_px(&area, 0120777, PX_BYTES);
		p = susp_entry(&area, "SL", invalid[i].length);
		p[4] = invalid[i].flags;
		if (invalid[i].length > 5) {
			p[5] = invalid[i].component;
			p[6] = invalid[i].text_length;
			if (invalid[i].length > 7)
				p[7] = invalid[i].text;
		}
		if (invalid[i].second)
			susp_sl(&area, 0x00, 0x00, "b", 1);
		image = rr_image(&area, 0);
		rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE, FSIMG_ECORRUPT,
		    invalid[i].label);
		free(image.bytes);
	}
	bzero(&area, sizeof(area));
	susp_px(&area, 0120777, PX_BYTES);
	susp_sl(&area, 0x00, 0x20, "", 0);
	image = rr_image(&area, 0);
	rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE, FSIMG_EUNSUPPORTED,
	    "SL lone host refused");
	free(image.bytes);
	memset(text, 'a', sizeof(text));
	for (unsigned size = 1024; size <= 1025; size++) {
		bzero(&area, sizeof(area));
		susp_px(&area, 0120777, 36);
		for (unsigned offset = 0; offset < size;) {
			unsigned count = size - offset > 200 ?
			    200 : size - offset;
			bool more = offset + count < size;

			susp_sl(&area, more, more, text + offset, count);
			offset += count;
		}
		image = rr_image(&area, 0);
		if (size == 1024)
			rr_witness(&image, "/A.TXT", FSIMG_ENTRY_SYMLINK,
			    text, size, "1024 byte SL target");
		else
			rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE,
			    FSIMG_EUNSUPPORTED, "1025 byte SL target");
		free(image.bytes);
		if (size == 1025) {
			susp_entry(&area, "TF", 5)[4] = 2;
			image = rr_image(&area, 0);
			rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE,
			    FSIMG_ECORRUPT, "overlong SL then corrupt TF");
			free(image.bytes);
		}
	}
	bzero(&area, sizeof(area));
	bzero(&next, sizeof(next));
	susp_px(&area, 0120777, 36);
	susp_sl(&area, 0x01, 0x01, "tar", 3);
	susp_sl(&next, 0x00, 0x00, "get", 3);
	susp_ce(&area, 111, 0, next.length);
	image = rr_image(&area, 0);
	bcopy(next.bytes, image.bytes + 111 * BLOCK, next.length);
	rr_witness(&image, "/A.TXT", FSIMG_ENTRY_SYMLINK, "target", 6,
	    "SL across CE");
	free(image.bytes);
}

static void
test_rr_times(void)
{
	static const unsigned char short_date[] = { 70, 1, 1, 0, 0, 1, 0 };
	static const unsigned char long_date[] = {
		'1', '9', '7', '0', '0', '1', '0', '1', '0', '0', '0', '0',
		'0', '1', '1', '2',
		0 /* Zone: UTC, in signed 15-minute units. */
	};
	susp_area_t area;
	image_t image;
	fixture_t f;
	const fsimg_entry_t *entry;
	unsigned char *p;
	fsimg_status_t status;

	static const struct {
		const char *label;
		unsigned width, flags, count, nanoseconds, precision;
		const unsigned char *date;
		bool omit;
		fsimg_status_t expected;
	} rows[] = {
		{ "TF short four dates", 7, 15, 4, 0, 1000000000,
		    short_date, false, FSIMG_OK },
		{ "TF long four dates", 17, 143, 4, 120000000, 10000000,
		    long_date, false, FSIMG_OK },
		{ "TF short excess dates", 7, 1, 4, 0, 1000000000,
		    short_date, false, FSIMG_ECORRUPT },
		{ "recording date without TF", 7, 0, 0, 0, 1000000000,
		    short_date, true, FSIMG_OK },
		{ "TF creation preserves recording modify", 7, 1, 1, 0,
		    1000000000, short_date, false, FSIMG_OK }
	};

	for (unsigned form = 0; form < sizeof(rows) / sizeof(rows[0]); form++) {
		unsigned width = rows[form].width;

		bzero(&area, sizeof(area));
		susp_px(&area, 0106754, 44);
		if (!rows[form].omit) {
			p = susp_entry(&area, "TF",
			    5 + rows[form].count * width);
			p[4] = rows[form].flags;
			for (unsigned i = 0; i < rows[form].count; i++)
				bcopy(rows[form].date, p + 5 + i * width,
				    width);
		}
		image = rr_image(&area, 0);
		bcopy(short_date,
		    image.bytes + ROOT * BLOCK + RR_CHILD + DR_DATE, 7);
		fixture_new(&f, &image, NULL);
		status = open_image(&f, FSIMG_ISO_VIEW_ROCKRIDGE);
		if (status == FSIMG_OK)
			status = fsimg_reader_next(f.reader, &entry, NULL);
		rr_status_equal(status, rows[form].expected, rows[form].label);
		if (status == FSIMG_OK) {
			rr_equal(entry->metadata.modified.seconds, 1,
			    rows[form].label);
			rr_equal(entry->metadata.valid & FSIMG_META_MODIFIED,
			    FSIMG_META_MODIFIED, rows[form].label);
			rr_equal(entry->metadata.modified.nanoseconds,
			    rows[form].nanoseconds, rows[form].label);
			rr_equal(entry->metadata.modified.precision_ns,
			    rows[form].precision, rows[form].label);
			if (rows[form].count == 4) {
				rr_equal(entry->metadata.valid &
				    (FSIMG_META_CREATED | FSIMG_META_ACCESSED |
				    FSIMG_META_CHANGED),
				    FSIMG_META_CREATED |
				    FSIMG_META_ACCESSED | FSIMG_META_CHANGED,
				    rows[form].label);
				rr_equal(entry->metadata.created.seconds, 1,
				    rows[form].label);
				rr_equal(entry->metadata.accessed.seconds,
				    1, rows[form].label);
				rr_equal(entry->metadata.changed.seconds, 1,
				    rows[form].label);
			}
			if (rows[form].count == 1) {
				rr_equal(entry->metadata.created.seconds, 1,
				    rows[form].label);
				rr_equal(entry->metadata.valid &
				    (FSIMG_META_ACCESSED | FSIMG_META_CHANGED),
				    0, rows[form].label);
			}
		}
		fixture_free(&f);
		free(image.bytes);
	}
}

static void
test_rr_shared_continuation(void)
{
	enum {
		DOT_PX = 64, DOT_CE = ROOT_DOT_BYTES - 1,
		PARENT_RECORD = DOT_CE + CE_BYTES + 1,
		PARENT_CE = PARENT_RECORD + DOT_BYTES,
		PARENT_BYTES = DOT_BYTES + CE_BYTES,
		FIRST_CHILD = PARENT_RECORD + PARENT_BYTES
	};
	susp_area_t area = {0};
	image_t image;
	fixture_t f;
	unsigned char *p, up = 1;
	size_t length;
	char name[16];

	susp_px(&area, 0100644, PX_BYTES);
	image = rr_image(&area, 0);
	/* Dot records and children use disjoint areas in the same block. */
	bzero(&area, sizeof(area));
	susp_px(&area, 0040755, PX_BYTES);
	bcopy(area.bytes, image.bytes + RR_AREA * BLOCK + DOT_PX, area.length);
	p = image.bytes + ROOT * BLOCK;
	bzero(p + DOT_CE, BLOCK - DOT_CE);
	bzero(&area, sizeof(area));
	susp_ce(&area, RR_AREA, DOT_PX, PX_BYTES);
	bcopy(area.bytes, p + DOT_CE, area.length);
	p[0] = PARENT_RECORD;
	record(p + PARENT_RECORD, &up, 1, ROOT, BLOCK, 2);
	bcopy(area.bytes, p + PARENT_CE, area.length);
	p[PARENT_RECORD] = PARENT_BYTES;
	p += FIRST_CHILD;
	for (unsigned i = 0; i < 24; i++) {
		snprintf(name, sizeof(name), "F%02u;1", i);
		length = record(p, name, strlen(name), DATA, 5, 0);
		bzero(&area, sizeof(area));
		susp_ce(&area, RR_AREA, 0, PX_BYTES);
		bcopy(area.bytes, p + length, area.length);
		p[0] += area.length;
		p += p[0];
	}
	fixture_new(&f, &image, NULL);
	rr_status_equal(open_image(&f, FSIMG_ISO_VIEW_ROCKRIDGE),
	    FSIMG_OK, "shared CE open");
	rr_status_equal(walk(&f), FSIMG_OK, "shared CE walk");
	/* Preparation and replay reuse the same cached block. */
	rr_equal(image.continuation_reads, 1,
	    "24 children share one continuation block");
	fixture_free(&f);
	free(image.bytes);
}

static void
test_rr_omitted_names(void)
{
	susp_area_t area = {0};
	image_t image;
	fixture_t f;
	fsimg_stats_t stats;
	fsimg_extract_options_t options;
	const fsimg_entry_t *entry;
	char temporary[] = "./iso-rr-XXXXXX";
	unsigned char *p;
	int parent;

	susp_nm(&area, "caf\xe9", 4, 0);
	image = rr_image(&area, 0);
	p = image.bytes + ROOT * BLOCK + RR_CHILD;
	record(p + p[0], "B;1", 3, DATA, 5, 0);
	fixture_new(&f, &image, NULL);
	CHECK(open_image(&f, FSIMG_ISO_VIEW_ROCKRIDGE) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	rr_equal(strcmp(entry->path, "/B"), 0,
	    "Latin-1 NM omitted; sibling retained");
	rr_status_equal(walk(&f), FSIMG_OK, "omitted name listing ends");
	CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
	rr_equal(stats.omitted_entries, 1, "one omitted NM");
	fixture_free(&f);
	CHECK(mkdtemp(temporary) != NULL);
	parent = open(temporary, O_RDONLY | O_DIRECTORY);
	CHECK(parent >= 0);
	for (unsigned skip = 0; skip < 2; skip++) {
		fixture_new(&f, &image, NULL);
		CHECK(open_image(&f, FSIMG_ISO_VIEW_ROCKRIDGE) == FSIMG_OK);
		fsimg_extract_options_init(&options);
		options.flags |= skip ? FSIMG_EXTRACT_SKIP_UNSUPPORTED : 0;
		rr_status_equal(fsimg_extract_at(f.reader, parent, "out",
		    &options, NULL),
		    skip ? FSIMG_OK : FSIMG_EUNSUPPORTED,
		    skip ? "extract omitted NM with skip" :
		    "extract omitted NM without skip");
		fixture_free(&f);
		remove_tree(parent, "out");
	}
	CHECK(close(parent) == 0);
	CHECK(rmdir(temporary) == 0);
	free(image.bytes);
}

/* Logical chains use blocks 40..59; relocated dots name physical parent
 * ROOT and carry PL naming the logical predecessor. RE records at ROOT
 * are physical directory aliases hidden from this view.
 */
static void
rr_relocated_chain(unsigned depth, bool several)
{
	susp_area_t area, link, parent_link;
	image_t image;
	unsigned char *root, *p;
	unsigned char self = 0, up = 1;
	char name[16], expected_path[128], label[96];
	size_t length, root_offset, count;
	fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_status_t status;
	unsigned directories, files;
	unsigned char content[5];

	snprintf(label, sizeof(label),
	    "relocation depth %u / several %u", depth, several);
	bzero(&area, sizeof(area));
	susp_px(&area, 0040755, 36);
	image = rr_image(&area, 0);
	root = image.bytes + ROOT * BLOCK;
	bzero(root + RR_CHILD, BLOCK - RR_CHILD);
	root_offset = RR_CHILD;
	for (unsigned level = 0; level < depth; level++) {
		unsigned block = 40 + level;
		unsigned predecessor = level == 0 ? ROOT : block - 1;
		unsigned extent = block, size = BLOCK, flags = 2;
		bool relocated = level == 7 || (several && level > 7);

		p = image.bytes + block * BLOCK;
		length = record(p, &self, 1, block, BLOCK, 2);
		length += record(p + length, &up, 1,
		    relocated ? ROOT : predecessor, BLOCK, 2);
		if (relocated) {
			bzero(&parent_link, sizeof(parent_link));
			put32(susp_entry(&parent_link, "PL", 12) + 4,
			    predecessor);
			bcopy(parent_link.bytes, p + length,
			    parent_link.length);
			p[DOT_BYTES] += parent_link.length;
		}
		bzero(&link, sizeof(link));
		susp_px(&link, 0040755, 36);
		if (relocated)
			put32(susp_entry(&link, "CL", 12) + 4, block);
		p = level == 0 ? root + root_offset :
		    image.bytes + predecessor * BLOCK +
		    ((level == 8 || (level > 8 && several)) ?
		    RELOCATED_CHILD : DOT_PAIR_BYTES);
		snprintf(name, sizeof(name), "D%02u", level);
		if (relocated) {
			extent = several ? MKISOFS_PLACEHOLDER :
			    XORRISO_PLACEHOLDER;
			size = 0;
			flags = 0;
		}
		length = record(p, name, strlen(name), extent, size,
		    flags);
		bcopy(link.bytes, p + length, link.length);
		p[0] += link.length;
		if (level == 0)
			root_offset += p[0];
		if (relocated) {
			p = root + root_offset;
			snprintf(name, sizeof(name), "R%02u", level);
			length = record(p, name, strlen(name), block, BLOCK, 2);
			bzero(&link, sizeof(link));
			susp_entry(&link, "RE", 4);
			bcopy(link.bytes, p + length, link.length);
			p[0] += link.length;
			root_offset += p[0];
		}
	}
	/* A payload under the deepest directory checks the CL content path. */
	p = image.bytes + (40 + depth - 1) * BLOCK;
	length = DOT_PAIR_BYTES;
	if (depth == 8 || (several && depth > 8))
		length += LINK_BYTES;
	record(p + length, "FILE;1", 6, DATA, 5, 0);
	expected_path[0] = 0;
	for (unsigned level = 0; level < depth; level++) {
		snprintf(name, sizeof(name), "/D%02u", level);
		strcat(expected_path, name);
	}
	strcat(expected_path, "/FILE");
	fixture_new(&f, &image, NULL);
	status = open_image(&f, FSIMG_ISO_VIEW_ROCKRIDGE);
	directories = 0;
	files = 0;
	if (status == FSIMG_OK) {
		while ((status = fsimg_reader_next(f.reader, &entry,
		    NULL)) == FSIMG_OK) {
			if (entry->kind == FSIMG_ENTRY_FILE) {
				rr_equal(strcmp(entry->path, expected_path),
				    0, label);
				rr_status_equal(fsimg_reader_read_at(f.reader,
				    0, content, sizeof(content), &count, NULL),
				    FSIMG_OK, label);
				rr_equal(count == 5 &&
				    bcmp(content, "hello", 5) == 0, 1, label);
				files++;
			} else {
				rr_equal(entry->kind, FSIMG_ENTRY_DIRECTORY,
				    label);
				directories++;
			}
		}
	}
	rr_status_equal(status, FSIMG_DONE, label);
	rr_equal(directories, depth, label);
	rr_equal(files, 1, label);
	fixture_free(&f);
	free(image.bytes);
}

static void
test_rr_relocation(void)
{
	enum setup {
		DOT_FLAG_CLEAR, MISSING_PL, WRONG_PL, ORDINARY_PL, ROOT_CL,
		ALIAS, LOOP, VALID, PRIMARY, DUPLICATE_CL, CL_RE
	};
	static const struct {
		const char *label;
		enum setup setup;
		fsimg_status_t expected;
	} rows[] = {
		{ "CL target dot flag clear", DOT_FLAG_CLEAR, FSIMG_ECORRUPT },
		{ "CL missing PL", MISSING_PL, FSIMG_ECORRUPT },
		{ "PL wrong parent", WRONG_PL, FSIMG_ECORRUPT },
		{ "PL ordinary directory", ORDINARY_PL, FSIMG_ECORRUPT },
		{ "CL root", ROOT_CL, FSIMG_ECORRUPT },
		{ "CL and ordinary alias", ALIAS, FSIMG_ECORRUPT },
		{ "relocation loop", LOOP, FSIMG_ECORRUPT },
		{ "CL placeholder valid", VALID, FSIMG_OK },
		{ "primary placeholder", PRIMARY, FSIMG_OK },
		{ "duplicate CL on valid placeholder", DUPLICATE_CL,
		    FSIMG_ECORRUPT },
		{ "CL and RE on directory PX placeholder", CL_RE,
		    FSIMG_ECORRUPT }
	};
	susp_area_t area, parent_link;
	image_t image;
	unsigned char *root, *p;
	size_t length;
	fixture_t f;
	const fsimg_entry_t *entry;

	for (unsigned depth = 8; depth <= 20; depth++) {
		for (unsigned several = 0; several < 2; several++)
			rr_relocated_chain(depth, several != 0);
	}
	for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		bzero(&area, sizeof(area));
		susp_px(&area, 0040755, 36);
		put32(susp_entry(&area, "CL", 12) + 4, 85);
		image = rr_image(&area, 0);
		root = image.bytes + ROOT * BLOCK;
		p = root + RR_CHILD;
		put32(p + DR_SIZE, 0);
		put32(p + DR_EXTENT, MKISOFS_PLACEHOLDER);
		p = image.bytes + 85 * BLOCK;
		directory(p, 85, ROOT, BLOCK);
		bzero(&parent_link, sizeof(parent_link));
		put32(susp_entry(&parent_link, "PL", 12) + 4, ROOT);
		bcopy(parent_link.bytes, p + DOT_PAIR_BYTES, 12);
		p[DOT_BYTES] += 12;
		switch (rows[i].setup) {
		case DOT_FLAG_CLEAR: p[DR_FLAGS] = 0; break;
		case MISSING_PL: bzero(p + DOT_PAIR_BYTES, 12); break;
		case WRONG_PL:
			put32(p + DOT_PAIR_BYTES + SUSP_PAYLOAD, 86);
			break;
		case ORDINARY_PL: /* PL on an ordinary directory. */
			bzero(image.bytes + RR_AREA * BLOCK + PX_BYTES, 12);
			put32(root + RR_CHILD + DR_EXTENT, 85);
			put32(root + RR_CHILD + DR_SIZE, BLOCK);
			root[RR_CHILD + DR_FLAGS] = 2;
			break;
		case ROOT_CL:
			put32(image.bytes + RR_AREA * BLOCK + PX_BYTES +
			    SUSP_PAYLOAD, ROOT);
			break;
		case ALIAS: /* Same extent through CL and an ordinary record. */
			record(root + RR_CHILD + root[RR_CHILD], "B", 1, 85,
			    BLOCK, 2);
			break;
		case LOOP:
			/* CL returns to its own directory; PL mismatches. */
			length = record(p + RELOCATED_CHILD, "LOOP", 4,
			    MKISOFS_PLACEHOLDER, 0, 0);
			bcopy(area.bytes, p + RELOCATED_CHILD + length,
			    area.length);
			p[RELOCATED_CHILD] += area.length;
			break;
		case VALID: break;
		case PRIMARY: break;
		case DUPLICATE_CL:
		case CL_RE:
			/* Extend the independently checked VALID row. */
			if (rows[i].setup == DUPLICATE_CL)
				put32(susp_entry(&area, "CL",
				    LINK_BYTES) + 4, 85);
			else {
				susp_entry(&area, "RE", 4);
				root[RR_CHILD + DR_FLAGS] = 2;
				put32(root + RR_CHILD + DR_EXTENT, 85);
				put32(root + RR_CHILD + DR_SIZE, BLOCK);
			}
			bcopy(area.bytes, image.bytes + RR_AREA * BLOCK,
			    area.length);
			/* The child's CE must expose the appended singleton. */
			put32(root + RR_CHILD + CHILD_SUSP + 20, area.length);
			break;
		}
		if (rows[i].setup != PRIMARY)
			rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE,
			    rows[i].expected, rows[i].label);
		else {
			fixture_new(&f, &image, NULL);
			CHECK(open_image(&f, FSIMG_ISO_VIEW_PRIMARY) ==
			    FSIMG_OK);
			CHECK(fsimg_reader_next(f.reader, &entry, NULL) ==
			    FSIMG_OK);
			rr_equal(entry->kind, FSIMG_ENTRY_FILE,
			    "primary placeholder empty file");
			rr_equal(entry->size, 0,
			    "primary placeholder zero size");
			fixture_free(&f);
		}
		free(image.bytes);
	}
}

static void
test_rr_dot_entries(void)
{
	static const struct {
		const char *label;
		unsigned version;
		fsimg_status_t expected;
	} rows[] = {
		{ "dot NM current and SL ignored", 1, FSIMG_OK },
		{ "dot NM bad entry version", 2, FSIMG_ECORRUPT }
	};
	static const struct {
		const char *label, *signature;
		unsigned mode, length;
		fsimg_status_t expected;
		const char *refusal;
	} checks[] = {
		{ "directory PX control", NULL, 0040755, 0, FSIMG_OK, NULL },
		{ "CL on dot", "CL", 0040755, 12, FSIMG_ECORRUPT, NULL },
		{ "RE on dot", "RE", 0040755, 4, FSIMG_ECORRUPT, NULL },
		{ "PL on first dot", "PL", 0040755, 12, FSIMG_ECORRUPT,
		    NULL },
		{ "regular PX", NULL, 0100644, 0, FSIMG_ECORRUPT, NULL },
		{ "PN on directory", "PN", 0040755, 20, FSIMG_ECORRUPT,
		    NULL },
		{ "SF refused", "SF", 0040755, 4, FSIMG_EUNSUPPORTED,
		    NULL },
		{ "ZF refused", "ZF", 0040755, 4, FSIMG_EUNSUPPORTED,
		    NULL },
		{ "SF then regular PX", "SF", 0100644, 4,
		    FSIMG_ECORRUPT, NULL },
		{ "ZF then regular PX", "ZF", 0100644, 4,
		    FSIMG_ECORRUPT, NULL },
		{ "ZF then CL", "CL", 0040755, 12, FSIMG_ECORRUPT, "ZF" },
		{ "SF then RE", "RE", 0040755, 4, FSIMG_ECORRUPT, "SF" },
		{ "ZF then PL", "PL", 0040755, 12, FSIMG_ECORRUPT, "ZF" }
	};
	static const struct {
		const char *label;
		bool root, parent;
	} positions[] = {
		{ "root dot", true, false },
		{ "root parent", true, true },
		{ "subdirectory dot", false, false },
		{ "subdirectory parent", false, true }
	};
	susp_area_t area = {0}, dot;
	char label[96];
	image_t image;
	unsigned char *p, self = 0, up = 1;
	size_t offset, length;

	susp_px(&area, 0040755, PX_BYTES);
	for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		image = rr_image(&area, 2);
		p = rr_child_directory(&image);
		bzero(&dot, sizeof(dot));
		susp_nm(&dot, "", 0, 2);
		dot.bytes[SUSP_VERSION] = rows[i].version;
		/* CURRENT and PARENT together are corrupt if interpreted. */
		susp_sl(&dot, 0x00, 0x06, "", 0);
		length = record(p, &self, 1, RR_DIRECTORY, BLOCK, 2);
		bcopy(dot.bytes, p + length, dot.length);
		p[0] += dot.length + (dot.length % 2);
		offset = p[0];
		p += offset;
		length = record(p, &up, 1, ROOT, BLOCK, 2);
		bcopy(dot.bytes, p + length, dot.length);
		p[0] += dot.length + (dot.length % 2);
		rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE,
		    rows[i].expected, rows[i].label);
		free(image.bytes);
	}
	for (unsigned i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
		bzero(&dot, sizeof(dot));
		if (checks[i].refusal != NULL)
			susp_entry(&dot, checks[i].refusal, 4);
		if (checks[i].signature != NULL)
			susp_entry(&dot, checks[i].signature, checks[i].length);
		susp_px(&dot, checks[i].mode, PX_BYTES);
		for (unsigned j = 0;
		    j < sizeof(positions) / sizeof(positions[0]); j++) {
			if (checks[i].signature != NULL &&
			    strcmp(checks[i].signature, "PL") == 0 &&
			    positions[j].parent)
				continue;
			image = rr_dot_image(&dot, positions[j].root,
			    positions[j].parent);
			snprintf(label, sizeof(label), "%s: %s",
			    positions[j].label, checks[i].label);
			rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE,
			    checks[i].expected, label);
			free(image.bytes);
		}
	}
}

static void
test_rr_misplaced_dot_names(void)
{
	static const struct {
		const char *label;
		unsigned char identifier;
	} rows[] = {
		{ "misplaced dot with visible NM", 0 },
		{ "misplaced parent with visible NM", 1 }
	};
	susp_area_t area = {0}, continuation = {0};
	image_t image;
	unsigned char *p;
	size_t length;

	susp_nm(&area, "visible", 7, 0);
	susp_ce(&continuation, RR_AREA, 0, area.length);
	for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		image = rr_image(&area, 0);
		p = image.bytes + ROOT * BLOCK + RR_CHILD;
		bzero(p, BLOCK - RR_CHILD);
		length = record(p, &rows[i].identifier, 1, DATA, 5, 0);
		bcopy(continuation.bytes, p + length, continuation.length);
		p[0] += continuation.length;
		rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE, FSIMG_ECORRUPT,
		    rows[i].label);
		rr_status(&image, FSIMG_ISO_VIEW_PRIMARY, FSIMG_ECORRUPT,
		    rows[i].label);
		free(image.bytes);
	}
}

static void
test_rr_faults(void)
{
	static const struct {
		const char *label;
		bool eof, cancel;
		fsimg_status_t expected;
	} faults[] = {
		{ "Rock Ridge CE read I/O failure", false, false, FSIMG_EIO },
		{ "Rock Ridge CE read EOF", true, false, FSIMG_EIO },
		{ "Rock Ridge CE read cancelled", false, true,
		    FSIMG_ECANCELLED }
	};
	static const struct {
		const char *label;
		bool read_target;
	} operations[] = {
		{ .label = "Rock Ridge symlink listing allocation",
		    .read_target = false },
		{ .label = "Rock Ridge symlink target read allocation",
		    .read_target = true }
	};
	susp_area_t area = {0};
	image_t image;
	fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_status_t status;
	unsigned char buffer[8];
	uint64_t ce_read_index;
	size_t count;
	bool allocation_hit;
	char label[96];

	susp_px(&area, 0120777, PX_BYTES);
	susp_sl(&area, 0x00, 0x00, "target", 6);
	image = rr_image(&area, 0);
	/* Locate the continuation callback independently of other read counts. */
	fixture_new(&f, &image, NULL);
	rr_status_equal(open_image(&f, FSIMG_ISO_VIEW_ROCKRIDGE), FSIMG_OK,
	    "Rock Ridge fault fixture open");
	rr_status_equal(fsimg_reader_next(f.reader, &entry, NULL), FSIMG_OK,
	    "Rock Ridge fault fixture next");
	rr_equal(image.continuation_reads, 1, "one CE read in fault fixture");
	ce_read_index = image.ce_read_index;
	fixture_free(&f);
	for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
		image.reads = 0;
		image.continuation_reads = 0;
		image.fail_read = ce_read_index;
		image.eof = faults[i].eof;
		image.cancel_read = faults[i].cancel;
		fixture_new(&f, &image, NULL);
		status = open_image(&f, FSIMG_ISO_VIEW_ROCKRIDGE);
		rr_status_equal(status, FSIMG_OK, faults[i].label);
		if (status == FSIMG_OK)
			status = walk(&f);
		rr_status_equal(status, faults[i].expected, faults[i].label);
		rr_equal(image.continuation_reads, 1, faults[i].label);
		rr_equal(image.reads, ce_read_index, faults[i].label);
		fixture_free(&f);
	}
	image.fail_read = 0;
	image.eof = false;
	image.cancel_read = false;
	/* Arm after open so the sweep reaches the common reader's decoded
	 * link target allocation, as well as the listing's scratch state.
	 * fixture_free checks that every failed prefix releases its resources.
	 */
	for (unsigned operation = 0;
	    operation < sizeof(operations) / sizeof(operations[0]); operation++) {
		for (unsigned n = 1;; n++) {
			fixture_new(&f, &image, NULL);
			status = open_image(&f, FSIMG_ISO_VIEW_ROCKRIDGE);
			rr_status_equal(status, FSIMG_OK,
			    operations[operation].label);
			fsimg_test_fail_alloc_after(f.ctx, n);
			if (status == FSIMG_OK)
				status = fsimg_reader_next(f.reader, &entry, NULL);
			if (status == FSIMG_OK) {
				rr_equal(entry->kind, FSIMG_ENTRY_SYMLINK,
				    operations[operation].label);
				if (operations[operation].read_target) {
					status = fsimg_reader_read_at(f.reader, 0,
					    buffer, sizeof(buffer), &count, NULL);
					if (status == FSIMG_OK) {
						rr_equal(count, 6,
						    operations[operation].label);
						rr_equal(count == 6 &&
						    bcmp(buffer, "target", 6) == 0, 1,
						    operations[operation].label);
					}
				}
			}
			if (status == FSIMG_OK)
				status = walk(&f);
			allocation_hit = f.ctx->allocation_attempts >= n;
			fsimg_test_fail_alloc_after(f.ctx, 0);
			snprintf(label, sizeof(label), "%s %u",
			    operations[operation].label, n);
			if (allocation_hit) {
				rr_equal(status == FSIMG_ENOMEM ||
				    status == FSIMG_ELIMIT, 1, label);
			} else
				rr_status_equal(status, FSIMG_OK, label);
			fixture_free(&f);
			if (!allocation_hit)
				break;
			CHECK(n < 10000);
		}
	}
	free(image.bytes);
}

static void
test_rr_later_sections(void)
{
	susp_area_t area = {0};
	image_t image;
	unsigned char *p;
	size_t length;

	susp_px(&area, 0100644, 36);
	image = rr_image(&area, 0x80);
	p = image.bytes + ROOT * BLOCK + RR_CHILD;
	put32(p + DR_SIZE, BLOCK);
	p += p[0];
	length = record(p, "A.TXT;1", 7, DATA + 1, 0, 0);
	bzero(&area, sizeof(area));
	susp_entry(&area, "ZF", 4);
	/* Semantically invalid, correctly framed. */
	susp_entry(&area, "PX", 4);
	bcopy(area.bytes, p + length, area.length);
	p[0] += area.length;
	rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE, FSIMG_OK,
	    "later section SUSP framed only");
	p[length + 2] = 3;
	rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE, FSIMG_ECORRUPT,
	    "later section framing invalid");
	free(image.bytes);
}

static void
test_rr_exact_queries(void)
{
	static const struct {
		const char *path;
		fsimg_status_t expected;
	} queries[] = {
		{ "/Exact;1", FSIMG_OK },
		{ "/Exact", FSIMG_EINVAL },
		{ "/exact;1", FSIMG_EINVAL }
	};
	susp_area_t area = {0};
	image_t image;
	fixture_t f;
	char temporary[] = "./iso-query-XXXXXX";
	int parent;

	susp_nm(&area, "Exact;1", 7, 0);
	image = rr_image(&area, 0);
	CHECK(mkdtemp(temporary) != NULL);
	parent = open(temporary, O_RDONLY | O_DIRECTORY);
	CHECK(parent >= 0);
	for (unsigned i = 0; i < 3; i++) {
		fixture_new(&f, &image, NULL);
		CHECK(open_image(&f, FSIMG_ISO_VIEW_ROCKRIDGE) == FSIMG_OK);
		rr_status_equal(extract(&f, parent, queries[i].path),
		    queries[i].expected, queries[i].path);
		fixture_free(&f);
		remove_tree(parent, "out");
	}
	CHECK(close(parent) == 0);
	CHECK(rmdir(temporary) == 0);
	free(image.bytes);
}

static void
test_rr_known_versions(void)
{
	static const char *const signatures[] = {
		"PX", "PN", "SL", "NM", "CL", "PL", "RE", "TF", "RR", "SF", "ZF"
	};
	static const unsigned lengths[] = { 36, 20, 5, 5, 12, 12, 4, 5, 5,
	    4, 4 };
	susp_area_t area;
	image_t image;
	char label[64];

	for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
		bzero(&area, sizeof(area));
		susp_entry(&area, signatures[i], lengths[i])[3] = 2;
		image = rr_image(&area, 0);
		snprintf(label, sizeof(label), "%s version 2 is corrupt",
		    signatures[i]);
		rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE, FSIMG_ECORRUPT,
		    label);
		free(image.bytes);
	}
}

static void
susp_seed(const char *path)
{
	unsigned char seed[512 + 4 * BLOCK] = {0};
	susp_area_t area = {0};
	unsigned char *p;
	FILE *file;

	p = susp_entry(&area, "SP", 7);
	p[4] = 0xbe;
	p[5] = 0xef;
	susp_er(&area, "RRIP_1991A", 1);
	bcopy(area.bytes, seed, area.length);
	bzero(&area, sizeof(area));
	susp_px(&area, 0100644, 36);
	susp_nm(&area, "fi", 2, 1);
	susp_ce(&area, 110, 0, 7);
	bcopy(area.bytes, seed + 128, area.length);
	bzero(&area, sizeof(area));
	susp_px(&area, 0120777, 44);
	susp_sl(&area, 0x01, 0x01, "fi", 2);
	susp_ce(&area, 111, 0, 9);
	bcopy(area.bytes, seed + 256, area.length);
	bzero(&area, sizeof(area));
	susp_px(&area, 0040755, 36);
	bcopy(area.bytes, seed + 384, area.length);
	bzero(&area, sizeof(area));
	susp_nm(&area, "le", 2, 0);
	bcopy(area.bytes, seed + 512, area.length);
	bzero(&area, sizeof(area));
	susp_sl(&area, 0x00, 0x00, "le", 2);
	bcopy(area.bytes, seed + 512 + BLOCK, area.length);
	file = fopen(path, "wb");
	CHECK(file != NULL);
	CHECK(fwrite(seed, 1, sizeof(seed), file) == sizeof(seed));
	CHECK(fclose(file) == 0);
}

int
main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--reject") == 0) {
		const fsimg_format_variant_t *variant;
		unsigned char prefix[17 * BLOCK];
		struct stat st;
		FILE *file = fopen(argv[2], "rb");
		size_t count;

		CHECK(file != NULL);
		CHECK(fstat(fileno(file), &st) == 0 && st.st_size >= 0);
		count = fread(prefix, 1, sizeof(prefix), file);
		CHECK(!ferror(file));
		CHECK(fclose(file) == 0);
		CHECK(fsimg_iso9660_format.probe(prefix, count, st.st_size,
		    &variant, NULL) == FSIMG_EFORMAT);
		return (0);
	}
	if (argc == 3 && (strcmp(argv[1], "--seed") == 0 ||
	    strcmp(argv[1], "--seed-boot") == 0)) {
		image_t image = image_new();
		FILE *file = fopen(argv[2], "wb");

		if (strcmp(argv[1], "--seed-boot") == 0) {
			free(image.bytes);
			image = boot_image(0);
			boot_efi_section(&image);
		}
		CHECK(file != NULL);
		CHECK(fwrite(image.bytes, 1, image.length, file) ==
		    image.length);
		CHECK(fclose(file) == 0);
		free(image.bytes);
		return (0);
	}
	if (argc == 3 && strcmp(argv[1], "--susp-seed") == 0) {
		susp_seed(argv[2]);
		return (0);
	}
	CHECK(argc == 1);
	test_susp_handshake_views();
	test_susp_continuation_bounds();
	test_susp_framing_and_precedence();
	test_rr_kinds_and_singletons();
	test_rr_names_and_continuations();
	test_rr_link_components();
	test_rr_times();
	test_rr_shared_continuation();
	test_rr_omitted_names();
	test_rr_relocation();
	test_rr_dot_entries();
	test_rr_misplaced_dot_names();
	test_rr_faults();
	test_rr_later_sections();
	test_rr_exact_queries();
	test_rr_known_versions();
	test_boot_platforms_and_sections();
	test_boot_entry_variants();
	test_boot_catalog_capacity();
	test_boot_validation_mutations();
	test_boot_entry_mutations();
	test_boot_extent_bounds();
	test_boot_data_remains_tree_data();

	test_displayed_version_suffix();
	test_empty_section_extents();
	test_descriptor_limit_terminator();
	test_label_nul_padding();
	test_smallest_volume();
	test_descriptor_permutations();
	test_section_count_boundary();
	test_other_format_probes();
	test_supplementary_info();
	test_probe();
	test_descriptors();
	test_geometry();
	test_records();
	test_tree();
	test_sections();
	test_extended_attributes_and_domain();
	test_names();
	test_info_metadata_views();
	test_extraction_and_selection();
	test_faults();
	test_exclusivity();
	printf("ISO 9660: %u classified cases, %u failures\n", cases, failures);
	return (failures == 0 ? 0 : 1);
}
