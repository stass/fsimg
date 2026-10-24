/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "ifs.h"
#include "unicode.h"

/* ImageFS's part of directory preparation (specs/design-directories.md). A
 * directory's children are its run in the child stream that tree.c built.
 * Each admitted child's payload is four little-endian fields: its kind
 * (one byte), its node index (eight bytes), the table offset of its record
 * (four bytes, zero for a synthesized directory) and its record's path
 * length (two bytes, zero when synthesized), which replay checks against
 * the record it reads again.
 */
/* A prepared entry's payload: the child record's kind, node, table offset
 * and path length, which replay checks against the record it rereads.
 */
enum {
	IFS_PAYLOAD_KIND = 0,		/* u8 */
	IFS_PAYLOAD_NODE = 1,		/* u64 */
	IFS_PAYLOAD_TABLE_OFFSET = 9,	/* u32 */
	IFS_PAYLOAD_PATH_LENGTH = 13,	/* u16, little-endian */
	IFS_PAYLOAD_BYTES = 15
};

_Static_assert(IFS_PATH_MAX <= FSIMG_DIRECTORY_KEY_BYTES,
    "an ImageFS component key fits a directory key buffer");
_Static_assert(IFS_PATH_MAX + 1 <= FSIMG_DIRECTORY_NAME_BYTES,
    "an ImageFS component fits a prepared directory record");

static fsimg_status_t admit_child(fsimg_reader_t *, fsimg_directory_build_t *,
    const ifs_child_t *, fsimg_error_t *);
static fsimg_status_t replay_record(fsimg_reader_t *, const char *, size_t,
    uint32_t, uint32_t, ifs_kind_t, fsimg_error_t *);
static void fill_entry(fsimg_reader_t *, ifs_kind_t, const ifs_record_t *);
static fsimg_status_t index_content(fsimg_reader_t *, fsimg_error_t *);

/* Admit one child. Its component came from scratch, so it is checked
 * against the path shapes the table pass enforced; the profile decides its
 * class in common preparation. A path holds no NUL, so the component is a
 * C string once terminated.
 */
static fsimg_status_t
admit_child(fsimg_reader_t *reader, fsimg_directory_build_t *build,
    const ifs_child_t *child, fsimg_error_t *error)
{
	fsimg_ifs_reader_t *ifs = ifs_state(reader);
	unsigned char payload[IFS_PAYLOAD_BYTES];
	char name[IFS_PATH_MAX + 1];

	if (child->node == 0 || child->node >= ifs->node_count ||
	    memchr(child->name, '\0', child->name_length) != NULL ||
	    memchr(child->name, '/', child->name_length) != NULL ||
	    ifs_path_check(child->name, child->name_length, NULL) != FSIMG_OK)
		return (ifs_corrupt(error, "invalid ImageFS child record"));
	bcopy(child->name, name, child->name_length);
	name[child->name_length] = '\0';

	payload[IFS_PAYLOAD_KIND] = (unsigned char)child->kind;
	fsimg_store_le64(payload + IFS_PAYLOAD_NODE, child->node);
	fsimg_store_le32(payload + IFS_PAYLOAD_TABLE_OFFSET,
	    child->table_offset);
	payload[IFS_PAYLOAD_PATH_LENGTH] =
	    (unsigned char)(child->path_length & 0xff);
	payload[IFS_PAYLOAD_PATH_LENGTH + 1] =
	    (unsigned char)(child->path_length >> 8);
	return (fsimg_directory_admit(build, name, payload, sizeof(payload),
	    error));
}

/* Prepare the root (node 0) at level 0, otherwise the directory replay
 * made current, by reading its run of the child stream: the cost is its
 * children, never its subtree.
 */
