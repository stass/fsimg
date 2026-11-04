/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/stat.h>

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "fsimg.h"
#include "host.h"
#include "test.h"

/*
 * The stack grammar and the GPT and MBR read side. Disks are built here by
 * an encoder independent of the library's decoder: it writes the UEFI and
 * MBR layouts from their published field tables with its own CRC-32 and
 * GUID byte order, and a hand-laid FAT12 volume stands in for a partition's
 * filesystem. Every passing case has a seeded failure beside it that must
 * fail with the named check, never with a bare-volume read.
 */

#define KIB		UINT64_C(1024)
#define MIB		(UINT64_C(1024) * 1024)
#define FAT_BYTES	MIB
#define GPT_ENTRIES	128u

#define ESP_GUID	"C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
#define LINUX_GUID	"0FC63DAF-8483-4772-8E79-3D69D8477DE4"
#define DISK_GUID	"11111111-2222-4333-8444-555555555555"

/* A disk image in memory. A sparse disk reads as zeros beyond its stored
 * bytes, so a 2 TiB MBR disk needs only its first sector.
 */
typedef struct memory_disk {
	unsigned char	*bytes;
	uint64_t	 stored;
	uint64_t	 length;
} memory_disk_t;

/* One GPT entry as the encoder writes it. */
typedef struct gpt_partition {
	const char	*type_guid;
	const uint16_t	*name;
	uint64_t	 first_lba;
	uint64_t	 last_lba;
} gpt_partition_t;

/* A test's view of the library: one context with scratch, and one image
 * over a memory disk.
 */
typedef struct fixture {
	fsimg_ctx_t	*ctx;
	fsimg_io_t	*io;
	fsimg_image_t	*image;
	uint64_t	 baseline_memory;	/* Charged by the context itself */
	char		 directory[1024];
	int		 scratch;
} fixture_t;

/* GPT names as UTF-16 units; name_data spells "Donnees" with an acute
 * e, a unit outside ASCII.
 */
static const uint16_t name_a[] = { 'A', 0 };
static const uint16_t name_b[] = { 'B', 0 };
static const uint16_t name_efi[] = { 'E', 'F', 'I', 0 };
static const uint16_t name_root[] = { 'r', 'o', 'o', 't', 0 };
static const uint16_t name_data[] = { 'D', 'o', 'n', 'n', 0xe9, 'e', 's', 0 };
/* An unpaired high surrogate, then ESC: untrusted units a report must
 * carry as U+FFFD and leave to the CLI's escaping.
 */
static const uint16_t name_hostile[] = { 0xd800, 'X', 0x1b, 0 };

static uint32_t crc32_oracle(const unsigned char *, size_t);
static void put16(unsigned char *, uint16_t);
static void put32(unsigned char *, uint32_t);
static void put64(unsigned char *, uint64_t);
static void guid_text(const char *, unsigned char *);
static void guid_put(unsigned char *, const char *);
static memory_disk_t disk_new(uint64_t);
static void make_fat12(unsigned char *);
static void gpt_build(memory_disk_t *, uint32_t, const gpt_partition_t *,
    unsigned);
static void gpt_reseal(memory_disk_t *, uint32_t);
static void mbr_entry(unsigned char *, unsigned, uint8_t, uint8_t, uint32_t,
    uint32_t);
static fsimg_status_t disk_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static fsimg_status_t disk_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t disk_write(void *, uint64_t, const void *, size_t,
    size_t *, fsimg_error_t *);
static void fixture_init(fixture_t *);
static void fixture_free(fixture_t *);
static fsimg_status_t fixture_open(fixture_t *, memory_disk_t *, const char *,
    uint32_t, fsimg_error_t *);
static fsimg_status_t select_with(fixture_t *, fsimg_selector_t, fsimg_io_t **,
    fsimg_error_t *);
static void expect_volume_lists(fixture_t *, fsimg_io_t *, const char *);
static memory_disk_t two_partition_gpt(uint32_t);
static void expect_gpt_corrupt(memory_disk_t *, const char *);
static void test_crc_oracle(void);
static void test_stack_grammar(void);
static void test_stack_rejections(void);
static void test_registry(void);
static void test_gpt_read(void);
static void test_gpt_selectors(void);
static void test_gpt_names(void);
static void test_gpt_seeded_failures(void);
static void test_mbr_truncated_partition_keeps_partial_sector(void);
static void test_mbr_missing_partition_is_listed_but_not_selected(void);
static void test_gpt_truncated_partitions_and_backup_report_missing_bytes(void);
static void test_gpt_failed_primary_stays_corrupt_on_cut_disk(void);
static void test_gpt_primary_array_cuts_are_corrupt(void);
static void test_gpt_declared_geometry_cannot_wrap(void);
static fsimg_status_t unwrap_write(void *, uint64_t, const void *, size_t,
    size_t *, fsimg_error_t *);
static void test_unwrap_requires_a_whole_partition(void);
static void test_extended_mbr_past_end_is_listed_but_unsupported(void);
static void test_gpt_primary_array_end_and_trailing_partial_sector(void);
static void expect_scheme_truncated(fixture_t *, const char *, uint64_t);
static void test_gpt_sector_sizes(void);
static void test_mbr_read(void);
static void test_mbr_seeded_failures(void);
static void test_mbr_zero_lba_is_not_a_table(void);
static void test_iso_system_area_scheme(void);
static void test_iso_corrupt_gpt_is_ambiguous(void);
static void test_corrupt_iso_gpt_is_ambiguous(void);
static void test_fat_gpt_is_ambiguous(void);
static void test_mbr_addressable_limit(void);
static void test_bare_volume(void);
static void test_assumed_identity_selects_unvouched_fat(void);
static void test_image_assume_identity_requires_named_slot_or_compression(void);
static void test_assumed_mbr_signature_selects_partition_bytes(void);
static void test_assumed_mbr_signature_is_reported_before_verification(void);
static void test_assumed_mbr_empty_table_is_reported_before_verification(void);
static void test_signed_mbr_with_assume_identity_verifies(void);
static void test_assumed_mbr_still_validates_entries(void);
static void test_assume_identity_does_not_change_mbr_detection(void);
static void test_forcing(void);
static void write_seed(const char *, const memory_disk_t *);

