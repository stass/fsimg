/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "fsimg.h"
#include "internal.h"
#include "layer.h"
#include "settings.h"
#include "test.h"

/*
 * The settings catalogue against independently written expectations: the
 * public records' members, masks and capability keys spelled out here
 * rather than derived from the rows under test, and the library's typed
 * default, presence and applicability rules exercised through the same
 * checks the entry points run.
 */

/* One public member the catalogue must bind, by offset and size; align
 * is its declared type's alignment where a list uses it, else 1.
 */
typedef struct member {
	const char	*name;
	size_t		 offset;
	size_t		 size;
	size_t		 align;
} member_t;

#define LAYER_MEMBER(m)	{ #m, offsetof(fsimg_layer_options_t, m),	\
	sizeof(((fsimg_layer_options_t *)NULL)->m), 1 }
/* A write options member and its type as fsimg.h declares it. */
#define WRITE_MEMBER(m, type)	{ #m, offsetof(fsimg_write_options_t, m),	\
	sizeof(((fsimg_write_options_t *)NULL)->m), _Alignof(type) }
#define INPUT_MEMBER(m)	{ #m, offsetof(fsimg_layer_inputs_t, m),	\
	sizeof(((fsimg_layer_inputs_t *)NULL)->m), 1 }

/* Every member of fsimg_layer_options_t but its reserved words. */
static const member_t layer_members[] = {
	LAYER_MEMBER(gpt_sector_bytes), LAYER_MEMBER(gpt_disk_bytes),
	LAYER_MEMBER(mbr_disk_bytes), LAYER_MEMBER(gpt_type),
	LAYER_MEMBER(gpt_name), LAYER_MEMBER(mbr_type),
	LAYER_MEMBER(gpt_disk_guid), LAYER_MEMBER(gpt_guid),
	LAYER_MEMBER(mbr_id), LAYER_MEMBER(mbr_id_set),
	LAYER_MEMBER(mbr_active), LAYER_MEMBER(dmg_compress),
	LAYER_MEMBER(dmg_level), LAYER_MEMBER(dmg_chunk_bytes),
	LAYER_MEMBER(dmg_uuid), LAYER_MEMBER(vhd_align_bytes),
	LAYER_MEMBER(vhd_uuid), LAYER_MEMBER(simg_block_bytes),
	LAYER_MEMBER(simg_crc), LAYER_MEMBER(gzip_level),
	LAYER_MEMBER(ifsboot_compress), LAYER_MEMBER(ifsboot_machine),
	LAYER_MEMBER(ifsboot_startup_vaddr), LAYER_MEMBER(ifsboot_paddr_bias),
	LAYER_MEMBER(ifsboot_image_paddr), LAYER_MEMBER(ifsboot_ram_paddr),
	LAYER_MEMBER(ifsboot_imagefs_paddr), LAYER_MEMBER(ifsboot_virtual),
	LAYER_MEMBER(ifsboot_machine_set), LAYER_MEMBER(ifsboot_virtual_set),
	LAYER_MEMBER(ifsboot_startup_vaddr_set),
	LAYER_MEMBER(ifsboot_paddr_bias_set),
	LAYER_MEMBER(ifsboot_image_paddr_set),
	LAYER_MEMBER(ifsboot_ram_paddr_set),
	LAYER_MEMBER(ifsboot_imagefs_paddr_set),
	LAYER_MEMBER(xz_level), LAYER_MEMBER(xz_check), LAYER_MEMBER(zstd_level)
};

/* The filesystem settings of fsimg_write_options_t. */
static const member_t write_members[] = {
	WRITE_MEMBER(sector_bytes, uint32_t),
	WRITE_MEMBER(allocation_unit_bytes, uint32_t),
	WRITE_MEMBER(byte_order, fsimg_byte_order_t),
	WRITE_MEMBER(volume_uuid, unsigned char),
	WRITE_MEMBER(inode_capacity, uint32_t),
	WRITE_MEMBER(reserve_percent, uint32_t),
	WRITE_MEMBER(mountpoint, const char *),
	WRITE_MEMBER(fat_root_entries, uint32_t)
};

static const member_t input_members[] = {
	INPUT_MEMBER(ifsboot_startup), INPUT_MEMBER(ifsboot_prefix)
};

/* What each registered layer advertises, in presentation order. */
static const struct {
	const char	*layer;
	const char	*keys;
} advertised[] = {
	{ "gpt", "sector,disk,type,name,disk-guid,guid" },
	{ "mbr", "disk,type,active,id" },
	{ "dmg", "compress,level,chunk,uuid" },
	{ "vhd", "align,uuid" },
	{ "simg", "block,crc" },
	{ "ifsboot", "startup,prefix,machine,virtual,startup_vaddr,paddr_bias,"
	    "image_paddr,ram_paddr,imagefs_paddr,compress" },
	{ "hbcifs", "" },
	{ "gzip", "level" },
	{ "zstd", "level" },
	{ "xz", "level,check" }
};

static void test_slices(void);
static void mark_member(unsigned char *, size_t, size_t, size_t);
static void test_member_bindings(void);
static void check_members(const unsigned char *, size_t, const member_t *,
    size_t, const char *);
