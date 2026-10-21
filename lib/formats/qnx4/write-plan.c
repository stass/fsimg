/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>
#include <time.h>

#include "unicode.h"
#include "write-qnx4.h"

/*
 * Planning runs a fixed sequence of passes over the frozen catalog. The
 * entries pass fills one row per entry: its record kind (a direct record
 * for a name of 1 to 16 bytes, otherwise a link record with an .inodes
 * slot and, above 48 bytes, a long-name record), its slot in its parent,
 * its encoded metadata and its block count. The geometry pass sizes the
 * format-owned objects and chooses N. The layout pass gives every object
 * one contiguous extent in a fixed order -- the root directory at 3 + B,
 * .inodes, lost+found, .longfilenames, then every entry in catalog ID
 * order -- so blocks 1 to A are exactly the allocated set. The summary
 * fills the plan information. Rows are the only per-entry state.
 */

/* Root entries with a fixed place: /.boot and /.altboot supply root-block
 * records 2 and 3 and take no slot; the format-owned names are refused.
 */
typedef enum qnx4_root_name {
	QNX4_ROOT_ORDINARY,
	QNX4_ROOT_BOOT,
	QNX4_ROOT_ALTBOOT,
	QNX4_ROOT_RESERVED
} qnx4_root_name_t;

static fsimg_status_t validate_options(fsimg_write_options_t *, unsigned,
    fsimg_error_t *);
static fsimg_status_t make_plan(fsimg_writer_t *, fsimg_error_t *);
static void free_plan(fsimg_plan_t *);
static qnx4_root_name_t classify_root_name(const char *);
static fsimg_status_t optional_loss(qnx4_plan_t *, const fsimg_entry_t *,
    uint32_t, const char *, fsimg_error_t *);
static fsimg_status_t encode_time(qnx4_plan_t *, const fsimg_entry_t *,
    uint32_t, const fsimg_timestamp_t *, uint32_t *, fsimg_error_t *);
static fsimg_status_t encode_metadata(qnx4_plan_t *, const fsimg_entry_t *,
    uint16_t, qnx4_row_t *, fsimg_error_t *);
static bool object_fits(uint64_t);
static uint32_t directory_blocks(uint64_t);
static fsimg_status_t take_slot(qnx4_row_t *, uint64_t, fsimg_entry_kind_t,
    uint32_t *, fsimg_error_t *);
static fsimg_status_t plan_storage(qnx4_plan_t *, const fsimg_catalog_entry_t *,
    qnx4_row_t *, fsimg_error_t *);
static fsimg_status_t plan_entry(qnx4_plan_t *, const fsimg_catalog_entry_t *,
    fsimg_error_t *);
static fsimg_status_t plan_entries(qnx4_plan_t *, fsimg_error_t *);
static uint64_t bitmap_blocks(uint64_t);
static fsimg_status_t size_tables(qnx4_plan_t *, fsimg_error_t *);
static fsimg_status_t choose_geometry(qnx4_plan_t *, fsimg_error_t *);
static fsimg_status_t place_object(qnx4_plan_t *, uint64_t, uint64_t *,
    fsimg_error_t *);
static fsimg_status_t layout(qnx4_plan_t *, fsimg_error_t *);

const fsimg_writer_ops_t fsimg_qnx4_writer_ops = {
	.validate = validate_options,
	.plan = make_plan,
	.emit = qnx4_write_emit,
	.free = free_plan
};

/* The common options with the profile's rules: 512-byte sectors and
 * blocks, no FAT root, a 31-bit reproducible epoch (the range the profile
 * stores), an exact size of 43 to 2^31 whole blocks, an .inodes capacity
 * whose table size fits 32 bits, and a label of at most 48 bytes under the
 * name profile's byte rules. NULL selects the empty label, which leaves
 * the root block's label field zero as the formatter does without -l. The
 * volume serial is a common identity the profile does not store.
 */