static uint32_t
crc32_oracle(const unsigned char *bytes, size_t length)
{
	uint32_t crc = 0xffffffffu;

	for (size_t i = 0; i < length; i++) {
		crc ^= bytes[i];
		for (int bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return (crc ^ 0xffffffffu);
}

static void
put16(unsigned char *p, uint16_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
}

static void
put32(unsigned char *p, uint32_t v)
{
	for (unsigned i = 0; i < 4; i++)
		p[i] = (unsigned char)(v >> (8 * i));
}

static void
put64(unsigned char *p, uint64_t v)
{
	for (unsigned i = 0; i < 8; i++)
		p[i] = (unsigned char)(v >> (8 * i));
}

/* A GUID's sixteen bytes in text order. */
static void
guid_text(const char *text, unsigned char *bytes)
{
	unsigned value;

	for (unsigned i = 0, position = 0; i < 16; i++) {
		if (text[position] == '-')
			position++;
		CHECK(sscanf(text + position, "%2x", &value) == 1);
		bytes[i] = (unsigned char)value;
		position += 2;
	}
}

/* GPT's mixed byte order: the 32-, 16- and 16-bit leading groups
 * little-endian, the last eight bytes as written.
 */
static void
guid_put(unsigned char *raw, const char *text)
{
	unsigned char bytes[16];

	guid_text(text, bytes);
	for (unsigned i = 0; i < 4; i++)
		raw[i] = bytes[3 - i];
	raw[4] = bytes[5];
	raw[5] = bytes[4];
	raw[6] = bytes[7];
	raw[7] = bytes[6];
	bcopy(bytes + 8, raw + 8, 8);
}

static memory_disk_t
disk_new(uint64_t length)
{
	memory_disk_t disk = { .stored = length, .length = length };

	disk.bytes = calloc(1, (size_t)length);
	CHECK(disk.bytes != NULL);
	return (disk);
}

/* A 1 MiB FAT12 volume laid out by hand: 2048 sectors of 512 bytes, one
 * sector per cluster, one reserved sector, two 6-sector FATs and a
 * 224-entry root directory holding one subdirectory, DIR, at cluster 2.
 */
static void
make_fat12(unsigned char *volume)
{
	static const char dot[] = ".          ";
	static const char dotdot[] = "..         ";
	unsigned char *boot = volume, *root, *directory;

	bzero(volume, FAT_BYTES);
	boot[0] = 0xeb;
	boot[1] = 0x3c;
	boot[2] = 0x90;
	bcopy("MSWIN4.1", boot + 3, 8);
	put16(boot + 11, 512);
	boot[13] = 1;
	put16(boot + 14, 1);
	boot[16] = 2;
	put16(boot + 17, 224);
	put16(boot + 19, 2048);
	boot[21] = 0xf8;
	put16(boot + 22, 6);
	put16(boot + 24, 32);
	put16(boot + 26, 2);
	boot[38] = 0x29;
	bcopy("NO NAME    FAT12   ", boot + 43, 19);
	boot[510] = 0x55;
	boot[511] = 0xaa;
	for (unsigned table = 0; table < 2; table++) {
		/* Media entry, the reserved entry, and cluster 2 end of chain. */
		static const unsigned char head[] = { 0xf8, 0xff, 0xff, 0xff, 0x0f };

		bcopy(head, volume + (1 + table * 6) * 512, sizeof(head));
	}
	root = volume + 13 * 512;
	bcopy("DIR        ", root, 11);
	root[11] = 0x10;
	put16(root + 26, 2);
	directory = volume + 27 * 512;
	bcopy(dot, directory, 11);
	directory[11] = 0x10;
	put16(directory + 26, 2);
	bcopy(dotdot, directory + 32, 11);
	directory[32 + 11] = 0x10;
}

/*
 * A GPT of 128 entries in the layout sgdisk writes: protective MBR, header
 * at LBA 1, primary array from LBA 2, usable range after it, backup array
 * before the backup header at the last LBA.
 */
static void
gpt_build(memory_disk_t *disk, uint32_t sector, const gpt_partition_t *parts,
    unsigned count)
{
	uint64_t last_lba, array_lbas;
	unsigned char *mbr, *entry;

	last_lba = disk->length / sector - 1;
	array_lbas = GPT_ENTRIES * 128 / sector;
	mbr = disk->bytes;
	mbr_entry(mbr, 0, 0x00, 0xee, 1,
	    last_lba > 0xffffffffu ? 0xffffffffu : (uint32_t)last_lba);
	for (unsigned copy = 0; copy < 2; copy++) {
		uint64_t header_lba = copy == 0 ? 1 : last_lba;
		uint64_t array_lba = copy == 0 ? 2 : last_lba - array_lbas;
		unsigned char *header = disk->bytes + header_lba * sector;

		bcopy("EFI PART", header, 8);
		put32(header + 8, 0x00010000);
		put32(header + 12, 92);
		put64(header + 24, header_lba);
		put64(header + 32, copy == 0 ? last_lba : 1);
		put64(header + 40, 2 + array_lbas);
		put64(header + 48, last_lba - array_lbas - 1);
		guid_put(header + 56, DISK_GUID);
		put64(header + 72, array_lba);
		put32(header + 80, GPT_ENTRIES);
		put32(header + 84, 128);
		for (unsigned i = 0; i < count; i++) {
			entry = disk->bytes + array_lba * sector + i * 128;
			guid_put(entry, parts[i].type_guid);
			guid_put(entry + 16, "AAAAAAAA-0000-4000-8000-00000000000A");
			entry[31] = (unsigned char)i;
			put64(entry + 32, parts[i].first_lba);
			put64(entry + 40, parts[i].last_lba);
			for (unsigned unit = 0; parts[i].name[unit] != 0; unit++)
				put16(entry + 56 + unit * 2, parts[i].name[unit]);
		}
	}
	gpt_reseal(disk, sector);
}

/* Recompute both arrays' and both headers' CRCs after an edit. */
static void
gpt_reseal(memory_disk_t *disk, uint32_t sector)
{
	uint64_t last_lba = disk->length / sector - 1;

	for (unsigned copy = 0; copy < 2; copy++) {
		unsigned char *header = disk->bytes + (copy == 0 ? 1 : last_lba) *
		    sector;
		uint64_t array_lba = header[72] | (uint64_t)header[73] << 8 |
		    (uint64_t)header[74] << 16;

		put32(header + 88, crc32_oracle(disk->bytes + array_lba * sector,
		    GPT_ENTRIES * 128));
		put32(header + 16, 0);
		put32(header + 16, crc32_oracle(header, 92));
	}
}

static void
mbr_entry(unsigned char *sector, unsigned slot, uint8_t status, uint8_t type,
    uint32_t first_lba, uint32_t sectors)
{
	unsigned char *entry = sector + 446 + slot * 16;

	entry[0] = status;
	entry[4] = type;
	put32(entry + 8, first_lba);
	put32(entry + 12, sectors);
	sector[510] = 0x55;
	sector[511] = 0xaa;
}

static fsimg_status_t
disk_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_disk_t *disk = argument;
	size_t stored;

	CHECK(offset <= disk->length && length <= disk->length - offset);
	bzero(buffer, length);
	if (offset < disk->stored) {
		stored = disk->stored - offset < length ?
		    (size_t)(disk->stored - offset) : length;
		bcopy(disk->bytes + offset, buffer, stored);
	}
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
disk_size(void *argument, uint64_t *size, fsimg_error_t *error FSIMG_UNUSED)
{
	*size = ((memory_disk_t *)argument)->length;
	return (FSIMG_OK);
}

/* The disk accepts writes, so only a read-only slice keeps them out. */
static fsimg_status_t
disk_write(void *argument FSIMG_UNUSED, uint64_t offset FSIMG_UNUSED,
    const void *buffer FSIMG_UNUSED, size_t length FSIMG_UNUSED,
    size_t *transferred FSIMG_UNUSED, fsimg_error_t *error FSIMG_UNUSED)
{
	CHECK(!"a selected slice wrote to the disk");
	return (FSIMG_EIO);
}

static void
fixture_init(fixture_t *f)
{
	fsimg_options_t options;
	fsimg_stats_t stats;

	bzero(f, sizeof(*f));
	snprintf(f->directory, sizeof(f->directory), "%s/fsimg-layers.XXXXXX",
	    getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp");
	CHECK(mkdtemp(f->directory) != NULL);
	f->scratch = open(f->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(f->scratch != -1);
	fsimg_options_init(&options);
	options.memory_bytes = 16 * MIB;
	options.scratch_directory_fd = f->scratch;
	CHECK(fsimg_ctx_new(&options, &f->ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	f->baseline_memory = stats.memory_bytes;
}

static void
fixture_free(fixture_t *f)
{
	fsimg_stats_t stats;

	fsimg_image_free(f->image);
	fsimg_io_free(f->io);
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == f->baseline_memory);
	fsimg_ctx_free(f->ctx);
	CHECK(close(f->scratch) == 0);
	CHECK(rmdir(f->directory) == 0);
}

/* Open an image of disk forcing the stack text, when given, and the GPT
 * sector size, when nonzero.
 */
static fsimg_status_t
fixture_open(fixture_t *f, memory_disk_t *disk, const char *stack,
    uint32_t gpt_sector, fsimg_error_t *error)
{
	fsimg_image_options_t options;
	fsimg_io_ops_t ops;

	fsimg_image_free(f->image);
	fsimg_io_free(f->io);
	f->image = NULL;
	fsimg_io_ops_init(&ops);
	ops.read_at = disk_read;
	ops.get_size = disk_size;
	ops.write_at = disk_write;
	CHECK(fsimg_io_from_callbacks(f->ctx, &ops, disk, &f->io, NULL) == FSIMG_OK);
	fsimg_image_options_init(&options);
	if (stack != NULL)
		CHECK(fsimg_stack_parse(stack, &options.stack, NULL) == FSIMG_OK);
	options.layers.gpt_sector_bytes = gpt_sector;
	return (fsimg_image_open(f->ctx, f->io, &options, &f->image, error));
}

static fsimg_status_t
select_with(fixture_t *f, fsimg_selector_t selector, fsimg_io_t **volume,
    fsimg_error_t *error)
{
	return (fsimg_image_select(f->image, &selector, volume, error));
}

/* The selected slice opens as an ordinary volume whose first entry is
 * the expected path; the slice is released before the image.
 */
static void
expect_volume_lists(fixture_t *f, fsimg_io_t *volume, const char *path)
{
	fsimg_reader_t *reader;
	const fsimg_entry_t *entry;

	CHECK(fsimg_reader_open(f->ctx, volume, NULL, &reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(reader, &entry, NULL) == FSIMG_OK);
	CHECK(strcmp(entry->path, path) == 0);
	fsimg_reader_free(reader);
	fsimg_io_free(volume);
}

/* A 4 MiB disk: partition 1 an EFI system partition named "EFI" holding
 * the FAT12 volume at 1 MiB, partition 2 a Linux partition named
 * "Donnees" with an acute e, all zeros, at 2 MiB.
 */
static memory_disk_t
two_partition_gpt(uint32_t sector)
{
	gpt_partition_t parts[] = {
		{ ESP_GUID, name_efi, MIB / sector, 2 * MIB / sector - 1 },
		{ LINUX_GUID, name_data, 2 * MIB / sector, 3 * MIB / sector - 1 }
	};
	memory_disk_t disk = disk_new(4 * MIB);

	make_fat12(disk.bytes + MIB);
	gpt_build(&disk, sector, parts, 2);
	return (disk);
}

/* The oracle's CRC is the standard one: its check value proves it. */
static void
test_crc_oracle(void)
{
	CHECK(crc32_oracle((const unsigned char *)"123456789", 9) == 0xcbf43926u);
}

/* Every legal combination of present slots parses to the slots its names
 * fill, and the canonical text re-parses to the same named slots.
 */
static void
test_stack_grammar(void)
{
	static const struct {
		const char	*text;
		const char	*canonical;
		fsimg_slot_state_t scheme;
		fsimg_slot_state_t container;
	} cases[] = {
		{ "fat32", "fat32", FSIMG_SLOT_UNSPECIFIED, FSIMG_SLOT_UNSPECIFIED },
		{ "fat32/gpt", "fat32/gpt", FSIMG_SLOT_NAMED, FSIMG_SLOT_UNSPECIFIED },
		{ "fat32/gpt/dmg", "fat32/gpt/dmg", FSIMG_SLOT_NAMED, FSIMG_SLOT_NAMED },
		{ "fat32/dmg", "fat32/dmg", FSIMG_SLOT_UNSPECIFIED, FSIMG_SLOT_NAMED },
		{ "fat32/-/dmg", "fat32/dmg", FSIMG_SLOT_EMPTY, FSIMG_SLOT_NAMED },
		{ "fat32/-", "fat32", FSIMG_SLOT_EMPTY, FSIMG_SLOT_UNSPECIFIED },
		{ "fat32/-/-", "fat32", FSIMG_SLOT_EMPTY, FSIMG_SLOT_EMPTY },
		{ "fat32/mbr/-", "fat32/mbr", FSIMG_SLOT_NAMED, FSIMG_SLOT_EMPTY },
		{ "exfat/mbr/vhd", "exfat/mbr/vhd", FSIMG_SLOT_NAMED, FSIMG_SLOT_NAMED },
		{ "qnx6/gpt/simg", "qnx6/gpt/simg", FSIMG_SLOT_NAMED, FSIMG_SLOT_NAMED },
		{ "hfs", "hfs", FSIMG_SLOT_UNSPECIFIED, FSIMG_SLOT_UNSPECIFIED },
		{ "gpt", "gpt", FSIMG_SLOT_NAMED, FSIMG_SLOT_UNSPECIFIED },
		{ "gpt/dmg", "gpt/dmg", FSIMG_SLOT_NAMED, FSIMG_SLOT_NAMED },
		{ "dmg", "dmg", FSIMG_SLOT_UNSPECIFIED, FSIMG_SLOT_NAMED }
	};
	fsimg_stack_t stack, again;
	char text[64];

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		CHECK(fsimg_stack_parse(cases[i].text, &stack, NULL) == FSIMG_OK);
		CHECK(stack.slots[FSIMG_LAYER_SCHEME].state == cases[i].scheme);
		CHECK(stack.slots[FSIMG_LAYER_CONTAINER].state == cases[i].container);
		CHECK(fsimg_stack_format(&stack, text, sizeof(text)) ==
		    strlen(cases[i].canonical));
		CHECK(strcmp(text, cases[i].canonical) == 0);
		CHECK(fsimg_stack_parse(text, &again, NULL) == FSIMG_OK);
		for (unsigned slot = 0; slot < FSIMG_STACK_SLOTS; slot++)
			CHECK(strcmp(stack.slots[slot].name, again.slots[slot].name) == 0);
	}
	/* The formatter reports the length it needs and truncates safely. */
	CHECK(fsimg_stack_parse("fat32/gpt/dmg", &stack, NULL) == FSIMG_OK);
	CHECK(fsimg_stack_format(&stack, text, 6) == 13);
	CHECK(strcmp(text, "fat32") == 0);
}

/* Each malformed stack is a syntax error naming the offending token. */
static void
test_stack_rejections(void)
{
	static const struct {
		const char	*text;
		const char	*message;
		const char	*named;
	} cases[] = {
		{ "fat32/fat32", "stack name repeats or misorders a slot", "fat32" },
		{ "fat32/-/gpt", "stack name repeats or misorders a slot", "gpt" },
		{ "gpt/fat32", "stack name repeats or misorders a slot", "fat32" },
		{ "fat32/mbr/gpt", "stack name repeats or misorders a slot", "gpt" },
		{ "-/gpt", "- cannot fill the filesystem slot", "-" },
		{ "fat33", "unknown stack name", "fat33" },
		{ "fat32/gzip", "unknown stack name", "gzip" },
		/* The token is rendered like every path an error carries. */
		{ "fat\\32/gpt", "unknown stack name", "fat\\\\32" },
		{ "fat32//dmg", "the stack has an empty token", "fat32//dmg" },
		{ "fat32/", "the stack has an empty token", "fat32/" },
		{ "fat32/-/-/-", "the stack has too many tokens", "fat32/-/-/-" },
		{ "", "the stack is empty", "" }
	};
	fsimg_stack_t stack;
	fsimg_error_t error;
	char long_text[sizeof(error.path) + 16];

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		CHECK(fsimg_stack_parse(cases[i].text, &stack, &error) == FSIMG_EINVAL);
		CHECK(strcmp(error.message, cases[i].message) == 0);
		CHECK(strcmp(error.path, cases[i].named) == 0);
	}

	/* A token too long for the field keeps its tail, as every rendered
	 * path does, and is marked truncated.
	 */
	memset(long_text, 'x', sizeof(long_text) - 1);
	long_text[0] = 'b';
	long_text[sizeof(long_text) - 2] = 'e';
	long_text[sizeof(long_text) - 1] = '\0';
	CHECK(fsimg_stack_parse(long_text, &stack, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.message, "unknown stack name") == 0);
	CHECK(error.path_truncated);
	CHECK(strlen(error.path) == sizeof(error.path) - 1);
	CHECK(error.path[0] == 'x' && error.path[sizeof(error.path) - 2] == 'e');
}

/* The registry lists every layer once, with the suffixes the command
 * strips, the capabilities of the schemes and containers, which are read,
 * written and verified but for the read-only hbcifs, the filesystem a
 * loader's package carries, and the keys of each layer's settings, its
 * file settings first, then its typed settings in table order. The
 * compressions are listed with their suffixes but no codec.
 */
static void
test_registry(void)
{
	static const unsigned scheme = FSIMG_CAP_READ | FSIMG_CAP_WRITE |
	    FSIMG_CAP_VERIFY;
	static const unsigned container = scheme;
	static const unsigned read_only = FSIMG_CAP_READ | FSIMG_CAP_VERIFY;
	static const struct {
		const char	*name;
		const char	*suffix;
		const char	*settings;
		const char	*codecs;
		fsimg_layer_kind_t kind;
		unsigned	 capabilities;
		const char	*carries;
	} expected[] = {
		{ "gpt", "", "sector,disk,type,name,disk-guid,guid", "",
		    FSIMG_LAYER_SCHEME, scheme, "" },
		{ "mbr", "", "disk,type,active,id", "", FSIMG_LAYER_SCHEME,
		    scheme, "" },
		{ "dmg", ".dmg", "compress,level,chunk,uuid", "raw,zero,zlib",
		    FSIMG_LAYER_CONTAINER, container, "" },
		{ "vhd", ".vhd", "align,uuid", "raw", FSIMG_LAYER_CONTAINER,
		    container, "" },
		{ "simg", ".simg", "block,crc", "raw,zero,fill",
		    FSIMG_LAYER_CONTAINER, container, "" },
		{ "ifsboot", "", "startup,prefix,machine,virtual,startup_vaddr,"
		    "paddr_bias,image_paddr,ram_paddr,imagefs_paddr,compress",
		    "raw,lzo1x,nrv2b", FSIMG_LAYER_CONTAINER, container, "ifs" },
		{ "hbcifs", "", "", "lzo1x", FSIMG_LAYER_CONTAINER,
		    read_only, "ifs" },
		{ "gzip", ".gz", "level", "", FSIMG_LAYER_COMPRESSION, container,
		    "" },
		{ "zstd", ".zst", "level", "", FSIMG_LAYER_COMPRESSION,
#ifdef FSIMG_ZSTD
		    container, "" },
#else
		    0, "" },
#endif
		{ "xz", ".xz", "level,check", "", FSIMG_LAYER_COMPRESSION,
#ifdef FSIMG_XZ
		    container, "" }
#else
		    0, "" }
#endif
	};
	fsimg_layer_info_t info;
	size_t cursor = 0;

	for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
		CHECK(fsimg_layer_next(&cursor, &info, NULL) == FSIMG_OK);
		CHECK(strcmp(info.name, expected[i].name) == 0);
		CHECK(strcmp(info.suffix, expected[i].suffix) == 0);
		CHECK(strcmp(info.settings, expected[i].settings) == 0);
		CHECK(info.kind == expected[i].kind);
		CHECK(info.capabilities == expected[i].capabilities);
		CHECK(strcmp(info.codecs, expected[i].codecs) == 0);
		CHECK(strcmp(info.carries, expected[i].carries) == 0);
		/* Only a layer that inflates names zlib's version: a
		 * container whose chunks use zlib, and gzip.
		 */
		CHECK((strstr(info.codecs, "zlib") != NULL ||
		    strcmp(info.name, "gzip") == 0) ==
		    (strncmp(info.codec_version, "zlib ", 5) == 0));
	}
	CHECK(fsimg_layer_next(&cursor, &info, NULL) == FSIMG_DONE);
}

/* Detection finds the GPT, lists its slots in order with their types,
 * names and claims, and the automatic selection opens the one partition
 * that holds a filesystem.
 */
static void
test_gpt_read(void)
{
	memory_disk_t disk = two_partition_gpt(512);
	fsimg_partition_info_t partition;
	fsimg_layer_report_t report;
	fsimg_image_info_t info;
	fsimg_io_t *volume;
	fixture_t f;
	unsigned char guid[16];

	fixture_init(&f);
	CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.scheme, "gpt") == 0 && strcmp(info.stack, "gpt") == 0);
	CHECK(info.sector_bytes == 512 && info.partition_count == GPT_ENTRIES);
	CHECK(info.disk_bytes == 4 * MIB && info.selected == 0);
	CHECK(info.disk_guid[0] == 0x11 && info.disk_guid[15] == 0x55);

	CHECK(fsimg_image_next_partition(f.image, &partition, NULL) == FSIMG_OK);
	CHECK(partition.index == 1 && partition.used);
	CHECK(partition.offset == MIB && partition.length == MIB);
	CHECK(strcmp(partition.type, "esp") == 0 && strcmp(partition.name, "EFI") == 0);
	guid_text(ESP_GUID, guid);
	CHECK(bcmp(partition.type_guid, guid, sizeof(guid)) == 0);
	CHECK(partition.claim == FSIMG_CLAIM_FORMAT &&
	    strcmp(partition.format, "fat12") == 0);
	CHECK(fsimg_image_next_partition(f.image, &partition, NULL) == FSIMG_OK);
	CHECK(partition.index == 2 && strcmp(partition.type, "linux") == 0);
	CHECK(strcmp(partition.name, "Donn\xc3\xa9" "es") == 0);
	CHECK(partition.claim == FSIMG_CLAIM_NONE);
	for (unsigned slot = 3; slot <= GPT_ENTRIES; slot++) {
		CHECK(fsimg_image_next_partition(f.image, &partition, NULL) == FSIMG_OK);
		CHECK(partition.index == slot && !partition.used);
	}
	CHECK(fsimg_image_next_partition(f.image, &partition, NULL) == FSIMG_DONE);

	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_AUTOMATIC },
	    &volume, NULL) == FSIMG_OK);
	expect_volume_lists(&f, volume, "/DIR");
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.stack, "fat12/gpt") == 0 && info.selected == 1);

	CHECK(fsimg_image_verify(f.image, 0, &report, NULL) == FSIMG_OK);
	CHECK(strcmp(report.scheme, "gpt") == 0 && report.partitions == 2);
	CHECK(report.complete && report.scope == FSIMG_VALIDATION_STRUCTURAL);
	fixture_free(&f);
	free(disk.bytes);
}