static void test_masks_and_files(void);
static void test_rows(void);
static void test_capability_keys(void);
static void test_defaults(void);
static void test_default_distinctions(void);
static void test_structure(void);
static void test_presence(void);
static void test_store(void);
static void test_write_applicability(void);
static void format_info(const char *, fsimg_format_info_t *);
static void test_layer_applicability(void);
static fsimg_status_t check_layers(const char *, const char *,
    const fsimg_layer_options_t *, const fsimg_layer_inputs_t *, bool,
    fsimg_error_t *);
static void test_file_settings(void);
static void test_bool_bytes(void);
static void test_write_member_coverage(void);

/* Every slice belongs to the one layer naming it, keys are unique, and a
 * layer's file settings come first.
 */
static void
test_slices(void)
{
	const fsimg_setting_slice_t *slice;
	const fsimg_setting_t *row;
	const fsimg_layer_t *layer;
	bool scalars;

	slice = &fsimg_filesystem_settings;
	for (size_t i = 0; i < slice->count; i++) {
		row = &slice->rows[i];
		CHECK(row->owner == NULL);
		CHECK(row->record != FSIMG_RECORD_INPUTS);
		CHECK(fsimg_setting_find(slice, row->key, strlen(row->key)) ==
		    row);
	}
	for (size_t i = 0; (layer = fsimg_layer_at(i)) != NULL; i++) {
		slice = layer->settings;
		if (slice == NULL)
			continue;
		CHECK(slice->count != 0);
		scalars = false;
		for (size_t j = 0; j < slice->count; j++) {
			row = &slice->rows[j];
			CHECK(row->owner != NULL &&
			    strcmp(row->owner, layer->name) == 0);
			CHECK(fsimg_setting_find(slice, row->key,
			    strlen(row->key)) == row);
			if (row->record != FSIMG_RECORD_INPUTS)
				scalars = true;
			else
				CHECK(!scalars);
		}
		/* No other layer references the same slice. */
		for (size_t k = i + 1; fsimg_layer_at(k) != NULL; k++)
			CHECK(fsimg_layer_at(k)->settings != slice);
	}
	CHECK(fsimg_layer_find("gpt")->settings == &fsimg_gpt_settings);
	CHECK(fsimg_layer_find("mbr")->settings == &fsimg_mbr_settings);
	CHECK(fsimg_layer_find("dmg")->settings == &fsimg_dmg_settings);
	CHECK(fsimg_layer_find("vhd")->settings == &fsimg_vhd_settings);
	CHECK(fsimg_layer_find("simg")->settings == &fsimg_simg_settings);
	CHECK(fsimg_layer_find("ifsboot")->settings == &fsimg_ifsboot_settings);
	CHECK(fsimg_layer_find("gzip")->settings == &fsimg_gzip_settings);
	CHECK(fsimg_layer_find("hbcifs")->settings == NULL);
}

/* Mark size bytes at offset, refusing a byte already marked. */
static void
mark_member(unsigned char *coverage, size_t capacity, size_t offset,
    size_t size)
{
	CHECK(offset <= capacity && size <= capacity - offset);
	for (size_t i = offset; i < offset + size; i++) {
		CHECK(coverage[i] == 0);
		coverage[i] = 1;
	}
}

/*
 * Rows bind disjoint members, a presence flag counting as its own member,
 * and together they cover exactly the members listed above: every layer
 * member but the reserved words, the filesystem settings of the write
 * options and both file settings.
 */
static void
test_member_bindings(void)
{
	unsigned char layers[sizeof(fsimg_layer_options_t)];
	unsigned char write[sizeof(fsimg_write_options_t)];
	unsigned char inputs[sizeof(fsimg_layer_inputs_t)];
	const fsimg_setting_slice_t *slices[16];
	const fsimg_setting_t *row;
	const fsimg_layer_t *layer;
	unsigned char *coverage;
	size_t count, capacity;

	bzero(layers, sizeof(layers));
	bzero(write, sizeof(write));
	bzero(inputs, sizeof(inputs));
	count = 0;
	slices[count++] = &fsimg_filesystem_settings;
	for (size_t i = 0; (layer = fsimg_layer_at(i)) != NULL; i++) {
		if (layer->settings != NULL)
			slices[count++] = layer->settings;
	}
	for (size_t i = 0; i < count; i++) {
		for (size_t j = 0; j < slices[i]->count; j++) {
			row = &slices[i]->rows[j];
			switch (row->record) {
			case FSIMG_RECORD_LAYERS:
				coverage = layers;
				capacity = sizeof(layers);
				break;
			case FSIMG_RECORD_WRITE:
				coverage = write;
				capacity = sizeof(write);
				break;
			case FSIMG_RECORD_INPUTS:
				coverage = inputs;
				capacity = sizeof(inputs);
				break;
			default:
				CHECK(row->kind == FSIMG_KIND_NONE &&
				    row->presence == FSIMG_NO_PRESENCE);
				continue;
			}
			mark_member(coverage, capacity, row->offset, row->size);
			if (row->presence != FSIMG_NO_PRESENCE) {
				CHECK(row->record == FSIMG_RECORD_LAYERS);
				mark_member(coverage, capacity, row->presence,
				    sizeof(bool));
			}
		}
	}
	check_members(layers, sizeof(layers), layer_members,
	    sizeof(layer_members) / sizeof(layer_members[0]), "layer");
	check_members(write, sizeof(write), write_members,
	    sizeof(write_members) / sizeof(write_members[0]), "write");
	check_members(inputs, sizeof(inputs), input_members,
	    sizeof(input_members) / sizeof(input_members[0]), "inputs");
}

