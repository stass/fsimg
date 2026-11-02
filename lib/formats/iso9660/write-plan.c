/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>
#include <time.h>

#include "fsimg-endian.h"
#include "unicode.h"
#include "write-iso9660.h"

/*
 * Planning (specs/design-iso9660-driver.md#planning) runs fixed passes
 * over the frozen catalog. Every pass reads rows and streams the earlier
 * passes wrote, and every size is fixed before any block number, so each
 * pass runs once:
 *
 *   1. entries: one row per entry with its kind, size and encoded
 *	metadata, and each directory's link count;
 *   2. primary names (write-names.c), then the depth check: the writer
 *	does not relocate, so a directory the primary hierarchy could only
 *	hold by Rock Ridge relocation is refused;
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
static void encode_labels(iso_plan_t *);
static fsimg_status_t encode_time(iso_plan_t *, const fsimg_entry_t *,
    uint32_t, const fsimg_timestamp_t *, unsigned char *, bool *,
    fsimg_error_t *);
static fsimg_status_t encode_metadata(iso_plan_t *, const fsimg_entry_t *,
    iso_row_t *, fsimg_error_t *);
static fsimg_status_t plan_entry(iso_plan_t *, const fsimg_catalog_entry_t *,
    fsimg_error_t *);
static fsimg_status_t plan_entries(iso_plan_t *, fsimg_error_t *);
static fsimg_status_t check_depth(iso_plan_t *, fsimg_error_t *);
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
	row->fragment_position = fsimg_load_le64(raw + 24);
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
		row->fragment_bytes[i] = fsimg_load_le16(raw + 102 + 2 * i);
	row->target_bytes = fsimg_load_le16(raw + 108);
	row->level = raw[116];
	row->record_entries = raw[117];
	row->fragments = raw[118];
	row->times = raw[119];
	bcopy(raw + 120, row->modified, ISO_DATE7_BYTES);
	bcopy(raw + 127, row->accessed, ISO_DATE7_BYTES);
	bcopy(raw + 134, row->changed, ISO_DATE7_BYTES);
	if (row->fragments > ISO_FRAGMENTS_MAX)
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
	fsimg_store_le64(raw + 24, row->fragment_position);
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
		fsimg_store_le16(raw + 102 + 2 * i, row->fragment_bytes[i]);
	fsimg_store_le16(raw + 108, row->target_bytes);
	raw[116] = row->level;
	raw[117] = row->record_entries;
	raw[118] = row->fragments;
	raw[119] = row->times;
	bcopy(row->modified, raw + 120, ISO_DATE7_BYTES);
	bcopy(row->accessed, raw + 127, ISO_DATE7_BYTES);
	bcopy(row->changed, raw + 134, ISO_DATE7_BYTES);
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
 * read through the link-target read; its components are at most 255
 * bytes, the most libisofs reads, else EUNSUPPORTED. Planning stores the target's encoded
 * SL length and hash in row; emission requires the same, so a target
 * that changed between the two reads is a changed source.
 */
