/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>
#include <time.h>

#include "fsimg-endian.h"
#include "unicode.h"
#include "write-hfsplus.h"

/* Planning runs a fixed sequence of passes over the frozen catalog:
 *
 *   entries   names encoded and refused, metadata selected into rows, fork
 *             blocks summed per candidate block size, and two leaf records
 *             per entry appended to an unsorted stream;
 *   sort      the stream ordered by (parent, namespace key), equal keys being
 *             siblings macOS would treat as one name;
 *   packing   node counts, from the records alone, so they do not depend on
 *             the block size;
 *   geometry  the block size and N;
 *   layout    the first block of every fork;
 *   summary   the plan information.
 *
 * Every value derives from the catalog, the options and the pinned Unicode
 * tables, and catalog IDs are themselves a sorted order, so the result does not
 * depend on source order, budgets, sort fan-in, scratch placement or time zone.
 */

/* The most blocks a volume or fork holds. */
#define BLOCKS_MAX		UINT64_C(0xffffffff)
/* The primary header's end and the alternate header's distance from the end. */
#define HEAD_BYTES		1536u
#define TAIL_BYTES		1024u
/* Stored times span 1904 to 2040; an epoch is Unix seconds within them. */
#define EPOCH_MIN		(-HFSPLUS_TIME_ORIGIN)
#define EPOCH_MAX		(INT64_C(0xffffffff) - HFSPLUS_TIME_ORIGIN)
#define DEFAULT_LABEL		"untitled"

/* The BSD file types the mode field stores. */
#define MODE_FIFO		0010000u
#define MODE_DIRECTORY		0040000u
#define MODE_FILE		0100000u
#define MODE_SYMLINK		0120000u

/* A name as the volume stores it, and the namespace key that identifies it.
 * Both are big-endian units; stored_units is the 1 to 255 of the on-disk name.
 */
typedef struct hfsplus_name {
	unsigned char	 stored[2 * HFSPLUS_NAME_UNITS_MAX];
	unsigned char	 key[HFSPLUS_KEY_BYTES_MAX];
	size_t		 key_bytes;
	size_t		 stored_units;
} hfsplus_name_t;

/* A block size the sizing search may take, with the data fork blocks it costs.
 * oversized says some file needs more blocks than a fork can hold at this size.
 */
typedef struct hfsplus_candidate {
	uint64_t	 data_blocks;
	uint32_t	 block_bytes;
	bool		 oversized;
} hfsplus_candidate_t;

/* The search list, in the order that settles a tie: the default, then the
 * others in increasing size.
 */
static const uint32_t candidate_sizes[] = {
	4096, 512, 1024, 2048, 8192, 16384, 32768, 65536
};
#define CANDIDATES_MAX	(sizeof(candidate_sizes) / sizeof(candidate_sizes[0]))

typedef struct hfsplus_candidates {
	hfsplus_candidate_t	 list[CANDIDATES_MAX];
	size_t			 count;
} hfsplus_candidates_t;

/* One candidate's volume: N blocks of block_bytes in volume_bytes. */
typedef struct hfsplus_geometry_choice {
	uint64_t	 volume_bytes;
	uint32_t	 block_bytes;
	uint32_t	 blocks;
	uint32_t	 head_blocks;
	uint32_t	 tail_blocks;
	uint32_t	 bitmap_blocks;
} hfsplus_geometry_choice_t;

/* The 23 adjacent units `fsck_hfs -n` calls an illegal name in the final sorted
 * stored name. A repair routine for names written under the pre-10.2
 * decomposition rules rewrites them, and a volume holding one fails its check,
 * so the writer refuses them although the format defines nothing against them.
 */
static const uint16_t illegal_sequences[][2] = {
	{ 0x0306, 0x0307 }, { 0x0308, 0x030d }, { 0x00a8, 0x030d },
	{ 0x0391, 0x030d }, { 0x0395, 0x030d }, { 0x0397, 0x030d },
	{ 0x0399, 0x030d }, { 0x039f, 0x030d }, { 0x03a5, 0x030d },
	{ 0x03a9, 0x030d }, { 0x03b1, 0x030d }, { 0x03b5, 0x030d },
	{ 0x03b7, 0x030d }, { 0x03b9, 0x030d }, { 0x03bf, 0x030d },
	{ 0x03c5, 0x030d }, { 0x03c9, 0x030d }, { 0x03d2, 0x030d },
	{ 0x09ac, 0x09bc }, { 0x0a21, 0x0a3c }, { 0x0b2f, 0x0b3c },
	{ 0x0e4d, 0x0e32 }, { 0x0ecd, 0x0eb2 }
};
_Static_assert(sizeof(illegal_sequences) / sizeof(illegal_sequences[0]) == 23,
    "the 23 sequences the native checker flags");