/* Each listed member wholly covered, and nothing covered outside them. */
static void
check_members(const unsigned char *coverage, size_t capacity,
    const member_t *members, size_t count, const char *record)
{
	unsigned char expected[sizeof(fsimg_write_options_t) +
	    sizeof(fsimg_layer_options_t)];

	CHECK(capacity <= sizeof(expected));
	bzero(expected, sizeof(expected));
	for (size_t i = 0; i < count; i++)
		memset(expected + members[i].offset, 1, members[i].size);
	for (size_t i = 0; i < capacity; i++) {
		if (coverage[i] != expected[i])
			fprintf(stderr, "%s byte %zu: catalogue %u, members %u\n",
			    record, i, coverage[i], expected[i]);
		CHECK(coverage[i] == expected[i]);
	}
}

/* Each public FSIMG_SETTING_* bit names exactly one filesystem row, and
 * each file setting identity exactly one row of the expected key.
 */
static void
test_masks_and_files(void)
{
	static const uint32_t bits[] = { FSIMG_SETTING_BYTE_ORDER,
	    FSIMG_SETTING_VOLUME_UUID, FSIMG_SETTING_INODE_CAPACITY,
	    FSIMG_SETTING_RESERVE, FSIMG_SETTING_MOUNTPOINT, FSIMG_SETTING_ROOT_ENTRIES };
	static const char *const bit_keys[] = { "endian", "uuid", "inodes",
	    "reserve", "mountpoint", "root" };
	static const char *const file_keys[FSIMG_FILE_SETTINGS] = {
		[FSIMG_FILE_IFSBOOT_STARTUP] = "ifsboot:startup",
		[FSIMG_FILE_IFSBOOT_PREFIX] = "ifsboot:prefix"
	};
	const fsimg_setting_slice_t *slice = &fsimg_filesystem_settings;
	const fsimg_setting_t *row;
	fsimg_format_info_t info;
	size_t cursor;
	uint32_t covered;
	unsigned found;
	char key[32];

	for (size_t b = 0; b < sizeof(bits) / sizeof(bits[0]); b++) {
		found = 0;
		for (size_t i = 0; i < slice->count; i++) {
			if (slice->rows[i].support != bits[b])
				continue;
			found++;
			CHECK(strcmp(slice->rows[i].key, bit_keys[b]) == 0);
		}
		CHECK(found == 1);
	}
	for (size_t i = 0; i < slice->count; i++) {
		row = &slice->rows[i];
		CHECK(row->support == 0 || (row->support & (row->support - 1)) == 0);
	}
	/* Every bit a registered format declares has its row. */
	covered = 0;
	for (size_t i = 0; i < slice->count; i++)
		covered |= slice->rows[i].support;
	cursor = 0;
	while (fsimg_format_next(&cursor, &info, NULL) == FSIMG_OK)
		CHECK((info.write_settings & ~covered) == 0);
	for (unsigned f = 0; f < FSIMG_FILE_SETTINGS; f++) {
		fsimg_file_setting_name(f, key, sizeof(key));
		CHECK(strcmp(key, file_keys[f]) == 0);
	}
	found = 0;
	for (size_t i = 0; i < fsimg_ifsboot_settings.count; i++) {
		if (fsimg_ifsboot_settings.rows[i].record == FSIMG_RECORD_INPUTS)
			found++;
	}
	CHECK(found == FSIMG_FILE_SETTINGS);
}

/* Each row's own facts agree: choices, ranges, kinds and syntax. */
static void
test_rows(void)
{
	const fsimg_setting_slice_t *slices[16];
	const fsimg_setting_choice_t *choice, *other;
	const fsimg_setting_t *row;
	const fsimg_layer_t *layer;
	size_t count;

	count = 0;
	slices[count++] = &fsimg_filesystem_settings;
	for (size_t i = 0; (layer = fsimg_layer_at(i)) != NULL; i++) {
		if (layer->settings != NULL)
			slices[count++] = layer->settings;
	}
	for (size_t i = 0; i < count; i++) {
		for (size_t j = 0; j < slices[i]->count; j++) {
			row = &slices[i]->rows[j];
			CHECK((row->scope & (FSIMG_SCOPE_READ |
			    FSIMG_SCOPE_WRITE)) != 0);
			/* Error paths render keys into 32-byte buffers. */
			CHECK((row->owner == NULL ? 0 : strlen(row->owner) + 1) +
			    strlen(row->key) < 32);
			/* Texts compare by emptiness, so their default is empty. */
			CHECK(row->kind != FSIMG_KIND_TEXT ||
			    row->initial.text == NULL ||
			    row->initial.text[0] == '\0');
			switch (row->syntax) {
			case FSIMG_SYNTAX_BYTES:
			case FSIMG_SYNTAX_COUNT:
				CHECK(row->kind == FSIMG_KIND_U32 ||
				    row->kind == FSIMG_KIND_U64);
				CHECK(row->max != 0 && (row->kind ==
				    FSIMG_KIND_U64 || row->max <= UINT32_MAX));
				break;
			case FSIMG_SYNTAX_HEX32:
				CHECK(row->kind == FSIMG_KIND_U32);
				break;
			case FSIMG_SYNTAX_BOOL:
				CHECK(row->kind == FSIMG_KIND_BOOL);
				break;
			case FSIMG_SYNTAX_CHOICE:
				CHECK(row->choices != NULL &&
				    row->choices[0].spelling != NULL);
				/* An enum's default is one of its values. */
				CHECK(row->kind == FSIMG_KIND_NONE ||
				    fsimg_setting_choice_of(row,
				    &row->initial) != NULL);
				break;
			case FSIMG_SYNTAX_UUID:
				CHECK(row->kind == FSIMG_KIND_ID16);
				break;
			case FSIMG_SYNTAX_TEXT:
				CHECK(row->kind == FSIMG_KIND_TEXT ||
				    (row->kind == FSIMG_KIND_TEXT_POINTER &&
				    row->text_bytes != 0));
				break;
			case FSIMG_SYNTAX_PATH:
				CHECK(row->kind == FSIMG_KIND_IO &&
				    row->record == FSIMG_RECORD_INPUTS);
				break;
			}
			for (choice = row->choices; choice != NULL &&
			    choice->spelling != NULL; choice++) {
				for (other = choice + 1; other->spelling != NULL;
				    other++)
					CHECK(strcmp(choice->spelling,
					    other->spelling) != 0);
			}
		}
	}
}

