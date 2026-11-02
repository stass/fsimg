/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "iso9660.h"

/*
 * Full verification (specs/design-iso9660-driver.md#verification). The
 * common walker walks every view the image offers, the selected one first
 * and counted, then rockridge, primary and joliet uncounted
 * (specs/design-directories.md#several-walks). iso_prepare reports each
 * directory it prepares and each later record of it through the two
 * iso_verify_ hooks below, which append to scratch streams:
 *
 * claims	(start byte, end byte, class, hierarchy) of every structure
 *		and data extent: the header's, and those of each hierarchy's
 *		physical walk, its continuation areas included;
 * paths	per hierarchy, (extent, parent's extent, identifier) of the
 *		root and of every directory record, from its physical walk;
 * placeholders	the extent of each directory the rockridge walk enters
 *		through CL;
 * relocated	the extent of each directory the primary walk finds
 *		carrying PL in its second dot record.
 *
 * A hierarchy's physical walk is the primary walk or the joliet walk,
 * which read every record of their hierarchy, so each record is reported
 * once. The rockridge walk is not physical: it skips each RE record and
 * with it the relocation directory producers mark with RE. A relocated
 * directory is one whose parent record carries PL; the primary walk has
 * already checked that record names its physical parent. The primary walk
 * also checks its directory record order as it goes.
 *
 * After the walks each stream is sorted externally and checked with
 * constant state: one sweep applies the sharing rules to the claims
 * (specs/design-iso9660.md#ownership); each hierarchy's path tables are
 * decoded and their sorted records must equal the walk's; and the two
 * relocation lists must be equal. Memory is the streams' charged scratch
 * and a few windows, whatever the image's size.
 */
#define VERIFY_BUFFER_BYTES	65536u
#define CLAIM_BYTES		18u	/* start, end (be64), class, hierarchy */
#define PATH_BYTES_MAX		(8u + 255u)	/* extent, parent (be32), name */
#define PATH_RECORD_MIN		10u	/* header, one identifier byte, pad */
#define PATH_RECORDS_MAX	65535u	/* parent numbers are 16 bits */
#define RELOCATION_BYTES	4u	/* directory (be32) */

#define HIERARCHY_PRIMARY	0u
#define HIERARCHY_JOLIET	1u
#define HIERARCHIES		2u

/* What a claim is, metadata first. Sorting by (start, end, class) puts the
 * boot catalog just before identical data claims, which may share it.
 */
typedef enum claim_class {
	CLAIM_SYSTEM_AREA,
	CLAIM_DESCRIPTORS,
	CLAIM_PATH_TABLE,
	CLAIM_DIRECTORY,
	CLAIM_CONTINUATION,
	CLAIM_CATALOG,
	CLAIM_DATA,
	CLAIM_CLASSES
} claim_class_t;

/* The failure when a claim of each class conflicts with an earlier one. */
static const char *const claim_overlap[CLAIM_CLASSES] = {
	[CLAIM_SYSTEM_AREA] = "ISO 9660 system area overlaps another structure",
	[CLAIM_DESCRIPTORS] =
	    "ISO 9660 volume descriptor set overlaps another structure",
	[CLAIM_PATH_TABLE] = "ISO 9660 path table overlaps another structure",
	[CLAIM_DIRECTORY] = "ISO 9660 directory overlaps another structure",
	[CLAIM_CONTINUATION] =
	    "SUSP continuation area overlaps another structure",
	[CLAIM_CATALOG] = "El Torito boot catalog overlaps another structure",
	[CLAIM_DATA] = "ISO 9660 file data overlaps a structure"
};

typedef struct claim {
	uint64_t	 start;		/* bytes from the volume's start */
	uint64_t	 end;
	unsigned	 class;
	unsigned	 hierarchy;
} claim_t;

/* A path table read in order through its own block window: its type-L or
 * type-M byte order and the position of its next byte.
 */
typedef struct path_table {
	iso_window_t	 window;
	uint32_t	 extent;
	uint32_t	 position;
	bool		 big_endian;
} path_table_t;

/* An identifier the order check remembers: length bytes, zero for none,
 * and whether it names a directory.
 */
typedef struct order_entry {
	bool		 directory;
	uint8_t		 length;
	unsigned char	 bytes[ISO_IDENTIFIER_MAX];
} order_entry_t;

/* Verification state behind the reader's verify pointer for the length of
 * fsimg_verify: the four streams; directories, the count of paths records
 * of each hierarchy; and, in the primary directory being prepared, the
 * last entry's identifier and the last one carrying a version. tables are
 * the two windows through which the path-table pass reads one table and
 * its partner: L and M in lockstep, then each table and its copy.
 */
struct iso_verify {
	path_table_t	 tables[2];
	fsimg_stream_t	*claims;
	fsimg_stream_t	*paths[HIERARCHIES];
	fsimg_stream_t	*placeholders;
	fsimg_stream_t	*relocated;
	uint64_t	 directories[HIERARCHIES];
	order_entry_t	 previous;
	order_entry_t	 versioned;
};

/* Every stream holds explicitly encoded big-endian records, so plain byte
 * order is the order wanted: claims by start, end and class, path and
 * relocation records by extent.
 */
