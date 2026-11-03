/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>
#include <time.h>

#include "fsimg-endian.h"
#include "settings.h"
#include "unicode.h"
#include "write-iso9660.h"

/*
 * Planning runs fixed passes
 * over the frozen catalog. Every pass reads rows and streams the earlier
 * passes wrote, and every size is fixed before any block number, so each
 * pass runs once:
 *
 *   1. entries: one row per entry with its kind, size and encoded
 *	metadata, and each directory's link count;
 *   2. primary names (write-names.c), then relocation: a physical
 *	parent-first pass moves directories under the relocation container
 *	and rebuilds the primary stream with CL placeholders and RE records;
 *   3. Joliet names (write-names.c);
 *   4. numbering: each hierarchy's directories in path-table order, a
 *	breadth-first walk of its name stream whose queue is the path
 *	stream itself;
 *   5. sizing: each directory's records packed into blocks, in
 *	path-table order, and each primary record's Rock Ridge entries
 *	split, their continuation fragments placed by next fit in that
 *	same order, which is the order emission writes them;
 *   6. layout: block numbers in the writer profile's order;
 *   7. the summary.
 */

/* The longest file identifier a directory must leave room for under the
 * 255-byte path limit: NAME.EXT;1 at level 1, 30 characters with ".;1"
 * otherwise.
 */
#define LEVEL1_FILE_IDENTIFIER	14u
#define FILE_IDENTIFIER_MAX	ISO_PRIMARY_IDENTIFIER_MAX

static fsimg_status_t validate_options(fsimg_write_options_t *, unsigned,
    fsimg_error_t *);
static fsimg_status_t make_plan(fsimg_writer_t *, fsimg_error_t *);
static void free_plan(fsimg_plan_t *);
static bool label_valid(const char *);
static size_t longest_component(const unsigned char *, size_t);
static fsimg_status_t encode_descriptor(const fsimg_write_options_t *, int64_t,
    unsigned char *, const char **);
static void encode_labels(iso_plan_t *);
static fsimg_status_t descriptor_error(const char *, fsimg_error_t *);
static fsimg_status_t encode_time(iso_plan_t *, const fsimg_entry_t *,
    uint32_t, const fsimg_timestamp_t *, unsigned char *, bool *,
    fsimg_error_t *);
static fsimg_status_t encode_metadata(iso_plan_t *, const fsimg_entry_t *,
    iso_row_t *, fsimg_error_t *);
static fsimg_status_t plan_entry(iso_plan_t *, const fsimg_catalog_entry_t *,
    fsimg_error_t *);
static fsimg_status_t plan_boot_entry(iso_plan_t *,
    const fsimg_catalog_entry_t *, fsimg_error_t *);
static fsimg_status_t plan_entries(iso_plan_t *, fsimg_error_t *);
static fsimg_status_t relocate_directories(iso_plan_t *, fsimg_error_t *);
static fsimg_status_t number_directories(iso_plan_t *, iso_hierarchy_t,
    fsimg_error_t *);
static fsimg_status_t size_dot(iso_plan_t *, iso_row_t *, iso_rr_record_t,
    uint64_t *, fsimg_error_t *);
static fsimg_status_t size_child(iso_plan_t *, iso_hierarchy_t,
    const iso_name_t *, uint64_t *, fsimg_error_t *);
static fsimg_status_t size_directory(iso_plan_t *, iso_hierarchy_t,
    uint64_t, fsimg_error_t *);
static fsimg_status_t size_directories(iso_plan_t *, iso_hierarchy_t,
    fsimg_error_t *);
static fsimg_status_t advance(uint64_t *, uint64_t, fsimg_error_t *);
static fsimg_status_t place_directories(iso_plan_t *, iso_hierarchy_t,
    uint64_t *, fsimg_error_t *);
static fsimg_status_t place_files(iso_plan_t *, uint64_t *, fsimg_error_t *);
static fsimg_status_t layout(iso_plan_t *, fsimg_error_t *);
static void summarize(iso_plan_t *);

const fsimg_writer_ops_t fsimg_iso9660_writer_ops = {
	.validate = validate_options,
	.plan = make_plan,
	.emit = iso_write_emit,
	.free = free_plan
};

/*
 * Settings.
 */

/* A label of 1 to 16 characters of the Basic Multilingual Plane without
 * control characters.
 */
static bool
label_valid(const char *label)
{
	uint16_t units[ISO_LABEL_UNITS_MAX];
	size_t length, count;

	length = strnlen(label, ISO_LABEL_TEXT_MAX + 1);
	if (length == 0 || length > ISO_LABEL_TEXT_MAX ||
	    fsimg_utf8_to_utf16(label, length, units, ISO_LABEL_UNITS_MAX,
	    &count, NULL) != FSIMG_OK)
		return (false);
	for (size_t i = 0; i < count; i++) {
		if (units[i] < 0x20 || (units[i] >= 0x7f && units[i] <= 0x9f) ||
		    (units[i] >= 0xd800 && units[i] <= 0xdfff))
			return (false);
	}
	return (true);
}

/* Whether the descriptor field table and the settings table agree: every
 * field names a descriptor row of the settings table of the matching kind
 * and capacity (a text row holds the characters its field does, and a
 * terminator), and every descriptor text or date row is named by exactly
 * one field. The two tables are linked only by the key's spelling, so this
 * is what keeps one from drifting from the other.
 */
bool
iso_descriptor_settings_match(void)
{
	const fsimg_setting_slice_t *table = &fsimg_filesystem_settings;
	const fsimg_setting_t *setting;
	unsigned named;

	for (unsigned i = 0; i < ISO_DESCRIPTOR_FIELDS; i++) {
		const iso_descriptor_field_t *field = &iso_descriptor_fields[i];

		setting = fsimg_setting_find(table, field->setting,
		    strlen(field->setting));
		if (setting == NULL || setting->support != FSIMG_SETTING_ISO_DESCRIPTOR)
			return (false);
		if (field->class == ISO_CLASS_DATE) {
			if (!fsimg_setting_is_date(setting))
				return (false);
		} else if (!fsimg_setting_is_borrowed_text(setting) ||
		    setting->text_bytes != field->width / 2 + 1) {
			return (false);
		}
	}
	for (size_t i = 0; i < table->count; i++) {
		setting = &table->rows[i];
		if (setting->support != FSIMG_SETTING_ISO_DESCRIPTOR)
			continue;
		named = 0;
		for (unsigned k = 0; k < ISO_DESCRIPTOR_FIELDS; k++)
			named += strcmp(setting->key,
			    iso_descriptor_fields[k].setting) == 0;
		if (named != 1)
			return (false);
	}
	return (true);
}

/* Encode the descriptor's eleven text and date fields from the options
 * into image, the bytes 190 to 880 of a descriptor laid out by
 * iso_descriptor_fields. Each row reads its value from the settings row
 * its setting key names, and goes through its class's one encoder; an
 * unset text is the row's unset text, a creation or modification date left
 * out is fallback, and any other date left out is absent. FSIMG_EINVAL,
 * naming the setting in *bad_key when it is not NULL, for the first value
 * its class refuses; FSIMG_ESTATE when a field names no settings row,
 * which iso_descriptor_settings_match rules out.
 */