static void
test_gpt_selectors(void)
{
	static const char *const long_name =
	    "abcdefghijklmnopqrstuvwxyzabcdefghijk";
	memory_disk_t disk = two_partition_gpt(512);
	gpt_partition_t fat_twice[] = {
		{ ESP_GUID, name_a, MIB / 512, 2 * MIB / 512 - 1 },
		{ ESP_GUID, name_b, 2 * MIB / 512, 3 * MIB / 512 - 1 }
	};
	memory_disk_t two_fat = disk_new(4 * MIB);
	fsimg_image_info_t info;
	fsimg_error_t error;
	fsimg_io_t *volume;
	uint64_t length;
	fixture_t f;

	fixture_init(&f);
	CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_INDEX,
	    .index = 2 }, &volume, NULL) == FSIMG_OK);
	CHECK(fsimg_io_get_size(volume, &length, NULL) == FSIMG_OK && length == MIB);
	fsimg_io_free(volume);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(info.selected == 2 && strcmp(info.stack, "gpt") == 0);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_INDEX,
	    .index = 3 }, &volume, &error) == FSIMG_EINVAL && volume == NULL);
	CHECK(strstr(error.message, "unused") != NULL);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_INDEX,
	    .index = GPT_ENTRIES + 1 }, &volume, NULL) == FSIMG_EINVAL);

	/* Names compare as UTF-16 units, exactly. */
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_NAME,
	    .text = "EFI" }, &volume, NULL) == FSIMG_OK);
	expect_volume_lists(&f, volume, "/DIR");
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_NAME,
	    .text = "efi" }, &volume, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.message, "no partition has that name") == 0);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_NAME,
	    .text = "Donn\xc3\xa9" "es" }, &volume, NULL) == FSIMG_OK);
	fsimg_io_free(volume);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_NAME,
	    .text = "Donnees" }, &volume, NULL) == FSIMG_EINVAL);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_NAME,
	    .text = long_name }, &volume, NULL) == FSIMG_EINVAL);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_NAME,
	    .text = "\xff" }, &volume, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.message, "invalid UTF-8 encoding") == 0);

	/* Types: a table name, a literal GUID; never a byte on GPT. */
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_TYPE,
	    .text = "esp" }, &volume, NULL) == FSIMG_OK);
	expect_volume_lists(&f, volume, "/DIR");
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_TYPE,
	    .text = "0fc63daf-8483-4772-8e79-3d69d8477de4" }, &volume, NULL) ==
	    FSIMG_OK);
	fsimg_io_free(volume);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK &&
	    info.selected == 2);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_TYPE,
	    .text = "0x83" }, &volume, NULL) == FSIMG_EINVAL);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_TYPE,
	    .text = "qnx4" }, &volume, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.message, "the partition type has no GPT GUID") == 0);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_TYPE,
	    .text = "basic" }, &volume, NULL) == FSIMG_EINVAL);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_TYPE,
	    .text = "bogus" }, &volume, NULL) == FSIMG_EINVAL);

	/* Ranges address the disk and must lie within it. */
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_RANGE,
	    .offset = MIB, .length = MIB }, &volume, NULL) == FSIMG_OK);
	expect_volume_lists(&f, volume, "/DIR");
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_RANGE,
	    .offset = 3 * MIB }, &volume, NULL) == FSIMG_OK);
	CHECK(fsimg_io_get_size(volume, &length, NULL) == FSIMG_OK && length == MIB);
	fsimg_io_free(volume);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_RANGE,
	    .offset = 4 * MIB + 1 }, &volume, NULL) == FSIMG_EINVAL);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_RANGE,
	    .offset = 3 * MIB, .length = 2 * MIB }, &volume, NULL) == FSIMG_EINVAL);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_DISK },
	    &volume, NULL) == FSIMG_OK);
	CHECK(fsimg_io_get_size(volume, &length, NULL) == FSIMG_OK &&
	    length == 4 * MIB);
	fsimg_io_free(volume);

	/* A range needs no table, so a damaged one does not block it. */
	disk.bytes[512 + 40] ^= 1;
	CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_RANGE,
	    .offset = MIB, .length = MIB }, &volume, NULL) == FSIMG_OK);
	expect_volume_lists(&f, volume, "/DIR");

	/* Two filesystems leave the automatic choice to the caller. */
	make_fat12(two_fat.bytes + MIB);
	make_fat12(two_fat.bytes + 2 * MIB);
	gpt_build(&two_fat, 512, fat_twice, 2);
	CHECK(fixture_open(&f, &two_fat, NULL, 0, NULL) == FSIMG_OK);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_AUTOMATIC },
	    &volume, &error) == FSIMG_EFORMAT);
	CHECK(strcmp(error.message, "several partitions hold a filesystem: 1, 2") == 0);
	fixture_free(&f);
	free(disk.bytes);
	free(two_fat.bytes);
}

/* Detection and every selection of a damaged table fail with the named
 * check as corruption, whether or not gpt is forced; nothing falls
 * through to a filesystem probe.
 */
static void
expect_gpt_corrupt(memory_disk_t *disk, const char *message)
{
	fsimg_image_info_t info;
	fsimg_error_t error;
	fsimg_io_t *volume;
	fixture_t f;

	fixture_init(&f);
	CHECK(fixture_open(&f, disk, NULL, 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, &error) == FSIMG_ECORRUPT);
	if (strcmp(error.message, message) != 0)
		fprintf(stderr, "expected \"%s\", got \"%s\"\n", message,
		    error.message);
	CHECK(strcmp(error.message, message) == 0);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_AUTOMATIC },
	    &volume, &error) == FSIMG_ECORRUPT && volume == NULL);
	CHECK(fixture_open(&f, disk, "gpt", 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, &error) == FSIMG_ECORRUPT);
	CHECK(strcmp(error.message, message) == 0);
	fixture_free(&f);
}

/* Names report lossily as UTF-8 for display; selection compares the
 * stored units, so a name that has no UTF-8 spelling selects nothing. The
 * selected slice refuses writes.
 */
static void
test_gpt_names(void)
{
	gpt_partition_t parts[] = {
		{ ESP_GUID, name_hostile, MIB / 512, 2 * MIB / 512 - 1 }
	};
	memory_disk_t disk = disk_new(4 * MIB);
	fsimg_partition_info_t partition;
	fsimg_io_t *volume;
	fsimg_error_t error;
	size_t written;
	fixture_t f;

	make_fat12(disk.bytes + MIB);
	gpt_build(&disk, 512, parts, 1);
	fixture_init(&f);
	CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_next_partition(f.image, &partition, NULL) == FSIMG_OK);
	CHECK(strcmp(partition.name, "\xef\xbf\xbdX\x1b") == 0);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_NAME,
	    .text = partition.name }, &volume, NULL) == FSIMG_EINVAL);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_INDEX,
	    .index = 1 }, &volume, NULL) == FSIMG_OK);
	CHECK(fsimg_io_write_at(volume, 0, "x", 1, &written, &error) != FSIMG_OK);
	fsimg_io_free(volume);
	fixture_free(&f);
	free(disk.bytes);
}

/* Table loss is independent of whether the selected filesystem fits. */
static void
expect_scheme_truncated(fixture_t *f, const char *scheme, uint64_t missing)
{
	fsimg_layer_report_t report;
	fsimg_error_t error;
	char count[32];

	snprintf(count, sizeof(count), "%llu", (unsigned long long)missing);
	CHECK(fsimg_image_verify(f->image, 0, &report, &error) == FSIMG_ECORRUPT);
	CHECK(strstr(error.message, "truncated") != NULL);
	CHECK(strstr(error.message, count) != NULL);
	CHECK(strcmp(error.path, scheme) == 0);
	if (strcmp(scheme, "gpt") == 0)
		CHECK(strstr(error.message, "backup") != NULL);
}