static hfsplus_compare_t
variant_compare(unsigned variant)
{
	return (variant == HFSPLUS_VARIANT_X ? HFSPLUS_COMPARE_BINARY :
	    HFSPLUS_COMPARE_FOLDED);
}

static bool
holds_illegal_sequence(const uint16_t *units, size_t count)
{
	for (size_t i = 0; i + 1 < count; i++) {
		for (size_t j = 0; j < sizeof(illegal_sequences) /
		    sizeof(illegal_sequences[0]); j++) {
			if (units[i] == illegal_sequences[j][0] &&
			    units[i + 1] == illegal_sequences[j][1])
				return (true);
		}
	}
	return (false);
}

/* Encode an image path component or label into its stored name and namespace
 * key. Returns NULL on success, otherwise why the name is refused.
 *
 * The final check holds the writer to the reader's own admission rule, so a
 * name that could not read back as the same component, such as a dot name,
 * never reaches a volume.
 */
static const char *
name_encode(hfsplus_compare_t compare, const char *component, size_t length,
    hfsplus_name_t *name)
{
	uint16_t units[HFSPLUS_NAME_UNITS_MAX];
	size_t count;

	if (!hfsplus_path_to_units(component, length, units, &count))
		return ("name is not valid UTF-8 within 255 HFS Plus units");
	if (count == 0)
		return ("name is empty");
	for (size_t i = 0; i < count; i++) {
		if (units[i] == 0xfffe || units[i] == 0xffff)
			return ("name holds a noncharacter macOS stores as "
			    "percent text");
		fsimg_store_be16(name->stored + 2 * i, units[i]);
	}
	if (holds_illegal_sequence(units, count))
		return ("name holds a sequence fsck_hfs calls illegal");
	name->stored_units = count;
	if (hfsplus_stored_key(compare, name->stored, count, name->key,
	    &name->key_bytes, NULL) != FSIMG_OK ||
	    hfsplus_name_admit(name->stored, count, NULL) != FSIMG_OK)
		return ("name does not read back from HFS Plus");
	if (name->key_bytes == 0)
		return ("name holds only units HFS Plus ignores");
	return (NULL);
}

/* The label's component: a NULL or empty label selects the native default. */
static const char *
label_component(const fsimg_write_options_t *options)
{
	if (options->label == NULL || options->label[0] == '\0')
		return (DEFAULT_LABEL);
	return (options->label);
}

fsimg_status_t
hfsplus_write_validate(fsimg_write_options_t *options, unsigned variant,
    fsimg_error_t *error)
{
	hfsplus_name_t label;
	const char *component, *refused;
	uint32_t block;

	block = options->allocation_unit_bytes == 0 ?
	    HFSPLUS_WRITE_BLOCK_DEFAULT : options->allocation_unit_bytes;
	if (block < HFSPLUS_WRITE_BLOCK_MIN || block > HFSPLUS_WRITE_BLOCK_MAX ||
	    (block & (block - 1)) != 0 ||
	    (options->sector_bytes != 0 && options->sector_bytes != 512) ||
	    options->volume_bytes > INT64_MAX ||
	    options->volume_bytes % 512 != 0 ||
	    options->volume_bytes / block > BLOCKS_MAX ||
	    ((options->flags & FSIMG_WRITE_REPRODUCIBLE) != 0 &&
	    (options->epoch < EPOCH_MIN || options->epoch > EPOCH_MAX)))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER,
		    "invalid HFS Plus geometry or reproducible epoch"));
	component = label_component(options);
	refused = name_encode(variant_compare(variant), component,
	    strlen(component), &label);
	if (refused != NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER,
		    "HFS Plus label is outside the name rules"));
	options->sector_bytes = 512;
	/* Minimization searches an open block size, so it stays zero. */
	if (options->allocation_unit_bytes != 0 ||
	    (options->flags & FSIMG_WRITE_MINIMIZE) == 0)
		options->allocation_unit_bytes = block;
	return (FSIMG_OK);
}

fsimg_status_t
hfsplus_row_get(hfsplus_plan_t *plan, uint64_t id, hfsplus_row_t *row,
    fsimg_error_t *error)
{
	unsigned char raw[HFSPLUS_ROW_BYTES];
	fsimg_status_t status;

	status = fsimg_array_get(plan->rows, id, raw, error);
	if (status != FSIMG_OK)
		return (status);
	row->length = fsimg_load_le64(raw);
	row->first_block = fsimg_load_le32(raw + 8);
	row->blocks = fsimg_load_le32(raw + 12);
	row->parent = fsimg_load_le32(raw + 16);
	row->uid = fsimg_load_le32(raw + 20);
	row->gid = fsimg_load_le32(raw + 24);
	row->created = fsimg_load_le32(raw + 28);
	row->modified = fsimg_load_le32(raw + 32);
	row->accessed = fsimg_load_le32(raw + 36);
	row->changed = fsimg_load_le32(raw + 40);
	row->valence = fsimg_load_le32(raw + 44);
	row->folders = fsimg_load_le32(raw + 48);
	row->mode = fsimg_load_le16(raw + 52);
	row->kind = raw[54];
	return (FSIMG_OK);
}