/* The capability records list the keys written above, untruncated. */
static void
test_capability_keys(void)
{
	fsimg_layer_info_t info;
	size_t cursor, index;

	cursor = 0;
	index = 0;
	while (fsimg_layer_next(&cursor, &info, NULL) == FSIMG_OK) {
		CHECK(index < sizeof(advertised) / sizeof(advertised[0]));
		CHECK(strcmp(info.name, advertised[index].layer) == 0);
		if (strcmp(info.settings, advertised[index].keys) != 0)
			fprintf(stderr, "%s: \"%s\"\n", info.name, info.settings);
		CHECK(strcmp(info.settings, advertised[index].keys) == 0);
		CHECK(strlen(advertised[index].keys) < sizeof(info.settings));
		index++;
	}
	CHECK(index == sizeof(advertised) / sizeof(advertised[0]));
}

/* The initializers store the documented defaults, which the default tests
 * then report as absent.
 */
static void
test_defaults(void)
{
	static const unsigned char nil[16];
	fsimg_write_options_t write;
	fsimg_layer_options_t layers;
	fsimg_layer_inputs_t inputs;
	fsimg_setting_records_t records;
	const fsimg_layer_t *layer;

	fsimg_write_options_init(&write);
	CHECK(write.reserve_percent == 3 &&
	    write.byte_order == FSIMG_BYTE_ORDER_LITTLE &&
	    write.mountpoint == NULL && write.inode_capacity == 0 &&
	    write.sector_bytes == 0 && write.allocation_unit_bytes == 0 &&
	    bcmp(write.volume_uuid, nil, sizeof(nil)) == 0);
	fsimg_layer_options_init(&layers);
	CHECK(layers.dmg_compress == FSIMG_DMG_ZLIB &&
	    layers.ifsboot_compress == FSIMG_IFSBOOT_LZO);
	CHECK(!layers.mbr_id_set && !layers.ifsboot_virtual_set &&
	    !layers.mbr_active && layers.gpt_type[0] == '\0');
	bzero(&inputs, sizeof(inputs));
	inputs.ifsboot_startup = NULL;
	inputs.ifsboot_prefix = NULL;
	records = (fsimg_setting_records_t){ .write = &write,
	    .layers = &layers, .inputs = &inputs };
	for (size_t i = 0; i < fsimg_filesystem_settings.count; i++) {
		CHECK(fsimg_setting_is_default(&fsimg_filesystem_settings.rows[i],
		    &records));
		CHECK(fsimg_setting_check(&fsimg_filesystem_settings.rows[i],
		    &records, NULL) == FSIMG_OK);
	}
	for (size_t i = 0; (layer = fsimg_layer_at(i)) != NULL; i++) {
		for (size_t j = 0; layer->settings != NULL &&
		    j < layer->settings->count; j++) {
			CHECK(fsimg_setting_is_default(&layer->settings->rows[j],
			    &records));
			CHECK(fsimg_setting_check(&layer->settings->rows[j],
			    &records, NULL) == FSIMG_OK);
		}
	}
	/* An absent record holds every default. */
	records = (fsimg_setting_records_t){ 0 };
	CHECK(fsimg_setting_is_default(fsimg_setting_find(
	    &fsimg_filesystem_settings, "reserve", 7), &records));
}

/* Defaults are typed values: 3 for the reserve, NULL (not "") for the
 * mountpoint, sixteen zero bytes for the UUID, an empty text.
 */