static void
test_mbr_truncated_partition_keeps_partial_sector(void)
{
	static const uint64_t cuts[] = { 4 * MIB, 2 * MIB, 2 * MIB + 123 };
	memory_disk_t disk = disk_new(4 * MIB);
	fsimg_partition_info_t partition;
	fsimg_image_info_t info;
	fsimg_io_t *volume;
	fixture_t f;

	make_fat12(disk.bytes + MIB);
	mbr_entry(disk.bytes, 0, 0x80, 1, 2048, 8192);
	fixture_init(&f);
	for (size_t i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++) {
		disk.length = cuts[i];
		fprintf(stderr, "MBR cut at %llu bytes\n",
		    (unsigned long long)disk.length);
		CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
		CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
		CHECK(info.missing_bytes == 5 * MIB - disk.length);
		CHECK(fsimg_image_next_partition(f.image, &partition, NULL) == FSIMG_OK);
		CHECK(partition.offset == MIB);
		CHECK(partition.length == disk.length - MIB);
		CHECK(partition.missing_bytes == 5 * MIB - disk.length);
		expect_scheme_truncated(&f, "mbr", info.missing_bytes);
		CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_AUTOMATIC },
		    &volume, NULL) == FSIMG_OK);
		expect_volume_lists(&f, volume, "/DIR");
		expect_scheme_truncated(&f, "mbr", info.missing_bytes);
	}
	fixture_free(&f);
	free(disk.bytes);
}

static void
test_mbr_missing_partition_is_listed_but_not_selected(void)
{
	memory_disk_t disk = disk_new(4 * MIB);
	fsimg_partition_info_t partition;
	fsimg_image_info_t info;
	fsimg_error_t error;
	fsimg_io_t *volume;
	fixture_t f;

	make_fat12(disk.bytes + MIB);
	mbr_entry(disk.bytes, 0, 0x80, 1, 2048, 2048);
	fixture_init(&f);
	for (unsigned past = 0; past < 2; past++) {
		mbr_entry(disk.bytes, 1, 0, 1, 8192 + past, 1024);
		CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
		CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
		CHECK(info.missing_bytes == (1024 + past) * 512);
		CHECK(fsimg_image_next_partition(f.image, &partition, NULL) == FSIMG_OK);
		CHECK(partition.length == MIB && partition.missing_bytes == 0);
		CHECK(fsimg_image_next_partition(f.image, &partition, NULL) == FSIMG_OK);
		CHECK(partition.index == 2 && partition.used);
		CHECK(partition.offset == (8192 + past) * 512);
		CHECK(partition.length == 0 && partition.missing_bytes == 1024 * 512);
		CHECK(partition.claim == FSIMG_CLAIM_NONE);
		expect_scheme_truncated(&f, "mbr", info.missing_bytes);
		CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_INDEX,
		    .index = 2 }, &volume, &error) == FSIMG_ECORRUPT);
		CHECK(strstr(error.message, "truncated") != NULL && volume == NULL);
		CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_AUTOMATIC },
		    &volume, NULL) == FSIMG_OK);
		expect_volume_lists(&f, volume, "/DIR");
		expect_scheme_truncated(&f, "mbr", info.missing_bytes);
	}
	fixture_free(&f);
	free(disk.bytes);
}

static void
test_gpt_truncated_partitions_and_backup_report_missing_bytes(void)
{
	static const uint64_t cuts[] = { 3 * MIB - 512, 3 * MIB - 123,
	    2 * MIB, 2 * MIB - 512, 4 * MIB - 512, 4 * MIB - 1 };
	fsimg_partition_info_t partition;
	fsimg_image_info_t info;
	fsimg_error_t error;
	fsimg_io_t *volume;
	memory_disk_t disk;
	fixture_t f;

	fixture_init(&f);
	for (uint32_t sector = 512; sector <= 4096; sector *= 8) {
		disk = two_partition_gpt(sector);
		for (size_t i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++) {
			disk.length = cuts[i];
			fprintf(stderr, "GPT sector %u, cut at %llu bytes\n",
			    sector, (unsigned long long)disk.length);
			CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
			CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
			CHECK(info.sector_bytes == sector);
			CHECK(info.missing_bytes == 4 * MIB - disk.length);
			CHECK(fsimg_image_next_partition(f.image, &partition, NULL) == FSIMG_OK);
			CHECK(partition.offset == MIB);
			CHECK(partition.length == (disk.length < 2 * MIB ?
			    disk.length - MIB : MIB));
			CHECK(partition.missing_bytes == MIB - partition.length);
			CHECK(fsimg_image_next_partition(f.image, &partition, NULL) == FSIMG_OK);
			CHECK(partition.offset == 2 * MIB);
			CHECK(partition.length == (disk.length <= 2 * MIB ? 0 :
			    (disk.length < 3 * MIB ? disk.length - 2 * MIB : MIB)));
			CHECK(partition.missing_bytes == MIB - partition.length);
			if (partition.length == 0) {
				CHECK(partition.claim == FSIMG_CLAIM_NONE);
				CHECK(select_with(&f, (fsimg_selector_t){
				    .kind = FSIMG_SELECT_INDEX, .index = 2 },
				    &volume, &error) == FSIMG_ECORRUPT);
				CHECK(strstr(error.message, "truncated") != NULL);
				CHECK(volume == NULL);
				CHECK(select_with(&f, (fsimg_selector_t){
				    .kind = FSIMG_SELECT_NAME, .text = "Donn\xc3\xa9" "es" },
				    &volume, &error) == FSIMG_ECORRUPT);
				CHECK(strstr(error.message, "truncated") != NULL);
				CHECK(volume == NULL);
				CHECK(select_with(&f, (fsimg_selector_t){
				    .kind = FSIMG_SELECT_TYPE, .text = "linux" },
				    &volume, &error) == FSIMG_ECORRUPT);
				CHECK(strstr(error.message, "truncated") != NULL);
				CHECK(volume == NULL);
			}
			expect_scheme_truncated(&f, "gpt", info.missing_bytes);
			CHECK(select_with(&f, (fsimg_selector_t){
			    .kind = FSIMG_SELECT_AUTOMATIC }, &volume, NULL) == FSIMG_OK);
			expect_volume_lists(&f, volume, "/DIR");
			expect_scheme_truncated(&f, "gpt", info.missing_bytes);
		}
		/* Sector-size ambiguity also applies when the backup is absent. */
		if (sector == 4096) {
			bcopy("EFI PART", disk.bytes + 512, 8);
			CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
			CHECK(fsimg_image_get_info(f.image, &info, &error) == FSIMG_EFORMAT);
			CHECK(strstr(error.message, "ambiguous") != NULL);
			CHECK(fixture_open(&f, &disk, "gpt", 4096, NULL) == FSIMG_OK);
			CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
		}
		free(disk.bytes);
	}
	fixture_free(&f);
}

static void
test_gpt_failed_primary_stays_corrupt_on_cut_disk(void)
{
	static const struct {
		unsigned offset;
		uint32_t value;
		const char *message;
	} edits[] = {
		{ 24, 2, "GPT primary header does not record LBA 1" },
		{ 84, 256, "GPT primary entry size is not 128 bytes" }
	};
	memory_disk_t disk;

	for (size_t i = 0; i < sizeof(edits) / sizeof(edits[0]); i++) {
		disk = two_partition_gpt(512);
		put32(disk.bytes + 512 + edits[i].offset, edits[i].value);
		gpt_reseal(&disk, 512);
		disk.length -= 512;
		expect_gpt_corrupt(&disk, edits[i].message);
		free(disk.bytes);
	}
	disk = two_partition_gpt(512);
	disk.bytes[512 + 40] ^= 1;
	/* An intact backup at the last LBA cannot rescue a failed primary. */
	expect_gpt_corrupt(&disk, "GPT primary header CRC32 mismatch");
	disk.length -= 512;
	expect_gpt_corrupt(&disk, "GPT primary header CRC32 mismatch");
	free(disk.bytes);
	disk = two_partition_gpt(512);
	disk.bytes[1024 + 57] ^= 1;
	disk.length -= 512;
	expect_gpt_corrupt(&disk, "GPT primary entry array CRC32 mismatch");
	free(disk.bytes);
}

static void
test_gpt_primary_array_cuts_are_corrupt(void)
{
	static const uint64_t cuts[] = { 1025, 1536, 1537, 8193, 17407 };
	memory_disk_t disk = two_partition_gpt(512);

	for (size_t i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++) {
		disk.length = cuts[i];
		fprintf(stderr, "GPT primary cut at %llu bytes\n",
		    (unsigned long long)disk.length);
		expect_gpt_corrupt(&disk, cuts[i] < 1536 ?
		    "GPT disk is too small for a table" :
		    "GPT primary entry array is truncated");
	}
	disk.length = 700;
	expect_gpt_corrupt(&disk, "GPT disk is too small for a table");
	free(disk.bytes);
}

/* Successful unwraps are compared byte for byte in memory. A failed unwrap
 * must never reach this sink, even if some partition bytes survive.
 */
static fsimg_status_t
unwrap_write(void *argument, uint64_t offset, const void *bytes, size_t length,
    size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	memory_disk_t *output = argument;

	CHECK(offset == output->stored);
	CHECK(offset <= output->length && length <= output->length - offset);
	bcopy(bytes, output->bytes + offset, length);
	output->stored += length;
	*transferred = length;
	return (FSIMG_OK);
}

static void
test_unwrap_requires_a_whole_partition(void)
{
	static const uint64_t cuts[] = { 2 * MIB - 123, MIB, 4 * MIB - 512 };
	memory_disk_t output = disk_new(MIB);
	fsimg_selector_t selector = { .kind = FSIMG_SELECT_INDEX, .index = 1 };
	fsimg_io_ops_t ops;
	fsimg_io_t *destination;
	fsimg_sink_t sink;
	fsimg_error_t error;
	memory_disk_t disk;
	fixture_t f;

	fixture_init(&f);
	fsimg_io_ops_init(&ops);
	ops.write_at = unwrap_write;
	ops.get_size = disk_size;
	CHECK(fsimg_io_from_callbacks(f.ctx, &ops, &output, &destination,
	    NULL) == FSIMG_OK);
	fsimg_sink_init(&sink, FSIMG_SINK_IO);
	sink.io = destination;
	for (unsigned gpt = 0; gpt < 2; gpt++) {
		disk = gpt != 0 ? two_partition_gpt(512) : disk_new(4 * MIB);
		if (gpt == 0) {
			make_fat12(disk.bytes + MIB);
			mbr_entry(disk.bytes, 0, 0, 1, 2048, 2048);
			mbr_entry(disk.bytes, 1, 0, 0x83, 6144, 2048);
		}
		for (size_t i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++) {
			disk.length = cuts[i];
			output.stored = 0;
			memset(output.bytes, 0xa5, MIB);
			CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
			if (disk.length < 2 * MIB) {
				CHECK(fsimg_image_unwrap(f.image, &selector, NULL,
				    &sink, &error) == FSIMG_ECORRUPT);
				if (disk.length > MIB)
					CHECK(strcmp(error.message, "the selected partition is truncated: 123 declared bytes are missing") == 0);
				else
					CHECK(strstr(error.message, "truncated") != NULL);
				CHECK(output.stored == 0);
				CHECK(error.output_state == FSIMG_OUTPUT_ABSENT);
				for (size_t byte = 0; byte < MIB; byte++)
					CHECK(output.bytes[byte] == 0xa5);
			} else {
				CHECK(fsimg_image_unwrap(f.image, &selector, NULL,
				    &sink, &error) == FSIMG_OK);
				CHECK(output.stored == MIB);
				CHECK(bcmp(output.bytes, disk.bytes + MIB, MIB) == 0);
			}
		}
		free(disk.bytes);
	}
	fsimg_io_free(destination);
	fixture_free(&f);
	free(output.bytes);
}

static void
test_extended_mbr_past_end_is_listed_but_unsupported(void)
{
	memory_disk_t disk = disk_new(512);
	fsimg_partition_info_t partition;
	fsimg_error_t error;
	fsimg_io_t *volume;
	fixture_t f;

	mbr_entry(disk.bytes, 0, 0, 0x0f, 2048, 2048);
	fixture_init(&f);
	CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_next_partition(f.image, &partition, NULL) == FSIMG_OK);
	CHECK(partition.index == 1 && partition.used);
	CHECK(partition.offset == MIB && partition.length == 0);
	CHECK(partition.missing_bytes == MIB);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_INDEX,
	    .index = 1 }, &volume, &error) == FSIMG_EUNSUPPORTED);
	CHECK(strcmp(error.message, "partition 1 is an extended partition; logical partitions are not supported") == 0);
	CHECK(volume == NULL);
	fixture_free(&f);
	free(disk.bytes);
}