fsimg_status_t
hfsplus_row_set(hfsplus_plan_t *plan, uint64_t id, const hfsplus_row_t *row,
    fsimg_error_t *error)
{
	unsigned char raw[HFSPLUS_ROW_BYTES];

	bzero(raw, sizeof(raw));
	fsimg_store_le64(raw, row->length);
	fsimg_store_le32(raw + 8, row->first_block);
	fsimg_store_le32(raw + 12, row->blocks);
	fsimg_store_le32(raw + 16, row->parent);
	fsimg_store_le32(raw + 20, row->uid);
	fsimg_store_le32(raw + 24, row->gid);
	fsimg_store_le32(raw + 28, row->created);
	fsimg_store_le32(raw + 32, row->modified);
	fsimg_store_le32(raw + 36, row->accessed);
	fsimg_store_le32(raw + 40, row->changed);
	fsimg_store_le32(raw + 44, row->valence);
	fsimg_store_le32(raw + 48, row->folders);
	fsimg_store_le16(raw + 52, row->mode);
	raw[54] = row->kind;
	return (fsimg_array_set(plan->rows, id, raw, error));
}

/* Store one timestamp field through the shared selector: an absent field, or
 * one outside 1904 to 2040, takes the format time, which reproducible mode
 * makes the epoch for every field. A present field the stored value does not
 * preserve is a loss.
 */
static fsimg_status_t
encode_time(hfsplus_plan_t *plan, const fsimg_entry_t *entry, uint32_t field,
    const fsimg_timestamp_t *time, uint32_t *out, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	bool exact, present, reproducible;

	present = (entry->metadata.valid & field) != 0;
	reproducible = (writer->options.flags & FSIMG_WRITE_REPRODUCIBLE) != 0;
	exact = fsimg_time_select_u32(present, time, HFSPLUS_TIME_ORIGIN,
	    UINT32_MAX, plan->format_time, reproducible, out);
	if (exact)
		return (FSIMG_OK);
	return (fsimg_writer_loss(writer, entry, field,
	    "HFS Plus cannot preserve the requested timestamp", error));
}

/* Select an entry's owner, mode and times into its row. */
static fsimg_status_t
encode_metadata(hfsplus_plan_t *plan, const fsimg_entry_t *entry,
    hfsplus_row_t *row, fsimg_error_t *error)
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
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "HFS Plus planning received an unknown entry kind"));
	}
	if ((m->valid & FSIMG_META_MODE) != 0)
		permissions = m->mode & FSIMG_MODE_BITS;
	row->mode = (uint16_t)(type | permissions);
	row->kind = (uint8_t)entry->kind;
	if ((m->valid & FSIMG_META_OWNER) != 0) {
		row->uid = m->uid;
		row->gid = m->gid;
	}
	status = encode_time(plan, entry, FSIMG_META_CREATED, &m->created,
	    &row->created, error);
	if (status == FSIMG_OK)
		status = encode_time(plan, entry, FSIMG_META_MODIFIED,
		    &m->modified, &row->modified, error);
	if (status == FSIMG_OK)
		status = encode_time(plan, entry, FSIMG_META_CHANGED,
		    &m->changed, &row->changed, error);
	if (status == FSIMG_OK)
		status = encode_time(plan, entry, FSIMG_META_ACCESSED,
		    &m->accessed, &row->accessed, error);
	return (status);
}

/* Append an entry's two leaf records: the primary, keyed by its parent and
 * name, and the thread, keyed by its own node ID and the empty name.
 */
static fsimg_status_t
append_records(fsimg_stream_t *stream, uint64_t id, uint32_t parent,
    bool folder, const hfsplus_name_t *name, fsimg_error_t *error)
{
	unsigned char raw[HFSPLUS_WRITE_RECORD_MAX];
	hfsplus_write_record_t record;
	size_t length;
	fsimg_status_t status;

	record.id = (uint32_t)id;
	record.units = (uint16_t)name->stored_units;
	record.name = name->stored;

	record.parent = parent;
	record.type = folder ? HFSPLUS_RECORD_FOLDER : HFSPLUS_RECORD_FILE;
	record.key = name->key;
	record.key_bytes = (uint16_t)name->key_bytes;
	length = hfsplus_write_record_encode(&record, raw);
	status = fsimg_stream_append(stream, raw, length, NULL, error);
	if (status != FSIMG_OK)
		return (status);

	record.parent = hfsplus_node_id(id);
	record.type = folder ? HFSPLUS_RECORD_FOLDER_THREAD :
	    HFSPLUS_RECORD_FILE_THREAD;
	record.key = NULL;
	record.key_bytes = 0;
	length = hfsplus_write_record_encode(&record, raw);
	return (fsimg_stream_append(stream, raw, length, NULL, error));
}

