/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "format.h"
#include "fsimg.h"
#include "internal.h"
#include "test.h"
#include "iso9660-image.h"

/* Indices in the public ISO information detail array. */
enum {
	DETAIL_ROCKRIDGE = 0, DETAIL_JOLIET_LEVEL = 1,
	DETAIL_ROOT_BLOCK = 5, DETAIL_PATH_TABLE_BYTES = 6
};

static void expect(image_t *image, fsimg_status_t expected, const char *name);
static void expect_open(image_t *image, fsimg_status_t expected,
    const char *name);
static fsimg_verify_report_t verify_expect(image_t *image,
    fsimg_iso_view_t view, unsigned flags, fsimg_status_t expected,
    const char *message, const char *name);
static image_t verify_tree(bool joliet);
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
static void test_extraction_budget_uses_selected_io_extent(void);
static void test_image_faults(image_t *image);
static void test_faults(void);
static void test_exclusivity(void);
static void test_smallest_volume(void);
static void permutations(unsigned char templates[6][BLOCK],
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
	    image->length, false, &variant, NULL);
	cases++;
	if (status != expected) {
		fprintf(stderr, "probe %s: expected %d, observed %d\n", name,
		    expected, status);
		failures++;
	}
}

/* Independent wire positions, deliberately not the driver's field table. */
static const struct {
	const char *name;
	unsigned offset;
	unsigned width;
} info_text_fields[] = {
	{ "volume_set", 190, 128 }, { "publisher", 318, 128 },
	{ "preparer", 446, 128 }, { "application", 574, 128 },
	{ "copyright_file", 702, 37 }, { "abstract_file", 739, 37 },
	{ "biblio_file", 776, 37 }
};

static image_t
info_descriptor_image(void)
{
	image_t image = image_new();

	joliet_add(&image);
	/* The metadata tests need only the root in the Joliet hierarchy. */
	put32(image.bytes + 17 * BLOCK + VD_PATH_SIZE, 10);
	bzero(image.bytes + 90 * BLOCK + 10, BLOCK - 10);
	bzero(image.bytes + 91 * BLOCK + 10, BLOCK - 10);
	return (image);
}

static void
check_info_text(image_t *image, fsimg_iso_view_t view, unsigned field,
    const char *expected)
{
	fixture_t fixture;
	fsimg_volume_info_t info;
	fsimg_verify_report_t report;

	fixture_new(&fixture, image, NULL);
	CHECK(open_image(&fixture, view) == FSIMG_OK);
	CHECK(fsimg_reader_get_info(fixture.reader, &info, NULL) == FSIMG_OK);
	CHECK(info.text_count == (expected == NULL ? 0u : 1u));
	if (expected != NULL) {
		CHECK(strcmp(info.texts[0].name, info_text_fields[field].name) == 0);
		CHECK(strcmp(info.texts[0].value, expected) == 0);
	}
	CHECK(fsimg_verify(fixture.reader, 0, &report, NULL) == FSIMG_OK);
	CHECK(report.complete);
	fixture_free(&fixture);
}

static void
test_descriptor_text_printability_and_view_selection(void)
{
	static const struct {
		const char *bytes;
		size_t length;
		const char *reported;
	} cases[] = {
		{ "lowercase / @ \\\"", 17, "lowercase / @ \\\"" },
		{ "_ROOT.TXT;1", 11, "_ROOT.TXT;1" },
		{ "A\0 \0", 4, "A" },
		{ "A\0B", 3, NULL },
		{ "\033[31m", 5, NULL },
		{ "A\177", 2, NULL },
		{ "A\200", 2, NULL },
		{ " \0", 2, NULL }
	};
	image_t image;
	unsigned char *field;

	for (unsigned row = 0; row < 7; row++) {
		char expected[129];

		image = info_descriptor_image();
		memset(expected, 'A', info_text_fields[row].width);
		expected[info_text_fields[row].width] = '\0';
		memset(image.bytes + 16 * BLOCK + info_text_fields[row].offset,
		    'A', info_text_fields[row].width);
		check_info_text(&image, FSIMG_ISO_VIEW_PRIMARY, row, expected);
		free(image.bytes);
	}
	for (unsigned row = 0; row < 7; row++) {
		for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
			image = info_descriptor_image();
			field = image.bytes + 16 * BLOCK + info_text_fields[row].offset;
			memset(field, ' ', info_text_fields[row].width);
			bcopy(cases[c].bytes, field, cases[c].length);
			check_info_text(&image, FSIMG_ISO_VIEW_PRIMARY, row,
			    cases[c].reported);
			/* The SVD is empty: using the primary text would be wrong. */
			check_info_text(&image, FSIMG_ISO_VIEW_JOLIET, row, NULL);
			free(image.bytes);
		}
	}
}

static void
test_descriptor_joliet_unicode_and_hostile_units(void)
{
	static const struct {
		uint16_t units[4];
		const char *reported;
	} cases[] = {
		{ { 'a', 0xe9, 0x4e2d, 0 }, "a\xc3\xa9\xe4\xb8\xad" },
		{ { 0xd83d, 0xde00, 0, 0 }, "\xf0\x9f\x98\x80" },
		{ { 'A', 0, ' ', 0 }, "A" },
		{ { 'A', 0, 'B', 0 }, NULL },
		{ { 'A', 1, 0, 0 }, NULL },
		{ { 'A', 0x85, 0, 0 }, NULL },
		{ { 0xd83d, 0, 0, 0 }, NULL },
		{ { 0xde00, 0, 0, 0 }, NULL }
	};
	image_t image;
	unsigned char *field;

	for (unsigned row = 0; row < 7; row++) {
		char expected[193];
		unsigned units = info_text_fields[row].width / 2;

		image = info_descriptor_image();
		field = image.bytes + 17 * BLOCK + info_text_fields[row].offset;
		for (unsigned i = 0; i < units; i++) {
			field[2 * i] = 0x4e;
			field[2 * i + 1] = 0x2d;
			bcopy("\xe4\xb8\xad", expected + 3 * i, 3);
		}
		expected[3 * units] = '\0';
		check_info_text(&image, FSIMG_ISO_VIEW_JOLIET, row, expected);
		free(image.bytes);
	}
	for (unsigned row = 0; row < 7; row++) {
		for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
			image = info_descriptor_image();
			field = image.bytes + 17 * BLOCK + info_text_fields[row].offset;
			for (unsigned i = 0; i < info_text_fields[row].width / 2; i++) {
				uint16_t unit = i < 4 ? cases[c].units[i] : ' ';

				field[2 * i] = unit >> 8;
				field[2 * i + 1] = unit;
			}
			check_info_text(&image, FSIMG_ISO_VIEW_JOLIET, row,
			    cases[c].reported);
			check_info_text(&image, FSIMG_ISO_VIEW_PRIMARY, row, NULL);
			free(image.bytes);
		}
	}
}

static void
test_descriptor_dates_offset_fraction_and_utc_range(void)
{
	static const char *const names[] = { "created", "modified",
	    "expires", "effective" };
	static const struct {
		const char *raw;
		const char *reported;
	} cases[] = {
		{ "1970010101000099\004", "1970-01-01 00:00:00" },
		{ "1969123123000042\374", "1970-01-01 00:00:00" },
		{ "0001010100000000\0", "0001-01-01 00:00:00" },
		{ "9999123123595900\0", "9999-12-31 23:59:59" },
		{ "0001010100000000\001", NULL },
		{ "9999123123595900\377", NULL },
		{ "0000000000000000\0", NULL }
	};
	fsimg_volume_info_t info;
	fsimg_verify_report_t report;
	fixture_t fixture;
	image_t image;

	for (unsigned date = 0; date < 4; date++) {
		for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
			image = info_descriptor_image();
			for (unsigned descriptor = 16; descriptor <= 17; descriptor++)
				bcopy(cases[c].raw, image.bytes + descriptor * BLOCK +
				    813 + 17 * date, 17);
			for (unsigned view = 0; view < 2; view++) {
				fixture_new(&fixture, &image, NULL);
				CHECK(open_image(&fixture, view == 0 ?
				    FSIMG_ISO_VIEW_PRIMARY : FSIMG_ISO_VIEW_JOLIET) == FSIMG_OK);
				CHECK(fsimg_reader_get_info(fixture.reader, &info, NULL) == FSIMG_OK);
				CHECK(info.text_count == (cases[c].reported == NULL ? 0u : 1u));
				if (cases[c].reported != NULL) {
					CHECK(strcmp(info.texts[0].name, names[date]) == 0);
					CHECK(strcmp(info.texts[0].value, cases[c].reported) == 0);
				}
				CHECK(fsimg_verify(fixture.reader, 0, &report, NULL) == FSIMG_OK);
				CHECK(report.complete);
				fixture_free(&fixture);
			}
			free(image.bytes);
		}
	}
}

static void
test_descriptor_malformed_dates_read_absent_in_either_descriptor(void)
{
	static const char *const names[] = { "created", "modified",
	    "expires", "effective" };
	static const char *const invalid[] = { "19700101000000X0\0",
	    "1970130100000000\0", "0000010100000000\0" };
	fsimg_volume_info_t info;
	fsimg_iso_view_t view;
	fixture_t fixture;
	image_t image;

	for (unsigned descriptor = 16; descriptor <= 17; descriptor++) {
		view = descriptor == 16 ? FSIMG_ISO_VIEW_PRIMARY :
		    FSIMG_ISO_VIEW_JOLIET;
		for (unsigned date = 0; date < 4; date++) {
			for (unsigned mutation = 0; mutation < 3; mutation++) {
				image = info_descriptor_image();
				bcopy(invalid[mutation], image.bytes + descriptor * BLOCK +
				    813 + 17 * date, 17);
				fixture_new(&fixture, &image, NULL);
				CHECK(open_image(&fixture, view) == FSIMG_OK);
				CHECK(fsimg_reader_get_info(fixture.reader, &info, NULL) == FSIMG_OK);
				/* The fixture's other dates and texts are absent too. */
				CHECK(info.text_count == 0);
				fixture_free(&fixture);
				verify_expect(&image, view, 0, FSIMG_ECORRUPT,
				    descriptor == 16 ?
				    "invalid ISO 9660 primary descriptor date" :
				    "invalid ISO 9660 Joliet descriptor date", names[date]);
				free(image.bytes);
			}
		}
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
	static const struct {
		const char *date;
		const char *name;
	} invalid_dates[] = {
		{ "2023023000000000", "primary date with Feb 30" },
		{ "202X020500000000", "primary date with non-digit" },
		{ "2023130500000000", "primary date with month 13" },
		{ "0000020500000000", "primary date with year 0" }
	};
	image_t image;
	fixture_t f;
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
	for (size_t i = 0; i < sizeof(invalid_dates) / sizeof(invalid_dates[0]);
	    i++) {
		bcopy(invalid_dates[i].date, p + VD_DATE, DATE17 - 1);
		expect(&image, FSIMG_OK, invalid_dates[i].name);
		verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_ECORRUPT,
		    "invalid ISO 9660 primary descriptor date",
		    invalid_dates[i].name);
		bzero(p + VD_DATE, DATE17);
	}
	for (unsigned i = 0; i < 4; i++) {
		char name[64];

		bcopy("19920205102039\0\0", p + VD_DATE + i * DATE17,
		    DATE17 - 1);
		p[VD_DATE + i * DATE17 + DATE17 - 1] = 0xec;
		CHECK(snprintf(name, sizeof(name),
		    "Kodak descriptor date at offset %u",
		    VD_DATE + i * DATE17) > 0);
		expect(&image, FSIMG_OK, name);
		verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_ECORRUPT,
		    "invalid ISO 9660 primary descriptor date", name);
		bzero(p + VD_DATE + i * DATE17, DATE17);
	}
	memset(p + VD_DATE, '0', DATE17 - 1);
	expect(&image, FSIMG_OK, "ASCII absent date");
	verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_OK, NULL,
	    "ASCII absent date");
	bzero(p + VD_DATE, DATE17);
	expect(&image, FSIMG_OK, "binary absent dates");
	verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_OK, NULL,
	    "binary absent dates");
	bcopy("1992020510203942", p + VD_DATE, DATE17 - 1);
	p[VD_DATE + DATE17 - 1] = 0xec;
	expect(&image, FSIMG_OK,
	    "valid descriptor date with hundredths and offset");
	verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_OK, NULL,
	    "valid descriptor date with hundredths and offset");
	bzero(p + VD_DATE, DATE17);
	put32(p + VD_SPACE, 17);
	expect(&image, FSIMG_ECORRUPT, "descriptor beyond declared space");
	free(image.bytes);

	image = verify_tree(true);
	next = image.bytes + 17 * BLOCK;
	expect(&image, FSIMG_OK, "valid Joliet descriptor with absent dates");
	verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_OK, NULL,
	    "valid Joliet descriptor with absent dates");
	bcopy("2023023000000000", next + VD_DATE, DATE17 - 1);
	expect(&image, FSIMG_OK, "invalid Joliet descriptor date (Feb 30)");
	verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_ECORRUPT,
	    "invalid ISO 9660 Joliet descriptor date",
	    "invalid Joliet descriptor date (Feb 30)");
	fixture_new(&f, &image, NULL);
	CHECK(open_image(&f, FSIMG_ISO_VIEW_JOLIET) == FSIMG_OK);
	CHECK(walk(&f) == FSIMG_OK);
	fixture_free(&f);
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
			record(p + DOT_PAIR_BYTES, "DIR", 3, block + 1,
			    BLOCK, 2);
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
	CHECK(info.capabilities == (FSIMG_CAP_READ | FSIMG_CAP_VERIFY));
	CHECK(info.detail_count == 13);
	for (unsigned i = 0; i < 13; i++) {
		CHECK(strcmp(info.details[i].name, details[i]) == 0);
		CHECK(info.details[i].value == values[i]);
	}
	CHECK(fsimg_verify(f.reader, 0, &report, NULL) == FSIMG_OK);
	CHECK(report.complete);
	fixture_free(&f);
	fixture_new(&f, &image, NULL);
	CHECK(open_image(&f, FSIMG_ISO_VIEW_PRIMARY) == FSIMG_OK);
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

