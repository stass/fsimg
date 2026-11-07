/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "erofs.h"
#include "fsimg-endian.h"
#include "unicode.h"

/*
 * EROFS's part of directory preparation. A directory's content is a run of
 * blocks, every one B bytes but the last, each an array of 12-byte entries
 * followed by their names. Names, "." and ".." among them, increase
 * strictly in byte order across the whole directory, so preparation carries
 * the previous name across each block boundary.
 *
 * The parent rule replaces a visited set. A directory D prepared from
 * parent P must name itself by "." and P by "..", and no other entry may
 * name D or P. Each directory reached therefore has exactly one parent, the
 * one the walk came from, so no directory can be its own ancestor and the
 * walk cannot cycle. The root is its own parent.
 *
 * Each admitted child's payload is its NID, the NID of the directory listing
 * it, which becomes the child's parent when it is a directory, and the
 * entry's file type, which replay checks against the child's mode.
 */
#define PAYLOAD_NID		0u
#define PAYLOAD_PARENT		8u
#define PAYLOAD_FILE_TYPE	16u
#define PAYLOAD_BYTES		17u

_Static_assert(PAYLOAD_BYTES <= FSIMG_DIRECTORY_PAYLOAD_BYTES,
    "an EROFS payload fits a directory record");
_Static_assert(EROFS_NAME_MAX <= FSIMG_DIRECTORY_KEY_BYTES,
    "an EROFS name key fits a directory key buffer");

/*
 * One directory under preparation: the directory and its parent, the block
 * being parsed, and the previous name in directory order, absent before the
 * first. The dot flags record which dot entries the directory listed. name
 * is the NUL-terminated copy of a child's name that admission takes.
 */
typedef struct erofs_scan {
	fsimg_reader_t	*reader;
	fsimg_directory_build_t *build;
	erofs_inode_t	 directory;
	uint64_t	 parent;
	size_t		 previous_length;
	bool		 has_previous;
	bool		 dot_seen;
	bool		 dotdot_seen;
	unsigned char	 block[1u << EROFS_BLKSZBITS_MAX];
	unsigned char	 previous[EROFS_NAME_MAX];
	char		 name[EROFS_NAME_MAX + 1];
} erofs_scan_t;

/* An entry's file type, as a kind; EROFS_FT_UNKNOWN defers to the mode. */
static const fsimg_entry_kind_t file_type_kinds[] = {
	[EROFS_FT_REGULAR] = FSIMG_ENTRY_FILE,
	[EROFS_FT_DIRECTORY] = FSIMG_ENTRY_DIRECTORY,
	[EROFS_FT_CHAR_DEVICE] = FSIMG_ENTRY_CHAR_DEVICE,
	[EROFS_FT_BLOCK_DEVICE] = FSIMG_ENTRY_BLOCK_DEVICE,
	[EROFS_FT_FIFO] = FSIMG_ENTRY_FIFO,
	[EROFS_FT_SOCKET] = FSIMG_ENTRY_SOCKET,
	[EROFS_FT_SYMLINK] = FSIMG_ENTRY_SYMLINK
};

static fsimg_status_t	blocks_scan(erofs_scan_t *, fsimg_error_t *);
static fsimg_status_t	child_admit(erofs_scan_t *, const erofs_dirent_t *,
			    const unsigned char *, fsimg_error_t *);
static fsimg_status_t	dot_check(const erofs_dirent_t *, uint64_t,
			    fsimg_error_t *);
static fsimg_status_t	entry_take(erofs_scan_t *, const erofs_dirblock_t *,
			    size_t, fsimg_error_t *);
static fsimg_status_t	entry_fill(fsimg_reader_t *, const erofs_inode_t *,
			    bool, fsimg_error_t *);
static bool		file_type_matches(uint8_t, fsimg_entry_kind_t);
static bool		name_native(const unsigned char *, size_t);
static fsimg_status_t	name_order_check(erofs_scan_t *, const unsigned char *,
			    size_t, fsimg_error_t *);
static fsimg_status_t	symlink_target_declare(fsimg_reader_t *,
			    const erofs_inode_t *, fsimg_error_t *);
static fsimg_timestamp_t time_of(const erofs_inode_t *);

/*
 * Prepare the root at level 0, its own parent, and otherwise the directory
 * replay made current, with the parent its payload named: the walker
 * prepares a directory below the root right after replaying its entry, so
 * current is that directory. Both dot entries
 * must be listed, except "." in a directory whose format word omits it.
 */
