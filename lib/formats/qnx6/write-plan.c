/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>
#include <time.h>

#include "unicode.h"
#include "write-qnx6.h"

/* Planning runs a fixed sequence of passes over the frozen catalog
 * (specs/design-qnx6-driver.md, Planning): the entries pass fills one row
 * per entry and the totals, the geometry search chooses N, the layout pass
 * assigns each object its block range, and the summary fills the plan
 * information. Rows are the only per-entry state.
 */

/* What the entries pass accumulates: the inodes used so far, the long
 * names, the content blocks of files and links, and whether .boot has a
 * slot in the root yet (boot_slot, for a synthesized one).
 */
typedef struct qnx6_totals {
	uint64_t	 inodes;
	uint64_t	 long_names;
	uint64_t	 payload_blocks;
	uint32_t	 boot_slot;
	bool		 boot_placed;
} qnx6_totals_t;

/* The system run for one candidate N: the inode capacity, both system tree
 * shapes and R, their block total.
 */
typedef struct qnx6_system {
	qnx6_shape_t	 bitmap;
	qnx6_shape_t	 inodes;
	uint64_t	 capacity;
	uint64_t	 reservation;
} qnx6_system_t;

static fsimg_status_t validate_options(fsimg_write_options_t *, unsigned,
    fsimg_error_t *);
static fsimg_status_t make_plan(fsimg_writer_t *, fsimg_error_t *);
static void free_plan(fsimg_plan_t *);
static int name_order(const char *, size_t, const char *);
static fsimg_status_t encode_time(qnx6_plan_t *, const fsimg_entry_t *,
    uint32_t, const fsimg_timestamp_t *, uint32_t *, fsimg_error_t *);
static fsimg_status_t encode_metadata(qnx6_plan_t *, const fsimg_entry_t *,
    uint16_t, qnx6_row_t *, fsimg_error_t *);
static void profile_directory(qnx6_plan_t *, uint32_t, uint16_t, qnx6_row_t *);
static uint64_t directory_bytes(const qnx6_plan_t *, uint64_t);
static fsimg_status_t add_object(qnx6_plan_t *, uint64_t, qnx6_shape_t *,
    fsimg_error_t *);
static fsimg_status_t add_slot(qnx6_row_t *, bool, uint32_t *,
    fsimg_error_t *);
static fsimg_status_t plan_entry(qnx6_plan_t *, const fsimg_catalog_entry_t *,
    qnx6_totals_t *, fsimg_error_t *);
static fsimg_status_t plan_entries(qnx6_plan_t *, qnx6_totals_t *,
    fsimg_error_t *);
static unsigned size_class(uint64_t);
static uint64_t ladder_capacity(uint64_t);
static uint32_t native_groups(const qnx6_system_t *, uint64_t);
static bool system_for(const qnx6_plan_t *, uint64_t, uint64_t, qnx6_system_t *);
static bool fits(const qnx6_plan_t *, uint64_t, uint64_t, uint64_t,
    qnx6_system_t *);
static fsimg_status_t choose_geometry(qnx6_plan_t *, const qnx6_totals_t *,
    fsimg_error_t *);
static fsimg_status_t place_object(qnx6_plan_t *, uint64_t, uint64_t *,
    fsimg_error_t *);
static fsimg_status_t layout(qnx6_plan_t *, fsimg_error_t *);

const fsimg_writer_ops_t fsimg_qnx6_writer_ops = {
	.validate = validate_options,
	.plan = make_plan,
	.emit = qnx6_write_emit,
	.free = free_plan
};

/* The common options with the profile's rules (Writer settings): one
 * 512-byte sector size, a classic block size (1024 when zero), no FAT root
 * (the common capability check refuses a label), a 32-bit reproducible
 * epoch, an exact size that holds the two
 * header areas and at least 64 blocks, and the typed QNX6 settings. The
 * volume serial is a common identity the profile does not store; the UUID
 * is its identity.
 */
