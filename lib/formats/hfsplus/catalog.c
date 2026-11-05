/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdio.h>
#include <strings.h>

#include "hfsplus.h"

/* The closed table of a record's flag word: locked, file thread, attributes,
 * security, hard-link chain, directory hard link inside, date added and the
 * four Fusion hints. The folder count bit is a folder's alone.
 */
#define FLAG_LINK_CHAIN		0x0020u
#define FLAGS_ACCEPTED		0x0fefu
#define FLAG_EXPANDED_TIMES	0x1000u

#define MODE_TYPE		0170000u
#define MODE_FIFO		0010000u
#define MODE_CHAR_DEVICE	0020000u
#define MODE_DIRECTORY		0040000u
#define MODE_BLOCK_DEVICE	0060000u
#define MODE_REGULAR		0100000u
#define MODE_SYMLINK		0120000u
#define MODE_SOCKET		0140000u

/* "iNode" and the reference in decimal, ten digits at most. */
#define INODE_NAME_UNITS_MAX	15u

/* A stub's Finder type and creator, compared as eight stored bytes. */
#define FINDER_CODES_BYTES	8u
static const unsigned char file_stub_codes[FINDER_CODES_BYTES] = {
	'h', 'l', 'n', 'k', 'h', 'f', 's', '+'
};
static const unsigned char directory_stub_codes[FINDER_CODES_BYTES] = {
	'f', 'd', 'r', 'p', 'M', 'A', 'C', 'S'
};

static fsimg_status_t flags_check(uint16_t, bool, fsimg_error_t *);
static fsimg_status_t folder_fields_check(const hfsplus_folder_t *,
    fsimg_error_t *);
static fsimg_status_t folder_validate(const hfsplus_record_t *,
    hfsplus_primary_t *, fsimg_error_t *);
static fsimg_status_t file_kind(const hfsplus_file_t *, fsimg_entry_kind_t *,
    fsimg_error_t *);
static fsimg_status_t kind_rules_check(const hfsplus_file_t *,
    fsimg_entry_kind_t, fsimg_error_t *);
static fsimg_status_t file_validate(fsimg_reader_t *, const hfsplus_record_t *,
    hfsplus_primary_t *, fsimg_error_t *);
static fsimg_status_t link_kind(fsimg_reader_t *, const hfsplus_file_t *,
    hfsplus_link_t *, fsimg_error_t *);
static bool fork_empty(const hfsplus_fork_t *);
static fsimg_status_t stub_check(const fsimg_hfsplus_reader_t *,
    const hfsplus_file_t *, fsimg_error_t *);

/* The flag word is a closed table; a bit outside it, the expanded timestamps
 * bit among them, changes interpretation in a way the reader does not know.
 */
static fsimg_status_t
flags_check(uint16_t flags, bool folder, fsimg_error_t *error)
{
	uint16_t accepted = FLAGS_ACCEPTED;

	if ((flags & FLAG_EXPANDED_TIMES) != 0)
		return (hfsplus_unsupported(error,
		    "HFS Plus catalog record has expanded timestamps"));
	if (folder)
		accepted |= HFSPLUS_FLAG_FOLDER_COUNT;
	if ((flags & ~accepted) != 0)
		return (hfsplus_unsupported(error,
		    "HFS Plus catalog record has an unknown flag"));
	return (FSIMG_OK);
}

/* The fields the root and every other folder share: the flag word and the kind
 * table's folder rows.
 */
static fsimg_status_t
folder_fields_check(const hfsplus_folder_t *folder, fsimg_error_t *error)
{
	fsimg_status_t status;

	status = flags_check(folder->flags, true, error);
	if (status != FSIMG_OK)
		return (status);
	if (folder->bsd.file_mode != 0 &&
	    (folder->bsd.file_mode & MODE_TYPE) != MODE_DIRECTORY)
		return (hfsplus_corrupt(error,
		    "HFS Plus folder's mode is not a directory's"));
	return (FSIMG_OK);
}

fsimg_status_t
hfsplus_root_validate(const hfsplus_folder_t *root, fsimg_error_t *error)
{
	return (folder_fields_check(root, error));
}

static fsimg_status_t
folder_validate(const hfsplus_record_t *record, hfsplus_primary_t *primary,
    fsimg_error_t *error)
{
	hfsplus_folder_t *folder = &primary->folder;
	fsimg_status_t status;

	if (!hfsplus_folder_decode(record->data, record->data_bytes, folder))
		return (hfsplus_corrupt(error,
		    "HFS Plus folder record is malformed"));
	status = folder_fields_check(folder, error);
	if (status != FSIMG_OK)
		return (status);
	if (folder->id < HFSPLUS_ID_FIRST_USER)
		return (hfsplus_corrupt(error,
		    "HFS Plus folder has a reserved node ID"));
	primary->kind = FSIMG_ENTRY_DIRECTORY;
	return (FSIMG_OK);
}