fsimg_status_t
ifs_prepare(fsimg_reader_t *reader, fsimg_directory_build_t *build,
    fsimg_error_t *error)
{
	fsimg_ifs_reader_t *ifs = ifs_state(reader);
	unsigned char record[IFS_CHILD_BYTES];
	ifs_child_t child;
	uint64_t node, cursor, count;
	size_t length;
	fsimg_status_t status;

	if (fsimg_directory_level(build) == 0)
		node = 0;
	else if (ifs->current.kind == IFS_KIND_DIRECTORY)
		node = ifs->current.node;
	else
		return (ifs_corrupt(error, "prepared ImageFS entry is not a directory"));
	status = ifs_group(reader, node, &cursor, &count, error);
	if (status != FSIMG_OK)
		return (status);
	for (uint64_t i = 0; i < count; i++) {
		status = fsimg_stream_next(ifs->children, &cursor, record,
		    sizeof(record), &length, error);
		if (status == FSIMG_DONE)
			return (ifs_corrupt(error, "invalid ImageFS child group"));
		if (status == FSIMG_OK)
			status = ifs_child_decode(record, length, &child, error);
		if (status == FSIMG_OK && child.parent != node)
			status = ifs_corrupt(error, "invalid ImageFS child group");
		if (status == FSIMG_OK)
			status = admit_child(reader, build, &child, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/* Fill the current entry, except its path, from a validated record, or
 * from the implicit-directory constants when record is NULL. Times are
 * unsigned UTC seconds; directory and FIFO sizes are zero.
 */
static void
fill_entry(fsimg_reader_t *reader, ifs_kind_t kind, const ifs_record_t *record)
{
	fsimg_entry_t *entry = &reader->entry;

	bzero(&entry->metadata, sizeof(entry->metadata));
	entry->size = 0;
	switch (kind) {
	case IFS_KIND_DIRECTORY:
		entry->kind = FSIMG_ENTRY_DIRECTORY;
		break;
	case IFS_KIND_FILE:
		entry->kind = FSIMG_ENTRY_FILE;
		entry->size = record->data_length;
		break;
	case IFS_KIND_SYMLINK:
		entry->kind = FSIMG_ENTRY_SYMLINK;
		entry->size = record->target_length;
		break;
	case IFS_KIND_FIFO:
		entry->kind = FSIMG_ENTRY_FIFO;
		break;
	}
	entry->metadata.valid = FSIMG_META_MODIFIED | FSIMG_META_OWNER |
	    FSIMG_META_MODE;
	entry->metadata.modified = (fsimg_timestamp_t){
		.seconds = record == NULL ? 0 : record->mtime,
		.precision_ns = 1000000000,
		.basis = FSIMG_TIME_UTC
	};
	if (record == NULL) {
		entry->metadata.mode = IFS_IMPLICIT_MODE;
		return;
	}
	entry->metadata.uid = record->uid;
	entry->metadata.gid = record->gid;
	entry->metadata.mode = record->permissions;
}

/* A regular file's content is its one range; a symbolic link's is its
 * target, inside its record. Either becomes a one-extent index over the
 * content domain, which checks it again; an empty file has none.
 */
static fsimg_status_t
index_content(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_ifs_reader_t *ifs = ifs_state(reader);
	const ifs_record_t *record = &ifs->current.record;
	fsimg_extent_t extent;
	fsimg_status_t status;

	if (reader->entry.size == 0)
		return (FSIMG_OK);
	extent.logical_offset = 0;
	extent.length = reader->entry.size;
	if (record->kind == IFS_KIND_FILE)
		extent.image_offset = record->data_offset;
	else
		extent.image_offset = (uint64_t)ifs->current.table_offset +
		    record->target_offset;
	status = fsimg_extent_index_new(&reader->session,
	    &reader->content_domain, &ifs->file, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_extent_index_append(ifs->file, &extent, error);
	if (status != FSIMG_OK)
		return (status);
	reader->initialized = reader->entry.size;
	return (FSIMG_OK);
}

/* Read a child's record again and check that it is the record preparation
 * admitted: the kind and the path length agree with the payload, and the
 * path's last component is the child's name.
 */
static fsimg_status_t
replay_record(fsimg_reader_t *reader, const char *name, size_t name_length,
    uint32_t table_offset, uint32_t path_length, ifs_kind_t kind,
    fsimg_error_t *error)
{
	fsimg_ifs_reader_t *ifs = ifs_state(reader);
	ifs_record_t *record = &ifs->current.record;
	const unsigned char *path;
	fsimg_status_t status;

	status = ifs_record_read(reader, table_offset, record, error);
	if (status != FSIMG_OK)
		return (status);
	path = ifs->record + record->path_offset;
	if (record->kind != kind || record->path_length != path_length ||
	    path_length < name_length ||
	    bcmp(path + path_length - name_length, name, name_length) != 0 ||
	    (path_length > name_length &&
	    path[path_length - name_length - 1] != '/'))
		return (ifs_corrupt(error, "invalid prepared ImageFS entry"));
	return (FSIMG_OK);
}

/* Make a replayed child current. Its payload came from scratch storage, so
 * every field is validated and the record is read and decoded again; a
 * synthesized directory takes the implicit-directory constants.
 */
fsimg_status_t
ifs_replay(fsimg_reader_t *reader, const char *name,
    const unsigned char *payload, size_t length, fsimg_error_t *error)
{
	fsimg_ifs_reader_t *ifs = ifs_state(reader);
	ifs_current_t *current = &ifs->current;
	size_t name_length;
	uint32_t path_length;
	fsimg_status_t status;

	fsimg_extent_index_free(ifs->file);
	ifs->file = NULL;
	bzero(current, sizeof(*current));
	if (length != IFS_PAYLOAD_BYTES ||
	    payload[IFS_PAYLOAD_KIND] < IFS_KIND_DIRECTORY ||
	    payload[IFS_PAYLOAD_KIND] > IFS_KIND_FIFO)
		return (ifs_corrupt(error, "invalid prepared ImageFS entry payload"));
	current->kind = (ifs_kind_t)payload[IFS_PAYLOAD_KIND];
	current->node = fsimg_load_le64(payload + IFS_PAYLOAD_NODE);
	current->table_offset = fsimg_load_le32(payload +
	    IFS_PAYLOAD_TABLE_OFFSET);
	path_length = (uint32_t)payload[IFS_PAYLOAD_PATH_LENGTH] |
	    (uint32_t)payload[IFS_PAYLOAD_PATH_LENGTH + 1] << 8;
	name_length = strlen(name);
	if (current->node == 0 || current->node >= ifs->node_count ||
	    name_length == 0 || name_length > IFS_PATH_MAX ||
	    (current->table_offset == 0 &&
	    (current->kind != IFS_KIND_DIRECTORY || path_length != 0)))
		return (ifs_corrupt(error, "invalid prepared ImageFS entry payload"));

	if (current->table_offset == 0) {
		fill_entry(reader, IFS_KIND_DIRECTORY, NULL);
		return (FSIMG_OK);
	}
	status = replay_record(reader, name, name_length, current->table_offset,
	    path_length, current->kind, error);
	if (status != FSIMG_OK)
		return (status);
	fill_entry(reader, current->kind, &current->record);
	return (index_content(reader, error));
}

/* The namespace key is the exact component bytes, raw for an omitted name:
 * ImageFS compares components byte for byte, case preserved and
 * significant. Equal paths collapsed into one entry in the child pass, so
 * two children of a directory never share a key.
 */
fsimg_status_t
ifs_keys(fsimg_reader_t *reader FSIMG_UNUSED, const char *name,
    const unsigned char *payload FSIMG_UNUSED, size_t length FSIMG_UNUSED,
    fsimg_directory_keys_t *keys, fsimg_error_t *error)
{
	size_t bytes = strlen(name);

	keys->count = 0;
	if (bytes == 0 || bytes > IFS_PATH_MAX)
		return (ifs_corrupt(error, "invalid ImageFS component"));
	bcopy(name, keys->key[0], bytes);
	keys->length[0] = bytes;
	keys->count = 1;
	return (FSIMG_OK);
}

/* A query spells a delivered name, so a component outside the profile
 * matches nothing and is refused before any directory is consulted.
 */
fsimg_status_t
ifs_key(fsimg_reader_t *reader FSIMG_UNUSED, const char *component,
    unsigned char *key, size_t *length, fsimg_error_t *error)
{
	size_t bytes = strlen(component);

	*length = 0;
	if (bytes > IFS_PATH_MAX ||
	    !fsimg_name_in_qnx_profile(component, bytes))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_READER,
		    "selection component is not representable in ImageFS"));
	bcopy(component, key, bytes);
	*length = bytes;
	return (FSIMG_OK);
}