static fsimg_status_t
validate_options(fsimg_write_options_t *options, unsigned variant FSIMG_UNUSED,
    fsimg_error_t *error)
{
	static const char empty_label[] = "";
	uint64_t blocks;
	size_t length;

	blocks = options->volume_bytes / QNX4_BLOCK_BYTES;
	if ((options->sector_bytes != 0 && options->sector_bytes != 512) ||
	    (options->allocation_unit_bytes != 0 &&
	    options->allocation_unit_bytes != QNX4_BLOCK_BYTES) ||
	    options->fat_root_entries != 0 ||
	    (options->volume_bytes != 0 &&
	    (options->volume_bytes % QNX4_BLOCK_BYTES != 0 ||
	    blocks < QNX4_BLOCKS_MIN || blocks > QNX4_BLOCKS_MAX)) ||
	    ((options->flags & FSIMG_WRITE_REPRODUCIBLE) != 0 &&
	    (options->epoch < 0 || options->epoch > QNX4_TIME_MAX)) ||
	    options->inode_capacity > QNX4_INODE_SLOTS_MAX)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER,
		    "invalid QNX4 geometry, settings or reproducible epoch"));
	if (options->label != NULL) {
		length = strnlen(options->label, QNX4_LABEL_BYTES + 1);
		if (length > QNX4_LABEL_BYTES ||
		    !qnx4_label_valid((const unsigned char *)options->label,
		    length))
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_WRITER, "invalid QNX4 volume label"));
	} else {
		options->label = empty_label;
	}
	options->sector_bytes = 512;
	options->allocation_unit_bytes = QNX4_BLOCK_BYTES;
	return (FSIMG_OK);
}

fsimg_status_t
qnx4_row_get(qnx4_plan_t *plan, uint64_t id, qnx4_row_t *row,
    fsimg_error_t *error)
{
	unsigned char raw[QNX4_ROW_BYTES];
	fsimg_status_t status;

	status = fsimg_array_get(plan->rows, id, raw, error);
	if (status != FSIMG_OK)
		return (status);
	row->size = fsimg_meta_get_u32(raw);
	row->first = fsimg_meta_get_u32(raw + 4);
	row->blocks = fsimg_meta_get_u32(raw + 8);
	row->record_block = fsimg_meta_get_u32(raw + 12);
	row->slot = fsimg_meta_get_u32(raw + 16);
	row->next_slot = fsimg_meta_get_u32(raw + 20);
	row->inode_slot = fsimg_meta_get_u32(raw + 24);
	row->long_name = fsimg_meta_get_u32(raw + 28);
	row->created = fsimg_meta_get_u32(raw + 32);
	row->modified = fsimg_meta_get_u32(raw + 36);
	row->accessed = fsimg_meta_get_u32(raw + 40);
	row->changed = fsimg_meta_get_u32(raw + 44);
	row->uid = qnx4_u16(raw + 48);
	row->gid = qnx4_u16(raw + 50);
	row->mode = qnx4_u16(raw + 52);
	row->links = qnx4_u16(raw + 54);
	row->record_slot = raw[56];
	row->status = raw[57];
	return (FSIMG_OK);
}

fsimg_status_t
qnx4_row_set(qnx4_plan_t *plan, uint64_t id, const qnx4_row_t *row,
    fsimg_error_t *error)
{
	unsigned char raw[QNX4_ROW_BYTES];

	bzero(raw, sizeof(raw));
	fsimg_meta_put_u32(raw, row->size);
	fsimg_meta_put_u32(raw + 4, row->first);
	fsimg_meta_put_u32(raw + 8, row->blocks);
	fsimg_meta_put_u32(raw + 12, row->record_block);
	fsimg_meta_put_u32(raw + 16, row->slot);
	fsimg_meta_put_u32(raw + 20, row->next_slot);
	fsimg_meta_put_u32(raw + 24, row->inode_slot);
	fsimg_meta_put_u32(raw + 28, row->long_name);
	fsimg_meta_put_u32(raw + 32, row->created);
	fsimg_meta_put_u32(raw + 36, row->modified);
	fsimg_meta_put_u32(raw + 40, row->accessed);
	fsimg_meta_put_u32(raw + 44, row->changed);
	qnx4_put16(raw + 48, row->uid);
	qnx4_put16(raw + 50, row->gid);
	qnx4_put16(raw + 52, row->mode);
	qnx4_put16(raw + 54, row->links);
	raw[56] = row->record_slot;
	raw[57] = row->status;
	return (fsimg_array_set(plan->rows, id, raw, error));
}

static qnx4_root_name_t
classify_root_name(const char *name)
{
	if (strcmp(name, ".boot") == 0)
		return (QNX4_ROOT_BOOT);
	if (strcmp(name, ".altboot") == 0)
		return (QNX4_ROOT_ALTBOOT);
	if (strcmp(name, ".bitmap") == 0 || strcmp(name, ".inodes") == 0 ||
	    strcmp(name, ".longfilenames") == 0 ||
	    strcmp(name, "lost+found") == 0)
		return (QNX4_ROOT_RESERVED);
	return (QNX4_ROOT_ORDINARY);
}