static void
test_gpt_primary_array_end_and_trailing_partial_sector(void)
{
	fsimg_layer_report_t report;
	fsimg_image_info_t info;
	memory_disk_t disk;
	fixture_t f;

	fixture_init(&f);
	for (uint32_t sector = 512; sector <= 4096; sector *= 8) {
		disk = two_partition_gpt(sector);
		disk.length = 2 * sector + GPT_ENTRIES * 128;
		CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
		CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
		CHECK(info.missing_bytes == 4 * MIB - disk.length);
		disk.length--;
		expect_gpt_corrupt(&disk, "GPT primary entry array is truncated");
		disk.length = 2 * sector + 1;
		expect_gpt_corrupt(&disk, "GPT disk is too small for a table");
		free(disk.bytes);
	}

	/* The backup remains at the last complete LBA. The sparse read callback
	 * supplies a partial trailing sector without enlarging the allocation.
	 */
	disk = two_partition_gpt(512);
	disk.length += 123;
	CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(info.missing_bytes == 0);
	CHECK(fsimg_image_verify(f.image, 0, &report, NULL) == FSIMG_OK);
	fixture_free(&f);
	free(disk.bytes);
}

static void
test_gpt_declared_geometry_cannot_wrap(void)
{
	static const uint64_t alternates[] = { UINT64_MAX, UINT64_MAX / 512 };
	const uint64_t alternate = UINT64_MAX / 512 - 1;
	const uint64_t last_usable = alternate - 33;
	fsimg_partition_info_t partition;
	fsimg_image_info_t info;
	memory_disk_t disk;
	fixture_t f;

	for (size_t i = 0; i < sizeof(alternates) / sizeof(alternates[0]); i++) {
		disk = two_partition_gpt(512);
		put64(disk.bytes + 512 + 32, alternates[i]);
		gpt_reseal(&disk, 512);
		disk.length = 3 * MIB;
		expect_gpt_corrupt(&disk,
		    "GPT primary alternate LBA declares a disk no offset can address");
		free(disk.bytes);
	}

	/* The largest addressable disk has space for both backup structures.
	 * One entry reaches the usable limit; no huge allocation is needed.
	 */
	disk = two_partition_gpt(512);
	bzero(disk.bytes + 1024 + 128, 128);
	put64(disk.bytes + 512 + 32, alternate);
	put64(disk.bytes + 512 + 48, last_usable);
	put64(disk.bytes + 1024 + 40, last_usable);
	gpt_reseal(&disk, 512);
	disk.length = 3 * MIB;
	fixture_init(&f);
	CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(info.missing_bytes == (alternate + 1) * 512 - disk.length);
	CHECK(fsimg_image_next_partition(f.image, &partition, NULL) == FSIMG_OK);
	CHECK(partition.offset == MIB);
	CHECK(partition.length == disk.length - MIB);
	CHECK(partition.missing_bytes == (last_usable + 1) * 512 - disk.length);
	fixture_free(&f);
	free(disk.bytes);

	disk = two_partition_gpt(512);
	put64(disk.bytes + 512 + 48, 8191);
	gpt_reseal(&disk, 512);
	disk.length = 3 * MIB;
	expect_gpt_corrupt(&disk, "GPT primary usable range is invalid");
	free(disk.bytes);

	/* A primary-only table still reserves the backup entry array. */
	disk = two_partition_gpt(512);
	put64(disk.bytes + 512 + 48, 8190);
	gpt_reseal(&disk, 512);
	disk.length = 3 * MIB;
	expect_gpt_corrupt(&disk,
	    "GPT primary usable range leaves no room for the backup array");
	free(disk.bytes);
}

static void
test_gpt_seeded_failures(void)
{
	/* With 512-byte sectors on the 4 MiB disk the primary header is at
	 * 512, its array at 1024, the backup array at 33 sectors from the
	 * end and the backup header in the last sector.
	 */
	static const struct {
		const char	*check;
		uint64_t	 offset;	/* Byte whose low bit flips */
		bool		 reseal;
	} flips[] = {
		{ "GPT primary header CRC32 mismatch", 512 + 40, false },
		{ "GPT primary entry array CRC32 mismatch", 1024 + 57, false },
		{ "GPT backup header CRC32 mismatch", 4 * MIB - 512 + 40, false },
		{ "GPT backup entry array differs from the primary",
		    4 * MIB - 33 * 512 + 57, true },
		{ "GPT backup header differs from the primary",
		    4 * MIB - 512 + 56, true }
	};
	static const struct {
		const char	*check;
		uint64_t	 header;	/* Byte offset of the header */
		unsigned	 field;
		unsigned	 width;
		uint64_t	 value;
	} fields[] = {
		{ "GPT primary header does not record LBA 1", 512, 24, 8, 2 },
		{ "GPT primary entry size is not 128 bytes", 512, 84, 4, 256 },
		{ "GPT primary entry count is outside 1 to 1024", 512, 80, 4, 0 },
		{ "GPT primary entry count is outside 1 to 1024", 512, 80, 4, 1025 },
		{ "GPT primary entry array overlaps the header or the usable range",
		    512, 72, 8, 1 },
		{ "GPT primary entry array overlaps the header or the usable range",
		    512, 72, 8, 3 },
		{ "GPT primary entry array overlaps the header or the usable range",
		    512, 72, 8, UINT64_MAX },
		{ "GPT primary usable range is invalid", 512, 48, 8, UINT64_MAX },
		{ "GPT backup header does not point back to the primary",
		    4 * MIB - 512, 32, 8, 2 },
		{ "GPT backup header does not point back to the primary",
		    4 * MIB - 512, 24, 8, UINT64_MAX },
		{ "GPT backup entry array overlaps the usable range or the header",
		    4 * MIB - 512, 72, 8, 2 },
		{ "GPT backup entry array overlaps the usable range or the header",
		    4 * MIB - 512, 72, 8, 8190 },
		{ "GPT backup header size is out of range", 4 * MIB - 512, 12, 4,
		    UINT32_MAX }
	};
	gpt_partition_t overlapping[] = {
		{ ESP_GUID, name_a, 2048, 4095 },
		{ LINUX_GUID, name_b, 4000, 5000 }
	};
	gpt_partition_t outside[] = {
		{ LINUX_GUID, name_a, 2048, 8191 - 32 }
	};
	memory_disk_t disk;
	fsimg_layer_report_t report;
	fixture_t f;

	for (size_t i = 0; i < sizeof(flips) / sizeof(flips[0]); i++) {
		disk = two_partition_gpt(512);
		disk.bytes[flips[i].offset] ^= 1;
		if (flips[i].reseal)
			gpt_reseal(&disk, 512);
		expect_gpt_corrupt(&disk, flips[i].check);
		free(disk.bytes);
	}

	disk = two_partition_gpt(512);
	put64(disk.bytes + 512 + 32, 8190);
	gpt_reseal(&disk, 512);
	expect_gpt_corrupt(&disk, "GPT primary alternate LBA is before the disk's last LBA");
	free(disk.bytes);

	disk = two_partition_gpt(512);
	put64(disk.bytes + 512 + 40, 8192);
	gpt_reseal(&disk, 512);
	expect_gpt_corrupt(&disk, "GPT primary usable range is invalid");
	free(disk.bytes);

	/* The geometry fields that keep the entry arrays inside the disk; each
	 * edit reseals only its header, since these checks precede the arrays.
	 */
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		disk = two_partition_gpt(512);
		if (fields[i].width == 4)
			put32(disk.bytes + fields[i].header + fields[i].field,
			    (uint32_t)fields[i].value);
		else
			put64(disk.bytes + fields[i].header + fields[i].field,
			    fields[i].value);
		put32(disk.bytes + fields[i].header + 16, 0);
		put32(disk.bytes + fields[i].header + 16,
		    crc32_oracle(disk.bytes + fields[i].header, 92));
		expect_gpt_corrupt(&disk, fields[i].check);
		free(disk.bytes);
	}

	/* A protective MBR and a primary header in a two-sector disk. */
	disk = two_partition_gpt(512);
	disk.length = 1024;
	expect_gpt_corrupt(&disk, "GPT disk is too small for a table");
	free(disk.bytes);

	disk = disk_new(4 * MIB);
	gpt_build(&disk, 512, overlapping, 2);
	expect_gpt_corrupt(&disk, "GPT partitions 1 and 2 overlap");
	disk.length = MIB;
	expect_gpt_corrupt(&disk, "GPT partitions 1 and 2 overlap");
	free(disk.bytes);

	disk = disk_new(4 * MIB);
	gpt_build(&disk, 512, outside, 1);
	expect_gpt_corrupt(&disk, "GPT partition 1 lies outside the usable range");
	disk.length = MIB;
	expect_gpt_corrupt(&disk, "GPT partition 1 lies outside the usable range");
	free(disk.bytes);

	/* A truncated disk has lost its backup at the recorded last LBA. */
	disk = two_partition_gpt(512);
	disk.length -= 512;
	fixture_init(&f);
	CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
	expect_scheme_truncated(&f, "gpt", 512);
	fixture_free(&f);
	disk.length += 512;
	bzero(disk.bytes + 4 * MIB - 512, 512);
	expect_gpt_corrupt(&disk, "GPT backup header signature is missing");
	free(disk.bytes);

	/* A protective MBR claims the disk even without a header. */
	disk = two_partition_gpt(512);
	bzero(disk.bytes + 512, 512);
	expect_gpt_corrupt(&disk, "GPT protective MBR has no GPT header at LBA 1");
	free(disk.bytes);

	/* Reading needs no protective MBR; verification does. */
	disk = two_partition_gpt(512);
	bzero(disk.bytes, 512);
	fixture_init(&f);
	CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_verify(f.image, 0, &report, NULL) == FSIMG_ECORRUPT);
	fixture_free(&f);
	free(disk.bytes);
}

/* A 4Kn table is found at 4096 bytes. Headers at both offsets need the
 * forced sector size, which then reads only its own copy.
 */
static void
test_gpt_sector_sizes(void)
{
	memory_disk_t disk = two_partition_gpt(4096);
	fsimg_image_info_t info;
	fsimg_error_t error;
	fsimg_io_t *volume;
	fixture_t f;

	fixture_init(&f);
	CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(info.sector_bytes == 4096 && info.partition_count == GPT_ENTRIES);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_AUTOMATIC },
	    &volume, NULL) == FSIMG_OK);
	expect_volume_lists(&f, volume, "/DIR");

	bcopy("EFI PART", disk.bytes + 512, 8);
	CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, &error) == FSIMG_EFORMAT);
	CHECK(strstr(error.message, "ambiguous") != NULL);
	CHECK(fixture_open(&f, &disk, "fat12/gpt", 4096, NULL) == FSIMG_OK);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_AUTOMATIC },
	    &volume, NULL) == FSIMG_OK);
	expect_volume_lists(&f, volume, "/DIR");
	CHECK(fixture_open(&f, &disk, "gpt", 512, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, &error) == FSIMG_ECORRUPT);
	CHECK(strcmp(error.message, "GPT primary header size is out of range") == 0);
	CHECK(fixture_open(&f, &disk, "fat12", 4096, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.message,
	    "setting names a layer the stack does not contain") == 0 &&
	    strcmp(error.path, "gpt:sector") == 0);
	fixture_free(&f);
	free(disk.bytes);
}

/* Four slots with the FAT type bytes read accepts; the FAT12 volume sits
 * in the active first slot.
 */
