/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>
#include <time.h>

#include "fsimg-endian.h"
#include "layer.h"
#include "sha256.h"
#include "unicode.h"
#include "write-apfs.h"

/*
 * Planning runs a fixed sequence of passes over the frozen catalog
 * (specs/design-apfs-driver.md, "Passes"):
 *
 *   entries     names checked and hashed, rows filled under the metadata
 *               policy, children, per-kind totals and data blocks counted,
 *               and one collision record per entry;
 *   collisions  siblings whose comparison forms are equal refused;
 *   records     every filesystem record encoded and sorted by the tree's
 *               key order;
 *   packing     each tree's node counts, from the records alone;
 *   size        the block count N: exact, or the first fit of the search;
 *   layout      every block, OID, ephemeral slot and pool block;
 *   summary     the plan information.
 *
 * Every value derives from the catalog, the options and the pinned Unicode
 * tables, and the format time is the epoch in reproducible mode, so the
 * result does not depend on source order, budgets, sort fan-in, scratch
 * placement, locale, time zone or the clock.
 */

/* A row's fields in APFS_ROW_BYTES of scratch, little endian. */
#define ROW_PARENT		0u
#define ROW_LENGTH		8u
#define ROW_BLOCKS_BEFORE	16u
#define ROW_FIRST_BLOCK		24u
#define ROW_CREATED		32u
#define ROW_MODIFIED		40u
#define ROW_CHANGED		48u
#define ROW_ACCESSED		56u
#define ROW_CHILDREN		64u
#define ROW_HASH		68u
#define ROW_UID			72u
#define ROW_GID			76u
#define ROW_MODE		80u
#define ROW_KIND		82u
_Static_assert(ROW_KIND < APFS_ROW_BYTES, "a row's fields fit its bytes");

/* The BSD file types the mode field stores. */
#define MODE_FIFO		0010000u
#define MODE_DIRECTORY		0040000u
#define MODE_FILE		0100000u
#define MODE_SYMLINK		0120000u

/* Inode and record values of the writer values table. */
#define INODE_INTERNAL_FLAGS	UINT64_C(0x8000)
#define PROTECTION_FIFO		4u
#define NAME_FIELD_FLAGS	APFS_XF_DO_NOT_COPY
#define DSTREAM_FIELD_FLAGS	APFS_XF_SYSTEM_FIELD
#define NS_PER_SECOND		INT64_C(1000000000)
#define DEFAULT_LABEL		"Untitled"
#define ROOT_NAME		"root"
#define PRIVATE_NAME		"private-dir"
#define SYMLINK_ATTRIBUTE	"com.apple.fs.symlink"
#define CONTAINER_UUID_PREFIX	"fsimg-apfs-container"
#define PROBE_CAP		(UINT64_C(1) << 16)

/*
 * The longest inode value: the fixed part, a blob of two fields, the name
 * field holding the longest name and its NUL, padded, and the data stream.
 */
#define INODE_VAL_MAX		(APFS_INODE_VAL_BYTES + APFS_XF_BLOB_BYTES + \
	    2u * APFS_XF_HEADER_BYTES + \
	    ((APFS_WRITE_NAME_MAX_BYTES + 1u + APFS_XF_ALIGN - 1u) & \
	    ~(APFS_XF_ALIGN - 1u)) + APFS_DSTREAM_BYTES)
_Static_assert(INODE_VAL_MAX <= APFS_WRITE_VAL_MAX,
    "an inode value fits the record frame");
_Static_assert(APFS_XATTR_KEY_BYTES + sizeof(SYMLINK_ATTRIBUTE) <=
    APFS_WRITE_KEY_MAX, "the link attribute's key fits the record frame");
_Static_assert(APFS_WRITE_RECORD_MAX <= UINT16_MAX,
    "a record's lengths fit its frame");

/* A collision record: parent (8), catalog ID (8), the namespace key. */
#define COLLISION_FIXED		16u
#define COLLISION_MAX		(COLLISION_FIXED + APFS_NAME_KEY_BYTES)

/*
 * The working state of one planning run, freed when it ends. The two forms
 * and the key are large, so they live here rather than on the stack:
 * nfd is a name's canonical decomposition, form its comparison form under
 * the volume's rule, and key that form's namespace key.
 */
typedef struct apfs_planner {
	apfs_plan_t		*plan;
	fsimg_writer_t		*writer;
	fsimg_stream_t		*collisions;
	fsimg_stream_t		*unsorted;
	apfs_name_form_t	 nfd;
	apfs_name_form_t	 form;
	char			 key[APFS_NAME_KEY_BYTES];
	unsigned char		 frame[COLLISION_MAX > APFS_WRITE_RECORD_MAX ?
				    COLLISION_MAX : APFS_WRITE_RECORD_MAX];
	unsigned char		 previous[COLLISION_MAX];
	unsigned char		 target[FSIMG_SYMLINK_TARGET_MAX];
} apfs_planner_t;

/*
 * The writer's operations: this module validates and plans, write-emit.c
 * emits.
 */
const fsimg_writer_ops_t fsimg_apfs_writer_ops = {
	.validate = apfs_write_validate,
	.plan = apfs_write_plan,
	.emit = apfs_write_emit,
	.free = apfs_write_free
};

static fsimg_status_t
writer_error(fsimg_status_t status, const char *message, fsimg_error_t *error)
{
	return (fsimg_error_set(error, status, 0, FSIMG_OP_WRITER, message));
}

/* Fail naming the entry at path. */
static fsimg_status_t
entry_error(fsimg_status_t status, const char *message, const char *path,
    fsimg_error_t *error)
{
	fsimg_error_set(error, status, 0, FSIMG_OP_WRITER, message);
	fsimg_error_path(error, path);
	return (status);
}

/*
 * Nanoseconds since 1970 of a time, or false when they leave the writer's
 * time range, the signed 64 bits the format stores. A negative second
 * count is taken one second up first, so no product leaves the range on
 * the way to a result inside it.
 */
static bool
time_ns(int64_t seconds, uint32_t nanoseconds, int64_t *ns)
{
	int64_t base, below;

	if (nanoseconds >= (uint64_t)NS_PER_SECOND ||
	    seconds > INT64_MAX / NS_PER_SECOND ||
	    seconds < INT64_MIN / NS_PER_SECOND - 1)
		return (false);
	if (seconds >= 0) {
		base = seconds * NS_PER_SECOND;
		if ((int64_t)nanoseconds > INT64_MAX - base)
			return (false);
		*ns = base + (int64_t)nanoseconds;
		return (true);
	}
	base = (seconds + 1) * NS_PER_SECOND;
	below = NS_PER_SECOND - (int64_t)nanoseconds;
	if (base < INT64_MIN + below)
		return (false);
	*ns = base - below;
	return (true);
}

/* Whether a NUL-terminated text is strict UTF-8. */
static bool
utf8_valid(const char *text, size_t length)
{
	const unsigned char *bytes = (const unsigned char *)text;
	size_t at, step;
	uint32_t point;

	for (at = 0; at < length; at += step) {
		if (!fsimg_utf8_decode(bytes + at, length - at, &step, &point))
			return (false);
	}
	return (true);
}

static bool
uuid_is_zero(const unsigned char uuid[16])
{
	for (size_t i = 0; i < 16; i++) {
		if (uuid[i] != 0)
			return (false);
	}
	return (true);
}

/*
 * The writer settings (specs/design-apfs-driver.md, "Writer settings").
 * The common settings check has already refused every setting the
 * capability record does not name, a placement list among them.
 */
