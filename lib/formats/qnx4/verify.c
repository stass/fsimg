/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include "fsimg-endian.h"
#include "qnx4.h"

/*
 * Full verification. One traversal reaches every object through the used
 * slots of every directory, and three facts the traversal establishes make
 * the reconciliation short:
 *
 * Ownership. Every object's storage, extent blocks included, is walked
 * under the exclusive policy exactly once -- .inodes, .longfilenames and
 * lost+found at open, each directory when it is entered, every other
 * object under its first name -- and the loader, root block and bitmap are
 * claimed at open. A block claimed twice already failed, and a claim checks
 * the bitmap first, so after the traversal the claimed set is exactly the
 * owned blocks. Allocation is then one comparison of the claimed set with
 * the bitmap, byte for byte: an allocated block outside it is an orphan.
 * The argument fails safe: an object some walk missed leaves its blocks
 * allocated but unclaimed, so a missing participant makes verification
 * fail, never pass.
 *
 * Names. A directory's link count is 2 plus its subdirectories, which its
 * own slot scan counts (the root's include lost+found), so it is checked
 * when the directory is prepared. A direct or root-block record has the
 * one name in the slot that holds or links it, so a non-directory there
 * must count 1. Only an .inodes object can have several names: each link
 * record's target slot is appended to a stream, which is sorted and merged
 * with one pass over .inodes. That pass checks every used slot but the
 * signature against its references -- one at least, and for a
 * non-directory exactly its link count -- and ignores the bytes of free
 * slots. A link to a free slot already failed at resolution.
 *
 * Long names. Decoding a link's long name checks the record's state and
 * back-reference and marks the record named; one pass over .longfilenames
 * then requires every used record to be marked.
 *
 * A volume whose label is invalid or whose lost+found holds a used slot
 * cannot be verified completely, and fails with FSIMG_EUNSUPPORTED before
 * any work; an unsupported object found by the traversal fails it the
 * same way, so a report is never complete after skipping anything.
 *
 * A few checks below cannot fail on a volume that stays unchanged during
 * verification, since an earlier check already holds them (a reference to
 * slot 0 or to a free slot, one past the table, a long-name record named
 * twice); they are kept as cheap guards of those invariants.
 */

#define REFERENCE_BYTES	4u	/* a logical .inodes slot, u32 */
#define PAYLOAD_BYTES	65536u