static fsimg_status_t
validate_options(fsimg_write_options_t *options, unsigned variant FSIMG_UNUSED,
    fsimg_error_t *error)
{
	uint32_t block;

	block = options->allocation_unit_bytes == 0 ? 1024 :
	    options->allocation_unit_bytes;
	if ((options->sector_bytes != 0 && options->sector_bytes != 512) ||
	    (block != 512 && block != 1024 && block != 2048 && block != 4096) ||
	    options->fat_root_entries != 0 ||
	    options->volume_bytes > INT64_MAX ||
	    (options->volume_bytes != 0 && (options->volume_bytes % 512 != 0 ||
	    options->volume_bytes < 16384 + (uint64_t)64 * block)) ||
	    ((options->flags & FSIMG_WRITE_REPRODUCIBLE) != 0 &&
	    (options->epoch < 0 || options->epoch > UINT32_MAX)) ||
	    (options->byte_order != FSIMG_BYTE_ORDER_LITTLE &&
	    options->byte_order != FSIMG_BYTE_ORDER_BIG) ||
	    options->reserve_percent > 100)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER,
		    "invalid QNX6 geometry, settings or reproducible epoch"));
	options->sector_bytes = 512;
	options->allocation_unit_bytes = block;
	return (FSIMG_OK);
}

fsimg_status_t
qnx6_row_get(qnx6_plan_t *plan, uint64_t id, qnx6_row_t *row,
    fsimg_error_t *error)
{
	unsigned char raw[QNX6_ROW_BYTES];
	fsimg_status_t status;

	status = fsimg_array_get(plan->rows, id, raw, error);
	if (status != FSIMG_OK)
		return (status);
	row->length = fsimg_meta_get_u64(raw);
	row->first = fsimg_meta_get_u32(raw + 8);
	row->inode = fsimg_meta_get_u32(raw + 12);
	row->slot = fsimg_meta_get_u32(raw + 16);
	row->next_slot = fsimg_meta_get_u32(raw + 20);
	row->long_name = fsimg_meta_get_u32(raw + 24);
	row->uid = fsimg_meta_get_u32(raw + 28);
	row->gid = fsimg_meta_get_u32(raw + 32);
	row->created = fsimg_meta_get_u32(raw + 36);
	row->modified = fsimg_meta_get_u32(raw + 40);
	row->accessed = fsimg_meta_get_u32(raw + 44);
	row->changed = fsimg_meta_get_u32(raw + 48);
	row->mode = (uint16_t)fsimg_meta_get_u32(raw + 52);
	row->links = (uint16_t)fsimg_meta_get_u32(raw + 56);
	row->status = raw[60];
	return (FSIMG_OK);
}

fsimg_status_t
qnx6_row_set(qnx6_plan_t *plan, uint64_t id, const qnx6_row_t *row,
    fsimg_error_t *error)
{
	unsigned char raw[QNX6_ROW_BYTES];

	bzero(raw, sizeof(raw));
	fsimg_meta_put_u64(raw, row->length);
	fsimg_meta_put_u32(raw + 8, row->first);
	fsimg_meta_put_u32(raw + 12, row->inode);
	fsimg_meta_put_u32(raw + 16, row->slot);
	fsimg_meta_put_u32(raw + 20, row->next_slot);
	fsimg_meta_put_u32(raw + 24, row->long_name);
	fsimg_meta_put_u32(raw + 28, row->uid);
	fsimg_meta_put_u32(raw + 32, row->gid);
	fsimg_meta_put_u32(raw + 36, row->created);
	fsimg_meta_put_u32(raw + 40, row->modified);
	fsimg_meta_put_u32(raw + 44, row->accessed);
	fsimg_meta_put_u32(raw + 48, row->changed);
	fsimg_meta_put_u32(raw + 52, row->mode);
	fsimg_meta_put_u32(raw + 56, row->links);
	raw[60] = row->status;
	return (fsimg_array_set(plan->rows, id, raw, error));
}

/* The shape of an object of length bytes; false past depth 5. */
bool
qnx6_object_shape(const qnx6_plan_t *plan, uint64_t length,
    qnx6_shape_t *shape)
{
	return (qnx6_shape(qnx6_tree_leaves(length, plan->geometry.block_size),
	    plan->geometry.pointers, shape));
}

/* Compare a name with a NUL-terminated one by unsigned bytes, a shorter
 * equal prefix first: the catalog's sibling order.
 */
static int
name_order(const char *name, size_t length, const char *other)
{
	size_t other_length = strlen(other);
	int order;

	order = memcmp(name, other, length < other_length ? length : other_length);
	if (order != 0)
		return (order);
	return (length < other_length ? -1 : length > other_length);
}