fsimg_status_t
apfs_write_validate(fsimg_write_options_t *options, unsigned variant,
    fsimg_error_t *error)
{
	int64_t ns;
	bool reproducible;

	(void)variant;
	reproducible = (options->flags & FSIMG_WRITE_REPRODUCIBLE) != 0;
	if ((options->sector_bytes != 0 &&
	    options->sector_bytes != APFS_WRITE_BLOCK_BYTES) ||
	    (options->allocation_unit_bytes != 0 &&
	    options->allocation_unit_bytes != APFS_WRITE_BLOCK_BYTES))
		return (writer_error(FSIMG_EINVAL,
		    "APFS sectors and blocks are 4096 bytes", error));
	if (options->volume_bytes >
	    APFS_WRITER_BLOCKS_MAX * APFS_WRITE_BLOCK_BYTES)
		return (writer_error(FSIMG_ERANGE, "APFS container exceeds "
		    "the writer's largest, 34359738368 blocks", error));
	if (options->volume_bytes != 0 &&
	    (options->volume_bytes % APFS_WRITE_BLOCK_BYTES != 0 ||
	    options->volume_bytes / APFS_WRITE_BLOCK_BYTES <
	    APFS_WRITE_BLOCKS_MIN))
		return (writer_error(FSIMG_EINVAL, "APFS container size is not "
		    "a multiple of 4096 bytes of at least 1024 blocks", error));
	if (options->label != NULL &&
	    (strlen(options->label) > APFS_FS_NAME_BYTES - 1 ||
	    !utf8_valid(options->label, strlen(options->label))))
		return (writer_error(FSIMG_EINVAL,
		    "APFS label is not UTF-8 of at most 255 bytes", error));
	if (!reproducible && uuid_is_zero(options->volume_uuid))
		return (writer_error(FSIMG_EINVAL,
		    "APFS volume UUID is zero outside reproducible mode", error));
	if (reproducible && !time_ns(options->epoch, 0, &ns))
		return (writer_error(FSIMG_EINVAL,
		    "epoch is outside the APFS time range", error));
	/* A NULL or empty label selects the default, a static string the writer
	 * copies.
	 */
	if (options->label == NULL || options->label[0] == '\0')
		options->label = DEFAULT_LABEL;
	options->sector_bytes = APFS_WRITE_BLOCK_BYTES;
	options->allocation_unit_bytes = APFS_WRITE_BLOCK_BYTES;
	return (FSIMG_OK);
}

fsimg_status_t
apfs_row_get(const apfs_plan_t *plan, uint64_t id, apfs_row_t *row,
    fsimg_error_t *error)
{
	unsigned char raw[APFS_ROW_BYTES];
	fsimg_status_t status;

	status = fsimg_array_get(plan->rows, id, raw, error);
	if (status != FSIMG_OK)
		return (status);
	row->parent = fsimg_load_le64(raw + ROW_PARENT);
	row->length = fsimg_load_le64(raw + ROW_LENGTH);
	row->blocks_before = fsimg_load_le64(raw + ROW_BLOCKS_BEFORE);
	row->first_block = fsimg_load_le64(raw + ROW_FIRST_BLOCK);
	row->created = (int64_t)fsimg_load_le64(raw + ROW_CREATED);
	row->modified = (int64_t)fsimg_load_le64(raw + ROW_MODIFIED);
	row->changed = (int64_t)fsimg_load_le64(raw + ROW_CHANGED);
	row->accessed = (int64_t)fsimg_load_le64(raw + ROW_ACCESSED);
	row->children = fsimg_load_le32(raw + ROW_CHILDREN);
	row->hash = fsimg_load_le32(raw + ROW_HASH);
	row->uid = fsimg_load_le32(raw + ROW_UID);
	row->gid = fsimg_load_le32(raw + ROW_GID);
	row->mode = fsimg_load_le16(raw + ROW_MODE);
	row->kind = raw[ROW_KIND];
	return (FSIMG_OK);
}

static fsimg_status_t
row_set(apfs_plan_t *plan, uint64_t id, const apfs_row_t *row,
    fsimg_error_t *error)
{
	unsigned char raw[APFS_ROW_BYTES];

	bzero(raw, sizeof(raw));
	fsimg_store_le64(raw + ROW_PARENT, row->parent);
	fsimg_store_le64(raw + ROW_LENGTH, row->length);
	fsimg_store_le64(raw + ROW_BLOCKS_BEFORE, row->blocks_before);
	fsimg_store_le64(raw + ROW_FIRST_BLOCK, row->first_block);
	fsimg_store_le64(raw + ROW_CREATED, (uint64_t)row->created);
	fsimg_store_le64(raw + ROW_MODIFIED, (uint64_t)row->modified);
	fsimg_store_le64(raw + ROW_CHANGED, (uint64_t)row->changed);
	fsimg_store_le64(raw + ROW_ACCESSED, (uint64_t)row->accessed);
	fsimg_store_le32(raw + ROW_CHILDREN, row->children);
	fsimg_store_le32(raw + ROW_HASH, row->hash);
	fsimg_store_le32(raw + ROW_UID, row->uid);
	fsimg_store_le32(raw + ROW_GID, row->gid);
	fsimg_store_le16(raw + ROW_MODE, row->mode);
	raw[ROW_KIND] = row->kind;
	return (fsimg_array_set(plan->rows, id, raw, error));
}

uint64_t
apfs_run_block(const apfs_plan_t *plan, uint64_t index)
{
	return (plan->run_first + index);
}

/* Pass 1: names */

/* The UTF-16 code units of strict UTF-8: two for a four-byte sequence, one
 * for any other.
 */
static size_t
utf8_units(const char *text, size_t length)
{
	const unsigned char *bytes = (const unsigned char *)text;
	size_t units = 0;

	for (size_t i = 0; i < length; i++) {
		if ((bytes[i] & 0xc0) != 0x80)
			units += bytes[i] >= 0xf0 ? 2 : 1;
	}
	return (units);
}

static size_t
form_units(const apfs_name_form_t *form)
{
	size_t units = 0;

	for (size_t i = 0; i < form->count; i++)
		units += form->points[i] > 0xffff ? 2 : 1;
	return (units);
}

/*
 * Check a name against the writer's rules and derive its hash and
 * namespace key, into planner->key. Returns NULL, or why it is refused.
 * The reader's rules come first: apfs_name_form() refuses an empty name,
 * "." and "..", bytes that are not strict UTF-8, a NUL and a slash. Then
 * the writer's own: 255 UTF-16 units in the name and in its NFD, and every
 * code point assigned, which refuses noncharacters such as U+FFFF and
 * unassigned points such as U+0378.
 */
static const char *
name_check(apfs_planner_t *planner, const char *name, size_t length,
    uint32_t *hash, size_t *key_length)
{
	const apfs_name_form_t *form;

	if (length > APFS_WRITE_NAME_MAX_BYTES)
		return ("name is longer than 255 UTF-16 units");
	if (apfs_name_form(APFS_NAMES_NFD, name, length, &planner->nfd) !=
	    FSIMG_OK)
		return ("name is outside the APFS name rules");
	if (utf8_units(name, length) > APFS_WRITE_NAME_MAX_UNITS ||
	    form_units(&planner->nfd) > APFS_WRITE_NAME_MAX_UNITS)
		return ("name is longer than 255 UTF-16 units");
	if (!apfs_name_assigned(name, length))
		return ("name holds a noncharacter or a code point Unicode "
		    "17.0.0 does not assign");
	form = &planner->nfd;
	if (!planner->plan->case_sensitive) {
		if (apfs_name_form(APFS_NAMES_CASELESS, name, length,
		    &planner->form) != FSIMG_OK)
			return ("name is outside the APFS name rules");
		form = &planner->form;
	}
	*hash = apfs_name_hash(form);
	if (apfs_name_key(form, planner->key, sizeof(planner->key),
	    key_length) != FSIMG_OK)
		return ("name's comparison form exceeds its bound");
	return (NULL);
}

