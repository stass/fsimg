/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "qnx4.h"
#include "unicode.h"

/*
 * QNX4's part of directory preparation (specs/design-directories.md). A
 * directory is an array of 64-byte slots scanned in full: "." and ".."
 * first, then direct records, link records and free slots anywhere. Each
 * admitted child's payload is the position of the object's record and the
 * position of the directory holding it, two little-endian 64-bit values.
 * Dot entries are checked against those positions and never admitted.
 */
#define QNX4_PAYLOAD_BYTES 16u

typedef struct qnx4_prepare {
	fsimg_reader_t	*reader;
	fsimg_directory_build_t *build;
	uint64_t	 self;		/* the directory's own record position */
	uint64_t	 parent;	/* its parent's record position */
	uint64_t	 subdirectories; /* lost+found and child directories */
	bool		 root;
	char		 name[QNX4_NAME_MAX + 1];
} qnx4_prepare_t;

static fsimg_status_t count_name(qnx4_prepare_t *, const qnx4_inode_t *,
    qnx4_place_t, uint64_t, fsimg_error_t *);
static fsimg_status_t admit(qnx4_prepare_t *, const char *,
    const qnx4_inode_t *, qnx4_place_t, uint64_t, uint64_t, fsimg_error_t *);
static fsimg_status_t admit_direct(qnx4_prepare_t *, uint64_t,
    const unsigned char *, fsimg_error_t *);
static fsimg_status_t admit_link(qnx4_prepare_t *, uint64_t,
    const unsigned char *, fsimg_error_t *);
static fsimg_status_t prepare_format_slot(qnx4_prepare_t *, uint64_t,
    const unsigned char *, fsimg_error_t *);
static fsimg_status_t prepare_slot(void *, uint64_t, uint64_t,
    const unsigned char *, fsimg_error_t *);

/* Visit every slot of a directory's `length` bytes through its index, in
 * 4 KiB windows that never cross an extent, so each slot's physical
 * position is known. The length is whole blocks and every extent is whole
 * blocks, so a slot never straddles two windows. Each slot charges one
 * work unit.
 */
