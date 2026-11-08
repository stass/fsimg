/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>
#include <time.h>

#include "fsimg-endian.h"
#include "write-erofs.h"

/*
 * Planning is a fixed sequence of passes over the sealed catalog. Each pass
 * is sequential, and plan rows, one per catalog ID in a scratch-backed
 * array, are the only per-entry state:
 *
 * 1. Settings and time fix the block size, the addressing, the format
 *    time and the UUID.
 * 2. Entries encode each entry's kind, size, owner, mode, time and link
 *    count, and pack each directory's names (write-dir.c) for its size.
 *    Under addressing=48 the superblock's epoch is lowered to the
 *    smallest modification second of any row.
 * 3. Measurement, under compression=lz4, refuses an output limit the least
 *    reachable volume exceeds, then encodes each regular file whose flat
 *    layout stores at least two blocks (write-lz4.c) and gives it the
 *    compressed layout when that stores fewer.
 * 4. Metadata placement chooses each inode's form and layout, which the
 *    finished link counts decide, and assigns its NID: in the primary
 *    metadata area, or under a metadata box in the box's content, with
 *    the box inode in the primary area.
 * 5. Data placement assigns each inode's start block in the same order,
 *    then the box's blocks after the data; under metabox=lz4 it encodes
 *    the box content once to count its clusters, through the box content
 *    generator emission uses (erofs_box_measure() in write-emit.c), so
 *    planning and emission form the box from the same bytes.
 * 6. Sizing checks the requested volume size; passes 4 and 5 have kept
 *    every block within the addressing's limit.
 *
 * Every row's encoded length depends only on the catalog, the settings and
 * a compressed file's measured clusters, so no pass changes an earlier
 * pass's result, and emission fills NIDs and start blocks into lengths
 * planning has already fixed.
 */

/* The wire layout of a plan row, little endian, every byte used. */
#define ROW_SIZE		0
#define ROW_NID			8
#define ROW_START_BLOCK		16
#define ROW_MTIME		24
#define ROW_MTIME_NSEC		32
#define ROW_UID			36
#define ROW_GID			40
#define ROW_LINKS		44
#define ROW_MODE		48
#define ROW_LAYOUT		50
#define ROW_EXTENDED		51
#define ROW_CLUSTERS		52
#define ROW_FIRST_CLUSTER	56

#define NSEC_PER_SEC		UINT32_C(1000000000)

static fsimg_status_t validate_options(fsimg_write_options_t *, unsigned,
    fsimg_error_t *);
static fsimg_status_t setting_refused(fsimg_error_t *, fsimg_status_t,
    const char *, const char *);
static bool row_consistent(const erofs_row_t *, uint32_t);
static fsimg_status_t make_plan(fsimg_writer_t *, fsimg_error_t *);
static void free_plan(fsimg_plan_t *);
static fsimg_status_t choose_format_time(erofs_plan_t *, fsimg_error_t *);
static fsimg_status_t encode_time(erofs_plan_t *, const fsimg_entry_t *,
    erofs_row_t *, fsimg_error_t *);
static fsimg_status_t encode_entry(erofs_plan_t *, const fsimg_entry_t *,
    erofs_row_t *, fsimg_error_t *);
static fsimg_status_t count_subdirectory(erofs_plan_t *, uint64_t,
    fsimg_error_t *);
static fsimg_status_t plan_entry(erofs_plan_t *, uint64_t, uint64_t *,
    fsimg_error_t *);
static fsimg_status_t plan_entries(erofs_plan_t *, fsimg_error_t *);
static uint64_t flat_blocks(const erofs_plan_t *, const erofs_row_t *,
    bool *);
static fsimg_status_t check_least_volume(erofs_plan_t *, fsimg_error_t *);
static fsimg_status_t measure_files(erofs_plan_t *, fsimg_error_t *);
static fsimg_status_t addressing_exceeded(const erofs_plan_t *,
    fsimg_error_t *);
static void choose_inode_shape(const erofs_plan_t *, erofs_row_t *);
static uint64_t place_body(uint64_t *, const erofs_row_t *, uint64_t,
    uint32_t);
static void plan_box_inode(erofs_plan_t *, uint64_t);
static fsimg_status_t place_metadata(erofs_plan_t *, fsimg_error_t *);
static fsimg_status_t place_rows(erofs_plan_t *, bool, uint64_t *,
    fsimg_error_t *);
static fsimg_status_t check_root_nid(erofs_plan_t *, fsimg_error_t *);
static fsimg_status_t place_box_blocks(erofs_plan_t *, fsimg_error_t *);
static fsimg_status_t place_data(erofs_plan_t *, uint64_t *, uint64_t *,
    fsimg_error_t *);
static fsimg_status_t size_volume(erofs_plan_t *, fsimg_error_t *);
static void summarize(erofs_plan_t *, uint64_t);

const fsimg_writer_ops_t fsimg_erofs_writer_ops = {
	.validate = validate_options,
	.plan = make_plan,
	.emit = erofs_write_emit,
	.free = free_plan
};

/*
 * The common options under the writer profile: a 512-byte sector, a block
 * of 512, 1024, 2048 or 4096 bytes, 4096 by default, a volume size of zero
 * or whole sectors whose whole blocks fit the block count the addressing
 * allows, no free-space request, a label that leaves the name field a
 * terminator, and the EROFS settings. Values outside each domain are
 * invalid. The writer emits the 32- and 48-bit layouts, uncompressed or
 * with LZ4 files, each without a metadata box or with a plain or LZ4 one.
 */