static fsimg_status_t check_scope(fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t start_accounting(fsimg_reader_t *, fsimg_error_t *);
static int reference_compare(const void *, size_t, const void *, size_t,
    void *);
static fsimg_status_t reference_next(fsimg_stream_t *, uint64_t *,
    uint64_t *, fsimg_error_t *);
static fsimg_status_t check_table_slot(uint64_t, const unsigned char *,
    uint64_t, fsimg_error_t *);
static fsimg_status_t verify_inodes(fsimg_reader_t *, fsimg_stream_t *,
    fsimg_error_t *);
static fsimg_status_t verify_long_names(fsimg_reader_t *, fsimg_error_t *);
static unsigned bits_in(unsigned);
static fsimg_status_t verify_allocation(fsimg_reader_t *,
    fsimg_verify_report_t *, fsimg_error_t *);
static fsimg_status_t traverse(fsimg_reader_t *, unsigned char *,
    fsimg_verify_report_t *, fsimg_error_t *);

/* Account one name of the object whose record a used slot holds or links,
 * found at place. Outside verification there is nothing to account.
 */
fsimg_status_t
qnx4_verify_name(fsimg_reader_t *reader, const qnx4_inode_t *object,
    qnx4_place_t place, uint64_t table_slot, fsimg_error_t *error)
{
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(reader);
	unsigned char record[REFERENCE_BYTES];

	if (qnx4->references == NULL)
		return (FSIMG_OK);
	if (place == QNX4_PLACE_TABLE) {
		/* open bounded the table to 2^31 bytes, 2^25 slots. */
		fsimg_store_le32(record, (uint32_t)table_slot);
		return (fsimg_stream_append(qnx4->references, record,
		    sizeof(record), NULL, error));
	}
	/* A directory's count is checked when it is prepared. */
	if ((object->mode & QNX4_MODE_TYPE) != QNX4_MODE_DIRECTORY &&
	    object->links != 1)
		return (qnx4_corrupt(error,
		    "QNX4 link count disagrees with the object's names"));
	return (FSIMG_OK);
}

/* Check a prepared directory's link count: its name, its "." and the ".."
 * of each subdirectory.
 */
fsimg_status_t
qnx4_verify_directory(fsimg_reader_t *reader, const qnx4_inode_t *directory,
    uint64_t subdirectories, fsimg_error_t *error)
{
	if (qnx4_state(reader)->references == NULL)
		return (FSIMG_OK);
	if (directory->links != 2 + subdirectories)
		return (qnx4_corrupt(error,
		    "QNX4 directory link count disagrees with its subdirectories"));
	return (FSIMG_OK);
}

/* Refuse a volume that cannot be verified completely: an invalid label
 * has no observed form to check, and the objects a used lost+found slot
 * would recover are never walked, so their blocks could not be owned.
 */
static fsimg_status_t
check_scope(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(reader);

	if (!qnx4->label_valid)
		return (qnx4_unsupported(error,
		    "QNX4 label is invalid; verification is incomplete"));
	if (qnx4->lost_found_used)
		return (qnx4_unsupported(error,
		    "QNX4 lost+found is not empty; verification is incomplete"));
	return (FSIMG_OK);
}

/* Create what the traversal accounts names into. The reference stream's
 * existence is what makes directory preparation account names; the
 * long-name bitset exists only when .longfilenames has records.
 */
static fsimg_status_t
start_accounting(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(reader);
	fsimg_status_t status;

	status = fsimg_stream_new(&reader->session, REFERENCE_BYTES,
	    &qnx4->references, error);
	if (status != FSIMG_OK)
		return (status);
	if (qnx4->names == NULL)
		return (FSIMG_OK);
	return (fsimg_bitset_new(&reader->session,
	    fsimg_extent_index_length(qnx4->names) / QNX4_BLOCK_BYTES,
	    &qnx4->long_named, error));
}

static int
reference_compare(const void *a, size_t alen, const void *b, size_t blen,
    void *argument FSIMG_UNUSED)
{
	uint32_t left, right;

	if (alen != REFERENCE_BYTES || blen != REFERENCE_BYTES)
		return ((alen > blen) - (alen < blen));
	left = fsimg_load_le32(a);
	right = fsimg_load_le32(b);
	return ((left > right) - (left < right));
}

/* The next referenced slot in the sorted stream, or UINT64_MAX when the
 * stream is exhausted; slot 0, the signature, is never a reference.
 */
static fsimg_status_t
reference_next(fsimg_stream_t *sorted, uint64_t *cursor, uint64_t *next,
    fsimg_error_t *error)
{
	unsigned char record[REFERENCE_BYTES];
	size_t length;
	fsimg_status_t status;

	*next = UINT64_MAX;
	status = fsimg_stream_next(sorted, cursor, record, sizeof(record),
	    &length, error);
	if (status == FSIMG_DONE)
		return (FSIMG_OK);
	if (status != FSIMG_OK)
		return (status);
	if (length != sizeof(record) || fsimg_load_le32(record) == 0)
		return (qnx4_corrupt(error, "invalid QNX4 reference record"));
	*next = fsimg_load_le32(record);
	return (FSIMG_OK);
}

/* Check .inodes slot `slot`, whose 64 bytes are raw, against the names
 * that link to it. Each used slot but 0 must be named; a non-directory
 * must have exactly as many names as its link count, while a directory's
 * single name is guaranteed by the claim made when it is entered and its
 * count was checked then. A free slot's bytes carry no meaning.
 */
static fsimg_status_t
check_table_slot(uint64_t slot, const unsigned char *raw, uint64_t names,
    fsimg_error_t *error)
{
	qnx4_inode_t record;
	bool directory;

	/* Slot 0 is the signature, never a record. */
	qnx4_inode_decode(raw, &record);
	if (slot == 0 || (record.status & QNX4_STATUS_USED) == 0) {
		if (names != 0)
			return (qnx4_corrupt(error,
			    "QNX4 link names a free .inodes slot"));
		return (FSIMG_OK);
	}
	if (names == 0)
		return (qnx4_corrupt(error, "QNX4 used .inodes slot has no name"));
	directory = (record.mode & QNX4_MODE_TYPE) == QNX4_MODE_DIRECTORY;
	if (!directory && record.links != names)
		return (qnx4_corrupt(error,
		    "QNX4 link count disagrees with the object's names"));
	return (FSIMG_OK);
}

/* Merge the sorted references with one pass over .inodes in 4 KiB
 * windows, one slot at a time. Counting a slot's references needs no work
 * charge of its own: each reference is a link slot the traversal charged.
 */
static fsimg_status_t
verify_inodes(fsimg_reader_t *reader, fsimg_stream_t *sorted,
    fsimg_error_t *error)
{
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(reader);
	unsigned char window[QNX4_WINDOW_BYTES];
	uint64_t cursor, next, names, table_bytes, offset, slot;
	size_t batch;
	fsimg_status_t status;

	cursor = 0;
	status = reference_next(sorted, &cursor, &next, error);
	if (status != FSIMG_OK)
		return (status);
	table_bytes = qnx4->inode_slots * QNX4_RECORD_BYTES;
	for (offset = 0; offset < table_bytes; offset += batch) {
		batch = table_bytes - offset > sizeof(window) ? sizeof(window) :
		    (size_t)(table_bytes - offset);
		status = qnx4_index_read(reader, qnx4->inodes, offset, window,
		    batch, error);
		if (status != FSIMG_OK)
			return (status);
		status = fsimg_session_charge(&reader->session, 0, 0,
		    batch / QNX4_RECORD_BYTES, error);
		if (status != FSIMG_OK)
			return (status);

		for (size_t w = 0; w < batch; w += QNX4_RECORD_BYTES) {
			slot = (offset + w) / QNX4_RECORD_BYTES;
			names = 0;
			while (next == slot) {
				names++;
				status = reference_next(sorted, &cursor, &next,
				    error);
				if (status != FSIMG_OK)
					return (status);
			}

			status = check_table_slot(slot, window + w, names,
			    error);
			if (status != FSIMG_OK)
				return (status);
		}
	}
	/* Every reference lies below the table's slot count. */
	if (next != UINT64_MAX)
		return (qnx4_corrupt(error, "QNX4 reference exceeds .inodes"));
	return (FSIMG_OK);
}

/* One pass over .longfilenames: every record in state 0x01 must have been
 * named by the link its back-reference designates. Other records are
 * unused and their bytes are ignored.
 */
static fsimg_status_t
verify_long_names(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(reader);
	unsigned char window[QNX4_WINDOW_BYTES];
	qnx4_longname_t record;
	uint64_t offset, length;
	size_t batch;
	bool named;
	fsimg_status_t status;

	if (qnx4->long_named == NULL)
		return (FSIMG_OK);
	length = fsimg_extent_index_length(qnx4->names);
	for (offset = 0; offset < length; offset += batch) {
		batch = length - offset > sizeof(window) ? sizeof(window) :
		    (size_t)(length - offset);
		status = qnx4_index_read(reader, qnx4->names, offset, window,
		    batch, error);
		if (status != FSIMG_OK)
			return (status);
		status = fsimg_session_charge(&reader->session, 0, 0,
		    batch / QNX4_BLOCK_BYTES, error);
		if (status != FSIMG_OK)
			return (status);
		for (size_t w = 0; w < batch; w += QNX4_BLOCK_BYTES) {
			qnx4_longname_decode(window + w, &record);
			if (record.state != 0x01)
				continue;
			status = fsimg_bitset_test(qnx4->long_named,
			    (offset + w) / QNX4_BLOCK_BYTES, &named, error);
			if (status != FSIMG_OK)
				return (status);
			if (!named)
				return (qnx4_corrupt(error,
				    "QNX4 used long-name record names no link"));
		}
	}
	return (FSIMG_OK);
}

static unsigned
bits_in(unsigned byte)
{
	unsigned count = 0;

	for (; byte != 0; byte &= byte - 1)
		count++;
	return (count);
}

/* Compare the bitmap's first N bits with the claimed set, which has the
 * same layout, in 4 KiB windows. A block is allocated exactly when it is
 * owned; the padding bits past N were checked at open and describe no
 * block, so the last byte is compared through a mask. The clear bits are
 * the free blocks.
 */
static fsimg_status_t
verify_allocation(fsimg_reader_t *reader, fsimg_verify_report_t *report,
    fsimg_error_t *error)
{
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(reader);
	const qnx4_geometry_t *g = &qnx4->geometry;
	unsigned char bitmap[QNX4_WINDOW_BYTES], owned[QNX4_WINDOW_BYTES];
	uint64_t offset, byte;
	size_t batch, transferred;
	unsigned valid, allocated;
	fsimg_status_t status;

	for (offset = 0; offset < g->bitmap_bytes; offset += batch) {
		batch = g->bitmap_bytes - offset > sizeof(bitmap) ?
		    sizeof(bitmap) : (size_t)(g->bitmap_bytes - offset);
		status = fsimg_reader_bytes(reader, QNX4_BITMAP_OFFSET + offset,
		    bitmap, batch, &transferred, error);
		if (status != FSIMG_OK)
			return (status);
		status = fsimg_bitset_read(qnx4->claimed, offset, owned, batch,
		    error);
		if (status != FSIMG_OK)
			return (status);
		/* One work unit per bitmap byte, eight blocks compared. */
		status = fsimg_session_charge(&reader->session, 0, 0, batch,
		    error);
		if (status != FSIMG_OK)
			return (status);

		for (size_t i = 0; i < batch; i++) {
			/* Byte `byte` describes blocks 8 * byte + 1 onward. */
			byte = offset + i;
			valid = 0xffu;
			if (byte == g->bitmap_bytes - 1u && g->blocks % 8 != 0)
				valid = (1u << (g->blocks % 8)) - 1;
			allocated = bitmap[i] & valid;
			if ((allocated & ~(unsigned)owned[i]) != 0)
				return (qnx4_corrupt(error,
				    "QNX4 allocated block has no owner"));
			if ((owned[i] & ~allocated) != 0)
				return (qnx4_corrupt(error,
				    "QNX4 owned block is marked free"));
			report->allocated_clusters += bits_in(allocated);
			report->free_clusters += bits_in(~allocated & valid);
		}
	}
	return (FSIMG_OK);
}

/* Walk every entry once, omitted ones included; preparing each directory
 * accounts its names. The payload of every entry is read, except a later
 * name of an .inodes object, which has its first name's bytes, read
 * already.
 */
static fsimg_status_t
traverse(fsimg_reader_t *reader, unsigned char *payload,
    fsimg_verify_report_t *report, fsimg_error_t *error)
{
	fsimg_status_t status;

	report->directories = 1;
	for (;;) {
		status = fsimg_walk_next(reader, error);
		if (status == FSIMG_DONE)
			return (FSIMG_OK);
		if (status != FSIMG_OK)
			return (status);
		status = fsimg_verify_account(reader, report, error);
		if (status != FSIMG_OK)
			return (status);
		if (reader->entry.kind == FSIMG_ENTRY_DIRECTORY)
			continue;
		if (payload != NULL && !qnx4_state(reader)->later_name) {
			status = fsimg_verify_payload(reader, payload,
			    PAYLOAD_BYTES, report, error);
			if (status != FSIMG_OK)
				return (status);
		}
	}
}

fsimg_status_t
qnx4_verify(fsimg_reader_t *reader, unsigned flags,
    fsimg_verify_report_t *report, fsimg_error_t *error)
{
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(reader);
	fsimg_stream_t *sorted;
	unsigned char *payload;
	fsimg_error_t detail;
	fsimg_status_t status;

	sorted = NULL;
	payload = NULL;
	reader->session.progress.phase = "QNX4 verification";
	status = check_scope(reader, error);
	if (status != FSIMG_OK)
		return (status);
	/* lost+found was walked at open; check_scope found no used slot in
	 * it, so it has no subdirectory and no prepare checks its count.
	 */
	if (qnx4->root[QNX4_RECORD_LOST_FOUND].links != 2)
		return (qnx4_corrupt(error,
		    "QNX4 directory link count disagrees with its subdirectories"));

	status = start_accounting(reader, error);
	if (status != FSIMG_OK)
		goto done;
	if ((flags & FSIMG_VERIFY_PAYLOAD) != 0) {
		payload = fsimg_alloc(reader->ctx, PAYLOAD_BYTES, &detail);
		if (payload == NULL) {
			status = detail.status;
			if (error != NULL)
				*error = detail;
			goto done;
		}
	}

	status = traverse(reader, payload, report, error);
	if (status != FSIMG_OK)
		goto done;

	/* Reconcile what the traversal recorded with .inodes,
	 * .longfilenames and the bitmap.
	 */
	status = fsimg_stream_seal(qnx4->references, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_sort(qnx4->references, reference_compare,
		    NULL, NULL, &sorted, error);
	if (status == FSIMG_OK)
		status = verify_inodes(reader, sorted, error);
	if (status == FSIMG_OK)
		status = verify_long_names(reader, error);
	if (status == FSIMG_OK)
		status = verify_allocation(reader, report, error);
	if (status == FSIMG_OK)
		status = fsimg_reader_stable(reader, error);
	if (status != FSIMG_OK)
		goto done;
	reader->info.free_bytes = report->free_clusters * QNX4_BLOCK_BYTES;
	reader->info.valid |= FSIMG_VOLUME_FREE;
done:
	fsimg_stream_free(sorted);
	fsimg_stream_free(qnx4->references);
	fsimg_bitset_free(qnx4->long_named);
	qnx4->references = NULL;
	qnx4->long_named = NULL;
	fsimg_dealloc(payload);
	return (status);
}