/* Pass 1: metadata */

/*
 * Store one timestamp under the metadata policy: an absent time takes the
 * format time, which is no loss. A present one is stored to the
 * nanosecond unless reproducible mode replaces it with the format time or
 * it lies outside the time range, which takes the format time too. Either
 * is a loss, as is a basis other than UTC, which the format cannot record,
 * except that a UTC time of the epoch's whole second is what reproducible
 * mode stores, and so exact (specs/design-metadata-loss.md, "Present,
 * reproducible").
 */
static fsimg_status_t
encode_time(apfs_planner_t *planner, const fsimg_entry_t *entry,
    uint32_t field, const fsimg_timestamp_t *time, int64_t *out,
    fsimg_error_t *error)
{
	fsimg_writer_t *writer = planner->writer;
	int64_t ns;
	bool utc = time->basis == FSIMG_TIME_UTC;

	*out = planner->plan->format_time;
	if ((entry->metadata.valid & field) == 0)
		return (FSIMG_OK);
	if ((writer->options.flags & FSIMG_WRITE_REPRODUCIBLE) != 0) {
		if (utc && time->nanoseconds == 0 &&
		    time->seconds == writer->options.epoch)
			return (FSIMG_OK);
	} else if (time_ns(time->seconds, time->nanoseconds, &ns)) {
		*out = ns;
		if (utc)
			return (FSIMG_OK);
	}
	return (fsimg_writer_loss(writer, entry, field,
	    "APFS cannot preserve the requested timestamp", error));
}

/* Select an entry's mode, owner and times into its row. */
static fsimg_status_t
encode_metadata(apfs_planner_t *planner, const fsimg_entry_t *entry,
    apfs_row_t *row, fsimg_error_t *error)
{
	const fsimg_metadata_t *m = &entry->metadata;
	uint32_t permissions, type;
	fsimg_status_t status;

	switch (entry->kind) {
	case FSIMG_ENTRY_FILE:
		type = MODE_FILE;
		permissions = 0644;
		break;
	case FSIMG_ENTRY_DIRECTORY:
		type = MODE_DIRECTORY;
		permissions = 0755;
		break;
	case FSIMG_ENTRY_SYMLINK:
		type = MODE_SYMLINK;
		permissions = 0755;
		break;
	case FSIMG_ENTRY_FIFO:
		type = MODE_FIFO;
		permissions = 0644;
		break;
	default:
		return (writer_error(FSIMG_ESTATE,
		    "APFS planning received an unknown entry kind", error));
	}
	if ((m->valid & FSIMG_META_MODE) != 0)
		permissions = m->mode & FSIMG_MODE_BITS;
	row->mode = (uint16_t)(type | permissions);
	row->kind = (uint8_t)entry->kind;
	if ((m->valid & FSIMG_META_OWNER) != 0) {
		row->uid = m->uid;
		row->gid = m->gid;
	}
	status = encode_time(planner, entry, FSIMG_META_CREATED, &m->created,
	    &row->created, error);
	if (status == FSIMG_OK)
		status = encode_time(planner, entry, FSIMG_META_MODIFIED,
		    &m->modified, &row->modified, error);
	if (status == FSIMG_OK)
		status = encode_time(planner, entry, FSIMG_META_CHANGED,
		    &m->changed, &row->changed, error);
	if (status == FSIMG_OK)
		status = encode_time(planner, entry, FSIMG_META_ACCESSED,
		    &m->accessed, &row->accessed, error);
	return (status);
}

/* Pass 1: entries */

/* Count the entry into the plan's totals; a file's data blocks too. */
static fsimg_status_t
count_entry(apfs_plan_t *plan, const fsimg_entry_t *entry, apfs_row_t *row,
    fsimg_error_t *error)
{
	uint64_t blocks;

	row->blocks_before = plan->data_blocks;
	switch (entry->kind) {
	case FSIMG_ENTRY_FILE:
		plan->files++;
		row->length = entry->size;
		blocks = apfs_row_blocks(row);
		/* Each sum stays at most the largest container's blocks, so
		 * no later sum of block counts can overflow.
		 */
		if (blocks > APFS_WRITER_BLOCKS_MAX - plan->data_blocks)
			return (entry_error(FSIMG_ERANGE, "file data exceeds "
			    "the largest APFS container", entry->path, error));
		plan->data_blocks += blocks;
		plan->common.info.initialized_bytes += entry->size;
		return (FSIMG_OK);
	case FSIMG_ENTRY_DIRECTORY:
		plan->directories++;
		return (FSIMG_OK);
	case FSIMG_ENTRY_SYMLINK:
		plan->symlinks++;
		row->length = entry->size;
		return (FSIMG_OK);
	default:
		plan->others++;
		return (FSIMG_OK);
	}
}

static fsimg_status_t
append_collision(apfs_planner_t *planner, uint64_t parent, uint64_t id,
    size_t key_length, fsimg_error_t *error)
{
	fsimg_store_le64(planner->frame, parent);
	fsimg_store_le64(planner->frame + 8, id);
	bcopy(planner->key, planner->frame + COLLISION_FIXED, key_length);
	return (fsimg_stream_append(planner->collisions, planner->frame,
	    COLLISION_FIXED + key_length, NULL, error));
}

/*
 * Pass 1 for one entry. A catalog entry's parent has a lower ID, so the
 * parent's row already exists and the entry counts itself into it.
 */
static fsimg_status_t
plan_entry(apfs_planner_t *planner, const fsimg_catalog_entry_t *catalog,
    fsimg_error_t *error)
{
	apfs_plan_t *plan = planner->plan;
	const fsimg_entry_t *entry = &catalog->entry;
	apfs_row_t row, parent;
	const char *component, *refused;
	size_t key_length;
	fsimg_status_t status;

	status = fsimg_session_charge(&planner->writer->session, 0, 0, 1,
	    error);
	if (status != FSIMG_OK)
		return (status);
	component = strrchr(entry->path, '/') + 1;
	bzero(&row, sizeof(row));
	refused = name_check(planner, component, strlen(component), &row.hash,
	    &key_length);
	if (refused != NULL)
		return (entry_error(FSIMG_EINVAL, refused, entry->path, error));
	row.parent = catalog->parent_id;
	status = encode_metadata(planner, entry, &row, error);
	if (status == FSIMG_OK)
		status = count_entry(plan, entry, &row, error);
	if (status == FSIMG_OK)
		status = row_set(plan, catalog->id, &row, error);
	if (status == FSIMG_OK)
		status = apfs_row_get(plan, catalog->parent_id, &parent, error);
	if (status != FSIMG_OK)
		return (status);
	/* The inode stores the child count as a signed 32-bit value. */
	if (parent.children == INT32_MAX)
		return (entry_error(FSIMG_ERANGE, "directory exceeds the APFS "
		    "child count", entry->path, error));
	parent.children++;
	status = row_set(plan, catalog->parent_id, &parent, error);
	if (status != FSIMG_OK)
		return (status);
	return (append_collision(planner, catalog->parent_id, catalog->id,
	    key_length, error));
}

/* The root's row: a directory with the default mode at the format time. */
static fsimg_status_t
plan_root(apfs_plan_t *plan, fsimg_error_t *error)
{
	apfs_row_t root;

	bzero(&root, sizeof(root));
	root.mode = MODE_DIRECTORY | 0755;
	root.kind = FSIMG_ENTRY_DIRECTORY;
	root.created = plan->format_time;
	root.modified = plan->format_time;
	root.changed = plan->format_time;
	root.accessed = plan->format_time;
	return (row_set(plan, 0, &root, error));
}