/* Encode one timestamp field as unsigned UTC seconds. An absent field, or
 * one outside the 32-bit range, takes the format time; reproducible mode
 * replaces every time with the epoch, which validation keeps in range.
 * Other times keep their whole seconds. A present field is exact when it is
 * a UTC instant with whole seconds in range (and, when reproducible, equal
 * to the epoch); otherwise it is an optional loss or a failure.
 */
static fsimg_status_t
encode_time(qnx6_plan_t *plan, const fsimg_entry_t *entry, uint32_t field,
    const fsimg_timestamp_t *time, uint32_t *out, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	bool exact, representable, reproducible;

	reproducible = (writer->options.flags & FSIMG_WRITE_REPRODUCIBLE) != 0;
	representable = time->seconds >= 0 && time->seconds <= UINT32_MAX;
	*out = plan->format_time;
	if ((entry->metadata.valid & field) == 0)
		return (FSIMG_OK);
	if (representable && !reproducible)
		*out = (uint32_t)time->seconds;
	exact = representable && time->basis == FSIMG_TIME_UTC &&
	    time->nanoseconds == 0 && time->seconds == (int64_t)*out;
	if (exact)
		return (FSIMG_OK);
	if ((entry->metadata.required & field) != 0 ||
	    (writer->options.flags & FSIMG_WRITE_ALLOW_LOSS) == 0) {
		fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0, FSIMG_OP_WRITER,
		    "QNX6 cannot preserve the requested timestamp");
		fsimg_error_path(error, entry->path);
		return (FSIMG_EUNSUPPORTED);
	}
	plan->common.info.metadata_losses++;
	fsimg_report_loss(writer->ctx, entry->path, field);
	return (FSIMG_OK);
}

/* Encode an entry's owner, mode and times into its row. profile_mode is the
 * permission set the profile fixes, for .boot, or zero for the kind's
 * default; a requested mode that differs from a fixed one is a loss.
 */
static fsimg_status_t
encode_metadata(qnx6_plan_t *plan, const fsimg_entry_t *entry,
    uint16_t profile_mode, qnx6_row_t *row, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	const fsimg_metadata_t *m = &entry->metadata;
	uint16_t type, permissions;
	fsimg_status_t status;

	switch (entry->kind) {
	case FSIMG_ENTRY_FILE:
		type = QNX6_MODE_FILE;
		permissions = 0644;
		break;
	case FSIMG_ENTRY_DIRECTORY:
		type = QNX6_MODE_DIRECTORY;
		permissions = 0755;
		break;
	case FSIMG_ENTRY_SYMLINK:
		type = QNX6_MODE_SYMLINK;
		permissions = 0777;
		break;
	case FSIMG_ENTRY_FIFO:
		type = QNX6_MODE_FIFO;
		permissions = 0777;
		break;
	default:
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "QNX6 planning received an unknown entry kind"));
	}
	if ((m->valid & FSIMG_META_MODE) != 0)
		permissions = (uint16_t)(m->mode & FSIMG_MODE_BITS);
	if (profile_mode != 0 && (m->valid & FSIMG_META_MODE) != 0 &&
	    permissions != profile_mode) {
		if ((m->required & FSIMG_META_MODE) != 0 ||
		    (writer->options.flags & FSIMG_WRITE_ALLOW_LOSS) == 0) {
			fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0, FSIMG_OP_WRITER,
			    "QNX6 fixes the permissions of this directory");
			fsimg_error_path(error, entry->path);
			return (FSIMG_EUNSUPPORTED);
		}
		plan->common.info.metadata_losses++;
		fsimg_report_loss(writer->ctx, entry->path, FSIMG_META_MODE);
	}
	if (profile_mode != 0)
		permissions = profile_mode;
	row->mode = (uint16_t)(type | permissions);
	row->uid = (m->valid & FSIMG_META_OWNER) != 0 ? m->uid : 0;
	row->gid = (m->valid & FSIMG_META_OWNER) != 0 ? m->gid : 0;
	status = encode_time(plan, entry, FSIMG_META_CREATED, &m->created,
	    &row->created, error);
	if (status == FSIMG_OK)
		status = encode_time(plan, entry, FSIMG_META_MODIFIED,
		    &m->modified, &row->modified, error);
	if (status == FSIMG_OK)
		status = encode_time(plan, entry, FSIMG_META_ACCESSED,
		    &m->accessed, &row->accessed, error);
	if (status == FSIMG_OK)
		status = encode_time(plan, entry, FSIMG_META_CHANGED,
		    &m->changed, &row->changed, error);
	return (status);
}