fsimg_status_t
erofs_prepare(fsimg_reader_t *reader, fsimg_directory_build_t *build,
    fsimg_error_t *error)
{
	fsimg_erofs_reader_t *erofs = erofs_state(reader);
	erofs_scan_t *scan;
	fsimg_error_t detail;
	fsimg_status_t status;

	scan = fsimg_alloc(reader->ctx, sizeof(*scan), &detail);
	if (scan == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	bzero(scan, sizeof(*scan));
	scan->reader = reader;
	scan->build = build;

	if (fsimg_directory_level(build) == 0) {
		status = erofs_inode_load(reader, erofs->super.root_nid,
		    &scan->directory, error);
		if (status != FSIMG_OK)
			goto done;
		scan->parent = erofs->super.root_nid;
	} else {
		scan->directory = erofs->current;
		scan->parent = erofs->current_parent;
	}
	if (scan->directory.kind != FSIMG_ENTRY_DIRECTORY) {
		status = erofs_reject(error, FSIMG_ECORRUPT,
		    "directory inode is not a directory");
		goto done;
	}

	status = blocks_scan(scan, error);
	if (status != FSIMG_OK)
		goto done;
	if (!scan->dot_seen && !scan->directory.omits_dot)
		status = erofs_reject(error, FSIMG_ECORRUPT,
		    "directory without \".\"");
	else if (!scan->dotdot_seen)
		status = erofs_reject(error, FSIMG_ECORRUPT,
		    "directory without \"..\"");
done:
	fsimg_dealloc(scan);
	return (status);
}

/*
 * Read the directory block by block, each its valid length: B bytes, or
 * what remains of the size in the last block. A block is decoded whole
 * before any of its entries is taken, and each block read is one unit of
 * work, so a directory naming a shared subtree many times exhausts the
 * work budget rather than running unbounded.
 */
static fsimg_status_t
blocks_scan(erofs_scan_t *scan, fsimg_error_t *error)
{
	fsimg_reader_t *reader = scan->reader;
	uint32_t block_bytes = erofs_state(reader)->block_bytes;
	erofs_dirblock_t block;
	uint64_t offset, remaining;
	const char *what;
	size_t valid, index;
	fsimg_status_t status;

	for (offset = 0; offset < scan->directory.size; offset += valid) {
		remaining = scan->directory.size - offset;
		valid = remaining < block_bytes ? (size_t)remaining :
		    block_bytes;
		status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
		if (status != FSIMG_OK)
			return (status);
		status = erofs_content_read(reader, &scan->directory, offset,
		    valid, scan->block, NULL, error);
		if (status != FSIMG_OK)
			return (status);
		status = erofs_dirblock_decode(scan->block, valid, &block,
		    &what);
		if (status != FSIMG_OK)
			return (erofs_reject(error, status, what));
		for (index = 0; index < block.count; index++) {
			status = entry_take(scan, &block, index, error);
			if (status != FSIMG_OK)
				return (status);
		}
	}
	return (FSIMG_OK);
}

/*
 * Take entry index of a decoded block: check its name's place in the
 * directory order, then check a dot entry under the parent rule, or admit
 * any other entry as a child.
 */
static fsimg_status_t
entry_take(erofs_scan_t *scan, const erofs_dirblock_t *block, size_t index,
    fsimg_error_t *error)
{
	const unsigned char *name;
	erofs_dirent_t dirent;
	const char *what;
	fsimg_status_t status;

	status = erofs_dirent_decode(block, index, &dirent, &what);
	if (status != FSIMG_OK)
		return (erofs_reject(error, status, what));
	name = block->bytes + dirent.name_offset;
	status = name_order_check(scan, name, dirent.name_length, error);
	if (status != FSIMG_OK)
		return (status);

	if (dirent.name_length == 1 && name[0] == '.') {
		if (scan->directory.omits_dot)
			return (erofs_reject(error, FSIMG_ECORRUPT,
			    "\".\" in a directory that omits it"));
		scan->dot_seen = true;
		return (dot_check(&dirent, scan->directory.nid, error));
	}
	if (dirent.name_length == 2 && name[0] == '.' && name[1] == '.') {
		scan->dotdot_seen = true;
		return (dot_check(&dirent, scan->parent, error));
	}
	if (dirent.nid == scan->directory.nid || dirent.nid == scan->parent)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "directory entry naming the directory or its parent"));
	return (child_admit(scan, &dirent, name, error));
}

/*
 * The name must follow the previous one strictly in byte order: a shorter
 * name that is a prefix of a longer one sorts first. It then becomes the
 * previous name, which is how order carries across a block boundary.
 */
static fsimg_status_t
name_order_check(erofs_scan_t *scan, const unsigned char *name, size_t length,
    fsimg_error_t *error)
{
	size_t common;
	int order;

	if (scan->has_previous) {
		common = scan->previous_length < length ?
		    scan->previous_length : length;
		order = memcmp(scan->previous, name, common);
		if (order > 0 || (order == 0 && scan->previous_length >= length))
			return (erofs_reject(error, FSIMG_ECORRUPT,
			    "directory names out of order"));
	}
	bcopy(name, scan->previous, length);
	scan->previous_length = length;
	scan->has_previous = true;
	return (FSIMG_OK);
}