static fsimg_status_t
plan_entries(apfs_planner_t *planner, fsimg_error_t *error)
{
	fsimg_writer_t *writer = planner->writer;
	const fsimg_catalog_entry_t *catalog;
	fsimg_status_t status;

	status = plan_root(planner->plan, error);
	for (uint64_t id = 1; status == FSIMG_OK && id <= writer->count; id++) {
		status = fsimg_catalog_get(writer, id, &catalog, error);
		if (status == FSIMG_OK)
			status = plan_entry(planner, catalog, error);
	}
	return (status);
}

/* Pass 2: collisions */

/* The collision order: parent, namespace key, then catalog ID. */
static int
collision_compare(const void *a, size_t alen, const void *b, size_t blen,
    void *argument)
{
	const unsigned char *x = a, *y = b;
	uint64_t u, v;
	size_t common;
	int order;

	(void)argument;
	u = fsimg_load_le64(x);
	v = fsimg_load_le64(y);
	if (u != v)
		return (u < v ? -1 : 1);
	common = (alen < blen ? alen : blen) - COLLISION_FIXED;
	order = memcmp(x + COLLISION_FIXED, y + COLLISION_FIXED, common);
	if (order != 0)
		return (order < 0 ? -1 : 1);
	if (alen != blen)
		return (alen < blen ? -1 : 1);
	u = fsimg_load_le64(x + 8);
	v = fsimg_load_le64(y + 8);
	return (u < v ? -1 : u > v);
}

/*
 * Sort the collision stream and refuse two equal (parent, form) records,
 * naming the later of the two: the sort ties them by catalog ID, so the
 * entry named does not depend on source order.
 */