static fsimg_status_t
encode_descriptor(const fsimg_write_options_t *options, int64_t fallback,
    unsigned char *image, const char **bad_key)
{
	const fsimg_setting_records_t records = { .write = options };
	const iso_descriptor_field_t *row;
	const fsimg_setting_t *setting;
	fsimg_setting_value_t value;
	unsigned char *field;
	bool encoded, set;

	for (unsigned i = 0; i < ISO_DESCRIPTOR_FIELDS; i++) {
		row = &iso_descriptor_fields[i];
		field = image + row->offset;
		setting = fsimg_setting_find(&fsimg_filesystem_settings, row->setting,
		    strlen(row->setting));
		/* The tables are checked to agree before any field is
		 * encoded, so a missing row is a defect in the tables, not
		 * in the caller's options.
		 */
		if (setting == NULL)
			return (FSIMG_ESTATE);
		fsimg_setting_load(setting, &records, &value);
		if (row->class != ISO_CLASS_DATE) {
			if (value.text == NULL)
				value.text = row->unset_text != NULL ? row->unset_text : "";
			encoded = iso_text_encode(row->class, value.text, field,
			    row->width);
		} else {
			set = !fsimg_setting_is_default(setting, &records);
			if (set || row->unset_dated) {
				encoded = iso_date17_encode(set ? value.i64 : fallback, field);
			} else {
				iso_date17_absent(field);
				encoded = true;
			}
		}
		if (!encoded) {
			if (bad_key != NULL)
				*bad_key = row->setting;
			return (FSIMG_EINVAL);
		}
	}
	return (FSIMG_OK);
}

/* FSIMG_EINVAL for a descriptor setting a class refuses, naming its key. */
static fsimg_status_t
descriptor_error(const char *key, fsimg_error_t *error)
{
	fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER,
	    "ISO 9660 descriptor text or date outside its field's rules");
	fsimg_error_path(error, key);
	return (FSIMG_EINVAL);
}

/* The common options with the writer profile's rules: 2048-byte sectors
 * and allocation units, an exact size in whole blocks or none, no free
 * space to reserve, level 1 to 3 (zero selects 3), a reproducible epoch
 * the seven-byte date can hold, and the label rule; NULL selects CDROM.
 * The volume serial is a common identity the format does not store.
 */
static fsimg_status_t
validate_options(fsimg_write_options_t *options, unsigned variant FSIMG_UNUSED,
    fsimg_error_t *error)
{
	static const char default_label[] = "CDROM";
	unsigned char image[ISO_DESCRIPTOR_BYTES];
	const char *bad_key;
	fsimg_status_t status;

	if (options->boot_load_sectors > UINT16_MAX ||
	    (options->boot_bios == NULL && options->boot_load_sectors != 0))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER,
		    "bootload requires boot and must fit in 16 bits"));
	if (options->boot_bios != NULL && options->boot_load_sectors == 0)
		options->boot_load_sectors = 4;
	if ((options->sector_bytes != 0 &&
	    options->sector_bytes != ISO_BLOCK_BYTES) ||
	    (options->allocation_unit_bytes != 0 &&
	    options->allocation_unit_bytes != ISO_BLOCK_BYTES) ||
	    options->volume_bytes % ISO_BLOCK_BYTES != 0 ||
	    options->min_free_bytes != 0 || options->iso_level > 3 ||
	    ((options->flags & FSIMG_WRITE_REPRODUCIBLE) != 0 &&
	    (options->epoch < ISO_DATE7_MIN || options->epoch > ISO_DATE7_MAX)))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER,
		    "invalid ISO 9660 geometry, level, free space or reproducible epoch"));
	if (!iso_descriptor_settings_match())
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "ISO 9660 descriptor fields and settings disagree"));
	status = encode_descriptor(options, 0, image, &bad_key);
	if (status == FSIMG_EINVAL)
		return (descriptor_error(bad_key, error));
	if (status != FSIMG_OK)
		return (fsimg_error_set(error, status, 0, FSIMG_OP_WRITER,
		    "ISO 9660 descriptor settings are not in the table"));
	if (options->label == NULL)
		options->label = default_label;
	else if (!label_valid(options->label))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER,
		    "invalid ISO 9660 volume label"));
	options->sector_bytes = ISO_BLOCK_BYTES;
	options->allocation_unit_bytes = ISO_BLOCK_BYTES;
	if (options->iso_level == 0)
		options->iso_level = 3;
	return (FSIMG_OK);
}

/* The label in both descriptors: Joliet's exact UCS-2 units padded with
 * U+0020, and the primary's d-character projection, letters uppercased
 * and every other character outside the set replaced by '_', padded with
 * spaces.
 */
static void
encode_labels(iso_plan_t *plan)
{
	const char *label = plan->common.writer->options.label;
	uint16_t units[ISO_LABEL_UNITS_MAX];
	unsigned char *primary = plan->label[ISO_PRIMARY];
	unsigned char *joliet = plan->label[ISO_JOLIET];
	size_t count;
	uint16_t c;

	count = 0;
	if (fsimg_utf8_to_utf16(label, strlen(label), units,
	    ISO_LABEL_UNITS_MAX, &count, NULL) != FSIMG_OK)
		count = 0;
	memset(primary, ' ', ISO_LABEL_BYTES);
	for (size_t i = 0; i < ISO_LABEL_BYTES / 2; i++)
		fsimg_store_be16(joliet + 2 * i, i < count ? units[i] : ' ');
	for (size_t i = 0; i < count; i++) {
		c = units[i];
		if (c >= 'a' && c <= 'z')
			c = (uint16_t)(c - 'a' + 'A');
		primary[i] = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		    c == '_' ? (unsigned char)c : '_';
	}
}

/*
 * Rows.
 */

/* The physical hierarchy may add one metadata-only directory. Every
 * driver lookup goes through this boundary; source operations continue
 * to use only the frozen catalog's IDs.
 */
fsimg_status_t
iso_catalog_get(iso_plan_t *plan, uint64_t id,
    const fsimg_catalog_entry_t **catalog, fsimg_error_t *error)
{
	if (id != 0 && id == plan->relocation_entry.id) {
		*catalog = &plan->relocation_entry;
		return (FSIMG_OK);
	}
	return (fsimg_catalog_get(plan->common.writer, id, catalog, error));
}