/* A present field the profile cannot hold exactly: a failure when the
 * field is required or the caller forbids losses, otherwise one counted
 * and reported loss.
 */
static fsimg_status_t
optional_loss(qnx4_plan_t *plan, const fsimg_entry_t *entry, uint32_t field,
    const char *message, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;

	if ((entry->metadata.required & field) != 0 ||
	    (writer->options.flags & FSIMG_WRITE_ALLOW_LOSS) == 0) {
		fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0, FSIMG_OP_WRITER,
		    message);
		fsimg_error_path(error, entry->path);
		return (FSIMG_EUNSUPPORTED);
	}
	plan->common.info.metadata_losses++;
	fsimg_report_loss(writer->ctx, entry->path, field);
	return (FSIMG_OK);
}

/* Encode one timestamp field as unsigned UTC seconds. An absent field, or
 * one outside 0 through 2147483647, takes the format time; reproducible
 * mode replaces every time with the epoch, which validation keeps in that
 * range. A present field is exact when it is a UTC instant with whole
 * seconds in range (and, when reproducible, equal to the epoch); otherwise
 * it is an optional loss.
 */
static fsimg_status_t
encode_time(qnx4_plan_t *plan, const fsimg_entry_t *entry, uint32_t field,
    const fsimg_timestamp_t *time, uint32_t *out, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	bool exact, representable, reproducible;

	reproducible = (writer->options.flags & FSIMG_WRITE_REPRODUCIBLE) != 0;
	representable = time->seconds >= 0 && time->seconds <= QNX4_TIME_MAX;
	*out = plan->format_time;
	if ((entry->metadata.valid & field) == 0)
		return (FSIMG_OK);
	if (representable && !reproducible)
		*out = (uint32_t)time->seconds;
	exact = representable && time->basis == FSIMG_TIME_UTC &&
	    time->nanoseconds == 0 && time->seconds == (int64_t)*out;
	if (exact)
		return (FSIMG_OK);
	return (optional_loss(plan, entry, field,
	    "QNX4 cannot preserve the requested timestamp", error));
}

/* Encode an entry's owner, mode and times into its row. permissions is
 * the default for an absent mode. An owner or group above 65534, which the
 * native filesystem refuses, is stored as 0 under the loss policy.
 */
static fsimg_status_t
encode_metadata(qnx4_plan_t *plan, const fsimg_entry_t *entry,
    uint16_t permissions, qnx4_row_t *row, fsimg_error_t *error)
{
	const fsimg_metadata_t *m = &entry->metadata;
	uint16_t type;
	fsimg_status_t status;

	switch (entry->kind) {
	case FSIMG_ENTRY_FILE:
		type = QNX4_MODE_FILE;
		break;
	case FSIMG_ENTRY_DIRECTORY:
		type = QNX4_MODE_DIRECTORY;
		break;
	case FSIMG_ENTRY_SYMLINK:
		type = QNX4_MODE_SYMLINK;
		break;
	case FSIMG_ENTRY_FIFO:
		type = QNX4_MODE_FIFO;
		break;
	default:
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "QNX4 planning received an unknown entry kind"));
	}
	if ((m->valid & FSIMG_META_MODE) != 0)
		permissions = (uint16_t)(m->mode & FSIMG_MODE_BITS);
	row->mode = (uint16_t)(type | permissions);

	row->uid = 0;
	row->gid = 0;
	if ((m->valid & FSIMG_META_OWNER) != 0) {
		if (m->uid <= QNX4_OWNER_MAX)
			row->uid = (uint16_t)m->uid;
		if (m->gid <= QNX4_OWNER_MAX)
			row->gid = (uint16_t)m->gid;
		if (m->uid > QNX4_OWNER_MAX || m->gid > QNX4_OWNER_MAX) {
			status = optional_loss(plan, entry, FSIMG_META_OWNER,
			    "QNX4 cannot store an owner or group above 65534",
			    error);
			if (status != FSIMG_OK)
				return (status);
		}
	}

	status = encode_time(plan, entry, FSIMG_META_CREATED, &m->created,
	    &row->created, error);
	if (status != FSIMG_OK)
		return (status);
	status = encode_time(plan, entry, FSIMG_META_MODIFIED, &m->modified,
	    &row->modified, error);
	if (status != FSIMG_OK)
		return (status);
	status = encode_time(plan, entry, FSIMG_META_ACCESSED, &m->accessed,
	    &row->accessed, error);
	if (status != FSIMG_OK)
		return (status);
	return (encode_time(plan, entry, FSIMG_META_CHANGED, &m->changed,
	    &row->changed, error));
}