static void
test_default_distinctions(void)
{
	const fsimg_setting_slice_t *fs = &fsimg_filesystem_settings;
	const fsimg_setting_t *reserve, *mountpoint, *uuid, *type, *root;
	fsimg_setting_records_t records;
	fsimg_write_options_t write;
	fsimg_layer_options_t layers;

	root = fsimg_setting_find(fs, "root", 4);
	CHECK(root != NULL && root->support == FSIMG_SETTING_ROOT_ENTRIES);
	reserve = fsimg_setting_find(fs, "reserve", 7);
	mountpoint = fsimg_setting_find(fs, "mountpoint", 10);
	uuid = fsimg_setting_find(fs, "uuid", 4);
	type = fsimg_setting_find(&fsimg_gpt_settings, "type", 4);
	CHECK(reserve != NULL && mountpoint != NULL && uuid != NULL &&
	    type != NULL);
	fsimg_write_options_init(&write);
	fsimg_layer_options_init(&layers);
	records = (fsimg_setting_records_t){ .write = &write,
	    .layers = &layers };
	CHECK(write.fat_root_entries == 0);
	CHECK(fsimg_setting_is_default(root, &records));
	write.fat_root_entries = 16;
	CHECK(!fsimg_setting_is_default(root, &records));
	write.fat_root_entries = 0;
	write.reserve_percent = 0;
	CHECK(!fsimg_setting_is_default(reserve, &records));
	write.reserve_percent = 3;
	CHECK(fsimg_setting_is_default(reserve, &records));
	write.mountpoint = "";
	CHECK(!fsimg_setting_is_default(mountpoint, &records));
	write.mountpoint = NULL;
	CHECK(fsimg_setting_is_default(mountpoint, &records));
	write.volume_uuid[7] = 1;
	CHECK(!fsimg_setting_is_default(uuid, &records));
	write.volume_uuid[7] = 0;
	CHECK(fsimg_setting_is_default(uuid, &records));
	layers.gpt_type[0] = 'x';
	CHECK(!fsimg_setting_is_default(type, &records));
}

/* Structural checks: termination within capacity and enum membership,
 * each naming the canonical key.
 */
static void
test_structure(void)
{
	const fsimg_setting_t *type, *compress, *boot;
	fsimg_setting_records_t records;
	fsimg_layer_options_t layers;
	fsimg_write_options_t write;
	fsimg_error_t error;

	type = fsimg_setting_find(&fsimg_gpt_settings, "type", 4);
	compress = fsimg_setting_find(&fsimg_dmg_settings, "compress", 8);
	boot = fsimg_setting_find(&fsimg_ifsboot_settings, "compress", 8);
	fsimg_layer_options_init(&layers);
	records = (fsimg_setting_records_t){ .layers = &layers };
	memset(layers.gpt_type, 'e', sizeof(layers.gpt_type));
	CHECK(fsimg_setting_check(type, &records, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "gpt:type") == 0);
	layers.gpt_type[sizeof(layers.gpt_type) - 1] = '\0';
	CHECK(fsimg_setting_check(type, &records, NULL) == FSIMG_OK);
	layers.dmg_compress = (fsimg_dmg_compression_t)2;
	CHECK(fsimg_setting_check(compress, &records, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "dmg:compress") == 0 &&
	    strstr(error.message, "zlib or none") != NULL);
	layers.dmg_compress = FSIMG_DMG_NONE;
	CHECK(fsimg_setting_check(compress, &records, NULL) == FSIMG_OK);
	layers.ifsboot_compress = (fsimg_ifsboot_compression_t)7;
	CHECK(fsimg_setting_check(boot, &records, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "ifsboot:compress") == 0 &&
	    strstr(error.message, "lzo or none") != NULL);
	fsimg_write_options_init(&write);
	write.byte_order = (fsimg_byte_order_t)2;
	CHECK(fsimg_write_settings_check(&write, NULL, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "endian") == 0);
}

/*
 * A presence flag decides: clear, the value must be its inactive default
 * and the setting is absent; set, the setting is supplied even when its
 * value is zero or false.
 */
static void
test_presence(void)
{
	const fsimg_setting_t *id, *machine, *virtual_bit;
	fsimg_setting_records_t records;
	fsimg_layer_options_t layers;
	fsimg_error_t error;

	id = fsimg_setting_find(&fsimg_mbr_settings, "id", 2);
	machine = fsimg_setting_find(&fsimg_ifsboot_settings, "machine", 7);
	virtual_bit = fsimg_setting_find(&fsimg_ifsboot_settings, "virtual", 7);
	fsimg_layer_options_init(&layers);
	records = (fsimg_setting_records_t){ .layers = &layers };

	layers.mbr_id = 7;
	CHECK(fsimg_setting_check(id, &records, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "mbr:id") == 0);
	layers.mbr_id = 0;
	CHECK(fsimg_setting_is_default(id, &records));
	layers.mbr_id_set = true;
	CHECK(!fsimg_setting_is_default(id, &records));
	CHECK(fsimg_setting_check(id, &records, NULL) == FSIMG_OK);

	layers.ifsboot_machine_set = true;
	CHECK(!fsimg_setting_is_default(machine, &records));

	layers.ifsboot_virtual = true;
	CHECK(fsimg_setting_check(virtual_bit, &records, &error) ==
	    FSIMG_EINVAL);
	CHECK(strcmp(error.path, "ifsboot:virtual") == 0);
	layers.ifsboot_virtual = false;
	CHECK(fsimg_setting_is_default(virtual_bit, &records));
	layers.ifsboot_virtual_set = true;
	CHECK(!fsimg_setting_is_default(virtual_bit, &records));
	CHECK(fsimg_setting_check(virtual_bit, &records, NULL) == FSIMG_OK);
}