/* Refuse a root entry whose key equals that of a format-owned root name, so a
 * later native journal or link cannot collide with it.
 */
static bool
is_owned_root_name(hfsplus_compare_t compare, const hfsplus_name_t *name)
{
	unsigned char key[HFSPLUS_KEY_BYTES_MAX];
	size_t key_bytes;

	for (size_t i = 0; i < HFSPLUS_OWNED_COUNT; i++) {
		if (hfsplus_stored_key(compare, hfsplus_owned_names[i].units,
		    hfsplus_owned_names[i].count, key, &key_bytes,
		    NULL) != FSIMG_OK)
			continue;
		if (key_bytes == name->key_bytes &&
		    memcmp(key, name->key, key_bytes) == 0)
			return (true);
	}
	return (false);
}

/* Add a file's or link's data fork to every candidate. It fails only when no
 * candidate is left: a size that fits none of the block sizes on offer.
 */
static fsimg_status_t
add_fork(hfsplus_candidates_t *candidates, const fsimg_entry_t *entry,
    fsimg_error_t *error)
{
	uint64_t blocks;
	size_t usable;

	usable = 0;
	for (size_t i = 0; i < candidates->count; i++) {
		hfsplus_candidate_t *candidate = &candidates->list[i];

		blocks = entry->size / candidate->block_bytes +
		    (entry->size % candidate->block_bytes != 0);
		if (blocks > BLOCKS_MAX)
			candidate->oversized = true;
		else
			candidate->data_blocks += blocks;
		if (!candidate->oversized)
			usable++;
	}
	if (usable != 0)
		return (FSIMG_OK);
	fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0, FSIMG_OP_WRITER,
	    "file needs more blocks than an HFS Plus fork holds");
	fsimg_error_path(error, entry->path);
	return (FSIMG_EUNSUPPORTED);
}

/* Fail on a name that is refused, naming the entry. */
static fsimg_status_t
refuse_name(const char *reason, const char *path, fsimg_error_t *error)
{
	fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER, reason);
	fsimg_error_path(error, path);
	return (FSIMG_EINVAL);
}

/* Pass 1 for one entry. A catalog entry's parent has a lower ID, so the
 * parent's row already exists and the entry can count itself into it.
 */
static fsimg_status_t
plan_entry(hfsplus_plan_t *plan, const fsimg_catalog_entry_t *catalog,
    fsimg_stream_t *unsorted, hfsplus_candidates_t *candidates,
    fsimg_error_t *error)
{
	const fsimg_entry_t *entry = &catalog->entry;
	hfsplus_name_t name;
	hfsplus_row_t row, parent;
	const char *component, *refused;
	fsimg_status_t status;
	bool folder;

	status = fsimg_session_charge(&plan->common.writer->session, 0, 0, 1,
	    error);
	if (status != FSIMG_OK)
		return (status);
	component = strrchr(entry->path, '/') + 1;
	refused = name_encode(plan->compare, component, strlen(component), &name);
	if (refused != NULL)
		return (refuse_name(refused, entry->path, error));
	if (catalog->parent_id == 0 && is_owned_root_name(plan->compare, &name))
		return (refuse_name("name is reserved in the HFS Plus root",
		    entry->path, error));

	bzero(&row, sizeof(row));
	row.parent = hfsplus_node_id(catalog->parent_id);
	status = encode_metadata(plan, entry, &row, error);
	if (status != FSIMG_OK)
		return (status);
	folder = entry->kind == FSIMG_ENTRY_DIRECTORY;
	if (folder) {
		plan->folder_count++;
	} else {
		plan->file_count++;
		if (entry->kind != FSIMG_ENTRY_FIFO) {
			row.length = entry->size;
			plan->common.info.initialized_bytes += entry->size;
			status = add_fork(candidates, entry, error);
			if (status != FSIMG_OK)
				return (status);
		}
	}
	status = hfsplus_row_set(plan, catalog->id, &row, error);
	if (status != FSIMG_OK)
		return (status);
	status = hfsplus_row_get(plan, catalog->parent_id, &parent, error);
	if (status != FSIMG_OK)
		return (status);
	parent.valence++;
	if (folder)
		parent.folders++;
	status = hfsplus_row_set(plan, catalog->parent_id, &parent, error);
	if (status != FSIMG_OK)
		return (status);
	return (append_records(unsorted, catalog->id, row.parent, folder, &name,
	    error));
}