/* Whether an object of `blocks` whole blocks stays within the QNX4 object
 * maximum of 2^31 - 1 bytes, which readers enforce on every record. Every
 * object the writer sizes in blocks, the directories and both tables,
 * passes through it; files are bounded by their byte size.
 */
static bool
object_fits(uint64_t blocks)
{
	return (blocks <= QNX4_FILE_MAX / QNX4_BLOCK_BYTES);
}

/* The blocks of a directory of the given slot count: whole 4096-byte
 * steps, at least one. Zero when the directory would not fit.
 */
static uint32_t
directory_blocks(uint64_t slots)
{
	uint64_t steps;

	steps = slots / QNX4_SLOTS_PER_STEP +
	    (slots % QNX4_SLOTS_PER_STEP != 0);
	if (steps == 0)
		steps = 1;
	if (!object_fits(steps * QNX4_DIRECTORY_STEP))
		return (0);
	return ((uint32_t)steps * QNX4_DIRECTORY_STEP);
}

/* Assign the next slot of directory directory_id to a child of the given
 * kind; a subdirectory counts as one more link. The root's slots 0 to 6
 * are fixed and slot 8 is .longfilenames, so its children take slot 7 and
 * then 9 on.
 */
static fsimg_status_t
take_slot(qnx4_row_t *directory, uint64_t directory_id,
    fsimg_entry_kind_t child_kind, uint32_t *slot, fsimg_error_t *error)
{
	bool subdirectory = child_kind == FSIMG_ENTRY_DIRECTORY;

	if (directory->next_slot == UINT32_MAX ||
	    (subdirectory && directory->links == QNX4_LINKS_MAX))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER,
		    "QNX4 directory has too many entries or subdirectories"));
	*slot = directory->next_slot++;
	if (directory_id == 0 && directory->next_slot == QNX4_ROOT_NAMES_SLOT)
		directory->next_slot++;
	if (subdirectory)
		directory->links++;
	return (FSIMG_OK);
}

/* An entry's size and block count, and its share of the totals: a file's
 * or symbolic link's content, a directory's slots ("." and ".." with one
 * per child) in whole 4096-byte steps, nothing for a FIFO.
 */
static fsimg_status_t
plan_storage(qnx4_plan_t *plan, const fsimg_catalog_entry_t *catalog,
    qnx4_row_t *row, fsimg_error_t *error)
{
	const fsimg_entry_t *entry = &catalog->entry;
	uint64_t blocks;

	switch (entry->kind) {
	case FSIMG_ENTRY_DIRECTORY:
		blocks = directory_blocks(catalog->children + 2);
		if (blocks == 0 || catalog->children > UINT32_MAX - 2) {
			fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER,
			    "QNX4 directory exceeds 2 GiB");
			fsimg_error_path(error, entry->path);
			return (FSIMG_ERANGE);
		}
		row->blocks = (uint32_t)blocks;
		row->size = row->blocks * QNX4_BLOCK_BYTES;
		row->next_slot = 2;
		break;
	case FSIMG_ENTRY_FILE:
	case FSIMG_ENTRY_SYMLINK:
		if (entry->size > QNX4_FILE_MAX) {
			fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
			    FSIMG_OP_WRITER, "QNX4 files are limited to 2^31 - 1 bytes");
			fsimg_error_path(error, entry->path);
			return (FSIMG_EUNSUPPORTED);
		}
		row->size = (uint32_t)entry->size;
		blocks = entry->size / QNX4_BLOCK_BYTES +
		    (entry->size % QNX4_BLOCK_BYTES != 0);
		row->blocks = (uint32_t)blocks;
		plan->payload_blocks += blocks;
		plan->common.info.initialized_bytes += entry->size;
		break;
	default:
		/* A FIFO has no content and no extent. */
		blocks = 0;
		break;
	}
	plan->objects += blocks;
	return (FSIMG_OK);
}