static fsimg_status_t
validate_options(fsimg_write_options_t *options, unsigned variant FSIMG_UNUSED,
    fsimg_error_t *error)
{
	static const char empty_label[] = "";
	uint64_t blocks_max;
	uint32_t block;
	bool legal_block;

	block = options->allocation_unit_bytes == 0 ?
	    EROFS_WRITE_BLOCK_DEFAULT : options->allocation_unit_bytes;
	blocks_max = options->erofs_addressing == 48 ?
	    EROFS_WRITE_BLOCKS_MAX_48 : EROFS_WRITE_BLOCKS_MAX;
	legal_block = block >= 1u << EROFS_BLKSZBITS_MIN &&
	    block <= 1u << EROFS_BLKSZBITS_MAX && (block & (block - 1)) == 0;
	if (!legal_block ||
	    (options->sector_bytes != 0 && options->sector_bytes != 512) ||
	    options->volume_bytes % 512 != 0 ||
	    options->volume_bytes / block > blocks_max ||
	    options->min_free_bytes != 0 ||
	    (options->label != NULL &&
	    strlen(options->label) > EROFS_WRITE_LABEL_MAX))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER,
		    "invalid EROFS geometry, free space or label"));

	if (options->erofs_addressing != 0 && options->erofs_addressing != 32 &&
	    options->erofs_addressing != 48)
		return (setting_refused(error, FSIMG_EINVAL, "addressing",
		    "EROFS addressing is 32 or 48 bits"));
	if (options->erofs_compression != FSIMG_EROFS_COMPRESSION_NONE &&
	    options->erofs_compression != FSIMG_EROFS_COMPRESSION_LZ4)
		return (setting_refused(error, FSIMG_EINVAL, "compression",
		    "EROFS compression is none or lz4"));
	if (options->erofs_metabox != FSIMG_EROFS_METABOX_NONE &&
	    options->erofs_metabox != FSIMG_EROFS_METABOX_PLAIN &&
	    options->erofs_metabox != FSIMG_EROFS_METABOX_LZ4)
		return (setting_refused(error, FSIMG_EINVAL, "metabox",
		    "EROFS metadata box is none, plain or lz4"));

	/* An absent label is an empty name field. */
	if (options->label == NULL)
		options->label = empty_label;
	options->sector_bytes = 512;
	options->allocation_unit_bytes = block;
	return (FSIMG_OK);
}

/* Refuse the setting key with status and message, naming the key. */
static fsimg_status_t
setting_refused(fsimg_error_t *error, fsimg_status_t status, const char *key,
    const char *message)
{
	fsimg_error_set(error, status, 0, FSIMG_OP_WRITER, message);
	fsimg_error_path(error, key);
	return (status);
}

/*
 * Rows come back from scratch storage, so a row no planning pass writes is
 * refused rather than handed to emission, which copies an inline tail at
 * the offset the NID gives: see row_consistent(). Under a metadata box no
 * row has an inline tail, so emission never derives a volume offset from
 * a box offset.
 */
fsimg_status_t
erofs_row_get(erofs_plan_t *plan, uint64_t id, erofs_row_t *row,
    fsimg_error_t *error)
{
	unsigned char raw[EROFS_ROW_BYTES];
	fsimg_status_t status;

	status = fsimg_array_get(plan->rows, id, raw, error);
	if (status != FSIMG_OK)
		return (status);
	if ((raw[ROW_LAYOUT] != EROFS_LAYOUT_FLAT &&
	    raw[ROW_LAYOUT] != EROFS_LAYOUT_FLAT_INLINE &&
	    raw[ROW_LAYOUT] != EROFS_LAYOUT_COMPRESSED_FULL) ||
	    raw[ROW_EXTENDED] > 1)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_WRITER, "invalid EROFS plan row"));
	row->size = fsimg_load_le64(raw + ROW_SIZE);
	row->nid = fsimg_load_le64(raw + ROW_NID);
	row->start_block = fsimg_load_le64(raw + ROW_START_BLOCK);
	row->mtime = (int64_t)fsimg_load_le64(raw + ROW_MTIME);
	row->mtime_nsec = fsimg_load_le32(raw + ROW_MTIME_NSEC);
	row->uid = fsimg_load_le32(raw + ROW_UID);
	row->gid = fsimg_load_le32(raw + ROW_GID);
	row->links = fsimg_load_le32(raw + ROW_LINKS);
	row->mode = fsimg_load_le16(raw + ROW_MODE);
	row->layout = (erofs_layout_t)raw[ROW_LAYOUT];
	row->extended = raw[ROW_EXTENDED] != 0;
	row->clusters = fsimg_load_le32(raw + ROW_CLUSTERS);
	row->first_cluster = fsimg_load_le64(raw + ROW_FIRST_CLUSTER);
	/* A compressed row's records lie among the adopted ones. */
	if (!row_consistent(row, plan->block_bytes) ||
	    (plan->metabox != FSIMG_EROFS_METABOX_NONE &&
	    row->layout == EROFS_LAYOUT_FLAT_INLINE) ||
	    row->first_cluster > plan->cluster_count ||
	    row->clusters > plan->cluster_count - row->first_cluster)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_WRITER, "invalid EROFS plan row"));
	return (FSIMG_OK);
}

/*
 * The invariants every stored row keeps: one of the four kinds the writer
 * emits, a FIFO empty, nanoseconds below a second, a flat-inline layout
 * only for a tail that fits one block with the fixed part, clusters only
 * under the compressed layout, which only a nonempty regular file has,
 * between one and its index records, and, once a NID is assigned, the part
 * of the inode that may not cross a block inside the NID's block.
 */
static bool
row_consistent(const erofs_row_t *row, uint32_t block_bytes)
{
	uint64_t tail, within;
	uint16_t type;
	bool compressed;

	type = row->mode & EROFS_MODE_TYPE_MASK;
	if ((type != EROFS_MODE_REGULAR && type != EROFS_MODE_DIRECTORY &&
	    type != EROFS_MODE_SYMLINK && type != EROFS_MODE_FIFO) ||
	    (type == EROFS_MODE_FIFO && row->size != 0) ||
	    row->mtime_nsec >= NSEC_PER_SEC)
		return (false);
	tail = row->size % block_bytes;
	if (row->layout == EROFS_LAYOUT_FLAT_INLINE &&
	    (tail == 0 || erofs_row_fixed_bytes(row) + tail > block_bytes))
		return (false);
	compressed = row->layout == EROFS_LAYOUT_COMPRESSED_FULL;
	if (compressed ? type != EROFS_MODE_REGULAR || row->clusters == 0 ||
	    row->clusters > erofs_row_index_records(row, block_bytes) :
	    row->clusters != 0 || row->first_cluster != 0)
		return (false);
	if (row->nid == 0)
		return (true);
	within = row->nid % (block_bytes / EROFS_NID_SLOT_BYTES) *
	    EROFS_NID_SLOT_BYTES;
	return (within + erofs_row_unbroken_bytes(row, block_bytes) <=
	    block_bytes);
}