fsimg_status_t
iso_row_get(iso_plan_t *plan, uint64_t id, iso_row_t *row,
    fsimg_error_t *error)
{
	unsigned char raw[ISO_ROW_BYTES];
	fsimg_status_t status;

	status = fsimg_array_get(plan->rows, id, raw, error);
	if (status != FSIMG_OK)
		return (status);
	row->size = fsimg_load_le64(raw);
	row->group[ISO_PRIMARY] = fsimg_load_le64(raw + 8);
	row->group[ISO_JOLIET] = fsimg_load_le64(raw + 16);
	row->child.fragment_position = fsimg_load_le64(raw + 24);
	row->target_hash = fsimg_load_le64(raw + 32);
	row->kind = fsimg_load_le32(raw + 40);
	row->mode = fsimg_load_le32(raw + 44);
	row->uid = fsimg_load_le32(raw + 48);
	row->gid = fsimg_load_le32(raw + 52);
	row->links = fsimg_load_le32(raw + 56);
	for (unsigned h = 0; h < ISO_HIERARCHIES; h++) {
		row->alias[h] = fsimg_load_le32(raw + 60 + 4 * h);
		row->children[h] = fsimg_load_le32(raw + 68 + 4 * h);
		row->block[h] = fsimg_load_le32(raw + 76 + 4 * h);
		row->bytes[h] = fsimg_load_le32(raw + 84 + 4 * h);
		row->number[h] = fsimg_load_le16(raw + 96 + 2 * h);
		row->identifier[h] = raw[114 + h];
	}
	row->first_block = fsimg_load_le32(raw + 92);
	row->path_length = fsimg_load_le16(raw + 100);
	for (unsigned i = 0; i < ISO_FRAGMENTS_MAX; i++)
		row->child.fragment_bytes[i] = fsimg_load_le16(raw + 102 + 2 * i);
	row->target_bytes = fsimg_load_le16(raw + 108);
	row->level = raw[116];
	row->child.record_entries = raw[117];
	row->child.fragments = raw[118];
	row->times = raw[119];
	bcopy(raw + 120, row->modified, ISO_DATE7_BYTES);
	bcopy(raw + 127, row->accessed, ISO_DATE7_BYTES);
	bcopy(raw + 134, row->changed, ISO_DATE7_BYTES);
	row->placeholder.fragment_position = fsimg_load_le64(raw + 144);
	for (unsigned i = 0; i < ISO_FRAGMENTS_MAX; i++)
		row->placeholder.fragment_bytes[i] = fsimg_load_le16(raw + 152 + 2 * i);
	row->placeholder.record_entries = raw[158];
	row->placeholder.fragments = raw[159];
	row->physical_parent = fsimg_load_le64(raw + 160);
	row->relocation_alias = fsimg_load_le32(raw + 168);
	if (row->size > ISO_VOLUME_BLOCKS_MAX * ISO_BLOCK_BYTES ||
	    row->relocation_alias > ISO_ALIAS_MAX ||
	    row->child.fragments > ISO_FRAGMENTS_MAX ||
	    row->placeholder.fragments > ISO_FRAGMENTS_MAX)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_WRITER, "ISO 9660 plan row is outside its limits"));
	return (FSIMG_OK);
}

fsimg_status_t
iso_row_set(iso_plan_t *plan, uint64_t id, const iso_row_t *row,
    fsimg_error_t *error)
{
	unsigned char raw[ISO_ROW_BYTES];

	bzero(raw, sizeof(raw));
	fsimg_store_le64(raw, row->size);
	fsimg_store_le64(raw + 8, row->group[ISO_PRIMARY]);
	fsimg_store_le64(raw + 16, row->group[ISO_JOLIET]);
	fsimg_store_le64(raw + 24, row->child.fragment_position);
	fsimg_store_le64(raw + 32, row->target_hash);
	fsimg_store_le32(raw + 40, row->kind);
	fsimg_store_le32(raw + 44, row->mode);
	fsimg_store_le32(raw + 48, row->uid);
	fsimg_store_le32(raw + 52, row->gid);
	fsimg_store_le32(raw + 56, row->links);
	for (unsigned h = 0; h < ISO_HIERARCHIES; h++) {
		fsimg_store_le32(raw + 60 + 4 * h, row->alias[h]);
		fsimg_store_le32(raw + 68 + 4 * h, row->children[h]);
		fsimg_store_le32(raw + 76 + 4 * h, row->block[h]);
		fsimg_store_le32(raw + 84 + 4 * h, row->bytes[h]);
		fsimg_store_le16(raw + 96 + 2 * h, row->number[h]);
		raw[114 + h] = row->identifier[h];
	}
	fsimg_store_le32(raw + 92, row->first_block);
	fsimg_store_le16(raw + 100, row->path_length);
	for (unsigned i = 0; i < ISO_FRAGMENTS_MAX; i++)
		fsimg_store_le16(raw + 102 + 2 * i, row->child.fragment_bytes[i]);
	fsimg_store_le16(raw + 108, row->target_bytes);
	raw[116] = row->level;
	raw[117] = row->child.record_entries;
	raw[118] = row->child.fragments;
	raw[119] = row->times;
	bcopy(row->modified, raw + 120, ISO_DATE7_BYTES);
	bcopy(row->accessed, raw + 127, ISO_DATE7_BYTES);
	bcopy(row->changed, raw + 134, ISO_DATE7_BYTES);
	fsimg_store_le64(raw + 144, row->placeholder.fragment_position);
	for (unsigned i = 0; i < ISO_FRAGMENTS_MAX; i++)
		fsimg_store_le16(raw + 152 + 2 * i, row->placeholder.fragment_bytes[i]);
	raw[158] = row->placeholder.record_entries;
	raw[159] = row->placeholder.fragments;
	fsimg_store_le64(raw + 160, row->physical_parent);
	fsimg_store_le32(raw + 168, row->relocation_alias);
	return (fsimg_array_set(plan->rows, id, raw, error));
}

/*
 * Shared sizing rules, which emission follows too.
 */

/* A file's sections: one up to 2^32 - 1 bytes, an empty file included;
 * above, sections of ISO_SECTION_MAX bytes and one with the rest.
 */
uint32_t
iso_sections(uint64_t size)
{
	if (size <= UINT32_MAX)
		return (1);
	return ((uint32_t)((size + ISO_SECTION_MAX - 1) / ISO_SECTION_MAX));
}

/* The data length of a file's section index. */
uint32_t
iso_section_bytes(uint64_t size, uint32_t index)
{
	uint32_t sections = iso_sections(size);

	if (sections == 1)
		return ((uint32_t)size);
	if (index + 1 < sections)
		return ((uint32_t)ISO_SECTION_MAX);
	return ((uint32_t)(size - (uint64_t)(sections - 1) * ISO_SECTION_MAX));
}

/* The system use bytes a record with an identifier of identifier_length
 * bytes can hold within ISO_RECORD_MAX.
 */
size_t
iso_record_room(size_t identifier_length)
{
	return (ISO_RECORD_MAX - iso_record_length(identifier_length, 0));
}

/* The length of the longest run of target bytes between slashes. */
static size_t
longest_component(const unsigned char *target, size_t length)
{
	size_t longest = 0, run = 0;

	for (size_t i = 0; i < length; i++) {
		run = target[i] == '/' ? 0 : run + 1;
		if (run > longest)
			longest = run;
	}
	return (longest);
}

/*
 * The Rock Ridge entries of entry id's record in its parent, into
 * plan->entries, their length in *length. A symbolic link's target is
 * read through the common source lifecycle, with components of at most
 * 255 bytes, and its encoded SL length and hash are stored in row. The
 * emission wrapper compares that observed snapshot with the planned one.
 */