/* A formatter-created directory: the root or a synthesized .boot, with
 * status 1, owner zero, the format time and no slots beyond its dots.
 */
static void
profile_directory(qnx6_plan_t *plan, uint32_t inode, uint16_t mode,
    qnx6_row_t *row)
{
	*row = (qnx6_row_t){
		.inode = inode,
		.next_slot = 2,
		.long_name = QNX6_SENTINEL,
		.created = plan->format_time,
		.modified = plan->format_time,
		.accessed = plan->format_time,
		.changed = plan->format_time,
		.mode = mode,
		.links = 2,
		.status = QNX6_STATUS_ALLOCATED
	};
}

/* The whole-block length of a directory of the given slot count. */
static uint64_t
directory_bytes(const qnx6_plan_t *plan, uint64_t slots)
{
	uint64_t bytes = slots * QNX6_SLOT_BYTES;
	uint32_t block = plan->geometry.block_size;

	return ((bytes / block + (bytes % block != 0)) * block);
}

/* Add an object of length bytes to the block total and return its shape. */
static fsimg_status_t
add_object(qnx6_plan_t *plan, uint64_t length, qnx6_shape_t *shape,
    fsimg_error_t *error)
{
	if (!qnx6_object_shape(plan, length, shape) ||
	    !fsimg_add_u64(plan->objects, shape->leaves + shape->indirect,
	    &plan->objects))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER,
		    "QNX6 object exceeds the block reference space"));
	return (FSIMG_OK);
}

/* Assign the next slot of a directory to a child, which counts as one more
 * link when it is a directory itself.
 */
static fsimg_status_t
add_slot(qnx6_row_t *directory, bool subdirectory, uint32_t *slot,
    fsimg_error_t *error)
{
	if (directory->next_slot == UINT32_MAX ||
	    (subdirectory && directory->links == QNX6_LINKS_MAX))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER,
		    "QNX6 directory has too many entries or subdirectories"));
	*slot = directory->next_slot++;
	if (subdirectory)
		directory->links++;
	return (FSIMG_OK);
}

/* Plan one catalog entry: its name, inode, slot, long-name index, metadata
 * and storage. A root child that sorts after .boot first gives a
 * synthesized .boot its slot, so root slots stay in byte order: catalog IDs
 * follow each directory's byte order (fsimg_catalog_count_children()), so a
 * catalog /.boot always comes before any root name that sorts after it, and
 * every directory's slots are assigned in byte order the same way.
 */