fsimg_status_t
erofs_row_set(erofs_plan_t *plan, uint64_t id, const erofs_row_t *row,
    fsimg_error_t *error)
{
	unsigned char raw[EROFS_ROW_BYTES];

	bzero(raw, sizeof(raw));
	fsimg_store_le64(raw + ROW_SIZE, row->size);
	fsimg_store_le64(raw + ROW_NID, row->nid);
	fsimg_store_le64(raw + ROW_START_BLOCK, row->start_block);
	fsimg_store_le64(raw + ROW_MTIME, (uint64_t)row->mtime);
	fsimg_store_le32(raw + ROW_MTIME_NSEC, row->mtime_nsec);
	fsimg_store_le32(raw + ROW_UID, row->uid);
	fsimg_store_le32(raw + ROW_GID, row->gid);
	fsimg_store_le32(raw + ROW_LINKS, row->links);
	fsimg_store_le16(raw + ROW_MODE, row->mode);
	raw[ROW_LAYOUT] = (unsigned char)row->layout;
	raw[ROW_EXTENDED] = row->extended ? 1 : 0;
	fsimg_store_le32(raw + ROW_CLUSTERS, row->clusters);
	fsimg_store_le64(raw + ROW_FIRST_CLUSTER, row->first_cluster);
	return (fsimg_array_set(plan->rows, id, raw, error));
}

/*
 * Whether catalog row id lives in the metadata box: every row under a box
 * at addressing=48, every row but the root's at addressing=32, whose root
 * stays in the primary area, where the 16-bit field can name it
 * (check_root_nid()).
 */
bool
erofs_row_in_box(const erofs_plan_t *plan, uint64_t id)
{
	return (plan->metabox != FSIMG_EROFS_METABOX_NONE &&
	    (plan->addressing48 || id != 0));
}

/*
 * The metadata box inode's body: a file's, except that under addressing=48
 * a compressed box reserves one 32-byte encoded extent per block of
 * content, the most clusters the former can make, so the body's size, and
 * the primary area's end, are known before the box is encoded. The header
 * counts the extents written; the reserved records after them stay zero.
 */
uint64_t
erofs_box_body_bytes(const erofs_plan_t *plan, const erofs_row_t *box)
{
	if (box->layout == EROFS_LAYOUT_COMPRESSED_FULL && plan->addressing48)
		return (erofs_row_fixed_bytes(box) + EROFS_EXTENT32_OFFSET +
		    erofs_row_index_records(box, plan->block_bytes) *
		    EROFS_EXTENT32_BYTES);
	return (erofs_row_body_bytes(plan, box));
}

/* The inode's fixed part: 32 bytes compact, 64 extended. */
uint32_t
erofs_row_fixed_bytes(const erofs_row_t *row)
{
	return (row->extended ? EROFS_INODE_EXTENDED_BYTES :
	    EROFS_INODE_COMPACT_BYTES);
}

/*
 * The inode's body: its fixed part and any inline tail, since the writer
 * stores no attributes, or under the compressed layout its fixed part and
 * map header, which starts where the fixed part ends, and then either the
 * full index, whose records start 16 bytes after the header, or under
 * addressing=48 one 32-byte encoded extent per cluster, starting 32 bytes
 * after it.
 */
uint64_t
erofs_row_body_bytes(const erofs_plan_t *plan, const erofs_row_t *row)
{
	if (row->layout != EROFS_LAYOUT_COMPRESSED_FULL)
		return (erofs_row_fixed_bytes(row) +
		    erofs_row_tail_bytes(row, plan->block_bytes));
	if (plan->addressing48)
		return (erofs_row_fixed_bytes(row) + EROFS_EXTENT32_OFFSET +
		    (uint64_t)row->clusters * EROFS_EXTENT32_BYTES);
	return (erofs_row_fixed_bytes(row) + EROFS_FULL_INDEX_OFFSET +
	    erofs_row_index_records(row, plan->block_bytes) *
	    EROFS_FULL_RECORD_BYTES);
}

/*
 * The leading bytes of the inode that placement keeps inside one block:
 * the whole body, fixed part and inline tail, of an uncompressed inode,
 * and the fixed part of a compressed one, whose index may continue across
 * blocks (specs/design-erofs.md, Layout).
 */
uint32_t
erofs_row_unbroken_bytes(const erofs_row_t *row, uint32_t block_bytes)
{
	if (row->layout == EROFS_LAYOUT_COMPRESSED_FULL)
		return (erofs_row_fixed_bytes(row));
	return (erofs_row_fixed_bytes(row) +
	    erofs_row_tail_bytes(row, block_bytes));
}

/* A compressed inode's index records: one per logical cluster of B bytes. */
uint64_t
erofs_row_index_records(const erofs_row_t *row, uint32_t block_bytes)
{
	return (row->size / block_bytes + (row->size % block_bytes != 0));
}

/* The bytes of content the inode's body holds: a flat-inline tail. */
uint32_t
erofs_row_tail_bytes(const erofs_row_t *row, uint32_t block_bytes)
{
	if (row->layout != EROFS_LAYOUT_FLAT_INLINE)
		return (0);
	return ((uint32_t)(row->size % block_bytes));
}

/*
 * The data blocks the inode stores: every block of its content under the
 * flat layout, a partial last block included, the full blocks only under
 * the flat-inline layout, whose tail is in the body, and one block per
 * cluster under the compressed layout.
 */