fsimg_status_t
qnx4_slots_scan(fsimg_reader_t *reader, fsimg_extent_index_t *index,
    uint64_t length, qnx4_slot_fn_t visit, void *argument,
    fsimg_error_t *error)
{
	unsigned char *window;
	fsimg_extent_t extent;
	uint64_t offset, within, image, slot;
	size_t batch, transferred;
	fsimg_error_t detail;
	fsimg_status_t status;

	window = fsimg_alloc(reader->ctx, QNX4_WINDOW_BYTES, &detail);
	if (window == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	status = FSIMG_OK;
	for (offset = 0; offset < length && status == FSIMG_OK;
	    offset += batch) {
		status = fsimg_extent_index_lookup(index, offset, &extent, error);
		if (status != FSIMG_OK)
			break;
		within = offset - extent.logical_offset;
		batch = QNX4_WINDOW_BYTES;
		if (batch > extent.length - within)
			batch = (size_t)(extent.length - within);
		if (batch > length - offset)
			batch = (size_t)(length - offset);
		status = fsimg_reader_bytes(reader, extent.image_offset + within,
		    window, batch, &transferred, error);
		for (size_t w = 0; w < batch && status == FSIMG_OK;
		    w += QNX4_RECORD_BYTES) {
			status = fsimg_session_charge(&reader->session, 0, 0, 1,
			    error);
			if (status != FSIMG_OK)
				break;
			image = extent.image_offset + within + w;
			slot = (offset + w) / QNX4_RECORD_BYTES;
			/* A slot that is both a record and a link is neither. */
			if ((window[w + 0x3f] & (QNX4_STATUS_USED |
			    QNX4_STATUS_LINK)) == (QNX4_STATUS_USED |
			    QNX4_STATUS_LINK)) {
				status = qnx4_corrupt(error,
				    "QNX4 record is both used and a link");
				break;
			}
			status = visit(argument, slot, QNX4_POSITION(image /
			    QNX4_BLOCK_BYTES + 1, image % QNX4_BLOCK_BYTES /
			    QNX4_RECORD_BYTES), window + w, error);
		}
	}
	fsimg_dealloc(window);
	return (status);
}

/* Decode a dot entry -- slot 0, ".", or slot 1, ".." -- and return the
 * position it names. Both are link records with status 0x08, no long-name
 * pointer and zero spare bytes. "." carries "I", 0x03, "QNX" at name bytes
 * 2 to 6 and an uninterpreted 32-bit value at 8; ".." is bare.
 */
fsimg_status_t
qnx4_dot_target(const unsigned char *raw, bool self, uint64_t *target,
    fsimg_error_t *error)
{
	static const unsigned char dot[8] = { '.', 0, 'I', 0x03, 'Q', 'N', 'X',
	    0 };
	static const unsigned char dotdot[8] = { '.', '.', 0, 0, 0, 0, 0, 0 };
	qnx4_link_t link;
	bool name_ok;

	*target = 0;
	qnx4_link_decode(raw, &link);
	name_ok = bcmp(link.name, self ? dot : dotdot, 8) == 0 &&
	    qnx4_bytes_zero(link.name + (self ? 12 : 8),
	    QNX4_LINK_NAME_MAX - (self ? 12 : 8));
	if (link.status != QNX4_STATUS_LINK || !name_ok ||
	    link.long_block != 0 || !link.spare_zero ||
	    link.target_slot >= QNX4_SLOTS_PER_BLOCK)
		return (qnx4_corrupt(error,
		    "QNX4 directory has a missing or wrong dot entry"));
	*target = QNX4_POSITION(link.target_block, link.target_slot);
	return (FSIMG_OK);
}

/* Account the name a used slot gives the object whose record it holds or
 * links, found at place: a child directory adds one to this directory's
 * link count, and verification checks the object's own count (see
 * verify.c). table_slot is the logical .inodes slot of a record in
 * QNX4_PLACE_TABLE. Every used slot past the dot entries is counted here,
 * admitted or not.
 */
static fsimg_status_t
count_name(qnx4_prepare_t *prepare, const qnx4_inode_t *object,
    qnx4_place_t place, uint64_t table_slot, fsimg_error_t *error)
{
	if ((object->mode & QNX4_MODE_TYPE) == QNX4_MODE_DIRECTORY)
		prepare->subdirectories++;
	return (qnx4_verify_name(prepare->reader, object, place, table_slot,
	    error));
}

/* Admit the object whose record is at position under name, after counting
 * the name: every admitted entry passes here, so none escapes the count.
 */
static fsimg_status_t
admit(qnx4_prepare_t *prepare, const char *name, const qnx4_inode_t *object,
    qnx4_place_t place, uint64_t position, uint64_t table_slot,
    fsimg_error_t *error)
{
	unsigned char payload[QNX4_PAYLOAD_BYTES];
	fsimg_status_t status;

	status = count_name(prepare, object, place, table_slot, error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_store_le64(payload, position);
	fsimg_store_le64(payload + 8, prepare->self);
	return (fsimg_directory_admit(prepare->build, name, payload,
	    sizeof(payload), error));
}

/* A direct record is its own object. It is validated at admission, so an
 * unsupported kind fails the whole directory instead of disappearing from
 * it.
 */
static fsimg_status_t
admit_direct(qnx4_prepare_t *prepare, uint64_t position,
    const unsigned char *raw, fsimg_error_t *error)
{
	qnx4_inode_t inode;
	fsimg_status_t status;

	qnx4_inode_decode(raw, &inode);
	status = qnx4_inode_validate(&inode, QNX4_PLACE_DIRECT, error);
	if (status != FSIMG_OK)
		return (status);
	status = qnx4_direct_name(&inode, prepare->name, error);
	if (status != FSIMG_OK)
		return (status);
	return (admit(prepare, prepare->name, &inode, QNX4_PLACE_DIRECT,
	    position, 0, error));
}

/* A link record names an .inodes record; its target is resolved and
 * validated at admission, and the entry carries the target's position.
 */
static fsimg_status_t
admit_link(qnx4_prepare_t *prepare, uint64_t position,
    const unsigned char *raw, fsimg_error_t *error)
{
	qnx4_link_t link;
	qnx4_inode_t target;
	uint64_t target_position, table_slot;
	fsimg_status_t status;

	qnx4_link_decode(raw, &link);
	if (link.status != QNX4_STATUS_LINK || !link.spare_zero)
		return (qnx4_unsupported(error,
		    "QNX4 link record has unexpected status or spare bytes"));
	status = qnx4_link_resolve(prepare->reader, &link, &target_position,
	    &table_slot, &target, error);
	if (status != FSIMG_OK)
		return (status);
	status = qnx4_link_name(prepare->reader, &link, position,
	    prepare->name, error);
	if (status != FSIMG_OK)
		return (status);
	return (admit(prepare, prepare->name, &target, QNX4_PLACE_TABLE,
	    target_position, table_slot, error));
}

/* A root slot naming a format-owned object, which open validated: fixed
 * slots 2 to 5, checked again here, and the .longfilenames record found
 * beyond them. Each gives its object one name. .bitmap, .inodes and
 * .longfilenames are never admitted, so their names are counted here;
 * .boot and .altboot are admitted as the root-block records their links
 * name.
 */
static fsimg_status_t
prepare_format_slot(qnx4_prepare_t *prepare, uint64_t slot,
    const unsigned char *raw, fsimg_error_t *error)
{
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(prepare->reader);
	const qnx4_inode_t *root = qnx4->root;
	qnx4_inode_t direct;
	fsimg_status_t status;

	if (slot < QNX4_ROOT_FIXED_SLOTS) {
		status = qnx4_root_fixed_slot(prepare->reader, (unsigned)slot,
		    raw, error);
		if (status != FSIMG_OK)
			return (status);
	}
	switch (slot) {
	case 2:		/* .bitmap, a direct record */
		qnx4_inode_decode(raw, &direct);
		return (count_name(prepare, &direct, QNX4_PLACE_DIRECT, 0,
		    error));
	case 3:		/* .inodes, a link to root-block record 1 */
		return (count_name(prepare, &root[QNX4_RECORD_INODES],
		    QNX4_PLACE_ROOT, 0, error));
	case 4:		/* .boot, a link to root-block record 2 */
		return (admit(prepare, ".boot", &root[QNX4_RECORD_BOOT],
		    QNX4_PLACE_ROOT, QNX4_POSITION(QNX4_ROOT_BLOCK,
		    QNX4_RECORD_BOOT), 0, error));
	case 5:		/* .altboot, a link to root-block record 3 */
		return (admit(prepare, ".altboot", &root[QNX4_RECORD_ALTBOOT],
		    QNX4_PLACE_ROOT, QNX4_POSITION(QNX4_ROOT_BLOCK,
		    QNX4_RECORD_ALTBOOT), 0, error));
	default:	/* .longfilenames, a direct record */
		qnx4_inode_decode(raw, &direct);
		return (count_name(prepare, &direct, QNX4_PLACE_DIRECT, 0,
		    error));
	}
}

/* One slot of the directory being prepared: the dot entries, the root's
 * format-owned slots, then free slots, links and direct records anywhere.
 */
static fsimg_status_t
prepare_slot(void *argument, uint64_t slot, uint64_t position,
    const unsigned char *raw, fsimg_error_t *error)
{
	qnx4_prepare_t *prepare = argument;
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(prepare->reader);
	uint64_t target;
	fsimg_status_t status;

	if (slot < 2) {
		status = qnx4_dot_target(raw, slot == 0, &target, error);
		if (status != FSIMG_OK)
			return (status);
		if (target != (slot == 0 ? prepare->self : prepare->parent))
			return (qnx4_corrupt(error,
			    "QNX4 directory has a missing or wrong dot entry"));
		return (FSIMG_OK);
	}
	if (prepare->root && (slot < QNX4_ROOT_FIXED_SLOTS ||
	    (qnx4->has_names &&
	    slot * QNX4_RECORD_BYTES == qnx4->names_slot_offset)))
		return (prepare_format_slot(prepare, slot, raw, error));
	if (qnx4_slot_free(raw))
		return (FSIMG_OK);
	if (qnx4_slot_link(raw))
		return (admit_link(prepare, position, raw, error));
	return (admit_direct(prepare, position, raw, error));
}

/* Prepare the root (record (2, 0)) at level 0, otherwise the directory
 * replay made current. Its walk claims its storage, so storage shared with
 * any object walked before, and a directory reached from a second parent,
 * through two link names or through a cycle, are corruption at entry.
 * The scan counts the directory's subdirectories for its link count; the
 * root's include lost+found, which no used slot names.
 */
fsimg_status_t
qnx4_prepare(fsimg_reader_t *reader, fsimg_directory_build_t *build,
    fsimg_error_t *error)
{
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(reader);
	const qnx4_inode_t *directory;
	fsimg_extent_index_t *index;
	qnx4_prepare_t *prepare;
	fsimg_error_t detail;
	fsimg_status_t status;

	prepare = fsimg_alloc(reader->ctx, sizeof(*prepare), &detail);
	if (prepare == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	prepare->reader = reader;
	prepare->build = build;
	prepare->root = fsimg_directory_level(build) == 0;
	prepare->subdirectories = prepare->root ? 1 : 0;
	if (prepare->root) {
		directory = &qnx4->root[QNX4_RECORD_ROOT];
		prepare->self = QNX4_POSITION(QNX4_ROOT_BLOCK, QNX4_RECORD_ROOT);
		prepare->parent = prepare->self;
	} else {
		directory = &qnx4->current;
		prepare->self = qnx4->current_position;
		prepare->parent = qnx4->current_parent;
	}
	index = NULL;
	status = qnx4_index_build(reader, directory, QNX4_WALK_EXCLUSIVE,
	    &index, error);
	if (status == FSIMG_OK)
		status = qnx4_slots_scan(reader, index, directory->size,
		    prepare_slot, prepare, error);
	if (status == FSIMG_OK)
		status = qnx4_verify_directory(reader, directory,
		    prepare->subdirectories, error);
	fsimg_extent_index_free(index);
	fsimg_dealloc(prepare);
	return (status);
}

/* Index the current file, symbolic link or FIFO under policy: exclusive
 * for its first name, allocated for a later name of an .inodes object,
 * whose blocks the first name claimed.
 */
fsimg_status_t
qnx4_index_current(fsimg_reader_t *reader, qnx4_walk_policy_t policy,
    fsimg_error_t *error)
{
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(reader);

	return (qnx4_index_build(reader, &qnx4->current, policy, &qnx4->file,
	    error));
}

/* Make a replayed child current. Its record came from scratch storage, so
 * the payload and name are validated and the record is read again. The
 * first name of a file, symbolic link or FIFO claims its storage at
 * admission, so storage it shares with any other object fails while
 * listing. A later name of the same .inodes object (a hard link) is
 * indexed when first read, so listing many names of a large file walks it
 * once. The exposed bit is set only after the claiming walk succeeds. A
 * direct or root-block record has exactly one name. A directory is indexed
 * on descent.
 */
fsimg_status_t
qnx4_replay(fsimg_reader_t *reader, const char *name,
    const unsigned char *payload, size_t length, fsimg_error_t *error)
{
	fsimg_qnx4_reader_t *qnx4 = qnx4_state(reader);
	qnx4_place_t place;
	uint64_t position, parent, table_slot;
	bool exposed;
	fsimg_status_t status;

	fsimg_extent_index_free(qnx4->file);
	qnx4->file = NULL;
	qnx4->later_name = false;
	if (length != QNX4_PAYLOAD_BYTES)
		return (qnx4_corrupt(error, "invalid prepared QNX4 entry payload"));
	position = fsimg_load_le64(payload);
	parent = fsimg_load_le64(payload + 8);
	if (parent / QNX4_SLOTS_PER_BLOCK > qnx4->geometry.blocks ||
	    position / QNX4_SLOTS_PER_BLOCK > qnx4->geometry.blocks)
		return (qnx4_corrupt(error, "invalid prepared QNX4 entry payload"));
	if (qnx4_name_native((const unsigned char *)name, strlen(name),
	    NULL) != FSIMG_OK)
		return (qnx4_corrupt(error, "invalid prepared QNX4 file name"));
	status = qnx4_object_read(reader, position, &qnx4->current, &place,
	    &table_slot, error);
	if (status != FSIMG_OK)
		return (status);
	status = qnx4_inode_entry(&qnx4->current, &reader->entry, error);
	if (status != FSIMG_OK)
		return (status);
	qnx4->current_position = position;
	qnx4->current_parent = parent;
	if (reader->entry.kind == FSIMG_ENTRY_DIRECTORY)
		return (FSIMG_OK);

	exposed = false;
	if (place == QNX4_PLACE_TABLE) {
		/* qnx4_object_read bounded the slot by the table's size. */
		status = fsimg_bitset_test(qnx4->exposed, table_slot, &exposed,
		    error);
		if (status != FSIMG_OK)
			return (status);
	}
	qnx4->later_name = exposed;
	if (!exposed) {
		status = qnx4_index_current(reader, QNX4_WALK_EXCLUSIVE, error);
		if (status == FSIMG_OK && place == QNX4_PLACE_TABLE)
			status = fsimg_bitset_test_and_set(qnx4->exposed,
			    table_slot, &exposed, error);
		if (status != FSIMG_OK)
			return (status);
	}
	reader->initialized = reader->entry.size;
	return (FSIMG_OK);
}

_Static_assert(QNX4_NAME_MAX <= FSIMG_DIRECTORY_KEY_BYTES,
    "a QNX4 name key fits a directory key buffer");

/* The namespace key is the exact name bytes, raw for an omitted name: QNX4
 * names are compared byte for byte, case preserved and significant.
 */
fsimg_status_t
qnx4_keys(fsimg_reader_t *reader FSIMG_UNUSED, const char *name,
    const unsigned char *payload FSIMG_UNUSED, size_t length FSIMG_UNUSED,
    fsimg_directory_keys_t *keys, fsimg_error_t *error)
{
	size_t bytes = strlen(name);

	keys->count = 0;
	if (qnx4_name_native((const unsigned char *)name, bytes, NULL) !=
	    FSIMG_OK)
		return (qnx4_corrupt(error, "invalid QNX4 file name"));
	bcopy(name, keys->key[0], bytes);
	keys->length[0] = bytes;
	keys->count = 1;
	return (FSIMG_OK);
}

/* A query spells a delivered name, so a component outside the profile
 * matches nothing and is refused before any directory is consulted.
 */
fsimg_status_t
qnx4_key(fsimg_reader_t *reader FSIMG_UNUSED, const char *component,
    unsigned char *key, size_t *length, fsimg_error_t *error)
{
	size_t bytes = strlen(component);

	*length = 0;
	if (bytes > QNX4_NAME_MAX || !fsimg_name_in_qnx_profile(component, bytes))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_READER,
		    "selection component is not representable in QNX4"));
	bcopy(component, key, bytes);
	*length = bytes;
	return (FSIMG_OK);
}