/* The kind table's file rows. A zero mode is a regular file with no owner or
 * mode; a type of zero with permission bits is no row of the table.
 */
static fsimg_status_t
file_kind(const hfsplus_file_t *file, fsimg_entry_kind_t *kind,
    fsimg_error_t *error)
{
	if (file->bsd.file_mode == 0) {
		*kind = FSIMG_ENTRY_FILE;
		return (FSIMG_OK);
	}
	switch (file->bsd.file_mode & MODE_TYPE) {
	case MODE_REGULAR:
		*kind = FSIMG_ENTRY_FILE;
		return (FSIMG_OK);
	case MODE_SYMLINK:
		*kind = FSIMG_ENTRY_SYMLINK;
		return (FSIMG_OK);
	case MODE_FIFO:
		*kind = FSIMG_ENTRY_FIFO;
		return (FSIMG_OK);
	case MODE_SOCKET:
		*kind = FSIMG_ENTRY_SOCKET;
		return (FSIMG_OK);
	case MODE_CHAR_DEVICE:
		*kind = FSIMG_ENTRY_CHAR_DEVICE;
		return (FSIMG_OK);
	case MODE_BLOCK_DEVICE:
		*kind = FSIMG_ENTRY_BLOCK_DEVICE;
		return (FSIMG_OK);
	default:
		return (hfsplus_corrupt(error,
		    "HFS Plus file's mode is not in the kind table"));
	}
}

/* What the format design requires of a kind's forks. A regular file's rules are
 * compression's, which need its attributes: hfsplus_record_attributes(). A FIFO
 * has none: its forks are claimed like any file's and nothing is delivered.
 */
static fsimg_status_t
kind_rules_check(const hfsplus_file_t *file, fsimg_entry_kind_t kind,
    fsimg_error_t *error)
{
	const hfsplus_fork_t *data = &file->data_fork;
	const hfsplus_fork_t *resource = &file->resource_fork;

	switch (kind) {
	case FSIMG_ENTRY_SYMLINK:
		if (data->logical_size == 0 ||
		    data->logical_size > HFSPLUS_LINK_TARGET_MAX)
			return (hfsplus_unsupported(error,
			    "HFS Plus symbolic link's target length is not 1 to 1023 bytes"));
		if (!fork_empty(resource))
			return (hfsplus_corrupt(error,
			    "HFS Plus symbolic link has a resource fork"));
		return (FSIMG_OK);
	case FSIMG_ENTRY_SOCKET:
	case FSIMG_ENTRY_CHAR_DEVICE:
	case FSIMG_ENTRY_BLOCK_DEVICE:
		if (!fork_empty(data) || !fork_empty(resource))
			return (hfsplus_corrupt(error,
			    "HFS Plus socket or device has a nonempty fork"));
		return (FSIMG_OK);
	default:
		return (FSIMG_OK);
	}
}

/* The stub tests run first: a stub's fileMode is 0100000 on current macOS and
 * 0 on the oldest discs, so it says nothing of the entry's kind.
 */
static fsimg_status_t
file_validate(fsimg_reader_t *reader, const hfsplus_record_t *record,
    hfsplus_primary_t *primary, fsimg_error_t *error)
{
	hfsplus_file_t *file = &primary->file;
	fsimg_status_t status;

	if (!hfsplus_file_decode(record->data, record->data_bytes, file))
		return (hfsplus_corrupt(error,
		    "HFS Plus file record is malformed"));
	status = flags_check(file->flags, false, error);
	if (status != FSIMG_OK)
		return (status);
	if (file->id < HFSPLUS_ID_FIRST_USER)
		return (hfsplus_corrupt(error,
		    "HFS Plus file has a reserved node ID"));
	status = link_kind(reader, file, &primary->link, error);
	if (status != FSIMG_OK)
		return (status);
	if (primary->link == HFSPLUS_LINK_FILE)
		return (stub_check(hfsplus_state(reader), file, error));
	if (primary->link != HFSPLUS_LINK_NONE)
		return (FSIMG_OK);
	status = file_kind(file, &primary->kind, error);
	if (status != FSIMG_OK)
		return (status);
	return (kind_rules_check(file, primary->kind, error));
}

fsimg_status_t
hfsplus_record_validate(fsimg_reader_t *reader, const hfsplus_record_t *record,
    hfsplus_primary_t *primary, fsimg_error_t *error)
{
	uint16_t type;

	primary->kind = 0;
	primary->link = HFSPLUS_LINK_NONE;
	if (!hfsplus_catalog_type(record->data, record->data_bytes, &type))
		return (hfsplus_corrupt(error,
		    "HFS Plus catalog record is too short for its type"));
	switch (type) {
	case HFSPLUS_RECORD_FOLDER:
		return (folder_validate(record, primary, error));
	case HFSPLUS_RECORD_FILE:
		return (file_validate(reader, record, primary, error));
	default:
		return (hfsplus_corrupt(error,
		    "HFS Plus catalog record is not a folder or file"));
	}
}