static fsimg_status_t
plan_entry(qnx6_plan_t *plan, const fsimg_catalog_entry_t *catalog,
    qnx6_totals_t *totals, fsimg_error_t *error)
{
	const fsimg_entry_t *entry = &catalog->entry;
	qnx6_row_t row, parent;
	qnx6_shape_t shape;
	const char *name;
	size_t length;
	fsimg_status_t status;
	bool boot, directory;

	status = fsimg_session_charge(&plan->common.writer->session, 0, 0, 1,
	    error);
	if (status != FSIMG_OK)
		return (status);
	name = strrchr(entry->path, '/') + 1;
	length = strlen(name);
	if (length > QNX6_NAME_MAX || !fsimg_name_in_qnx_profile(name, length)) {
		fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER,
		    "name is outside the QNX6 name profile");
		fsimg_error_path(error, entry->path);
		return (FSIMG_EINVAL);
	}
	directory = entry->kind == FSIMG_ENTRY_DIRECTORY;
	boot = catalog->parent_id == 0 && strcmp(name, ".boot") == 0;
	if (boot && !directory) {
		fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0, FSIMG_OP_WRITER,
		    "QNX6 reserves /.boot for a directory");
		fsimg_error_path(error, entry->path);
		return (FSIMG_EUNSUPPORTED);
	}
	if (!boot && totals->inodes == UINT32_MAX)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER,
		    "QNX6 entries exceed the inode number space"));
	status = qnx6_row_get(plan, catalog->parent_id, &parent, error);
	if (status != FSIMG_OK)
		return (status);
	if (catalog->parent_id == 0 && !totals->boot_placed &&
	    name_order(name, length, ".boot") > 0) {
		status = add_slot(&parent, true, &totals->boot_slot, error);
		if (status != FSIMG_OK)
			return (status);
		totals->boot_placed = true;
		plan->boot_row = plan->common.writer->count + 1;
	}
	if (boot) {
		totals->boot_placed = true;
		plan->boot_row = catalog->id;
	}
	bzero(&row, sizeof(row));
	status = add_slot(&parent, directory, &row.slot, error);
	if (status == FSIMG_OK)
		status = qnx6_row_set(plan, catalog->parent_id, &parent, error);
	if (status == FSIMG_OK)
		status = encode_metadata(plan, entry,
		    boot ? QNX6_BOOT_MODE & FSIMG_MODE_BITS : 0, &row, error);
	if (status != FSIMG_OK)
		return (status);

	/* Inodes 1 and 2 are the root and .boot; the rest follow the catalog.
	 * The formatter creates .boot (status 1); fsimg creates the rest as
	 * the filesystem would (status 3).
	 */
	if (boot) {
		row.inode = QNX6_BOOT_INODE;
		row.status = QNX6_STATUS_ALLOCATED;
	} else {
		row.inode = (uint32_t)++totals->inodes;
		row.status = QNX6_STATUS_ALLOCATED | QNX6_STATUS_CREATED;
	}
	row.links = directory ? 2 : 1;
	row.next_slot = directory ? 2 : 0;
	row.long_name = QNX6_SENTINEL;
	if (length > QNX6_INLINE_NAME_MAX)
		row.long_name = (uint32_t)totals->long_names++;
	if (directory) {
		row.length = directory_bytes(plan, catalog->children + 2);
	} else if (entry->kind != FSIMG_ENTRY_FIFO) {
		row.length = entry->size;
		plan->common.info.initialized_bytes += entry->size;
	}
	status = add_object(plan, row.length, &shape, error);
	if (status != FSIMG_OK) {
		fsimg_error_path(error, entry->path);
		return (status);
	}
	if (!directory)
		totals->payload_blocks += shape.leaves;
	return (qnx6_row_set(plan, catalog->id, &row, error));
}

/* Pass 1: one row per entry in catalog ID order. The root's length and a
 * synthesized .boot are completed after the pass, since only its end shows
 * whether the catalog has /.boot.
 */
static fsimg_status_t
plan_entries(qnx6_plan_t *plan, qnx6_totals_t *totals, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	const fsimg_catalog_entry_t *catalog;
	qnx6_row_t root, boot;
	qnx6_shape_t shape;
	fsimg_status_t status;

	profile_directory(plan, QNX6_ROOT_INODE, QNX6_ROOT_MODE, &root);
	status = qnx6_row_set(plan, 0, &root, error);
	if (status != FSIMG_OK)
		return (status);
	totals->inodes = QNX6_BOOT_INODE;
	for (uint64_t id = 1; id <= writer->count; id++) {
		status = fsimg_catalog_get(writer, id, &catalog, error);
		if (status != FSIMG_OK)
			return (status);
		status = plan_entry(plan, catalog, totals, error);
		if (status != FSIMG_OK)
			return (status);
	}
	status = qnx6_row_get(plan, 0, &root, error);
	if (status != FSIMG_OK)
		return (status);
	if (!totals->boot_placed) {
		status = add_slot(&root, true, &totals->boot_slot, error);
		if (status != FSIMG_OK)
			return (status);
		plan->boot_row = writer->count + 1;
	}
	if (qnx6_boot_synthesized(plan)) {
		profile_directory(plan, QNX6_BOOT_INODE, QNX6_BOOT_MODE, &boot);
		boot.slot = totals->boot_slot;
		boot.length = directory_bytes(plan, 2);
		status = add_object(plan, boot.length, &shape, error);
		if (status == FSIMG_OK)
			status = qnx6_row_set(plan, plan->boot_row, &boot, error);
		if (status != FSIMG_OK)
			return (status);
	}
	root.length = directory_bytes(plan, root.next_slot);
	status = add_object(plan, root.length, &shape, error);
	if (status != FSIMG_OK)
		return (status);
	return (qnx6_row_set(plan, 0, &root, error));
}

/* The native formatter's size class of N blocks, 0 through 3 for N up to
 * 0xfffff, up to 0x7fffff, below 0x8000000 and above. Its default inode
 * count and its group cap both step at these bounds.
 */