static void
test_mbr_read(void)
{
	memory_disk_t disk = disk_new(4 * MIB);
	fsimg_partition_info_t partition;
	fsimg_layer_report_t report;
	fsimg_image_info_t info;
	fsimg_error_t error;
	fsimg_io_t *volume;
	fixture_t f;

	make_fat12(disk.bytes + MIB);
	put32(disk.bytes + 440, 0x12345678);
	mbr_entry(disk.bytes, 0, 0x80, 0x0e, 2048, 2048);
	mbr_entry(disk.bytes, 1, 0x00, 0x0b, 4096, 2048);
	mbr_entry(disk.bytes, 2, 0x00, 0x04, 6144, 1024);
	fixture_init(&f);
	CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.scheme, "mbr") == 0 && info.partition_count == 4);
	CHECK(info.sector_bytes == 512 && info.mbr_disk_id == 0x12345678);
	CHECK(fsimg_image_next_partition(f.image, &partition, NULL) == FSIMG_OK);
	CHECK(partition.index == 1 && partition.active && partition.mbr_type == 0x0e);
	CHECK(strcmp(partition.type, "fat16") == 0 && partition.offset == MIB);
	CHECK(strcmp(partition.format, "fat12") == 0 && partition.name[0] == '\0');
	CHECK(fsimg_image_next_partition(f.image, &partition, NULL) == FSIMG_OK);
	CHECK(partition.index == 2 && !partition.active &&
	    strcmp(partition.type, "fat32") == 0);
	CHECK(fsimg_image_next_partition(f.image, &partition, NULL) == FSIMG_OK);
	CHECK(partition.index == 3 && strcmp(partition.type, "fat16") == 0);
	CHECK(fsimg_image_next_partition(f.image, &partition, NULL) == FSIMG_OK);
	CHECK(partition.index == 4 && !partition.used);
	CHECK(fsimg_image_next_partition(f.image, &partition, NULL) == FSIMG_DONE);

	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_AUTOMATIC },
	    &volume, NULL) == FSIMG_OK);
	expect_volume_lists(&f, volume, "/DIR");
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_TYPE,
	    .text = "fat32" }, &volume, NULL) == FSIMG_OK);
	fsimg_io_free(volume);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_TYPE,
	    .text = "0x04" }, &volume, NULL) == FSIMG_OK);
	fsimg_io_free(volume);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK &&
	    info.selected == 3);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_TYPE,
	    .text = "fat16" }, &volume, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.message, "several partitions have that type") == 0);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_NAME,
	    .text = "EFI" }, &volume, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.message, "partition names exist only in GPT") == 0);
	CHECK(fsimg_image_verify(f.image, FSIMG_VERIFY_PAYLOAD, &report, NULL) ==
	    FSIMG_OK);
	CHECK(strcmp(report.scheme, "mbr") == 0 && report.partitions == 3);

	/* An extended partition is listed but not selectable. */
	mbr_entry(disk.bytes, 3, 0x00, 0x05, 7168, 1024);
	CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_INDEX,
	    .index = 4 }, &volume, NULL) == FSIMG_EUNSUPPORTED);
	fixture_free(&f);
	free(disk.bytes);
}

static void
test_mbr_seeded_failures(void)
{
	static const struct {
		uint32_t	 first[2];
		uint32_t	 sectors[2];
		const char	*check;
	} corrupt[] = {
		{ { 2048, 3000 }, { 2048, 100 }, "MBR partitions 1 and 2 overlap" },
		{ { 9000, 9500 }, { 1000, 1000 }, "MBR partitions 1 and 2 overlap" }
	};
	memory_disk_t disk;
	fsimg_image_info_t info;
	fsimg_error_t error;
	fsimg_io_t *volume;
	fixture_t f;

	fixture_init(&f);
	for (size_t i = 0; i < sizeof(corrupt) / sizeof(corrupt[0]); i++) {
		disk = disk_new(4 * MIB);
		for (unsigned slot = 0; slot < 2; slot++) {
			if (corrupt[i].sectors[slot] != 0)
				mbr_entry(disk.bytes, slot, 0x00, 0x0c,
				    corrupt[i].first[slot], corrupt[i].sectors[slot]);
		}
		CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
		CHECK(fsimg_image_get_info(f.image, &info, &error) == FSIMG_ECORRUPT);
		CHECK(strcmp(error.message, corrupt[i].check) == 0);
		free(disk.bytes);
	}

	/* Without its signature an MBR is no claim at all. */
	disk = disk_new(4 * MIB);
	mbr_entry(disk.bytes, 0, 0x00, 0x0c, 2048, 2048);
	disk.bytes[510] = 0;
	CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, &error) == FSIMG_EFORMAT);
	CHECK(strcmp(error.message, "no partition table or filesystem recognized") == 0);
	CHECK(fixture_open(&f, &disk, "mbr", 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, &error) == FSIMG_EFORMAT);
	CHECK(strcmp(error.message, "no MBR partition table") == 0);
	free(disk.bytes);

	/* A 0xEE entry belongs to GPT, whose header is then missing. */
	disk = disk_new(4 * MIB);
	mbr_entry(disk.bytes, 0, 0x00, 0x0c, 2048, 2048);
	mbr_entry(disk.bytes, 1, 0x00, 0xee, 1, 2047);
	CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, &error) == FSIMG_ECORRUPT);
	CHECK(strcmp(error.message, "GPT protective MBR has no GPT header at LBA 1") == 0);
	free(disk.bytes);

	/* A FAT boot sector whose code area also reads as a table is
	 * ambiguous until the scheme slot is forced empty.
	 */
	disk = disk_new(FAT_BYTES);
	make_fat12(disk.bytes);
	mbr_entry(disk.bytes, 0, 0x00, 0x0c, 1, 16);
	CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, &error) == FSIMG_EFORMAT);
	CHECK(strstr(error.message, "both a partition table and a filesystem") != NULL);
	CHECK(fixture_open(&f, &disk, "fat12/-", 0, NULL) == FSIMG_OK);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_AUTOMATIC },
	    &volume, NULL) == FSIMG_OK);
	expect_volume_lists(&f, volume, "/DIR");
	fixture_free(&f);
	free(disk.bytes);
}

/* A used entry at sector zero describes a whole disk, not a partition. */
static void
test_mbr_zero_lba_is_not_a_table(void)
{
	memory_disk_t disk;
	fsimg_image_info_t info;
	fsimg_error_t error;
	fsimg_io_t *volume;
	fixture_t f;

	fixture_init(&f);
	for (unsigned entries = 1; entries <= 2; entries++) {
		disk = disk_new(4 * MIB);
		mbr_entry(disk.bytes, 0, 0x00, 0x01, 0, 100);
		if (entries == 2)
			mbr_entry(disk.bytes, 1, 0x00, 0x01, 2048, 2048);
		CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
		CHECK(fsimg_image_get_info(f.image, &info, &error) == FSIMG_EFORMAT);
		CHECK(strcmp(error.message,
		    "no partition table or filesystem recognized") == 0);
		CHECK(fixture_open(&f, &disk, "mbr", 0, NULL) == FSIMG_OK);
		CHECK(fsimg_image_get_info(f.image, &info, &error) == FSIMG_EFORMAT);
		CHECK(strcmp(error.message, "no MBR partition table") == 0);
		free(disk.bytes);
	}

	disk = disk_new(FAT_BYTES);
	make_fat12(disk.bytes);
	mbr_entry(disk.bytes, 0, 0x00, 0x01, 0, FAT_BYTES / 512);
	CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.scheme, "") == 0);
	CHECK(strcmp(info.hybrid_scheme, "") == 0);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_AUTOMATIC },
	    &volume, NULL) == FSIMG_OK);
	expect_volume_lists(&f, volume, "/DIR");
	CHECK(fixture_open(&f, &disk, "mbr", 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, &error) == FSIMG_EFORMAT);
	CHECK(strcmp(error.message, "no MBR partition table") == 0);
	fixture_free(&f);
	free(disk.bytes);
}

/* A boot descriptor at block 16 suffices for ISO's identity probe. Its
 * system area can hold a table without making the bare ISO ambiguous.
 * Only detection is tested here; CLI fixtures contain a complete ISO.
 */
static void
test_iso_system_area_scheme(void)
{
	static const struct {
		const char	*stack;
		const char	*scheme;
		const char	*hybrid;
	} cases[] = {
		{ NULL, "", "gpt" },
		{ "iso9660", "", "gpt" },
		{ "gpt", "gpt", "" },
		{ "iso9660/-", "", "" }
	};
	gpt_partition_t partition = {
		ESP_GUID, name_efi, MIB / 512, 2 * MIB / 512 - 1
	};
	memory_disk_t disk = disk_new(4 * MIB);
	fsimg_image_info_t info;
	fsimg_error_t error;
	fsimg_io_t *volume;
	fixture_t f;

	make_fat12(disk.bytes + MIB);
	gpt_build(&disk, 512, &partition, 1);
	bcopy("\000CD001\001", disk.bytes + 32768, 7);
	fixture_init(&f);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		CHECK(fixture_open(&f, &disk, cases[i].stack, 0, NULL) == FSIMG_OK);
		CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
		CHECK(strcmp(info.scheme, cases[i].scheme) == 0);
		CHECK(strcmp(info.hybrid_scheme, cases[i].hybrid) == 0);
		CHECK(info.partition_count ==
		    (cases[i].scheme[0] == '\0' ? 0 : GPT_ENTRIES));
		if (cases[i].stack == NULL) {
			CHECK(select_with(&f, (fsimg_selector_t){
			    .kind = FSIMG_SELECT_INDEX, .index = 1 },
			    &volume, &error) == FSIMG_EINVAL);
			CHECK(strcmp(error.message, "the disk is read as its "
			    "volume; force the gpt scheme to select a "
			    "partition") == 0);
		}
		if (cases[i].scheme[0] != '\0') {
			CHECK(select_with(&f, (fsimg_selector_t){
			    .kind = FSIMG_SELECT_AUTOMATIC }, &volume, NULL) == FSIMG_OK);
			expect_volume_lists(&f, volume, "/DIR");
			CHECK(select_with(&f, (fsimg_selector_t){
			    .kind = FSIMG_SELECT_INDEX, .index = 1 },
			    &volume, NULL) == FSIMG_OK);
			expect_volume_lists(&f, volume, "/DIR");
		}
	}
	free(disk.bytes);

	disk = disk_new(4 * MIB);
	make_fat12(disk.bytes + MIB);
	mbr_entry(disk.bytes, 0, 0x00, 0x01, MIB / 512, FAT_BYTES / 512);
	bcopy("\000CD001\001", disk.bytes + 32768, 7);
	CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.scheme, "") == 0);
	CHECK(strcmp(info.hybrid_scheme, "mbr") == 0);
	CHECK(info.partition_count == 0);
	fixture_free(&f);
	free(disk.bytes);
}

/* A protective MBR claims GPT even when its header is missing. ISO's
 * reserved system area does not excuse a corrupt partition-table claim.
 */
static void
test_iso_corrupt_gpt_is_ambiguous(void)
{
	memory_disk_t disk = disk_new(4 * MIB);
	fsimg_image_info_t info;
	fsimg_error_t error;
	fixture_t f;

	mbr_entry(disk.bytes, 0, 0x00, 0xee, 1, disk.length / 512 - 1);
	bcopy("\000CD001\001", disk.bytes + 32768, 7);
	fixture_init(&f);
	CHECK(fixture_open(&f, &disk, "gpt", 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, &error) == FSIMG_ECORRUPT);
	CHECK(strcmp(error.message,
	    "GPT protective MBR has no GPT header at LBA 1") == 0);
	CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, &error) == FSIMG_EFORMAT);
	CHECK(strstr(error.message,
	    "both a partition table and a filesystem") != NULL);
	fixture_free(&f);
	free(disk.bytes);
}

/* A primary descriptor invokes ISO's geometry probe. Its zero volume
 * space size is corrupt, so a valid GPT cannot become a hybrid hint.
 */
static void
test_corrupt_iso_gpt_is_ambiguous(void)
{
	gpt_partition_t partition = {
		ESP_GUID, name_efi, MIB / 512, 2 * MIB / 512 - 1
	};
	memory_disk_t disk = disk_new(4 * MIB);
	fsimg_image_info_t info;
	fsimg_error_t error;
	fixture_t f;

	gpt_build(&disk, 512, &partition, 1);
	bcopy("\001CD001\001", disk.bytes + 32768, 7);
	fixture_init(&f);
	CHECK(fixture_open(&f, &disk, "gpt", 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.scheme, "gpt") == 0);
	CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, &error) == FSIMG_EFORMAT);
	CHECK(strstr(error.message,
	    "both a partition table and a filesystem") != NULL);
	fixture_free(&f);
	free(disk.bytes);
}

/* FAT's probe uses only the boot sector. The GPT encoder preserves its
 * BPB while replacing FAT data with GPT metadata; no FAT read is attempted.
 * A filesystem without a reserved system area remains ambiguous with GPT.
 */
