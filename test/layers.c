/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
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
static void test_gpt_sector_sizes(void);
static void test_mbr_read(void);
static void test_mbr_seeded_failures(void);
static void test_mbr_addressable_limit(void);
static void test_bare_volume(void);
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
		{ "fat32//dmg", "the stack has an empty token", "fat32//dmg" },
		{ "fat32/", "the stack has an empty token", "fat32/" },
		{ "fat32/-/-/-", "the stack has too many tokens", "fat32/-/-/-" },
		{ "", "the stack is empty", "" }
	};
	fsimg_stack_t stack;
	fsimg_error_t error;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		CHECK(fsimg_stack_parse(cases[i].text, &stack, &error) == FSIMG_EINVAL);
		CHECK(strcmp(error.message, cases[i].message) == 0);
		CHECK(strcmp(error.path, cases[i].named) == 0);
	}
}

/* The registry lists every layer once, with the suffixes the command
 * strips, the capabilities of the schemes alone and the keys of each
 * layer's settings in table order.
 */
static void
test_registry(void)
{
	static const unsigned scheme = FSIMG_CAP_READ | FSIMG_CAP_WRITE |
	    FSIMG_CAP_VERIFY;
	static const unsigned container = FSIMG_CAP_READ | FSIMG_CAP_VERIFY;
	static const struct {
		const char	*name;
		const char	*suffix;
		const char	*settings;
		const char	*codecs;
		fsimg_layer_kind_t kind;
		unsigned	 capabilities;
	} expected[] = {
		{ "gpt", "", "sector,disk,type,name,disk-guid,guid", "",
		    FSIMG_LAYER_SCHEME, scheme },
		{ "mbr", "", "disk,type,active,id", "", FSIMG_LAYER_SCHEME,
		    scheme },
		{ "dmg", ".dmg", "", "raw,zero,zlib", FSIMG_LAYER_CONTAINER,
		    container },
		{ "vhd", ".vhd", "", "raw", FSIMG_LAYER_CONTAINER, container },
		{ "simg", ".simg", "", "raw,zero,fill", FSIMG_LAYER_CONTAINER,
		    container },
		{ "gzip", ".gz", "", "", FSIMG_LAYER_COMPRESSION, 0 }
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
		/* Only a layer whose chunks use zlib names its version. */
		CHECK((strstr(info.codecs, "zlib") != NULL) ==
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
	expect_gpt_corrupt(&disk, "GPT primary alternate LBA is not the disk's last LBA");
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
	free(disk.bytes);

	disk = disk_new(4 * MIB);
	gpt_build(&disk, 512, outside, 1);
	expect_gpt_corrupt(&disk, "GPT partition 1 lies outside the usable range");
	free(disk.bytes);

	/* A truncated disk has lost its backup at the recorded last LBA. */
	disk = two_partition_gpt(512);
	disk.length -= 512;
	expect_gpt_corrupt(&disk, "GPT primary alternate LBA is not the disk's last LBA");
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
		{ { 2048, 0 }, { 8192, 0 }, "MBR partition 1 extends beyond the disk" },
		{ { 0, 0 }, { 100, 0 }, "MBR partition 1 overlaps the master boot record" }
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
	test_crc_oracle();
	test_stack_grammar();
	test_stack_rejections();
	test_registry();
	test_gpt_read();
	test_gpt_selectors();
	test_gpt_names();
	test_gpt_seeded_failures();
	test_gpt_sector_sizes();
	test_mbr_read();
	test_mbr_seeded_failures();
	test_mbr_addressable_limit();
	test_bare_volume();
	test_forcing();
	return (0);
}