static unsigned
size_class(uint64_t blocks)
{
	if (blocks <= 0xfffff)
		return (0);
	if (blocks <= 0x7fffff)
		return (1);
	if (blocks < 0x8000000)
		return (2);
	return (3);
}

/* The native formatter's default inode count for N blocks: one per 8, 32,
 * 64 or 128 blocks by the size class of N.
 */
static uint64_t
ladder_capacity(uint64_t blocks)
{
	static const unsigned blocks_per_inode[] = { 8, 32, 64, 128 };

	return (blocks / blocks_per_inode[size_class(blocks)]);
}

/* The allocation-group count the native formatter writes for N blocks and
 * the given system trees: a ladder over m, the smaller of the bitmap and
 * inode-table leaf counts (1, 4, 8, 16 or 32 for m up to 3, 7, 15, 31 and
 * above), capped at 4, 8, 16 or 32 by the size class of N. No on-disk
 * structure depends on it; the running filesystem uses it to spread new
 * directories and indirect blocks, so storing the native value gives the
 * native allocator the group count it would use on a natively formatted
 * volume of this geometry.
 */
static uint32_t
native_groups(const qnx6_system_t *system, uint64_t blocks)
{
	static const uint32_t caps[] = { 4, 8, 16, 32 };
	uint64_t m;
	uint32_t ladder, cap;

	m = system->bitmap.leaves < system->inodes.leaves ?
	    system->bitmap.leaves : system->inodes.leaves;
	if (m <= 3)
		ladder = 1;
	else if (m <= 7)
		ladder = 4;
	else if (m <= 15)
		ladder = 8;
	else if (m <= 31)
		ladder = 16;
	else
		ladder = 32;
	cap = caps[size_class(blocks)];
	return (ladder < cap ? ladder : cap);
}

/* The system run for N blocks: the inode capacity (the requested one, or
 * the ladder raised to the inodes the catalog needs, rounded up to a whole
 * inode-table block), the bitmap and inode-table shapes, and R.
 */
static bool
system_for(const qnx6_plan_t *plan, uint64_t blocks, uint64_t needed,
    qnx6_system_t *system)
{
	const fsimg_writer_t *writer = plan->common.writer;
	uint32_t block = plan->geometry.block_size;
	uint64_t per_block, capacity, bitmap_bytes;

	per_block = block / QNX6_INODE_BYTES;
	capacity = writer->options.inode_capacity;
	if (capacity == 0) {
		capacity = ladder_capacity(blocks);
		if (capacity < needed)
			capacity = needed;
	}
	capacity = (capacity / per_block + (capacity % per_block != 0)) * per_block;
	if (capacity > UINT32_MAX)
		return (false);
	bitmap_bytes = blocks / 8 + (blocks % 8 != 0);
	if (!qnx6_shape(qnx6_tree_leaves(bitmap_bytes, block), plan->geometry.pointers,
	    &system->bitmap) ||
	    !qnx6_shape(capacity / per_block, plan->geometry.pointers,
	    &system->inodes))
		return (false);
	system->capacity = capacity;
	system->reservation = system->bitmap.leaves + system->bitmap.indirect +
	    system->inodes.leaves + system->inodes.indirect;
	return (true);
}

/* Whether N blocks hold the system run and its twin, base blocks of
 * objects and long names, the reserve percentage and the requested free
 * space. Raising N by 64 adds 64 blocks against at most fourteen system
 * blocks (twice one bitmap leaf, two inode-table leaves and one indirect
 * block per level of each depth-2 tree) and ceil(64 * reserve / 100)
 * reserve blocks, so the predicate is monotone over that step for reserves
 * up to 76 percent. Above that the binary search still returns an N that
 * fits, which may not be the smallest, and when that N is past the depth-2
 * limit of the system trees the request fails although a smaller N would
 * have fitted.
 */
static bool
fits(const qnx6_plan_t *plan, uint64_t blocks, uint64_t base, uint64_t needed,
    qnx6_system_t *system)
{
	const fsimg_write_options_t *options = &plan->common.writer->options;
	uint64_t total, reserve, free_blocks;

	if (!system_for(plan, blocks, needed, system))
		return (false);
	reserve = blocks * options->reserve_percent / 100 +
	    (blocks * options->reserve_percent % 100 != 0);
	free_blocks = options->min_free_bytes / plan->geometry.block_size +
	    (options->min_free_bytes % plan->geometry.block_size != 0);
	total = system->reservation * 2;
	return (fsimg_add_u64(total, base, &total) &&
	    fsimg_add_u64(total, reserve, &total) &&
	    fsimg_add_u64(total, free_blocks, &total) && total <= blocks);
}