static fsimg_status_t
encode_child_entries(iso_plan_t *plan, const iso_name_t *name, iso_row_t *row,
    fsimg_status_t component_refusal, size_t *length, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	const fsimg_catalog_entry_t *catalog;
	iso_rr_input_t input;
	unsigned char identifier[ISO_PRIMARY_IDENTIFIER_MAX];
	size_t base, target_length;
	size_t identifier_length;
	uint64_t hash;
	fsimg_status_t status;

	bzero(&input, sizeof(input));
	status = iso_catalog_get(plan, name->id, &catalog, error);
	if (status != FSIMG_OK)
		return (status);
	if (row->kind == FSIMG_ENTRY_SYMLINK) {
		status = fsimg_writer_link_target(writer, name->id, plan->target,
		    &target_length, error);
		if (status != FSIMG_OK)
			return (status);
		if (target_length == 0 || target_length > ISO_LINK_TARGET_MAX) {
			status = fsimg_error_set(error, FSIMG_ESOURCE_CHANGED,
			    0, FSIMG_OP_WRITER,
			    "ISO 9660 link target length changed");
			fsimg_error_path(error, catalog->entry.path);
			return (status);
		}
		if (longest_component(plan->target, target_length) >
		    ISO_LINK_COMPONENT_MAX) {
			status = fsimg_error_set(error, component_refusal, 0,
			    FSIMG_OP_WRITER, "ISO 9660 link target components "
			    "are limited to 255 bytes");
			fsimg_error_path(error, catalog->entry.path);
			return (status);
		}
		input.target = plan->target;
		input.target_length = target_length;
	}
	input.row = row;
	input.record = ISO_RR_CHILD;
	if (name->role == ISO_NAME_PLACEHOLDER)
		input.record = ISO_RR_PLACEHOLDER;
	else if (name->role == ISO_NAME_RELOCATED)
		input.record = ISO_RR_RELOCATED;
	input.name = strrchr(catalog->entry.path, '/') + 1;
	input.name_length = strlen(input.name);
	if (name->role == ISO_NAME_RELOCATED) {
		if (row->relocation_alias == 0 ||
		    !iso_identifier(plan, ISO_PRIMARY, input.name, input.name_length,
		    true, row->relocation_alias, identifier, &identifier_length))
			return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
			    FSIMG_OP_WRITER, "invalid ISO 9660 relocation alias"));
		if (name->length != identifier_length ||
		    bcmp(name->identifier, identifier, identifier_length) != 0)
			return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
			    FSIMG_OP_WRITER, "ISO 9660 relocation alias disagrees with its row"));
	}
	*length = iso_rr_encode(&input, plan->entries);
	if (row->kind != FSIMG_ENTRY_SYMLINK)
		return (FSIMG_OK);

	/* The SL entries are what follows the entries of the same record
	 * without a target.
	 */
	input.target_length = 0;
	base = iso_rr_encode(&input, plan->entries + *length);
	hash = iso_hash(plan->entries + base, *length - base);
	row->target_bytes = (uint16_t)(*length - base);
	row->target_hash = hash;
	return (FSIMG_OK);
}

fsimg_status_t
iso_child_entries(iso_plan_t *plan, const iso_name_t *name, iso_row_t *row,
    size_t *length, fsimg_error_t *error)
{
	return (encode_child_entries(plan, name, row, FSIMG_EUNSUPPORTED,
	    length, error));
}

/* Rebuild an emitted record through the same encoder, then require the
 * target snapshot planning recorded. An unrepresentable target after
 * planning accepted it is a changed source, not a profile refusal.
 */
fsimg_status_t
iso_check_child_entries(iso_plan_t *plan, const iso_name_t *name,
    const iso_row_t *row, size_t *length, fsimg_error_t *error)
{
	iso_row_t encoded = *row;
	const fsimg_catalog_entry_t *catalog;
	fsimg_status_t status;

	status = encode_child_entries(plan, name, &encoded, FSIMG_ESOURCE_CHANGED,
	    length, error);
	if (status != FSIMG_OK)
		return (status);
	if (row->kind != FSIMG_ENTRY_SYMLINK ||
	    (row->target_bytes == encoded.target_bytes &&
	    row->target_hash == encoded.target_hash))
		return (FSIMG_OK);
	status = iso_catalog_get(plan, name->id, &catalog, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
	    FSIMG_OP_WRITER, "ISO 9660 link target changed since planning");
	fsimg_error_path(error, catalog->entry.path);
	return (status);
}

/*
 * Pass 1: entries.
 */

/* Encode one time field into its seven-byte form. An absent modification
 * time takes the fallback, as it is also the recording date; another
 * absent time stays absent. A present time keeps its whole UTC seconds
 * within 1900 to 2155, else takes the fallback; reproducible mode stores
 * the fallback, the epoch, for every time. A present time the stored
 * value does not preserve, through its range, fraction, basis or the
 * epoch, is a loss.
 */
static fsimg_status_t
encode_time(iso_plan_t *plan, const fsimg_entry_t *entry, uint32_t field,
    const fsimg_timestamp_t *time, unsigned char *out, bool *present,
    fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	int64_t seconds;
	bool exact, in_range, reproducible;

	*present = (entry->metadata.valid & field) != 0;
	if (!*present) {
		if (field == FSIMG_META_MODIFIED)
			iso_date7_encode(plan->fallback, out);
		return (FSIMG_OK);
	}
	reproducible = (writer->options.flags & FSIMG_WRITE_REPRODUCIBLE) != 0;
	in_range = time->seconds >= ISO_DATE7_MIN &&
	    time->seconds <= ISO_DATE7_MAX;
	seconds = in_range && !reproducible ? time->seconds : plan->fallback;
	iso_date7_encode(seconds, out);
	exact = in_range && time->basis == FSIMG_TIME_UTC &&
	    time->nanoseconds == 0 && seconds == time->seconds;
	if (exact)
		return (FSIMG_OK);
	return (fsimg_writer_loss(writer, entry, field,
	    "ISO 9660 cannot preserve the requested timestamp", error));
}

/* An entry's PX mode, owner and TF times. The PX type follows the kind;
 * an absent mode takes the profile's default permissions and an absent
 * owner uid and gid 0. Both are stored whole, so neither loses anything.
 */
static fsimg_status_t
encode_metadata(iso_plan_t *plan, const fsimg_entry_t *entry, iso_row_t *row,
    fsimg_error_t *error)
{
	const fsimg_metadata_t *m = &entry->metadata;
	uint32_t type, permissions;
	bool present;
	fsimg_status_t status;

	switch (entry->kind) {
	case FSIMG_ENTRY_FILE:
		type = ISO_PX_FILE;
		permissions = ISO_FILE_PERMISSIONS;
		break;
	case FSIMG_ENTRY_DIRECTORY:
		type = ISO_PX_DIRECTORY;
		permissions = ISO_DIRECTORY_PERMISSIONS;
		break;
	case FSIMG_ENTRY_SYMLINK:
		type = ISO_PX_SYMLINK;
		permissions = ISO_OTHER_PERMISSIONS;
		break;
	case FSIMG_ENTRY_FIFO:
		type = ISO_PX_FIFO;
		permissions = ISO_OTHER_PERMISSIONS;
		break;
	default:
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "ISO 9660 planning received an unknown entry kind"));
	}
	if ((m->valid & FSIMG_META_MODE) != 0)
		permissions = m->mode & FSIMG_MODE_BITS;
	row->mode = type | permissions;
	if ((m->valid & FSIMG_META_OWNER) != 0) {
		row->uid = m->uid;
		row->gid = m->gid;
	}

	status = encode_time(plan, entry, FSIMG_META_MODIFIED, &m->modified,
	    row->modified, &present, error);
	if (status != FSIMG_OK)
		return (status);
	status = encode_time(plan, entry, FSIMG_META_ACCESSED, &m->accessed,
	    row->accessed, &present, error);
	if (status != FSIMG_OK)
		return (status);
	if (present)
		row->times |= ISO_TIME_ACCESSED;
	status = encode_time(plan, entry, FSIMG_META_CHANGED, &m->changed,
	    row->changed, &present, error);
	if (status != FSIMG_OK)
		return (status);
	if (present)
		row->times |= ISO_TIME_CHANGED;
	return (FSIMG_OK);
}

/* Match boot settings during the catalog pass. Each selected entry is an
 * ordinary file; the catalog references its planned payload unchanged.
 */