/* A store commits the value with its flag, and defines a text's tail. */
static void
test_store(void)
{
	const fsimg_setting_t *id, *type, *mountpoint;
	fsimg_setting_records_t records;
	fsimg_setting_targets_t targets;
	fsimg_setting_value_t value;
	fsimg_write_options_t write;
	fsimg_layer_options_t layers;

	id = fsimg_setting_find(&fsimg_mbr_settings, "id", 2);
	type = fsimg_setting_find(&fsimg_gpt_settings, "type", 4);
	mountpoint = fsimg_setting_find(&fsimg_filesystem_settings,
	    "mountpoint", 10);
	fsimg_layer_options_init(&layers);
	fsimg_write_options_init(&write);
	targets = (fsimg_setting_targets_t){ .write = &write,
	    .layers = &layers };
	records = (fsimg_setting_records_t){ .layers = &layers };

	value = (fsimg_setting_value_t){ .u32 = 0 };
	fsimg_setting_store(id, &targets, &value);
	CHECK(layers.mbr_id_set && layers.mbr_id == 0);

	memset(layers.gpt_type, 'x', sizeof(layers.gpt_type));
	value = (fsimg_setting_value_t){ .text = "esp" };
	fsimg_setting_store(type, &targets, &value);
	CHECK(strcmp(layers.gpt_type, "esp") == 0);
	for (size_t i = 3; i < sizeof(layers.gpt_type); i++)
		CHECK(layers.gpt_type[i] == '\0');

	/* A loaded text borrows the field: storing it back, including an
	 * overlapping suffix, preserves the text and initializes the tail.
	 */
	fsimg_setting_load(type, &records, &value);
	fsimg_setting_store(type, &targets, &value);
	CHECK(strcmp(layers.gpt_type, "esp") == 0);
	value.text++;
	fsimg_setting_store(type, &targets, &value);
	CHECK(strcmp(layers.gpt_type, "sp") == 0);
	for (size_t i = 2; i < sizeof(layers.gpt_type); i++)
		CHECK(layers.gpt_type[i] == '\0');

	value = (fsimg_setting_value_t){ .text = "" };
	fsimg_setting_store(mountpoint, &targets, &value);
	CHECK(write.mountpoint == value.text);
}

/* The capability record of a writable format. */
static void
format_info(const char *name, fsimg_format_info_t *out)
{
	size_t cursor;

	cursor = 0;
	while (fsimg_format_next(&cursor, out, NULL) == FSIMG_OK) {
		if (strcmp(out->name, name) == 0)
			return;
	}
	CHECK(!"format registered");
}

/*
 * A format that lacks a setting's support bit accepts only its default:
 * FAT takes reserve=3 and refuses reserve=0, while QNX6 takes either.
 * Geometry has no bit and every writer accepts it. Without a writer every
 * setting must hold its default.
 */
static void
test_write_applicability(void)
{
	fsimg_format_info_t fat, qnx6, ifs;
	fsimg_write_options_t write;
	fsimg_error_t error;

	format_info("fat32", &fat);
	format_info("qnx6", &qnx6);
	format_info("ifs", &ifs);
	fsimg_write_options_init(&write);
	CHECK(fsimg_write_settings_check(&write, &fat, NULL) == FSIMG_OK);
	CHECK(fsimg_write_settings_check(&write, NULL, NULL) == FSIMG_OK);
	write.reserve_percent = 0;
	CHECK(fsimg_write_settings_check(&write, &fat, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "reserve") == 0 &&
	    error.operation == FSIMG_OP_WRITER);
	CHECK(fsimg_write_settings_check(&write, &qnx6, NULL) == FSIMG_OK);
	fsimg_write_options_init(&write);
	write.mountpoint = "";
	CHECK(fsimg_write_settings_check(&write, &fat, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "mountpoint") == 0);
	CHECK(fsimg_write_settings_check(&write, &ifs, NULL) == FSIMG_OK);
	fsimg_write_options_init(&write);
	write.sector_bytes = 512;
	CHECK(fsimg_write_settings_check(&write, &fat, NULL) == FSIMG_OK);
	CHECK(fsimg_write_settings_check(&write, NULL, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "sector") == 0 &&
	    error.operation == FSIMG_OP_IMAGE);
	/* The first failing row in catalogue order is reported. */
	fsimg_write_options_init(&write);
	write.inode_capacity = 10;
	write.byte_order = FSIMG_BYTE_ORDER_BIG;
	CHECK(fsimg_write_settings_check(&write, &fat, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "endian") == 0);
}

static void
test_root_setting_applicability(void)
{
	static const char *const formats[] = {
		"fat12", "fat16", "fat32", "exfat", "qnx4", "qnx6", "ifs"
	};
	fsimg_format_info_t info;
	fsimg_write_options_t write;
	fsimg_error_t error;

	CHECK(FSIMG_SETTING_ROOT_ENTRIES == 0x20u);
	for (size_t f = 0; f < sizeof(formats) / sizeof(formats[0]); f++) {
		format_info(formats[f], &info);
		fsimg_write_options_init(&write);
		CHECK(fsimg_write_settings_check(&write, &info, NULL) == FSIMG_OK);
		write.fat_root_entries = 16;
		if (f < 2) {
			CHECK((info.write_settings & FSIMG_SETTING_ROOT_ENTRIES) != 0);
			CHECK(fsimg_write_settings_check(&write, &info, NULL) == FSIMG_OK);
		} else {
			CHECK((info.write_settings & FSIMG_SETTING_ROOT_ENTRIES) == 0);
			CHECK(fsimg_write_settings_check(&write, &info, &error) == FSIMG_EINVAL);
			CHECK(strcmp(error.path, "root") == 0);
			CHECK(strcmp(error.message, "write setting does not apply to the image format") == 0);
		}
	}
	CHECK(fsimg_write_settings_check(&write, NULL, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "root") == 0);
	CHECK(strcmp(error.message, "filesystem settings must hold their defaults without a filesystem writer") == 0);
}