uint64_t
erofs_row_data_blocks(const erofs_row_t *row, uint32_t block_bytes)
{
	uint64_t blocks;

	if (row->layout == EROFS_LAYOUT_COMPRESSED_FULL)
		return (row->clusters);
	blocks = row->size / block_bytes;
	if (row->layout == EROFS_LAYOUT_FLAT && row->size % block_bytes != 0)
		blocks++;
	return (blocks);
}

/*
 * Pass 1 for time: the format time is the reproducible epoch, or the
 * host's UTC clock sampled once. It is the time of every inode whose own
 * is absent, and the superblock's epoch, with zero fixed nanoseconds,
 * until pass 2 lowers that under addressing=48.
 */
static fsimg_status_t
choose_format_time(erofs_plan_t *plan, fsimg_error_t *error)
{
	const fsimg_write_options_t *options = &plan->common.writer->options;
	time_t now;

	if ((options->flags & FSIMG_WRITE_REPRODUCIBLE) != 0) {
		plan->format_time = options->epoch;
	} else {
		now = time(NULL);
		if (now == (time_t)-1)
			return (fsimg_error_set(error, FSIMG_EIO, 0,
			    FSIMG_OP_WRITER, "host clock is unavailable"));
		plan->format_time = (int64_t)now;
	}
	plan->epoch = plan->format_time;
	return (FSIMG_OK);
}

/*
 * The stored modification time: the entry's own, to the nanosecond, when
 * present, or the format time when it is absent and in reproducible mode,
 * where every inode carries the epoch. Any signed second count is
 * representable, so the time is preserved unless reproducible mode
 * replaces it or its basis is not UTC, whose offset or wall-clock meaning
 * the format cannot record.
 */
static fsimg_status_t
encode_time(erofs_plan_t *plan, const fsimg_entry_t *entry, erofs_row_t *row,
    fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	const fsimg_timestamp_t *modified = &entry->metadata.modified;
	bool reproducible;

	row->mtime = plan->format_time;
	row->mtime_nsec = 0;
	if ((entry->metadata.valid & FSIMG_META_MODIFIED) == 0)
		return (FSIMG_OK);
	reproducible = (writer->options.flags & FSIMG_WRITE_REPRODUCIBLE) != 0;
	if (!reproducible) {
		row->mtime = modified->seconds;
		row->mtime_nsec = modified->nanoseconds;
	}
	if (!reproducible && modified->basis == FSIMG_TIME_UTC)
		return (FSIMG_OK);
	return (fsimg_writer_loss(writer, entry, FSIMG_META_MODIFIED,
	    "EROFS cannot preserve the requested modification time", error));
}

/*
 * Encode an entry's kind, size, mode, owner and time into its row. The
 * mode defaults to 0755 for directories and symbolic links and 0644 for
 * files and FIFOs; an absent owner is 0. A directory's size and its
 * subdirectory links are filled in later by plan_entry().
 */
static fsimg_status_t
encode_entry(erofs_plan_t *plan, const fsimg_entry_t *entry, erofs_row_t *row,
    fsimg_error_t *error)
{
	const fsimg_metadata_t *m = &entry->metadata;
	uint16_t type, permissions;

	bzero(row, sizeof(*row));
	row->links = 1;
	row->layout = EROFS_LAYOUT_FLAT;
	switch (entry->kind) {
	case FSIMG_ENTRY_FILE:
		type = EROFS_MODE_REGULAR;
		permissions = 0644;
		row->size = entry->size;
		break;
	case FSIMG_ENTRY_DIRECTORY:
		type = EROFS_MODE_DIRECTORY;
		permissions = 0755;
		row->links = 2;
		break;
	case FSIMG_ENTRY_SYMLINK:
		type = EROFS_MODE_SYMLINK;
		permissions = 0755;
		row->size = entry->size;
		break;
	case FSIMG_ENTRY_FIFO:
		type = EROFS_MODE_FIFO;
		permissions = 0644;
		break;
	default:
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_WRITER,
		    "EROFS planning received an unknown entry kind"));
	}
	if ((m->valid & FSIMG_META_MODE) != 0)
		permissions = (uint16_t)(m->mode & FSIMG_MODE_BITS);
	row->mode = (uint16_t)(type | permissions);
	if ((m->valid & FSIMG_META_OWNER) != 0) {
		row->uid = m->uid;
		row->gid = m->gid;
	}
	return (encode_time(plan, entry, row, error));
}

/* Add one subdirectory to its parent's link count. */
static fsimg_status_t
count_subdirectory(erofs_plan_t *plan, uint64_t parent_id,
    fsimg_error_t *error)
{
	erofs_row_t parent;
	fsimg_status_t status;

	status = erofs_row_get(plan, parent_id, &parent, error);
	if (status != FSIMG_OK)
		return (status);
	if (parent.links == UINT32_MAX)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER,
		    "EROFS directory link count exceeds 32 bits"));
	parent.links++;
	return (erofs_row_set(plan, parent_id, &parent, error));
}

/*
 * Pass 2 for catalog entry id. Its row is encoded from the catalog
 * snapshot before a directory's children are walked, since the walk
 * reuses the snapshot's record buffer. The parent's row, earlier in ID
 * order, gains a link for a subdirectory. Under addressing=48 the epoch
 * follows the smallest modification second, so the compact form can store
 * any time within 2^32 seconds after it.
 */