/*
 * A dot entry names expected, the directory for "." and its parent for "..",
 * both directories, so a file type other than unknown or directory
 * contradicts it.
 */
static fsimg_status_t
dot_check(const erofs_dirent_t *dirent, uint64_t expected, fsimg_error_t *error)
{
	if (dirent->nid != expected)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "dot entry breaks the parent rule"));
	if (dirent->file_type != EROFS_FT_UNKNOWN &&
	    dirent->file_type != EROFS_FT_DIRECTORY)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "dot entry file type"));
	return (FSIMG_OK);
}

/*
 * Admit one child under its raw name, which the block decoder proved 1 to
 * 255 bytes without "/" or NUL, and which is neither dot entry. Admission
 * classes it by the portable profile; the child's inode is read at replay.
 */
static fsimg_status_t
child_admit(erofs_scan_t *scan, const erofs_dirent_t *dirent,
    const unsigned char *name, fsimg_error_t *error)
{
	unsigned char payload[PAYLOAD_BYTES];

	bcopy(name, scan->name, dirent->name_length);
	scan->name[dirent->name_length] = '\0';
	fsimg_store_le64(payload + PAYLOAD_NID, dirent->nid);
	fsimg_store_le64(payload + PAYLOAD_PARENT, scan->directory.nid);
	payload[PAYLOAD_FILE_TYPE] = dirent->file_type;
	return (fsimg_directory_admit(scan->build, scan->name, payload,
	    sizeof(payload), error));
}

/*
 * Make a replayed child current. Its record came back from scratch storage,
 * so its name and payload are checked again before the inode they name is
 * loaded. The entry's file type must agree with the inode's mode, and the
 * attribute region must be valid. A symbolic link's target is read and
 * checked now; a regular file's content is resolved when it is read.
 */
fsimg_status_t
erofs_replay(fsimg_reader_t *reader, const char *name,
    const unsigned char *payload, size_t length, fsimg_error_t *error)
{
	fsimg_erofs_reader_t *erofs = erofs_state(reader);
	erofs_inode_t inode;
	uint64_t nid, parent;
	uint8_t file_type;
	bool attributes;
	fsimg_status_t status;

	if (length != PAYLOAD_BYTES ||
	    !name_native((const unsigned char *)name, strlen(name)))
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "prepared directory entry"));
	nid = fsimg_load_le64(payload + PAYLOAD_NID);
	parent = fsimg_load_le64(payload + PAYLOAD_PARENT);
	file_type = payload[PAYLOAD_FILE_TYPE];
	if (file_type > EROFS_FT_SYMLINK)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "prepared directory entry"));

	status = erofs_inode_load(reader, nid, &inode, error);
	if (status != FSIMG_OK)
		return (status);
	if (!file_type_matches(file_type, inode.kind))
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "directory file type disagrees with the inode's mode"));
	status = erofs_xattr_check(reader, &inode, &attributes, error);
	if (status != FSIMG_OK)
		return (status);

	status = entry_fill(reader, &inode, attributes, error);
	if (status != FSIMG_OK)
		return (status);
	erofs->current = inode;
	erofs->current_parent = parent;
	return (FSIMG_OK);
}

static bool
file_type_matches(uint8_t file_type, fsimg_entry_kind_t kind)
{
	return (file_type == EROFS_FT_UNKNOWN ||
	    file_type_kinds[file_type] == kind);
}

/*
 * Fill the current entry from a loaded inode. Only a regular file and a
 * symbolic link report a size; a directory's size is its directory data,
 * which the entry model does not report. The one stored time is a
 * modification time under MTIME and a change time otherwise.
 */