#define SHARED_FILE_BYTES (40u * 1024u)
#define SHARED_FILE_COUNT 8u

/* All payloads share an accessible extent. The descriptor declares more
 * space than the stored image, so only the selected I/O length is a safe
 * default budget for their cumulative logical extraction size.
 */
static image_t
shared_extent_image(void)
{
	image_t image = image_new();
	unsigned char *root = image.bytes + ROOT * BLOCK;
	size_t position = DOT_PAIR_BYTES;
	char name[16];

	put32(image.bytes + 16 * BLOCK + VD_SPACE, 1024);
	bzero(root + position, BLOCK - position);
	for (unsigned i = 0; i < SHARED_FILE_COUNT; i++) {
		CHECK(snprintf(name, sizeof(name), "F%03u.TXT;1", i) == 10);
		position += record(root + position, name, strlen(name), DATA,
		    SHARED_FILE_BYTES, 0);
	}
	return (image);
}

static void
test_extraction_budget_uses_selected_io_extent(void)
{
	typedef struct output_case {
		uint64_t context_limit;
		uint64_t extraction_limit;
		uint64_t expected_limit;
		unsigned completed_files;
		fsimg_status_t status;
	} output_case_t;
	static const output_case_t cases[] = {
		{ .expected_limit = BLOCKS * BLOCK, .completed_files = 6,
		  .status = FSIMG_ELIMIT },
		{ .context_limit = SHARED_FILE_COUNT * SHARED_FILE_BYTES,
		  .completed_files = SHARED_FILE_COUNT, .status = FSIMG_OK },
		{ .extraction_limit = SHARED_FILE_COUNT * SHARED_FILE_BYTES,
		  .completed_files = SHARED_FILE_COUNT, .status = FSIMG_OK },
		{ .context_limit = 4 * SHARED_FILE_BYTES,
		  .extraction_limit = SHARED_FILE_COUNT * SHARED_FILE_BYTES,
		  .expected_limit = 4 * SHARED_FILE_BYTES, .completed_files = 4,
		  .status = FSIMG_ELIMIT },
		{ .context_limit = SHARED_FILE_COUNT * SHARED_FILE_BYTES,
		  .extraction_limit = 2 * SHARED_FILE_BYTES,
		  .expected_limit = 2 * SHARED_FILE_BYTES, .completed_files = 2,
		  .status = FSIMG_ELIMIT }
	};
	image_t image = shared_extent_image();
	unsigned char *prefixed;
	fixture_t f;
	fsimg_io_t *selected;
	fsimg_read_options_t read_options;
	fsimg_extract_options_t extract_options;
	fsimg_options_t context_options;
	fsimg_error_t error;
	fsimg_status_t status;
	char temporary[] = "./iso-budget-XXXXXX", name[32];
	struct stat st;
	int parent;

	/* A nonzero slice makes the backing object's extent differ too. */
	prefixed = calloc(1, (size_t)image.stored + BLOCK);
	CHECK(prefixed != NULL);
	bcopy(image.bytes, prefixed + BLOCK, (size_t)image.stored);
	free(image.bytes);
	image.bytes = prefixed;
	image.length += BLOCK;
	image.stored += BLOCK;
	CHECK(mkdtemp(temporary) != NULL);
	parent = open(temporary, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(parent >= 0);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		fsimg_options_init(&context_options);
		context_options.max_output_bytes = cases[i].context_limit;
		fixture_new(&f, &image, &context_options);
		CHECK(fsimg_io_slice(f.io, BLOCK, BLOCKS * BLOCK, &selected,
		    NULL) == FSIMG_OK);
		fsimg_read_options_init(&read_options);
		CHECK(fsimg_reader_open(f.ctx, selected, &read_options, &f.reader,
		    NULL) == FSIMG_OK);
		fsimg_extract_options_init(&extract_options);
		extract_options.max_output_bytes = cases[i].extraction_limit;
		status = fsimg_extract_at(f.reader, parent, "out", &extract_options, &error);
		if (status != cases[i].status)
			fprintf(stderr, "shared extraction case %zu: expected %d, observed %d\n",
			    i, cases[i].status, status);
		CHECK(status == cases[i].status);
		if (status == FSIMG_ELIMIT) {
			CHECK(error.resource == FSIMG_RESOURCE_OUTPUT);
			CHECK(error.limit == cases[i].expected_limit);
			CHECK(error.observed ==
			    (uint64_t)(cases[i].completed_files + 1) * SHARED_FILE_BYTES);
			CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
		}
		for (unsigned file = 0; file < SHARED_FILE_COUNT; file++) {
			snprintf(name, sizeof(name), "out/F%03u.TXT", file);
			if (file < cases[i].completed_files) {
				CHECK(fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) == 0);
				CHECK(st.st_size == SHARED_FILE_BYTES);
			} else {
				CHECK(faccessat(parent, name, F_OK, 0) == -1 && errno == ENOENT);
			}
		}
		fsimg_reader_free(f.reader);
		f.reader = NULL;
		fsimg_io_free(selected);
		fixture_free(&f);
		remove_tree(parent, "out");
	}
	CHECK(close(parent) == 0);
	CHECK(rmdir(temporary) == 0);
	free(image.bytes);
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
	bcopy("19920205102039\0\0", image.bytes + 16 * BLOCK + VD_DATE,
	    DATE17 - 1);
	image.bytes[16 * BLOCK + VD_DATE + DATE17 - 1] = 0xec;
	fixture_new(&f, &image, NULL);
	CHECK(open_image(&f, FSIMG_ISO_VIEW_PRIMARY) == FSIMG_OK);
	CHECK(extract(&f, parent, NULL) == FSIMG_OK);
	fd = openat(parent, "out/A.TXT", O_RDONLY | O_NOFOLLOW);
	CHECK(fd >= 0);
	CHECK(read(fd, buffer, sizeof(buffer)) == 5);
	CHECK(bcmp(buffer, "hello", 5) == 0);
	CHECK(close(fd) == 0);
	fixture_free(&f);
	remove_tree(parent, "out");
	bzero(image.bytes + 16 * BLOCK + VD_DATE, DATE17);
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
	/* Sweep cancellation checkpoints in open, then in open and walk. */
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
		CHECK(formats[i]->probe(image.bytes, 17 * BLOCK, image.length, false,
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
permutations(unsigned char templates[6][BLOCK], unsigned *order,
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
		    image.length, false, &variant, NULL) == FSIMG_EFORMAT);
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
	expect_open(&image, FSIMG_EUNSUPPORTED,
	    "62 section entries exceed catalog");
	boot_header(catalog + 64, 0x90, 0xef, 61);
	expect_open(&image, FSIMG_EUNSUPPORTED,
	    "further header exceeds catalog");
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
		{ "SL current plus parent", NULL, "b", 0x06, 0,
		    FSIMG_ECORRUPT },
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

/* A zero PL omits it; controls and refusal rows share the same PX. */
static void
rr_parent_stream(susp_area_t *area, unsigned parent, const char *refusal)
{
	bzero(area, sizeof(*area));
	susp_px(area, 0040755, PX_BYTES);
	if (parent != 0)
		put32(susp_entry(area, "PL", LINK_BYTES) + 4, parent);
	if (refusal != NULL)
		susp_entry(area, refusal, 4);
}

static void
test_rr_relocation(void)
{
	enum setup {
		DOT_FLAG_CLEAR, MISSING_PL, WRONG_PL, ORDINARY_PL, ROOT_CL,
		ALIAS, LOOP, VALID, PRIMARY, DUPLICATE_CL, CL_RE, RE_REFUSED,
		SEVERAL
	};
	static const struct {
		const char *label;
		enum setup setup;
		fsimg_status_t expected;
		const char *placeholder_refusal, *parent_refusal, *dot_refusal;
	} rows[] = {
		{ .label = "CL target dot flag clear",
		    .setup = DOT_FLAG_CLEAR,
		    .expected = FSIMG_ECORRUPT },
		{ .label = "CL missing PL",
		    .setup = MISSING_PL,
		    .expected = FSIMG_ECORRUPT },
		{ .label = "PL wrong parent",
		    .setup = WRONG_PL,
		    .expected = FSIMG_ECORRUPT },
		{ .label = "PL ordinary directory",
		    .setup = ORDINARY_PL,
		    .expected = FSIMG_ECORRUPT },
		{ .label = "CL root",
		    .setup = ROOT_CL,
		    .expected = FSIMG_ECORRUPT },
		{ .label = "CL and ordinary alias",
		    .setup = ALIAS,
		    .expected = FSIMG_ECORRUPT },
		{ .label = "relocation loop",
		    .setup = LOOP,
		    .expected = FSIMG_ECORRUPT },
		{ .label = "CL placeholder valid",
		    .setup = VALID,
		    .expected = FSIMG_OK },
		{ .label = "primary placeholder",
		    .setup = PRIMARY,
		    .expected = FSIMG_OK },
		{ .label = "duplicate CL on valid placeholder",
		    .setup = DUPLICATE_CL,
		    .expected = FSIMG_ECORRUPT },
		{ .label = "CL and RE on directory PX placeholder",
		    .setup = CL_RE,
		    .expected = FSIMG_ECORRUPT },
		{ .label = "PL wrong parent with ZF",
		    .setup = WRONG_PL,
		    .expected = FSIMG_ECORRUPT,
		    .parent_refusal = "ZF" },
		{ .label = "PL wrong parent with SF",
		    .setup = WRONG_PL,
		    .expected = FSIMG_ECORRUPT,
		    .parent_refusal = "SF" },
		{ .label = "CL missing PL with parent ZF",
		    .setup = MISSING_PL,
		    .expected = FSIMG_ECORRUPT,
		    .parent_refusal = "ZF" },
		{ .label = "ZF placeholder target missing PL",
		    .setup = MISSING_PL,
		    .expected = FSIMG_ECORRUPT,
		    .placeholder_refusal = "ZF" },
		{ .label = "ZF placeholder valid target",
		    .setup = VALID,
		    .expected = FSIMG_EUNSUPPORTED,
		    .placeholder_refusal = "ZF" },
		{ .label = "ZF on skipped RE directory",
		    .setup = RE_REFUSED,
		    .expected = FSIMG_EUNSUPPORTED,
		    .placeholder_refusal = "ZF" },
		{ .label = "CL missing PL with dot ZF", .setup = MISSING_PL,
		    .expected = FSIMG_ECORRUPT, .dot_refusal = "ZF" },
		{ .label = "valid PL with parent ZF", .setup = VALID,
		    .expected = FSIMG_EUNSUPPORTED, .parent_refusal = "ZF" },
		{ .label = "ZF placeholder has several sections",
		    .setup = SEVERAL, .expected = FSIMG_ECORRUPT,
		    .placeholder_refusal = "ZF" }
	};
	susp_area_t area, parent_link, dot;
	image_t image;
	unsigned char *root, *p;
	size_t length;
	unsigned parent;
	fixture_t f;
	const fsimg_entry_t *entry;

	for (unsigned depth = 8; depth <= 20; depth++) {
		for (unsigned several = 0; several < 2; several++)
			rr_relocated_chain(depth, several != 0);
	}
	for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		bzero(&area, sizeof(area));
		susp_px(&area, 0040755, 36);
		if (rows[i].setup == RE_REFUSED)
			susp_entry(&area, "RE", 4);
		else
			put32(susp_entry(&area, "CL", 12) + 4, 85);
		if (rows[i].placeholder_refusal != NULL)
			susp_entry(&area, rows[i].placeholder_refusal, 4);
		image = rr_image(&area, 0);
		root = image.bytes + ROOT * BLOCK;
		p = root + RR_CHILD;
		if (rows[i].setup == RE_REFUSED) {
			put32(p + DR_EXTENT, RR_DIRECTORY);
			put32(p + DR_SIZE, BLOCK);
			p[DR_FLAGS] = 2;
		} else {
			put32(p + DR_SIZE, 0);
			put32(p + DR_EXTENT, MKISOFS_PLACEHOLDER);
		}
		p = image.bytes + 85 * BLOCK;
		directory(p, 85, ROOT, BLOCK);
		if (rows[i].dot_refusal != NULL) {
			bzero(&dot, sizeof(dot));
			susp_px(&dot, 0040755, PX_BYTES);
			susp_entry(&dot, rows[i].dot_refusal, 4);
			bcopy(dot.bytes, p + DOT_BYTES, dot.length);
			p[0] += dot.length;
		}
		parent = ROOT;
		if (rows[i].setup == MISSING_PL)
			parent = 0;
		else if (rows[i].setup == WRONG_PL)
			parent = RR_DIRECTORY + 1;
		rr_parent_stream(&parent_link, parent, rows[i].parent_refusal);
		length = record(p + p[0], "\1", 1, ROOT, BLOCK, 2);
		bcopy(parent_link.bytes, p + p[0] + length,
		    parent_link.length);
		p[p[0]] = length + parent_link.length;
		switch (rows[i].setup) {
		case DOT_FLAG_CLEAR: p[DR_FLAGS] = 0; break;
		case MISSING_PL: break;
		case WRONG_PL: break;
		case SEVERAL:
			p = root + RR_CHILD;
			put32(p + DR_EXTENT, DATA);
			put32(p + DR_SIZE, BLOCK);
			p[DR_FLAGS] = 0x80;
			/* Complete the same run with non-overlapping file data. */
			record(p + p[0], p + DR_NAME, p[DR_NAME_SIZE],
			    DATA + 1, 5, 0);
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
		case LOOP: {
			/* CL returns to its own directory; PL mismatches. */
			size_t child_offset = p[0] + p[p[0]];

			length = record(p + child_offset, "LOOP", 4,
			    MKISOFS_PLACEHOLDER, 0, 0);
			bcopy(area.bytes, p + child_offset + length,
			    area.length);
			p[child_offset] += area.length;
			break;
		}
		case VALID: break;
		case PRIMARY: break;
		case RE_REFUSED: break;
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
		{ "PL on non-relocated dot", "PL", 0040755, 12, FSIMG_ECORRUPT,
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
		{ "ZF then PL", "PL", 0040755, 12, FSIMG_ECORRUPT, "ZF" },
		{ "SF then PL", "PL", 0040755, 12, FSIMG_ECORRUPT, "SF" }
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
	static const struct {
		const char *label;
		bool root, cross_block;
		fsimg_status_t expected;
	} pairs[] = {
		{ .label = "subdirectory dot ZF before forbidden parent PL",
		    .expected = FSIMG_ECORRUPT },
		{ .label = "root dot ZF before forbidden parent PL",
		    .root = true, .expected = FSIMG_ECORRUPT },
		{ .label = "documented exception: parent CE crosses block "
		    "before forbidden PL check", .cross_block = true,
		    .expected = FSIMG_EUNSUPPORTED }
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
			image = rr_dot_image(&dot, positions[j].root,
			    positions[j].parent);
			snprintf(label, sizeof(label), "%s: %s",
			    positions[j].label, checks[i].label);
			rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE,
			    checks[i].expected, label);
			free(image.bytes);
		}
	}
	for (unsigned i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
		bzero(&dot, sizeof(dot));
		if (!pairs[i].cross_block) {
			susp_px(&dot, 0040755, PX_BYTES);
			susp_entry(&dot, "ZF", 4);
		}
		image = rr_dot_image(&dot, pairs[i].root, false);
		rr_parent_stream(&dot, ROOT, NULL);
		if (pairs[i].cross_block)
			susp_ce(&dot, RR_AREA, BLOCK - 2, 4);
		p = image.bytes +
		    (pairs[i].root ? ROOT : RR_DIRECTORY) * BLOCK;
		p += p[0];
		length = p[0];
		/* Preserve the root's child after enlarging its '..'. */
		if (pairs[i].root)
			bcopy(p + length, p + length + dot.length,
			    p[length]);
		bcopy(dot.bytes, p + length, dot.length);
		p[0] += dot.length;
		rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE,
		    pairs[i].expected, pairs[i].label);
		free(image.bytes);
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
	/* Locate the continuation callback independently of other reads. */
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
	    operation < sizeof(operations) / sizeof(operations[0]);
	    operation++) {
		for (unsigned n = 1;; n++) {
			fixture_new(&f, &image, NULL);
			status = open_image(&f, FSIMG_ISO_VIEW_ROCKRIDGE);
			rr_status_equal(status, FSIMG_OK,
			    operations[operation].label);
			fsimg_test_fail_alloc_after(f.ctx, n);
			if (status == FSIMG_OK)
				status = fsimg_reader_next(f.reader, &entry,
				    NULL);
			if (status == FSIMG_OK) {
				rr_equal(entry->kind, FSIMG_ENTRY_SYMLINK,
				    operations[operation].label);
				if (operations[operation].read_target) {
					status = fsimg_reader_read_at(f.reader,
					    0, buffer, sizeof(buffer), &count,
					    NULL);
					if (status == FSIMG_OK) {
						const char *name;

						name =
						    operations[operation].label;
						rr_equal(count, 6, name);
						rr_equal(count == 6 &&
						    bcmp(buffer, "target",
						    6) == 0, 1, name);
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
	static const struct {
		const char *label;
		unsigned flags;
		fsimg_status_t expected;
	} devices[] = {
		{ "device PX missing last section", 0x80, FSIMG_ECORRUPT },
		{ "device PX valid single section", 0, FSIMG_EUNSUPPORTED }
	};
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
	bzero(&area, sizeof(area));
	susp_px(&area, 0100644, PX_BYTES);
	susp_entry(&area, "ZF", 4);
	image = rr_image(&area, 0);
	p = image.bytes + ROOT * BLOCK + RR_CHILD;
	p[DR_NAME] = '/';
	rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE, FSIMG_ECORRUPT,
	    "ZF without NM with invalid primary identifier");
	free(image.bytes);
	/* A refused device's run must still be checked for completeness. */
	for (unsigned i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
		bzero(&area, sizeof(area));
		susp_px(&area, 0020600, PX_BYTES);
		susp_entry(&area, "PN", 20);
		image = rr_image(&area, devices[i].flags);
		p = image.bytes + ROOT * BLOCK + RR_CHILD;
		/* Whole blocks satisfy the size rule for non-final sections. */
		put32(p + DR_SIZE, BLOCK);
		rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE,
		    devices[i].expected, devices[i].label);
		free(image.bytes);
	}
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

/* Open the requested view and report an open failure with its row name. */
static bool
joliet_open(fixture_t *f, image_t *image, fsimg_iso_view_t view,
    const char *row)
{
	fsimg_status_t status;

	fixture_new(f, image, NULL);
	status = open_image(f, view);
	rr_status_equal(status, FSIMG_OK, row);
	return (status == FSIMG_OK);
}

/* Check the ordered paths, end of enumeration, and omission count. */
static void
joliet_names(image_t *image, fsimg_iso_view_t view,
    const char *const *paths, size_t count, uint64_t omitted, const char *row)
{
	fixture_t f;
	const fsimg_entry_t *entry;
	fsimg_stats_t stats;
	fsimg_status_t status;

	if (joliet_open(&f, image, view, row)) {
		for (size_t i = 0; i < count; i++) {
			status = fsimg_reader_next(f.reader, &entry, NULL);
			rr_status_equal(status, FSIMG_OK, row);
			if (status != FSIMG_OK)
				break;
			if (strcmp(entry->path, paths[i]) != 0) {
				fprintf(stderr, "%s: expected path %s, observed %s\n",
				    row, paths[i], entry->path);
				failures++;
			}
		}
		rr_status_equal(fsimg_reader_next(f.reader, &entry, NULL),
		    FSIMG_DONE, row);
		CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
		rr_equal(stats.omitted_entries, omitted, row);
	}
	fixture_free(&f);
}

/* Check the selected profile and its hierarchy and descriptor details. */
static void
joliet_profile(image_t *image, fsimg_iso_view_t view, const char *profile,
    unsigned rockridge, unsigned level, const char *row)
{
	fixture_t f;
	fsimg_volume_info_t info;

	if (joliet_open(&f, image, view, row)) {
		CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
		rr_equal(strcmp(info.profile, profile), 0, row);
		rr_equal(info.details[DETAIL_ROCKRIDGE].value, rockridge, row);
		rr_equal(info.details[DETAIL_JOLIET_LEVEL].value, level, row);
		if (strcmp(profile, "joliet") == 0) {
			rr_equal(info.details[DETAIL_ROOT_BLOCK].value,
			    JOLIET_ROOT, row);
			rr_equal(info.details[DETAIL_PATH_TABLE_BYTES].value,
			    22, row);
		}
	}
	fixture_free(&f);
}

static void
test_joliet_views(void)
{
	static const char escapes[] = { '@', 'C', 'E' };
	static const char *const levels[] = {
		"Joliet %/@ level 1", "Joliet %/C level 2", "Joliet %/E level 3"
	};
	static const char *const primary[] = { "/A.TXT" };
	susp_area_t area = {0};
	image_t image;

	for (unsigned i = 0; i < 3; i++) {
		image = image_new();
		joliet_add(&image);
		image.bytes[17 * BLOCK + VD_ESCAPE + 2] = escapes[i];
		joliet_profile(&image, FSIMG_ISO_VIEW_AUTO, "joliet", 0,
		    i + 1, levels[i]);
		free(image.bytes);
	}
	image = image_new();
	rr_status(&image, FSIMG_ISO_VIEW_JOLIET, FSIMG_EUNSUPPORTED,
	    "forced Joliet without descriptor");
	joliet_add(&image);
	rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE, FSIMG_EUNSUPPORTED,
	    "forced Rock Ridge on Joliet-only image");
	joliet_names(&image, FSIMG_ISO_VIEW_PRIMARY, primary, 1, 0,
	    "forced primary lists primary names beside Joliet");
	image.bytes[17 * BLOCK + VD_ESCAPE + 3] = 1;
	joliet_profile(&image, FSIMG_ISO_VIEW_AUTO, "primary", 0, 0,
	    "nonzero escape tail is not Joliet: auto primary");
	rr_status(&image, FSIMG_ISO_VIEW_JOLIET, FSIMG_EUNSUPPORTED,
	    "nonzero escape tail is not Joliet: forced unsupported");
	free(image.bytes);
	image = rr_image(&area, 0);
	joliet_add(&image);
	joliet_profile(&image, FSIMG_ISO_VIEW_AUTO, "rockridge", 1, 3,
	    "auto prefers Rock Ridge over Joliet");
	image.bytes[ROOT * BLOCK + ROOT_SP + SUSP_LENGTH] = 3;
	joliet_profile(&image, FSIMG_ISO_VIEW_JOLIET, "joliet", 3, 3,
	    "forced Joliet ignores damaged primary handshake");
	free(image.bytes);
}

static void
test_joliet_identifiers(void)
{
	static const struct {
		const char *row;
		uint16_t units[12];
		size_t count;
		const char *displayed;
		fsimg_status_t status;
		bool omitted;
	} rows[] = {
		{ "Joliet BMP e acute and CJK", { 0xe9, 0x4e2d }, 2,
		    "/\xc3\xa9\xe4\xb8\xad", FSIMG_OK, false },
		{ "Joliet surrogate pair", { 0xd83d, 0xde00 }, 2,
		    "/\xf0\x9f\x98\x80", FSIMG_OK, false },
		{ "Joliet terminal high surrogate omitted", { 0xd83d }, 1,
		    NULL, FSIMG_OK, true },
		{ "Joliet high surrogate before ordinary unit omitted",
		    { 0xd83d, 'A' }, 2, NULL, FSIMG_OK, true },
		{ "Joliet lone low surrogate omitted", { 0xde00 }, 1,
		    NULL, FSIMG_OK, true },
		{ "Joliet U+0001 omitted", { 1 }, 1, NULL, FSIMG_OK, true },
		{ "Joliet U+007F omitted", { 127 }, 1, NULL, FSIMG_OK, true },
		{ "Joliet NUL corrupt", { 'A', 0 }, 2, NULL,
		    FSIMG_ECORRUPT, false },
		{ "Joliet slash corrupt", { 'A', '/' }, 2, NULL,
		    FSIMG_ECORRUPT, false },
		{ "Joliet version one display", { 'A','.','T','X','T',';', '1' },
		    7, "/A.TXT", FSIMG_OK, false },
		{ "Joliet version two retained", { 'A','.','T','X','T',';', '2' },
		    7, "/A.TXT;2", FSIMG_OK, false },
		{ "Joliet trailing dot retained", { 'A','.' }, 2,
		    "/A.", FSIMG_OK, false },
		{ "Joliet repeated version corrupt", { 'X',';','1',';','1' },
		    5, NULL, FSIMG_ECORRUPT, false },
		{ "Joliet empty display corrupt", { ';','1' }, 2,
		    NULL, FSIMG_ECORRUPT, false },
		{ "Joliet dot corrupt", { '.' }, 1, NULL,
		    FSIMG_ECORRUPT, false },
		{ "Joliet dot dot corrupt", { '.','.' }, 2, NULL,
		    FSIMG_ECORRUPT, false }
	};
	static const uint16_t sibling[] = { 'B' };
	/* 33 header bytes + 220 name bytes + one pad fill LEN_DR=254. */
	static const unsigned lengths[] = { 64, 103, 110, 110 };
	static const char *const long_rows[] = {
		"Joliet 64 units", "Joliet 103 units", "Joliet maximum 110 units",
		"Joliet maximum 110 three-byte units (330 UTF-8 bytes)"
	};
	image_t image;
	unsigned char *root;
	size_t size;
	uint16_t units[110];
	char path[332];
	const char *paths[2];

	for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		image = image_new();
		joliet_add(&image);
		root = image.bytes + JOLIET_ROOT * BLOCK + DOT_PAIR_BYTES;
		size = joliet_record(root, rows[i].units, rows[i].count,
		    DATA, 5, 0);
		joliet_record(root + size, sibling, 1, DATA, 5, 0);
		if (rows[i].status != FSIMG_OK) {
			rr_status(&image, FSIMG_ISO_VIEW_JOLIET,
			    rows[i].status, rows[i].row);
		} else {
			paths[0] = rows[i].omitted ? "/B" : rows[i].displayed;
			paths[1] = "/B";
			joliet_names(&image, FSIMG_ISO_VIEW_JOLIET, paths,
			    rows[i].omitted ? 1 : 2, rows[i].omitted,
			    rows[i].row);
		}
		free(image.bytes);
	}
	for (unsigned i = 0; i < 4; i++) {
		image = image_new();
		joliet_add(&image);
		path[0] = '/';
		for (unsigned j = 0; j < lengths[i]; j++) {
			units[j] = i == 3 ? 0x4e2d : 'a';
			if (i == 3)
				bcopy("\xe4\xb8\xad", path + 1 + j * 3, 3);
			else
				path[1 + j] = 'a';
		}
		path[1 + lengths[i] * (i == 3 ? 3 : 1)] = '\0';
		joliet_record(image.bytes + JOLIET_ROOT * BLOCK +
		    DOT_PAIR_BYTES, units, lengths[i], DATA, 5, 0);
		paths[0] = path;
		joliet_names(&image, FSIMG_ISO_VIEW_JOLIET, paths, 1, 0,
		    long_rows[i]);
		free(image.bytes);
	}
	image = image_new();
	joliet_add(&image);
	root = image.bytes + JOLIET_ROOT * BLOCK + DOT_PAIR_BYTES;
	record(root, "\0A\0", 3, DATA, 5, 0);
	rr_status(&image, FSIMG_ISO_VIEW_JOLIET, FSIMG_ECORRUPT,
	    "Joliet odd identifier length corrupt");
	free(image.bytes);
}

static void
test_joliet_namespace(void)
{
	static const uint16_t a[] = { 'A' }, version[] = { 'A',';','1' };
	static const uint16_t lower[] = { 'a' }, nfc[] = { 0xe9 };
	static const uint16_t nfd[] = { 'e', 0x301 };
	static const uint16_t unpaired[] = { 0xd83d };
	static const uint16_t unpaired_version[] = { 0xd83d, ';', '1' };
	static const char *const paths[] = { "/A", "/a", "/\xc3\xa9", "/e\xcc\x81" };
	image_t image = image_new();
	unsigned char *root;
	size_t offset;

	joliet_add(&image);
	root = image.bytes + JOLIET_ROOT * BLOCK + DOT_PAIR_BYTES;
	offset = joliet_record(root, a, 1, DATA, 5, 0);
	joliet_record(root + offset, version, 3, DATA, 5, 0);
	rr_status(&image, FSIMG_ISO_VIEW_JOLIET, FSIMG_ECORRUPT,
	    "Joliet A and A;1 collide after display");
	bzero(root + offset, BLOCK - DOT_PAIR_BYTES - offset);
	offset += joliet_record(root + offset, lower, 1, DATA, 5, 0);
	offset += joliet_record(root + offset, nfc, 1, DATA, 5, 0);
	joliet_record(root + offset, nfd, 2, DATA, 5, 0);
	joliet_names(&image, FSIMG_ISO_VIEW_JOLIET, paths, 4, 0,
	    "Joliet case and NFC/NFD spellings are distinct");

	/* Omitted names still participate in display-name collision checks. */
	bzero(root, BLOCK - DOT_PAIR_BYTES);
	offset = joliet_record(root, unpaired, 1, DATA, 5, 0);
	joliet_names(&image, FSIMG_ISO_VIEW_JOLIET, NULL, 0, 1,
	    "Joliet unpaired collision name alone is omitted");
	joliet_record(root + offset, unpaired_version, 3, DATA, 5, 0);
	rr_status(&image, FSIMG_ISO_VIEW_JOLIET, FSIMG_ECORRUPT,
	    "Joliet omitted surrogate and surrogate;1 collide after display");
	bzero(root, BLOCK - DOT_PAIR_BYTES);
	offset = joliet_record(root, unpaired_version, 3, DATA, 5, 0);
	joliet_names(&image, FSIMG_ISO_VIEW_JOLIET, NULL, 0, 1,
	    "Joliet versioned unpaired collision name alone is omitted");
	joliet_record(root + offset, unpaired, 1, DATA, 5, 0);
	rr_status(&image, FSIMG_ISO_VIEW_JOLIET, FSIMG_ECORRUPT,
	    "Joliet omitted surrogate;1 and surrogate collide after display");
	free(image.bytes);
}

static void
test_joliet_tree(void)
{
	static const uint16_t d[] = { 'D' }, alias[] = { 'E' };
	susp_area_t area = {0};
	image_t image;
	unsigned char *root, *child;
	size_t size;

	susp_nm(&area, "primary", 7, 0);
	image = rr_image(&area, 0);
	joliet_add(&image);
	root = image.bytes + JOLIET_ROOT * BLOCK + DOT_PAIR_BYTES;
	child = image.bytes + JOLIET_CHILD * BLOCK;
	size = joliet_record(root, d, 1, JOLIET_CHILD, BLOCK, 2);
	directory(child, JOLIET_CHILD, JOLIET_ROOT, BLOCK);
	rr_status(&image, FSIMG_ISO_VIEW_JOLIET, FSIMG_OK,
	    "Joliet subdirectory parent names parent");
	put32(child + DOT_BYTES + DR_EXTENT, JOLIET_CHILD);
	rr_status(&image, FSIMG_ISO_VIEW_JOLIET, FSIMG_ECORRUPT,
	    "Joliet subdirectory parent names itself corrupt");
	joliet_profile(&image, FSIMG_ISO_VIEW_AUTO, "rockridge", 1, 3,
	    "auto reads Rock Ridge beside corrupt Joliet parent");
	rr_status(&image, FSIMG_ISO_VIEW_AUTO, FSIMG_OK,
	    "auto walks Rock Ridge beside corrupt Joliet parent");
	put32(child + DOT_BYTES + DR_EXTENT, JOLIET_ROOT);
	joliet_record(root + size, alias, 1, JOLIET_CHILD, BLOCK, 2);
	rr_status(&image, FSIMG_ISO_VIEW_JOLIET, FSIMG_ECORRUPT,
	    "two Joliet records opening one directory corrupt");
	bzero(root + size, BLOCK - DOT_PAIR_BYTES - size);
	put32(root + DR_EXTENT, JOLIET_ROOT);
	rr_status(&image, FSIMG_ISO_VIEW_JOLIET, FSIMG_ECORRUPT,
	    "Joliet child names Joliet root corrupt");
	free(image.bytes);
}

static void
test_joliet_label(void)
{
	static const struct {
		const char *row;
		uint16_t units[16];
		const char *label;
	} rows[] = {
		{ "Joliet label non-ASCII with space and NUL padding",
		    { 0xe9, 0x4e2d, ' ', ' ', 0 }, "\xc3\xa9\xe4\xb8\xad" },
		{ "Joliet maximum 16-unit label with non-BMP pairs",
		    { 0xd83d, 0xde00, 'A', 'B', 'C', 'D', 'E', 'F',
		      'G', 'H', 'I', 'J', 'K', 'L', 0xd83d, 0xde01 },
		    "\xf0\x9f\x98\x80" "ABCDEFGHIJKL" "\xf0\x9f\x98\x81" },
		{ "Joliet label unpaired surrogate absent", { 0xd83d }, NULL },
		{ "Joliet label control absent", { 'A', 1 }, NULL }
	};
	image_t image;
	fixture_t f;
	fsimg_volume_info_t info;
	unsigned char *label;

	for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		image = image_new();
		joliet_add(&image);
		label = image.bytes + 17 * BLOCK + VD_LABEL;
		for (unsigned j = 0; j < 16; j++) {
			label[2 * j] = rows[i].units[j] >> 8;
			label[2 * j + 1] = rows[i].units[j];
		}
		if (joliet_open(&f, &image, FSIMG_ISO_VIEW_JOLIET,
		    rows[i].row)) {
			CHECK(fsimg_reader_get_info(f.reader, &info, NULL) == FSIMG_OK);
			rr_equal((info.valid & FSIMG_VOLUME_LABEL) != 0,
			    rows[i].label != NULL, rows[i].row);
			if (rows[i].label != NULL &&
			    (info.valid & FSIMG_VOLUME_LABEL) != 0)
				rr_equal(strcmp(info.label, rows[i].label), 0,
				    rows[i].row);
		}
		fixture_free(&f);
		free(image.bytes);
	}
}

static void
test_joliet_content(void)
{
	static const uint16_t name[] = { 'A','.','T','X','T',';','1' };
	static const fsimg_iso_view_t views[] = {
		FSIMG_ISO_VIEW_PRIMARY, FSIMG_ISO_VIEW_JOLIET
	};
	image_t image = image_new();
	fixture_t f;
	const fsimg_entry_t *entry;
	unsigned char *p;
	unsigned char buffer[BLOCK + 5];
	unsigned char first[5] = {0};
	size_t count, size;
	fsimg_status_t status;
	const char *row = "Joliet and primary shared extent content and metadata";

	joliet_add(&image);
	p = image.bytes + JOLIET_ROOT * BLOCK + DOT_PAIR_BYTES;
	size = joliet_record(p, name, 7, DATA, 5, 1);
	bcopy("\106\1\1\1\0\0\4", p + DR_DATE, 7);
	for (unsigned i = 0; i < 2; i++) {
		if (joliet_open(&f, &image, views[i], row)) {
			status = fsimg_reader_next(f.reader, &entry, NULL);
			rr_status_equal(status, FSIMG_OK, row);
			if (status == FSIMG_OK) {
				rr_status_equal(fsimg_reader_read_at(f.reader, 0,
				    buffer, 5, &count, NULL), FSIMG_OK, row);
				rr_equal(count, 5, row);
				rr_equal(bcmp(buffer, "hello", 5), 0, row);
				if (i == 0)
					bcopy(buffer, first, 5);
				else {
					rr_equal(bcmp(first, buffer, 5), 0, row);
					rr_equal((entry->metadata.attributes &
					    FSIMG_ATTR_HIDDEN) != 0, 1, row);
					rr_equal((entry->metadata.valid &
					    FSIMG_META_MODIFIED) != 0, 1, row);
					rr_equal(entry->metadata.modified.seconds, 0, row);
					rr_equal(entry->metadata.modified.offset_minutes, 60, row);
				}
			}
		}
		fixture_free(&f);
	}
	put32(p + DR_SIZE, BLOCK);
	p[DR_FLAGS] = 0x80;
	joliet_record(p + size, name, 7, DATA + 1, 5, 0);
	bcopy("world", image.bytes + (DATA + 1) * BLOCK, 5);
	row = "Joliet multi-extent file reads both sections";
	if (joliet_open(&f, &image, FSIMG_ISO_VIEW_JOLIET, row)) {
		status = fsimg_reader_next(f.reader, &entry, NULL);
		rr_status_equal(status, FSIMG_OK, row);
		if (status == FSIMG_OK) {
			rr_equal(entry->size, BLOCK + 5, row);
			rr_status_equal(fsimg_reader_read_at(f.reader, 0, buffer,
			    sizeof(buffer), &count, NULL), FSIMG_OK, row);
			rr_equal(count, sizeof(buffer), row);
			rr_equal(bcmp(buffer, image.bytes + DATA * BLOCK, BLOCK), 0, row);
			rr_equal(bcmp(buffer + BLOCK, "world", 5), 0, row);
		}
		rr_status_equal(walk(&f), FSIMG_OK, row);
	}
	fixture_free(&f);
	free(image.bytes);
}

static void
test_joliet_selection(void)
{
	static const uint16_t name[] = { 'A','.','T','X','T',';','1' };
	static const char *const queries[] = { "/A.TXT;1", "/A.TXT" };
	image_t image = image_new();
	fixture_t f;
	char temporary[] = "./iso-joliet-selection-XXXXXX", buffer[5];
	int parent, fd;
	fsimg_status_t status;
	const char *row = "Joliet versioned and displayed queries select same file";

	joliet_add(&image);
	joliet_record(image.bytes + JOLIET_ROOT * BLOCK + DOT_PAIR_BYTES,
	    name, 7, DATA, 5, 0);
	CHECK(mkdtemp(temporary) != NULL);
	parent = open(temporary, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(parent >= 0);
	for (unsigned i = 0; i < 2; i++) {
		if (joliet_open(&f, &image, FSIMG_ISO_VIEW_JOLIET, row)) {
			status = extract(&f, parent, queries[i]);
			rr_status_equal(status, FSIMG_OK, row);
			if (status == FSIMG_OK) {
				fd = openat(parent, "out/A.TXT", O_RDONLY | O_NOFOLLOW);
				CHECK(fd >= 0);
				CHECK(read(fd, buffer, sizeof(buffer)) == sizeof(buffer));
				rr_equal(bcmp(buffer, "hello", 5), 0, row);
				CHECK(close(fd) == 0);
			}
			remove_tree(parent, "out");
		}
		fixture_free(&f);
	}
	CHECK(close(parent) == 0);
	CHECK(rmdir(temporary) == 0);
	free(image.bytes);
}

enum { PT_ROOT = 0, PT_A = 10, PT_B = 20, PT_C = 30, PT_D = 40, PT_END = 50,
    PT_EXTENT = 2, PT_PARENT = 6, PT_NAME = 8, PT_PAD = 9, PT_XATTR = 1 };

/* Every negative verifier assertion also checks the public incomplete
 * contract. Return the report so positive rows can check exact accounting.
 */
static fsimg_verify_report_t
verify_expect(image_t *image, fsimg_iso_view_t view, unsigned flags,
    fsimg_status_t expected, const char *message, const char *name)
{
	fsimg_verify_report_t report = {0};
	fsimg_error_t error = {0};
	fsimg_status_t status;
	fixture_t f;

	fixture_new(&f, image, NULL);
	status = open_image(&f, view);
	rr_status_equal(status, FSIMG_OK, name);
	if (status == FSIMG_OK)
		status = fsimg_verify(f.reader, flags, &report, &error);
	cases++;
	if (status != expected || report.complete != (expected == FSIMG_OK) ||
	    (message != NULL && strstr(error.message, message) == NULL)) {
		fprintf(stderr, "%s: expected status=%d complete=%d message=%s; "
		    "observed status=%d complete=%d message=%s\n", name,
		    expected, expected == FSIMG_OK, message == NULL ? "*" : message,
		    status, report.complete, error.message);
		failures++;
	}
	fixture_free(&f);
	return (report);
}

/* Sorted physical tree: root/{A/C,B/D}. Joliet uses two-byte names. */
static image_t
verify_tree(bool joliet)
{
	static const unsigned extents[] = { 85, 86, 87, 92 };
	static const unsigned parents[] = { ROOT, ROOT, 85, 86 };
	static const unsigned numbers[] = { 1, 1, 2, 3 };
	image_t image = image_new();
	unsigned root = joliet ? JOLIET_ROOT : ROOT;
	unsigned vd = joliet ? 17 : 16;
	unsigned l = joliet ? 90 : 82, m = l + 1;
	unsigned char *p;
	size_t offset = 10;

	if (joliet)
		joliet_add(&image);
	verify_table_root(&image, vd, root, l, m);
	bzero(image.bytes + root * BLOCK, BLOCK);
	directory(image.bytes + root * BLOCK, root, root, BLOCK);
	for (unsigned i = 0; i < 4; i++) {
		unsigned parent = parents[i] == ROOT ? root : parents[i];
		unsigned char name[] = { 0, 'A' + i };
		const unsigned char *identifier = joliet ? name : name + 1;
		size_t length = joliet ? 2 : 1;
		size_t position = DOT_PAIR_BYTES;

		directory(image.bytes + extents[i] * BLOCK, extents[i],
		    parent, BLOCK);
		if (i == 1)
			position += joliet ? 36 : 34;
		record(image.bytes + parent * BLOCK + position, identifier,
		    length, extents[i], BLOCK, 2);
		p = image.bytes + l * BLOCK + offset;
		verify_path(p, identifier, length, extents[i], numbers[i], false);
		p = image.bytes + m * BLOCK + offset;
		offset += verify_path(p, identifier, length, extents[i],
		    numbers[i], true);
	}
	put32(image.bytes + vd * BLOCK + VD_PATH_SIZE, offset);
	return (image);
}

static void
test_verify_counts_distinct_claimed_blocks(void)
{
	image_t image = image_new();
	fsimg_verify_report_t report;

	report = verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_OK,
	    NULL, "baseline distinct blocks");
	/* system 0..15, descriptors 16..17, tables 82,83, root 84, data 104 */
	rr_equal(report.allocated_clusters, 22, "distinct claimed blocks");
	rr_equal(report.free_clusters, 0, "no free-block inference");
	rr_equal(report.bad_clusters, 0, "no bad-block inference");
	rr_equal(report.files, 1, "baseline files");
	rr_equal(report.directories, 1, "baseline root included");
	rr_equal(report.logical_bytes, 5, "baseline logical bytes");
	/* A directory length may end before the zero block tail. */
	put32(image.bytes + 16 * BLOCK + VD_ROOT + DR_SIZE, 108);
	put32(image.bytes + ROOT * BLOCK + DR_SIZE, 108);
	put32(image.bytes + ROOT * BLOCK + DOT_BYTES + DR_SIZE, 108);
	verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_OK, NULL,
	    "directory length need not be block aligned");
	free(image.bytes);
}