static fsimg_status_t
plan_entry(erofs_plan_t *plan, uint64_t id, uint64_t *child_cursor,
    fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	const fsimg_catalog_entry_t *catalog;
	erofs_row_t row;
	uint64_t parent_id;
	fsimg_status_t status;

	status = fsimg_session_charge(&writer->session, 0, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_catalog_get(writer, id, &catalog, error);
	if (status != FSIMG_OK)
		return (status);
	status = encode_entry(plan, &catalog->entry, &row, error);
	if (status != FSIMG_OK)
		return (status);
	parent_id = catalog->parent_id;
	if (plan->addressing48 && row.mtime < plan->epoch)
		plan->epoch = row.mtime;

	if (catalog->entry.kind == FSIMG_ENTRY_DIRECTORY) {
		status = count_subdirectory(plan, parent_id, error);
		if (status != FSIMG_OK)
			return (status);
		status = erofs_dir_walk(plan, child_cursor, id, parent_id, NULL,
		    &row.size, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (erofs_row_set(plan, id, &row, error));
}

/*
 * Pass 2: the root's row, which no catalog entry describes (mode 0755,
 * owner 0, the format time), then one row per entry in ID order. Walking
 * directories in increasing ID order consumes the child stream in order,
 * so one cursor serves them all.
 */
static fsimg_status_t
plan_entries(erofs_plan_t *plan, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	erofs_row_t root;
	uint64_t child_cursor;
	fsimg_status_t status;

	root = (erofs_row_t){
		.mtime = plan->format_time,
		.links = 2,
		.mode = EROFS_MODE_DIRECTORY | 0755,
		.layout = EROFS_LAYOUT_FLAT
	};
	child_cursor = 0;
	status = erofs_dir_walk(plan, &child_cursor, 0, 0, NULL, &root.size,
	    error);
	if (status != FSIMG_OK)
		return (status);
	status = erofs_row_set(plan, 0, &root, error);
	if (status != FSIMG_OK)
		return (status);

	for (uint64_t id = 1; id <= writer->count; id++) {
		status = plan_entry(plan, id, &child_cursor, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/*
 * The number of data blocks the row stores under its flat layout, in the
 * shape pass 4 will give it. *measured is set when pass 3 measures the
 * row: a regular file of at least two flat blocks, since a compressed file
 * stores at least one.
 */
static uint64_t
flat_blocks(const erofs_plan_t *plan, const erofs_row_t *row, bool *measured)
{
	erofs_row_t shaped = *row;
	uint64_t blocks;

	choose_inode_shape(plan, &shaped);
	blocks = erofs_row_data_blocks(&shaped, plan->block_bytes);
	*measured = (row->mode & EROFS_MODE_TYPE_MASK) == EROFS_MODE_REGULAR &&
	    blocks >= 2;
	return (blocks);
}

/*
 * Before pass 3 reads any content, the least volume the plan can reach,
 * whatever the measurements find, goes to the output limit check
 * (fsimg_writer_output_floor()), so a limit even that volume exceeds fails
 * unread. It is kept in least_volume, and make_plan() refuses a sized
 * volume below it as a corrupt plan, so a layout rule that lets a file
 * store fewer blocks than this bound counts fails every LZ4 plan that
 * uses it instead of refusing valid images. A requested volume is the
 * volume. Otherwise the metadata area
 * covers at least the boot area and the superblock, a measured file
 * stores at least one cluster per 64 KiB window, since a cluster holds at
 * most a window, unless it stays flat, and every other row stores its flat
 * blocks. The sums saturate: a volume past 64 bits fits no limit.
 */
static fsimg_status_t
check_least_volume(erofs_plan_t *plan, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	const uint32_t block_bytes = plan->block_bytes;
	uint64_t volume, blocks, flat, windows;
	erofs_row_t row;
	bool measured;
	fsimg_status_t status;

	volume = writer->options.volume_bytes;
	if (volume == 0) {
		blocks = (EROFS_WRITE_METADATA_START + block_bytes - 1) /
		    block_bytes;
		for (uint64_t id = 0; id <= writer->count; id++) {
			status = erofs_row_get(plan, id, &row, error);
			if (status != FSIMG_OK)
				return (status);
			flat = flat_blocks(plan, &row, &measured);
			windows = row.size / EROFS_LZ4_WINDOW_BYTES +
			    (row.size % EROFS_LZ4_WINDOW_BYTES != 0);
			fsimg_stat_add(&blocks,
			    measured && windows < flat ? windows : flat);
		}
		if (!fsimg_mul_u64(blocks, block_bytes, &volume))
			volume = UINT64_MAX;
	}
	plan->least_volume = volume;
	return (fsimg_writer_output_floor(writer, volume, error));
}

/*
 * Pass 3, under compression=lz4 only (specs/design-erofs.md, LZ4 profile):
 * once the least volume fits the output limit, each regular file is
 * measured when its flat layout, with the form and inline tail placement
 * will choose, stores at least two blocks, and takes the compressed layout
 * when its clusters are fewer than those blocks. Its records then stay in
 * the plan's cluster records; a file that stays flat leaves them to be
 * overwritten by the next. A file is read to its end even once its
 * clusters reach its flat blocks: every cluster but the last holds at
 * least a block of content, so that happens only within its last block.
 */
static fsimg_status_t
measure_files(erofs_plan_t *plan, fsimg_error_t *error)
{
	fsimg_writer_t *writer = plan->common.writer;
	const uint64_t records_max = (uint64_t)UINT32_MAX + 1;
	erofs_lz4_former_t *former;
	erofs_row_t row;
	uint64_t flat, clusters, record_limit;
	bool measured;
	fsimg_status_t status;

	if (writer->options.erofs_compression != FSIMG_EROFS_COMPRESSION_LZ4)
		return (FSIMG_OK);
	status = check_least_volume(plan, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_store_new(&writer->session, &plan->cluster_records,
	    error);
	if (status != FSIMG_OK)
		return (status);
	status = erofs_lz4_former_new(writer->ctx, &writer->session,
	    plan->block_bytes, &former, error);
	if (status != FSIMG_OK)
		return (status);
	for (uint64_t id = 1; id <= writer->count; id++) {
		status = erofs_row_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			break;
		flat = flat_blocks(plan, &row, &measured);
		if (!measured)
			continue;
		/* A cluster holds at most a window, so a file needing more
		 * clusters than the addressing has blocks is refused unread.
		 */
		if (row.size / EROFS_LZ4_WINDOW_BYTES >= plan->blocks_max) {
			status = addressing_exceeded(plan, error);
			break;
		}
		/*
		 * At most 2^32 records are kept: a file with more stays flat
		 * below, so recording up to flat of them would only fill
		 * scratch.
		 */
		record_limit = flat < records_max ? flat : records_max;
		status = erofs_lz4_measure(plan, former, id, row.size,
		    record_limit, &clusters, error);
		if (status != FSIMG_OK)
			break;
		if (clusters >= flat)
			continue;
		if (clusters > plan->blocks_max) {
			status = addressing_exceeded(plan, error);
			break;
		}
		/*
		 * The row's cluster count and the layout word are 32 bits.
		 * Only under addressing=48 can a file pass that, since
		 * blocks_max is UINT32_MAX otherwise, and its flat layout
		 * holds it, so it stays flat.
		 */
		if (clusters > UINT32_MAX)
			continue;
		row.layout = EROFS_LAYOUT_COMPRESSED_FULL;
		row.clusters = (uint32_t)clusters;
		row.first_cluster = plan->cluster_count;
		plan->cluster_count += clusters;
		status = erofs_row_set(plan, id, &row, error);
		if (status != FSIMG_OK)
			break;
	}
	erofs_lz4_former_free(former);
	return (status);
}

/*
 * Refuse a plan whose metadata or data passes the addressing's largest
 * block number, naming the setting.
 */
static fsimg_status_t
addressing_exceeded(const erofs_plan_t *plan, fsimg_error_t *error)
{
	return (setting_refused(error, FSIMG_ERANGE, "addressing",
	    plan->addressing48 ? "EROFS volume exceeds 48-bit block addressing" :
	    "EROFS volume exceeds 32-bit block addressing"));
}

/*
 * The inode's form and layout. It is compact when its size fits 32 bits,
 * its owner, group and link count 16 bits each, and its time is one the
 * compact form stores exactly: the superblock's epoch plus an unsigned
 * offset of at most span, with zero nanoseconds; otherwise it is extended.
 * Without addressing=48 the epoch is the format time and span is 0. Under
 * it pass 2 has lowered the epoch to the smallest time and span is the
 * 32-bit offset's range, and a compact link count must be 1, the implicit
 * count whose word carries start-block bits, so every directory, which
 * has at least two links, is extended. A file measurement compressed
 * keeps the compressed layout. Other content whose size is not a multiple
 * of the block keeps its tail in the body when the fixed part and the
 * tail fit one block (flat-inline), and in a data block of its own
 * otherwise (flat). Under a metadata box no inode has an inline tail, so
 * the box holds no source bytes and is generated from the plan alone.
 */
static void
choose_inode_shape(const erofs_plan_t *plan, erofs_row_t *row)
{
	const uint64_t span = plan->addressing48 ? UINT32_MAX : 0;
	const uint32_t links_max = plan->addressing48 ? 1 : UINT16_MAX;
	uint64_t tail;
	bool compact_time;

	/* Unsigned subtraction is exact once mtime >= epoch. */
	compact_time = row->mtime >= plan->epoch && row->mtime_nsec == 0 &&
	    (uint64_t)row->mtime - (uint64_t)plan->epoch <= span;
	row->extended = row->size > UINT32_MAX || row->uid > UINT16_MAX ||
	    row->gid > UINT16_MAX ||
	    row->links > links_max || !compact_time;
	if (row->layout == EROFS_LAYOUT_COMPRESSED_FULL)
		return;
	tail = row->size % plan->block_bytes;
	row->layout = EROFS_LAYOUT_FLAT;
	if (plan->metabox == FSIMG_EROFS_METABOX_NONE && tail != 0 &&
	    erofs_row_fixed_bytes(row) + tail <= plan->block_bytes)
		row->layout = EROFS_LAYOUT_FLAT_INLINE;
}

/*
 * Place a body at *offset, the end of the previous one, and advance past
 * it: it starts on the next 32-byte boundary, and moves to the next block
 * when the part of it that may not cross a block would cross one. Returns
 * the body's offset.
 */
static uint64_t
place_body(uint64_t *offset, const erofs_row_t *row, uint64_t body,
    uint32_t block_bytes)
{
	uint64_t start, within;

	start = (*offset + EROFS_NID_SLOT_BYTES - 1) &
	    ~(uint64_t)(EROFS_NID_SLOT_BYTES - 1);
	within = start % block_bytes;
	if (within + erofs_row_unbroken_bytes(row, block_bytes) > block_bytes)
		start += block_bytes - within;
	*offset = start + body;
	return (start);
}

/*
 * The metadata box inode's row for content of content bytes: a regular
 * file of mode 0600, owner 0, one link and the format time, its form by
 * the common rule, flat under metabox=plain and compressed under
 * metabox=lz4 unless the box is empty, which happens only at
 * addressing=32 with the root alone and stores nothing. Data placement
 * adds its start block and clusters.
 */
static void
plan_box_inode(erofs_plan_t *plan, uint64_t content)
{
	erofs_row_t *box = &plan->box;

	*box = (erofs_row_t){
		.size = content,
		.nid = EROFS_WRITE_BOX_NID,
		.mtime = plan->format_time,
		.links = 1,
		.mode = EROFS_MODE_REGULAR | 0600,
		.layout = plan->metabox == FSIMG_EROFS_METABOX_LZ4 &&
		    content != 0 ? EROFS_LAYOUT_COMPRESSED_FULL :
		    EROFS_LAYOUT_FLAT
	};
	choose_inode_shape(plan, box);
	plan->box_body = erofs_box_body_bytes(plan, box);
}

/*
 * Pass 4: inodes in ID order, root first, each placed by place_body(): an
 * inline tail may not cross a block boundary, and the layout keeps a fixed
 * part in one block, so with a body that is the fixed part and any tail
 * the whole body stays in one block. A compressed inode keeps only its
 * fixed part in one block; its index continues across blocks.
 *
 * Without a metadata box the inodes fill the primary area from byte 1152.
 * Under one, the rows erofs_row_in_box() names fill the box's content from
 * byte 0 by the same rules, their NIDs carrying EROFS_NID_BOX, and the
 * content ends at a block boundary, as the primary area does; then the
 * box inode, whose body follows from the content's size, takes NID 37,
 * and at addressing=32 the root follows it in the primary area. Either
 * area ends at a block boundary.
 *
 * Offsets in either space stay within the addressing's block limit, at
 * most 2^60 bytes, checked after every inode, and each inode adds at most
 * a block and a body, which a file size below 2^64 bounds far below 2^63,
 * so nothing can overflow.
 */
static fsimg_status_t
place_metadata(erofs_plan_t *plan, fsimg_error_t *error)
{
	const uint32_t block_bytes = plan->block_bytes;
	uint64_t primary, content;
	fsimg_status_t status;

	primary = EROFS_WRITE_METADATA_START;
	if (plan->metabox != FSIMG_EROFS_METABOX_NONE) {
		content = 0;
		status = place_rows(plan, true, &content, error);
		if (status != FSIMG_OK)
			return (status);
		content = (content + block_bytes - 1) / block_bytes *
		    block_bytes;
		plan_box_inode(plan, content);
		primary = EROFS_WRITE_BOX_NID * EROFS_NID_SLOT_BYTES +
		    plan->box_body;
		if (primary > plan->blocks_max * block_bytes)
			return (addressing_exceeded(plan, error));
	}
	status = place_rows(plan, false, &primary, error);
	if (status != FSIMG_OK)
		return (status);
	status = check_root_nid(plan, error);
	if (status != FSIMG_OK)
		return (status);
	plan->metadata_end = (primary + block_bytes - 1) / block_bytes *
	    block_bytes;
	return (FSIMG_OK);
}

/*
 * Place the rows in the box, or those outside it, in ID order from
 * *offset, each choosing its form and layout first, and leave *offset at
 * the last body's end. A row in the box takes EROFS_NID_BOX in its NID.
 */
static fsimg_status_t
place_rows(erofs_plan_t *plan, bool in_box, uint64_t *offset,
    fsimg_error_t *error)
{
	const uint32_t block_bytes = plan->block_bytes;
	const uint64_t limit = plan->blocks_max * block_bytes;
	erofs_row_t row;
	fsimg_status_t status;

	for (uint64_t id = 0; id <= plan->common.writer->count; id++) {
		if (erofs_row_in_box(plan, id) != in_box)
			continue;
		status = erofs_row_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		choose_inode_shape(plan, &row);
		row.nid = place_body(offset, &row,
		    erofs_row_body_bytes(plan, &row), block_bytes) /
		    EROFS_NID_SLOT_BYTES;
		if (in_box)
			row.nid |= EROFS_NID_BOX;
		if (*offset > limit)
			return (addressing_exceeded(plan, error));
		status = erofs_row_set(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/*
 * Without addressing=48 the superblock names the root in a 16-bit field.
 * The root's inode is the primary area's first without a box, NID 36, but
 * under one it follows the box inode, whose body under metabox=lz4 holds
 * one index record per block of box content, so a box of millions of
 * inodes, or one holding the indexes of many gigabytes of LZ4 files at
 * small blocks, would push it past 16 bits: such a plan is refused,
 * naming the setting.
 */
static fsimg_status_t
check_root_nid(erofs_plan_t *plan, fsimg_error_t *error)
{
	erofs_row_t root;
	fsimg_status_t status;

	if (plan->addressing48)
		return (FSIMG_OK);
	status = erofs_row_get(plan, 0, &root, error);
	if (status != FSIMG_OK)
		return (status);
	if (root.nid > UINT16_MAX)
		return (setting_refused(error, FSIMG_ERANGE, "metabox",
		    "EROFS metadata box puts the root beyond the 16-bit root "
		    "field; use addressing=48"));
	return (FSIMG_OK);
}

/*
 * Pass 5 for the metadata box: its stored blocks follow the data, from
 * used_blocks on, so no data address depends on them, and used_blocks
 * grows to include them. A plain box stores its
 * content's blocks. An LZ4 box's content, which holds every NID and start
 * block and so is final now, is encoded once to count its clusters, whose
 * records follow the files' (erofs_box_measure()); the layout word holds
 * the count in 32 bits. An empty box stores nothing.
 */
static fsimg_status_t
place_box_blocks(erofs_plan_t *plan, fsimg_error_t *error)
{
	erofs_row_t *box = &plan->box;
	uint64_t blocks;
	fsimg_status_t status;

	if (plan->metabox == FSIMG_EROFS_METABOX_NONE)
		return (FSIMG_OK);
	if (box->layout == EROFS_LAYOUT_COMPRESSED_FULL) {
		status = erofs_box_measure(plan, error);
		if (status != FSIMG_OK)
			return (status);
	}
	blocks = erofs_row_data_blocks(box, plan->block_bytes);
	if (blocks > plan->blocks_max - plan->used_blocks)
		return (addressing_exceeded(plan, error));
	box->start_block = blocks != 0 ? plan->used_blocks : 0;
	plan->used_blocks += blocks;
	return (FSIMG_OK);
}

/*
 * Pass 5: each inode's stored blocks, contiguous and in ID order, from the
 * metadata area's end, then the metadata box's (place_box_blocks()). An
 * inode that stores no block keeps start block 0, the word the layout
 * leaves unused. For the summary it also counts the data blocks and the
 * bytes of file and link content; both stay below the addressing's block
 * limit's byte count, checked per inode.
 */
static fsimg_status_t
place_data(erofs_plan_t *plan, uint64_t *payload_blocks,
    uint64_t *payload_bytes, fsimg_error_t *error)
{
	const uint32_t block_bytes = plan->block_bytes;
	erofs_row_t row;
	uint64_t next, blocks;
	uint16_t type;
	fsimg_status_t status;

	*payload_blocks = 0;
	*payload_bytes = 0;
	next = plan->metadata_end / block_bytes;
	for (uint64_t id = 0; id <= plan->common.writer->count; id++) {
		status = erofs_row_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		blocks = erofs_row_data_blocks(&row, block_bytes);
		if (blocks > plan->blocks_max - next)
			return (addressing_exceeded(plan, error));
		row.start_block = blocks != 0 ? next : 0;
		next += blocks;
		status = erofs_row_set(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);

		type = row.mode & EROFS_MODE_TYPE_MASK;
		if (type == EROFS_MODE_REGULAR || type == EROFS_MODE_SYMLINK) {
			*payload_blocks += blocks;
			*payload_bytes += row.size;
		}
	}
	plan->used_blocks = next;
	plan->data_end = next;
	return (place_box_blocks(plan, error));
}

/*
 * Pass 6: without volume_bytes the volume is exactly the used blocks. A
 * requested volume V holds its whole blocks, floor(V / B), which must be
 * at least the used blocks, and keeps its length: a fixed disk fits a
 * partition to 512-byte sectors, so a partial block may follow the last
 * block. So block_count * B <= volume_bytes < (block_count + 1) * B.
 * Emission leaves the excess blocks and that partial block zero, and the
 * block count excludes the partial block. validate_options() has already
 * required whole sectors and a block count the addressing allows.
 */
static fsimg_status_t
size_volume(erofs_plan_t *plan, fsimg_error_t *error)
{
	uint64_t requested, volume_bytes;

	volume_bytes = plan->common.writer->options.volume_bytes;
	requested = volume_bytes / plan->block_bytes;
	if (volume_bytes == 0) {
		plan->block_count = plan->used_blocks;
		plan->volume_bytes = plan->block_count * plan->block_bytes;
		return (FSIMG_OK);
	}
	if (requested < plan->used_blocks)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER,
		    "EROFS content does not fit the volume size"));
	plan->block_count = requested;
	plan->volume_bytes = volume_bytes;
	return (FSIMG_OK);
}

/*
 * The plan information. Metadata is everything but file and link content
 * blocks: the boot area, the superblock, the inodes with their inline
 * tails, directory blocks and the metadata box's blocks. A partial block
 * after the last block is part of the volume's length but of no block
 * category.
 */
static void
summarize(erofs_plan_t *plan, uint64_t payload_blocks)
{
	fsimg_writer_t *writer = plan->common.writer;
	fsimg_plan_info_t *info = &plan->common.info;
	const uint32_t block_bytes = plan->block_bytes;

	info->volume_bytes = plan->volume_bytes;
	info->sector_bytes = 512;
	info->allocation_unit_bytes = block_bytes;
	info->allocation_units = plan->block_count;
	info->allocated_bytes = plan->used_blocks * block_bytes;
	info->free_bytes = (plan->block_count - plan->used_blocks) *
	    block_bytes;
	info->metadata_bytes = (plan->used_blocks - payload_blocks) *
	    block_bytes;
	info->scratch_bytes = writer->ctx->stats.scratch_bytes;
}

static fsimg_status_t
make_plan(fsimg_writer_t *writer, fsimg_error_t *error)
{
	erofs_plan_t *plan;
	fsimg_plan_info_t *info;
	fsimg_error_t detail;
	uint64_t payload_blocks, payload_bytes;
	fsimg_status_t status;

	plan = fsimg_alloc(writer->ctx, sizeof(*plan), &detail);
	if (plan == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	bzero(plan, sizeof(*plan));
	plan->common.writer = writer;
	info = &plan->common.info;
	strcpy(info->format, "erofs");
	info->entries = writer->count;
	info->payload_bytes = writer->payload_bytes;
	info->required_io_flags = FSIMG_IO_WRITE;
	if ((writer->options.flags & FSIMG_WRITE_DURABLE) != 0)
		info->required_io_flags |= FSIMG_IO_DURABLE;

	/* Pass 1: settings, time and identity. */
	plan->block_bytes = writer->options.allocation_unit_bytes;
	plan->addressing48 = writer->options.erofs_addressing == 48;
	plan->blocks_max = plan->addressing48 ? EROFS_WRITE_BLOCKS_MAX_48 :
	    EROFS_WRITE_BLOCKS_MAX;
	plan->metabox = writer->options.erofs_metabox;
	/* The catalog's inodes and the root, and the box inode under a box. */
	plan->inode_count = writer->count + 1 +
	    (plan->metabox != FSIMG_EROFS_METABOX_NONE);
	/* An inode's serial number, its 1-based position, is 32 bits. */
	if (plan->inode_count > UINT32_MAX) {
		status = fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_WRITER, "EROFS inodes exceed the 32-bit serial "
		    "number");
		goto fail;
	}
	status = choose_format_time(plan, error);
	if (status == FSIMG_OK)
		status = fsimg_container_identifier(writer->options.volume_uuid,
		    "uuid", FSIMG_ROLE_EROFS, writer->options.flags,
		    writer->options.epoch, plan->uuid, error);
	if (status != FSIMG_OK)
		goto fail;

	status = fsimg_array_new(&writer->session, EROFS_ROW_BYTES,
	    writer->count + 1, &plan->rows, error);
	if (status != FSIMG_OK)
		goto fail;
	status = plan_entries(plan, error);
	if (status != FSIMG_OK)
		goto fail;
	status = measure_files(plan, error);
	if (status != FSIMG_OK)
		goto fail;
	status = place_metadata(plan, error);
	if (status != FSIMG_OK)
		goto fail;
	status = place_data(plan, &payload_blocks, &payload_bytes, error);
	if (status != FSIMG_OK)
		goto fail;
	status = size_volume(plan, error);
	if (status == FSIMG_OK && plan->volume_bytes < plan->least_volume)
		status = fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_WRITER, "EROFS volume is below its least volume");
	if (status != FSIMG_OK)
		goto fail;

	info->initialized_bytes = payload_bytes;
	summarize(plan, payload_blocks);
	writer->plan = &plan->common;
	return (FSIMG_OK);
fail:
	free_plan(&plan->common);
	return (status);
}

static void
free_plan(fsimg_plan_t *base)
{
	erofs_plan_t *plan = (erofs_plan_t *)base;

	if (plan == NULL)
		return;
	fsimg_array_free(plan->rows);
	fsimg_store_free(plan->cluster_records);
	fsimg_dealloc(plan);
}