/* The root's row and records: its name is the label, its parent the root's
 * parent, its metadata the defaults at the format time.
 */
static fsimg_status_t
plan_root(hfsplus_plan_t *plan, fsimg_stream_t *unsorted, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	hfsplus_name_t label;
	hfsplus_row_t root;
	const char *component, *refused;
	fsimg_status_t status;

	component = label_component(&writer->options);
	refused = name_encode(plan->compare, component, strlen(component), &label);
	if (refused != NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER,
		    "HFS Plus label is outside the name rules"));
	bzero(&root, sizeof(root));
	root.parent = HFSPLUS_ID_ROOT_PARENT;
	root.mode = MODE_DIRECTORY | 0755;
	root.kind = FSIMG_ENTRY_DIRECTORY;
	root.created = plan->format_time;
	root.modified = plan->format_time;
	root.accessed = plan->format_time;
	root.changed = plan->format_time;
	status = hfsplus_row_set(plan, 0, &root, error);
	if (status != FSIMG_OK)
		return (status);
	return (append_records(unsorted, 0, HFSPLUS_ID_ROOT_PARENT, true, &label,
	    error));
}

static void
open_candidates(const fsimg_write_options_t *options,
    hfsplus_candidates_t *candidates)
{
	bzero(candidates, sizeof(*candidates));
	if (options->allocation_unit_bytes != 0) {
		candidates->list[0].block_bytes = options->allocation_unit_bytes;
		candidates->count = 1;
		return;
	}
	for (size_t i = 0; i < CANDIDATES_MAX; i++)
		candidates->list[i].block_bytes = candidate_sizes[i];
	candidates->count = CANDIDATES_MAX;
}