static fsimg_status_t
plan_boot_entry(iso_plan_t *plan, const fsimg_catalog_entry_t *catalog,
    fsimg_error_t *error)
{
	const fsimg_write_options_t *options = &plan->common.writer->options;
	const fsimg_entry_t *entry = &catalog->entry;
	bool bios, efi;

	bios = options->boot_bios != NULL &&
	    strcmp(entry->path, options->boot_bios) == 0;
	efi = options->boot_efi != NULL &&
	    strcmp(entry->path, options->boot_efi) == 0;
	if (!bios && !efi)
		return (FSIMG_OK);
	if (entry->kind != FSIMG_ENTRY_FILE || entry->size == 0 ||
	    entry->size > UINT32_MAX)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER,
		    bios ? "boot must name a nonempty single-section regular file" :
		    "efiboot must name a nonempty single-section regular file"));
	if (bios && iso_boot_image_blocks((uint16_t)options->boot_load_sectors) >
	    ISO_BLOCKS(entry->size))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER,
		    "bootload exceeds the boot image's allocated blocks"));
	if (bios)
		plan->boot_bios = catalog->id;
	if (efi)
		plan->boot_efi = catalog->id;
	return (FSIMG_OK);
}

/* One entry's row: its name within the Rock Ridge bound, its metadata, a
 * file's size within the level, and one more link on its parent when it
 * is a directory.
 */
static fsimg_status_t
plan_entry(iso_plan_t *plan, const fsimg_catalog_entry_t *catalog,
    fsimg_error_t *error)
{
	const fsimg_entry_t *entry = &catalog->entry;
	iso_row_t row, parent;
	fsimg_status_t status;

	if (strlen(strrchr(entry->path, '/') + 1) > ISO_RR_NAME_MAX) {
		status = fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_WRITER,
		    "ISO 9660 Rock Ridge names are limited to 255 bytes");
		fsimg_error_path(error, entry->path);
		return (status);
	}
	/* The shared capability record also describes the reader, whose
	 * names, targets and metadata are wider than this writer profile.
	 */
	if (entry->kind == FSIMG_ENTRY_SYMLINK &&
	    entry->size > ISO_LINK_TARGET_MAX) {
		status = fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_WRITER, "ISO 9660 link targets are limited to 1023 bytes");
		fsimg_error_path(error, entry->path);
		return (status);
	}
	status = fsimg_writer_loss(plan->common.writer, entry,
	    entry->metadata.valid & (FSIMG_META_CREATED | FSIMG_META_ATTRIBUTES),
	    "ISO 9660 cannot preserve creation time or attributes", error);
	if (status != FSIMG_OK)
		return (status);
	status = plan_boot_entry(plan, catalog, error);
	if (status != FSIMG_OK)
		return (status);
	bzero(&row, sizeof(row));
	row.kind = entry->kind;
	row.physical_parent = catalog->parent_id;
	row.links = entry->kind == FSIMG_ENTRY_DIRECTORY ? 2 : 1;
	status = encode_metadata(plan, entry, &row, error);
	if (status != FSIMG_OK)
		return (status);
	if (entry->kind == FSIMG_ENTRY_FILE) {
		/* Bound the size first: every section and block count below
		 * is computed from it in 64 bits and must neither wrap nor
		 * outgrow the largest volume.
		 */
		if (entry->size > ISO_VOLUME_BLOCKS_MAX * ISO_BLOCK_BYTES) {
			status = fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
			    FSIMG_OP_WRITER, "ISO 9660 files are limited to "
			    "2^32 - 1 blocks");
			fsimg_error_path(error, entry->path);
			return (status);
		}
		if (entry->size > UINT32_MAX && plan->level < 3) {
			status = fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
			    FSIMG_OP_WRITER, "ISO 9660 files above 4 GiB "
			    "need interchange level 3");
			fsimg_error_path(error, entry->path);
			return (status);
		}
		row.size = entry->size;
		plan->common.info.initialized_bytes += entry->size;
	}
	status = iso_row_set(plan, catalog->id, &row, error);
	if (status != FSIMG_OK || entry->kind != FSIMG_ENTRY_DIRECTORY)
		return (status);
	status = iso_row_get(plan, catalog->parent_id, &parent, error);
	if (status != FSIMG_OK)
		return (status);
	parent.links++;
	return (iso_row_set(plan, catalog->parent_id, &parent, error));
}

/* Pass 1: the root's row, which has no catalog entry and takes the
 * defaults and the fallback time, then one row per entry in ID order,
 * which visits each parent before its children.
 */
static fsimg_status_t
plan_entries(iso_plan_t *plan, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	const fsimg_catalog_entry_t *catalog;
	iso_row_t root;
	fsimg_status_t status;

	bzero(&root, sizeof(root));
	root.kind = FSIMG_ENTRY_DIRECTORY;
	root.mode = ISO_PX_DIRECTORY | ISO_DIRECTORY_PERMISSIONS;
	root.links = 2;
	root.level = 1;
	iso_date7_encode(plan->fallback, root.modified);
	status = iso_row_set(plan, 0, &root, error);
	if (status != FSIMG_OK)
		return (status);
	for (uint64_t id = 1; id <= writer->count; id++) {
		status = fsimg_session_charge(&writer->session, 0, 0, 1, error);
		if (status != FSIMG_OK)
			return (status);
		status = iso_catalog_get(plan, id, &catalog, error);
		if (status != FSIMG_OK)
			return (status);
		status = plan_entry(plan, catalog, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (status != FSIMG_OK)
		return (status);
	if (writer->options.boot_bios != NULL && plan->boot_bios == 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER,
		    "boot names no file in the image"));
	if (writer->options.boot_efi != NULL && plan->boot_efi == 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER,
		    "efiboot names no file in the image"));
	return (FSIMG_OK);
}

/* Initialize the relocation container before the parent-first pass uses
 * it. A synthetic ID follows the catalog, so numeric ID order cannot
 * initialize this one physical parent for us.
 */
static fsimg_status_t
make_relocation_directory(iso_plan_t *plan, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	const fsimg_catalog_entry_t *catalog;
	iso_row_t row, root;
	uint64_t id;
	fsimg_status_t status;

	for (id = 1; id <= writer->count; id++) {
		status = fsimg_session_charge(&writer->session, 0, 0, 1, error);
		if (status != FSIMG_OK)
			return (status);
		status = iso_catalog_get(plan, id, &catalog, error);
		if (status != FSIMG_OK)
			return (status);
		if (strcmp(catalog->entry.path, "/.rr_moved") != 0)
			continue;
		if (catalog->entry.kind != FSIMG_ENTRY_DIRECTORY ||
		    catalog->children != 0) {
			status = fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
			    FSIMG_OP_WRITER, "relocation needs an empty .rr_moved directory");
			fsimg_error_path(error, "/.rr_moved");
			return (status);
		}
		break;
	}
	if (id <= writer->count) {
		status = iso_row_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
	} else {
		bzero(&row, sizeof(row));
		row.kind = FSIMG_ENTRY_DIRECTORY;
		row.mode = ISO_PX_DIRECTORY | ISO_DIRECTORY_PERMISSIONS;
		row.links = 2;
		iso_date7_encode(plan->fallback, row.modified);
		plan->relocation_entry = (fsimg_catalog_entry_t){
		    .id = id, .entry = { .path = "/.rr_moved",
		    .kind = FSIMG_ENTRY_DIRECTORY } };
		status = iso_row_get(plan, 0, &root, error);
		if (status != FSIMG_OK)
			return (status);
		root.links++;
		status = iso_row_set(plan, 0, &root, error);
		if (status != FSIMG_OK)
			return (status);
	}
	plan->relocation_id = id;
	row.level = 2;
	row.path_length = 8;
	row.physical_parent = 0;
	row.identifier[ISO_PRIMARY] = 8;
	row.alias[ISO_PRIMARY] = 0;
	return (iso_row_set(plan, id, &row, error));
}