/* Plan one catalog entry: check its name, fix its record kind and its slot
 * in its parent, encode its metadata and size its storage. /.boot and
 * /.altboot are regular files whose records are root-block records 2 and
 * 3; they take no slot, keep the formatter's permissions unless the entry
 * gives a mode, and are marked written only when they hold content.
 */
static fsimg_status_t
plan_entry(qnx4_plan_t *plan, const fsimg_catalog_entry_t *catalog,
    fsimg_error_t *error)
{
	const fsimg_entry_t *entry = &catalog->entry;
	qnx4_root_name_t role;
	qnx4_row_t row, parent;
	const char *name;
	size_t length;
	uint16_t permissions;
	fsimg_status_t status;
	bool indirect;

	status = fsimg_session_charge(&plan->common.writer->session, 0, 0, 1,
	    error);
	if (status != FSIMG_OK)
		return (status);
	name = strrchr(entry->path, '/') + 1;
	length = strlen(name);
	if (length > QNX4_NAME_MAX || !fsimg_name_in_qnx_profile(name, length)) {
		fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER,
		    "name is outside the QNX4 name profile");
		fsimg_error_path(error, entry->path);
		return (FSIMG_EINVAL);
	}
	role = catalog->parent_id == 0 ? classify_root_name(name) :
	    QNX4_ROOT_ORDINARY;
	if (role == QNX4_ROOT_RESERVED) {
		fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER,
		    "QNX4 reserves this root name for a format-owned file");
		fsimg_error_path(error, entry->path);
		return (FSIMG_EINVAL);
	}
	if ((role == QNX4_ROOT_BOOT || role == QNX4_ROOT_ALTBOOT) &&
	    entry->kind != FSIMG_ENTRY_FILE) {
		fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0, FSIMG_OP_WRITER,
		    "QNX4 reserves /.boot and /.altboot for regular files");
		fsimg_error_path(error, entry->path);
		return (FSIMG_EUNSUPPORTED);
	}

	bzero(&row, sizeof(row));
	row.inode_slot = QNX4_NONE;
	row.long_name = QNX4_NONE;
	row.links = entry->kind == FSIMG_ENTRY_DIRECTORY ? 2 : 1;
	switch (entry->kind) {
	case FSIMG_ENTRY_FILE:
		permissions = 0644;
		break;
	case FSIMG_ENTRY_DIRECTORY:
		permissions = 0755;
		break;
	default:
		permissions = 0777;
		break;
	}
	if (role == QNX4_ROOT_BOOT || role == QNX4_ROOT_ALTBOOT)
		permissions = QNX4_BOOT_PERMISSIONS;
	status = encode_metadata(plan, entry, permissions, &row, error);
	if (status == FSIMG_OK)
		status = plan_storage(plan, catalog, &row, error);
	if (status != FSIMG_OK)
		return (status);

	if (role == QNX4_ROOT_BOOT || role == QNX4_ROOT_ALTBOOT) {
		row.status = QNX4_STATUS_TABLE | QNX4_STATUS_USED;
		if (row.size != 0)
			row.status |= QNX4_STATUS_MODIFIED;
		if (role == QNX4_ROOT_BOOT)
			plan->boot_row = catalog->id;
		else
			plan->altboot_row = catalog->id;
		return (qnx4_row_set(plan, catalog->id, &row, error));
	}

	/* A name of 17 bytes or more is a link record naming an .inodes slot,
	 * assigned in catalog ID order from slot 1; above 48 bytes the name
	 * itself goes to the next long-name record.
	 */
	status = qnx4_row_get(plan, catalog->parent_id, &parent, error);
	if (status == FSIMG_OK)
		status = take_slot(&parent, catalog->parent_id, entry->kind,
		    &row.slot, error);
	if (status == FSIMG_OK)
		status = qnx4_row_set(plan, catalog->parent_id, &parent, error);
	if (status != FSIMG_OK)
		return (status);
	indirect = length > QNX4_DIRECT_NAME_MAX;
	if (indirect) {
		if (plan->used_inode_slots == QNX4_INODE_SLOTS_MAX - 1)
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_WRITER, "QNX4 entries exceed the .inodes table"));
		row.inode_slot = ++plan->used_inode_slots;
	}
	if (length > QNX4_LINK_NAME_MAX)
		row.long_name = plan->long_names++;
	row.status = QNX4_STATUS_USED;
	if (indirect)
		row.status |= QNX4_STATUS_TABLE;
	if (entry->kind == FSIMG_ENTRY_FILE)
		row.status |= QNX4_STATUS_MODIFIED;
	return (qnx4_row_set(plan, catalog->id, &row, error));
}