/* Pass 1: rows and unsorted records, in catalog ID order. */
static fsimg_status_t
plan_entries(hfsplus_plan_t *plan, fsimg_stream_t *unsorted,
    hfsplus_candidates_t *candidates, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	const fsimg_catalog_entry_t *catalog;
	fsimg_status_t status;

	status = plan_root(plan, unsorted, error);
	if (status != FSIMG_OK)
		return (status);
	for (uint64_t id = 1; id <= writer->count; id++) {
		status = fsimg_catalog_get(writer, id, &catalog, error);
		if (status != FSIMG_OK)
			return (status);
		status = plan_entry(plan, catalog, unsorted, candidates, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/* Whether two sorted neighbours are the same name: the same parent and key.
 * Threads never are, since a thread's key is its own node ID.
 */
static bool
records_collide(const hfsplus_write_record_t *a,
    const hfsplus_write_record_t *b)
{
	return (a->parent == b->parent && a->key_bytes == b->key_bytes &&
	    memcmp(a->key, b->key, a->key_bytes) == 0);
}

/* Pass 2's check: adjacent equal keys are siblings macOS would treat as one
 * name. The sort ties them by catalog ID, so the second named is deterministic.
 */
static fsimg_status_t
check_collisions(hfsplus_plan_t *plan, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	unsigned char raw[2][HFSPLUS_WRITE_RECORD_MAX];
	hfsplus_write_record_t previous, record;
	const fsimg_catalog_entry_t *catalog;
	uint64_t cursor = 0;
	size_t current = 0;
	size_t length;
	fsimg_status_t status;
	bool have_previous = false;

	/* The two buffers alternate, so previous keeps pointing at live bytes
	 * while the next record is read into the other.
	 */
	for (;;) {
		status = fsimg_stream_next(plan->records, &cursor, raw[current],
		    sizeof(raw[current]), &length, error);
		if (status == FSIMG_DONE)
			return (FSIMG_OK);
		if (status == FSIMG_OK)
			status = fsimg_session_charge(&writer->session, 0, 0, 1,
			    error);
		if (status != FSIMG_OK)
			return (status);
		if (!hfsplus_write_record_decode(raw[current], length, &record))
			return (fsimg_error_set(error, FSIMG_ESTATE, 0,
			    FSIMG_OP_WRITER,
			    "HFS Plus sorted record is inconsistent"));
		if (have_previous && records_collide(&previous, &record)) {
			status = fsimg_catalog_get(writer, record.id, &catalog,
			    error);
			if (status != FSIMG_OK)
				return (status);
			return (refuse_name(
			    "name is equivalent to a sibling under HFS Plus",
			    catalog->entry.path, error));
		}
		previous = record;
		have_previous = true;
		current = 1 - current;
	}
}

/* Pass 2: sort, check and keep the records. The sort's result has no ties to
 * break but colliding names, which the check refuses, so it does not depend on
 * the run sizes or fan-in the sort chooses.
 */
static fsimg_status_t
sort_records(hfsplus_plan_t *plan, fsimg_stream_t *unsorted,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	status = fsimg_stream_seal(unsorted, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_sort(unsorted,
		    hfsplus_write_record_compare, NULL, NULL, &plan->records,
		    error);
	if (status != FSIMG_OK)
		return (status);
	return (check_collisions(plan, error));
}

/* The bitmap's blocks for a volume of N blocks. */
static uint64_t
bitmap_blocks(uint64_t blocks, uint32_t block_bytes)
{
	uint64_t bytes;

	bytes = blocks / 8 + (blocks % 8 != 0);
	return (bytes / block_bytes + (bytes % block_bytes != 0));
}

/* The blocks everything but the bitmap and the reserved blocks needs at this
 * candidate: the three trees, the data forks and the requested free space.
 * False when the sum passes the block count a volume can have.
 */
static bool
candidate_demand(const hfsplus_plan_t *plan,
    const hfsplus_candidate_t *candidate, uint64_t *demand)
{
	const fsimg_write_options_t *options = &plan->common.writer->options;
	uint32_t block = candidate->block_bytes;
	uint64_t free_blocks;

	free_blocks = options->min_free_bytes / block +
	    (options->min_free_bytes % block != 0);
	*demand = candidate->data_blocks;
	return (fsimg_add_u64(*demand, free_blocks, demand) &&
	    fsimg_add_u64(*demand, hfsplus_tree_blocks(&plan->extents, block),
	    demand) &&
	    fsimg_add_u64(*demand, hfsplus_tree_blocks(&plan->attributes, block),
	    demand) &&
	    fsimg_add_u64(*demand, hfsplus_tree_blocks(&plan->catalog, block),
	    demand) &&
	    *demand <= BLOCKS_MAX);
}

/* Geometry at an exact volume size: N is what the slice holds. The tail is the
 * blocks below N that overlap the last 1024 bytes: on a slice longer than N
 * blocks by 1024 bytes or more, the alternate header lies past allocation and
 * no block is reserved for it.
 */
static bool
geometry_exact(uint64_t volume_bytes, uint32_t block, uint64_t demand,
    hfsplus_geometry_choice_t *choice)
{
	uint64_t blocks, bitmap, head, needed, tail, tail_start;

	if (volume_bytes < HEAD_BYTES + TAIL_BYTES)
		return (false);
	blocks = volume_bytes / block;
	head = (HEAD_BYTES + block - 1) / block;
	tail_start = (volume_bytes - TAIL_BYTES) / block;
	tail = blocks > tail_start ? blocks - tail_start : 0;
	bitmap = bitmap_blocks(blocks, block);
	needed = head + tail + bitmap + demand;
	if (needed > blocks)
		return (false);
	choice->volume_bytes = volume_bytes;
	choice->block_bytes = block;
	choice->blocks = (uint32_t)blocks;
	choice->head_blocks = (uint32_t)head;
	choice->tail_blocks = (uint32_t)tail;
	choice->bitmap_blocks = (uint32_t)bitmap;
	return (true);
}

/* Geometry by automatic sizing: the smallest N with
 *
 *   head + tail + demand + bitmap(N) <= N.
 *
 * bitmap() is monotone, so iterating N = base + bitmap(N) from below the answer
 * rises to the least fixed point, and a block of bitmap covers 8 * block_bytes
 * blocks, so it stops within a few steps. The bitmap can force N past the
 * content's own need: the bitmap block that the block after a boundary needs
 * also adds a block, which is then free.
 */
static bool
geometry_automatic(uint32_t block, uint64_t demand,
    hfsplus_geometry_choice_t *choice)
{
	uint64_t base, bitmap, blocks, head, tail;

	head = (HEAD_BYTES + block - 1) / block;
	tail = (TAIL_BYTES + block - 1) / block;
	base = head + tail + demand;
	blocks = base + 1;
	for (;;) {
		bitmap = bitmap_blocks(blocks, block);
		if (base + bitmap <= blocks)
			break;
		blocks = base + bitmap;
	}
	if (blocks > BLOCKS_MAX)
		return (false);
	choice->volume_bytes = blocks * block;
	choice->block_bytes = block;
	choice->blocks = (uint32_t)blocks;
	choice->head_blocks = (uint32_t)head;
	choice->tail_blocks = (uint32_t)tail;
	choice->bitmap_blocks = (uint32_t)bitmap;
	return (true);
}

/* One candidate's geometry, or the failure that rules it out. */
static fsimg_status_t
size_candidate(const hfsplus_plan_t *plan,
    const hfsplus_candidate_t *candidate, hfsplus_geometry_choice_t *choice,
    fsimg_error_t *error)
{
	const fsimg_write_options_t *options = &plan->common.writer->options;
	uint64_t demand;
	bool fits;

	if (candidate->oversized)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_WRITER,
		    "file needs more blocks than an HFS Plus fork holds"));
	fits = candidate_demand(plan, candidate, &demand);
	if (fits && options->volume_bytes != 0)
		fits = geometry_exact(options->volume_bytes,
		    candidate->block_bytes, demand, choice);
	else if (fits)
		fits = geometry_automatic(candidate->block_bytes, demand, choice);
	if (!fits)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER,
		    "HFS Plus content does not fit the volume"));
	return (FSIMG_OK);
}