/* A directory's physical level and path follow its already planned
 * parent. Relocation replaces that parent with the initialized container;
 * its own alias length is known independently of the numeric alias value.
 * Logical parents and names stay in the frozen catalog and original stream.
 */
static fsimg_status_t
relocate_directories(iso_plan_t *plan, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	const fsimg_catalog_entry_t *catalog;
	unsigned char identifier[ISO_PRIMARY_IDENTIFIER_MAX];
	iso_row_t row, parent;
	uint32_t longest, path, relocated = 0;
	const char *component;
	size_t length;
	fsimg_status_t status;

	longest = plan->level == 1 ? LEVEL1_FILE_IDENTIFIER :
	    FILE_IDENTIFIER_MAX;
	for (uint64_t id = 1; id <= writer->count; id++) {
		status = fsimg_session_charge(&writer->session, 0, 0, 1, error);
		if (status != FSIMG_OK)
			return (status);
		status = iso_catalog_get(plan, id, &catalog, error);
		if (status != FSIMG_OK)
			return (status);
		status = iso_row_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		if (row.kind != FSIMG_ENTRY_DIRECTORY || id == plan->relocation_id)
			continue;
		status = iso_row_get(plan, row.physical_parent, &parent, error);
		if (status != FSIMG_OK)
			return (status);
		path = parent.path_length + (row.physical_parent == 0 ? 0 : 1) +
		    row.identifier[ISO_PRIMARY];
		if (parent.level + 1u > ISO_PHYSICAL_LEVEL_MAX ||
		    path > ISO_PATH_MAX - 1 - longest) {
			/* The helper reads catalog snapshots. Re-fetch this entry
			 * before using its component afterward.
			 */
			if (plan->relocation_id == 0) {
				status = make_relocation_directory(plan, error);
				if (status != FSIMG_OK)
					return (status);
			}
			status = iso_catalog_get(plan, id, &catalog, error);
			if (status != FSIMG_OK)
				return (status);
			component = strrchr(catalog->entry.path, '/') + 1;
			row.relocation_alias = ++relocated;
			iso_identifier(plan, ISO_PRIMARY, component, strlen(component),
			    true, row.relocation_alias, identifier, &length);
			row.physical_parent = plan->relocation_id;
			status = iso_row_get(plan, plan->relocation_id, &parent, error);
			if (status != FSIMG_OK)
				return (status);
			parent.links++;
			status = iso_row_set(plan, plan->relocation_id, &parent, error);
			if (status != FSIMG_OK)
				return (status);
			path = parent.path_length + 1 + (uint32_t)length;
		}
		row.level = (uint8_t)(parent.level + 1);
		row.path_length = (uint16_t)path;
		status = iso_row_set(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (plan->relocation_id == 0)
		return (FSIMG_OK);
	return (iso_write_relocated_names(plan, error));
}

/*
 * Pass 4: numbering. The path stream lists a hierarchy's directories in
 * path-table order: the root is record 1, and visiting record i appends
 * its subdirectories in name-stream order, which is the order within one
 * parent the path table requires. The stream is its own queue: the walk
 * reads it behind the records it appends, so it visits every directory
 * once, parents before children, and numbers never decrease by parent.
 */
static fsimg_status_t
number_directories(iso_plan_t *plan, iso_hierarchy_t hierarchy,
    fsimg_error_t *error)
{
	fsimg_session_t *session = &plan->common.writer->session;
	unsigned char path[ISO_PATH_STREAM_MAX], raw[ISO_NAME_RECORD_MAX];
	fsimg_stream_t *paths;
	iso_name_t name;
	iso_row_t row, child;
	uint64_t cursor, names, id, bytes;
	size_t length;
	uint32_t number, count;
	fsimg_status_t status;

	status = fsimg_stream_new(session, ISO_PATH_STREAM_MAX,
	    &plan->paths[hierarchy], error);
	if (status != FSIMG_OK)
		return (status);
	paths = plan->paths[hierarchy];
	fsimg_store_le64(path, 0);
	fsimg_store_le16(path + 8, 1);
	path[ISO_PATH_STREAM_HEADER] = 0;
	status = fsimg_stream_append(paths, path, ISO_PATH_STREAM_HEADER + 1,
	    NULL, error);
	if (status == FSIMG_OK)
		status = iso_row_get(plan, 0, &row, error);
	if (status != FSIMG_OK)
		return (status);
	row.number[hierarchy] = 1;
	status = iso_row_set(plan, 0, &row, error);
	if (status != FSIMG_OK)
		return (status);
	count = 1;
	bytes = ISO_PATH_HEADER_BYTES + 2;

	cursor = 0;
	for (number = 1;; number++) {
		status = fsimg_stream_next(paths, &cursor, path, sizeof(path),
		    &length, error);
		if (status == FSIMG_DONE)
			break;
		if (status == FSIMG_OK)
			status = fsimg_session_charge(session, 0, 0, 1, error);
		if (status != FSIMG_OK)
			return (status);
		id = fsimg_load_le64(path);
		status = iso_row_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		names = row.group[hierarchy];
		for (uint32_t i = 0; i < row.children[hierarchy]; i++) {
			status = iso_name_next(plan->names[hierarchy], &names,
			    raw, &name, error);
			if (status != FSIMG_OK)
				return (status == FSIMG_DONE ? FSIMG_ECORRUPT :
				    status);
			if (!iso_name_directory(&name))
				continue;
			if (count == ISO_DIRECTORIES_MAX)
				return (fsimg_error_set(error,
				    FSIMG_EUNSUPPORTED, 0, FSIMG_OP_WRITER,
				    "ISO 9660 hierarchy has more than 65535 "
				    "directories"));
			count++;
			fsimg_store_le64(path, name.id);
			fsimg_store_le16(path + 8, (uint16_t)number);
			bcopy(name.identifier, path + ISO_PATH_STREAM_HEADER,
			    name.length);
			status = fsimg_stream_append(paths, path,
			    ISO_PATH_STREAM_HEADER + name.length, NULL, error);
			if (status == FSIMG_OK)
				status = iso_row_get(plan, name.id, &child,
				    error);
			if (status != FSIMG_OK)
				return (status);
			child.number[hierarchy] = (uint16_t)count;
			status = iso_row_set(plan, name.id, &child, error);
			if (status != FSIMG_OK)
				return (status);
			bytes += ISO_PATH_HEADER_BYTES + name.length +
			    name.length % 2;
		}
	}
	plan->directories[hierarchy] = count;
	plan->path_bytes[hierarchy] = (uint32_t)bytes;
	return (FSIMG_OK);
}

/*
 * Pass 5: sizing.
 */

/* Size a dot record of the directory whose row is self: its Rock Ridge
 * entries, PX and TF of the directory it names, which row describes; the
 * root's first dot record adds SP and ER and is the one dot record whose
 * entries continue, its split kept in the root's row.
 */
static fsimg_status_t
size_dot(iso_plan_t *plan, iso_row_t *row, iso_rr_record_t record,
    uint64_t *position, fsimg_error_t *error)
{
	iso_rr_input_t input = { .row = row, .record = record };
	iso_split_t split;
	size_t length;
	fsimg_status_t status;

	length = iso_rr_encode(&input, plan->entries);
	status = iso_susp_split(plan->entries, length, iso_record_room(1),
	    &split, error);
	if (status != FSIMG_OK)
		return (status);
	if (record == ISO_RR_ROOT_SELF) {
		row->child.record_entries = (uint8_t)split.record_bytes;
		row->child.fragments = (uint8_t)split.fragments;
		for (unsigned i = 0; i < split.fragments; i++)
			row->child.fragment_bytes[i] = split.fragment_bytes[i];
		row->child.fragment_position = iso_place(&plan->continuation_bytes,
		    split.fragment_bytes[0]);
		for (unsigned i = 1; i < split.fragments; i++)
			iso_place(&plan->continuation_bytes,
			    split.fragment_bytes[i]);
	} else if (split.fragments != 0) {
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_WRITER, "ISO 9660 dot record entries continue"));
	}
	iso_place(position, iso_record_length(1, split.record_bytes));
	return (FSIMG_OK);
}

