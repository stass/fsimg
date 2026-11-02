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
	uint64_t	 fail_read;
	bool		 eof;
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
	record(p + 68, "A.TXT;1", 7, DATA, 5, 0);
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
	*count = 0;
	if (image->reads == image->fail_read)
		return (image->eof ? FSIMG_OK : FSIMG_EIO);
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
			record(p + 68, "DIR", 3, block + 1, BLOCK, 2);
		else
			record(p + 68, "A.TXT;1", 7, DATA, 5, 0);
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
test_faults(void)
{
	image_t image = image_new();
	fixture_t f;
	fsimg_options_t options;
	fsimg_status_t status;
	const fsimg_entry_t *entry;
	unsigned remaining;
	unsigned char buffer[8];
	uint64_t reads;
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
			fixture_new(&f, &image, NULL);
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
	image.reads = 0;
	fixture_new(&f, &image, NULL);
	CHECK(open_image(&f, FSIMG_ISO_VIEW_PRIMARY) == FSIMG_OK);
	CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_read_at(f.reader, 0, buffer, sizeof(buffer),
	    &count, NULL) == FSIMG_OK);
	reads = image.reads;
	fixture_free(&f);
	for (unsigned eof = 0; eof < 2; eof++) {
		for (uint64_t n = 1; n <= reads; n++) {
			image.reads = 0;
			image.fail_read = n;
			image.eof = eof != 0;
			fixture_new(&f, &image, NULL);
			status = open_image(&f, FSIMG_ISO_VIEW_PRIMARY);
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
	image.fail_read = 0;
	for (unsigned blocks = 0; blocks < BLOCKS; blocks++) {
		image.length = blocks * BLOCK;
		expect(&image, blocks < 17 ? FSIMG_EFORMAT :
		    blocks <= DATA ? FSIMG_ECORRUPT : FSIMG_OK,
		    "truncation at each block boundary");
	}
	image.length = BLOCKS * BLOCK;
	for (remaining = 0; remaining < 2; remaining++) {
		unsigned budget = remaining;

		fsimg_options_init(&options);
		options.cancelled = cancelled;
		options.cancel_argument = &budget;
		fixture_new(&f, &image, &options);
		status = open_image(&f, FSIMG_ISO_VIEW_PRIMARY);
		if (status == FSIMG_OK)
			status = walk(&f);
		CHECK(status == FSIMG_ECANCELLED);
		fixture_free(&f);
	}
	fsimg_options_init(&options);
	options.max_work = 1;
	fixture_new(&f, &image, &options);
	status = open_image(&f, FSIMG_ISO_VIEW_PRIMARY);
	if (status == FSIMG_OK)
		status = walk(&f);
	CHECK(status == FSIMG_ELIMIT);
	fixture_free(&f);
	record(image.bytes + ROOT * BLOCK + 108, "B;1", 3, DATA, 5, 0);
	fsimg_options_init(&options);
	options.max_entries = 1;
	fixture_new(&f, &image, &options);
	status = open_image(&f, FSIMG_ISO_VIEW_PRIMARY);
	if (status == FSIMG_OK)
		status = walk(&f);
	CHECK(status == FSIMG_ELIMIT);
	fixture_free(&f);
	CHECK(close(parent) == 0);
	CHECK(rmdir(temporary) == 0);
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
	expect(&image, FSIMG_EUNSUPPORTED, "62 section entries exceed catalog");
	boot_header(catalog + 64, 0x90, 0xef, 61);
	expect(&image, FSIMG_EUNSUPPORTED, "further header exceeds catalog");
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
		expect(&image, mutations[i].status, mutations[i].name);
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
		expect(&image, mutations[i].status, mutations[i].name);
		free(image.bytes);
	}
	image = boot_image(0);
	boot_efi_section(&image);
	catalog = image.bytes + BOOT_CATALOG * BLOCK;
	boot_le16(catalog + 66, 65535);
	expect(&image, FSIMG_ECORRUPT,
	    "last section count 65535 runs into zero entry");
	boot_header(catalog + 64, 0x90, 0xef, 2);
	boot_header(catalog + 128, 0x91, 0, 1);
	boot_entry(catalog + 160, 0, 4, DATA);
	expect(&image, FSIMG_ECORRUPT,
	    "more section count runs over following last header");
	bzero(catalog + 128, 64);
	boot_header(catalog + 64, 0x90, 0xef, 1);
	expect(&image, FSIMG_ECORRUPT, "non-header after more section");
	catalog[64] = 0x91;
	image.bytes[17 * BLOCK + 39] = 1;
	expect(&image, FSIMG_ECORRUPT, "boot identifier nonzero");
	image.bytes[17 * BLOCK + 39] = 0;
	boot_record(image.bytes + 18 * BLOCK, BOOT_CATALOG);
	descriptor(image.bytes + 19 * BLOCK, 255, 1);
	expect(&image, FSIMG_EUNSUPPORTED, "two El Torito boot records");
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
		expect(&image, FSIMG_ECORRUPT, catalogs[i].name);
	}
	/* Leave the catalog readable in the slice but outside the volume. */
	bcopy(image.bytes + BOOT_CATALOG * BLOCK,
	    image.bytes + (BLOCKS - 1) * BLOCK, BLOCK);
	put32(image.bytes + 16 * BLOCK + VD_SPACE, BLOCKS - 1);
	boot_record(image.bytes + 17 * BLOCK, BLOCKS - 1);
	expect(&image, FSIMG_ECORRUPT, "catalog in slice past volume");
	put32(image.bytes + 16 * BLOCK + VD_SPACE, BLOCKS);
	boot_record(image.bytes + 17 * BLOCK, BOOT_CATALOG);
	image.length = BOOT_CATALOG * BLOCK;
	expect(&image, FSIMG_ECORRUPT, "catalog past truncated slice");
	image.length = BLOCKS * BLOCK;
	for (unsigned i = 0; i < sizeof(images) / sizeof(images[0]); i++) {
		boot_entry(entry, 0, 4, images[i].block);
		expect(&image, FSIMG_ECORRUPT, images[i].name);
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
	CHECK(argc == 1);
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