fsimg_status_t
iso_child_entries(iso_plan_t *plan, uint64_t id, iso_row_t *row,
    bool planning, size_t *length, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	const fsimg_catalog_entry_t *catalog;
	iso_rr_input_t input;
	size_t base, target_length;
	uint64_t hash;
	fsimg_status_t status;

	bzero(&input, sizeof(input));
	status = fsimg_catalog_get(writer, id, &catalog, error);
	if (status != FSIMG_OK)
		return (status);
	if (row->kind == FSIMG_ENTRY_SYMLINK) {
		status = fsimg_writer_link_target(writer, id, plan->target,
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
			status = fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
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
	input.name = strrchr(catalog->entry.path, '/') + 1;
	input.name_length = strlen(input.name);
	*length = iso_rr_encode(&input, plan->entries);
	if (row->kind != FSIMG_ENTRY_SYMLINK)
		return (FSIMG_OK);

	/* The SL entries are what follows the entries of the same record
	 * without a target.
	 */
	input.target_length = 0;
	base = iso_rr_encode(&input, plan->entries + *length);
	hash = iso_hash(plan->entries + base, *length - base);
	if (planning) {
		row->target_bytes = (uint16_t)(*length - base);
		row->target_hash = hash;
	} else if (row->target_bytes != *length - base ||
	    row->target_hash != hash) {
		status = fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
		    FSIMG_OP_WRITER, "ISO 9660 link target changed since planning");
		fsimg_error_path(error, catalog->entry.path);
		return (status);
	}
	return (FSIMG_OK);
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
	bzero(&row, sizeof(row));
	row.kind = entry->kind;
	row.links = entry->kind == FSIMG_ENTRY_DIRECTORY ? 2 : 1;
	status = encode_metadata(plan, entry, &row, error);
	if (status != FSIMG_OK)
		return (status);
	if (entry->kind == FSIMG_ENTRY_FILE) {
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
	for (uint64_t id = 1; status == FSIMG_OK && id <= writer->count;
	    id++) {
		status = fsimg_session_charge(&writer->session, 0, 0, 1, error);
		if (status == FSIMG_OK)
			status = fsimg_catalog_get(writer, id, &catalog, error);
		if (status == FSIMG_OK)
			status = plan_entry(plan, catalog, error);
	}
	return (status);
}

/*
 * Pass 2: the depth check. Each directory's physical level, the root
 * being 1, and physical path length, its parent's plus a separator below
 * the root and its primary identifier, follow from its parent's, which
 * catalog ID order computes first. ECMA-119 bounds the primary hierarchy
 * to 8 levels and 255-byte paths, so a directory at level 9, or whose
 * path leaves no room for a separator and the level's longest file
 * identifier, needs Rock Ridge relocation, which this writer does not
 * write: planning refuses it, naming the first such directory.
 */
static fsimg_status_t
check_depth(iso_plan_t *plan, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	const fsimg_catalog_entry_t *catalog;
	iso_row_t row, parent;
	uint32_t longest, path;
	fsimg_status_t status;

	longest = plan->level == 1 ? LEVEL1_FILE_IDENTIFIER :
	    FILE_IDENTIFIER_MAX;
	for (uint64_t id = 1; id <= writer->count; id++) {
		status = fsimg_catalog_get(writer, id, &catalog, error);
		if (status == FSIMG_OK)
			status = iso_row_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		if (row.kind != FSIMG_ENTRY_DIRECTORY)
			continue;
		status = iso_row_get(plan, catalog->parent_id, &parent, error);
		if (status != FSIMG_OK)
			return (status);
		path = parent.path_length + (catalog->parent_id == 0 ? 0 : 1) +
		    row.identifier[ISO_PRIMARY];
		if (parent.level + 1u > ISO_PHYSICAL_LEVEL_MAX ||
		    path > ISO_PATH_MAX - 1 - longest) {
			status = fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
			    FSIMG_OP_WRITER, "ISO 9660 directory needs Rock "
			    "Ridge relocation, which is not yet written");
			fsimg_error_path(error, catalog->entry.path);
			return (status);
		}
		row.level = (uint8_t)(parent.level + 1);
		row.path_length = (uint16_t)path;
		status = iso_row_set(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
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
			if (!name.directory)
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
		row->record_entries = (uint8_t)split.record_bytes;
		row->fragments = (uint8_t)split.fragments;
		for (unsigned i = 0; i < split.fragments; i++)
			row->fragment_bytes[i] = split.fragment_bytes[i];
		row->fragment_position = iso_place(&plan->continuation_bytes,
		    split.fragment_bytes[0]);
		for (unsigned i = 1; i < split.fragments; i++)
			iso_place(&plan->continuation_bytes,
			    split.fragment_bytes[i]);
	} else if (split.fragments != 0) {
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_WRITER, "ISO 9660 dot record entries continue"));
	}
	iso_place(position, iso_record_length(1, split.record_bytes));
	return (FSIMG_OK);
}