/* Pass 4: the one candidate's geometry, or under minimization the candidate of
 * the smallest volume, ties going to the search list's order. Failure reports
 * the first candidate's reason, the default block size's.
 */
static fsimg_status_t
choose_geometry(const hfsplus_plan_t *plan,
    const hfsplus_candidates_t *candidates, hfsplus_geometry_choice_t *best,
    fsimg_error_t *error)
{
	fsimg_session_t *session = &plan->common.writer->session;
	hfsplus_geometry_choice_t choice;
	fsimg_error_t detail, first_failure;
	fsimg_status_t status;
	bool failed, found;

	failed = false;
	found = false;
	bzero(&first_failure, sizeof(first_failure));
	for (size_t i = 0; i < candidates->count; i++) {
		status = fsimg_session_charge(session, 0, 0, 1, error);
		if (status != FSIMG_OK)
			return (status);
		status = size_candidate(plan, &candidates->list[i], &choice,
		    &detail);
		if (status != FSIMG_OK) {
			if (status != FSIMG_ERANGE &&
			    status != FSIMG_EUNSUPPORTED) {
				if (error != NULL)
					*error = detail;
				return (status);
			}
			if (!failed)
				first_failure = detail;
			failed = true;
			continue;
		}
		if (!found || choice.volume_bytes < best->volume_bytes)
			*best = choice;
		found = true;
	}
	if (found)
		return (FSIMG_OK);
	if (error != NULL)
		*error = first_failure;
	return (first_failure.status);
}

/* Pass 5: first blocks in the fixed order, the bitmap at the end of the reserved
 * head, the three trees, and then each file's and link's fork in catalog ID
 * order. The geometry's inequality has already proved that this ends in time.
 */
static fsimg_status_t
layout(hfsplus_plan_t *plan, uint64_t *payload_blocks, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	uint32_t block = plan->block_bytes;
	hfsplus_row_t row;
	uint64_t next;
	fsimg_status_t status;

	next = (uint64_t)plan->head_blocks + plan->bitmap_blocks;
	hfsplus_tree_place(&plan->extents, (uint32_t)next, block);
	next += plan->extents.blocks;
	hfsplus_tree_place(&plan->attributes, (uint32_t)next, block);
	next += plan->attributes.blocks;
	hfsplus_tree_place(&plan->catalog, (uint32_t)next, block);
	next += plan->catalog.blocks;

	*payload_blocks = 0;
	for (uint64_t id = 1; id <= writer->count; id++) {
		status = fsimg_session_charge(&writer->session, 0, 0, 1, error);
		if (status == FSIMG_OK)
			status = hfsplus_row_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		if (row.length == 0)
			continue;
		row.blocks = (uint32_t)(row.length / block +
		    (row.length % block != 0));
		row.first_block = (uint32_t)next;
		next += row.blocks;
		*payload_blocks += row.blocks;
		status = hfsplus_row_set(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (next + plan->tail_blocks > plan->total_blocks)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "HFS Plus layout disagrees with its geometry"));
	plan->allocated = (uint32_t)next;
	return (FSIMG_OK);
}

/* Pass 6: everything that is not file or link content is metadata, the slice's
 * bytes past the last block among them.
 */
static void
fill_summary(hfsplus_plan_t *plan, uint64_t payload_blocks)
{
	fsimg_plan_info_t *info = &plan->common.info;
	uint64_t block = plan->block_bytes;
	uint64_t in_use, free_blocks;

	in_use = (uint64_t)plan->allocated + plan->tail_blocks;
	free_blocks = plan->total_blocks - in_use;
	info->volume_bytes = plan->volume_bytes;
	info->sector_bytes = 512;
	info->allocation_unit_bytes = plan->block_bytes;
	info->allocation_units = plan->total_blocks;
	info->allocated_bytes = in_use * block;
	info->free_bytes = free_blocks * block;
	info->metadata_bytes = plan->volume_bytes - free_blocks * block -
	    payload_blocks * block;
	info->scratch_bytes = plan->common.writer->ctx->stats.scratch_bytes;
}

/* The format time, the one host clock read: the epoch when reproducible,
 * otherwise the UTC clock at planning, which must be a date the volume stores.
 */