/* The one route to the file hard-link data folder: a keyed search in the root
 * by its exact stored units. A case variant a folded search lands on is no
 * data folder, and may never host indirect nodes. The cost falls only on a
 * volume holding a record with a stub's Finder codes.
 */
fsimg_status_t
hfsplus_private_data_lookup(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_hfsplus_reader_t *hfsplus = hfsplus_state(reader);
	const hfsplus_owned_name_t *name =
	    &hfsplus_owned_names[HFSPLUS_OWNED_PRIVATE_DATA];
	hfsplus_folder_t folder;
	hfsplus_record_t record;
	hfsplus_key_t key;
	bool found;
	fsimg_status_t status;

	status = hfsplus_catalog_key_init(&key, HFSPLUS_ID_ROOT, name->units,
	    name->count, error);
	if (status == FSIMG_OK)
		status = hfsplus_catalog_find_exact(&hfsplus->catalog, &key,
		    &record, &found, error);
	if (status != FSIMG_OK)
		return (status);
	if (found) {
		if (!hfsplus_folder_decode(record.data, record.data_bytes,
		    &folder))
			return (hfsplus_corrupt(error,
			    "HFS Plus private data folder is not a folder"));
		hfsplus->private_data_present = true;
		hfsplus->private_data_id = folder.id;
		hfsplus->private_data_date = folder.dates.created;
	}
	hfsplus->private_data_known = true;
	return (FSIMG_OK);
}

/* The stub tests of the format design, the macOS filesystem's: the Finder
 * codes, and a createDate equal to the root folder's or the file hard-link
 * data folder's, which both stub kinds carry. Flag 0x0020 is part of the
 * directory stub's test alone. Anything else with those codes, a Finder alias
 * among them, is an ordinary file. A record without the codes costs nothing.
 */
static fsimg_status_t
link_kind(fsimg_reader_t *reader, const hfsplus_file_t *file,
    hfsplus_link_t *link, fsimg_error_t *error)
{
	fsimg_hfsplus_reader_t *hfsplus = hfsplus_state(reader);
	bool file_codes, directory_codes, dated;
	fsimg_status_t status;

	*link = HFSPLUS_LINK_NONE;
	file_codes = bcmp(file->finder_info, file_stub_codes,
	    FINDER_CODES_BYTES) == 0;
	directory_codes = bcmp(file->finder_info, directory_stub_codes,
	    FINDER_CODES_BYTES) == 0;
	if (!file_codes && !directory_codes)
		return (FSIMG_OK);
	if (!hfsplus->private_data_known) {
		status = hfsplus_private_data_lookup(reader, error);
		if (status != FSIMG_OK)
			return (status);
	}
	dated = file->dates.created == hfsplus->root.dates.created ||
	    (hfsplus->private_data_present &&
	    file->dates.created == hfsplus->private_data_date);
	if (!dated)
		return (FSIMG_OK);
	if (file_codes)
		*link = HFSPLUS_LINK_FILE;
	else if ((file->flags & FLAG_LINK_CHAIN) != 0)
		*link = HFSPLUS_LINK_DIRECTORY;
	return (FSIMG_OK);
}

static bool
fork_empty(const hfsplus_fork_t *fork)
{
	return (fork->logical_size == 0 && fork->total_blocks == 0);
}

/* What a file stub must be whatever resolves it, so every validated record is held
 * to it, one no walk reaches included.
 */
static fsimg_status_t
stub_check(const fsimg_hfsplus_reader_t *hfsplus, const hfsplus_file_t *stub,
    fsimg_error_t *error)
{
	if (stub->bsd.special == 0)
		return (hfsplus_corrupt(error,
		    "HFS Plus hard-link stub has a zero reference"));
	if (!fork_empty(&stub->data_fork) || !fork_empty(&stub->resource_fork))
		return (hfsplus_corrupt(error,
		    "HFS Plus hard-link stub has a nonempty fork"));
	if (!hfsplus->private_data_present)
		return (hfsplus_corrupt(error,
		    "HFS Plus hard-link stub on a volume with no private data folder"));
	return (FSIMG_OK);
}

/* The chain fields, the indirect node's reserved word and the stub's owner and
 * group, are never read: the reference in special names the node. The name's
 * spelling is exact, since a folded search would also land on "inode7".
 */