static void
test_fat_gpt_is_ambiguous(void)
{
	gpt_partition_t partition = {
		ESP_GUID, name_efi, MIB / 512, 2 * MIB / 512 - 1
	};
	memory_disk_t disk = disk_new(4 * MIB);
	fsimg_image_info_t info;
	fsimg_error_t error;
	fixture_t f;

	make_fat12(disk.bytes);
	/* Clear the primary GPT area, including any FAT directory bytes. */
	bzero(disk.bytes + 512, 512 + GPT_ENTRIES * 128);
	gpt_build(&disk, 512, &partition, 1);
	fixture_init(&f);
	CHECK(fixture_open(&f, &disk, "gpt", 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.scheme, "gpt") == 0);
	CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, &error) == FSIMG_EFORMAT);
	CHECK(strstr(error.message,
	    "both a partition table and a filesystem") != NULL);
	fixture_free(&f);
	free(disk.bytes);
}

/* An entry addresses sectors below 2^32: a partition may end exactly
 * there on a 2 TiB disk, and one sector more cannot be expressed.
 */
static void
test_mbr_addressable_limit(void)
{
	memory_disk_t disk = disk_new(512);
	fsimg_partition_info_t partition;
	fsimg_image_info_t info;
	fsimg_error_t error;
	fixture_t f;

	disk.length = UINT64_C(512) << 32;
	mbr_entry(disk.bytes, 0, 0x00, 0x83, 2048, 0xffffffffu - 2047);
	fixture_init(&f);
	CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_next_partition(f.image, &partition, NULL) == FSIMG_OK);
	CHECK(partition.offset + partition.length == disk.length);
	CHECK(strcmp(partition.type, "linux") == 0);
	mbr_entry(disk.bytes, 0, 0x00, 0x83, 2048, 0xffffffffu - 2046);
	CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, &error) == FSIMG_ECORRUPT);
	CHECK(strcmp(error.message,
	    "MBR partition 1 ends beyond the sectors an entry can address") == 0);
	fixture_free(&f);
	free(disk.bytes);
}

/* A disk without a scheme is one volume with no slots. */
static void
test_bare_volume(void)
{
	memory_disk_t disk = disk_new(FAT_BYTES);
	fsimg_partition_info_t partition;
	fsimg_image_info_t info;
	fsimg_error_t error;
	fsimg_io_t *volume;
	fixture_t f;

	make_fat12(disk.bytes);
	fixture_init(&f);
	CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(info.scheme[0] == '\0' && info.partition_count == 0);
	CHECK(fsimg_image_next_partition(f.image, &partition, NULL) == FSIMG_DONE);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_INDEX,
	    .index = 1 }, &volume, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.message, "the disk has no partition table") == 0);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_AUTOMATIC },
	    &volume, NULL) == FSIMG_OK);
	expect_volume_lists(&f, volume, "/DIR");
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.stack, "fat12") == 0 && info.selected == 0);
	fixture_free(&f);
	free(disk.bytes);
}

static void
test_assumed_identity_selects_unvouched_fat(void)
{
	memory_disk_t disk = disk_new(FAT_BYTES);
	fixture_t f;
	fsimg_image_options_t image_options;
	fsimg_read_options_t read_options;
	fsimg_io_t *volume;
	fsimg_reader_t *reader;
	const fsimg_entry_t *entry;
	unsigned char data[900], expected[900];
	size_t count;
	unsigned char *file = disk.bytes + 13 * 512 + 32;

	make_fat12(disk.bytes);
	bzero(disk.bytes, 3);
	bzero(disk.bytes + 510, 2);
	for (unsigned table = 0; table < 2; table++) {
		unsigned char *head = disk.bytes + (1 + table * 6) * 512;
		head[0] = 0, head[1] = 0, head[2] = 3;
		/* Keep directory cluster 2; the file follows 3 -> 4 -> EOC. */
		head[4] = 0x4f, head[5] = 0, head[6] = 0xff, head[7] = 0x0f;
	}
	bcopy("HELLO   TXT", file, 11);
	file[11] = 0x20;
	put16(file + 26, 3);
	put32(file + 28, sizeof(expected));
	for (size_t i = 0; i < sizeof(expected); i++)
		expected[i] = (unsigned char)(i * 37);
	bcopy(expected, disk.bytes + 28 * 512, sizeof(expected));
	fixture_init(&f);
	CHECK(fixture_open(&f, &disk, "fat12", 0, NULL) == FSIMG_OK);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_AUTOMATIC },
	    &volume, NULL) == FSIMG_EFORMAT);
	fsimg_image_free(f.image);
	f.image = NULL;
	fsimg_image_options_init(&image_options);
	CHECK(fsimg_stack_parse("fat12/-", &image_options.stack, NULL) == FSIMG_OK);
	image_options.flags = FSIMG_IMAGE_ASSUME_IDENTITY;
	CHECK(fsimg_image_open(f.ctx, f.io, &image_options, &f.image, NULL) == FSIMG_OK);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_AUTOMATIC },
	    &volume, NULL) == FSIMG_OK);
	fsimg_read_options_init(&read_options);
	read_options.format = "fat12";
	read_options.flags = FSIMG_READ_ASSUME_IDENTITY;
	CHECK(fsimg_reader_open(f.ctx, volume, &read_options, &reader, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_next(reader, &entry, NULL) == FSIMG_OK);
	CHECK(strcmp(entry->path, "/DIR") == 0);
	CHECK(fsimg_reader_next(reader, &entry, NULL) == FSIMG_OK);
	CHECK(strcmp(entry->path, "/HELLO.TXT") == 0);
	CHECK(fsimg_reader_read_at(reader, 0, data, sizeof(data), &count, NULL) == FSIMG_OK);
	CHECK(count == sizeof(data) && bcmp(data, expected, sizeof(data)) == 0);
	CHECK(fsimg_reader_next(reader, &entry, NULL) == FSIMG_DONE);
	fsimg_reader_free(reader);
	fsimg_io_free(volume);
	fixture_free(&f);
	free(disk.bytes);
}

static void
test_image_assume_identity_requires_named_slot_or_compression(void)
{
	memory_disk_t disk = disk_new(FAT_BYTES);
	fixture_t f;
	fsimg_image_options_t options;

	make_fat12(disk.bytes);
	fixture_init(&f);
	CHECK(fixture_open(&f, &disk, NULL, 0, NULL) == FSIMG_OK);
	fsimg_image_free(f.image);
	f.image = NULL;
	fsimg_image_options_init(&options);
	options.flags = FSIMG_IMAGE_ASSUME_IDENTITY;
	CHECK(fsimg_image_open(f.ctx, f.io, &options, &f.image, NULL) == FSIMG_EINVAL);
	fixture_free(&f);
	free(disk.bytes);
}

/* One partition at LBA 1, with recognizable bytes on both sides of it. */
static memory_disk_t
single_partition_mbr(void)
{
	memory_disk_t disk = disk_new(FAT_BYTES + 2 * 512);

	make_fat12(disk.bytes + 512);
	mbr_entry(disk.bytes, 0, 0x80, 0x01, 1, FAT_BYTES / 512);
	disk.bytes[0] = 0x5a;
	disk.bytes[512 + FAT_BYTES] = 0xa5;
	return (disk);
}

/* Reopen the fixture's I/O, assuming only identities named by this stack. */
static void
reopen_assuming_identity(fixture_t *f, const char *stack)
{
	fsimg_image_options_t options;

	fsimg_image_free(f->image);
	f->image = NULL;
	fsimg_image_options_init(&options);
	CHECK(fsimg_stack_parse(stack, &options.stack, NULL) == FSIMG_OK);
	options.flags = FSIMG_IMAGE_ASSUME_IDENTITY;
	CHECK(fsimg_image_open(f->ctx, f->io, &options, &f->image, NULL) == FSIMG_OK);
}

static void
test_assumed_mbr_signature_selects_partition_bytes(void)
{
	memory_disk_t disk = single_partition_mbr();
	fsimg_image_info_t info;
	fsimg_error_t error;
	fsimg_io_t *volume;
	fixture_t f;
	unsigned char *data;
	uint64_t length;
	size_t count, offset;

	bzero(disk.bytes + 510, 2);
	fixture_init(&f);
	CHECK(fixture_open(&f, &disk, "fat12/mbr/-", 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, &error) == FSIMG_EFORMAT);
	CHECK(strcmp(error.path, "mbr") == 0);

	reopen_assuming_identity(&f, "fat12/mbr/-");
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.scheme, "mbr") == 0);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_INDEX,
	    .index = 1 }, &volume, NULL) == FSIMG_OK);
	CHECK(fsimg_io_get_size(volume, &length, NULL) == FSIMG_OK);
	CHECK(length == FAT_BYTES);
	data = malloc((size_t)length);
	CHECK(data != NULL);
	for (offset = 0; offset < (size_t)length; offset += count) {
		CHECK(fsimg_io_read_at(volume, offset, data + offset,
		    (size_t)length - offset, &count, NULL) == FSIMG_OK);
		CHECK(count > 0 && count <= (size_t)length - offset);
	}
	CHECK(bcmp(data, disk.bytes + 512, (size_t)length) == 0);
	CHECK(fsimg_io_read_at(volume, length, data, 1, &count, NULL) == FSIMG_ERANGE);
	CHECK(count == 0);
	free(data);
	fsimg_io_free(volume);
	fixture_free(&f);
	free(disk.bytes);
}

/* Resolve identity first, then verify must refuse without further I/O. */
static void
expect_assumed_mbr_verify_failure(memory_disk_t *disk, const char *message)
{
	fsimg_layer_report_t report;
	fsimg_image_info_t info;
	fsimg_stats_t before, after;
	fsimg_error_t error;
	fixture_t f;

	fixture_init(&f);
	CHECK(fixture_open(&f, disk, "mbr", 0, NULL) == FSIMG_OK);
	reopen_assuming_identity(&f, "mbr");
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.scheme, "mbr") == 0);
	CHECK(fsimg_ctx_get_stats(f.ctx, &before, NULL) == FSIMG_OK);
	CHECK(fsimg_image_verify(f.image, FSIMG_VERIFY_PAYLOAD, &report,
	    &error) == FSIMG_ECORRUPT);
	CHECK(strcmp(error.message, message) == 0);
	CHECK(fsimg_ctx_get_stats(f.ctx, &after, NULL) == FSIMG_OK);
	CHECK(after.io_calls == before.io_calls);
	CHECK(after.read_bytes == before.read_bytes);
	fixture_free(&f);
}

static void
test_assumed_mbr_signature_is_reported_before_verification(void)
{
	memory_disk_t disk = single_partition_mbr();

	bzero(disk.bytes + 510, 2);
	expect_assumed_mbr_verify_failure(&disk,
	    "mbr identity was assumed: the MBR has no 0x55AA signature");
	free(disk.bytes);
}

static void
test_assumed_mbr_empty_table_is_reported_before_verification(void)
{
	memory_disk_t disk = single_partition_mbr();

	bzero(disk.bytes + 446, 4 * 16);
	expect_assumed_mbr_verify_failure(&disk,
	    "mbr identity was assumed: the MBR has no used partition entry");
	free(disk.bytes);
}

static void
test_signed_mbr_with_assume_identity_verifies(void)
{
	memory_disk_t disk = single_partition_mbr();
	fsimg_layer_report_t report;
	fixture_t f;

	fixture_init(&f);
	CHECK(fixture_open(&f, &disk, "mbr", 0, NULL) == FSIMG_OK);
	reopen_assuming_identity(&f, "mbr");
	CHECK(fsimg_image_verify(f.image, FSIMG_VERIFY_PAYLOAD, &report, NULL) == FSIMG_OK);
	CHECK(report.complete && report.partitions == 1);
	CHECK(strcmp(report.scheme, "mbr") == 0);
	fixture_free(&f);
	free(disk.bytes);
}

static void
test_assumed_mbr_still_validates_entries(void)
{
	static const struct {
		uint8_t status;
		uint8_t type;
		uint32_t first_lba;
		uint32_t sectors;
	} invalid[] = {
		{ 0x80, 0x01, 0, 2048 },
		{ 0x80, 0xee, 1, 2048 },
		{ 0x01, 0x01, 1, 2048 },
		{ 0x80, 0x00, 1, 2048 },
		{ 0x80, 0x01, 1, 0 }
	};
	memory_disk_t disk;
	fsimg_image_info_t info;
	fixture_t f;

	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		disk = single_partition_mbr();
		mbr_entry(disk.bytes, 0, invalid[i].status, invalid[i].type,
		    invalid[i].first_lba, invalid[i].sectors);
		bzero(disk.bytes + 510, 2);
		fixture_init(&f);
		CHECK(fixture_open(&f, &disk, "mbr", 0, NULL) == FSIMG_OK);
		reopen_assuming_identity(&f, "mbr");
		CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_EFORMAT);
		fixture_free(&f);
		free(disk.bytes);
	}
}