static void
test_verify_file_metadata_overlap_is_corrupt(void)
{
	static const struct {
		const char *name;
		unsigned extent;
		const char *message;
	} rows[] = {
		{ "file over directory", ROOT, "file data overlaps a structure" },
		{ "file over volume descriptor", 16,
		    "file data overlaps a structure" },
		{ "file over path table", 82, "file data overlaps a structure" },
		{ "file over system area", 0,
		    "file data overlaps a structure" }
	};

	for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		image_t image = image_new();

		put32(image.bytes + ROOT * BLOCK + DOT_PAIR_BYTES + DR_EXTENT,
		    rows[i].extent);
		/* Extended attributes occupy the reserved blocks; delivered data
		 * starts just after the terminator, inside the content domain.
		 * W claims the attributes too, so only the sweep rejects these.
		 */
		if (rows[i].extent < 18)
			image.bytes[ROOT * BLOCK + DOT_PAIR_BYTES + DR_XATTR] =
			    18 - rows[i].extent;

		verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_ECORRUPT,
		    rows[i].message, rows[i].name);
		free(image.bytes);
	}
}

static void
test_verify_data_sharing_and_empty_sections(void)
{
	image_t image = image_new();
	unsigned char *p = image.bytes + ROOT * BLOCK + DOT_PAIR_BYTES;
	fsimg_verify_report_t report;

	record(p + p[0], "B.TXT;1", 7, DATA, 5, 0);
	report = verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_OK,
	    NULL, "two files share one extent");
	rr_equal(report.allocated_clusters, 22, "shared data counted once");
	record(p + p[0], "B.TXT;1", 7, 0, 0, 0);
	verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_OK, NULL,
	    "empty file at extent zero claims nothing");
	joliet_add(&image);
	verify_table_root(&image, 17, JOLIET_ROOT, 90, 91);
	record(image.bytes + JOLIET_ROOT * BLOCK + DOT_PAIR_BYTES,
	    "\0A\0.\0T\0X\0T\0;\0" "1", 14, DATA, 5, 0);
	verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_OK, NULL,
	    "primary and Joliet share file data");
	free(image.bytes);
}