static fsimg_status_t
check_collisions(apfs_planner_t *planner, fsimg_error_t *error)
{
	fsimg_writer_t *writer = planner->writer;
	const fsimg_catalog_entry_t *catalog;
	fsimg_stream_t *sorted;
	uint64_t cursor = 0;
	size_t length, previous_length = 0;
	fsimg_status_t status;

	status = fsimg_stream_seal(planner->collisions, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_sort(planner->collisions,
		    collision_compare, NULL, NULL, &sorted, error);
	if (status != FSIMG_OK)
		return (status);
	for (;;) {
		status = fsimg_stream_next(sorted, &cursor, planner->frame,
		    COLLISION_MAX, &length, error);
		if (status == FSIMG_DONE) {
			status = FSIMG_OK;
			break;
		}
		if (status == FSIMG_OK)
			status = fsimg_session_charge(&writer->session, 0, 0, 1,
			    error);
		if (status != FSIMG_OK)
			break;
		if (length < COLLISION_FIXED) {
			status = writer_error(FSIMG_ESTATE,
			    "APFS collision record is inconsistent", error);
			break;
		}
		if (previous_length == length &&
		    fsimg_load_le64(planner->previous) ==
		    fsimg_load_le64(planner->frame) &&
		    memcmp(planner->previous + COLLISION_FIXED,
		    planner->frame + COLLISION_FIXED,
		    length - COLLISION_FIXED) == 0) {
			status = fsimg_catalog_get(writer,
			    fsimg_load_le64(planner->frame + 8), &catalog, error);
			if (status == FSIMG_OK)
				status = entry_error(FSIMG_EINVAL, "name is "
				    "equivalent to a sibling under the APFS "
				    "volume's name rules", catalog->entry.path,
				    error);
			break;
		}
		bcopy(planner->frame, planner->previous, length);
		previous_length = length;
	}
	fsimg_stream_free(sorted);
	/* Only the check reads the collision records. */
	fsimg_stream_free(planner->collisions);
	planner->collisions = NULL;
	return (status);
}

/* Pass 3: records */

bool
apfs_write_record_decode(const unsigned char *raw, size_t length,
    apfs_write_record_t *record)
{
	if (length < APFS_WRITE_FRAME_FIXED)
		return (false);
	record->id = fsimg_load_le64(raw);
	record->key_len = fsimg_load_le16(raw + 8);
	record->val_len = fsimg_load_le16(raw + 10);
	if ((size_t)APFS_WRITE_FRAME_FIXED + record->key_len +
	    record->val_len != length)
		return (false);
	/* Every filesystem key holds its header, which the comparator reads. */
	if (record->key_len < APFS_JKEY_BYTES)
		return (false);
	record->key = raw + APFS_WRITE_FRAME_FIXED;
	record->val = record->key + record->key_len;
	return (true);
}

/* The filesystem tree's key order, then the catalog ID, which no two
 * records of distinct keys need but which makes the order total.
 */
static int
record_compare(const void *a, size_t alen, const void *b, size_t blen,
    void *argument)
{
	apfs_write_record_t x, y;
	int order;

	(void)argument;
	if (!apfs_write_record_decode(a, alen, &x) ||
	    !apfs_write_record_decode(b, blen, &y))
		return (alen < blen ? -1 : alen > blen);
	order = apfs_fs_tree_profile(true)->compare(x.key, x.key_len, y.key,
	    y.key_len);
	if (order != 0)
		return (order);
	return (x.id < y.id ? -1 : x.id > y.id);
}

static fsimg_status_t
encode_failed(fsimg_error_t *error)
{
	return (writer_error(FSIMG_ESTATE,
	    "APFS record does not encode within its bounds", error));
}

/* Append one encoded record, borrowed for the call, to the stream. */
static fsimg_status_t
append_record(apfs_planner_t *planner, uint64_t id, const unsigned char *key,
    size_t key_len, const unsigned char *val, size_t val_len,
    fsimg_error_t *error)
{
	unsigned char *frame = planner->frame;

	if (key_len > APFS_WRITE_KEY_MAX || val_len > APFS_WRITE_VAL_MAX)
		return (encode_failed(error));
	fsimg_store_le64(frame, id);
	fsimg_store_le16(frame + 8, (uint16_t)key_len);
	fsimg_store_le16(frame + 10, (uint16_t)val_len);
	bcopy(key, frame + APFS_WRITE_FRAME_FIXED, key_len);
	bcopy(val, frame + APFS_WRITE_FRAME_FIXED + key_len, val_len);
	return (fsimg_stream_append(planner->unsorted, frame,
	    APFS_WRITE_FRAME_FIXED + key_len + val_len, NULL, error));
}

/* What an inode record holds beyond its row. */
typedef struct apfs_inode_args {
	const char	*name;		/* NUL-terminated after name_len */
	size_t		 name_len;
	uint64_t	 ino;
	uint64_t	 parent_ino;
} apfs_inode_args_t;

/*
 * An inode record: the fixed part from the row and the writer values, then
 * the name field and, for a file with content, the data-stream field. The
 * blob is encoded in place after the fixed part and decoded into the
 * record, as the codec's contract asks.
 */
static fsimg_status_t
append_inode(apfs_planner_t *planner, uint64_t id, const apfs_row_t *row,
    const apfs_inode_args_t *args, fsimg_error_t *error)
{
	unsigned char key[APFS_JKEY_BYTES], val[INODE_VAL_MAX];
	unsigned char stream[APFS_DSTREAM_BYTES];
	apfs_xfield_t fields[2];
	apfs_inode_val_t inode;
	apfs_dstream_t dstream;
	uint16_t count = 1;
	size_t blob;
	bool has_stream;

	has_stream = row->kind == FSIMG_ENTRY_FILE && row->length != 0;
	fields[0].data = (const unsigned char *)args->name;
	fields[0].size = (uint16_t)(args->name_len + 1);
	fields[0].type = APFS_XF_INO_NAME;
	fields[0].flags = NAME_FIELD_FLAGS;
	if (has_stream) {
		bzero(&dstream, sizeof(dstream));
		dstream.size = row->length;
		dstream.alloced_size = apfs_row_blocks(row) *
		    APFS_WRITE_BLOCK_BYTES;
		dstream.total_bytes_written = row->length;
		if (!apfs_dstream_encode(stream, sizeof(stream), &dstream))
			return (encode_failed(error));
		fields[1].data = stream;
		fields[1].size = APFS_DSTREAM_BYTES;
		fields[1].type = APFS_XF_INO_DSTREAM;
		fields[1].flags = DSTREAM_FIELD_FLAGS;
		count = 2;
	}
	bzero(&inode, sizeof(inode));
	/* The blob's length, which the value buffer must hold. */
	if (!apfs_xfields_size(fields, count, &blob) ||
	    blob > sizeof(val) - APFS_INODE_VAL_BYTES)
		return (encode_failed(error));
	/* Encode it after the fixed part... */
	if (!apfs_xfields_encode(val + APFS_INODE_VAL_BYTES, blob, fields,
	    count))
		return (encode_failed(error));
	/* ...and decode it into the record, whose encoder copies it onto
	 * itself.
	 */
	if (!apfs_xfields_decode(val + APFS_INODE_VAL_BYTES, blob,
	    &inode.xfields))
		return (encode_failed(error));
	inode.parent = args->parent_ino;
	inode.private_id = args->ino;
	inode.create_time = row->created;
	inode.mod_time = row->modified;
	inode.change_time = row->changed;
	inode.access_time = row->accessed;
	inode.internal_flags = INODE_INTERNAL_FLAGS;
	inode.count = row->kind == FSIMG_ENTRY_DIRECTORY ?
	    (int32_t)row->children : 1;
	inode.protection_class = row->kind == FSIMG_ENTRY_FIFO ?
	    PROTECTION_FIFO : 0;
	if (row->kind == FSIMG_ENTRY_DIRECTORY)
		inode.write_generation = 1 + row->children;
	else
		inode.write_generation = has_stream ? 2 : 1;
	inode.owner = row->uid;
	inode.group = row->gid;
	inode.mode = row->mode;
	if (!apfs_bare_key_encode(key, sizeof(key), APFS_J_INODE, args->ino) ||
	    !apfs_inode_val_encode(val, APFS_INODE_VAL_BYTES + blob, &inode))
		return (encode_failed(error));
	return (append_record(planner, id, key, sizeof(key), val,
	    APFS_INODE_VAL_BYTES + blob, error));
}

static unsigned
drec_type(uint8_t kind)
{
	switch (kind) {
	case FSIMG_ENTRY_DIRECTORY:
		return (APFS_DT_DIR);
	case FSIMG_ENTRY_SYMLINK:
		return (APFS_DT_LNK);
	case FSIMG_ENTRY_FIFO:
		return (APFS_DT_FIFO);
	default:
		return (APFS_DT_REG);
	}
}

/* A hashed directory record in the parent, added at the format time. */
static fsimg_status_t
append_drec(apfs_planner_t *planner, uint64_t id,
    const apfs_inode_args_t *args, uint32_t hash, uint8_t kind,
    fsimg_error_t *error)
{
	unsigned char key[APFS_WRITE_KEY_MAX], val[APFS_DREC_VAL_BYTES];
	apfs_drec_key_t drec_key;
	apfs_drec_val_t drec;
	size_t key_len;

	bzero(&drec_key, sizeof(drec_key));
	drec_key.name = args->name;
	drec_key.parent = args->parent_ino;
	drec_key.hash = hash;
	drec_key.name_len = (uint16_t)args->name_len;
	drec_key.hashed = true;
	key_len = APFS_DREC_HASHED_KEY_BYTES + args->name_len + 1;
	bzero(&drec, sizeof(drec));
	drec.child = args->ino;
	drec.date_added = planner->plan->format_time;
	drec.flags = (uint16_t)drec_type(kind);
	if (key_len > sizeof(key) ||
	    !apfs_drec_key_encode(key, key_len, &drec_key) ||
	    !apfs_drec_val_encode(val, sizeof(val), &drec))
		return (encode_failed(error));
	return (append_record(planner, id, key, key_len, val, sizeof(val),
	    error));
}

/*
 * A file's data-stream ID record and the file extents that tile its run,
 * ceil(r / cap) of them for its rounded size r, each of the cap's length
 * but the last. The physical block stays zero: emission fills it from the
 * row, and its width does not depend on the value.
 */
static fsimg_status_t
append_stream(apfs_planner_t *planner, uint64_t id, const apfs_row_t *row,
    uint64_t ino, fsimg_error_t *error)
{
	unsigned char key[APFS_EXTENT_KEY_BYTES], val[APFS_EXTENT_VAL_BYTES];
	apfs_extent_key_t extent_key;
	apfs_extent_val_t extent;
	uint64_t rounded;
	fsimg_status_t status;

	if (!apfs_bare_key_encode(key, APFS_JKEY_BYTES, APFS_J_DSTREAM_ID,
	    ino) || !apfs_dstream_id_val_encode(val, APFS_DSTREAM_ID_VAL_BYTES,
	    1))
		return (encode_failed(error));
	status = append_record(planner, id, key, APFS_JKEY_BYTES, val,
	    APFS_DSTREAM_ID_VAL_BYTES, error);
	rounded = apfs_row_blocks(row) * APFS_WRITE_BLOCK_BYTES;
	for (uint64_t logical = 0; status == FSIMG_OK && logical < rounded;
	    logical += APFS_WRITE_EXTENT_CAP) {
		extent_key.private_id = ino;
		extent_key.logical = logical;
		bzero(&extent, sizeof(extent));
		extent.length = rounded - logical < APFS_WRITE_EXTENT_CAP ?
		    rounded - logical : APFS_WRITE_EXTENT_CAP;
		if (!apfs_extent_key_encode(key, sizeof(key), &extent_key) ||
		    !apfs_extent_val_encode(val, sizeof(val), &extent))
			return (encode_failed(error));
		status = append_record(planner, id, key, sizeof(key), val,
		    sizeof(val), error);
	}
	return (status);
}

/*
 * A link's target, read through the common link-target operation, in its
 * inline filesystem-owned attribute, ending in one NUL. The catalog bounds
 * the length; a target the read returns at another length, empty, longer
 * than 1023 bytes or holding a NUL would not read back as itself.
 */
static fsimg_status_t
append_link(apfs_planner_t *planner, const fsimg_catalog_entry_t *catalog,
    const apfs_row_t *row, uint64_t ino, fsimg_error_t *error)
{
	unsigned char key[APFS_WRITE_KEY_MAX], val[APFS_WRITE_VAL_MAX];
	apfs_name_key_t attribute;
	apfs_xattr_val_t xattr;
	size_t key_len, length;
	fsimg_status_t status;

	status = fsimg_writer_link_target(planner->writer, catalog->id,
	    planner->target, &length, error);
	if (status != FSIMG_OK)
		return (status);
	if (length != row->length || length == 0 ||
	    length >= FSIMG_SYMLINK_TARGET_MAX ||
	    memchr(planner->target, '\0', length) != NULL)
		return (entry_error(FSIMG_EINVAL, "link target is outside "
		    "the APFS target rules", catalog->entry.path, error));
	planner->target[length] = '\0';
	attribute.name = SYMLINK_ATTRIBUTE;
	attribute.oid = ino;
	attribute.name_len = (uint16_t)(sizeof(SYMLINK_ATTRIBUTE) - 1);
	key_len = APFS_XATTR_KEY_BYTES + sizeof(SYMLINK_ATTRIBUTE);
	bzero(&xattr, sizeof(xattr));
	xattr.data = planner->target;
	xattr.flags = APFS_XATTR_DATA_EMBEDDED | APFS_XATTR_FS_OWNED;
	xattr.data_len = (uint16_t)(length + 1);
	if (!apfs_xattr_key_encode(key, key_len, &attribute) ||
	    !apfs_xattr_val_encode(val, APFS_XATTR_VAL_BYTES + length + 1,
	    &xattr))
		return (encode_failed(error));
	return (append_record(planner, catalog->id, key, key_len, val,
	    APFS_XATTR_VAL_BYTES + length + 1, error));
}

/* The records of one catalog entry. */
static fsimg_status_t
entry_records(apfs_planner_t *planner, const fsimg_catalog_entry_t *catalog,
    fsimg_error_t *error)
{
	const char *component = strrchr(catalog->entry.path, '/') + 1;
	apfs_inode_args_t args;
	apfs_row_t row;
	fsimg_status_t status;

	status = fsimg_session_charge(&planner->writer->session, 0, 0, 1,
	    error);
	if (status == FSIMG_OK)
		status = apfs_row_get(planner->plan, catalog->id, &row, error);
	if (status != FSIMG_OK)
		return (status);
	args.name = component;
	args.name_len = strlen(component);
	args.ino = apfs_write_ino(catalog->id);
	args.parent_ino = apfs_write_ino(catalog->parent_id);
	status = append_inode(planner, catalog->id, &row, &args, error);
	if (status == FSIMG_OK)
		status = append_drec(planner, catalog->id, &args, row.hash,
		    row.kind, error);
	if (status == FSIMG_OK && row.kind == FSIMG_ENTRY_FILE &&
	    row.length != 0)
		status = append_stream(planner, catalog->id, &row, args.ino,
		    error);
	if (status == FSIMG_OK && row.kind == FSIMG_ENTRY_SYMLINK)
		status = append_link(planner, catalog, &row, args.ino, error);
	return (status);
}

/*
 * The native records of the empty volume: the root and private
 * directories, both under parent 1, with their native names. The private
 * directory stays empty and has the root's metadata.
 */
static fsimg_status_t
volume_records(apfs_planner_t *planner, fsimg_error_t *error)
{
	static const struct {
		const char	*name;
		uint64_t	 ino;
	} reserved[] = {
		{ ROOT_NAME, APFS_WRITE_INO_ROOT },
		{ PRIVATE_NAME, APFS_WRITE_INO_PRIVATE }
	};
	apfs_inode_args_t args;
	apfs_row_t root, row;
	size_t key_length;
	uint32_t hash;
	fsimg_status_t status;

	status = apfs_row_get(planner->plan, 0, &root, error);
	for (size_t i = 0; status == FSIMG_OK &&
	    i < sizeof(reserved) / sizeof(reserved[0]); i++) {
		args.name = reserved[i].name;
		args.name_len = strlen(reserved[i].name);
		args.ino = reserved[i].ino;
		args.parent_ino = APFS_WRITE_INO_ROOT_PARENT;
		row = root;
		if (reserved[i].ino == APFS_WRITE_INO_PRIVATE)
			row.children = 0;
		if (name_check(planner, args.name, args.name_len, &hash,
		    &key_length) != NULL)
			return (writer_error(FSIMG_ESTATE,
			    "APFS reserved name is refused", error));
		/* Row 0 completes the root's records; the private
		 * directory's need no row.
		 */
		status = append_inode(planner, 0, &row, &args, error);
		if (status == FSIMG_OK)
			status = append_drec(planner, 0, &args, hash,
			    FSIMG_ENTRY_DIRECTORY, error);
	}
	return (status);
}

static fsimg_status_t
plan_records(apfs_planner_t *planner, fsimg_error_t *error)
{
	fsimg_writer_t *writer = planner->writer;
	const fsimg_catalog_entry_t *catalog;
	fsimg_status_t status;

	status = volume_records(planner, error);
	for (uint64_t id = 1; status == FSIMG_OK && id <= writer->count; id++) {
		status = fsimg_catalog_get(writer, id, &catalog, error);
		if (status == FSIMG_OK)
			status = entry_records(planner, catalog, error);
	}
	if (status == FSIMG_OK)
		status = fsimg_stream_seal(planner->unsorted, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_sort(planner->unsorted, record_compare,
		    NULL, NULL, &planner->plan->records, error);
	/* The sorted stream replaces it. */
	fsimg_stream_free(planner->unsorted);
	planner->unsorted = NULL;
	return (status);
}

/* Pass 4: packing */

/*
 * Count every tree through its generator (write-trees.c), then give each
 * metadata object its run index in the format design's order: the
 * container object map's block and its tree, the volume superblock, the
 * volume object map's block and its tree, the filesystem, extent-reference
 * and snapshot trees.
 */
static fsimg_status_t
pack_trees(apfs_planner_t *planner, fsimg_error_t *error)
{
	apfs_plan_t *plan = planner->plan;
	apfs_tree_plan_t *trees = plan->trees;
	apfs_tree_run_t run = { .writer = planner->writer, .plan = plan };
	fsimg_status_t status;

	for (unsigned id = 0; id < APFS_TREE_COUNT; id++) {
		status = apfs_tree_pack(&run, id, &trees[id].shape, error);
		if (status != FSIMG_OK)
			return (status);
	}
	plan->comap_run = 0;
	trees[APFS_TREE_COMAP].first_run = 1;
	plan->volume_run = trees[APFS_TREE_COMAP].first_run +
	    trees[APFS_TREE_COMAP].shape.node_count;
	plan->vomap_run = plan->volume_run + 1;
	trees[APFS_TREE_VOMAP].first_run = plan->vomap_run + 1;
	trees[APFS_TREE_FS].first_run = trees[APFS_TREE_VOMAP].first_run +
	    trees[APFS_TREE_VOMAP].shape.node_count;
	trees[APFS_TREE_EXTENTREF].first_run = trees[APFS_TREE_FS].first_run +
	    trees[APFS_TREE_FS].shape.node_count;
	trees[APFS_TREE_SNAPSHOT].first_run =
	    trees[APFS_TREE_EXTENTREF].first_run +
	    trees[APFS_TREE_EXTENTREF].shape.node_count;
	plan->meta_blocks = trees[APFS_TREE_SNAPSHOT].first_run +
	    trees[APFS_TREE_SNAPSHOT].shape.node_count;
	plan->volume_allocated = trees[APFS_TREE_VOMAP].shape.node_count + 1 +
	    trees[APFS_TREE_FS].shape.node_count +
	    trees[APFS_TREE_EXTENTREF].shape.node_count +
	    trees[APFS_TREE_SNAPSHOT].shape.node_count + plan->data_blocks;
	plan->next_oid = APFS_WRITE_OID_FS_FIRST +
	    trees[APFS_TREE_FS].shape.node_count;
	return (FSIMG_OK);
}

/* Passes 5 and 6: the run */

/*
 * The native geometry and fixed regions for N blocks, and the run's first
 * block. The fields are the writer's own, so regions the check refuses are
 * an internal error. The native formatter places the fixed regions, which
 * the list holds in ascending block order, as one gapless prefix from
 * block 0, and the run is contiguous after it. A geometry with a gap would
 * need the run to skip a region, which the layout does not do, so the
 * check refuses it loudly instead of planning blocks inside a region.
 * apfs_fixed_regions() keeps every region inside the N blocks, so the sum
 * cannot wrap.
 */
static fsimg_status_t
geometry_for(uint64_t blocks, apfs_nx_t *nx, apfs_spaceman_t *sm,
    apfs_region_t regions[APFS_FIXED_REGIONS], uint64_t *run_first,
    fsimg_error_t *error)
{
	fsimg_error_t detail;
	uint64_t next = 0;

	if (!apfs_native_geometry(blocks, nx, sm) ||
	    apfs_fixed_regions(nx, sm, regions, &detail) != FSIMG_OK)
		return (writer_error(FSIMG_ESTATE,
		    "APFS native geometry is inconsistent", error));
	for (size_t i = 0; i < APFS_FIXED_REGIONS; i++) {
		if (regions[i].first != next)
			return (writer_error(FSIMG_ESTATE,
			    "APFS fixed regions do not form a prefix", error));
		next += regions[i].count;
	}
	*run_first = next;
	return (FSIMG_OK);
}

/*
 * The block after the run: the fixed-region prefix, the metadata blocks
 * and every file's data, contiguous. It is also the allocated block count,
 * since the allocation is exactly [0, end). run_first is at most N and
 * data_blocks at most 2^35 under the entry pass, so the sum cannot wrap.
 */
static uint64_t
run_end(const apfs_plan_t *plan, uint64_t run_first)
{
	return (run_first + plan->meta_blocks + plan->data_blocks);
}

/*
 * The blocks N lacks to hold a run ending at end and the requested free
 * space after it, so no count below N plus the shortfall can hold it. Zero
 * when N fits.
 */
static uint64_t
shortfall(uint64_t end, uint64_t blocks, uint64_t free_blocks)
{
	uint64_t needed = end + free_blocks;

	return (needed > blocks ? needed - blocks : 0);
}

/* The free blocks the options request. */
static uint64_t
requested_free(const apfs_plan_t *plan)
{
	uint64_t bytes = plan->common.writer->options.min_free_bytes;

	return (bytes / APFS_WRITE_BLOCK_BYTES +
	    (bytes % APFS_WRITE_BLOCK_BYTES != 0));
}

/*
 * Evaluate the layout at N: its shortfall, zero when it fits. Sizing and
 * layout share geometry_for() and run_end(), so they cannot disagree.
 */
static fsimg_status_t
probe(apfs_plan_t *plan, uint64_t blocks, uint64_t *missing,
    fsimg_error_t *error)
{
	apfs_nx_t nx;
	apfs_spaceman_t sm;
	apfs_region_t regions[APFS_FIXED_REGIONS];
	uint64_t run_first;
	fsimg_status_t status;

	status = fsimg_session_charge(&plan->common.writer->session, 0, 0, 1,
	    error);
	if (status == FSIMG_OK)
		status = geometry_for(blocks, &nx, &sm, regions, &run_first,
		    error);
	if (status == FSIMG_OK)
		*missing = shortfall(run_end(plan, run_first), blocks,
		    requested_free(plan));
	return (status);
}

/*
 * Pass 5: N. An exact size must fit. Otherwise the search starts at the
 * lower bound every N needs, raised to the minimum container (the size
 * step is one block), and each failed probe advances past its whole
 * shortfall. Minimization takes the same first fit.
 */
static fsimg_status_t
choose_blocks(apfs_plan_t *plan, fsimg_error_t *error)
{
	const fsimg_write_options_t *options = &plan->common.writer->options;
	uint64_t blocks, missing;
	fsimg_status_t status;

	if (options->volume_bytes != 0) {
		blocks = options->volume_bytes / APFS_WRITE_BLOCK_BYTES;
		status = probe(plan, blocks, &missing, error);
		if (status == FSIMG_OK && missing != 0)
			status = writer_error(FSIMG_ERANGE,
			    "APFS content does not fit the container", error);
		plan->block_count = blocks;
		return (status);
	}
	if (!fsimg_add_u64(1 + plan->meta_blocks, plan->data_blocks,
	    &blocks) || !fsimg_add_u64(blocks, requested_free(plan), &blocks))
		blocks = UINT64_MAX;
	if (blocks < APFS_WRITE_BLOCKS_MIN)
		blocks = APFS_WRITE_BLOCKS_MIN;
	for (uint64_t probes = 0;; probes++) {
		if (blocks > APFS_WRITER_BLOCKS_MAX)
			return (writer_error(FSIMG_ERANGE, "APFS content needs "
			    "more than the largest container, 34359738368 "
			    "blocks", error));
		if (probes == PROBE_CAP)
			return (writer_error(FSIMG_ERANGE, "APFS size search "
			    "reached its cap of 65536 probes", error));
		status = probe(plan, blocks, &missing, error);
		if (status != FSIMG_OK)
			return (status);
		if (missing == 0)
			break;
		/* Both terms are at most about 2^53, far below overflow. */
		blocks += missing;
	}
	plan->block_count = blocks;
	return (FSIMG_OK);
}

/* The chunks the allocation, blocks [0, end), touches. */
static uint64_t
touched_chunks(const apfs_plan_t *plan, uint64_t end)
{
	uint64_t per_chunk = plan->sm.blocks_per_chunk;

	return (end / per_chunk + (end % per_chunk != 0));
}

/*
 * Give every file with data its first block: file data follows the
 * metadata in catalog order, so the file whose data starts at offset b
 * starts at block first + b.
 */
static fsimg_status_t
assign_files(apfs_plan_t *plan, uint64_t first, fsimg_error_t *error)
{
	fsimg_session_t *session = &plan->common.writer->session;
	uint64_t count = plan->common.writer->count;
	apfs_row_t row;
	fsimg_status_t status = FSIMG_OK;

	for (uint64_t id = 1; status == FSIMG_OK && id <= count; id++) {
		status = fsimg_session_charge(session, 0, 0, 1, error);
		if (status == FSIMG_OK)
			status = apfs_row_get(plan, id, &row, error);
		if (status != FSIMG_OK || apfs_row_blocks(&row) == 0)
			continue;
		row.first_block = first + row.blocks_before;
		status = row_set(plan, id, &row, error);
	}
	return (status);
}

/*
 * Pass 6: the regions and the native fields for N, the run, every file's
 * first block, the ephemeral table and the internal pool's blocks.
 * Metadata indexes and OIDs were fixed by packing.
 */
static fsimg_status_t
layout(apfs_plan_t *plan, fsimg_error_t *error)
{
	const apfs_sm_device_t *main;
	fsimg_error_t detail;
	fsimg_status_t status;

	status = geometry_for(plan->block_count, &plan->nx, &plan->sm,
	    plan->regions, &plan->run_first, error);
	if (status != FSIMG_OK)
		return (status);
	plan->allocated_blocks = run_end(plan, plan->run_first);
	if (shortfall(plan->allocated_blocks, plan->block_count,
	    requested_free(plan)) != 0)
		return (writer_error(FSIMG_ESTATE,
		    "APFS layout disagrees with its size", error));
	status = assign_files(plan, plan->run_first + plan->meta_blocks, error);
	if (status != FSIMG_OK)
		return (status);
	plan->common.info.allocated_bytes = plan->allocated_blocks *
	    APFS_WRITE_BLOCK_BYTES;
	if (apfs_spaceman_check(&plan->sm, APFS_WRITE_BLOCK_BYTES,
	    plan->block_count, &plan->sm_blocks, &detail) != FSIMG_OK)
		return (writer_error(FSIMG_ESTATE,
		    "APFS native space manager is inconsistent", error));
	plan->ephemeral[0].oid = APFS_WRITE_OID_SPACEMAN;
	plan->ephemeral[0].kind = APFS_KIND_SPACEMAN;
	plan->ephemeral[0].bytes = (uint32_t)(plan->sm_blocks *
	    APFS_WRITE_BLOCK_BYTES);
	plan->ephemeral[0].first_slot = 0;
	plan->ephemeral[1].oid = APFS_WRITE_OID_REAPER;
	plan->ephemeral[1].kind = APFS_KIND_NX_REAPER;
	plan->ephemeral[1].bytes = APFS_WRITE_BLOCK_BYTES;
	plan->ephemeral[1].first_slot = (uint32_t)plan->sm_blocks;

	main = &plan->sm.devices[APFS_SM_DEVICE_MAIN];
	plan->pool_cibs = main->cib_count;
	plan->pool_cabs = main->cab_count;
	plan->pool_bitmaps = touched_chunks(plan, plan->allocated_blocks);
	if (plan->pool_cibs + plan->pool_cabs + plan->pool_bitmaps >
	    plan->sm.ip_block_count)
		return (writer_error(FSIMG_ESTATE,
		    "APFS internal pool does not hold its blocks", error));
	return (FSIMG_OK);
}

/* Pass 7: everything that is not file data is metadata. */
static void
fill_summary(apfs_plan_t *plan)
{
	fsimg_plan_info_t *info = &plan->common.info;
	uint64_t total = plan->block_count * APFS_WRITE_BLOCK_BYTES;

	info->volume_bytes = total;
	info->sector_bytes = APFS_WRITE_BLOCK_BYTES;
	info->allocation_unit_bytes = APFS_WRITE_BLOCK_BYTES;
	info->allocation_units = plan->block_count;
	info->free_bytes = total - info->allocated_bytes;
	info->metadata_bytes = info->allocated_bytes -
	    plan->data_blocks * APFS_WRITE_BLOCK_BYTES;
	info->scratch_bytes = plan->common.writer->ctx->stats.scratch_bytes;
	plan->next_ino = APFS_WRITE_INO_BASE + plan->common.writer->count + 1;
}

/*
 * The format time, the one host clock read: the epoch when reproducible,
 * which validation has kept inside the time range, otherwise the UTC
 * clock at planning, refused outside it.
 */
static fsimg_status_t
select_format_time(apfs_plan_t *plan, fsimg_error_t *error)
{
	const fsimg_write_options_t *options = &plan->common.writer->options;
	struct timespec now;

	if ((options->flags & FSIMG_WRITE_REPRODUCIBLE) != 0) {
		if (!time_ns(options->epoch, 0, &plan->format_time))
			return (writer_error(FSIMG_ESTATE,
			    "epoch is outside the APFS time range", error));
		return (FSIMG_OK);
	}
	if (clock_gettime(CLOCK_REALTIME, &now) != 0)
		return (writer_error(FSIMG_EIO, "host clock is unavailable",
		    error));
	if (now.tv_nsec < 0 || !time_ns((int64_t)now.tv_sec,
	    (uint32_t)now.tv_nsec, &plan->format_time))
		return (writer_error(FSIMG_ERANGE,
		    "host clock is outside the APFS time range", error));
	return (FSIMG_OK);
}

/*
 * The UUIDs: the volume's from the setting or, in reproducible mode, role 5
 * of the layer identifier rule; the container's the first 16 bytes of the
 * SHA-256 of "fsimg-apfs-container" and the volume UUID, as a version 8
 * UUID of the RFC 4122 variant.
 */
static fsimg_status_t
select_uuids(apfs_plan_t *plan, fsimg_error_t *error)
{
	const fsimg_write_options_t *options = &plan->common.writer->options;
	unsigned char digest[FSIMG_SHA256_DIGEST_LENGTH];
	fsimg_sha256_t sha;
	fsimg_status_t status;

	status = fsimg_container_identifier(options->volume_uuid, "uuid",
	    FSIMG_ROLE_APFS, options->flags, options->epoch, plan->volume_uuid,
	    error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_sha256_init(&sha);
	fsimg_sha256_update(&sha, CONTAINER_UUID_PREFIX,
	    sizeof(CONTAINER_UUID_PREFIX) - 1);
	fsimg_sha256_update(&sha, plan->volume_uuid, sizeof(plan->volume_uuid));
	fsimg_sha256_final(digest, &sha);
	bcopy(digest, plan->container_uuid, sizeof(plan->container_uuid));
	plan->container_uuid[6] = (unsigned char)((digest[6] & 0x0f) | 0x80);
	plan->container_uuid[8] = (unsigned char)((digest[8] & 0x3f) | 0x80);
	return (FSIMG_OK);
}

/* The passes, with the plan allocated and the planner's streams open. */
static fsimg_status_t
run_passes(apfs_planner_t *planner, fsimg_error_t *error)
{
	apfs_plan_t *plan = planner->plan;
	fsimg_writer_t *writer = planner->writer;
	fsimg_status_t status;

	status = select_format_time(plan, error);
	if (status == FSIMG_OK)
		status = select_uuids(plan, error);
	if (status == FSIMG_OK)
		status = fsimg_array_new(&writer->session, APFS_ROW_BYTES,
		    writer->count + 1, &plan->rows, error);
	if (status == FSIMG_OK)
		status = plan_entries(planner, error);
	if (status == FSIMG_OK)
		status = check_collisions(planner, error);
	if (status == FSIMG_OK)
		status = plan_records(planner, error);
	if (status == FSIMG_OK)
		status = pack_trees(planner, error);
	if (status == FSIMG_OK)
		status = choose_blocks(plan, error);
	if (status == FSIMG_OK)
		status = layout(plan, error);
	if (status == FSIMG_OK)
		fill_summary(plan);
	return (status);
}

/* Allocate bytes from the context, zeroed, or fail through error. */
static fsimg_status_t
alloc_zeroed(fsimg_ctx_t *ctx, size_t bytes, void **memory,
    fsimg_error_t *error)
{
	fsimg_error_t detail;

	*memory = fsimg_alloc(ctx, bytes, &detail);
	if (*memory == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	bzero(*memory, bytes);
	return (FSIMG_OK);
}

fsimg_status_t
apfs_write_plan(fsimg_writer_t *writer, fsimg_error_t *error)
{
	apfs_planner_t *planner;
	apfs_plan_t *plan;
	fsimg_plan_info_t *info;
	fsimg_status_t status;
	void *memory;

	status = alloc_zeroed(writer->ctx, sizeof(*plan), &memory, error);
	if (status != FSIMG_OK)
		return (status);
	plan = memory;
	status = alloc_zeroed(writer->ctx, sizeof(*planner), &memory, error);
	if (status != FSIMG_OK) {
		fsimg_dealloc(plan);
		return (status);
	}
	planner = memory;
	plan->common.writer = writer;
	plan->case_sensitive = writer->options.case_sensitive;
	info = &plan->common.info;
	strcpy(info->format, "apfs");
	info->entries = writer->count;
	info->payload_bytes = writer->payload_bytes;
	info->required_io_flags = FSIMG_IO_WRITE;
	if ((writer->options.flags & FSIMG_WRITE_DURABLE) != 0)
		info->required_io_flags |= FSIMG_IO_DURABLE;
	planner->plan = plan;
	planner->writer = writer;

	status = fsimg_stream_new(&writer->session, COLLISION_MAX,
	    &planner->collisions, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_new(&writer->session,
		    APFS_WRITE_RECORD_MAX, &planner->unsorted, error);
	if (status == FSIMG_OK)
		status = run_passes(planner, error);
	fsimg_stream_free(planner->collisions);
	fsimg_stream_free(planner->unsorted);
	fsimg_dealloc(planner);
	if (status != FSIMG_OK) {
		apfs_write_free(&plan->common);
		return (status);
	}
	writer->plan = &plan->common;
	return (FSIMG_OK);
}

void
apfs_write_free(fsimg_plan_t *base)
{
	apfs_plan_t *plan = (apfs_plan_t *)base;

	if (plan == NULL)
		return;
	fsimg_stream_free(plan->records);
	fsimg_array_free(plan->rows);
	fsimg_dealloc(plan);
}
