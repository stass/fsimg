/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "qnx6.h"
#include "unicode.h"

/* QNX6's part of directory preparation (specs/design-directories.md). A
 * directory is an array of 32-byte slots: "." and ".." first, then live
 * slots naming inodes, with unused slots anywhere. Each admitted child's
 * payload is its inode number and the inode number of the directory holding
 * it, as two little-endian 32-bit values.
 */
#define QNX6_PAYLOAD_BYTES 8u

typedef struct qnx6_scan {
	fsimg_reader_t	*reader;
	fsimg_directory_build_t *build;
	fsimg_extent_index_t *index;
	uint64_t	 length;
	uint32_t	 self;
	uint32_t	 parent;
	unsigned char	 window[QNX6_WINDOW_BYTES];
	char		 name[QNX6_NAME_MAX + 1];
} qnx6_scan_t;

static fsimg_status_t reference(fsimg_reader_t *, uint32_t, fsimg_error_t *);
static fsimg_status_t dot_entry(qnx6_scan_t *, const qnx6_slot_t *,
    const char *, uint32_t, fsimg_error_t *);
static fsimg_status_t admit_slot(qnx6_scan_t *, const qnx6_slot_t *,
    fsimg_error_t *);
static fsimg_status_t scan_slots(qnx6_scan_t *, fsimg_error_t *);

/* During full verification every live slot, the dot entries included, is
 * one reference to its inode; the counts are reconciled with link counts.
 */
static fsimg_status_t
reference(fsimg_reader_t *reader, uint32_t inode, fsimg_error_t *error)
{
	fsimg_qnx6_reader_t *qnx6 = qnx6_state(reader);
	unsigned char record[4];

	if (qnx6->references == NULL)
		return (FSIMG_OK);
	fsimg_store_le32(record, inode);
	return (fsimg_stream_append(qnx6->references, record, sizeof(record),
	    NULL, error));
}

/* Slot 0 is "." naming the directory itself and slot 1 is ".." naming its
 * parent; both name inode 1 in the root. They are checked, not admitted.
 */
static fsimg_status_t
dot_entry(qnx6_scan_t *scan, const qnx6_slot_t *slot, const char *dots,
    uint32_t inode, fsimg_error_t *error)
{
	size_t length = strlen(dots);

	if (slot->long_name || slot->length != length ||
	    bcmp(slot->name, dots, length) != 0 || slot->inode != inode)
		return (qnx6_corrupt(error, "QNX6 directory has a missing or wrong dot entry"));
	return (reference(scan->reader, inode, error));
}

/* Decode a live slot's name and inode and admit the child. The inode is
 * validated now, so an unsupported kind fails the whole directory instead
 * of disappearing from it.
 */