fsimg_status_t
hfsplus_link_resolve(fsimg_reader_t *reader, hfsplus_primary_t *primary,
    hfsplus_cursor_t *cursor, fsimg_error_t *error)
{
	fsimg_hfsplus_reader_t *hfsplus = hfsplus_state(reader);
	const hfsplus_file_t *stub = &primary->file;
	unsigned char units[2 * INODE_NAME_UNITS_MAX];
	char text[INODE_NAME_UNITS_MAX + 1];
	hfsplus_record_t record;
	hfsplus_key_t key;
	size_t count;
	bool found;
	fsimg_status_t status;

	count = (size_t)snprintf(text, sizeof(text), "iNode%u",
	    (unsigned)stub->bsd.special);
	for (size_t i = 0; i < count; i++) {
		units[2 * i] = 0;
		units[2 * i + 1] = (unsigned char)text[i];
	}
	hfsplus_cursor_init(cursor, &hfsplus->catalog,
	    hfsplus->catalog.search_node);
	status = hfsplus_catalog_key_init(&key, hfsplus->private_data_id, units,
	    count, error);
	if (status == FSIMG_OK)
		status = hfsplus_catalog_locate(cursor, &key, &record, &found,
		    error);
	if (status != FSIMG_OK)
		return (status);
	if (!found)
		return (hfsplus_corrupt(error,
		    "HFS Plus hard-link stub's indirect node is missing"));
	status = hfsplus_record_validate(reader, &record, primary, error);
	if (status != FSIMG_OK)
		return (status);
	return (hfsplus_indirect_node_check(primary, error));
}

fsimg_status_t
hfsplus_indirect_node_check(const hfsplus_primary_t *primary,
    fsimg_error_t *error)
{
	if (primary->link != HFSPLUS_LINK_NONE ||
	    (primary->kind != FSIMG_ENTRY_FILE &&
	    primary->kind != FSIMG_ENTRY_SYMLINK))
		return (hfsplus_corrupt(error,
		    "HFS Plus indirect node is a stub or of another kind"));
	return (FSIMG_OK);
}

fsimg_status_t
hfsplus_thread_lookup(hfsplus_cursor_t *cursor, uint32_t id, uint16_t type,
    hfsplus_thread_t *thread, fsimg_error_t *error)
{
	hfsplus_record_t record;
	hfsplus_ckey_t stored;
	hfsplus_key_t key;
	fsimg_status_t status;

	status = hfsplus_catalog_key_init(&key, id, NULL, 0, error);
	if (status == FSIMG_OK)
		status = hfsplus_btree_search(cursor, &key, error);
	if (status != FSIMG_OK)
		return (status);
	if (!cursor->valid)
		return (hfsplus_corrupt(error, "HFS Plus thread record is missing"));
	status = hfsplus_cursor_record(cursor, &record, error);
	if (status != FSIMG_OK)
		return (status);
	if (!hfsplus_ckey_decode(record.key, record.key_bytes, &stored))
		return (hfsplus_corrupt(error, "HFS Plus key is malformed"));
	if (stored.parent != id || stored.units != 0)
		return (hfsplus_corrupt(error, "HFS Plus thread record is missing"));
	if (!hfsplus_thread_decode(record.data, record.data_bytes, thread) ||
	    thread->type != type)
		return (hfsplus_corrupt(error,
		    "HFS Plus thread record is malformed or of another kind"));
	return (FSIMG_OK);
}

void
hfsplus_child_fill(const hfsplus_primary_t *primary, hfsplus_child_t *child)
{
	const hfsplus_file_t *file = &primary->file;
	const hfsplus_folder_t *folder = &primary->folder;
	bool directory = primary->kind == FSIMG_ENTRY_DIRECTORY;
	const hfsplus_bsd_t *bsd = directory ? &folder->bsd : &file->bsd;

	bzero(child, sizeof(*child));
	child->kind = primary->kind;
	child->id = directory ? folder->id : file->id;
	child->dates = directory ? folder->dates : file->dates;
	if (bsd->file_mode != 0) {
		child->flags |= HFSPLUS_CHILD_OWNER_MODE;
		child->owner = bsd->owner_id;
		child->group = bsd->group_id;
		child->mode = bsd->file_mode & FSIMG_MODE_BITS;
	}
	switch (primary->kind) {
	case FSIMG_ENTRY_DIRECTORY:
		child->valence = folder->valence;
		if ((folder->flags & HFSPLUS_FLAG_FOLDER_COUNT) != 0) {
			child->flags |= HFSPLUS_CHILD_FOLDER_COUNT;
			child->folder_count = folder->folder_count;
		}
		break;
	case FSIMG_ENTRY_FILE:
	case FSIMG_ENTRY_SYMLINK:
		child->size = file->data_fork.logical_size;
		break;
	case FSIMG_ENTRY_CHAR_DEVICE:
	case FSIMG_ENTRY_BLOCK_DEVICE:
		child->special = bsd->special;
		break;
	default:
		break;
	}
}