/* Size one child's records at *position. Every primary section carries
 * the same entries and split, with separate continuation areas. One
 * pattern and its first position describe all sections without an index.
 */
static fsimg_status_t
size_child(iso_plan_t *plan, iso_hierarchy_t hierarchy,
    const iso_name_t *name, uint64_t *position, fsimg_error_t *error)
{
	iso_split_t split;
	iso_record_plan_t *record;
	iso_row_t row;
	size_t length;
	uint32_t sections;
	fsimg_status_t status;

	status = iso_row_get(plan, name->id, &row, error);
	if (status != FSIMG_OK)
		return (status);
	sections = row.kind == FSIMG_ENTRY_FILE ? iso_sections(row.size) : 1;
	bzero(&split, sizeof(split));
	if (hierarchy == ISO_PRIMARY) {
		status = iso_child_entries(plan, name, &row, &length,
		    error);
		if (status == FSIMG_OK)
			status = iso_susp_split(plan->entries, length,
			    iso_record_room(name->length), &split, error);
		if (status != FSIMG_OK)
			return (status);
		record = name->role == ISO_NAME_PLACEHOLDER ?
		    &row.placeholder : &row.child;
		record->record_entries = (uint8_t)split.record_bytes;
		record->fragments = (uint8_t)split.fragments;
		for (unsigned i = 0; i < split.fragments; i++)
			record->fragment_bytes[i] = split.fragment_bytes[i];
		for (uint32_t section = 0; section < sections; section++) {
			for (unsigned i = 0; i < split.fragments; i++) {
				uint64_t placed = iso_place(&plan->continuation_bytes,
				    split.fragment_bytes[i]);

				if (section == 0 && i == 0)
					record->fragment_position = placed;
			}
		}
		status = iso_row_set(plan, name->id, &row, error);
		if (status != FSIMG_OK)
			return (status);
	}
	for (uint32_t i = 0; i < sections; i++)
		iso_place(position, iso_record_length(name->length, split.record_bytes));
	return (FSIMG_OK);
}

/* Size directory id in a hierarchy: its two dot records, then its
 * children's records in name-stream order, packed into whole blocks with
 * no record crossing one. Its length is those blocks.
 */
static fsimg_status_t
size_directory(iso_plan_t *plan, iso_hierarchy_t hierarchy, uint64_t id,
    fsimg_error_t *error)
{
	unsigned char raw[ISO_NAME_RECORD_MAX];
	iso_name_t name;
	iso_row_t row, parent;
	uint64_t position, cursor, bytes;
	fsimg_status_t status;

	status = iso_row_get(plan, id, &row, error);
	if (status != FSIMG_OK)
		return (status);
	position = 0;
	if (hierarchy == ISO_PRIMARY) {
		parent = row;
		if (id != 0) {
			status = iso_row_get(plan, row.physical_parent, &parent, error);
			if (status != FSIMG_OK)
				return (status);
		}
		status = size_dot(plan, &row, id == 0 ? ISO_RR_ROOT_SELF :
		    ISO_RR_DOT, &position, error);
		if (status == FSIMG_OK)
			status = size_dot(plan, &parent,
			    row.relocation_alias != 0 ? ISO_RR_PARENT_LINK : ISO_RR_DOT, &position,
			    error);
		if (status != FSIMG_OK)
			return (status);
	} else {
		iso_place(&position, iso_record_length(1, 0));
		iso_place(&position, iso_record_length(1, 0));
	}

	cursor = row.group[hierarchy];
	for (uint32_t i = 0; i < row.children[hierarchy]; i++) {
		status = iso_name_next(plan->names[hierarchy], &cursor, raw,
		    &name, error);
		if (status != FSIMG_OK)
			return (status == FSIMG_DONE ? FSIMG_ECORRUPT : status);
		status = fsimg_session_charge(&plan->common.writer->session,
		    0, 0, 1, error);
		if (status != FSIMG_OK)
			return (status);
		status = size_child(plan, hierarchy, &name, &position, error);
		if (status != FSIMG_OK)
			return (status);
	}
	bytes = ISO_BLOCKS(position) * ISO_BLOCK_BYTES;
	if (bytes > UINT32_MAX - ISO_BLOCK_BYTES + 1) {
		status = fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_WRITER, "ISO 9660 directory exceeds 4 GiB");
		return (status);
	}
	row.bytes[hierarchy] = (uint32_t)bytes;
	return (iso_row_set(plan, id, &row, error));
}

/* Pass 5 for one hierarchy: every directory in path-table order. */
static fsimg_status_t
size_directories(iso_plan_t *plan, iso_hierarchy_t hierarchy,
    fsimg_error_t *error)
{
	unsigned char path[ISO_PATH_STREAM_MAX];
	uint64_t cursor;
	size_t length;
	fsimg_status_t status;

	cursor = 0;
	for (;;) {
		status = fsimg_stream_next(plan->paths[hierarchy], &cursor,
		    path, sizeof(path), &length, error);
		if (status == FSIMG_DONE)
			return (FSIMG_OK);
		if (status != FSIMG_OK)
			return (status);
		status = size_directory(plan, hierarchy, fsimg_load_le64(path),
		    error);
		if (status != FSIMG_OK)
			return (status);
	}
}

/*
 * Pass 6: layout.
 */

/* Advance the block cursor by blocks, within the format's 2^32 - 1. */
static fsimg_status_t
advance(uint64_t *next, uint64_t blocks, fsimg_error_t *error)
{
	if (blocks > ISO_VOLUME_BLOCKS_MAX - *next)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER,
		    "ISO 9660 image exceeds 2^32 - 1 blocks"));
	*next += blocks;
	return (FSIMG_OK);
}

