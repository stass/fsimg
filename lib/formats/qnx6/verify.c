/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "qnx6.h"

/* Full verification of the selected snapshot. One traversal records an
 * ownership run for every block any index visits and a reference for every
 * live directory slot. Sorted, the runs are merged with the bitmap and the
 * references with the inode table, so allocation, free counts and link
 * counts are checked in two sequential passes over bounded windows.
 *
 * An ownership record is the run's first block and block count (u32 each)
 * and its owner (an inode number, or a system identity above the inode
 * space, u64).
 */
#define OWNER_RECORD_BYTES 16u

typedef struct qnx6_owner_cursor {
	fsimg_stream_t	*stream;
	uint64_t	 cursor;
	uint64_t	 owner;
	uint32_t	 first;
	uint32_t	 end;
	bool		 exhausted;
} qnx6_owner_cursor_t;

static int compare_u64(uint64_t, uint64_t);
static int owner_compare(const void *, size_t, const void *, size_t, void *);
static int reference_compare(const void *, size_t, const void *, size_t,
    void *);
static fsimg_status_t own_system_trees(fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t owner_next(fsimg_reader_t *, qnx6_owner_cursor_t *,
    fsimg_error_t *);
static fsimg_status_t verify_allocations(fsimg_reader_t *, fsimg_stream_t *,
    fsimg_verify_report_t *, fsimg_error_t *);
static fsimg_status_t reference_next(fsimg_stream_t *, uint64_t *,
    uint32_t *, fsimg_error_t *);
static fsimg_status_t verify_inodes(fsimg_reader_t *, fsimg_stream_t *,
    fsimg_error_t *);
static fsimg_status_t read_payload(fsimg_reader_t *, unsigned char *,
    fsimg_verify_report_t *, fsimg_error_t *);

/* Record one run of blocks owned by one object. Tree walks call this while
 * verification has created the ownership stream.
 */
fsimg_status_t
qnx6_own(fsimg_reader_t *reader, uint32_t first, uint32_t count,
    uint64_t owner, fsimg_error_t *error)
{
	unsigned char record[OWNER_RECORD_BYTES];

	fsimg_store_le32(record, first);
	fsimg_store_le32(record + 4, count);
	fsimg_store_le64(record + 8, owner);
	return (fsimg_stream_append(qnx6_state(reader)->owners, record,
	    sizeof(record), NULL, error));
}

static int
compare_u64(uint64_t a, uint64_t b)
{
	return ((a > b) - (a < b));
}

/* Order runs by first block; runs that start at one block overlap, so
 * their relative order does not matter.
 */
static int
owner_compare(const void *a, size_t alen, const void *b, size_t blen,
    void *argument FSIMG_UNUSED)
{
	if (alen != OWNER_RECORD_BYTES || blen != OWNER_RECORD_BYTES)
		return ((alen > blen) - (alen < blen));
	return (compare_u64(fsimg_load_le32(a), fsimg_load_le32(b)));
}

static int
reference_compare(const void *a, size_t alen, const void *b, size_t blen,
    void *argument FSIMG_UNUSED)
{
	if (alen != 4 || blen != 4)
		return ((alen > blen) - (alen < blen));
	return (compare_u64(fsimg_load_le32(a), fsimg_load_le32(b)));
}

/* The system trees were indexed at open, before the ownership stream
 * existed; walking them again records every block they own, indirect
 * blocks included. The long-name tree's blocks are already claimed, so it
 * is only checked for allocation.
 */
static fsimg_status_t
own_system_trees(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_qnx6_reader_t *qnx6 = qnx6_state(reader);
	const qnx6_object_t trees[] = {
		{ .tree = &qnx6->header.bitmap, .owner = QNX6_OWNER_BITMAP,
		    .domain = QNX6_DOMAIN_SYSTEM, .role = QNX6_ROLE_BITMAP,
		    .policy = QNX6_WALK_ALLOCATED, .records = true },
		{ .tree = &qnx6->header.inodes, .owner = QNX6_OWNER_INODES,
		    .domain = QNX6_DOMAIN_SYSTEM, .role = QNX6_ROLE_INODES,
		    .policy = QNX6_WALK_ALLOCATED, .records = true },
		{ .tree = &qnx6->header.names, .owner = QNX6_OWNER_NAMES,
		    .domain = QNX6_DOMAIN_CONTENT, .role = QNX6_ROLE_NAMES,
		    .policy = QNX6_WALK_ALLOCATED, .records = true }
	};
	fsimg_extent_index_t *index;
	fsimg_status_t status;

	for (size_t i = 0; i < sizeof(trees) / sizeof(trees[0]); i++) {
		status = qnx6_index_build(reader, &trees[i], &index, error);
		fsimg_extent_index_free(index);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/* Advance to the next run, which must not overlap the previous one. Each
 * tree is walked once during verification, a later name of an inode never,
 * so any overlap is a block with two owners or used twice by one.
 */
static fsimg_status_t
owner_next(fsimg_reader_t *reader, qnx6_owner_cursor_t *owners,
    fsimg_error_t *error)
{
	unsigned char record[OWNER_RECORD_BYTES];
	uint32_t blocks, first, count;
	size_t length;
	fsimg_status_t status;

	blocks = qnx6_state(reader)->geometry.blocks;
	status = fsimg_stream_next(owners->stream, &owners->cursor, record,
	    sizeof(record), &length, error);
	if (status == FSIMG_DONE) {
		owners->exhausted = true;
		return (FSIMG_OK);
	}
	if (status != FSIMG_OK)
		return (status);
	first = fsimg_load_le32(record);
	count = fsimg_load_le32(record + 4);
	if (length != sizeof(record) || first >= blocks || count == 0 ||
	    count > blocks - first)
		return (qnx6_corrupt(error, "invalid QNX6 ownership record"));
	if (first < owners->end)
		return (qnx6_corrupt(error, "QNX6 block has several owners"));
	owners->first = first;
	owners->end = first + count;
	owners->owner = fsimg_load_le64(record + 8);
	return (FSIMG_OK);
}

/* Merge the sorted runs with the bitmap. The reservation and its twin are
 * always allocated and only system trees own blocks there; from 2R on a
 * block is allocated exactly when something owns it. The clear bits must
 * match the header's free count.
 */
static fsimg_status_t
verify_allocations(fsimg_reader_t *reader, fsimg_stream_t *sorted,
    fsimg_verify_report_t *report, fsimg_error_t *error)
{
	fsimg_qnx6_reader_t *qnx6 = qnx6_state(reader);
	const qnx6_geometry_t *g = &qnx6->geometry;
	qnx6_owner_cursor_t owners;
	uint64_t system_end, free_blocks;
	bool owned, allocated;
	fsimg_status_t status;

	bzero(&owners, sizeof(owners));
	owners.stream = sorted;
	status = owner_next(reader, &owners, error);
	if (status != FSIMG_OK)
		return (status);
	system_end = (uint64_t)g->reservation * 2;
	free_blocks = 0;
	for (uint32_t block = 0; block < g->blocks; block++) {
		/* One unit of work per bitmap byte examined. */
		if (block % 8 == 0) {
			status = fsimg_session_charge(&reader->session, 0, 0, 1,
			    error);
			if (status != FSIMG_OK)
				return (status);
		}
		status = qnx6_block_allocated(reader, block, &allocated, error);
		if (status != FSIMG_OK)
			return (status);
		while (!owners.exhausted && block >= owners.end) {
			status = owner_next(reader, &owners, error);
			if (status != FSIMG_OK)
				return (status);
		}
		owned = !owners.exhausted && block >= owners.first;
		if (block < system_end) {
			if (!allocated)
				return (qnx6_corrupt(error, "QNX6 system block is marked free"));
			if (owned && owners.owner != QNX6_OWNER_BITMAP &&
			    owners.owner != QNX6_OWNER_INODES)
				return (qnx6_corrupt(error, "QNX6 object owns a system block"));
		} else if (allocated != owned) {
			return (qnx6_corrupt(error, allocated ?
			    "QNX6 allocated block has no owner" :
			    "QNX6 owned block is marked free"));
		}
		if (allocated)
			report->allocated_clusters++;
		else
			free_blocks++;
	}
	/* The last run may end at N; nothing may follow it. */
	if (!owners.exhausted) {
		status = owner_next(reader, &owners, error);
		if (status != FSIMG_OK)
			return (status);
		if (!owners.exhausted)
			return (qnx6_corrupt(error, "QNX6 ownership exceeds the volume"));
	}
	if (free_blocks != qnx6->header.free_blocks)
		return (qnx6_corrupt(error, "QNX6 free block count mismatch"));
	report->free_clusters = free_blocks;
	return (FSIMG_OK);
}

/* The next inode number in the sorted reference stream, or 0 when the
 * stream is exhausted; references are never 0.
 */
static fsimg_status_t
reference_next(fsimg_stream_t *sorted, uint64_t *cursor, uint32_t *next,
    fsimg_error_t *error)
{
	unsigned char record[4];
	size_t length;
	fsimg_status_t status;

	*next = 0;
	status = fsimg_stream_next(sorted, cursor, record, sizeof(record),
	    &length, error);
	if (status == FSIMG_DONE)
		return (FSIMG_OK);
	if (status != FSIMG_OK)
		return (status);
	*next = fsimg_load_le32(record);
	if (length != sizeof(record) || *next == 0)
		return (qnx6_corrupt(error, "invalid QNX6 reference record"));
	return (FSIMG_OK);
}

/* Merge the sorted slot references with one pass over the inode table.
 * A referenced inode's link count equals its references; for a directory
 * that is 2 + subdirectories, from its name, "." and each child's "..".
 * Every other inode is unallocated, and the header's free inode count is
 * the capacity minus the referenced inodes.
 */
static fsimg_status_t
verify_inodes(fsimg_reader_t *reader, fsimg_stream_t *sorted,
    fsimg_error_t *error)
{
	fsimg_qnx6_reader_t *qnx6 = qnx6_state(reader);
	const qnx6_geometry_t *g = &qnx6->geometry;
	unsigned char table[QNX6_WINDOW_BYTES];
	qnx6_inode_t inode;
	uint64_t cursor, offset, window, table_bytes, referenced, count;
	uint32_t next;
	size_t batch;
	fsimg_status_t status;

	cursor = 0;
	status = reference_next(sorted, &cursor, &next, error);
	if (status != FSIMG_OK)
		return (status);
	referenced = 0;
	table_bytes = (uint64_t)g->inode_count * QNX6_INODE_BYTES;
	window = UINT64_MAX;
	for (uint64_t number = 1; number <= g->inode_count; number++) {
		count = 0;
		while (next == number) {
			count++;
			status = reference_next(sorted, &cursor, &next, error);
			if (status != FSIMG_OK)
				return (status);
		}

		offset = (number - 1) * QNX6_INODE_BYTES;
		if (offset / QNX6_WINDOW_BYTES * QNX6_WINDOW_BYTES != window) {
			window = offset / QNX6_WINDOW_BYTES * QNX6_WINDOW_BYTES;
			batch = table_bytes - window > sizeof(table) ?
			    sizeof(table) : (size_t)(table_bytes - window);
			status = qnx6_index_read(reader, qnx6->inodes, window,
			    table, batch, error);
			if (status != FSIMG_OK)
				return (status);
			status = fsimg_session_charge(&reader->session, 0, 0,
			    batch / QNX6_INODE_BYTES, error);
			if (status != FSIMG_OK)
				return (status);
		}
		qnx6_inode_decode(table + (offset - window), g->big_endian, &inode);
		if (count == 0) {
			if ((inode.status & QNX6_STATUS_ALLOCATED) != 0)
				return (qnx6_corrupt(error,
				    "QNX6 allocated inode has no name"));
			continue;
		}
		referenced++;
		if ((inode.status & QNX6_STATUS_ALLOCATED) == 0 ||
		    inode.links != count)
			return (qnx6_corrupt(error, "QNX6 inode link count mismatch"));
	}
	/* Sorted references all fall inside the table unless one is left. */
	if (next != 0)
		return (qnx6_corrupt(error, "QNX6 reference exceeds the inode table"));
	if (g->inode_count - referenced != qnx6->header.free_inodes)
		return (qnx6_corrupt(error, "QNX6 free inode count mismatch"));
	return (FSIMG_OK);
}

static fsimg_status_t
read_payload(fsimg_reader_t *reader, unsigned char *buffer,
    fsimg_verify_report_t *report, fsimg_error_t *error)
{
	uint64_t offset;
	size_t batch, count;
	fsimg_status_t status;

	for (offset = 0; offset < reader->initialized; offset += batch) {
		batch = reader->initialized - offset > QNX6_BUFFER_BYTES ?
		    QNX6_BUFFER_BYTES : (size_t)(reader->initialized - offset);
		status = fsimg_reader_content(reader, offset, buffer, batch,
		    &count, error);
		report->payload_bytes_read += count;
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

fsimg_status_t
qnx6_verify(fsimg_reader_t *reader, unsigned flags,
    fsimg_verify_report_t *report, fsimg_error_t *error)
{
	fsimg_qnx6_reader_t *qnx6 = qnx6_state(reader);
	fsimg_stream_t *owners, *references;
	unsigned char *buffer;
	fsimg_error_t detail;
	fsimg_status_t status;

	owners = references = NULL;
	buffer = NULL;
	reader->session.progress.phase = "QNX6 verification";
	status = fsimg_stream_new(&reader->session, OWNER_RECORD_BYTES,
	    &qnx6->owners, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_new(&reader->session, 4, &qnx6->references,
		    error);
	if (status == FSIMG_OK)
		status = own_system_trees(reader, error);
	if (status != FSIMG_OK)
		goto done;
	if ((flags & FSIMG_VERIFY_PAYLOAD) != 0) {
		buffer = fsimg_alloc(reader->ctx, QNX6_BUFFER_BYTES, &detail);
		if (buffer == NULL) {
			status = detail.status;
			if (error != NULL)
				*error = detail;
			goto done;
		}
	}

	/* One traversal, omitted entries included: every index built records
	 * its blocks and every prepared directory records its slot references.
	 */
	report->directories = 1;
	for (;;) {
		status = fsimg_walk_next(reader, error);
		if (status == FSIMG_DONE)
			break;
		if (status != FSIMG_OK)
			goto done;
		status = fsimg_verify_account(reader, report, error);
		if (status != FSIMG_OK)
			goto done;
		if (reader->entry.kind == FSIMG_ENTRY_DIRECTORY)
			continue;
		/* A later name of an inode has the same bytes as its first
		 * name, which was read already.
		 */
		if (buffer != NULL && !qnx6->later_name) {
			status = read_payload(reader, buffer, report, error);
			if (status != FSIMG_OK)
				goto done;
		}
	}

	/* Reconcile what the traversal recorded with the bitmap and the
	 * inode table.
	 */
	status = fsimg_stream_seal(qnx6->owners, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_sort(qnx6->owners, owner_compare, NULL,
		    NULL, &owners, error);
	if (status == FSIMG_OK)
		status = verify_allocations(reader, owners, report, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_seal(qnx6->references, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_sort(qnx6->references, reference_compare,
		    NULL, NULL, &references, error);
	if (status == FSIMG_OK)
		status = verify_inodes(reader, references, error);
	if (status == FSIMG_OK)
		status = fsimg_reader_stable(reader, error);
	if (status != FSIMG_OK)
		goto done;
	reader->info.free_bytes = report->free_clusters *
	    qnx6->geometry.block_size;
	reader->info.valid |= FSIMG_VOLUME_FREE;
done:
	fsimg_stream_free(owners);
	fsimg_stream_free(references);
	fsimg_stream_free(qnx6->owners);
	fsimg_stream_free(qnx6->references);
	qnx6->owners = qnx6->references = NULL;
	fsimg_dealloc(buffer);
	return (status);
}