static void
test_verify_susp_without_rockridge_claims_root_continuation(void)
{
	static const fsimg_iso_view_t views[] = {
		FSIMG_ISO_VIEW_AUTO, FSIMG_ISO_VIEW_PRIMARY, FSIMG_ISO_VIEW_JOLIET
	};

	for (unsigned joliet = 0; joliet < 2; joliet++) {
		for (unsigned unknown_er = 0; unknown_er < 2; unknown_er++) {
			for (unsigned overlap = 0; overlap < 2; overlap++) {
				image_t image = image_new();
				susp_area_t handshake = {0}, area = {0};
				fsimg_verify_report_t report;
				unsigned block = overlap ? DATA : RR_AREA;
				unsigned char *root = image.bytes + ROOT * BLOCK;
				unsigned char *sp, self = 0, up = 1;
				size_t length;

				if (joliet) {
					joliet_add(&image);
					verify_table_root(&image, 17, JOLIET_ROOT, 90, 91);
				}
				if (unknown_er)
					susp_er(&area, "FSIMG_TEST_EXTENSION", 1);
				susp_entry(&area, "ST", 4);
				sp = susp_entry(&handshake, "SP", SP_BYTES);
				sp[4] = 0xbe;
				sp[5] = 0xef;
				susp_ce(&handshake, block, 0, area.length);
				bzero(root, BLOCK);
				length = record(root, &self, 1, ROOT, BLOCK, 2);
				bcopy(handshake.bytes, root + length, handshake.length);
				root[0] += handshake.length + (handshake.length % 2);
				length = root[0];
				length += record(root + length, &up, 1, ROOT, BLOCK, 2);
				record(root + length, "A.TXT;1", 7, DATA, 5, 0);
				bcopy(area.bytes, image.bytes + block * BLOCK, area.length);

				for (unsigned v = 0; v < (joliet ? 3u : 2u); v++) {
					char label[128];

					snprintf(label, sizeof(label),
					    "SUSP without RR: Joliet=%u ER=%u overlap=%u view=%u",
					    joliet, unknown_er, overlap, (unsigned)views[v]);
					report = verify_expect(&image, views[v], 0,
					    overlap ? FSIMG_ECORRUPT : FSIMG_OK,
					    overlap ? "overlap" : NULL, label);
					if (!overlap) {
						/* Baseline 22, CE 110; Joliet adds descriptor
						 * 18, path tables 90/91 and root 88. */
						rr_equal(report.allocated_clusters,
						    joliet ? 27 : 23,
						    label);
					}
				}
				free(image.bytes);
			}
		}
	}
}

