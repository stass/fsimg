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
 * 1. Settings and time fix the block size, the format time and the UUID.
 * 2. Entries encode each entry's kind, size, owner, mode, time and link
 *    count, and pack each directory's names (write-dir.c) for its size.
 * 3. Metadata placement chooses each inode's form and layout, which the
 *    finished link counts decide, and assigns its NID.
 * 4. Data placement assigns each inode's start block in the same order.
 * 5. Sizing checks the requested volume size and the 32-bit block limit.
 *
 * Every row's encoded length depends only on the catalog and the settings,
 * so no pass changes an earlier pass's result, and emission fills NIDs and
 * start blocks into lengths planning has already fixed.
 */

/* The wire layout of a plan row, little endian; bytes 52 to 63 are zero. */
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
static void choose_inode_shape(erofs_row_t *, int64_t, uint32_t);
static fsimg_status_t place_metadata(erofs_plan_t *, fsimg_error_t *);
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
 * The common options under the writer profile: a 512-byte sector, a
 * 4096-byte block, a volume size of zero or whole sectors whose whole
 * blocks fit the 32-bit block count, no free-space request, a label that
 * leaves the name field a terminator, and the EROFS settings. Values
 * outside each domain are invalid. The smaller blocks EROFS defines are
 * refused as unsupported until the kernel is shown to mount them. The
 * writer emits the uncompressed 32-bit layout without a metadata box; the
 * other values of the three settings name layouts it does not write, so
 * they too are unsupported rather than invalid. A metadata box needs
 * 48-bit addressing whether or not it is supported.
 */
static fsimg_status_t
validate_options(fsimg_write_options_t *options, unsigned variant FSIMG_UNUSED,
    fsimg_error_t *error)
{
	static const char empty_label[] = "";
	uint32_t block;
	bool legal_block;

	block = options->allocation_unit_bytes == 0 ? EROFS_WRITE_BLOCK_BYTES :
	    options->allocation_unit_bytes;
	legal_block = block >= 512 && block <= 4096 && (block & (block - 1)) == 0;
	if (!legal_block ||
	    (options->sector_bytes != 0 && options->sector_bytes != 512) ||
	    options->volume_bytes % 512 != 0 ||
	    options->volume_bytes / block > EROFS_WRITE_BLOCKS_MAX ||
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
	if (options->erofs_metabox != FSIMG_EROFS_METABOX_NONE &&
	    options->erofs_addressing != 48)
		return (setting_refused(error, FSIMG_EINVAL, "metabox",
		    "an EROFS metadata box requires 48-bit addressing"));

	if (block != EROFS_WRITE_BLOCK_BYTES)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_WRITER,
		    "the EROFS writer emits 4096-byte blocks only"));
	if (options->erofs_compression != FSIMG_EROFS_COMPRESSION_NONE)
		return (setting_refused(error, FSIMG_EUNSUPPORTED, "compression",
		    "the EROFS writer does not compress files"));
	if (options->erofs_addressing == 48)
		return (setting_refused(error, FSIMG_EUNSUPPORTED, "addressing",
		    "the EROFS writer does not write 48-bit addressing"));
	if (options->erofs_metabox != FSIMG_EROFS_METABOX_NONE)
		return (setting_refused(error, FSIMG_EUNSUPPORTED, "metabox",
		    "the EROFS writer does not write a metadata box"));

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
 * the offset the NID gives: see row_consistent().
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
	    raw[ROW_LAYOUT] != EROFS_LAYOUT_FLAT_INLINE) ||
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
	if (!row_consistent(row, plan->block_bytes))
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_WRITER, "invalid EROFS plan row"));
	return (FSIMG_OK);
}

/*
 * The invariants every stored row keeps: one of the four kinds the writer
 * emits, a FIFO empty, nanoseconds below a second, a flat-inline layout
 * only for a tail that fits one block with the fixed part, and, once a NID
 * is assigned, the whole body inside the NID's block.
 */