/* Size one child's records at *position in its directory. In the primary
 * hierarchy the first record carries the child's Rock Ridge entries and
 * the split, with its fragments placed, goes to the child's row; every
 * other record of a multi-section file is the bare record.
 */
static fsimg_status_t
size_child(iso_plan_t *plan, iso_hierarchy_t hierarchy,
    const iso_name_t *name, uint64_t *position, fsimg_error_t *error)
{
	iso_split_t split;
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
		status = iso_child_entries(plan, name->id, &row, true, &length,
		    error);
		if (status == FSIMG_OK)
			status = iso_susp_split(plan->entries, length,
			    iso_record_room(name->length), &split, error);
		if (status != FSIMG_OK)
			return (status);
		row.record_entries = (uint8_t)split.record_bytes;
		row.fragments = (uint8_t)split.fragments;
		for (unsigned i = 0; i < split.fragments; i++) {
			row.fragment_bytes[i] = split.fragment_bytes[i];
			if (i == 0)
				row.fragment_position = iso_place(
				    &plan->continuation_bytes,
				    split.fragment_bytes[i]);
			else
				iso_place(&plan->continuation_bytes,
				    split.fragment_bytes[i]);
		}
		status = iso_row_set(plan, name->id, &row, error);
		if (status != FSIMG_OK)
			return (status);
	}
	iso_place(position, iso_record_length(name->length,
	    split.record_bytes));
	for (uint32_t i = 1; i < sections; i++)
		iso_place(position, iso_record_length(name->length, 0));
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
	const fsimg_catalog_entry_t *catalog;
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
			status = fsimg_catalog_get(plan->common.writer, id,
			    &catalog, error);
			if (status == FSIMG_OK)
				status = iso_row_get(plan, catalog->parent_id,
				    &parent, error);
			if (status != FSIMG_OK)
				return (status);
		}
		status = size_dot(plan, &row, id == 0 ? ISO_RR_ROOT_SELF :
		    ISO_RR_DOT, &position, error);
		if (status == FSIMG_OK)
			status = size_dot(plan, &parent, ISO_RR_DOT, &position,
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
		if (status == FSIMG_OK)
			status = fsimg_session_charge(
			    &plan->common.writer->session, 0, 0, 1, error);
		if (status == FSIMG_OK)
			status = size_child(plan, hierarchy, &name, &position,
			    error);
		if (status != FSIMG_OK)
			return (status == FSIMG_DONE ? FSIMG_ECORRUPT : status);
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

	/* The primary descriptor, the Joliet descriptor and the terminator. */
	next = ISO_SYSTEM_BLOCKS + 3;
	for (unsigned h = 0; h < ISO_HIERARCHIES; h++) {
		table_blocks = ISO_BLOCKS(plan->path_bytes[h]);
		plan->path_l[h] = (uint32_t)next;
		plan->path_m[h] = (uint32_t)(next + table_blocks);
		next += 2 * table_blocks;
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
	iso_date17_encode(plan->fallback, plan->volume_date);
	encode_labels(plan);

	status = fsimg_array_new(&writer->session, ISO_ROW_BYTES,
	    writer->count + 1, &plan->rows, error);
	if (status == FSIMG_OK)
		status = plan_entries(plan, error);
	if (status == FSIMG_OK)
		status = iso_write_names(plan, ISO_PRIMARY, error);
	if (status == FSIMG_OK)
		status = check_depth(plan, error);
	if (status == FSIMG_OK)
		status = iso_write_names(plan, ISO_JOLIET, error);
	for (unsigned h = 0; status == FSIMG_OK && h < ISO_HIERARCHIES; h++)
		status = number_directories(plan, (iso_hierarchy_t)h, error);
	for (unsigned h = 0; status == FSIMG_OK && h < ISO_HIERARCHIES; h++)
		status = size_directories(plan, (iso_hierarchy_t)h, error);
	if (status == FSIMG_OK)
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