static fsimg_status_t
admit_slot(qnx6_scan_t *scan, const qnx6_slot_t *slot, fsimg_error_t *error)
{
	qnx6_inode_t inode;
	unsigned char payload[QNX6_PAYLOAD_BYTES];
	fsimg_status_t status;

	status = qnx6_slot_name(scan->reader, slot, scan->name, error);
	if (status != FSIMG_OK)
		return (status);
	status = qnx6_inode_read(scan->reader, slot->inode, &inode, error);
	if (status != FSIMG_OK)
		return (status);
	status = reference(scan->reader, slot->inode, error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_store_le32(payload, slot->inode);
	fsimg_store_le32(payload + 4, scan->self);
	return (fsimg_directory_admit(scan->build, scan->name, payload,
	    sizeof(payload), error));
}

/* Read the directory's declared length in 4 KiB windows, one slot at a
 * time. The length is a multiple of 32 and so is every window, so a slot
 * never straddles two. An unused slot (inode or length zero) is not an end
 * marker and is not decoded further.
 */
static fsimg_status_t
scan_slots(qnx6_scan_t *scan, fsimg_error_t *error)
{
	fsimg_reader_t *reader = scan->reader;
	bool big = qnx6_state(reader)->geometry.big_endian;
	qnx6_slot_t slot;
	uint64_t offset;
	size_t window, within;
	fsimg_status_t status;

	if (scan->length < 2 * QNX6_SLOT_BYTES)
		return (qnx6_corrupt(error, "QNX6 directory has no dot entries"));
	window = 0;
	within = 0;
	for (offset = 0; offset < scan->length; offset += QNX6_SLOT_BYTES) {
		if (within == window) {
			window = scan->length - offset > sizeof(scan->window) ?
			    sizeof(scan->window) : (size_t)(scan->length - offset);
			status = qnx6_index_read(reader, scan->index, offset,
			    scan->window, window, error);
			if (status != FSIMG_OK)
				return (status);
			within = 0;
		}
		status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
		if (status != FSIMG_OK)
			return (status);
		qnx6_slot_decode(scan->window + within, big, &slot);
		within += QNX6_SLOT_BYTES;
		if (offset == 0)
			status = dot_entry(scan, &slot, ".", scan->self, error);
		else if (offset == QNX6_SLOT_BYTES)
			status = dot_entry(scan, &slot, "..", scan->parent, error);
		else if (slot.inode == 0 || slot.length == 0)
			continue;
		else
			status = admit_slot(scan, &slot, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/* Prepare the root (inode 1) at level 0, otherwise the directory replay
 * made current. Its walk claims every block, so storage shared with any
 * object walked before, and a directory reached from a second parent or
 * through a cycle, are corruption.
 */
fsimg_status_t
qnx6_prepare(fsimg_reader_t *reader, fsimg_directory_build_t *build,
    fsimg_error_t *error)
{
	fsimg_qnx6_reader_t *qnx6 = qnx6_state(reader);
	qnx6_object_t object;
	qnx6_inode_t root;
	qnx6_scan_t *scan;
	const qnx6_inode_t *directory;
	fsimg_error_t detail;
	fsimg_status_t status;

	scan = fsimg_alloc(reader->ctx, sizeof(*scan), &detail);
	if (scan == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	scan->reader = reader;
	scan->build = build;
	scan->index = NULL;
	if (fsimg_directory_level(build) == 0) {
		status = qnx6_inode_read(reader, QNX6_ROOT_INODE, &root, error);
		if (status != FSIMG_OK)
			goto done;
		if ((root.mode & QNX6_MODE_TYPE) != QNX6_MODE_DIRECTORY) {
			status = qnx6_corrupt(error, "QNX6 root inode is not a directory");
			goto done;
		}
		directory = &root;
		scan->self = scan->parent = QNX6_ROOT_INODE;
	} else {
		directory = &qnx6->current;
		scan->self = qnx6->current_inode;
		scan->parent = qnx6->current_parent;
	}
	scan->length = directory->size;
	object = (qnx6_object_t){
		.tree = &directory->tree,
		.owner = scan->self,
		.domain = QNX6_DOMAIN_CONTENT,
		.role = QNX6_ROLE_DIRECTORY,
		.policy = QNX6_WALK_EXCLUSIVE,
		.records = true
	};
	status = qnx6_index_build(reader, &object, &scan->index, error);
	if (status == FSIMG_OK)
		status = scan_slots(scan, error);
done:
	fsimg_extent_index_free(scan->index);
	fsimg_dealloc(scan);
	return (status);
}

/* Index the current file, symbolic link or FIFO under policy: exclusive
 * for its first name, which records ownership, allocated for a later name,
 * whose runs would repeat the first name's.
 */
fsimg_status_t
qnx6_index_current(fsimg_reader_t *reader, qnx6_walk_policy_t policy,
    fsimg_error_t *error)
{
	fsimg_qnx6_reader_t *qnx6 = qnx6_state(reader);
	qnx6_object_t object;

	object = (qnx6_object_t){
		.tree = &qnx6->current.tree,
		.owner = qnx6->current_inode,
		.domain = QNX6_DOMAIN_CONTENT,
		.role = reader->entry.kind == FSIMG_ENTRY_SYMLINK ?
		    QNX6_ROLE_SYMLINK : QNX6_ROLE_PAYLOAD,
		.policy = policy,
		.records = policy == QNX6_WALK_EXCLUSIVE
	};
	return (qnx6_index_build(reader, &object, &qnx6->file, error));
}

/* Make a replayed child current. Its record came from scratch storage, so
 * the payload and name are validated and the inode is read again. The
 * first name of a file, symbolic link or FIFO claims its tree now, so
 * storage it shares with any other object fails while listing. A later
 * name of the same inode (a hard link) names a tree already claimed; it is
 * indexed when first read, so listing many names of a large file walks it
 * once. The exposed bit is set only after the claiming walk succeeds, and
 * the image is stable while the reader is open. A directory is indexed on
 * descent.
 */
fsimg_status_t
qnx6_replay(fsimg_reader_t *reader, const char *name,
    const unsigned char *payload, size_t length, fsimg_error_t *error)
{
	fsimg_qnx6_reader_t *qnx6 = qnx6_state(reader);
	uint32_t inode, parent;
	bool exposed;
	fsimg_status_t status;

	fsimg_extent_index_free(qnx6->file);
	qnx6->file = NULL;
	qnx6->later_name = false;
	if (length != QNX6_PAYLOAD_BYTES)
		return (qnx6_corrupt(error, "invalid prepared QNX6 entry payload"));
	inode = fsimg_load_le32(payload);
	parent = fsimg_load_le32(payload + 4);
	if (parent == 0 || parent > qnx6->geometry.inode_count)
		return (qnx6_corrupt(error, "invalid prepared QNX6 entry payload"));
	if (qnx6_name_native((const unsigned char *)name, strlen(name),
	    NULL) != FSIMG_OK)
		return (qnx6_corrupt(error, "invalid prepared QNX6 file name"));
	status = qnx6_inode_read(reader, inode, &qnx6->current, error);
	if (status != FSIMG_OK)
		return (status);
	status = qnx6_inode_entry(&qnx6->current, &reader->entry, error);
	if (status != FSIMG_OK)
		return (status);
	qnx6->current_inode = inode;
	qnx6->current_parent = parent;
	if (reader->entry.kind == FSIMG_ENTRY_DIRECTORY)
		return (FSIMG_OK);
	/* qnx6_inode_read bounded inode by the inode count. */
	status = fsimg_bitset_test(qnx6->exposed, inode, &exposed, error);
	qnx6->later_name = exposed;
	if (status == FSIMG_OK && !exposed) {
		status = qnx6_index_current(reader, QNX6_WALK_EXCLUSIVE, error);
		if (status == FSIMG_OK)
			status = fsimg_bitset_test_and_set(qnx6->exposed, inode,
			    &exposed, error);
	}
	if (status == FSIMG_OK)
		reader->initialized = reader->entry.size;
	return (status);
}

_Static_assert(QNX6_NAME_MAX <= FSIMG_DIRECTORY_KEY_BYTES,
    "a QNX6 name key fits a directory key buffer");

/* The namespace key is the exact name bytes, raw for an omitted name: QNX6
 * names are compared byte for byte, case preserved and significant.
 */
fsimg_status_t
qnx6_keys(fsimg_reader_t *reader FSIMG_UNUSED, const char *name,
    const unsigned char *payload FSIMG_UNUSED, size_t length FSIMG_UNUSED,
    fsimg_directory_keys_t *keys, fsimg_error_t *error)
{
	size_t bytes = strlen(name);

	keys->count = 0;
	if (qnx6_name_native((const unsigned char *)name, bytes, NULL) !=
	    FSIMG_OK)
		return (qnx6_corrupt(error, "invalid QNX6 file name"));
	bcopy(name, keys->key[0], bytes);
	keys->length[0] = bytes;
	keys->count = 1;
	return (FSIMG_OK);
}

/* A query spells a delivered name, so a component outside the profile
 * matches nothing and is refused before any directory is consulted.
 */
fsimg_status_t
qnx6_key(fsimg_reader_t *reader FSIMG_UNUSED, const char *component,
    unsigned char *key, size_t *length, fsimg_error_t *error)
{
	size_t bytes = strlen(component);

	*length = 0;
	if (bytes > QNX6_NAME_MAX || !fsimg_name_in_qnx_profile(component, bytes))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_READER,
		    "selection component is not representable in QNX6"));
	bcopy(component, key, bytes);
	*length = bytes;
	return (FSIMG_OK);
}