/* Pass 2: N from the exact slice, or the smallest multiple of 64 blocks
 * that fits; then the system trees must have the native depth of at most 2.
 */
static fsimg_status_t
choose_geometry(qnx6_plan_t *plan, const qnx6_totals_t *totals,
    fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	qnx6_geometry_t *g = &plan->geometry;
	qnx6_system_t system;
	uint64_t base, needed, blocks, low, high, middle, end;

	if (!qnx6_shape(totals->long_names, g->pointers, &plan->names) ||
	    !fsimg_add_u64(plan->objects, plan->names.leaves + plan->names.indirect,
	    &base))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER,
		    "QNX6 content exceeds the block reference space"));
	needed = totals->inodes;
	if (writer->options.inode_capacity != 0 &&
	    writer->options.inode_capacity < needed)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER,
		    "QNX6 inode capacity is smaller than the number of entries"));
	if (writer->options.volume_bytes != 0) {
		/* E = floor(V / 4096) * 4096 and N = (E - 16384) / B. */
		end = writer->options.volume_bytes / QNX6_HEADER_AREA *
		    QNX6_HEADER_AREA;
		blocks = (end - QNX6_DATA_ORIGIN - QNX6_HEADER_AREA) /
		    g->block_size;
		if (blocks > UINT32_MAX)
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_WRITER,
			    "QNX6 volume size exceeds the 32-bit block count"));
		if (!fits(plan, blocks, base, needed, &system))
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_WRITER,
			    "QNX6 content and reserve do not fit the volume size"));
		g->volume_bytes = writer->options.volume_bytes;
	} else {
		/* Search k for N = 64k over the 32-bit reference space. */
		low = 1;
		high = UINT32_MAX / 64;
		if (!fits(plan, high * 64, base, needed, &system))
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_WRITER,
			    "QNX6 content and reserve exceed the largest volume"));
		while (low < high) {
			middle = low + (high - low) / 2;
			if (fits(plan, middle * 64, base, needed, &system))
				high = middle;
			else
				low = middle + 1;
		}
		blocks = low * 64;
		if (!fits(plan, blocks, base, needed, &system))
			return (fsimg_error_set(error, FSIMG_ESTATE, 0,
			    FSIMG_OP_WRITER, "QNX6 size search lost its fit"));
		end = QNX6_DATA_ORIGIN + blocks * g->block_size + QNX6_HEADER_AREA;
		g->volume_bytes = end;
	}
	if (system.bitmap.depth > QNX6_SYSTEM_DEPTH_MAX ||
	    system.inodes.depth > QNX6_SYSTEM_DEPTH_MAX)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER,
		    "QNX6 volume is too large for the classic system trees"));
	g->second_header = end - QNX6_HEADER_AREA;
	g->blocks = (uint32_t)blocks;
	g->inode_count = (uint32_t)system.capacity;
	g->reservation = (uint32_t)system.reservation;
	g->groups = native_groups(&system, blocks);
	plan->bitmap = system.bitmap;
	plan->inodes = system.inodes;
	plan->used_inodes = (uint32_t)needed;
	return (FSIMG_OK);
}

/* Give one row the next block range: its indirect blocks, then its leaves.
 * A directory's slots must fit its blocks.
 */
static fsimg_status_t
place_object(qnx6_plan_t *plan, uint64_t id, uint64_t *next,
    fsimg_error_t *error)
{
	qnx6_shape_t shape;
	qnx6_row_t row;
	fsimg_status_t status;

	status = fsimg_session_charge(&plan->common.writer->session, 0, 0, 1,
	    error);
	if (status == FSIMG_OK)
		status = qnx6_row_get(plan, id, &row, error);
	if (status != FSIMG_OK)
		return (status);
	if (!qnx6_object_shape(plan, row.length, &shape) ||
	    ((row.mode & QNX6_MODE_TYPE) == QNX6_MODE_DIRECTORY &&
	    (uint64_t)row.next_slot * QNX6_SLOT_BYTES > row.length))
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "QNX6 row disagrees with its planned storage"));
	row.first = (uint32_t)*next;
	*next += shape.indirect + shape.leaves;
	if (*next > plan->geometry.blocks)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "QNX6 layout exceeds the planned blocks"));
	return (qnx6_row_set(plan, id, &row, error));
}