/* Pass 1: one row per entry in catalog ID order, then the root's own size,
 * which follows from the slots its children took: at least the nine fixed
 * ones, slots 0 to 8.
 */
static fsimg_status_t
plan_entries(qnx4_plan_t *plan, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	const fsimg_catalog_entry_t *catalog;
	qnx4_row_t root;
	uint32_t slots;
	fsimg_status_t status;

	root = (qnx4_row_t){
		.record_block = QNX4_ROOT_BLOCK,
		.record_slot = QNX4_RECORD_ROOT,
		.next_slot = QNX4_ROOT_ENTRY_SLOT,
		.inode_slot = QNX4_NONE,
		.long_name = QNX4_NONE,
		.created = plan->format_time,
		.modified = plan->format_time,
		.accessed = plan->format_time,
		.changed = plan->format_time,
		.mode = QNX4_ROOT_MODE,
		/* ".", the root's own name and lost+found's "..". */
		.links = 3,
		.status = QNX4_STATUS_USED | QNX4_STATUS_CLEAN
	};
	status = qnx4_row_set(plan, 0, &root, error);
	if (status != FSIMG_OK)
		return (status);
	for (uint64_t id = 1; id <= writer->count; id++) {
		status = fsimg_catalog_get(writer, id, &catalog, error);
		if (status != FSIMG_OK)
			return (status);
		status = plan_entry(plan, catalog, error);
		if (status != FSIMG_OK)
			return (status);
	}
	status = qnx4_row_get(plan, 0, &root, error);
	if (status != FSIMG_OK)
		return (status);
	slots = root.next_slot > QNX4_ROOT_NAMES_SLOT + 1 ? root.next_slot :
	    QNX4_ROOT_NAMES_SLOT + 1;
	root.blocks = directory_blocks(slots);
	if (root.blocks == 0)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER,
		    "QNX4 root directory exceeds 2 GiB"));
	root.size = root.blocks * QNX4_BLOCK_BYTES;
	plan->root_blocks = root.blocks;
	return (qnx4_row_set(plan, 0, &root, error));
}

/* B(N), the bitmap's blocks for N blocks: one bit per block. */
static uint64_t
bitmap_blocks(uint64_t blocks)
{
	return (blocks / QNX4_BITS_PER_BLOCK +
	    (blocks % QNX4_BITS_PER_BLOCK != 0));
}

/* The format-owned tables: .inodes holds max(128, inodes=, 1 + used) slots
 * rounded up to a whole block, and .longfilenames one block per long name
 * in 16-block steps, none without long names.
 */
static fsimg_status_t
size_tables(qnx4_plan_t *plan, fsimg_error_t *error)
{
	uint64_t slots, names;

	slots = QNX4_INODES_MIN;
	if (plan->common.writer->options.inode_capacity > slots)
		slots = plan->common.writer->options.inode_capacity;
	if ((uint64_t)plan->used_inode_slots + 1 > slots)
		slots = (uint64_t)plan->used_inode_slots + 1;
	slots = (slots + QNX4_SLOTS_PER_BLOCK - 1) / QNX4_SLOTS_PER_BLOCK *
	    QNX4_SLOTS_PER_BLOCK;
	names = ((uint64_t)plan->long_names + QNX4_NAMES_STEP - 1) /
	    QNX4_NAMES_STEP * QNX4_NAMES_STEP;
	if (!object_fits(slots / QNX4_SLOTS_PER_BLOCK))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER,
		    "QNX4 .inodes exceeds 2 GiB"));
	if (!object_fits(names))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER,
		    "QNX4 .longfilenames exceeds 2 GiB"));
	plan->inode_slots = (uint32_t)slots;
	plan->inodes_blocks = plan->inode_slots / QNX4_SLOTS_PER_BLOCK;
	plan->names_blocks = (uint32_t)names;
	return (FSIMG_OK);
}

/* Pass 2: N. The plan's demand D is every block after the loader, the root
 * block and the bitmap: the root directory, .inodes, lost+found,
 * .longfilenames, the objects and the requested free space. N blocks hold
 * the plan when 2 + B(N) + D <= N. An exact size must hold it. Otherwise
 * the search starts at 3 + D, the least any volume needs, and raises N to
 * 2 + B(N) + D until it holds. Each step adds only the bitmap's growth,
 * one block per 4096, and never passes the smallest fitting N: if N is at
 * most that smallest N*, then 2 + B(N) + D <= 2 + B(N*) + D <= N*. So the
 * search ends, within a few steps, exactly at N*. An empty catalog needs
 * D = 40 and ends at the formatter's 43-block minimum.
 */