static void
test_verify_susp_without_rockridge_parent_link_is_opaque(void)
{
	static const fsimg_iso_view_t views[] = {
		FSIMG_ISO_VIEW_AUTO, FSIMG_ISO_VIEW_PRIMARY
	};
	image_t image = image_new();
	susp_area_t handshake = {0}, parent_link = {0};
	fsimg_verify_report_t report;
	unsigned char *root = image.bytes + ROOT * BLOCK;
	unsigned char *child = image.bytes + RR_DIRECTORY * BLOCK;
	unsigned char *sp, self = 0, up = 1;
	size_t length;

	sp = susp_entry(&handshake, "SP", SP_BYTES);
	sp[4] = 0xbe;
	sp[5] = 0xef;
	susp_er(&handshake, "FSIMG_TEST_EXTENSION", 1);
	bzero(root, BLOCK);
	length = record(root, &self, 1, ROOT, BLOCK, 2);
	bcopy(handshake.bytes, root + length, handshake.length);
	root[0] += handshake.length + (handshake.length % 2);
	length = root[0];
	length += record(root + length, &up, 1, ROOT, BLOCK, 2);
	record(root + length, "A", 1, RR_DIRECTORY, BLOCK, 2);

	/* An unknown extension's PL is framed, without relocation semantics. */
	directory(child, RR_DIRECTORY, ROOT, BLOCK);
	put32(susp_entry(&parent_link, "PL", LINK_BYTES) + 4, ROOT);
	bcopy(parent_link.bytes, child + DOT_PAIR_BYTES, parent_link.length);
	child[DOT_BYTES] += parent_link.length;
	verify_path(image.bytes + 82 * BLOCK + 10, "A", 1,
	    RR_DIRECTORY, 1, false);
	verify_path(image.bytes + 83 * BLOCK + 10, "A", 1,
	    RR_DIRECTORY, 1, true);
	put32(image.bytes + 16 * BLOCK + VD_PATH_SIZE, 20);

	for (unsigned v = 0; v < sizeof(views) / sizeof(views[0]); v++) {
		const char *label = views[v] == FSIMG_ISO_VIEW_AUTO ?
		    "SUSP without RR: parent PL under auto" :
		    "SUSP without RR: parent PL under forced primary";

		report = verify_expect(&image, views[v], 0, FSIMG_OK, NULL, label);
		rr_equal(report.directories, 2, label);
	}
	free(image.bytes);
}

static void
test_verify_susp_skip_without_rockridge_is_unsupported_but_lists(void)
{
	static const fsimg_iso_view_t views[] = {
		FSIMG_ISO_VIEW_AUTO, FSIMG_ISO_VIEW_PRIMARY, FSIMG_ISO_VIEW_JOLIET
	};
	image_t image = image_new();
	susp_area_t handshake = {0};
	unsigned char *root = image.bytes + ROOT * BLOCK, *sp;
	unsigned char self = 0, up = 1;
	size_t length;
	fixture_t f;
	const fsimg_entry_t *entry;

	joliet_add(&image);
	verify_table_root(&image, 17, JOLIET_ROOT, 90, 91);
	joliet_record(image.bytes + JOLIET_ROOT * BLOCK + DOT_PAIR_BYTES,
	    (const uint16_t[]){ 'A', '.', 'T', 'X', 'T', ';', '1' },
	    7, DATA, 5, 0);
	sp = susp_entry(&handshake, "SP", SP_BYTES);
	sp[4] = 0xbe;
	sp[5] = 0xef;
	sp[6] = 1;
	bzero(root, BLOCK);
	length = record(root, &self, 1, ROOT, BLOCK, 2);
	bcopy(handshake.bytes, root + length, handshake.length);
	root[0] += handshake.length + (handshake.length % 2);
	length = root[0];
	length += record(root + length, &up, 1, ROOT, BLOCK, 2);
	record(root + length, "A.TXT;1", 7, DATA, 5, 0);

	for (unsigned v = 0; v < sizeof(views) / sizeof(views[0]); v++) {
		fixture_new(&f, &image, NULL);
		CHECK(open_image(&f, views[v]) == FSIMG_OK);
		CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_OK);
		CHECK(strcmp(entry->path, "/A.TXT") == 0);
		CHECK(fsimg_reader_next(f.reader, &entry, NULL) == FSIMG_DONE);
		fixture_free(&f);
		verify_expect(&image, views[v], 0, FSIMG_EUNSUPPORTED,
		    "unsupported SP skip count", "SUSP skip without RR is incomplete");
	}
	free(image.bytes);
}

static void
test_verify_continuations_share_blocks_but_not_bytes(void)
{
	for (unsigned overlap = 0; overlap < 2; overlap++) {
		susp_area_t area = {0}, ce = {0};
		image_t image;
		unsigned char *p;
		size_t length;

		susp_entry(&area, "ST", 4);
		image = rr_image(&area, 0);
		p = image.bytes + ROOT * BLOCK + RR_CHILD;
		p += p[0];
		length = record(p, "B.TXT;1", 7, DATA, 5, 0);
		susp_ce(&ce, RR_AREA, overlap ? 0 : 4, 4);
		bcopy(ce.bytes, p + length, ce.length);
		p[0] += ce.length;
		bcopy(area.bytes, image.bytes + RR_AREA * BLOCK + 4, 4);
		verify_expect(&image, FSIMG_ISO_VIEW_ROCKRIDGE, 0,
		    overlap ? FSIMG_ECORRUPT : FSIMG_OK,
		    overlap ? "overlap" : NULL,
		    overlap ? "CE byte overlap" : "disjoint CE bytes in one block");
		if (!overlap) {
			put32(image.bytes + ROOT * BLOCK + RR_CHILD + DR_EXTENT,
			    RR_AREA);
			verify_expect(&image, FSIMG_ISO_VIEW_ROCKRIDGE, 0,
			    FSIMG_ECORRUPT, "overlap", "file over continuation");
		}
		free(image.bytes);
	}
}

static void
test_verify_continuation_inside_directory_is_corrupt(void)
{
	susp_area_t area = {0}, ce = {0};
	image_t image;
	unsigned char *p;

	susp_entry(&area, "ST", 4);
	image = rr_image(&area, 0);
	/* The second block contains a valid file record whose name is ST.
	 * This keeps the primary walk valid while RR reads those same bytes.
	 */
	put32(image.bytes + 16 * BLOCK + VD_ROOT + DR_SIZE, 2 * BLOCK);
	put32(image.bytes + ROOT * BLOCK + DR_SIZE, 2 * BLOCK);
	put32(image.bytes + ROOT * BLOCK + ROOT_DOT_BYTES + DR_SIZE, 2 * BLOCK);
	p = image.bytes + (ROOT + 1) * BLOCK;
	record(p, area.bytes, area.length, DATA, 0, 0);
	susp_ce(&ce, ROOT + 1, DR_NAME, 4);
	p = image.bytes + ROOT * BLOCK + RR_CHILD + CHILD_SUSP;
	bcopy(ce.bytes, p, ce.length);
	verify_expect(&image, FSIMG_ISO_VIEW_ROCKRIDGE, 0, FSIMG_ECORRUPT,
	    "continuation area overlaps another structure", "CE inside directory block");
	free(image.bytes);
}