/* Pass 3: block ranges in a fixed order. The system run and its twin
 * occupy [0, 2R); the root, .boot, every other entry in catalog ID order
 * and the long-name stream follow without gaps, so [0, A) is exactly the
 * allocated set.
 */
static fsimg_status_t
layout(qnx6_plan_t *plan, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	uint64_t next, expected;
	fsimg_status_t status;

	next = (uint64_t)plan->geometry.reservation * 2;
	expected = next + plan->objects + plan->names.indirect + plan->names.leaves;
	status = place_object(plan, 0, &next, error);
	if (status == FSIMG_OK)
		status = place_object(plan, plan->boot_row, &next, error);
	for (uint64_t id = 1; status == FSIMG_OK && id <= writer->count; id++) {
		if (id != plan->boot_row)
			status = place_object(plan, id, &next, error);
	}
	if (status != FSIMG_OK)
		return (status);
	plan->names_first = (uint32_t)next;
	next += plan->names.indirect + plan->names.leaves;
	if (next != expected || next > plan->geometry.blocks)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "QNX6 allocation accounting mismatch"));
	plan->allocated = (uint32_t)next;
	return (FSIMG_OK);
}

static fsimg_status_t
make_plan(fsimg_writer_t *writer, fsimg_error_t *error)
{
	qnx6_plan_t *plan;
	qnx6_totals_t totals;
	fsimg_plan_info_t *info;
	const qnx6_geometry_t *g;
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
	g = &plan->geometry;
	plan->common.writer = writer;
	strcpy(info->format, "qnx6");
	info->entries = writer->count;
	info->payload_bytes = writer->payload_bytes;
	info->required_io_flags = FSIMG_IO_WRITE;
	if ((writer->options.flags & FSIMG_WRITE_DURABLE) != 0)
		info->required_io_flags |= FSIMG_IO_DURABLE;
	plan->geometry.block_size = writer->options.allocation_unit_bytes;
	plan->geometry.pointers = plan->geometry.block_size / 4;
	plan->geometry.big_endian = writer->options.byte_order == FSIMG_BYTE_ORDER_BIG;

	/* Header and absent inode times: the epoch when reproducible,
	 * otherwise the host's UTC clock at planning.
	 */
	if ((writer->options.flags & FSIMG_WRITE_REPRODUCIBLE) != 0) {
		plan->format_time = (uint32_t)writer->options.epoch;
	} else {
		now = time(NULL);
		if (now < 0 || (uint64_t)now > UINT32_MAX) {
			status = fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_WRITER, "host clock is outside the QNX6 time range");
			goto fail;
		}
		plan->format_time = (uint32_t)now;
	}
	status = fsimg_array_new(&writer->session, QNX6_ROW_BYTES,
	    writer->count + 2, &plan->rows, error);
	if (status != FSIMG_OK)
		goto fail;
	bzero(&totals, sizeof(totals));
	status = plan_entries(plan, &totals, error);
	if (status == FSIMG_OK)
		status = choose_geometry(plan, &totals, error);
	if (status == FSIMG_OK)
		status = layout(plan, error);
	if (status != FSIMG_OK)
		goto fail;

	/* Pass 4: the summary. Metadata is everything outside file and link
	 * content: the boot and header areas, the system run, directories,
	 * long names and indirect blocks.
	 */
	info->volume_bytes = g->volume_bytes;
	info->sector_bytes = 512;
	info->allocation_unit_bytes = g->block_size;
	info->allocated_bytes = (uint64_t)plan->allocated * g->block_size;
	info->free_bytes = (uint64_t)(g->blocks - plan->allocated) * g->block_size;
	info->metadata_bytes = QNX6_DATA_ORIGIN + QNX6_HEADER_AREA +
	    ((uint64_t)plan->allocated - totals.payload_blocks) * g->block_size;
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
	qnx6_plan_t *plan = (qnx6_plan_t *)base;

	if (plan == NULL)
		return;
	fsimg_array_free(plan->rows);
	fsimg_dealloc(plan);
}