static fsimg_status_t
check_layers(const char *text, const char *compression,
    const fsimg_layer_options_t *layers, const fsimg_layer_inputs_t *inputs,
    bool writing, fsimg_error_t *error)
{
	fsimg_stack_t stack;

	CHECK(fsimg_stack_parse(text, &stack, NULL) == FSIMG_OK);
	return (fsimg_layer_options_check(&stack, compression == NULL ? NULL :
	    fsimg_layer_find(compression), layers, inputs, writing, error));
}

/*
 * A layer setting away from its default needs its layer selected, and on
 * read a read-side row; a file setting follows the same rules. Compose
 * refuses partition settings.
 */
static void
test_layer_applicability(void)
{
	fsimg_layer_options_t layers;
	fsimg_layer_inputs_t inputs;
	fsimg_error_t error;
	int object;

	fsimg_layer_options_init(&layers);
	snprintf(layers.gpt_type, sizeof(layers.gpt_type), "esp");
	CHECK(check_layers("fat32/gpt", NULL, &layers, NULL, true, NULL) ==
	    FSIMG_OK);
	CHECK(check_layers("fat32/mbr", NULL, &layers, NULL, true, &error) ==
	    FSIMG_EINVAL);
	CHECK(strcmp(error.path, "gpt:type") == 0 &&
	    strstr(error.message, "does not contain") != NULL);
	CHECK(check_layers("fat32/gpt", NULL, &layers, NULL, false, &error) ==
	    FSIMG_EINVAL);
	CHECK(strcmp(error.path, "gpt:type") == 0 &&
	    strstr(error.message, "only to creation") != NULL);
	CHECK(fsimg_layer_options_disk_only(&layers, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "gpt:type") == 0);

	fsimg_layer_options_init(&layers);
	layers.gpt_sector_bytes = 4096;
	layers.gpt_disk_bytes = 1 << 20;
	CHECK(fsimg_layer_options_disk_only(&layers, NULL) == FSIMG_OK);
	layers.gpt_disk_bytes = 0;
	CHECK(check_layers("gpt", NULL, &layers, NULL, false, NULL) ==
	    FSIMG_OK);
	CHECK(check_layers("fat32", NULL, &layers, NULL, false, &error) ==
	    FSIMG_EINVAL);
	CHECK(strcmp(error.path, "gpt:sector") == 0);

	/* A compression's setting needs the operation's compression. */
	fsimg_layer_options_init(&layers);
	layers.gzip_level = 9;
	CHECK(check_layers("fat32", NULL, &layers, NULL, true, &error) ==
	    FSIMG_EINVAL);
	CHECK(strcmp(error.path, "gzip:level") == 0);
	CHECK(check_layers("fat32", "gzip", &layers, NULL, true, NULL) ==
	    FSIMG_OK);

	/* A file setting is present when its object is; its pointer is only
	 * compared with NULL here.
	 */
	fsimg_layer_options_init(&layers);
	bzero(&inputs, sizeof(inputs));
	inputs.ifsboot_prefix = NULL;
	inputs.ifsboot_startup = (fsimg_io_t *)(void *)&object;
	CHECK(check_layers("ifs/ifsboot", NULL, &layers, &inputs, true,
	    NULL) == FSIMG_OK);
	CHECK(check_layers("ifs", NULL, &layers, &inputs, true, &error) ==
	    FSIMG_EINVAL);
	CHECK(strcmp(error.path, "ifsboot:startup") == 0);
	CHECK(check_layers("ifs/ifsboot", NULL, &layers, &inputs, false,
	    &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "ifsboot:startup") == 0);
}

/* File settings are their catalogue rows: stored, loaded and found as
 * sections by key.
 */
static void
test_file_settings(void)
{
	fsimg_layer_inputs_t inputs;
	fsimg_file_setting_t setting;
	int object;

	bzero(&inputs, sizeof(inputs));
	inputs.ifsboot_startup = NULL;
	inputs.ifsboot_prefix = NULL;
	fsimg_file_setting_set(&inputs, FSIMG_FILE_IFSBOOT_PREFIX,
	    (fsimg_io_t *)(void *)&object);
	CHECK(inputs.ifsboot_prefix == (fsimg_io_t *)(void *)&object &&
	    inputs.ifsboot_startup == NULL);
	CHECK(fsimg_file_setting_input(&inputs, FSIMG_FILE_IFSBOOT_PREFIX) ==
	    inputs.ifsboot_prefix);
	CHECK(fsimg_file_setting_input(NULL, FSIMG_FILE_IFSBOOT_PREFIX) ==
	    NULL);
	CHECK(fsimg_layer_section(fsimg_layer_find("ifsboot"), "startup",
	    &setting) && setting == FSIMG_FILE_IFSBOOT_STARTUP);
	CHECK(fsimg_layer_section(fsimg_layer_find("ifsboot"), "prefix",
	    &setting) && setting == FSIMG_FILE_IFSBOOT_PREFIX);
	CHECK(!fsimg_layer_section(fsimg_layer_find("ifsboot"), "machine",
	    &setting));
	CHECK(!fsimg_layer_section(fsimg_layer_find("hbcifs"), "prefix",
	    &setting));
	CHECK(!fsimg_layer_section(NULL, "prefix", &setting));
}