static int
bytes_compare(const void *a, size_t a_length, const void *b, size_t b_length,
    void *argument FSIMG_UNUSED)
{
	int order;

	order = memcmp(a, b, a_length < b_length ? a_length : b_length);
	if (order != 0)
		return (order);
	return ((a_length > b_length) - (a_length < b_length));
}

/* Seal stream and sort it into *sorted. */
static fsimg_status_t
stream_sorted(fsimg_stream_t *stream, fsimg_stream_t **sorted,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	status = fsimg_stream_seal(stream, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_sort(stream, bytes_compare, NULL, NULL,
		    sorted, error);
	return (status);
}

/* Ownership claims. */

static fsimg_status_t
claim_add(iso_verify_t *verify, claim_class_t class, unsigned hierarchy,
    uint64_t start, uint64_t end, fsimg_error_t *error)
{
	unsigned char raw[CLAIM_BYTES];

	if (start == end)
		return (FSIMG_OK);
	fsimg_store_be64(raw, start);
	fsimg_store_be64(raw + 8, end);
	raw[16] = (unsigned char)class;
	raw[17] = (unsigned char)hierarchy;
	return (fsimg_stream_append(verify->claims, raw, sizeof(raw), NULL,
	    error));
}

/* Claim blocks whole blocks at block; both are below 2^33, so the byte
 * offsets cannot wrap.
 */
static fsimg_status_t
claim_blocks(iso_verify_t *verify, claim_class_t class,
    unsigned hierarchy, uint64_t block, uint64_t blocks, fsimg_error_t *error)
{
	return (claim_add(verify, class, hierarchy, block * ISO_BLOCK_BYTES,
	    (block + blocks) * ISO_BLOCK_BYTES, error));
}

/* A hierarchy's path tables and their optional copies, each the whole
 * blocks of the descriptor's path table size, which must lie within the
 * readable volume before path_tables_check reads them.
 */
static fsimg_status_t
path_table_claims(fsimg_reader_t *reader, const iso_descriptor_t *descriptor,
    unsigned hierarchy, fsimg_error_t *error)
{
	fsimg_iso9660_reader_t *iso = iso_state(reader);
	const uint32_t tables[] = {
		descriptor->path_table_l, descriptor->path_table_l_copy,
		descriptor->path_table_m, descriptor->path_table_m_copy
	};
	const bool optional[] = { false, true, false, true };
	uint64_t blocks = ISO_BLOCKS(descriptor->path_table_bytes);
	fsimg_status_t status;

	for (unsigned i = 0; i < sizeof(tables) / sizeof(tables[0]); i++) {
		if (optional[i] && tables[i] == 0)
			continue;
		status = iso_extent_check(iso, tables[i], blocks, error);
		if (status == FSIMG_OK)
			status = claim_blocks(iso->verify, CLAIM_PATH_TABLE,
			    hierarchy, tables[i], blocks, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/* What open found before the first directory: the system area, the
 * descriptor set through its terminator, each hierarchy's path tables, the
 * boot catalog, and each boot image's part within the volume, as data so
 * that a file may share it.
 */
static fsimg_status_t
header_claims(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_iso9660_reader_t *iso = iso_state(reader);
	iso_verify_t *verify = iso->verify;
	uint64_t start, end, size = iso->primary.volume_space_size;
	fsimg_status_t status;

	status = claim_blocks(verify, CLAIM_SYSTEM_AREA, 0, 0,
	    ISO_SYSTEM_BLOCKS, error);
	if (status == FSIMG_OK)
		status = claim_blocks(verify, CLAIM_DESCRIPTORS, 0,
		    ISO_SYSTEM_BLOCKS,
		    (uint64_t)iso->terminator_block + 1 - ISO_SYSTEM_BLOCKS,
		    error);
	if (status == FSIMG_OK)
		status = path_table_claims(reader, &iso->primary,
		    HIERARCHY_PRIMARY, error);
	if (status == FSIMG_OK && iso->joliet_level != 0)
		status = path_table_claims(reader, &iso->joliet,
		    HIERARCHY_JOLIET, error);
	if (status != FSIMG_OK || !iso->boot.present)
		return (status);
	status = claim_blocks(verify, CLAIM_CATALOG, 0, iso->boot.catalog_block,
	    1, error);
	for (unsigned i = 0; status == FSIMG_OK && i < iso->boot.entry_count;
	    i++) {
		start = iso->boot.entries[i].image_block;
		end = start +
		    iso_boot_image_blocks(iso->boot.entries[i].sector_count);
		if (end > size)
			end = size;
		if (start < end)
			status = claim_blocks(verify, CLAIM_DATA, 0, start,
			    end - start, error);
	}
	return (status);
}

static fsimg_status_t
claim_decode(const unsigned char *raw, size_t length, claim_t *claim,
    fsimg_error_t *error)
{
	if (length != CLAIM_BYTES)
		return (iso_corrupt(error, "invalid ISO 9660 claim record"));
	claim->start = fsimg_load_be64(raw);
	claim->end = fsimg_load_be64(raw + 8);
	claim->class = raw[16];
	claim->hierarchy = raw[17];
	if (claim->start >= claim->end || claim->class >= CLAIM_CLASSES)
		return (iso_corrupt(error, "invalid ISO 9660 claim record"));
	return (FSIMG_OK);
}

static bool
claim_identical(const claim_t *a, const claim_t *b)
{
	return (a->start == b->start && a->end == b->end);
}

/* Sweep the claims sorted by start, end and class, applying the sharing
 * rules with constant state: data_end, the furthest end of any data claim
 * seen, and metadata, the last metadata claim, which ends furthest since
 * the metadata claims seen so far never overlap. Data may overlap data; a
 * data claim overlaps open metadata only as the identical boot catalog,
 * which sorts just before it; a metadata claim overlaps nothing open. No
 * directory or path table is claimed twice: each physical walk prepares a
 * directory once, and the header claims each table once. Count the blocks
 * some claim covers.
 */
static fsimg_status_t
claims_sweep(fsimg_session_t *session, fsimg_stream_t *claims,
    uint64_t *allocated, fsimg_error_t *error)
{
	unsigned char raw[CLAIM_BYTES];
	claim_t claim, metadata;
	uint64_t cursor, data_end, covered, first, last;
	size_t length;
	bool open, have_metadata;
	fsimg_status_t status;

	*allocated = 0;
	cursor = data_end = covered = 0;
	have_metadata = false;
	for (;;) {
		status = fsimg_stream_next(claims, &cursor, raw, sizeof(raw),
		    &length, error);
		if (status == FSIMG_DONE)
			return (FSIMG_OK);
		if (status == FSIMG_OK)
			status = fsimg_session_charge(session, 0, 0, 1, error);
		if (status == FSIMG_OK)
			status = claim_decode(raw, length, &claim, error);
		if (status != FSIMG_OK)
			return (status);
		open = have_metadata && metadata.end > claim.start;
		if (claim.class == CLAIM_DATA) {
			if (open && !(metadata.class == CLAIM_CATALOG &&
			    claim_identical(&metadata, &claim)))
				return (iso_corrupt(error,
				    claim_overlap[claim.class]));
			if (claim.end > data_end)
				data_end = claim.end;
		} else {
			if (data_end > claim.start || open)
				return (iso_corrupt(error,
				    claim_overlap[claim.class]));
			metadata = claim;
			have_metadata = true;
		}
		first = claim.start / ISO_BLOCK_BYTES;
		last = ISO_BLOCKS(claim.end);
		if (first < covered)
			first = covered;
		if (last > first) {
			*allocated += last - first;
			covered = last;
		}
	}
}

/* Order. */

/* Whether identifier sorts strictly before entry, so may not follow it. */
static bool
order_descends(const order_entry_t *entry, const unsigned char *identifier,
    size_t length, bool directory)
{
	return (entry->length != 0 && iso_primary_order(entry->bytes,
	    entry->length, entry->directory, identifier, length, directory) > 0);
}

static void
order_remember(order_entry_t *entry, const unsigned char *identifier,
    uint8_t length, bool directory)
{
	bcopy(identifier, entry->bytes, length);
	entry->length = length;
	entry->directory = directory;
}

/* A primary directory's records after the dot records never descend. The
 * sections of one file repeat its identifier and compare equal. A
 * directory ties with the files of its name, so FOO.;1, FOO and FOO.;2
 * never descend pairwise; each record is therefore also compared with
 * the last one carrying a version, which keeps versions descending across
 * the tie. Versions descend only within one name and extension, and a
 * versioned record of an earlier name or extension always sorts before.
 */
static fsimg_status_t
order_check(iso_verify_t *verify, const iso_record_t *record,
    const unsigned char *identifier, fsimg_error_t *error)
{
	bool directory = (record->flags & ISO_FLAG_DIRECTORY) != 0;
	uint8_t length = record->identifier_length;

	if (order_descends(&verify->previous, identifier, length, directory) ||
	    order_descends(&verify->versioned, identifier, length, directory))
		return (iso_corrupt(error,
		    "ISO 9660 primary directory records are not sorted"));
	order_remember(&verify->previous, identifier, length, directory);
	if (iso_primary_versioned(identifier, length, directory))
		order_remember(&verify->versioned, identifier, length,
		    directory);
	return (FSIMG_OK);
}

/* The walk hooks. */

/* Append the path record of the directory at extent, named identifier in
 * the directory at parent.
 */
static fsimg_status_t
path_append(fsimg_stream_t *stream, uint32_t extent, uint32_t parent,
    const unsigned char *identifier, size_t length, fsimg_error_t *error)
{
	unsigned char raw[PATH_BYTES_MAX];

	fsimg_store_be32(raw, extent);
	fsimg_store_be32(raw + 4, parent);
	bcopy(identifier, raw + 8, length);
	return (fsimg_stream_append(stream, raw, 8 + length, NULL, error));
}

/* A physical walk found a directory of hierarchy. */
static fsimg_status_t
path_emit(iso_verify_t *verify, unsigned hierarchy, uint32_t extent,
    uint32_t parent, const unsigned char *identifier, size_t length,
    fsimg_error_t *error)
{
	verify->directories[hierarchy]++;
	return (path_append(verify->paths[hierarchy], extent, parent,
	    identifier, length, error));
}

static fsimg_status_t
relocation_note(fsimg_stream_t *stream, uint32_t directory,
    fsimg_error_t *error)
{
	unsigned char raw[RELOCATION_BYTES];

	fsimg_store_be32(raw, directory);
	return (fsimg_stream_append(stream, raw, sizeof(raw), NULL, error));
}

/* The walk in progress, which iso->view names: the hierarchy it walks, and
 * whether it is that hierarchy's physical walk rather than the rockridge
 * walk.
 */
static unsigned
walk_hierarchy(const fsimg_iso9660_reader_t *iso)
{
	return (iso->view == FSIMG_ISO_VIEW_JOLIET ? HIERARCHY_JOLIET :
	    HIERARCHY_PRIMARY);
}

static bool
walk_physical(const fsimg_iso9660_reader_t *iso)
{
	return (iso->view != FSIMG_ISO_VIEW_ROCKRIDGE);
}

/* Whether the walk in progress is the primary walk of a volume using
 * SUSP, which frames every record's SUSP stream. That covers every
 * continuation area the reader follows: the handshake's, from the root's
 * first dot record, whether or not Rock Ridge is present, and the
 * rockridge view's, from the records of the primary hierarchy; and those
 * of the subtrees RE hides from that view.
 */
static bool
walk_frames_susp(const fsimg_iso9660_reader_t *iso)
{
	return (iso->view == FSIMG_ISO_VIEW_PRIMARY && iso->susp);
}

/* Frame the SUSP stream of record, decoded from raw at offset of volume
 * block number block, which the primary view does not otherwise read:
 * claim its continuation areas, the record's own system use area lying in
 * its directory's claim, and tell in *parent_link whether it carries PL.
 */
static fsimg_status_t
susp_scan(fsimg_reader_t *reader, uint32_t block, uint32_t offset,
    const iso_record_t *record, const unsigned char *raw, bool *parent_link,
    fsimg_error_t *error)
{
	iso_verify_t *verify = iso_state(reader)->verify;
	const iso_area_t *area;
	iso_entry_t entry;
	iso_susp_t susp;
	uint64_t start;
	fsimg_status_t status;

	*parent_link = false;
	iso_susp_start(&susp, raw, block, offset, record);
	while ((status = iso_susp_next(reader, &susp, &entry, error)) ==
	    FSIMG_OK) {
		if (entry.signature[0] == 'P' && entry.signature[1] == 'L')
			*parent_link = true;
	}
	if (status != FSIMG_DONE)
		return (status);
	for (unsigned i = 1; i < susp.count; i++) {
		area = &susp.areas[i];
		start = (uint64_t)area->block * ISO_BLOCK_BYTES + area->offset;
		status = claim_add(verify, CLAIM_CONTINUATION, 0, start,
		    start + area->length, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/* Frame both dot records of the directory at extent, which preparation
 * has checked, and note the directory as relocated when its parent record
 * carries PL.
 */
static fsimg_status_t
dots_scan(fsimg_reader_t *reader, uint32_t extent, fsimg_error_t *error)
{
	fsimg_iso9660_reader_t *iso = iso_state(reader);
	iso_record_t self, parent;
	bool self_link, parent_link;
	fsimg_status_t status;

	/* The parent record follows the directory's own, at its length. */
	status = iso_block_read(reader, extent, error);
	if (status == FSIMG_OK)
		status = iso_record_decode(iso->window.bytes, 0, &self, error);
	if (status == FSIMG_OK)
		status = susp_scan(reader, extent, 0, &self, iso->window.bytes,
		    &self_link, error);
	if (status == FSIMG_OK)
		status = iso_record_decode(iso->window.bytes, self.length,
		    &parent, error);
	if (status == FSIMG_OK)
		status = susp_scan(reader, extent, self.length, &parent,
		    iso->window.bytes + self.length, &parent_link, error);
	if (status == FSIMG_OK && parent_link)
		status = relocation_note(iso->verify->relocated, extent, error);
	return (status);
}

/* iso_prepare checked the dot records of directory; relocated tells it
 * was entered through CL. The rockridge walk notes a relocated directory.
 * A physical walk claims the directory's blocks, starts its order, records
 * the root's path-table record, whose identifier is a zero byte, and in
 * the primary walk of a volume using SUSP frames its dot records.
 */
fsimg_status_t
iso_verify_directory(fsimg_reader_t *reader, const iso_cursor_t *directory,
    bool relocated, fsimg_error_t *error)
{
	static const unsigned char root_identifier = 0;
	fsimg_iso9660_reader_t *iso = iso_state(reader);
	iso_verify_t *verify = iso->verify;
	fsimg_status_t status;

	if (verify == NULL)
		return (FSIMG_OK);
	if (!walk_physical(iso))
		return (relocated ? relocation_note(verify->placeholders,
		    directory->extent, error) : FSIMG_OK);
	verify->previous.length = 0;
	verify->versioned.length = 0;
	status = claim_blocks(verify, CLAIM_DIRECTORY, walk_hierarchy(iso),
	    directory->extent, directory->blocks, error);
	if (status == FSIMG_OK &&
	    directory->extent == iso_hierarchy(iso)->root.extent)
		status = path_emit(verify, walk_hierarchy(iso),
		    directory->extent, directory->extent, &root_identifier, 1,
		    error);
	if (status == FSIMG_OK && walk_frames_susp(iso))
		status = dots_scan(reader, directory->extent, error);
	return (status);
}

/* What one entry's record owns: a directory record its path-table record,
 * a file section its data with its extended attribute record.
 */
static fsimg_status_t
record_claims(iso_verify_t *verify, unsigned hierarchy,
    const iso_cursor_t *directory, const iso_record_t *record,
    const unsigned char *raw, fsimg_error_t *error)
{
	if ((record->flags & ISO_FLAG_DIRECTORY) != 0)
		return (path_emit(verify, hierarchy, record->extent,
		    directory->extent, raw + ISO_RECORD_IDENTIFIER,
		    record->identifier_length, error));
	if (record->data_length == 0)
		return (FSIMG_OK);
	return (claim_blocks(verify, CLAIM_DATA, 0, record->extent,
	    record->ea_blocks + ISO_BLOCKS(record->data_length), error));
}

/* iso_prepare read record, an entry's, at the cursor's last position in
 * directory, and found it valid. A physical walk frames its SUSP stream
 * in the primary walk of a volume using SUSP, checks a primary
 * record's order, and records what the record owns.
 */
fsimg_status_t
iso_verify_record(fsimg_reader_t *reader, const iso_cursor_t *directory,
    const iso_record_t *record, const unsigned char *raw,
    fsimg_error_t *error)
{
	fsimg_iso9660_reader_t *iso = iso_state(reader);
	iso_verify_t *verify = iso->verify;
	bool parent_link;
	fsimg_status_t status = FSIMG_OK;

	if (verify == NULL || !walk_physical(iso))
		return (FSIMG_OK);
	if (walk_frames_susp(iso))
		status = susp_scan(reader, directory->record_block,
		    directory->record_offset, record, raw, &parent_link, error);
	if (status == FSIMG_OK && walk_hierarchy(iso) == HIERARCHY_PRIMARY)
		status = order_check(verify, record,
		    raw + ISO_RECORD_IDENTIFIER, error);
	if (status == FSIMG_OK)
		status = record_claims(verify, walk_hierarchy(iso), directory,
		    record, raw, error);
	return (status);
}

/* Path tables (specs/design-iso9660.md#path-tables). */

static void
table_start(path_table_t *table, uint32_t extent, bool big_endian)
{
	table->window.valid = false;
	table->extent = extent;
	table->position = 0;
	table->big_endian = big_endian;
}

/* Read length bytes at the table's position into out. The caller keeps
 * the read within the table's size, whose blocks header_claims checked.
 */
static fsimg_status_t
table_read(fsimg_reader_t *reader, path_table_t *table, unsigned char *out,
    size_t length, fsimg_error_t *error)
{
	uint64_t block;
	size_t offset, chunk;
	fsimg_status_t status;

	while (length > 0) {
		block = (uint64_t)table->extent +
		    table->position / ISO_BLOCK_BYTES;
		offset = table->position % ISO_BLOCK_BYTES;
		chunk = ISO_BLOCK_BYTES - offset;
		if (chunk > length)
			chunk = length;
		status = iso_window_read(reader, &table->window, block, error);
		if (status != FSIMG_OK)
			return (status);
		bcopy(table->window.bytes + offset, out, chunk);
		out += chunk;
		length -= chunk;
		table->position += (uint32_t)chunk;
	}
	return (FSIMG_OK);
}

/* Decode the record at the table's position, which must lie whole within
 * the size bytes the table holds; a record may cross a block. An odd
 * identifier is followed by a zero pad byte.
 */
static fsimg_status_t
path_record_read(fsimg_reader_t *reader, path_table_t *table, uint32_t size,
    iso_path_record_t *record, fsimg_error_t *error)
{
	unsigned char header[ISO_PATH_HEADER_BYTES], pad;
	fsimg_status_t status;

	if (size - table->position < sizeof(header))
		return (iso_corrupt(error,
		    "ISO 9660 path table ends inside a record"));
	status = table_read(reader, table, header, sizeof(header), error);
	if (status != FSIMG_OK)
		return (status);
	iso_path_header_decode(header, table->big_endian, record);
	if (record->length == 0)
		return (iso_corrupt(error,
		    "ISO 9660 path table record has no identifier"));
	if (size - table->position <
	    (uint32_t)record->length + (record->length & 1u))
		return (iso_corrupt(error,
		    "ISO 9660 path table ends inside a record"));
	status = table_read(reader, table, record->identifier, record->length,
	    error);
	if (status != FSIMG_OK || (record->length & 1u) == 0)
		return (status);
	status = table_read(reader, table, &pad, 1, error);
	if (status == FSIMG_OK && pad != 0)
		return (iso_corrupt(error,
		    "ISO 9660 path table pad byte is not zero"));
	return (status);
}

static bool
path_records_equal(const iso_path_record_t *a, const iso_path_record_t *b)
{
	return (a->length == b->length && a->ea_length == b->ea_length &&
	    a->extent == b->extent && a->parent == b->parent &&
	    bcmp(a->identifier, b->identifier, a->length) == 0);
}

/* The order rules of record number, after previous: a parent number below
 * its own; and in the primary hierarchy parent numbers that never decrease
 * and identifiers that never descend within one parent. Record 1 is the
 * root, its own parent, with the identifier 0x00.
 */
static fsimg_status_t
path_record_order(unsigned hierarchy, uint32_t number,
    const iso_path_record_t *previous, const iso_path_record_t *record,
    fsimg_error_t *error)
{
	if (number == 1) {
		if (record->length != 1 || record->identifier[0] != 0 ||
		    record->parent != 1)
			return (iso_corrupt(error,
			    "ISO 9660 path table does not begin with the root"));
		return (FSIMG_OK);
	}
	if (record->parent == 0 || record->parent >= number)
		return (iso_corrupt(error,
		    "ISO 9660 path table record's parent does not precede it"));
	if (hierarchy != HIERARCHY_PRIMARY || number == 2)
		return (FSIMG_OK);
	if (record->parent < previous->parent ||
	    (record->parent == previous->parent &&
	    iso_primary_order(previous->identifier, previous->length, true,
	    record->identifier, record->length, true) > 0))
		return (iso_corrupt(error,
		    "ISO 9660 primary path table is not sorted"));
	return (FSIMG_OK);
}

/* An optional copy, when present, equals its table's size bytes. */
static fsimg_status_t
path_copy_check(fsimg_reader_t *reader, uint32_t table, uint32_t copy,
    uint32_t size, fsimg_error_t *error)
{
	path_table_t *tables = iso_state(reader)->verify->tables;
	uint32_t length;
	fsimg_status_t status;

	if (copy == 0)
		return (FSIMG_OK);
	table_start(&tables[0], table, false);
	table_start(&tables[1], copy, false);
	for (uint64_t block = 0; block < ISO_BLOCKS(size); block++) {
		status = iso_window_read(reader, &tables[0].window,
		    table + block, error);
		if (status == FSIMG_OK)
			status = iso_window_read(reader, &tables[1].window,
			    copy + block, error);
		if (status != FSIMG_OK)
			return (status);
		length = size - (uint32_t)block * ISO_BLOCK_BYTES;
		if (length > ISO_BLOCK_BYTES)
			length = ISO_BLOCK_BYTES;
		if (bcmp(tables[0].window.bytes, tables[1].window.bytes,
		    length) != 0)
			return (iso_corrupt(error,
			    "ISO 9660 path table copy differs from its table"));
	}
	return (FSIMG_OK);
}

/* Whether two sorted streams hold the same records, failing with message
 * when they differ.
 */
static fsimg_status_t
streams_equal(fsimg_session_t *session, fsimg_stream_t *a, fsimg_stream_t *b,
    const char *message, fsimg_error_t *error)
{
	unsigned char ra[PATH_BYTES_MAX], rb[PATH_BYTES_MAX];
	uint64_t ca, cb;
	size_t la, lb;
	fsimg_status_t sa, sb;

	ca = cb = 0;
	for (;;) {
		sa = fsimg_stream_next(a, &ca, ra, sizeof(ra), &la, error);
		if (sa != FSIMG_OK && sa != FSIMG_DONE)
			return (sa);
		sb = fsimg_stream_next(b, &cb, rb, sizeof(rb), &lb, error);
		if (sb != FSIMG_OK && sb != FSIMG_DONE)
			return (sb);
		if (sa == FSIMG_DONE && sb == FSIMG_DONE)
			return (FSIMG_OK);
		if (sa != sb || la != lb || bcmp(ra, rb, la) != 0)
			return (iso_corrupt(error, message));
		sa = fsimg_session_charge(session, 0, 0, 1, error);
		if (sa != FSIMG_OK)
			return (sa);
	}
}

/* Decode the type-L and type-M tables in lockstep into records, each
 * (extent, parent's extent, identifier) as the walk records it. Each
 * record's parent number precedes it, so its parent's extent is already
 * in parents, a paged array indexed by record number.
 */
static fsimg_status_t
path_tables_decode(fsimg_reader_t *reader, const iso_descriptor_t *descriptor,
    unsigned hierarchy, fsimg_array_t *parents, fsimg_stream_t *records,
    fsimg_error_t *error)
{
	iso_verify_t *verify = iso_state(reader)->verify;
	iso_path_record_t l, m, previous;
	unsigned char raw[4];
	uint32_t number, parent_extent;
	fsimg_status_t status;

	table_start(&verify->tables[0], descriptor->path_table_l, false);
	table_start(&verify->tables[1], descriptor->path_table_m, true);
	bzero(&previous, sizeof(previous));
	for (number = 1; verify->tables[0].position <
	    descriptor->path_table_bytes; number++) {
		/* The walk found at most this many directories. */
		if (number > PATH_RECORDS_MAX)
			return (iso_corrupt(error, "ISO 9660 path table does "
			    "not match the directories"));
		status = path_record_read(reader, &verify->tables[0],
		    descriptor->path_table_bytes, &l, error);
		if (status == FSIMG_OK)
			status = path_record_read(reader, &verify->tables[1],
			    descriptor->path_table_bytes, &m, error);
		if (status != FSIMG_OK)
			return (status);
		if (!path_records_equal(&l, &m))
			return (iso_corrupt(error,
			    "ISO 9660 type L and M path tables differ"));
		if (l.ea_length != 0)
			return (iso_unsupported(error, "ISO 9660 directory has "
			    "an extended attribute record"));
		status = path_record_order(hierarchy, number, &previous, &l,
		    error);
		if (status != FSIMG_OK)
			return (status);
		parent_extent = l.extent;
		if (number > 1) {
			status = fsimg_array_get(parents, l.parent - 1u, raw,
			    error);
			if (status != FSIMG_OK)
				return (status);
			parent_extent = fsimg_load_be32(raw);
		}
		fsimg_store_be32(raw, l.extent);
		status = fsimg_array_set(parents, number - 1u, raw, error);
		if (status == FSIMG_OK)
			status = path_append(records, l.extent, parent_extent,
			    l.identifier, l.length, error);
		if (status != FSIMG_OK)
			return (status);
		previous = l;
	}
	return (FSIMG_OK);
}

/* Check one hierarchy's path tables: decoded in lockstep, their copies
 * byte for byte, and their records, sorted by extent, equal to the
 * records its physical walk emitted for every directory record. A
 * hierarchy of more than 65535 directories cannot be numbered.
 */
static fsimg_status_t
path_tables_check(fsimg_reader_t *reader, unsigned hierarchy,
    fsimg_error_t *error)
{
	fsimg_iso9660_reader_t *iso = iso_state(reader);
	iso_verify_t *verify = iso->verify;
	const iso_descriptor_t *descriptor = hierarchy == HIERARCHY_JOLIET ?
	    &iso->joliet : &iso->primary;
	uint32_t size = descriptor->path_table_bytes;
	fsimg_stream_t *records, *sorted, *walked;
	fsimg_array_t *parents;
	uint64_t count;
	fsimg_status_t status;

	if (verify->directories[hierarchy] > PATH_RECORDS_MAX)
		return (iso_unsupported(error,
		    "ISO 9660 hierarchy has more than 65535 directories"));
	if (size < PATH_RECORD_MIN)
		return (iso_corrupt(error,
		    "ISO 9660 path table is shorter than its root record"));
	/* Each record is at least PATH_RECORD_MIN bytes. */
	count = size / PATH_RECORD_MIN;
	if (count > PATH_RECORDS_MAX)
		count = PATH_RECORDS_MAX;
	records = sorted = walked = NULL;
	parents = NULL;
	status = fsimg_array_new(&reader->session, 4, count, &parents, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_new(&reader->session, PATH_BYTES_MAX,
		    &records, error);
	if (status == FSIMG_OK)
		status = path_tables_decode(reader, descriptor, hierarchy,
		    parents, records, error);
	if (status == FSIMG_OK)
		status = path_copy_check(reader, descriptor->path_table_l,
		    descriptor->path_table_l_copy, size, error);
	if (status == FSIMG_OK)
		status = path_copy_check(reader, descriptor->path_table_m,
		    descriptor->path_table_m_copy, size, error);
	if (status == FSIMG_OK)
		status = stream_sorted(records, &sorted, error);
	if (status == FSIMG_OK)
		status = stream_sorted(verify->paths[hierarchy], &walked,
		    error);
	if (status == FSIMG_OK)
		status = streams_equal(&reader->session, sorted, walked,
		    "ISO 9660 path table does not match the directories",
		    error);
	fsimg_array_free(parents);
	fsimg_stream_free(records);
	fsimg_stream_free(sorted);
	fsimg_stream_free(walked);
	return (status);
}

/* Relocation (specs/design-iso9660.md#relocation): the directories the
 * primary walk found carrying PL are exactly those the rockridge walk
 * entered through CL. Neither list repeats a directory: each walk prepares
 * a directory once.
 */
static fsimg_status_t
relocation_match(fsimg_reader_t *reader, fsimg_error_t *error)
{
	iso_verify_t *verify = iso_state(reader)->verify;
	fsimg_stream_t *placeholders, *relocated;
	fsimg_status_t status;

	placeholders = relocated = NULL;
	status = stream_sorted(verify->placeholders, &placeholders, error);
	if (status == FSIMG_OK)
		status = stream_sorted(verify->relocated, &relocated, error);
	if (status == FSIMG_OK)
		status = streams_equal(&reader->session, placeholders,
		    relocated, "Rock Ridge relocated directories and CL "
		    "entries do not match", error);
	fsimg_stream_free(placeholders);
	fsimg_stream_free(relocated);
	return (status);
}

/* The walks. */

static bool
view_offered(const fsimg_iso9660_reader_t *iso, fsimg_iso_view_t view)
{
	switch (view) {
	case FSIMG_ISO_VIEW_ROCKRIDGE:
		return (iso->rockridge == ISO_ROCKRIDGE_PRESENT);
	case FSIMG_ISO_VIEW_JOLIET:
		return (iso->joliet_level != 0);
	case FSIMG_ISO_VIEW_PRIMARY:
		return (true);
	case FSIMG_ISO_VIEW_AUTO:
		break;
	}
	return (false);
}

/* Walk the tree of the view iso->view names to its end, accounting its
 * entries, which only the counted walk counts, and reading the payload of
 * every file and link when buffer is set.
 */
static fsimg_status_t
walk(fsimg_reader_t *reader, unsigned char *buffer,
    fsimg_verify_report_t *report, fsimg_error_t *error)
{
	fsimg_status_t status;

	for (;;) {
		status = fsimg_walk_next(reader, error);
		if (status == FSIMG_DONE)
			return (FSIMG_OK);
		if (status == FSIMG_OK)
			status = fsimg_verify_account(reader, report, error);
		if (status == FSIMG_OK && buffer != NULL &&
		    reader->entry.kind != FSIMG_ENTRY_DIRECTORY)
			status = fsimg_verify_payload(reader, buffer,
			    VERIFY_BUFFER_BYTES, report, error);
		if (status != FSIMG_OK)
			return (status);
	}
}

/* Walk every offered view: the selected one first, then rockridge,
 * primary and joliet, each but the first after a restart. iso->view names
 * the walk in progress to the hooks.
 */
static fsimg_status_t
walks(fsimg_reader_t *reader, unsigned char *buffer,
    fsimg_verify_report_t *report, fsimg_error_t *error)
{
	static const fsimg_iso_view_t later[] = {
		FSIMG_ISO_VIEW_ROCKRIDGE, FSIMG_ISO_VIEW_PRIMARY,
		FSIMG_ISO_VIEW_JOLIET
	};
	fsimg_iso9660_reader_t *iso = iso_state(reader);
	fsimg_iso_view_t plan[1 + sizeof(later) / sizeof(later[0])];
	unsigned count = 0;
	fsimg_status_t status;

	plan[count++] = iso->view;
	for (unsigned i = 0; i < sizeof(later) / sizeof(later[0]); i++) {
		if (later[i] != iso->view && view_offered(iso, later[i]))
			plan[count++] = later[i];
	}
	for (unsigned i = 0; i < count; i++) {
		if (i > 0) {
			status = fsimg_walk_restart(reader, error);
			if (status != FSIMG_OK)
				return (status);
		}
		iso->view = plan[i];
		status = walk(reader, buffer, report, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

static fsimg_status_t
verify_new(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_iso9660_reader_t *iso = iso_state(reader);
	iso_verify_t *verify;
	fsimg_session_t *session = &reader->session;
	fsimg_error_t detail;
	fsimg_status_t status;

	verify = fsimg_alloc(reader->ctx, sizeof(*verify), &detail);
	if (verify == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	bzero(verify, sizeof(*verify));
	iso->verify = verify;
	status = fsimg_stream_new(session, CLAIM_BYTES, &verify->claims, error);
	for (unsigned i = 0; status == FSIMG_OK && i < HIERARCHIES; i++)
		status = fsimg_stream_new(session, PATH_BYTES_MAX,
		    &verify->paths[i], error);
	if (status == FSIMG_OK)
		status = fsimg_stream_new(session, RELOCATION_BYTES,
		    &verify->placeholders, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_new(session, RELOCATION_BYTES,
		    &verify->relocated, error);
	return (status);
}

static void
verify_free(fsimg_iso9660_reader_t *iso)
{
	iso_verify_t *verify = iso->verify;

	if (verify == NULL)
		return;
	fsimg_stream_free(verify->claims);
	for (unsigned i = 0; i < HIERARCHIES; i++)
		fsimg_stream_free(verify->paths[i]);
	fsimg_stream_free(verify->placeholders);
	fsimg_stream_free(verify->relocated);
	fsimg_dealloc(verify);
	iso->verify = NULL;
}

/* Full verification: the handshake when open skipped it, the header
 * claims, the walks, then the claim sweep, each hierarchy's path tables
 * and the relocation match. The report counts the counted walk's entries
 * and every walk's payload bytes; allocated units are the blocks some
 * claim covers, and no block is free or bad. The selected view is restored
 * on every exit.
 */
fsimg_status_t
iso_verify(fsimg_reader_t *reader, unsigned flags,
    fsimg_verify_report_t *report, fsimg_error_t *error)
{
	fsimg_iso9660_reader_t *iso = iso_state(reader);
	fsimg_iso_view_t selected = iso->view;
	fsimg_stream_t *claims;
	unsigned char *buffer;
	fsimg_error_t detail;
	fsimg_status_t status;

	claims = NULL;
	buffer = NULL;
	reader->session.progress.phase = "ISO 9660 verification";
	status = FSIMG_OK;
	if (iso->rockridge == ISO_ROCKRIDGE_UNKNOWN)
		status = iso_handshake(reader, error);
	/* The SUSP cursor reads no stream behind skipped bytes, so neither
	 * Rock Ridge nor any continuation area could be verified.
	 */
	if (status == FSIMG_OK && iso->susp_skip != 0)
		status = iso_unsupported(error,
		    "ISO 9660 SUSP has an unsupported SP skip count");
	if (status == FSIMG_OK)
		status = verify_new(reader, error);
	if (status == FSIMG_OK)
		status = header_claims(reader, error);
	if (status == FSIMG_OK && (flags & FSIMG_VERIFY_PAYLOAD) != 0) {
		buffer = fsimg_alloc(reader->ctx, VERIFY_BUFFER_BYTES, &detail);
		if (buffer == NULL) {
			status = detail.status;
			if (error != NULL)
				*error = detail;
		}
	}
	if (status != FSIMG_OK)
		goto done;

	report->directories = 1;
	status = walks(reader, buffer, report, error);
	if (status == FSIMG_OK)
		status = stream_sorted(iso->verify->claims, &claims, error);
	if (status == FSIMG_OK)
		status = claims_sweep(&reader->session, claims,
		    &report->allocated_clusters, error);
	if (status == FSIMG_OK)
		status = path_tables_check(reader, HIERARCHY_PRIMARY, error);
	if (status == FSIMG_OK && iso->joliet_level != 0)
		status = path_tables_check(reader, HIERARCHY_JOLIET, error);
	if (status == FSIMG_OK)
		status = relocation_match(reader, error);
	if (status == FSIMG_OK)
		status = fsimg_reader_stable(reader, error);
done:
	iso->view = selected;
	fsimg_stream_free(claims);
	verify_free(iso);
	fsimg_dealloc(buffer);
	return (status);
}
