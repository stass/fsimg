/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "format.h"
#include "fsimg-endian.h"
#include "hfsplus.h"

/*
 * Opening a volume completes recognition in a fixed order, each step failing
 * before the next reads:
 *
 * 1. check_identity(): the identity; enter_wrapper() then narrows the reader to
 *    a wrapped volume's embedded range, so every later step, the journal's
 *    offsets and verification among them, works on that range alone;
 * 2. derive_geometry(): the header, its attribute table and the geometry;
 * 3. open_extents_tree(): the extents tree, whose fork resolves from its inline
 *    extents alone;
 * 4. open_catalog_tree(): the catalog tree, whose fork's overflow lookups now
 *    work, and its comparator;
 * 5. open_remaining_forks(): the attributes tree and the bitmap and startup
 *    forks;
 * 6. journal_open(): the journal, under the info block's flags table;
 * 7. open_root(): the root folder through its thread, whose name is the label.
 *
 * Open reads at most the header, three header nodes, a few search paths and
 * the two journal records.
 */

/* The attribute word's closed table: bits 7 to 15 and 31. */
#define ATTRIBUTES_KNOWN	UINT32_C(0x8000ff80)
#define ATTRIBUTE_JOURNALED	(UINT32_C(1) << 13)

#define JIB_FLAG_IN_VOLUME	0x1u
#define JIB_FLAG_EXTERNAL	0x2u
#define JIB_FLAG_NEEDS_INIT	0x4u

#define JOURNAL_SIZE_MIN	(UINT64_C(256) * 1024)
#define JOURNAL_SIZE_MAX	(UINT64_C(1024) * 1024 * 1024)
#define JOURNAL_ALIGN		512u

/* The bytes no fork can occupy: the boot blocks and primary header, and the
 * alternate header with the reserved sector after it.
 */
#define RESERVED_HEAD_BYTES	1536u
#define RESERVED_TAIL_BYTES	1024u

/* The outer volume's sector size. */
#define WRAPPER_SECTOR_BYTES	512u

static fsimg_status_t check_identity(fsimg_reader_t *,
    const fsimg_format_variant_t *, const unsigned char *, hfsplus_mdb_t *,
    bool *, fsimg_error_t *);
static fsimg_status_t wrapper_range(const hfsplus_mdb_t *, uint64_t *,
    uint64_t *, fsimg_error_t *);
static fsimg_status_t enter_wrapper(fsimg_reader_t *, const hfsplus_mdb_t *,
    unsigned char *, uint64_t *, fsimg_error_t *);
static fsimg_status_t derive_geometry(fsimg_reader_t *,
    const unsigned char *, uint64_t, fsimg_error_t *);