static void
test_assume_identity_does_not_change_mbr_detection(void)
{
	memory_disk_t disk = single_partition_mbr();
	fsimg_image_info_t info;
	fsimg_error_t plain, assumed;
	fixture_t f;

	bzero(disk.bytes + 510, 2);
	fixture_init(&f);
	CHECK(fixture_open(&f, &disk, "fat12", 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, &plain) == FSIMG_EFORMAT);
	reopen_assuming_identity(&f, "fat12");
	CHECK(fsimg_image_get_info(f.image, &info, &assumed) == FSIMG_EFORMAT);
	CHECK(strcmp(plain.message, assumed.message) == 0);
	CHECK(strcmp(plain.path, assumed.path) == 0);
	fixture_free(&f);
	free(disk.bytes);
}

/* Forcing selects and never relaxes: an absent forced signature is an
 * unrecognized image, and layers this build lacks are unsupported.
 */
static void
test_forcing(void)
{
	memory_disk_t gpt = two_partition_gpt(512);
	memory_disk_t bare = disk_new(FAT_BYTES);
	fsimg_image_info_t info;
	fsimg_error_t error;
	fsimg_io_t *volume;
	fixture_t f;

	make_fat12(bare.bytes);
	fixture_init(&f);
	/* A forced container runs only its own probe, which a raw disk fails. */
	CHECK(fixture_open(&f, &gpt, "fat32/-/dmg", 0, &error) == FSIMG_EFORMAT);
	CHECK(strcmp(error.message, "no DMG trailer") == 0);
	CHECK(fixture_open(&f, &gpt, "hfs", 0, NULL) == FSIMG_EUNSUPPORTED);
	CHECK(fixture_open(&f, &gpt, "fat12/mbr", 4096, NULL) == FSIMG_EINVAL);

	CHECK(fixture_open(&f, &bare, "gpt", 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, &error) == FSIMG_EFORMAT);
	CHECK(strcmp(error.message, "no GPT signature") == 0);

	/* A forced filesystem narrows the probes but still unwraps the GPT. */
	CHECK(fixture_open(&f, &gpt, "fat12", 0, NULL) == FSIMG_OK);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_AUTOMATIC },
	    &volume, NULL) == FSIMG_OK);
	expect_volume_lists(&f, volume, "/DIR");
	CHECK(fixture_open(&f, &gpt, "exfat", 0, NULL) == FSIMG_OK);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_AUTOMATIC },
	    &volume, &error) == FSIMG_EFORMAT);
	CHECK(strcmp(error.message, "no recognized filesystem in any partition") == 0);
	/* A scheme slot forced empty reads the whole disk as the volume. */
	CHECK(fixture_open(&f, &gpt, "fat12/-", 0, NULL) == FSIMG_OK);
	CHECK(select_with(&f, (fsimg_selector_t){ .kind = FSIMG_SELECT_AUTOMATIC },
	    &volume, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(info.scheme[0] == '\0' && strcmp(info.stack, "fat12") == 0);
	fsimg_io_free(volume);
	fixture_free(&f);
	free(gpt.bytes);
	free(bare.bytes);
}

static void
write_seed(const char *path, const memory_disk_t *disk)
{
	FILE *file;

	file = fopen(path, "wb");
	CHECK(file != NULL);
	CHECK(fwrite(disk->bytes, 1, (size_t)disk->length, file) == disk->length);
	CHECK(fclose(file) == 0);
}

static void test_force_location_named_fs_automatic(void);

static void
test_force_location_named_fs_automatic(void)
{
	memory_disk_t disk = single_partition_mbr();
	fsimg_image_info_t info;
	fsimg_io_t *volume = NULL;
	fixture_t f;

	fixture_init(&f);
	CHECK(fixture_open(&f, &disk, "fat12", 0, NULL) == FSIMG_OK);
	reopen_assuming_identity(&f, "fat12");
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.scheme, "mbr") == 0);
	CHECK(info.partition_count == 4);
	CHECK(select_with(&f, (fsimg_selector_t){
	    .kind = FSIMG_SELECT_AUTOMATIC },
	    &volume, NULL) == FSIMG_EINVAL);
	CHECK(volume == NULL);
	fixture_free(&f);
	free(disk.bytes);
}

static void test_force_location_named_scheme_automatic(void);

static void
test_force_location_named_scheme_automatic(void)
{
	memory_disk_t disk = single_partition_mbr();
	fsimg_image_info_t info;
	fsimg_io_t *volume = NULL;
	fixture_t f;

	fixture_init(&f);
	CHECK(fixture_open(&f, &disk, "fat12/mbr", 0, NULL) == FSIMG_OK);
	reopen_assuming_identity(&f, "fat12/mbr");
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.scheme, "mbr") == 0);
	CHECK(info.partition_count == 4);
	CHECK(select_with(&f, (fsimg_selector_t){
	    .kind = FSIMG_SELECT_AUTOMATIC },
	    &volume, NULL) == FSIMG_EINVAL);
	CHECK(volume == NULL);
	fixture_free(&f);
	free(disk.bytes);
}

static void test_force_location_empty_scheme_automatic(void);

static void
test_force_location_empty_scheme_automatic(void)
{
	memory_disk_t disk = disk_new(FAT_BYTES);
	fsimg_image_info_t info;
	fsimg_io_t *volume = NULL;
	fixture_t f;

	make_fat12(disk.bytes);
	fixture_init(&f);
	CHECK(fixture_open(&f, &disk, "fat12/-", 0, NULL) == FSIMG_OK);
	reopen_assuming_identity(&f, "fat12/-");
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.scheme, "") == 0);
	CHECK(info.partition_count == 0);
	CHECK(select_with(&f, (fsimg_selector_t){
	    .kind = FSIMG_SELECT_AUTOMATIC },
	    &volume, NULL) == FSIMG_OK);
	expect_volume_lists(&f, volume, "/DIR");
	fixture_free(&f);
	free(disk.bytes);
}

static void test_force_location_named_scheme_index(void);

static void
test_force_location_named_scheme_index(void)
{
	memory_disk_t disk = single_partition_mbr();
	fsimg_image_info_t info;
	fsimg_io_t *volume = NULL;
	fixture_t f;

	fixture_init(&f);
	CHECK(fixture_open(&f, &disk, "fat12/mbr", 0, NULL) == FSIMG_OK);
	reopen_assuming_identity(&f, "fat12/mbr");
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.scheme, "mbr") == 0);
	CHECK(info.partition_count == 4);
	CHECK(select_with(&f, (fsimg_selector_t){
	    .kind = FSIMG_SELECT_INDEX, .index = 1 },
	    &volume, NULL) == FSIMG_OK);
	expect_volume_lists(&f, volume, "/DIR");
	fixture_free(&f);
	free(disk.bytes);
}

static void test_force_location_detected_scheme_index(void);

static void
test_force_location_detected_scheme_index(void)
{
	memory_disk_t disk = single_partition_mbr();
	fsimg_image_info_t info;
	fsimg_io_t *volume = NULL;
	fixture_t f;

	fixture_init(&f);
	CHECK(fixture_open(&f, &disk, "fat12", 0, NULL) == FSIMG_OK);
	reopen_assuming_identity(&f, "fat12");
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.scheme, "mbr") == 0);
	CHECK(info.partition_count == 4);
	CHECK(select_with(&f, (fsimg_selector_t){
	    .kind = FSIMG_SELECT_INDEX, .index = 1 },
	    &volume, NULL) == FSIMG_OK);
	expect_volume_lists(&f, volume, "/DIR");
	fixture_free(&f);
	free(disk.bytes);
}

static void test_force_location_unnamed_fs_automatic(void);

static void
test_force_location_unnamed_fs_automatic(void)
{
	memory_disk_t disk = single_partition_mbr();
	fsimg_image_info_t info;
	fsimg_io_t *volume = NULL;
	fixture_t f;

	/* A scheme-only spelling leaves the filesystem slot unspecified. */
	fixture_init(&f);
	CHECK(fixture_open(&f, &disk, "mbr", 0, NULL) == FSIMG_OK);
	reopen_assuming_identity(&f, "mbr");
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.scheme, "mbr") == 0);
	CHECK(info.partition_count == 4);
	CHECK(select_with(&f, (fsimg_selector_t){
	    .kind = FSIMG_SELECT_AUTOMATIC },
	    &volume, NULL) == FSIMG_OK);
	expect_volume_lists(&f, volume, "/DIR");
	fixture_free(&f);
	free(disk.bytes);
}

static void test_force_location_unforced_automatic(void);

static void
test_force_location_unforced_automatic(void)
{
	memory_disk_t disk = single_partition_mbr();
	fsimg_image_info_t info;
	fsimg_io_t *volume = NULL;
	fixture_t f;

	fixture_init(&f);
	CHECK(fixture_open(&f, &disk, "fat12", 0, NULL) == FSIMG_OK);
	CHECK(fsimg_image_get_info(f.image, &info, NULL) == FSIMG_OK);
	CHECK(strcmp(info.scheme, "mbr") == 0);
	CHECK(info.partition_count == 4);
	CHECK(select_with(&f, (fsimg_selector_t){
	    .kind = FSIMG_SELECT_AUTOMATIC },
	    &volume, NULL) == FSIMG_OK);
	expect_volume_lists(&f, volume, "/DIR");
	fixture_free(&f);
	free(disk.bytes);
}

int
main(int argc, char **argv)
{
	gpt_partition_t seed_parts[] = {
		{ ESP_GUID, name_efi, 34, 63 },
		{ LINUX_GUID, name_root, 64, 94 }
	};
	memory_disk_t seed;

	/* Small disks seed the image fuzzer's corpus. */
	if (argc == 3 && strcmp(argv[1], "--seed-gpt") == 0) {
		seed = disk_new(128 * KIB);
		gpt_build(&seed, 512, seed_parts, 2);
		write_seed(argv[2], &seed);
		free(seed.bytes);
		return (0);
	}
	if (argc == 3 && strcmp(argv[1], "--seed-mbr") == 0) {
		seed = disk_new(128 * KIB);
		mbr_entry(seed.bytes, 0, 0x80, 0x0c, 1, 63);
		mbr_entry(seed.bytes, 1, 0x00, 0x83, 64, 64);
		write_seed(argv[2], &seed);
		free(seed.bytes);
		return (0);
	}
	CHECK(argc == 1);
	test_force_location_named_fs_automatic();
	test_force_location_named_scheme_automatic();
	test_force_location_empty_scheme_automatic();
	test_force_location_named_scheme_index();
	test_force_location_detected_scheme_index();
	test_force_location_unnamed_fs_automatic();
	test_force_location_unforced_automatic();
	test_crc_oracle();
	test_stack_grammar();
	test_stack_rejections();
	test_registry();
	test_gpt_read();
	test_gpt_selectors();
	test_gpt_names();
	test_mbr_truncated_partition_keeps_partial_sector();
	test_mbr_missing_partition_is_listed_but_not_selected();
	test_gpt_truncated_partitions_and_backup_report_missing_bytes();
	test_gpt_failed_primary_stays_corrupt_on_cut_disk();
	test_gpt_primary_array_cuts_are_corrupt();
	test_gpt_declared_geometry_cannot_wrap();
	test_unwrap_requires_a_whole_partition();
	test_extended_mbr_past_end_is_listed_but_unsupported();
	test_gpt_primary_array_end_and_trailing_partial_sector();
	test_gpt_seeded_failures();
	test_gpt_sector_sizes();
	test_mbr_read();
	test_mbr_seeded_failures();
	test_mbr_zero_lba_is_not_a_table();
	test_iso_system_area_scheme();
	test_iso_corrupt_gpt_is_ambiguous();
	test_corrupt_iso_gpt_is_ambiguous();
	test_fat_gpt_is_ambiguous();
	test_mbr_addressable_limit();
	test_bare_volume();
	test_assumed_identity_selects_unvouched_fat();
	test_image_assume_identity_requires_named_slot_or_compression();
	test_forcing();
	test_assumed_mbr_signature_selects_partition_bytes();
	test_assumed_mbr_signature_is_reported_before_verification();
	test_assumed_mbr_empty_table_is_reported_before_verification();
	test_signed_mbr_with_assume_identity_verifies();
	test_assumed_mbr_still_validates_entries();
	test_assume_identity_does_not_change_mbr_detection();
	return (0);
}