static fsimg_status_t
entry_fill(fsimg_reader_t *reader, const erofs_inode_t *inode,
    bool attributes, fsimg_error_t *error)
{
	fsimg_metadata_t *metadata = &reader->entry.metadata;
	uint32_t major, minor;

	bzero(metadata, sizeof(*metadata));
	reader->entry.kind = inode->kind;
	reader->entry.size = 0;
	metadata->valid = FSIMG_META_OWNER | FSIMG_META_MODE;
	metadata->uid = inode->uid;
	metadata->gid = inode->gid;
	metadata->mode = inode->permissions;
	if (erofs_state(reader)->features.mtime) {
		metadata->valid |= FSIMG_META_MODIFIED;
		metadata->modified = time_of(inode);
	} else {
		metadata->valid |= FSIMG_META_CHANGED;
		metadata->changed = time_of(inode);
	}
	if (attributes)
		metadata->valid |= FSIMG_META_EXTRA_DATA;

	switch (inode->kind) {
	case FSIMG_ENTRY_FILE:
		reader->entry.size = inode->size;
		reader->initialized = inode->size;
		break;
	case FSIMG_ENTRY_SYMLINK:
		return (symlink_target_declare(reader, inode, error));
	case FSIMG_ENTRY_CHAR_DEVICE:
	case FSIMG_ENTRY_BLOCK_DEVICE:
		erofs_device_decode(inode->layout_word, &major, &minor);
		metadata->valid |= FSIMG_META_DEVICE;
		metadata->dev_major = major;
		metadata->dev_minor = minor;
		break;
	case FSIMG_ENTRY_DIRECTORY:
	case FSIMG_ENTRY_FIFO:
	case FSIMG_ENTRY_SOCKET:
		break;
	}
	return (FSIMG_OK);
}

/*
 * An extended inode stores its own nanoseconds; a compact inode stores
 * whole seconds after the epoch and shares the epoch's nanoseconds, so its
 * time is precise to the second.
 */
static fsimg_timestamp_t
time_of(const erofs_inode_t *inode)
{
	return ((fsimg_timestamp_t){
		.seconds = inode->time,
		.nanoseconds = inode->time_nsec,
		.precision_ns = inode->extended ? 1 : 1000000000,
		.basis = FSIMG_TIME_UTC
	});
}

/*
 * A symbolic link stores its target as content. The inode decoder bounded
 * its size to 1 to 1024 bytes, refusing an empty target as corrupt and a
 * longer one as unsupported; the target is read through the mapper and a
 * NUL byte in it is corrupt. Delivery then serves the checked bytes, so
 * what a caller reads is exactly what was checked.
 */
static fsimg_status_t
symlink_target_declare(fsimg_reader_t *reader, const erofs_inode_t *inode,
    fsimg_error_t *error)
{
	unsigned char target[EROFS_SYMLINK_MAX];
	size_t length;
	fsimg_status_t status;

	/* The decoder's bounds, repeated as the buffer's own guard. */
	if (inode->size == 0)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "symbolic link size"));
	if (inode->size > sizeof(target))
		return (erofs_reject(error, FSIMG_EUNSUPPORTED,
		    "symbolic link size"));
	length = (size_t)inode->size;
	status = erofs_content_read(reader, inode, 0, length, target, NULL,
	    error);
	if (status != FSIMG_OK)
		return (status);
	if (memchr(target, '\0', length) != NULL)
		return (erofs_reject(error, FSIMG_ECORRUPT,
		    "symbolic link target holds a NUL byte"));
	return (fsimg_reader_set_link_target(reader, target, length, error));
}

/*
 * The checks the block decoder applies to a stored name, for a name that
 * came back from scratch storage or a caller: 1 to 255 bytes, no "/", and
 * neither dot entry. A C string holds no NUL.
 */
static bool
name_native(const unsigned char *name, size_t length)
{
	if (length == 0 || length > EROFS_NAME_MAX)
		return (false);
	if (memchr(name, '/', length) != NULL)
		return (false);
	if ((length == 1 && name[0] == '.') ||
	    (length == 2 && name[0] == '.' && name[1] == '.'))
		return (false);
	return (true);
}

/*
 * The namespace key is the raw name, compared bytewise without
 * normalization or case folding, as the kernel looks names up; an omitted
 * name is keyed by its raw bytes like any other.
 */
fsimg_status_t
erofs_keys(fsimg_reader_t *reader FSIMG_UNUSED, const char *name,
    const unsigned char *payload FSIMG_UNUSED, size_t length FSIMG_UNUSED,
    fsimg_directory_keys_t *keys, fsimg_error_t *error)
{
	size_t bytes = strlen(name);

	keys->count = 0;
	if (!name_native((const unsigned char *)name, bytes))
		return (erofs_reject(error, FSIMG_ECORRUPT, "file name"));
	bcopy(name, keys->key[0], bytes);
	keys->length[0] = bytes;
	keys->count = 1;
	return (FSIMG_OK);
}

/*
 * A query spells a delivered name, so a component outside the portable
 * profile matches nothing and is refused before any directory is read.
 */
fsimg_status_t
erofs_key(fsimg_reader_t *reader FSIMG_UNUSED, const char *component,
    unsigned char *key, size_t *length, fsimg_error_t *error)
{
	size_t bytes = strlen(component);

	*length = 0;
	if (!name_native((const unsigned char *)component, bytes) ||
	    !fsimg_name_in_portable_profile(component, bytes))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_READER,
		    "selection component is not representable in EROFS"));
	bcopy(component, key, bytes);
	*length = bytes;
	return (FSIMG_OK);
}