static fsimg_status_t
select_format_time(hfsplus_plan_t *plan, fsimg_error_t *error)
{
	const fsimg_write_options_t *options = &plan->common.writer->options;
	int64_t seconds;

	if ((options->flags & FSIMG_WRITE_REPRODUCIBLE) != 0)
		seconds = options->epoch;
	else
		seconds = (int64_t)time(NULL);
	if (seconds < EPOCH_MIN || seconds > EPOCH_MAX)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER,
		    "format time is outside the HFS Plus date range"));
	plan->format_time = (uint32_t)(seconds + HFSPLUS_TIME_ORIGIN);
	return (FSIMG_OK);
}

/* The passes, with the plan already allocated. */
static fsimg_status_t
run_passes(hfsplus_plan_t *plan, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	hfsplus_candidates_t candidates;
	hfsplus_geometry_choice_t geometry;
	fsimg_stream_t *unsorted;
	uint64_t payload_blocks;
	fsimg_status_t status;

	status = select_format_time(plan, error);
	if (status != FSIMG_OK)
		return (status);
	/* Every entry has two leaf records, and the header counts them in 32
	 * bits. Node and catalog IDs fit within that too.
	 */
	if (writer->count + 1 > UINT32_MAX / 2)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER,
		    "entries exceed the HFS Plus catalog record count"));
	status = fsimg_array_new(&writer->session, HFSPLUS_ROW_BYTES,
	    writer->count + 1, &plan->rows, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_stream_new(&writer->session, HFSPLUS_WRITE_RECORD_MAX,
	    &unsorted, error);
	if (status != FSIMG_OK)
		return (status);
	open_candidates(&writer->options, &candidates);
	status = plan_entries(plan, unsorted, &candidates, error);
	if (status == FSIMG_OK)
		status = sort_records(plan, unsorted, error);
	fsimg_stream_free(unsorted);
	if (status != FSIMG_OK)
		return (status);

	status = hfsplus_tree_pack(&writer->session, plan->records,
	    &plan->catalog, plan->levels, error);
	if (status != FSIMG_OK)
		return (status);
	status = choose_geometry(plan, &candidates, &geometry, error);
	if (status != FSIMG_OK)
		return (status);
	plan->volume_bytes = geometry.volume_bytes;
	plan->block_bytes = geometry.block_bytes;
	plan->total_blocks = geometry.blocks;
	plan->head_blocks = geometry.head_blocks;
	plan->tail_blocks = geometry.tail_blocks;
	plan->bitmap_blocks = geometry.bitmap_blocks;
	status = layout(plan, &payload_blocks, error);
	if (status != FSIMG_OK)
		return (status);
	plan->next_catalog_id = (uint32_t)writer->count + HFSPLUS_ID_FIRST_USER;
	fill_summary(plan, payload_blocks);
	return (FSIMG_OK);
}

fsimg_status_t
hfsplus_write_plan(fsimg_writer_t *writer, fsimg_error_t *error)
{
	hfsplus_plan_t *plan;
	fsimg_plan_info_t *info;
	fsimg_error_t detail;
	fsimg_status_t status;

	plan = fsimg_alloc(writer->ctx, sizeof(*plan), &detail);
	if (plan == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	bzero(plan, sizeof(*plan));
	plan->common.writer = writer;
	plan->variant = writer->variant;
	plan->compare = variant_compare(writer->variant);
	hfsplus_tree_shape_init(&plan->extents, HFSPLUS_TREE_EXTENTS, plan->compare);
	hfsplus_tree_shape_init(&plan->catalog, HFSPLUS_TREE_CATALOG, plan->compare);
	hfsplus_tree_shape_init(&plan->attributes, HFSPLUS_TREE_ATTRIBUTES,
	    plan->compare);
	info = &plan->common.info;
	strcpy(info->format,
	    writer->variant == HFSPLUS_VARIANT_X ? "hfsx" : "hfsplus");
	info->entries = writer->count;
	info->payload_bytes = writer->payload_bytes;
	info->required_io_flags = FSIMG_IO_WRITE;
	if ((writer->options.flags & FSIMG_WRITE_DURABLE) != 0)
		info->required_io_flags |= FSIMG_IO_DURABLE;

	status = run_passes(plan, error);
	if (status != FSIMG_OK) {
		hfsplus_write_free(&plan->common);
		return (status);
	}
	writer->plan = &plan->common;
	return (FSIMG_OK);
}

void
hfsplus_write_free(fsimg_plan_t *base)
{
	hfsplus_plan_t *plan = (hfsplus_plan_t *)base;

	if (plan == NULL)
		return;
	for (size_t i = 0; i < HFSPLUS_BTREE_DEPTH_MAX; i++)
		fsimg_stream_free(plan->levels[i]);
	fsimg_stream_free(plan->records);
	fsimg_array_free(plan->rows);
	fsimg_dealloc(plan);
}