static void
test_verify_directory_and_path_table_overlap_is_corrupt(void)
{
	image_t image = image_new();

	/* CE-in-directory above exercises a nonidentical metadata overlap
	 * after both walks; nested directory dots fail the walk earlier.
	 */
	boot_le32(image.bytes + 16 * BLOCK + VD_PATH_L, ROOT);
	verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_ECORRUPT,
	    "overlap", "path table over directory");
	free(image.bytes);
	image = verify_tree(true);
	boot_le32(image.bytes + 17 * BLOCK + VD_PATH_L, 82);
	verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_ECORRUPT,
	    "path table overlaps another structure", "hierarchies share path table");
	free(image.bytes);
}

static void
test_verify_boot_catalog_and_image_sharing(void)
{
	image_t image = boot_image(0);
	unsigned char *p = image.bytes + ROOT * BLOCK + DOT_PAIR_BYTES;
	fsimg_verify_report_t report;

	verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_OK, NULL,
	    "boot image is also a tree file");
	bzero(p, BLOCK - DOT_PAIR_BYTES);
	record(p, "BOOT.CAT;1", 10, BOOT_CATALOG, BLOCK, 0);
	verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_OK, NULL,
	    "catalog published as exact one-section file");
	put32(p + DR_SIZE, 2 * BLOCK);
	verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_ECORRUPT,
	    "overlap", "file larger than catalog overlaps catalog");
	put32(p + DR_SIZE, BLOCK);
	/* Eight sectors cover the catalog and its following block. */
	boot_entry(image.bytes + BOOT_CATALOG * BLOCK + 32, 0, 8, BOOT_CATALOG);
	verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_ECORRUPT,
	    "file data overlaps a structure", "boot image partially overlaps catalog");
	boot_entry(image.bytes + BOOT_CATALOG * BLOCK + 32, 0, 12, BLOCKS - 1);

	report = verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_OK,
	    NULL, "boot image crosses volume end");
	/* baseline minus data, plus boot descriptor, catalog and final block */
	rr_equal(report.allocated_clusters, 24, "boot claims only in-volume part");
	free(image.bytes);
}

static void
test_verify_path_tables_reject_malformed_records(void)
{
	enum mutation { DIFFER, COPY_DIFFER, M_COPY_DIFFER, COPY_EQUAL,
	    COPY_ALIAS, PARENT_SELF,
	    MISSING, EXTRA, WRONG_PARENT, WRONG_NAME, TRUNCATED, PAD,
	    NOT_ROOT, OUTSIDE, XATTR };
	static const struct {
		const char *name;
		enum mutation mutation;
		fsimg_status_t expected;
		const char *message;
	} rows[] = {
		{ "L and M differ", DIFFER, FSIMG_ECORRUPT, "type L and M" },
		{ "optional copy differs", COPY_DIFFER, FSIMG_ECORRUPT, "copy differs" },
		{ "optional M copy differs", M_COPY_DIFFER, FSIMG_ECORRUPT, "copy differs" },
		{ "optional copy equal", COPY_EQUAL, FSIMG_OK, NULL },
		{ "optional copy aliases table", COPY_ALIAS, FSIMG_ECORRUPT, "path table overlaps another structure" },
		{ "parent does not precede record", PARENT_SELF, FSIMG_ECORRUPT, "parent does not precede" },
		{ "directory missing", MISSING, FSIMG_ECORRUPT, "path table does not match the directories" },
		{ "extra directory", EXTRA, FSIMG_ECORRUPT, "path table does not match the directories" },
		{ "wrong parent", WRONG_PARENT, FSIMG_ECORRUPT, "path table does not match the directories" },
		{ "wrong identifier", WRONG_NAME, FSIMG_ECORRUPT, "path table does not match the directories" },
		{ "record truncated", TRUNCATED, FSIMG_ECORRUPT, "ends inside a record" },
		{ "nonzero pad", PAD, FSIMG_ECORRUPT, "pad byte is not zero" },
		{ "first record not root", NOT_ROOT, FSIMG_ECORRUPT, "does not begin with the root" },
		{ "table outside volume", OUTSIDE, FSIMG_ECORRUPT, "path table lies outside the volume" },
		{ "extended attributes", XATTR, FSIMG_EUNSUPPORTED, "extended attribute" }
	};

	for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		image_t image = verify_tree(false);
		unsigned char *vd = image.bytes + 16 * BLOCK;
		unsigned char *l = image.bytes + 82 * BLOCK;
		unsigned char *m = image.bytes + 83 * BLOCK;

		switch (rows[i].mutation) {
		case DIFFER: m[PT_A + PT_EXTENT + 3]++; break;
		case COPY_DIFFER:
		case COPY_EQUAL:
			boot_le32(vd + VD_PATH_L + 4, 80);
			bcopy(l, image.bytes + 80 * BLOCK, PT_END);
			if (rows[i].mutation == COPY_DIFFER)
				image.bytes[80 * BLOCK + PT_A + PT_NAME] = 'Z';
			break;
		case M_COPY_DIFFER:
			/* Optional type-M copy changes A's identifier. */
			vd[VD_PATH_M + 7] = 80;
			bcopy(m, image.bytes + 80 * BLOCK, PT_END);
			image.bytes[80 * BLOCK + PT_A + PT_NAME] = 'Z';
			break;
		case COPY_ALIAS: boot_le32(vd + VD_PATH_L + 4, 82); break;
		case PARENT_SELF: l[PT_A + PT_PARENT] = m[PT_A + PT_PARENT + 1] = 2; break;
		case MISSING: put32(vd + VD_PATH_SIZE, 40); break;
		case EXTRA:
			verify_path(l + PT_END, "E", 1, 93, 3, false);
			verify_path(m + PT_END, "E", 1, 93, 3, true);
			put32(vd + VD_PATH_SIZE, 60);
			break;
		case WRONG_PARENT: l[PT_D + PT_PARENT] = m[PT_D + PT_PARENT + 1] = 2; break;
		case WRONG_NAME: l[PT_D + PT_NAME] = m[PT_D + PT_NAME] = 'Z'; break;
		case TRUNCATED: put32(vd + VD_PATH_SIZE, 49); break;
		case PAD: l[PT_ROOT + PT_PAD] = m[PT_ROOT + PT_PAD] = 1; break;
		case NOT_ROOT: l[PT_ROOT + PT_NAME] = m[PT_ROOT + PT_NAME] = 'R'; break;
		case OUTSIDE: boot_le32(vd + VD_PATH_L, BLOCKS); break;
		case XATTR: l[PT_ROOT + PT_XATTR] = m[PT_ROOT + PT_XATTR] = 1; break;
		}
		if (rows[i].mutation == OUTSIDE) {
			fsimg_read_options_t options;
			fsimg_error_t error = {0};
			fixture_t f;

			/* Placement is checked at open, before W is available. */
			fixture_new(&f, &image, NULL);
			fsimg_read_options_init(&options);
			options.iso_view = FSIMG_ISO_VIEW_PRIMARY;
			rr_status_equal(fsimg_reader_open(f.ctx, f.io, &options,
			    &f.reader, &error), FSIMG_ECORRUPT, rows[i].name);
			CHECK(strstr(error.message, rows[i].message) != NULL);
			CHECK(f.reader == NULL);
			fixture_free(&f);
		} else
			verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0,
			    rows[i].expected, rows[i].message, rows[i].name);
		free(image.bytes);
	}
}

static void
test_verify_primary_path_order_and_joliet_structural_order(void)
{
	for (unsigned joliet = 0; joliet < 2; joliet++) {
		for (unsigned decrease = 0; decrease < 2; decrease++) {
			image_t image = verify_tree(joliet != 0);
			unsigned l = joliet ? 90 : 82;
			unsigned char saved[12];
			size_t size = 10;

			verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_OK,
			    NULL, "sorted directory tree baseline");
			for (unsigned table = l; table <= l + 1; table++) {
				unsigned char *p = image.bytes + table * BLOCK;
				size_t a = decrease ? PT_C : PT_A;
				size_t b = decrease ? PT_D : PT_B;

				bcopy(p + a, saved, size);
				bcopy(p + b, p + a, size);
				bcopy(saved, p + b, size);
				/* Swapping A/B also swaps their record numbers. */
				if (!decrease) {
					p[PT_C + PT_PARENT + (table != l)] = 3;
					p[PT_D + PT_PARENT + (table != l)] = 2;
				}
			}
			verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0,
			    joliet ? FSIMG_OK : FSIMG_ECORRUPT,
			    joliet ? NULL : "primary path table is not sorted",
			    decrease ? "parent numbers decrease" : "identifiers descend");
			if (joliet) {
				image.bytes[l * BLOCK + PT_A + PT_PARENT] = 2;
				image.bytes[(l + 1) * BLOCK + PT_A + PT_PARENT + 1] = 2;
				verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0,
				    FSIMG_ECORRUPT, "parent does not precede",
				    "Joliet parent must precede its own record");
			}
			free(image.bytes);
		}
	}
}

static void
test_verify_path_record_can_cross_block_boundary(void)
{
	image_t image = image_new();
	unsigned char name[220];
	size_t offset = 10, position = DOT_PAIR_BYTES;

	verify_table_root(&image, 16, ROOT, 70, 72);
	bzero(image.bytes + ROOT * BLOCK, 2 * BLOCK);
	directory(image.bytes + ROOT * BLOCK, ROOT, ROOT, 2 * BLOCK);
	put32(image.bytes + 16 * BLOCK + VD_ROOT + DR_SIZE, 2 * BLOCK);
	for (unsigned i = 0; i < 9; i++) {
		memset(name, 'A' + i, sizeof(name));
		if (position % BLOCK + 254 > BLOCK)
			position += BLOCK - position % BLOCK;
		position += record(image.bytes + ROOT * BLOCK + position,
		    name, sizeof(name), 30 + i, BLOCK, 2);
		directory(image.bytes + (30 + i) * BLOCK, 30 + i, ROOT, BLOCK);
		put32(image.bytes + (30 + i) * BLOCK + DOT_BYTES + DR_SIZE,
		    2 * BLOCK);
		verify_path(image.bytes + 70 * BLOCK + offset, name,
		    sizeof(name), 30 + i, 1, false);
		offset += verify_path(image.bytes + 72 * BLOCK + offset, name,
		    sizeof(name), 30 + i, 1, true);
	}
	CHECK(offset == 2062);
	put32(image.bytes + 16 * BLOCK + VD_PATH_SIZE, offset);
	verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_OK, NULL,
	    "path record crosses block boundary");
	free(image.bytes);
}

static void
test_verify_excess_path_table_records_are_corrupt(void)
{
	image_t image = image_new();
	unsigned char zero = 0;
	const size_t bytes = 65536 * 10;
	const unsigned l = 128, m = 449, blocks = 770;

	image.bytes = realloc(image.bytes, blocks * BLOCK);
	CHECK(image.bytes != NULL);
	bzero(image.bytes + image.stored, blocks * BLOCK - image.stored);
	image.stored = image.length = blocks * BLOCK;
	put32(image.bytes + 16 * BLOCK + VD_SPACE, blocks);
	verify_table_root(&image, 16, ROOT, l, m);
	for (unsigned i = 1; i < 65536; i++) {
		verify_path(image.bytes + l * BLOCK + i * 10, &zero, 1,
		    ROOT, 1, false);
		verify_path(image.bytes + m * BLOCK + i * 10, &zero, 1,
		    ROOT, 1, true);
	}
	put32(image.bytes + 16 * BLOCK + VD_PATH_SIZE, bytes);
	verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_ECORRUPT,
	    "path table does not match the directories", "excess path table records");
	free(image.bytes);
}

static void
test_verify_directory_count_limit(void)
{
	const unsigned blocks = VERIFY_DIRECTORY_FIRST_CHILD + VERIFY_DIRECTORY_CHILDREN;
	image_t image = image_new();

	image.directory_scale = true;
	image.length = (uint64_t)blocks * BLOCK;
	put32(image.bytes + 16 * BLOCK + VD_SPACE, blocks);
	put32(image.bytes + 16 * BLOCK + VD_ROOT + DR_SIZE,
	    VERIFY_DIRECTORY_ROOT_BLOCKS * BLOCK);
	verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_EUNSUPPORTED,
	    "more than 65535 directories", "walk directory count exceeds path numbering");
	free(image.bytes);
}

static void
test_verify_primary_directory_order_and_versions(void)
{
	static const struct {
		const char *first;
		const char *second;
		fsimg_status_t expected;
		const char *name;
	} rows[] = {
		{ "B.TXT;1", "A.TXT;1", FSIMG_ECORRUPT, "primary records descend" },
		{ "FOO.TXT;2", "FOO.TXT;1", FSIMG_OK, "versions descend" },
		{ "FOO.TXT;1", "FOO.TXT;2", FSIMG_ECORRUPT, "versions ascend" },
		{ "A;B;2", "A;B;1", FSIMG_OK, "last semicolon versions descend" },
		{ "A;B;1", "A;B;2", FSIMG_ECORRUPT, "last semicolon versions ascend" }
	};

	for (unsigned rr = 0; rr < 2; rr++) {
		for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
			susp_area_t area = {0};
			image_t image = rr ? rr_image(&area, 0) : image_new();
			unsigned start = rr ? RR_CHILD : DOT_PAIR_BYTES;
			unsigned char *p = image.bytes + ROOT * BLOCK + start;
			size_t length;

			bzero(p, BLOCK - start);
			length = record(p, rows[i].first, strlen(rows[i].first),
			    DATA, 5, 0);
			record(p + length, rows[i].second, strlen(rows[i].second),
			    DATA + 1, 5, 0);
			verify_expect(&image, rr ? FSIMG_ISO_VIEW_ROCKRIDGE :
			    FSIMG_ISO_VIEW_PRIMARY, 0, rows[i].expected,
			    rows[i].expected == FSIMG_OK ? NULL :
			    "primary directory records are not sorted", rows[i].name);
			free(image.bytes);
		}
	}
}