static fsimg_status_t open_extents_tree(fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t select_comparator(fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t allocate_scan_node(fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t open_catalog_tree(fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t open_remaining_forks(fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t journal_open(fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t journal_state_from_flags(const fsimg_hfsplus_reader_t *,
    uint32_t, hfsplus_journal_state_t *, fsimg_error_t *);
static fsimg_status_t journal_location(fsimg_reader_t *, const hfsplus_jib_t *,
    hfsplus_extent_t *, fsimg_error_t *);
static fsimg_status_t journal_header_check(fsimg_reader_t *,
    const hfsplus_jib_t *, const hfsplus_extent_t *, fsimg_error_t *);
static fsimg_status_t open_root(fsimg_reader_t *, fsimg_error_t *);

/* The identity signature, which a caller assuming the identity waives; a
 * signature the variant it forced contradicts is refused either way. A volume
 * with the waived signature fails the header's pairing check in the next step.
 * *wrapper says whether the block at 1024 is a wrapper's, decoded into *mdb.
 */
static fsimg_status_t
check_identity(fsimg_reader_t *reader, const fsimg_format_variant_t *forced,
    const unsigned char *prefix, hfsplus_mdb_t *mdb, bool *wrapper,
    fsimg_error_t *error)
{
	const unsigned char *block = prefix + HFSPLUS_BOOT_BYTES;
	uint16_t signature = fsimg_load_be16(block);
	bool known, variant_x;
	fsimg_status_t status;

	*wrapper = hfsplus_wrapper_decode(block, HFSPLUS_HEADER_BYTES, mdb);
	known = *wrapper || signature == HFSPLUS_SIGNATURE_PLUS ||
	    signature == HFSPLUS_SIGNATURE_X;
	status = fsimg_reader_identity(reader,
	    known && hfsplus_boot_area_valid(prefix),
	    "no HFS Plus identity signature",
	    "no HFS Plus signature at 1024 over a zero or LK boot area", error);
	if (status != FSIMG_OK)
		return (status);
	variant_x = signature == HFSPLUS_SIGNATURE_X;
	if (forced != NULL && known &&
	    (forced->id == HFSPLUS_VARIANT_X) != variant_x)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0,
		    FSIMG_OP_READER,
		    "HFS Plus signature contradicts the forced variant"));
	return (FSIMG_OK);
}

/*
 * The range the wrapper's location fields name, refused when hostile: it must
 * lie in the outer volume's allocation blocks, start past the master directory
 * block and hold an HFS Plus header. This reads only the fields, so a cut image
 * gets the same answer as a whole one.
 *
 * The fields are 16 and 32 bits wide, so no sum or product here can overflow 64
 * bits: the largest end is below 2^50.
 */
static fsimg_status_t
wrapper_range(const hfsplus_mdb_t *mdb, uint64_t *offset, uint64_t *length,
    fsimg_error_t *error)
{
	if (mdb->allocation_block_size == 0 ||
	    mdb->allocation_block_size % WRAPPER_SECTOR_BYTES != 0)
		return (hfsplus_corrupt(error,
		    "HFS wrapper's allocation block size is not a multiple of 512"));
	if (mdb->embedded_count == 0)
		return (hfsplus_corrupt(error,
		    "HFS wrapper's embedded extent is empty"));
	if ((uint32_t)mdb->embedded_start + mdb->embedded_count >
	    mdb->allocation_blocks)
		return (hfsplus_corrupt(error,
		    "HFS wrapper's embedded extent lies past its outer volume"));

	*offset = (uint64_t)mdb->first_block_sector * WRAPPER_SECTOR_BYTES +
	    (uint64_t)mdb->embedded_start * mdb->allocation_block_size;
	*length = (uint64_t)mdb->embedded_count * mdb->allocation_block_size;
	if (*offset < HFSPLUS_PROBE_BYTES)
		return (hfsplus_corrupt(error,
		    "HFS wrapper's embedded volume overlaps its master directory block"));
	/* Otherwise a whole image would read as cut inside its embedded header. */
	if (*length < HFSPLUS_PROBE_BYTES)
		return (hfsplus_corrupt(error,
		    "HFS wrapper's embedded range is shorter than an HFS Plus header"));
	return (FSIMG_OK);
}

/*
 * Narrow the reader to the embedded range and read the embedded volume's first
 * bytes into prefix. From here the driver sees only that volume; the outer
 * volume is an envelope and is not interpreted, so a cut in its tail alone
 * shows nowhere. *embedded_bytes receives the range's length, uncut.
 */
static fsimg_status_t
enter_wrapper(fsimg_reader_t *reader, const hfsplus_mdb_t *mdb,
    unsigned char *prefix, uint64_t *embedded_bytes, fsimg_error_t *error)
{
	fsimg_hfsplus_reader_t *hfsplus = hfsplus_state(reader);
	uint64_t offset = 0, length = 0;
	size_t transferred;
	fsimg_status_t status;

	status = wrapper_range(mdb, &offset, &length, error);
	if (status == FSIMG_OK)
		status = fsimg_reader_narrow(reader, offset, length, error);
	if (status != FSIMG_OK)
		return (status);
	*embedded_bytes = length;
	hfsplus->wrapper_offset = offset;
	hfsplus->wrapped = true;

	status = fsimg_reader_bytes(reader, 0, prefix, HFSPLUS_PROBE_BYTES,
	    &transferred, error);
	if (status != FSIMG_OK)
		return (status);
	if (fsimg_load_be16(prefix + HFSPLUS_BOOT_BYTES) !=
	    HFSPLUS_SIGNATURE_PLUS ||
	    fsimg_load_be16(prefix + HFSPLUS_BOOT_BYTES + 2) != HFSPLUS_VERSION_PLUS)
		return (hfsplus_corrupt(error,
		    "HFS wrapper's embedded volume is not HFS Plus version 4"));
	return (FSIMG_OK);
}

/* Decode the header, apply the attribute table and derive the content domain
 * [r * B, (N - t) * B), where r blocks overlap [0, 1536) and t blocks below N
 * overlap [V - 1024, V). A wrapper states V: embedded_bytes, read only when
 * wrapped, is its range's length, cut or not. A bare volume's V is the slice's
 * length when it reaches N * B. A shorter slice proves the volume's tail is gone
 * and reports N * B, but that is only a lower bound on V, so t cannot be known
 * and is taken as 0: the volume keeps the geometry it declares, the read funnel
 * refuses what the slice does not hold, and bytes it does hold in the last
 * block are harmless.
 */
static fsimg_status_t
derive_geometry(fsimg_reader_t *reader, const unsigned char *prefix,
    uint64_t embedded_bytes, fsimg_error_t *error)
{
	fsimg_hfsplus_reader_t *hfsplus = hfsplus_state(reader);
	hfsplus_header_t *header = &hfsplus->header;
	hfsplus_geometry_t *g = &hfsplus->geometry;
	uint64_t allocation_bytes, slice_bytes, tail_block, volume_bytes;
	bool tail_known;
	uint32_t block_size, blocks, first, end;
	fsimg_status_t status;

	if (!hfsplus_header_decode(prefix + HFSPLUS_BOOT_BYTES,
	    HFSPLUS_HEADER_BYTES, header))
		return (hfsplus_corrupt(error, "HFS Plus header is malformed"));
	status = hfsplus_header_check(header, error);
	if (status != FSIMG_OK)
		return (status);
	/* Both operands are 32 bits wide, so the product cannot overflow. */
	allocation_bytes = (uint64_t)header->total_blocks * header->block_size;
	slice_bytes = reader->slice_bytes;
	if (hfsplus->wrapped) {
		/* A shorter range would read as a cut that nothing cut. */
		if (allocation_bytes > embedded_bytes)
			return (hfsplus_corrupt(error,
			    "HFS wrapper's embedded range is shorter than the volume's block size and count"));
		volume_bytes = embedded_bytes;
		tail_known = true;
	} else {
		tail_known = allocation_bytes <= slice_bytes;
		volume_bytes = tail_known ? slice_bytes : allocation_bytes;
	}
	/* N * B is whole sectors, so this fires only on a slice reaching it. */
	if (volume_bytes % 512 != 0)
		return (hfsplus_corrupt(error,
		    "HFS Plus volume length is not whole sectors"));
	if ((header->attributes & ~ATTRIBUTES_KNOWN) != 0)
		return (hfsplus_unsupported(error,
		    "HFS Plus volume has an unsupported attribute"));
	if (header->free_blocks > header->total_blocks)
		return (hfsplus_corrupt(error,
		    "HFS Plus free block count exceeds the volume"));
	if (header->next_catalog_id < HFSPLUS_ID_FIRST_USER)
		return (hfsplus_corrupt(error,
		    "HFS Plus next catalog ID is reserved"));

	block_size = header->block_size;
	blocks = header->total_blocks;
	first = (uint32_t)((RESERVED_HEAD_BYTES + (uint64_t)block_size - 1) /
	    block_size);
	if (tail_known) {
		tail_block = (volume_bytes - RESERVED_TAIL_BYTES) / block_size;
		end = tail_block < blocks ? (uint32_t)tail_block : blocks;
	} else
		end = blocks;
	if (first >= end)
		return (hfsplus_corrupt(error,
		    "HFS Plus volume leaves no blocks for content"));
	g->volume_bytes = volume_bytes;
	g->slice_bytes = slice_bytes;
	g->block_size = block_size;
	g->blocks = blocks;
	g->first_block = first;
	g->end_block = end;
	reader->content_domain = (fsimg_extent_domain_t){
		.offset = (uint64_t)first * block_size,
		.length = (uint64_t)(end - first) * block_size,
		.unit_bytes = block_size
	};
	return (FSIMG_OK);
}

/* The extents tree comes first and its fork may not need overflow: the
 * resolver refuses that, so opening it never reads itself.
 */
static fsimg_status_t
open_extents_tree(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_hfsplus_reader_t *hfsplus = hfsplus_state(reader);
	fsimg_status_t status;

	status = hfsplus_tree_open(reader, &hfsplus->extents,
	    HFSPLUS_TREE_EXTENTS, HFSPLUS_ID_EXTENTS,
	    &hfsplus->header.extents_file, error);
	if (status != FSIMG_OK)
		return (status);
	if (!hfsplus->extents.present)
		return (hfsplus_corrupt(error,
		    "HFS Plus extents tree is missing"));
	return (FSIMG_OK);
}

/* The catalog's name order comes from its header, never from the signature
 * alone: an HFSX volume can be case-insensitive.
 */
static fsimg_status_t
select_comparator(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_hfsplus_reader_t *hfsplus = hfsplus_state(reader);
	uint8_t type = hfsplus->catalog.header.key_compare_type;
	bool variant_x = hfsplus->header.signature == HFSPLUS_SIGNATURE_X;

	if (type == HFSPLUS_COMPARE_TYPE_FOLDED || (type == 0 && !variant_x))
		hfsplus->catalog.compare = HFSPLUS_COMPARE_FOLDED;
	else if (type == HFSPLUS_COMPARE_TYPE_BINARY && variant_x)
		hfsplus->catalog.compare = HFSPLUS_COMPARE_BINARY;
	else
		return (hfsplus_unsupported(error,
		    "HFS Plus catalog's key compare type is not supported"));
	return (FSIMG_OK);
}

/* The second catalog buffer: a directory scan's cursor keeps its node while
 * searches run in the tree's own.
 */
static fsimg_status_t
allocate_scan_node(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_hfsplus_reader_t *hfsplus = hfsplus_state(reader);
	fsimg_error_t detail;

	hfsplus->catalog_scan_node = fsimg_alloc(reader->ctx,
	    hfsplus->catalog.header.node_size, &detail);
	if (hfsplus->catalog_scan_node == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	return (FSIMG_OK);
}

static fsimg_status_t
open_catalog_tree(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_hfsplus_reader_t *hfsplus = hfsplus_state(reader);
	fsimg_status_t status;

	status = hfsplus_tree_open(reader, &hfsplus->catalog,
	    HFSPLUS_TREE_CATALOG, HFSPLUS_ID_CATALOG,
	    &hfsplus->header.catalog_file, error);
	if (status != FSIMG_OK)
		return (status);
	if (!hfsplus->catalog.present)
		return (hfsplus_corrupt(error, "HFS Plus catalog is missing"));
	status = select_comparator(reader, error);
	if (status != FSIMG_OK)
		return (status);
	return (allocate_scan_node(reader, error));
}

/* A zero attributes or startup fork means the volume has none; the startup file
 * is validated and held by nothing, since no read needs it.
 */
static fsimg_status_t
open_remaining_forks(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_hfsplus_reader_t *hfsplus = hfsplus_state(reader);
	const hfsplus_header_t *header = &hfsplus->header;
	fsimg_extent_index_t *startup;
	fsimg_status_t status;

	status = hfsplus_tree_open(reader, &hfsplus->attributes,
	    HFSPLUS_TREE_ATTRIBUTES, HFSPLUS_ID_ATTRIBUTES,
	    &header->attributes_file, error);
	if (status != FSIMG_OK)
		return (status);
	status = hfsplus_fork_index(reader, HFSPLUS_ID_ALLOCATION,
	    HFSPLUS_FORK_DATA, &header->allocation_file, &hfsplus->bitmap,
	    error);
	if (status != FSIMG_OK)
		return (status);
	if (hfsplus->bitmap == NULL)
		return (hfsplus_corrupt(error, "HFS Plus bitmap is missing"));
	status = hfsplus_fork_index(reader, HFSPLUS_ID_STARTUP,
	    HFSPLUS_FORK_DATA, &header->startup_file, &startup, error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_extent_index_free(startup);
	return (FSIMG_OK);
}

/* The location rule: the journal is the one extent of the root's .journal file,
 * found by exact stored units, in whole blocks, and the info block's offset and
 * size must agree with it. Nothing is searched for and nothing converted.
 */
static fsimg_status_t
journal_location(fsimg_reader_t *reader, const hfsplus_jib_t *jib,
    hfsplus_extent_t *extent, fsimg_error_t *error)
{
	const hfsplus_owned_name_t *name =
	    &hfsplus_owned_names[HFSPLUS_OWNED_JOURNAL];
	uint32_t block_size = hfsplus_state(reader)->geometry.block_size;
	hfsplus_key_t key;
	hfsplus_record_t record;
	hfsplus_file_t file;
	bool found;
	fsimg_status_t status;

	status = hfsplus_catalog_key_init(&key, HFSPLUS_ID_ROOT, name->units,
	    name->count, error);
	if (status == FSIMG_OK)
		status = hfsplus_catalog_find_exact(
		    &hfsplus_state(reader)->catalog, &key, &record, &found,
		    error);
	if (status != FSIMG_OK)
		return (status);
	if (!found)
		return (hfsplus_corrupt(error,
		    "HFS Plus journaled volume has no .journal file"));
	if (!hfsplus_file_decode(record.data, record.data_bytes, &file))
		return (hfsplus_corrupt(error,
		    "HFS Plus .journal is not a file record"));
	status = hfsplus_fork_one_extent(reader, &file.data_fork, extent,
	    error);
	if (status != FSIMG_OK)
		return (status);
	if (file.data_fork.logical_size != jib->size ||
	    file.data_fork.logical_size !=
	    (uint64_t)extent->count * block_size ||
	    jib->offset != (uint64_t)extent->start * block_size)
		return (hfsplus_corrupt(error,
		    "HFS Plus .journal disagrees with the journal info block"));
	return (FSIMG_OK);
}

/* Read the journal header at the extent's start and apply the wrap rule and the
 * reader policy: no transaction pending is read normally, any pending one is
 * refused, since fsimg never replays.
 */
static fsimg_status_t
journal_header_check(fsimg_reader_t *reader, const hfsplus_jib_t *jib,
    const hfsplus_extent_t *extent, fsimg_error_t *error)
{
	fsimg_hfsplus_reader_t *hfsplus = hfsplus_state(reader);
	unsigned char raw[HFSPLUS_JOURNAL_BYTES];
	hfsplus_journal_t journal;
	uint64_t start, end;
	size_t transferred;
	fsimg_status_t status;

	status = fsimg_reader_bytes(reader,
	    (uint64_t)extent->start * hfsplus->geometry.block_size, raw,
	    sizeof(raw), &transferred, error);
	if (status != FSIMG_OK)
		return (status);
	if (hfsplus_journal_is_older_form(raw, sizeof(raw)))
		return (hfsplus_unsupported(error,
		    "HFS Plus journal has an older header form fsimg does not decode"));
	if (!hfsplus_journal_decode(raw, sizeof(raw), &journal))
		return (hfsplus_corrupt(error,
		    "HFS Plus journal header is malformed or fails its checksum"));
	if (journal.size != jib->size || journal.size < JOURNAL_SIZE_MIN ||
	    journal.size > JOURNAL_SIZE_MAX)
		return (hfsplus_corrupt(error,
		    "HFS Plus journal size is invalid"));
	if (journal.start == 0 || journal.start > journal.size ||
	    journal.start % JOURNAL_ALIGN != 0 ||
	    journal.end == 0 || journal.end > journal.size ||
	    journal.end % JOURNAL_ALIGN != 0)
		return (hfsplus_corrupt(error,
		    "HFS Plus journal position is out of bounds"));

	/* A position equal to size is the log's wrap point, jhdr_size. */
	start = journal.start;
	end = journal.end;
	if (start == journal.size)
		start = journal.jhdr_size;
	if (end == journal.size)
		end = journal.jhdr_size;
	if (start != end)
		return (hfsplus_unsupported(error,
		    "HFS Plus journal holds transactions; mount the volume to replay"));
	return (FSIMG_OK);
}

/* The info block's flags table of the format design, one row per test in its
 * order; the first matching row decides.
 */
static fsimg_status_t
journal_state_from_flags(const fsimg_hfsplus_reader_t *hfsplus, uint32_t flags,
    hfsplus_journal_state_t *state, fsimg_error_t *error)
{
	if ((flags & JIB_FLAG_EXTERNAL) != 0)
		return (hfsplus_unsupported(error,
		    "HFS Plus journal is on another device"));
	if ((flags & JIB_FLAG_IN_VOLUME) == 0)
		return (hfsplus_corrupt(error,
		    "HFS Plus journal info block has no location flag"));
	if ((flags & JIB_FLAG_NEEDS_INIT) != 0) {
		*state = HFSPLUS_JOURNAL_UNINITIALIZED;
		return (FSIMG_OK);
	}
	if (hfsplus->wrapped)
		return (hfsplus_unsupported(error,
		    "HFS Plus journal inside a wrapper has no established location"));
	*state = HFSPLUS_JOURNAL_CLEAN;
	return (FSIMG_OK);
}

/* Read the info block and apply its flags: the table decides the state, every
 * state but a refusal requires the location rule, and only a local initialized
 * journal has a header to read.
 */
static fsimg_status_t
journal_open(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_hfsplus_reader_t *hfsplus = hfsplus_state(reader);
	const hfsplus_geometry_t *g = &hfsplus->geometry;
	unsigned char raw[HFSPLUS_JIB_BYTES];
	uint32_t info_block = hfsplus->header.journal_info_block;
	hfsplus_extent_t extent;
	hfsplus_jib_t jib;
	hfsplus_journal_state_t state = HFSPLUS_JOURNAL_NONE;
	size_t transferred;
	fsimg_status_t status;

	if ((hfsplus->header.attributes & ATTRIBUTE_JOURNALED) == 0)
		return (FSIMG_OK);
	if (info_block < g->first_block || info_block >= g->end_block)
		return (hfsplus_corrupt(error,
		    "HFS Plus journal info block lies outside the content domain"));
	status = fsimg_reader_bytes(reader,
	    (uint64_t)info_block * g->block_size, raw, sizeof(raw),
	    &transferred, error);
	if (status != FSIMG_OK)
		return (status);
	if (!hfsplus_jib_decode(raw, sizeof(raw), &jib))
		return (hfsplus_corrupt(error,
		    "HFS Plus journal info block is short"));

	status = journal_state_from_flags(hfsplus, jib.flags, &state, error);
	if (status == FSIMG_OK)
		status = journal_location(reader, &jib, &extent, error);
	if (status == FSIMG_OK && state == HFSPLUS_JOURNAL_CLEAN)
		status = journal_header_check(reader, &jib, &extent, error);
	if (status != FSIMG_OK)
		return (status);
	hfsplus->journal = state;
	hfsplus->journal_info_block = info_block;
	hfsplus->journal_start = extent.start;
	hfsplus->journal_blocks = extent.count;
	return (FSIMG_OK);
}

/* The root: the thread (2, "") must be a folder thread with parent 1, and the
 * primary (1, name) it names a folder record with ID 2. That name is the label;
 * a name the display mapping refuses leaves it absent and does not block the
 * volume. A stored U+0000 reads as U+2400 by that mapping.
 */
static fsimg_status_t
open_root(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_hfsplus_reader_t *hfsplus = hfsplus_state(reader);
	hfsplus_key_t key;
	hfsplus_record_t record;
	hfsplus_thread_t thread;
	hfsplus_folder_t folder;
	fsimg_error_t refused;
	bool found;
	fsimg_status_t status;

	status = hfsplus_catalog_key_init(&key, HFSPLUS_ID_ROOT, NULL, 0,
	    error);
	if (status == FSIMG_OK)
		status = hfsplus_catalog_find_exact(&hfsplus->catalog, &key,
		    &record, &found, error);
	if (status != FSIMG_OK)
		return (status);
	if (!found)
		return (hfsplus_corrupt(error,
		    "HFS Plus root thread is missing"));
	if (!hfsplus_thread_decode(record.data, record.data_bytes, &thread) ||
	    thread.type != HFSPLUS_RECORD_FOLDER_THREAD ||
	    thread.parent != HFSPLUS_ID_ROOT_PARENT)
		return (hfsplus_corrupt(error,
		    "HFS Plus root thread is not a folder thread under the root's parent"));

	/* The key keeps the name for the label: this search overwrites the
	 * buffer the thread borrows it from.
	 */
	status = hfsplus_catalog_key_init(&key, HFSPLUS_ID_ROOT_PARENT,
	    thread.name, thread.units, error);
	if (status == FSIMG_OK)
		status = hfsplus_catalog_find_exact(&hfsplus->catalog, &key,
		    &record, &found, error);
	if (status != FSIMG_OK)
		return (status);
	if (!found)
		return (hfsplus_corrupt(error,
		    "HFS Plus root folder is missing"));
	if (!hfsplus_folder_decode(record.data, record.data_bytes, &folder) ||
	    folder.id != HFSPLUS_ID_ROOT)
		return (hfsplus_corrupt(error,
		    "HFS Plus root record is not folder 2"));
	status = hfsplus_root_validate(&folder, error);
	if (status != FSIMG_OK)
		return (status);
	hfsplus->root = folder;

	hfsplus->label_valid =
	    hfsplus_name_admit(key.catalog.name, key.catalog.units,
	    &refused) == FSIMG_OK &&
	    hfsplus_name_to_path(key.catalog.name, key.catalog.units,
	    hfsplus->label, &hfsplus->label_bytes, &refused) == FSIMG_OK;
	return (FSIMG_OK);
}

fsimg_status_t
hfsplus_volume_open(fsimg_reader_t *reader,
    const fsimg_format_variant_t *forced,
    fsimg_error_t *error)
{
	unsigned char prefix[HFSPLUS_PROBE_BYTES];
	hfsplus_mdb_t mdb;
	uint64_t embedded_bytes = 0;
	bool wrapper;
	size_t transferred;
	fsimg_status_t status;

	if (reader->slice_bytes < HFSPLUS_PROBE_BYTES)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0,
		    FSIMG_OP_READER,
		    "HFS Plus volume is shorter than its header"));
	status = fsimg_reader_bytes(reader, 0, prefix, sizeof(prefix),
	    &transferred, error);
	if (status != FSIMG_OK)
		return (status);
	status = check_identity(reader, forced, prefix, &mdb, &wrapper, error);
	if (status == FSIMG_OK && wrapper)
		status = enter_wrapper(reader, &mdb, prefix, &embedded_bytes, error);
	if (status == FSIMG_OK)
		status = derive_geometry(reader, prefix, embedded_bytes, error);
	if (status == FSIMG_OK)
		status = open_extents_tree(reader, error);
	if (status == FSIMG_OK)
		status = open_catalog_tree(reader, error);
	if (status == FSIMG_OK)
		status = open_remaining_forks(reader, error);
	if (status == FSIMG_OK)
		status = journal_open(reader, error);
	if (status == FSIMG_OK)
		status = open_root(reader, error);
	return (status);
}
