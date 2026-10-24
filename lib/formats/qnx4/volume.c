/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "qnx4.h"

/*
 * Opening a volume completes recognition in a fixed order, each step
 * failing before the next one reads:
 *
 * 1. geometry from the slice length alone;
 * 2. the root block's five records, zero ranges and label;
 * 3. the bitmap's bounded self-check, so every later walk can check
 *    allocation;
 * 4. claims of the loader, the root block and the bitmap;
 * 5. the root directory's slots, read without claiming (its prepare claims
 *    it like every other directory): the fixed ones, and the direct record
 *    naming .longfilenames;
 * 6. .inodes, claimed, with its signature and locator;
 * 7. .longfilenames, claimed, with its locator;
 * 8. lost+found, claimed, with its dot entries.
 */

/* What the scan of the root's slots found: .longfilenames' record and
 * its byte offset in the root directory.
 */
typedef struct root_scan {
	fsimg_reader_t	*reader;
	qnx4_inode_t	 names;
	uint64_t	 names_offset;
	bool		 names_found;
} root_scan_t;

typedef struct lost_scan {
	fsimg_reader_t	*reader;
	bool		 used;
} lost_scan_t;

static fsimg_status_t derive_geometry(fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t check_root_block(fsimg_reader_t *, fsimg_error_t *);
static void decode_label(fsimg_qnx4_reader_t *);
static fsimg_status_t root_slot(void *, uint64_t, uint64_t,
    const unsigned char *, fsimg_error_t *);
static fsimg_status_t scan_root(fsimg_reader_t *, root_scan_t *,
    fsimg_error_t *);
static fsimg_status_t open_inodes(fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t open_names(fsimg_reader_t *, const root_scan_t *,
    fsimg_error_t *);
static fsimg_status_t lost_slot(void *, uint64_t, uint64_t,
    const unsigned char *, fsimg_error_t *);
static fsimg_status_t open_lost_found(fsimg_reader_t *, fsimg_error_t *);

/* N is the slice length in blocks, B = ceil(N / 4096) bitmap blocks hold
 * its ceil(N / 8) bitmap bytes, and object storage lies past the bitmap.
 */
static fsimg_status_t
derive_geometry(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(reader);
	qnx4_geometry_t *g = &qnx4->geometry;
	uint64_t volume_bytes, blocks;
	fsimg_status_t status;

	status = fsimg_io_get_size(reader->input, &volume_bytes, error);
	if (status != FSIMG_OK)
		return (status);
	if (volume_bytes % QNX4_BLOCK_BYTES != 0)
		return (qnx4_corrupt(error,
		    "QNX4 volume length is not whole blocks"));
	blocks = volume_bytes / QNX4_BLOCK_BYTES;
	if (blocks < QNX4_BLOCKS_MIN || blocks > QNX4_BLOCKS_MAX)
		return (qnx4_corrupt(error,
		    "QNX4 volume block count is out of range"));
	g->volume_bytes = volume_bytes;
	g->blocks = (uint32_t)blocks;
	g->bitmap_blocks = (uint32_t)((blocks + QNX4_BITS_PER_BLOCK - 1) /
	    QNX4_BITS_PER_BLOCK);
	g->bitmap_bytes = (uint32_t)((blocks + 7) / 8);
	reader->content_domain = (fsimg_extent_domain_t){
		.offset = (2 + (uint64_t)g->bitmap_blocks) * QNX4_BLOCK_BYTES,
		.length = (blocks - 2 - g->bitmap_blocks) * QNX4_BLOCK_BYTES,
		.unit_bytes = QNX4_BLOCK_BYTES
	};
	return (FSIMG_OK);
}

/* The label is the bytes at 0x1c0 before the first NUL, or all 48. It is
 * valid when the bytes after that NUL are zero and its own bytes follow
 * the profile's byte rules. An invalid label never blocks reading; it is
 * reported absent.
 */
static void
decode_label(fsimg_qnx4_reader_t *qnx4)
{
	const unsigned char *field = qnx4->root_block + QNX4_LABEL_OFFSET;
	const unsigned char *nul;
	size_t length;

	nul = memchr(field, '\0', QNX4_LABEL_BYTES);
	length = nul == NULL ? QNX4_LABEL_BYTES : (size_t)(nul - field);
	qnx4->label_valid = qnx4_bytes_zero(field + length,
	    QNX4_LABEL_BYTES - length) && qnx4_label_valid(field, length);
	bzero(qnx4->label, sizeof(qnx4->label));
	if (qnx4->label_valid)
		bcopy(field, qnx4->label, length);
}

/* Block 2: records 0 to 4 -- the root directory, .inodes, .boot, .altboot
 * and lost+found -- with the statuses and kinds the formatter writes, zero
 * from 0x140 to 0x1bf and from 0x1f0, and the label between. The root
 * directory's first extent starts right after the bitmap.
 */
static fsimg_status_t
check_root_block(fsimg_reader_t *reader, fsimg_error_t *error)
{
	static const uint16_t kinds[QNX4_ROOT_RECORDS] = {
		QNX4_MODE_DIRECTORY, QNX4_MODE_FILE, QNX4_MODE_FILE,
		QNX4_MODE_FILE, QNX4_MODE_DIRECTORY
	};
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(reader);
	qnx4_inode_t *record;
	size_t transferred;
	fsimg_status_t status;

	status = fsimg_reader_bytes(reader, QNX4_BLOCK_BYTES, qnx4->root_block,
	    sizeof(qnx4->root_block), &transferred, error);
	if (status != FSIMG_OK)
		return (status);
	for (unsigned i = 0; i < QNX4_ROOT_RECORDS; i++)
		qnx4_inode_decode(qnx4->root_block + i * QNX4_RECORD_BYTES,
		    &qnx4->root[i]);

	record = &qnx4->root[QNX4_RECORD_ROOT];
	status = qnx4_root_block_identity(qnx4->root_block,
	    qnx4->geometry.volume_bytes, error);
	if (status != FSIMG_OK)
		return (status);
	for (unsigned i = 0; i < QNX4_ROOT_RECORDS; i++) {
		status = qnx4_inode_validate(&qnx4->root[i], QNX4_PLACE_ROOT,
		    error);
		if (status != FSIMG_OK)
			return (status);
		if ((qnx4->root[i].mode & QNX4_MODE_TYPE) != kinds[i])
			return (qnx4_corrupt(error,
			    "QNX4 root-block record has the wrong kind"));
	}
	if (!qnx4_bytes_zero(qnx4->root_block + 0x140, 0x80) ||
	    !qnx4_bytes_zero(qnx4->root_block + 0x1f0, 0x10))
		return (qnx4_unsupported(error,
		    "QNX4 root block has nonzero reserved bytes"));
	if (record->first_extent.first != 3 + qnx4->geometry.bitmap_blocks)
		return (qnx4_corrupt(error,
		    "QNX4 root directory does not follow the bitmap"));
	decode_label(qnx4);
	return (FSIMG_OK);
}

/* Check root slot `slot` of the fixed ones against the format: "." and ".."
 * naming the root record, the .bitmap direct record over the bitmap's
 * blocks, and the links .inodes, .boot and .altboot to root-block records
 * 1 to 3. Open checks them; the root's prepare checks them again.
 */
fsimg_status_t
qnx4_root_fixed_slot(fsimg_reader_t *reader, unsigned slot,
    const unsigned char *raw, fsimg_error_t *error)
{
	static const char *const links[] = { ".inodes", ".boot", ".altboot" };
	const qnx4_geometry_t *g = &qnx4_state(reader)->geometry;
	qnx4_inode_t bitmap;
	qnx4_link_t link;
	const char *name;
	uint64_t target;
	fsimg_status_t status;

	switch (slot) {
	case 0:
	case 1:
		status = qnx4_dot_target(raw, slot == 0, &target, error);
		if (status != FSIMG_OK)
			return (status);
		if (target != QNX4_POSITION(QNX4_ROOT_BLOCK, QNX4_RECORD_ROOT))
			return (qnx4_corrupt(error,
			    "QNX4 directory has a missing or wrong dot entry"));
		return (FSIMG_OK);
	case 2:
		if (qnx4_slot_free(raw) || qnx4_slot_link(raw))
			return (qnx4_corrupt(error, "QNX4 .bitmap record is missing"));
		qnx4_inode_decode(raw, &bitmap);
		status = qnx4_inode_validate(&bitmap, QNX4_PLACE_DIRECT, error);
		if (status != FSIMG_OK)
			return (status);
		if (bitmap.name_length != 7 || bcmp(bitmap.name, ".bitmap", 7) != 0 ||
		    !bitmap.name_padding_zero ||
		    (bitmap.mode & QNX4_MODE_TYPE) != QNX4_MODE_FILE ||
		    bitmap.extents != 1 ||
		    bitmap.first_extent.first != QNX4_BITMAP_FIRST ||
		    bitmap.first_extent.blocks != g->bitmap_blocks ||
		    bitmap.size != g->bitmap_bytes)
			return (qnx4_corrupt(error,
			    "QNX4 .bitmap record disagrees with the geometry"));
		return (FSIMG_OK);
	case 3:
	case 4:
	case 5:
		name = links[slot - 3];
		qnx4_link_decode(raw, &link);
		if (!qnx4_slot_link(raw) ||
		    link.name_length != strlen(name) ||
		    bcmp(link.name, name, link.name_length) != 0 ||
		    !link.name_padding_zero || link.long_block != 0 ||
		    link.target_block != QNX4_ROOT_BLOCK ||
		    link.target_slot != slot - 2)
			return (qnx4_corrupt(error,
			    "QNX4 root link to a root-block record is wrong"));
		if (link.status != QNX4_STATUS_LINK || !link.spare_zero)
			return (qnx4_unsupported(error,
			    "QNX4 link record has unexpected status or spare bytes"));
		return (FSIMG_OK);
	default:
		return (qnx4_corrupt(error, "QNX4 root slot is not a fixed one"));
	}
}

/* .inodes is root-block record 1: whole blocks of 64-byte records whose
 * slot 0 is the table's signature, the name "IamTHE.inodeFILE", the value
 * 0x00100000 and zeros.
 */
static fsimg_status_t
open_inodes(fsimg_reader_t *reader, fsimg_error_t *error)
{
	static const unsigned char signature[16] = {
		'I', 'a', 'm', 'T', 'H', 'E', '.', 'i', 'n', 'o', 'd', 'e', 'F',
		'I', 'L', 'E'
	};
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(reader);
	const qnx4_inode_t *record = &qnx4->root[QNX4_RECORD_INODES];
	unsigned char slot0[QNX4_RECORD_BYTES];
	fsimg_status_t status;

	if (record->size == 0 || record->size % QNX4_BLOCK_BYTES != 0)
		return (qnx4_corrupt(error, "QNX4 .inodes size is not whole blocks"));
	status = qnx4_index_build(reader, record, QNX4_WALK_EXCLUSIVE,
	    &qnx4->inodes, error);
	if (status != FSIMG_OK)
		return (status);
	status = qnx4_index_read(reader, qnx4->inodes, 0, slot0, sizeof(slot0),
	    error);
	if (status != FSIMG_OK)
		return (status);
	if (bcmp(slot0, signature, sizeof(signature)) != 0 ||
	    fsimg_load_le32(slot0 + 16) != UINT32_C(0x00100000) ||
	    !qnx4_bytes_zero(slot0 + 20, sizeof(slot0) - 20))
		return (qnx4_corrupt(error, "QNX4 .inodes signature is missing"));
	status = qnx4_locator_build(reader, qnx4->inodes, &qnx4->inodes_locator,
	    error);
	if (status != FSIMG_OK)
		return (status);
	qnx4->inode_slots = record->size / QNX4_RECORD_BYTES;
	return (fsimg_bitset_new(&reader->session, qnx4->inode_slots,
	    &qnx4->exposed, error));
}

/* One root slot at open: the fixed slots are checked against the format,
 * and beyond them the direct record named .longfilenames is found. A
 * second one, a link named .longfilenames, or any used slot named .bitmap
 * or .inodes, would be a second copy of a format-owned file. A link with a
 * long-name pointer has a name of 49 bytes or more, which is no
 * format-owned name, so it is skipped.
 */
static fsimg_status_t
root_slot(void *argument, uint64_t slot, uint64_t position FSIMG_UNUSED,
    const unsigned char *raw, fsimg_error_t *error)
{
	root_scan_t *scan = argument;
	size_t length;
	bool direct;

	if (slot < QNX4_ROOT_FIXED_SLOTS)
		return (qnx4_root_fixed_slot(scan->reader, (unsigned)slot, raw,
		    error));
	if (qnx4_slot_free(raw))
		return (FSIMG_OK);
	direct = !qnx4_slot_link(raw);
	if (!direct && fsimg_load_le32(raw + 0x35) != 0)
		return (FSIMG_OK);
	length = strnlen((const char *)raw, direct ? QNX4_DIRECT_NAME_MAX :
	    QNX4_LINK_NAME_MAX);
	if ((length == 7 && bcmp(raw, ".bitmap", 7) == 0) ||
	    (length == 7 && bcmp(raw, ".inodes", 7) == 0))
		return (qnx4_corrupt(error,
		    "QNX4 root names a format-owned file twice"));
	if (length != 14 || bcmp(raw, ".longfilenames", 14) != 0)
		return (FSIMG_OK);
	if (!direct || scan->names_found)
		return (qnx4_corrupt(error,
		    "QNX4 root names .longfilenames other than by one record"));
	scan->names_found = true;
	scan->names_offset = slot * QNX4_RECORD_BYTES;
	qnx4_inode_decode(raw, &scan->names);
	return (FSIMG_OK);
}

/* Read every root slot once without claiming the root's storage. */
static fsimg_status_t
scan_root(fsimg_reader_t *reader, root_scan_t *scan, fsimg_error_t *error)
{
	const qnx4_inode_t *root = &qnx4_state(reader)->root[QNX4_RECORD_ROOT];
	fsimg_extent_index_t *index;
	fsimg_status_t status;

	bzero(scan, sizeof(*scan));
	scan->reader = reader;
	status = qnx4_index_build(reader, root, QNX4_WALK_ALLOCATED, &index,
	    error);
	if (status != FSIMG_OK)
		return (status);
	status = qnx4_slots_scan(reader, index, root->size, root_slot, scan,
	    error);
	fsimg_extent_index_free(index);
	return (status);
}

/* .longfilenames: whole blocks of 512-byte long-name records, claimed, with
 * a locator for the long-name pointers of links. A volume formatted
 * without it has neither.
 */
static fsimg_status_t
open_names(fsimg_reader_t *reader, const root_scan_t *scan,
    fsimg_error_t *error)
{
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(reader);
	const qnx4_inode_t *record = &scan->names;
	fsimg_status_t status;

	if (!scan->names_found)
		return (FSIMG_OK);
	status = qnx4_inode_validate(record, QNX4_PLACE_DIRECT, error);
	if (status != FSIMG_OK)
		return (status);
	if (!record->name_padding_zero ||
	    (record->mode & QNX4_MODE_TYPE) != QNX4_MODE_FILE ||
	    record->size % QNX4_BLOCK_BYTES != 0)
		return (qnx4_corrupt(error,
		    "QNX4 .longfilenames is not whole long-name records"));
	status = qnx4_index_build(reader, record, QNX4_WALK_EXCLUSIVE,
	    &qnx4->names, error);
	if (status == FSIMG_OK && qnx4->names != NULL)
		status = qnx4_locator_build(reader, qnx4->names,
		    &qnx4->names_locator, error);
	if (status != FSIMG_OK)
		return (status);
	qnx4->has_names = true;
	qnx4->names_slot_offset = scan->names_offset;
	return (FSIMG_OK);
}

/* Every slot of lost+found past its dot entries is free on every observed
 * volume. A used one never blocks reading; its content is never exposed,
 * and full verification reports the volume incomplete.
 */
static fsimg_status_t
lost_slot(void *argument, uint64_t slot, uint64_t position FSIMG_UNUSED,
    const unsigned char *raw, fsimg_error_t *error)
{
	lost_scan_t *scan = argument;
	const qnx4_geometry_t *g = &qnx4_state(scan->reader)->geometry;
	uint64_t target;
	fsimg_status_t status;

	/* A slot both used and a link never gets here: the slot scan refuses
	 * it as corruption in every directory, lost+found included.
	 */
	if (slot >= 2) {
		if (!qnx4_slot_free(raw))
			scan->used = true;
		return (FSIMG_OK);
	}
	status = qnx4_dot_target(raw, slot == 0, &target, error);
	if (status != FSIMG_OK)
		return (status);
	/* "." names record 4. The formatter points ".." at the root's own
	 * "." slot, (3 + B, 0). The root record (2, 0), which every other
	 * child of the root names, is a plausible parent but no native
	 * lost+found has it, so it is unsupported rather than corrupt.
	 */
	if (slot == 0 && target != QNX4_POSITION(QNX4_ROOT_BLOCK,
	    QNX4_RECORD_LOST_FOUND))
		return (qnx4_corrupt(error,
		    "QNX4 directory has a missing or wrong dot entry"));
	if (slot == 1 && target == QNX4_POSITION(QNX4_ROOT_BLOCK,
	    QNX4_RECORD_ROOT))
		return (qnx4_unsupported(error,
		    "QNX4 lost+found names the root record as its parent"));
	if (slot == 1 && target != QNX4_POSITION(3 + g->bitmap_blocks, 0))
		return (qnx4_corrupt(error,
		    "QNX4 directory has a missing or wrong dot entry"));
	return (FSIMG_OK);
}

/* lost+found is reached only through root-block record 4: its storage is
 * claimed and its dot entries checked, and the index is released.
 */
static fsimg_status_t
open_lost_found(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(reader);
	const qnx4_inode_t *record = &qnx4->root[QNX4_RECORD_LOST_FOUND];
	fsimg_extent_index_t *index;
	lost_scan_t scan = { reader, false };
	fsimg_status_t status;

	status = qnx4_index_build(reader, record, QNX4_WALK_EXCLUSIVE, &index,
	    error);
	if (status != FSIMG_OK)
		return (status);
	status = qnx4_slots_scan(reader, index, record->size, lost_slot, &scan,
	    error);
	fsimg_extent_index_free(index);
	qnx4->lost_found_used = scan.used;
	return (status);
}

fsimg_status_t
qnx4_volume_open(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(reader);
	root_scan_t scan;
	bool claimed;
	fsimg_status_t status;

	status = derive_geometry(reader, error);
	if (status != FSIMG_OK)
		return (status);
	status = check_root_block(reader, error);
	if (status != FSIMG_OK)
		return (status);
	status = qnx4_bitmap_open(reader, error);
	if (status != FSIMG_OK)
		return (status);

	/* Bit b - 1 of the claimed set is block b, as in the bitmap itself;
	 * the loader, the root block and the bitmap are claimed at open.
	 */
	status = fsimg_bitset_new(&reader->session, qnx4->geometry.blocks,
	    &qnx4->claimed, error);
	if (status == FSIMG_OK)
		status = fsimg_bitset_test_and_set_range(qnx4->claimed, 0,
		    2 + (uint64_t)qnx4->geometry.bitmap_blocks, &claimed, error);
	if (status != FSIMG_OK)
		return (status);

	status = scan_root(reader, &scan, error);
	if (status == FSIMG_OK)
		status = open_inodes(reader, error);
	if (status == FSIMG_OK)
		status = open_names(reader, &scan, error);
	if (status == FSIMG_OK)
		status = open_lost_found(reader, error);
	return (status);
}