static bool
row_consistent(const erofs_row_t *row, uint32_t block_bytes)
{
	uint64_t tail, within;
	uint16_t type;

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
	if (row->nid == 0)
		return (true);
	within = row->nid % (block_bytes / EROFS_NID_SLOT_BYTES) *
	    EROFS_NID_SLOT_BYTES;
	return (within + erofs_row_body_bytes(row, block_bytes) <= block_bytes);
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
	return (fsimg_array_set(plan->rows, id, raw, error));
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
 * stores no attributes. Neither an inline tail nor a fixed part the writer
 * places crosses a block, so the whole body lies in one block.
 */
uint32_t
erofs_row_body_bytes(const erofs_row_t *row, uint32_t block_bytes)
{
	return (erofs_row_fixed_bytes(row) +
	    erofs_row_tail_bytes(row, block_bytes));
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
 * flat layout, a partial last block included, and the full blocks only
 * under the flat-inline layout, whose tail is in the body.
 */
uint64_t
erofs_row_data_blocks(const erofs_row_t *row, uint32_t block_bytes)
{
	uint64_t blocks;

	blocks = row->size / block_bytes;
	if (row->layout == EROFS_LAYOUT_FLAT && row->size % block_bytes != 0)
		blocks++;
	return (blocks);
}

/*
 * Pass 1 for time: the format time is the reproducible epoch, or the
 * host's UTC clock sampled once. It is the superblock's epoch, with zero
 * fixed nanoseconds, and the time of every inode whose own is absent.
 */
static fsimg_status_t
choose_format_time(erofs_plan_t *plan, fsimg_error_t *error)
{
	const fsimg_write_options_t *options = &plan->common.writer->options;
	time_t now;

	if ((options->flags & FSIMG_WRITE_REPRODUCIBLE) != 0) {
		plan->format_time = options->epoch;
		return (FSIMG_OK);
	}
	now = time(NULL);
	if (now == (time_t)-1)
		return (fsimg_error_set(error, FSIMG_EIO, 0, FSIMG_OP_WRITER,
		    "host clock is unavailable"));
	plan->format_time = (int64_t)now;
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
 * order, gains a link for a subdirectory.
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
 * The inode's form and layout. It is compact when its size fits 32 bits,
 * its owner and link count 16 bits each, and its time is the format time
 * to the nanosecond, which a compact inode stores as the superblock's
 * epoch; otherwise extended. Content whose size is not a multiple of the
 * block keeps its tail in the body when the fixed part and the tail fit
 * one block (flat-inline), and in a data block of its own otherwise
 * (flat).
 */
static void
choose_inode_shape(erofs_row_t *row, int64_t format_time,
    uint32_t block_bytes)
{
	uint64_t tail;

	row->extended = row->size > UINT32_MAX || row->uid > UINT16_MAX ||
	    row->gid > UINT16_MAX || row->links > UINT16_MAX ||
	    row->mtime != format_time || row->mtime_nsec != 0;
	tail = row->size % block_bytes;
	row->layout = EROFS_LAYOUT_FLAT;
	if (tail != 0 && erofs_row_fixed_bytes(row) + tail <= block_bytes)
		row->layout = EROFS_LAYOUT_FLAT_INLINE;
}

/*
 * Pass 3: inodes in ID order, root first, from byte 1152. Each starts on
 * the next 32-byte boundary. An inline tail may not cross a block
 * boundary, and the layout keeps a fixed part in one block, so with a
 * body that is the fixed part and any tail the whole body stays in one
 * block: an inode that would cross starts at the next block. The area
 * ends at a block boundary. Offsets
 * stay below the 32-bit block limit, checked after every inode, and each
 * inode adds at most a block and a body, so nothing can overflow.
 */
static fsimg_status_t
place_metadata(erofs_plan_t *plan, fsimg_error_t *error)
{
	const uint32_t block_bytes = plan->block_bytes;
	const uint64_t limit = EROFS_WRITE_BLOCKS_MAX * block_bytes;
	erofs_row_t row;
	uint64_t offset, within;
	uint32_t body;
	fsimg_status_t status;

	offset = EROFS_WRITE_METADATA_START;
	for (uint64_t id = 0; id <= plan->common.writer->count; id++) {
		status = erofs_row_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		choose_inode_shape(&row, plan->format_time, block_bytes);
		body = erofs_row_body_bytes(&row, block_bytes);

		offset = (offset + EROFS_NID_SLOT_BYTES - 1) &
		    ~(uint64_t)(EROFS_NID_SLOT_BYTES - 1);
		within = offset % block_bytes;
		if (within + body > block_bytes)
			offset += block_bytes - within;
		row.nid = offset / EROFS_NID_SLOT_BYTES;
		offset += body;
		if (offset > limit)
			return (setting_refused(error, FSIMG_ERANGE, "addressing",
			    "EROFS metadata exceeds 32-bit block addressing"));
		status = erofs_row_set(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
	}
	plan->metadata_end = (offset + block_bytes - 1) / block_bytes *
	    block_bytes;
	return (FSIMG_OK);
}

/*
 * Pass 4: each inode's stored blocks, contiguous and in ID order, from the
 * metadata area's end. An inode that stores no block keeps start block 0,
 * the word the layout leaves unused. For the summary it also counts the
 * data blocks and the bytes of file and link content; both stay below the
 * 32-bit block limit's byte count, checked per inode.
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
		if (blocks > EROFS_WRITE_BLOCKS_MAX - next)
			return (setting_refused(error, FSIMG_ERANGE, "addressing",
			    "EROFS data exceeds 32-bit block addressing"));
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
	return (FSIMG_OK);
}

/*
 * Pass 5: without volume_bytes the volume is exactly the used blocks. A
 * requested volume V holds its whole blocks, floor(V / B), which must be
 * at least the used blocks, and keeps its length: a fixed disk fits a
 * partition to 512-byte sectors, so a partial block may follow the last
 * block. So block_count * B <= volume_bytes < (block_count + 1) * B.
 * Emission leaves the excess blocks and that partial block zero, and the
 * block count excludes the partial block. validate_options() has already
 * required whole sectors and a block count within 32 bits. An inode's
 * serial number, its 1-based position, is 32 bits in both forms.
 */
static fsimg_status_t
size_volume(erofs_plan_t *plan, fsimg_error_t *error)
{
	uint64_t requested, volume_bytes;

	if (plan->inode_count > UINT32_MAX)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER,
		    "EROFS inodes exceed the 32-bit serial number"));

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
 * tails, and directory blocks. A partial block after the last block is
 * part of the volume's length but of no block category.
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
	plan->inode_count = writer->count + 1;
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
	status = place_metadata(plan, error);
	if (status != FSIMG_OK)
		goto fail;
	status = place_data(plan, &payload_blocks, &payload_bytes, error);
	if (status != FSIMG_OK)
		goto fail;
	status = size_volume(plan, error);
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
	fsimg_dealloc(plan);
}