static fsimg_status_t
choose_geometry(qnx4_plan_t *plan, fsimg_error_t *error)
{
	const fsimg_write_options_t *options = &plan->common.writer->options;
	qnx4_geometry_t *g = &plan->geometry;
	uint64_t demand, free_blocks, blocks, needed;

	free_blocks = options->min_free_bytes / QNX4_BLOCK_BYTES +
	    (options->min_free_bytes % QNX4_BLOCK_BYTES != 0);
	demand = (uint64_t)plan->root_blocks + plan->inodes_blocks +
	    QNX4_LOST_FOUND_BLOCKS + plan->names_blocks;
	if (!fsimg_add_u64(demand, plan->objects, &demand) ||
	    !fsimg_add_u64(demand, free_blocks, &demand) ||
	    demand > QNX4_BLOCKS_MAX)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER,
		    "QNX4 content and free space exceed the largest volume"));
	if (options->volume_bytes != 0) {
		blocks = options->volume_bytes / QNX4_BLOCK_BYTES;
		if (2 + bitmap_blocks(blocks) + demand > blocks)
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_WRITER,
			    "QNX4 content and free space do not fit the volume size"));
	} else {
		blocks = 3 + demand;
		for (;;) {
			needed = 2 + bitmap_blocks(blocks) + demand;
			if (needed <= blocks)
				break;
			blocks = needed;
		}
		if (blocks > QNX4_BLOCKS_MAX)
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_WRITER,
			    "QNX4 content and free space exceed the largest volume"));
	}
	g->blocks = (uint32_t)blocks;
	g->bitmap_blocks = (uint32_t)bitmap_blocks(blocks);
	g->bitmap_bytes = (uint32_t)(blocks / 8 + (blocks % 8 != 0));
	g->volume_bytes = blocks * QNX4_BLOCK_BYTES;
	return (FSIMG_OK);
}

/* Give one entry the next run of blocks and fix its identity: root-block
 * record 2 or 3 for /.boot and /.altboot, its .inodes slot for a link
 * name, else its direct slot in the parent, whose first block the layout
 * already fixed because a parent's ID is below its children's. The slot
 * must lie in the parent's blocks and a directory's slots in its own.
 */
static fsimg_status_t
place_object(qnx4_plan_t *plan, uint64_t id, uint64_t *next,
    fsimg_error_t *error)
{
	const fsimg_catalog_entry_t *catalog;
	qnx4_row_t row, parent;
	fsimg_status_t status;

	status = fsimg_session_charge(&plan->common.writer->session, 0, 0, 1,
	    error);
	if (status == FSIMG_OK)
		status = fsimg_catalog_get(plan->common.writer, id, &catalog,
		    error);
	if (status == FSIMG_OK)
		status = qnx4_row_get(plan, id, &row, error);
	if (status == FSIMG_OK)
		status = qnx4_row_get(plan, catalog->parent_id, &parent, error);
	if (status != FSIMG_OK)
		return (status);
	if ((catalog->entry.kind == FSIMG_ENTRY_DIRECTORY &&
	    (uint64_t)row.next_slot * QNX4_RECORD_BYTES > row.size) ||
	    (uint64_t)*next + row.blocks > (uint64_t)plan->geometry.blocks + 1)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "QNX4 row disagrees with its planned storage"));
	row.first = row.blocks == 0 ? 0 : (uint32_t)*next;
	*next += row.blocks;

	if (id == plan->boot_row || id == plan->altboot_row) {
		row.record_block = QNX4_ROOT_BLOCK;
		row.record_slot = id == plan->boot_row ? QNX4_RECORD_BOOT :
		    QNX4_RECORD_ALTBOOT;
	} else if (row.inode_slot != QNX4_NONE) {
		row.record_block = plan->inodes_first +
		    row.inode_slot / QNX4_SLOTS_PER_BLOCK;
		row.record_slot =
		    (uint8_t)(row.inode_slot % QNX4_SLOTS_PER_BLOCK);
	} else {
		if ((uint64_t)row.slot >= (uint64_t)parent.blocks *
		    QNX4_SLOTS_PER_BLOCK)
			return (fsimg_error_set(error, FSIMG_ESTATE, 0,
			    FSIMG_OP_WRITER, "QNX4 slot lies outside its directory"));
		row.record_block = parent.first +
		    row.slot / QNX4_SLOTS_PER_BLOCK;
		row.record_slot = (uint8_t)(row.slot % QNX4_SLOTS_PER_BLOCK);
	}
	return (qnx4_row_set(plan, id, &row, error));
}