static void
test_verify_directory_and_file_equal_order_keys(void)
{
	for (unsigned reverse = 0; reverse < 2; reverse++) {
		image_t image = image_new();
		unsigned char *p = image.bytes + ROOT * BLOCK + DOT_PAIR_BYTES;

		bzero(p, BLOCK - DOT_PAIR_BYTES);
		if (reverse)
			p += record(p, "FOO.;1", 6, DATA, 5, 0);
		p += record(p, "FOO", 3, 85, BLOCK, 2);
		if (!reverse)
			record(p, "FOO.;1", 6, DATA, 5, 0);
		directory(image.bytes + 85 * BLOCK, 85, ROOT, BLOCK);
		verify_path(image.bytes + 82 * BLOCK + 10, "FOO", 3, 85, 1, false);
		verify_path(image.bytes + 83 * BLOCK + 10, "FOO", 3, 85, 1, true);
		put32(image.bytes + 16 * BLOCK + VD_PATH_SIZE, 22);
		verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_OK, NULL,
		    reverse ? "file before equal-key directory" :
		    "directory before equal-key file");
		free(image.bytes);
	}
}

static void
test_verify_versions_descend_across_directory_file_ties(void)
{
	static const struct {
		const char *names[3];
		fsimg_status_t expected;
		const char *label;
	} rows[] = {
		{ { "FOO.;1", "FOO", "FOO.;2" }, FSIMG_ECORRUPT,
		    "ascending versions across directory tie" },
		{ { "FOO.;2", "FOO", "FOO.;1" }, FSIMG_OK,
		    "descending versions across directory tie" },
		{ { "FOO", "FOO.;2", "FOO.;1" }, FSIMG_OK,
		    "directory precedes descending versions" },
		{ { "FOO.;1", "FOO.;2", "FOO" }, FSIMG_ECORRUPT,
		    "directory follows ascending versions" },
		{ { "FOO", "FOO.;1", "FOO.;2" }, FSIMG_ECORRUPT,
		    "directory precedes ascending versions" },
		{ { "FOO.;2", "FOO.;1", "FOO" }, FSIMG_OK,
		    "directory follows descending versions" }
	};

	for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		image_t image = image_new();
		unsigned char *p = image.bytes + ROOT * BLOCK + DOT_PAIR_BYTES;

		bzero(p, BLOCK - DOT_PAIR_BYTES);
		for (unsigned n = 0; n < 3; n++) {
			bool is_directory = strcmp(rows[i].names[n], "FOO") == 0;

			p += record(p, rows[i].names[n], strlen(rows[i].names[n]),
			    is_directory ? 85 : DATA + n,
			    is_directory ? BLOCK : 5, is_directory ? 2 : 0);
		}
		directory(image.bytes + 85 * BLOCK, 85, ROOT, BLOCK);
		verify_path(image.bytes + 82 * BLOCK + 10, "FOO", 3, 85, 1, false);
		verify_path(image.bytes + 83 * BLOCK + 10, "FOO", 3, 85, 1, true);
		put32(image.bytes + 16 * BLOCK + VD_PATH_SIZE, 22);
		verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, rows[i].expected,
		    rows[i].expected == FSIMG_OK ? NULL :
		    "primary directory records are not sorted", rows[i].label);
		free(image.bytes);
	}
}

static void
test_verify_unsorted_joliet_directory_is_accepted(void)
{
	image_t image = verify_tree(true);
	unsigned char *p = image.bytes + JOLIET_ROOT * BLOCK + DOT_PAIR_BYTES;
	unsigned char saved[36];

	bcopy(p, saved, 36);
	bcopy(p + 36, p, 36);
	bcopy(saved, p + 36, 36);
	verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_OK, NULL,
	    "unsorted Joliet beside sorted primary");
	/* Root record is ten bytes; the first UCS-2 child name starts at +8. */
	image.bytes[90 * BLOCK + PT_A + PT_NAME + 1] = 'Z';
	image.bytes[91 * BLOCK + PT_A + PT_NAME + 1] = 'Z';
	verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_ECORRUPT,
	    "path table does not match the directories", "Joliet table identifier mismatch");

	free(image.bytes);
}

/* Relocate Z into the logical root: root/{A(CL),RR_MOVED(RE)/{Z(RE)}}.
 * Primary sees an empty A file and the physical RR_MOVED/Z tree.
 * RE hides RR_MOVED in the Rock Ridge view.
 */
static image_t
verify_relocation_image(void)
{
	susp_area_t cl = {0}, pl = {0};
	image_t image;
	unsigned char *p;
	size_t length;

	put32(susp_entry(&cl, "CL", 12) + 4, 86);
	image = rr_image(&cl, 0);
	p = image.bytes + ROOT * BLOCK + RR_CHILD;
	put32(p + DR_SIZE, 0);
	put32(p + DR_EXTENT, MKISOFS_PLACEHOLDER);
	p += p[0];
	length = record(p, "RR_MOVED", 8, 85, BLOCK, 2);
	bcopy("RE\004\001", p + length, 4);
	p[0] += 4;
	directory(image.bytes + 85 * BLOCK, 85, ROOT, BLOCK);
	p = image.bytes + 85 * BLOCK + DOT_PAIR_BYTES;
	length = record(p, "Z", 1, 86, BLOCK, 2);
	bcopy("RE\004\001", p + length, 4);
	p[0] += 4;
	p = image.bytes + 86 * BLOCK;
	directory(p, 86, 85, BLOCK);
	put32(susp_entry(&pl, "PL", 12) + 4, ROOT);
	bcopy(pl.bytes, p + DOT_PAIR_BYTES, pl.length);
	p[DOT_BYTES] += pl.length;
	verify_path(image.bytes + 82 * BLOCK + 10, "RR_MOVED", 8, 85, 1, false);
	verify_path(image.bytes + 83 * BLOCK + 10, "RR_MOVED", 8, 85, 1, true);
	verify_path(image.bytes + 82 * BLOCK + 26, "Z", 1, 86, 2, false);
	verify_path(image.bytes + 83 * BLOCK + 26, "Z", 1, 86, 2, true);
	put32(image.bytes + 16 * BLOCK + VD_PATH_SIZE, 36);
	joliet_add(&image);
	verify_table_root(&image, 17, JOLIET_ROOT, 90, 91);
	return (image);
}

static void
test_verify_relocation_matches_cl_and_pl_and_hides_re(void)
{
	static const fsimg_iso_view_t views[] = {
		FSIMG_ISO_VIEW_ROCKRIDGE, FSIMG_ISO_VIEW_PRIMARY,
		FSIMG_ISO_VIEW_JOLIET
	};
	image_t image = verify_relocation_image();
	fsimg_verify_report_t report;

	for (unsigned i = 0; i < sizeof(views) / sizeof(views[0]); i++) {
		report = verify_expect(&image, views[i], 0, FSIMG_OK, NULL,
		    "valid relocation in every selected view");
		rr_equal(report.files, views[i] == FSIMG_ISO_VIEW_PRIMARY ? 1 : 0,
		    "selected view files");
		rr_equal(report.directories, views[i] == FSIMG_ISO_VIEW_JOLIET ? 1 :
		    views[i] == FSIMG_ISO_VIEW_PRIMARY ? 3 : 2,
		    "selected view directories");
		rr_equal(report.logical_bytes, 0, "selected view logical bytes");
	}
	{
		static const char *const names[] = { "/A.TXT" };

		joliet_names(&image, FSIMG_ISO_VIEW_ROCKRIDGE, names, 1, 0,
		    "producer relocation directory is hidden");
	}
	/* Remove CL while retaining a structurally valid empty placeholder. */
	bzero(image.bytes + RR_AREA * BLOCK, 12);
	rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE, FSIMG_OK,
	    "orphaned PL directory still lists");
	{
		static const char *const names[] = { "/A.TXT" };

		joliet_names(&image, FSIMG_ISO_VIEW_ROCKRIDGE, names, 1, 0,
		    "orphaned PL directory is hidden from listing");
	}
	for (unsigned i = 0; i < sizeof(views) / sizeof(views[0]); i++)
		verify_expect(&image, views[i], 0, FSIMG_ECORRUPT,
		    "Rock Ridge relocated directories and CL entries do not match",
		    "orphaned PL directory");
	/* Without PL these RE markers merely hide ordinary directories. */
	bzero(image.bytes + 86 * BLOCK + DOT_PAIR_BYTES, LINK_BYTES);
	image.bytes[86 * BLOCK + DOT_BYTES] = DOT_BYTES;
	for (unsigned i = 0; i < sizeof(views) / sizeof(views[0]); i++)
		verify_expect(&image, views[i], 0, FSIMG_OK, NULL,
		    "unreferenced RE without PL verifies");
	free(image.bytes);
	image = verify_relocation_image();
	bzero(image.bytes + 85 * BLOCK + DOT_PAIR_BYTES, BLOCK - DOT_PAIR_BYTES);
	put32(image.bytes + 16 * BLOCK + VD_PATH_SIZE, 26);
	rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE, FSIMG_OK,
	    "CL target outside primary tree still lists");
	verify_expect(&image, FSIMG_ISO_VIEW_ROCKRIDGE, 0, FSIMG_ECORRUPT,
	    "Rock Ridge relocated directories and CL entries do not match",
	    "CL target with PL outside primary tree");
	free(image.bytes);
	image = verify_relocation_image();
	/* RR validates PL logically, W must also validate the physical parent. */
	put32(image.bytes + 86 * BLOCK + DOT_BYTES + DR_EXTENT, ROOT);
	verify_expect(&image, FSIMG_ISO_VIEW_ROCKRIDGE, 0, FSIMG_ECORRUPT,
	    "directory's parent record does not name its parent",
	    "CL target physical parent differs from its physical container");
	free(image.bytes);
}

/* RE hides an ordinary directory and its child from the Rock Ridge walk.
 * The primary walk must frame the child's SUSP without interpreting TF.
 */
static void
test_verify_hidden_subtree_susp_framing(void)
{
	susp_area_t area = {0};
	image_t image = rr_image(&area, 0);
	unsigned char *p = image.bytes + ROOT * BLOCK + RR_CHILD;
	size_t length;

	bzero(p, BLOCK - RR_CHILD);
	length = record(p, "HIDDEN", 6, 85, BLOCK, 2);
	bcopy("RE\004\001", p + length, 4);
	p[0] += 4;
	directory(image.bytes + 85 * BLOCK, 85, ROOT, BLOCK);
	p = image.bytes + 85 * BLOCK + DOT_PAIR_BYTES;
	length = record(p, "CHILD;1", 7, DATA, 5, 0);
	/* TF selects a timestamp without supplying it; framing is valid. */
	bcopy("TF\005\001\100", p + length, 5);
	p[0] += 6;
	verify_path(image.bytes + 82 * BLOCK + 10, "HIDDEN", 6, 85, 1, false);
	verify_path(image.bytes + 83 * BLOCK + 10, "HIDDEN", 6, 85, 1, true);
	put32(image.bytes + 16 * BLOCK + VD_PATH_SIZE, 24);
	joliet_names(&image, FSIMG_ISO_VIEW_ROCKRIDGE, NULL, 0, 0,
	    "RE hides ordinary directory and child");
	verify_expect(&image, FSIMG_ISO_VIEW_ROCKRIDGE, 0, FSIMG_OK, NULL,
	    "hidden subtree well-framed TF with missing timestamp");
	/* The entry claims seven bytes in a six-byte area. */
	p[length + 2] = 7;
	joliet_names(&image, FSIMG_ISO_VIEW_ROCKRIDGE, NULL, 0, 0,
	    "RE hides child with malformed SUSP");
	verify_expect(&image, FSIMG_ISO_VIEW_ROCKRIDGE, 0, FSIMG_ECORRUPT,
	    "SUSP entry extends past its area", "primary walk frames hidden child SUSP");
	free(image.bytes);
}

/* Open without faults so every injected allocation belongs to verification.
 * The first untouched attempt proves the entire allocation sequence ran.
 * fixture_free checks all memory, scratch and descriptors after each prefix.
 */
static void
test_verify_allocation_failures_are_incomplete(void)
{
	image_t image = verify_relocation_image();
	fsimg_verify_report_t report;
	fsimg_status_t status;
	fixture_t f;
	bool allocation_hit;

	/* A regular child exercises actual payload delivery in both trees. */
	record(image.bytes + 86 * BLOCK + RELOCATED_CHILD, "PAYLOAD;1", 9,
	    DATA, 5, 0);

	for (unsigned payload = 0; payload < 2; payload++) {
		for (unsigned n = 1;; n++) {
			fixture_new(&f, &image, NULL);
			CHECK(open_image(&f, FSIMG_ISO_VIEW_ROCKRIDGE) == FSIMG_OK);
			fsimg_test_fail_alloc_after(f.ctx, n);
			status = fsimg_verify(f.reader,
			    payload ? FSIMG_VERIFY_PAYLOAD : 0, &report, NULL);
			allocation_hit = f.ctx->allocation_attempts >= n;
			if (allocation_hit) {
				CHECK(status != FSIMG_OK);
				CHECK(!report.complete);
			} else {
				CHECK(status == FSIMG_OK);
				CHECK(report.complete);
			}
			fsimg_test_fail_alloc_after(f.ctx, 0);
			fixture_free(&f);
			if (!allocation_hit) {
				printf("ISO verifier allocation faults: payload=%u attempts=%u\n",
				    payload, n - 1);
				break;
			}
			CHECK(n < 10000);
		}
	}
	free(image.bytes);
}