/*
 * Every member of fsimg_write_options_t is one of: a catalogue row (above),
 * a mode field an operation without a filesystem writer passes through,
 * or an ordinary volume field that check_mode in lib/layers/output.c
 * requires at its default. Listed members are disjoint, and an unlisted
 * run of bytes is accepted only as the exact padding that aligns the
 * member after it (or the record's end), so a member added to the public
 * record without being placed in one of these lists, and so in wrap's
 * check, fails here.
 */
static void
test_write_member_coverage(void)
{
	static const member_t passed[] = {
		WRITE_MEMBER(size, size_t), WRITE_MEMBER(version, uint32_t),
		WRITE_MEMBER(flags, uint32_t), WRITE_MEMBER(epoch, int64_t),
		WRITE_MEMBER(layers, const fsimg_layer_options_t *),
		WRITE_MEMBER(inputs, const fsimg_layer_inputs_t *),
		WRITE_MEMBER(compression, char)
	};
	static const member_t checked[] = {
		WRITE_MEMBER(volume_bytes, uint64_t),
		WRITE_MEMBER(min_free_bytes, uint64_t),
		WRITE_MEMBER(label, const char *),
		WRITE_MEMBER(volume_serial, uint32_t),
		WRITE_MEMBER(reserved, uint64_t)
	};
	const member_t *lists[] = { write_members, passed, checked };
	const size_t counts[] = {
		sizeof(write_members) / sizeof(write_members[0]),
		sizeof(passed) / sizeof(passed[0]),
		sizeof(checked) / sizeof(checked[0])
	};
	unsigned char coverage[sizeof(fsimg_write_options_t)];
	size_t start, end, align;

	bzero(coverage, sizeof(coverage));
	for (size_t l = 0; l < sizeof(lists) / sizeof(lists[0]); l++) {
		for (size_t i = 0; i < counts[l]; i++)
			mark_member(coverage, sizeof(coverage), lists[l][i].offset,
			    lists[l][i].size);
	}
	for (start = 0; start < sizeof(coverage); start = end) {
		end = start + 1;
		if (coverage[start] != 0)
			continue;
		while (end < sizeof(coverage) && coverage[end] == 0)
			end++;
		/* The alignment of what follows the run. */
		align = _Alignof(fsimg_write_options_t);
		for (size_t l = 0; l < sizeof(lists) / sizeof(lists[0]); l++) {
			for (size_t i = 0; i < counts[l]; i++) {
				if (lists[l][i].offset == end)
					align = lists[l][i].align;
			}
		}
		if ((start + align - 1) / align * align != end)
			fprintf(stderr, "write options bytes %zu-%zu are unlisted\n",
			    start, end - 1);
		CHECK((start + align - 1) / align * align == end);
	}
}

/*
 * A bool member or presence flag holding a byte other than 0 or 1 is not
 * a bool: it is refused naming its key, never read as false, so a stray
 * setting of an absent layer cannot pass as absent.
 */
static void
test_bool_bytes(void)
{
	static const unsigned char garbage[] = { 2, 0x80, 0xff };
	fsimg_layer_options_t layers;
	fsimg_error_t error;

	for (size_t i = 0; i < sizeof(garbage) / sizeof(garbage[0]); i++) {
		fsimg_layer_options_init(&layers);
		bcopy(&garbage[i], &layers.mbr_id_set, 1);
		CHECK(check_layers("fat32", NULL, &layers, NULL, true, &error) ==
		    FSIMG_EINVAL);
		CHECK(strcmp(error.path, "mbr:id") == 0);
		fsimg_layer_options_init(&layers);
		bcopy(&garbage[i], &layers.ifsboot_virtual_set, 1);
		CHECK(check_layers("ifs/ifsboot", NULL, &layers, NULL, true,
		    &error) == FSIMG_EINVAL);
		CHECK(strcmp(error.path, "ifsboot:virtual") == 0);
		fsimg_layer_options_init(&layers);
		bcopy(&garbage[i], &layers.simg_crc, 1);
		CHECK(check_layers("fat32/simg", NULL, &layers, NULL, true,
		    &error) == FSIMG_EINVAL);
		CHECK(strcmp(error.path, "simg:crc") == 0);
	}
}

int
main(void)
{
	test_slices();
	test_member_bindings();
	test_masks_and_files();
	test_rows();
	test_capability_keys();
	test_defaults();
	test_default_distinctions();
	test_structure();
	test_presence();
	test_store();
	test_write_applicability();
	test_root_setting_applicability();
	test_layer_applicability();
	test_file_settings();
	test_bool_bytes();
	test_write_member_coverage();
	printf("settings tests passed\n");
	return (0);
}