/* Pass 3: first blocks in the fixed order. Nothing leaves a gap, so the
 * last block used is A and blocks 1 to A are the allocated set.
 */
static fsimg_status_t
layout(qnx4_plan_t *plan, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	qnx4_row_t root;
	uint64_t next, expected;
	fsimg_status_t status;

	status = qnx4_row_get(plan, 0, &root, error);
	if (status != FSIMG_OK)
		return (status);
	root.first = qnx4_plan_root_first(plan);
	status = qnx4_row_set(plan, 0, &root, error);
	if (status != FSIMG_OK)
		return (status);
	plan->inodes_first = root.first + plan->root_blocks;
	plan->lost_found_first = plan->inodes_first + plan->inodes_blocks;
	plan->names_first = plan->lost_found_first + QNX4_LOST_FOUND_BLOCKS;
	next = (uint64_t)plan->names_first + plan->names_blocks;
	expected = next + plan->objects;
	for (uint64_t id = 1; id <= writer->count; id++) {
		status = place_object(plan, id, &next, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (next != expected || next - 1 > plan->geometry.blocks)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "QNX4 allocation accounting mismatch"));
	plan->allocated = (uint32_t)(next - 1);
	return (FSIMG_OK);
}

static fsimg_status_t
make_plan(fsimg_writer_t *writer, fsimg_error_t *error)
{
	qnx4_plan_t *plan;
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
	strcpy(info->format, "qnx4");
	info->entries = writer->count;
	info->payload_bytes = writer->payload_bytes;
	info->required_io_flags = FSIMG_IO_WRITE;
	if ((writer->options.flags & FSIMG_WRITE_DURABLE) != 0)
		info->required_io_flags |= FSIMG_IO_DURABLE;

	/* Format-owned records and absent times: the epoch when
	 * reproducible, otherwise the host's UTC clock at planning.
	 */
	if ((writer->options.flags & FSIMG_WRITE_REPRODUCIBLE) != 0) {
		plan->format_time = (uint32_t)writer->options.epoch;
	} else {
		now = time(NULL);
		if (now < 0 || (int64_t)now > QNX4_TIME_MAX) {
			status = fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_WRITER, "host clock is outside the QNX4 time range");
			goto fail;
		}
		plan->format_time = (uint32_t)now;
	}
	status = fsimg_array_new(&writer->session, QNX4_ROW_BYTES,
	    writer->count + 1, &plan->rows, error);
	if (status != FSIMG_OK)
		goto fail;
	status = plan_entries(plan, error);
	if (status == FSIMG_OK)
		status = size_tables(plan, error);
	if (status == FSIMG_OK)
		status = choose_geometry(plan, error);
	if (status == FSIMG_OK)
		status = layout(plan, error);
	if (status != FSIMG_OK)
		goto fail;

	/* Pass 4: the summary. Metadata is every allocated block outside
	 * file and link content: the loader, the root block, the bitmap,
	 * directories and the format-owned tables.
	 */
	info->volume_bytes = plan->geometry.volume_bytes;
	info->sector_bytes = 512;
	info->allocation_unit_bytes = QNX4_BLOCK_BYTES;
	info->allocated_bytes = (uint64_t)plan->allocated * QNX4_BLOCK_BYTES;
	info->free_bytes = (uint64_t)(plan->geometry.blocks - plan->allocated) *
	    QNX4_BLOCK_BYTES;
	info->metadata_bytes = ((uint64_t)plan->allocated -
	    plan->payload_blocks) * QNX4_BLOCK_BYTES;
	info->scratch_bytes = writer->ctx->stats.scratch_bytes;
	writer->plan = &plan->common;
	return (FSIMG_OK);
fail:
	free_plan(&plan->common);
	return (status);
}

static void
free_plan(fsimg_plan_t *base)
{
	qnx4_plan_t *plan = (qnx4_plan_t *)base;

	if (plan == NULL)
		return;
	fsimg_array_free(plan->rows);
	fsimg_dealloc(plan);
}