/* Sweep every cancellation checkpoint through all offered walks, claim
 * sorting/sweeping, path comparison and relocation matching. Opening uses
 * an unexhausted budget so each injected cancellation belongs to W.
 * fixture_free checks memory, scratch and descriptor accounting each time.
 */
static void
test_verify_cancellation_at_every_checkpoint_is_incomplete(void)
{
	image_t image = verify_relocation_image();
	fsimg_options_t options;
	fsimg_verify_report_t report;
	fsimg_status_t status;
	fixture_t f;
	unsigned budget, checkpoints;
	bool cancelled_after_walks = false;

	fsimg_options_init(&options);
	options.cancelled = cancelled;
	options.cancel_argument = &budget;
	for (checkpoints = 0;; checkpoints++) {
		budget = UINT_MAX;
		fixture_new(&f, &image, &options);
		CHECK(open_image(&f, FSIMG_ISO_VIEW_ROCKRIDGE) == FSIMG_OK);
		budget = checkpoints;
		bzero(&report, sizeof(report));
		status = fsimg_verify(f.reader, 0, &report, NULL);
		CHECK(status == FSIMG_OK || status == FSIMG_ECANCELLED);
		if (status == FSIMG_ECANCELLED) {
			CHECK(!report.complete);
			/* Allocated blocks are counted only in the claim sweep,
			 * after every offered walk has finished.
			 */
			if (report.allocated_clusters > 0)
				cancelled_after_walks = true;
		} else
			CHECK(report.complete);
		fixture_free(&f);
		if (status == FSIMG_OK)
			break;
		CHECK(checkpoints < 10000);
	}
	CHECK(cancelled_after_walks);
	printf("ISO verifier cancellation: checkpoints=%u\n", checkpoints);
	free(image.bytes);
}

static void
test_verify_checks_unselected_joliet_parent_records(void)
{
	susp_area_t area = {0};
	image_t image;

	susp_entry(&area, "ST", 4);
	image = rr_image(&area, 0);
	joliet_add(&image);
	record(image.bytes + JOLIET_ROOT * BLOCK + DOT_PAIR_BYTES,
	    "\0D", 2, JOLIET_CHILD, BLOCK, 2);
	directory(image.bytes + JOLIET_CHILD * BLOCK,
	    JOLIET_CHILD, JOLIET_CHILD, BLOCK);
	rr_status(&image, FSIMG_ISO_VIEW_ROCKRIDGE, FSIMG_OK,
	    "RR listing ignores corrupt unselected Joliet");
	verify_expect(&image, FSIMG_ISO_VIEW_ROCKRIDGE, 0, FSIMG_ECORRUPT,
	    "directory's parent record does not name its parent",
	    "RR verification checks Joliet parent");
	verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_ECORRUPT,
	    "directory's parent record does not name its parent",
	    "forced primary verification checks Joliet parent");
	free(image.bytes);
}

static void
test_verify_forced_primary_rejects_unsupported_rr_skip(void)
{
	susp_area_t area = {0};
	image_t image = rr_image(&area, 0);

	image.bytes[ROOT * BLOCK + ROOT_SP + 6] = 1;
	verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_EUNSUPPORTED,
	    "unsupported SP skip count", "forced primary still checks RR handshake");
	free(image.bytes);
}

static void
test_verify_omissions_belong_to_counted_walk(void)
{
	susp_area_t area = {0};
	image_t image;
	fsimg_verify_report_t report;

	susp_nm(&area, "\001", 1, 0);
	image = rr_image(&area, 0);
	report = verify_expect(&image, FSIMG_ISO_VIEW_ROCKRIDGE, 0, FSIMG_OK,
	    NULL, "RR counted omitted name");
	rr_equal(report.omitted_entries, 1, "RR omissions");
	rr_equal(report.files, 0, "omitted RR file is not counted");
	rr_equal(report.logical_bytes, 0, "omitted RR bytes are not counted");
	report = verify_expect(&image, FSIMG_ISO_VIEW_PRIMARY, 0, FSIMG_OK,
	    NULL, "primary counted before omitted RR walk");
	rr_equal(report.omitted_entries, 0, "uncounted RR omissions excluded");
	rr_equal(report.files, 1, "primary file counted");
	rr_equal(report.logical_bytes, 5, "primary bytes counted");
	free(image.bytes);
}

static void
test_verify_payload_reads_sections_once_per_offered_view(void)
{
	susp_area_t area = {0};
	image_t image = rr_image(&area, 0);
	unsigned char *p = image.bytes + ROOT * BLOCK + RR_CHILD;
	fsimg_verify_report_t report;

	bzero(p, BLOCK - RR_CHILD);
	p += record(p, "A.TXT;1", 7, DATA, BLOCK, 0x80);
	record(p, "A.TXT;1", 7, DATA + 1, 5, 0);
	joliet_add(&image);
	verify_table_root(&image, 17, JOLIET_ROOT, 90, 91);
	p = image.bytes + JOLIET_ROOT * BLOCK + DOT_PAIR_BYTES;
	p += record(p, "\0A\0.\0T\0X\0T\0;\0" "1", 14, DATA, BLOCK, 0x80);
	record(p, "\0A\0.\0T\0X\0T\0;\0" "1", 14, DATA + 1, 5, 0);
	report = verify_expect(&image, FSIMG_ISO_VIEW_ROCKRIDGE,
	    FSIMG_VERIFY_PAYLOAD, FSIMG_OK, NULL, "multi-section payload in three views");
	rr_equal(report.files, 1, "sections count as one file");
	rr_equal(report.logical_bytes, BLOCK + 5, "sections logical size");
	rr_equal(report.payload_bytes_read, 3 * (BLOCK + 5),
	    "payload read once in each offered view");
	/* Headers and all trees remain readable; the final section is absent. */
	image.length = (DATA + 1) * BLOCK;
	verify_expect(&image, FSIMG_ISO_VIEW_ROCKRIDGE, FSIMG_VERIFY_PAYLOAD,
	    FSIMG_ECORRUPT, "extent is empty, misaligned or outside its allocation domain",
	    "section past readable image fails payload W");
	/* Content-index construction rejects the absent section even without
	 * payload verification, before fsimg_verify_payload runs.
	 */
	verify_expect(&image, FSIMG_ISO_VIEW_ROCKRIDGE, 0, FSIMG_ECORRUPT,
	    "extent is empty, misaligned or outside its allocation domain",
	    "content index rejects absent section without payload");

	free(image.bytes);
}

static void
test_verify_second_walk_read_failure_is_incomplete(void)
{
	susp_area_t area = {0}, ce = {0};
	image_t image;
	fixture_t f;
	fsimg_verify_report_t report = {0};
	fsimg_status_t status;
	unsigned char *p;
	size_t length;

	susp_entry(&area, "ST", 4);
	image = rr_image(&area, 0);
	p = image.bytes + ROOT * BLOCK + RR_CHILD;
	p += p[0];
	length = record(p, "B.TXT;1", 7, DATA, 5, 0);
	susp_ce(&ce, RR_AREA + 1, 0, 4);
	bcopy(ce.bytes, p + length, ce.length);
	p[0] += ce.length;
	bcopy(area.bytes, image.bytes + (RR_AREA + 1) * BLOCK, 4);
	fixture_new(&f, &image, NULL);
	CHECK(open_image(&f, FSIMG_ISO_VIEW_PRIMARY) == FSIMG_OK);
	/* Two CE blocks evict one another from the continuation window.
	 * Refuse the first CE only after both selected files are counted,
	 * so the injected read belongs to the second, uncounted RR walk.
	 */
	image.verify_report = &report;
	status = fsimg_verify(f.reader, 0, &report, NULL);
	rr_status_equal(status, FSIMG_EIO, "read failure in second walk");
	CHECK(!report.complete);
	rr_equal(report.files, 2, "selected walk finished before injected read");
	fixture_free(&f);
	free(image.bytes);
}

static void
test_verify_million_data_claims_spill_to_scratch(void)
{
	image_t image = image_new();
	fixture_t f;
	fsimg_options_t options;
	fsimg_verify_report_t report;
	fsimg_error_t error = {0};
	fsimg_stats_t stats;
	fsimg_status_t status;
	struct timespec start, end;
	double seconds;

	image.scale = true;
	image.length = (uint64_t)(VERIFY_SCALE_DATA + VERIFY_SCALE_SECTIONS) * BLOCK;
	put32(image.bytes + 16 * BLOCK + VD_SPACE,
	    VERIFY_SCALE_DATA + VERIFY_SCALE_SECTIONS);
	put32(image.bytes + 16 * BLOCK + VD_ROOT + DR_SIZE,
	    VERIFY_SCALE_BLOCKS * BLOCK);
	fsimg_options_init(&options);
	options.memory_bytes = 16 * 1024 * 1024;
	options.max_entries = VERIFY_SCALE_FILES + 1;
	options.max_work = UINT64_C(10000000000);
	fixture_new(&f, &image, &options);
	CHECK(fsimg_test_meta_cache(f.ctx, 2, NULL) == FSIMG_OK);
	CHECK(open_image(&f, FSIMG_ISO_VIEW_PRIMARY) == FSIMG_OK);
	CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
	status = fsimg_verify(f.reader, 0, &report, &error);
	CHECK(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
	CHECK(fsimg_ctx_get_stats(f.ctx, &stats, NULL) == FSIMG_OK);
	seconds = end.tv_sec - start.tv_sec +
	    (end.tv_nsec - start.tv_nsec) / 1000000000.0;
	printf("ISO verifier million claims: seconds=%.3f scratch_peak=%" PRIu64
	    " memory_peak=%" PRIu64 " status=%d message=%s\n", seconds,
	    stats.scratch_peak, stats.memory_peak, status,
	    error.message);
	rr_status_equal(status, FSIMG_OK, "million data claims");
	CHECK(report.complete);
	rr_equal(report.files, VERIFY_SCALE_FILES, "multi-section counted files");
	rr_equal(report.allocated_clusters, 20 + VERIFY_SCALE_BLOCKS +
	    VERIFY_SCALE_SECTIONS, "million claims share one ascending section range");
	rr_equal(report.logical_bytes, (uint64_t)VERIFY_SCALE_CLAIMS * BLOCK,
	    "all million sections counted");
	CHECK(stats.scratch_peak >= UINT64_C(1000000) * 18);
	CHECK(stats.memory_peak < UINT64_C(1000000) * 18);
	CHECK(stats.scratch_peak < UINT64_C(80) * 1024 * 1024);
	fixture_free(&f);
	free(image.bytes);
}

static void
test_verifier(void)
{
	test_verify_counts_distinct_claimed_blocks();
	test_verify_file_metadata_overlap_is_corrupt();
	test_verify_data_sharing_and_empty_sections();
	test_verify_susp_without_rockridge_claims_root_continuation();
	test_verify_susp_without_rockridge_parent_link_is_opaque();
	test_verify_susp_skip_without_rockridge_is_unsupported_but_lists();
	test_verify_continuations_share_blocks_but_not_bytes();
	test_verify_continuation_inside_directory_is_corrupt();
	test_verify_directory_and_path_table_overlap_is_corrupt();
	test_verify_boot_catalog_and_image_sharing();
	test_verify_path_tables_reject_malformed_records();
	test_verify_primary_path_order_and_joliet_structural_order();
	test_verify_path_record_can_cross_block_boundary();
	test_verify_excess_path_table_records_are_corrupt();
	test_verify_directory_count_limit();
	test_verify_primary_directory_order_and_versions();
	test_verify_directory_and_file_equal_order_keys();
	test_verify_versions_descend_across_directory_file_ties();
	test_verify_unsorted_joliet_directory_is_accepted();
	test_verify_relocation_matches_cl_and_pl_and_hides_re();
	test_verify_hidden_subtree_susp_framing();
	test_verify_allocation_failures_are_incomplete();
	test_verify_cancellation_at_every_checkpoint_is_incomplete();
	test_verify_checks_unselected_joliet_parent_records();
	test_verify_forced_primary_rejects_unsupported_rr_skip();
	test_verify_omissions_belong_to_counted_walk();
	test_verify_payload_reads_sections_once_per_offered_view();
	test_verify_second_walk_read_failure_is_incomplete();
	test_verify_million_data_claims_spill_to_scratch();
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
		CHECK(fsimg_iso9660_format.probe(prefix, count, st.st_size, false,
		    &variant, NULL) == FSIMG_EFORMAT);
		return (0);
	}
	if (argc == 3 && (strcmp(argv[1], "--seed") == 0 ||
	    strcmp(argv[1], "--seed-boot") == 0 ||
	    strcmp(argv[1], "--seed-shared") == 0)) {
		image_t image = strcmp(argv[1], "--seed-shared") == 0 ?
		    shared_extent_image() : image_new();
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
	if (argc == 2 && strcmp(argv[1], "--verify-scale") == 0) {
		test_verify_million_data_claims_spill_to_scratch();
		return (failures == 0 ? 0 : 1);
	}
	if (argc == 2 && strcmp(argv[1], "--output-limit") == 0) {
		test_extraction_budget_uses_selected_io_extent();
		return (0);
	}
	CHECK(argc == 1);
	test_extraction_budget_uses_selected_io_extent();
	test_verifier();
	test_joliet_views();
	test_joliet_identifiers();
	test_joliet_namespace();
	test_joliet_tree();
	test_joliet_label();
	test_joliet_content();
	test_joliet_selection();
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
	test_descriptor_text_printability_and_view_selection();
	test_descriptor_joliet_unicode_and_hostile_units();
	test_descriptor_dates_offset_fraction_and_utc_range();
	test_descriptor_malformed_dates_read_absent_in_either_descriptor();
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