/* Give a hierarchy's directories their extents in path-table order. */
static fsimg_status_t
place_directories(iso_plan_t *plan, iso_hierarchy_t hierarchy,
    uint64_t *next, fsimg_error_t *error)
{
	unsigned char path[ISO_PATH_STREAM_MAX];
	iso_row_t row;
	uint64_t cursor, id;
	size_t length;
	fsimg_status_t status;

	cursor = 0;
	for (;;) {
		status = fsimg_stream_next(plan->paths[hierarchy], &cursor,
		    path, sizeof(path), &length, error);
		if (status == FSIMG_DONE)
			return (FSIMG_OK);
		if (status != FSIMG_OK)
			return (status);
		id = fsimg_load_le64(path);
		status = iso_row_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		row.block[hierarchy] = (uint32_t)*next;
		status = advance(next, row.bytes[hierarchy] / ISO_BLOCK_BYTES,
		    error);
		if (status == FSIMG_OK)
			status = iso_row_set(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
	}
}

/* Give each nonempty file its contiguous sections in catalog ID order.
 * An empty file's record names block 0.
 */
static fsimg_status_t
place_files(iso_plan_t *plan, uint64_t *next, fsimg_error_t *error)
{
	iso_row_t row;
	uint64_t blocks;
	fsimg_status_t status;

	for (uint64_t id = 1; id <= plan->common.writer->count; id++) {
		status = fsimg_session_charge(&plan->common.writer->session, 0,
		    0, 1, error);
		if (status == FSIMG_OK)
			status = iso_row_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		if (row.kind != FSIMG_ENTRY_FILE || row.size == 0)
			continue;
		blocks = ISO_BLOCKS(row.size);
		row.first_block = (uint32_t)*next;
		status = advance(next, blocks, error);
		if (status == FSIMG_OK)
			status = iso_row_set(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		plan->data_blocks += blocks;
	}
	return (FSIMG_OK);
}

/* Pass 6: block numbers in the profile's order. A requested volume size
 * must hold the image and pads it with zero blocks the volume counts.
 */
static fsimg_status_t
layout(iso_plan_t *plan, fsimg_error_t *error)
{
	uint64_t next, requested, table_blocks;
	fsimg_status_t status;

	/* Three descriptors, plus the boot descriptor when images are selected. */
	next = ISO_SYSTEM_BLOCKS + 3 +
	    (plan->boot_bios != 0 || plan->boot_efi != 0 ? 1 : 0);
	for (unsigned h = 0; h < ISO_HIERARCHIES; h++) {
		table_blocks = ISO_BLOCKS(plan->path_bytes[h]);
		plan->path_l[h] = (uint32_t)next;
		plan->path_m[h] = (uint32_t)(next + table_blocks);
		next += 2 * table_blocks;
	}
	if (plan->boot_bios != 0 || plan->boot_efi != 0) {
		plan->boot_catalog_block = (uint32_t)next;
		next++;
	}
	status = place_directories(plan, ISO_PRIMARY, &next, error);
	if (status != FSIMG_OK)
		return (status);
	plan->continuation_block = (uint32_t)next;
	status = advance(&next, ISO_BLOCKS(plan->continuation_bytes), error);
	if (status != FSIMG_OK)
		return (status);
	plan->continuation_blocks = (uint32_t)ISO_BLOCKS(
	    plan->continuation_bytes);
	status = place_directories(plan, ISO_JOLIET, &next, error);
	if (status == FSIMG_OK)
		status = place_files(plan, &next, error);
	if (status != FSIMG_OK)
		return (status);

	requested = plan->common.writer->options.volume_bytes / ISO_BLOCK_BYTES;
	if (requested != 0 && requested < next)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER,
		    "ISO 9660 image does not fit the volume size"));
	if (requested > ISO_VOLUME_BLOCKS_MAX)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER,
		    "ISO 9660 volume size exceeds 2^32 - 1 blocks"));
	plan->volume_blocks = (uint32_t)(requested != 0 ? requested : next);
	plan->common.info.allocated_bytes = next * ISO_BLOCK_BYTES;
	return (FSIMG_OK);
}

/* Pass 7: the summary. Every block up to the last the layout used is
 * allocated, the system area included; metadata is all of it but file
 * data; the format has no free space.
 */
static void
summarize(iso_plan_t *plan)
{
	fsimg_plan_info_t *info = &plan->common.info;

	info->volume_bytes = (uint64_t)plan->volume_blocks * ISO_BLOCK_BYTES;
	info->sector_bytes = ISO_BLOCK_BYTES;
	info->allocation_unit_bytes = ISO_BLOCK_BYTES;
	info->allocation_units = plan->volume_blocks;
	info->metadata_bytes = info->allocated_bytes -
	    plan->data_blocks * ISO_BLOCK_BYTES;
	info->free_bytes = 0;
	info->scratch_bytes = plan->common.writer->ctx->stats.scratch_bytes;
}

static fsimg_status_t
make_plan(fsimg_writer_t *writer, fsimg_error_t *error)
{
	iso_plan_t *plan;
	fsimg_plan_info_t *info;
	fsimg_error_t detail;
	time_t now;
	fsimg_status_t status;

	plan = fsimg_alloc(writer->ctx, sizeof(*plan), &detail);
	if (plan == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	info = &plan->common.info;
	plan->common.writer = writer;
	strcpy(info->format, "iso9660");
	info->entries = writer->count;
	info->payload_bytes = writer->payload_bytes;
	info->required_io_flags = FSIMG_IO_WRITE;
	if ((writer->options.flags & FSIMG_WRITE_DURABLE) != 0)
		info->required_io_flags |= FSIMG_IO_DURABLE;
	plan->level = writer->options.iso_level;

	/* Absent modification times and the volume dates: the epoch when
	 * reproducible, otherwise the host's UTC clock at planning.
	 */
	if ((writer->options.flags & FSIMG_WRITE_REPRODUCIBLE) != 0) {
		plan->fallback = writer->options.epoch;
	} else {
		now = time(NULL);
		if ((int64_t)now < ISO_DATE7_MIN ||
		    (int64_t)now > ISO_DATE7_MAX) {
			status = fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_WRITER,
			    "host clock is outside the ISO 9660 date range");
			goto fail;
		}
		plan->fallback = (int64_t)now;
	}
	if (encode_descriptor(&writer->options, plan->fallback,
	    plan->descriptor, NULL) != FSIMG_OK) {
		status = fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "validated descriptor settings no longer encode");
		goto fail;
	}
	encode_labels(plan);

	status = fsimg_array_new(&writer->session, ISO_ROW_BYTES,
	    writer->count + 2, &plan->rows, error);
	if (status != FSIMG_OK)
		goto fail;
	status = plan_entries(plan, error);
	if (status != FSIMG_OK)
		goto fail;
	status = iso_write_names(plan, ISO_PRIMARY, error);
	if (status != FSIMG_OK)
		goto fail;
	status = relocate_directories(plan, error);
	if (status != FSIMG_OK)
		goto fail;
	status = iso_write_names(plan, ISO_JOLIET, error);
	if (status != FSIMG_OK)
		goto fail;
	for (unsigned h = 0; h < ISO_HIERARCHIES; h++) {
		status = number_directories(plan, (iso_hierarchy_t)h, error);
		if (status != FSIMG_OK)
			goto fail;
	}
	for (unsigned h = 0; h < ISO_HIERARCHIES; h++) {
		status = size_directories(plan, (iso_hierarchy_t)h, error);
		if (status != FSIMG_OK)
			goto fail;
	}
	status = layout(plan, error);
	if (status != FSIMG_OK)
		goto fail;
	summarize(plan);
	writer->plan = &plan->common;
	return (FSIMG_OK);
fail:
	free_plan(&plan->common);
	return (status);
}

static void
free_plan(fsimg_plan_t *base)
{
	iso_plan_t *plan = (iso_plan_t *)base;

	if (plan == NULL)
		return;
	for (unsigned h = 0; h < ISO_HIERARCHIES; h++) {
		fsimg_stream_free(plan->names[h]);
		fsimg_stream_free(plan->paths[h]);
	}
	fsimg_array_free(plan->rows);
	fsimg_dealloc(plan);
}
