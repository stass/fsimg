/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "apfs.h"
#include "codec.h"
#include "decmpfs.h"
#include "unicode.h"

/*
 * Inodes and the entries they make (specs/design-apfs-driver.md,
 * "Directories"; specs/design-apfs.md, "Inodes", "Extended fields",
 * "Extended attributes", "Symbolic links", "Transparent compression").
 *
 * An entry is learnt in one pass over the lookup cursor: the inode record,
 * then the attribute records filed after it under the same owner. The
 * inode is judged against the directory record that named it, so a
 * damaged inode costs its own entry. The attribute pass consumes the
 * attributes the driver serves itself, each only on the inode kind it
 * belongs to, and marks any other as extra data the entry model cannot
 * carry.
 */
#define ROLE_INODE	"inode"
#define ROLE_ATTRIBUTE	"attribute"
#define ROLE_LINK	"symbolic link"
#define ROLE_COMPRESSED	"compressed file"

#define NS_PER_SECOND	INT64_C(1000000000)

/* The directory-record types, indexed by type, with their entry kinds; a
 * zero kind is no type.
 */
static const fsimg_entry_kind_t type_kinds[] = {
	[APFS_DT_FIFO] = FSIMG_ENTRY_FIFO,
	[APFS_DT_CHAR] = FSIMG_ENTRY_CHAR_DEVICE,
	[APFS_DT_DIRECTORY] = FSIMG_ENTRY_DIRECTORY,
	[APFS_DT_BLOCK] = FSIMG_ENTRY_BLOCK_DEVICE,
	[APFS_DT_FILE] = FSIMG_ENTRY_FILE,
	[APFS_DT_SYMLINK] = FSIMG_ENTRY_SYMLINK,
	[APFS_DT_SOCKET] = FSIMG_ENTRY_SOCKET
};

/* The data size of each inode field type the format design knows,
 * FIELD_ANY for one of any size; zero, and every type past the table, is
 * unknown.
 */
#define FIELD_ANY	UINT16_MAX

static const uint16_t field_sizes[] = {
	[APFS_XF_INO_SNAP_XID] = 8,
	[APFS_XF_INO_DELTA_TREE_OID] = 8,
	[APFS_XF_INO_DOCUMENT_ID] = 4,
	[APFS_XF_INO_NAME] = FIELD_ANY,		/* judged as a name */
	[APFS_XF_INO_PREV_FSIZE] = 8,
	[6] = FIELD_ANY,			/* reserved */
	[APFS_XF_INO_FINDER_INFO] = 32,
	[APFS_XF_INO_DSTREAM] = APFS_DSTREAM_BYTES,
	[9] = FIELD_ANY,			/* reserved */
	[APFS_XF_INO_DIR_STATS] = APFS_DIR_STATS_BYTES,
	[APFS_XF_INO_FS_UUID] = APFS_UUID_BYTES,
	[12] = FIELD_ANY,			/* reserved */
	[APFS_XF_INO_SPARSE_BYTES] = 8,
	[APFS_XF_INO_RDEV] = 4,
	[APFS_XF_INO_PURGEABLE_FLAGS] = FIELD_ANY,
	[APFS_XF_INO_ORIG_SYNC_ROOT] = 8
};

/* Where an attribute the driver consumes was found, if at all. */
typedef enum attr_form {
	ATTR_ABSENT,
	ATTR_INLINE,	/* its value in its record */
	ATTR_STREAM	/* its value in a data stream */
} attr_form_t;

/* What the attribute pass learnt of one inode: the form and value length
 * of the compression header and the resource fork. A header held in its
 * record is decoded into header; one held in a stream is not read here.
 */
typedef struct attr_scan {
	fsimg_decmpfs_header_t header;
	uint64_t	header_length;
	uint64_t	fork_size;
	attr_form_t	header_form;
	attr_form_t	fork_form;
	bool		link_seen;
	bool		extra_data;
} attr_scan_t;

static fsimg_status_t	attr_classify(fsimg_reader_t *,
			    const apfs_inode_t *, const apfs_record_t *,
			    attr_scan_t *, fsimg_error_t *);
static fsimg_status_t	attr_scan(fsimg_reader_t *, uint64_t,
			    const apfs_inode_t *, attr_scan_t *,
			    fsimg_error_t *);
static bool		attr_named(const apfs_name_key_t *, const char *);
static fsimg_status_t	compressed_size(const apfs_inode_t *,
			    const attr_scan_t *, uint64_t *, bool *,
			    fsimg_error_t *);
static fsimg_status_t	compressed_size_streamed(const apfs_inode_t *,
			    const attr_scan_t *, uint64_t *, bool *,
			    fsimg_error_t *);
static void		entry_fill(fsimg_reader_t *, const apfs_inode_t *);
static fsimg_status_t	fields_read(apfs_inode_t *, const char **,
			    uint16_t *, fsimg_error_t *);
static bool		is_compressed(const apfs_inode_t *);
static bool		name_field_valid(const char *, uint16_t);
static fsimg_status_t	link_target(fsimg_reader_t *,
			    const apfs_xattr_val_t *, attr_scan_t *,
			    fsimg_error_t *);
static void		timestamp_set(fsimg_timestamp_t *, int64_t);

bool
apfs_type_kind(uint8_t type, fsimg_entry_kind_t *kind)
{
	if (type >= sizeof(type_kinds) / sizeof(type_kinds[0]) ||
	    type_kinds[type] == 0)
		return (false);
	if (kind != NULL)
		*kind = type_kinds[type];
	return (true);
}

/* The name field's text: strict UTF-8 holding no NUL, nonempty. */
static bool
name_field_valid(const char *name, uint16_t length)
{
	const unsigned char *bytes = (const unsigned char *)name;
	size_t at = 0, taken;
	uint32_t point;

	if (length == 0)
		return (false);
	while (at < length) {
		if (!fsimg_utf8_decode(bytes + at, length - at, &taken, &point) ||
		    point == 0)
			return (false);
		at += taken;
	}
	return (true);
}

/*
 * Walk an inode's extended fields: every known type at its size, the
 * three the listing uses decoded into inode and name, an unknown type
 * ignored unless its flags mark it data-dependent. The blob's shape, a
 * repeated type included, is the codec's to have refused.
 */
static fsimg_status_t
fields_read(apfs_inode_t *inode, const char **name, uint16_t *name_len,
    fsimg_error_t *error)
{
	const apfs_xfields_t *fields = &inode->val.xfields;
	apfs_dstream_t dstream;
	apfs_xfield_t field;
	uint16_t size;

	*name = NULL;
	*name_len = 0;
	for (uint16_t i = 0; i < fields->count; i++) {
		if (!apfs_xfield_get(fields, i, &field))
			return (apfs_corrupt(error, ROLE_INODE,
			    "extended fields are malformed"));
		size = field.type < sizeof(field_sizes) /
		    sizeof(field_sizes[0]) ? field_sizes[field.type] : 0;
		if (size == 0) {
			if ((field.flags & APFS_XF_DATA_DEPENDENT) != 0)
				return (apfs_unsupported(error, ROLE_INODE,
				    "has an unknown data-dependent field"));
			continue;
		}
		if (size != FIELD_ANY && field.size != size)
			return (apfs_corrupt(error, ROLE_INODE,
			    "extended field has the wrong size"));
		switch (field.type) {
		case APFS_XF_INO_NAME:
			if (!apfs_xfield_name(&field, name, name_len) ||
			    !name_field_valid(*name, *name_len))
				return (apfs_corrupt(error, ROLE_INODE,
				    "name field is malformed"));
			break;
		case APFS_XF_INO_DSTREAM:
			if (!apfs_dstream_decode(field.data, field.size,
			    &dstream) || dstream.size > INT64_MAX)
				return (apfs_corrupt(error, ROLE_INODE,
				    "data stream is malformed"));
			inode->has_dstream = true;
			inode->size = dstream.size;
			inode->alloced = dstream.alloced_size;
			break;
		case APFS_XF_INO_RDEV:
			if (inode->kind != FSIMG_ENTRY_CHAR_DEVICE &&
			    inode->kind != FSIMG_ENTRY_BLOCK_DEVICE)
				return (apfs_corrupt(error, ROLE_INODE,
				    "device number is on a non-device"));
			(void)apfs_xfield_u32(&field, &inode->rdev);
			inode->has_rdev = true;
			break;
		default:
			break;
		}
	}
	return (FSIMG_OK);
}

fsimg_status_t
apfs_inode_decode(const apfs_record_t *record, apfs_inode_t *inode,
    const char **name, uint16_t *name_len, fsimg_error_t *error)
{
	const apfs_inode_val_t *val = &inode->val;

	bzero(inode, sizeof(*inode));
	if (record->ghost || !apfs_inode_val_decode(record->val,
	    record->val_len, &inode->val))
		return (apfs_corrupt(error, ROLE_INODE, "is malformed"));
	if (!apfs_type_kind((val->mode & APFS_MODE_TYPE_MASK) >> 12,
	    &inode->kind))
		return (apfs_corrupt(error, ROLE_INODE, "has an invalid type"));
	if ((val->internal_flags & ~APFS_INODE_FLAGS_VALID) != 0)
		return (apfs_unsupported(error, ROLE_INODE,
		    "has unsupported internal flags"));
	/* The dataless flag is judged on every kind: a directory whose
	 * records are not on the volume would otherwise list as empty.
	 */
	if ((val->bsd_flags & APFS_BSD_DATALESS) != 0)
		return (apfs_unsupported(error, ROLE_INODE,
		    "content is not on the volume"));
	return (fields_read(inode, name, name_len, error));
}

fsimg_status_t
apfs_inode_get(apfs_reader_t *apfs, const apfs_child_t *child,
    const char *name, apfs_inode_t *inode, fsimg_error_t *error)
{
	unsigned char key[APFS_JKEY_BYTES];
	const apfs_inode_val_t *val = &inode->val;
	apfs_record_t record;
	const char *stored;
	uint64_t oid;
	uint16_t stored_len;
	bool found;
	fsimg_status_t status;

	bzero(inode, sizeof(*inode));
	if (!apfs_bare_key_encode(key, sizeof(key), APFS_J_INODE, child->inode))
		return (apfs_corrupt(error, ROLE_INODE, "number is invalid"));
	status = apfs_cursor_seek(&apfs->lookup, key, sizeof(key),
	    APFS_SEEK_GE, &found, error);
	if (status != FSIMG_OK)
		return (status);
	if (found)
		apfs_cursor_record(&apfs->lookup, &record);
	if (!found || !apfs_bare_key_decode(record.key, record.key_len,
	    APFS_J_INODE, &oid) || oid != child->inode)
		return (apfs_corrupt(error, ROLE_INODE, "is missing"));
	status = apfs_inode_decode(&record, inode, &stored, &stored_len, error);
	if (status != FSIMG_OK)
		return (status);
	if ((val->mode & APFS_MODE_TYPE_MASK) >> 12 != child->type)
		return (apfs_corrupt(error, ROLE_INODE,
		    "type does not match its directory record"));

	/* The parent and sharing rules: a directory has one parent and one
	 * name, so no cycle or second name survives replay.
	 */
	if (inode->kind == FSIMG_ENTRY_DIRECTORY) {
		if (val->parent != child->parent)
			return (apfs_corrupt(error, ROLE_INODE,
			    "directory names another parent"));
		if (name != NULL && (stored == NULL ||
		    strlen(name) != stored_len ||
		    bcmp(name, stored, stored_len) != 0))
			return (apfs_corrupt(error, ROLE_INODE,
			    "directory name field differs from its record"));
	}
	/* fields_read took what the entry needs; the span would go stale
	 * when the cursor moves, so it is not left to be read.
	 */
	inode->val.xfields = (apfs_xfields_t){ 0 };
	return (FSIMG_OK);
}

static bool
attr_named(const apfs_name_key_t *key, const char *name)
{
	size_t length = strlen(name);

	return (key->name_len == length && bcmp(key->name, name, length) == 0);
}

/*
 * The target rule: inline, owned by the filesystem, one final NUL that is
 * not part of the target, 1 to APFS_LINK_TARGET_MAX bytes. The bytes
 * checked are declared while the record holding them is current.
 */
static fsimg_status_t
link_target(fsimg_reader_t *reader, const apfs_xattr_val_t *val,
    attr_scan_t *scan, fsimg_error_t *error)
{
	size_t length;

	if ((val->flags & APFS_XATTR_DATA_EMBEDDED) == 0 ||
	    (val->flags & APFS_XATTR_FS_OWNED) == 0)
		return (apfs_corrupt(error, ROLE_LINK,
		    "target is not an inline filesystem attribute"));
	if (val->data_len < 2 || val->data[val->data_len - 1] != '\0')
		return (apfs_corrupt(error, ROLE_LINK,
		    "target is empty or unterminated"));
	length = (size_t)val->data_len - 1;
	if (length > APFS_LINK_TARGET_MAX)
		return (apfs_unsupported(error, ROLE_LINK,
		    "target is longer than 1023 bytes"));
	if (memchr(val->data, '\0', length) != NULL)
		return (apfs_corrupt(error, ROLE_LINK,
		    "target holds a NUL"));
	scan->link_seen = true;
	return (fsimg_reader_set_link_target(reader, val->data, length,
	    error));
}

static bool
is_compressed(const apfs_inode_t *inode)
{
	return (inode->kind == FSIMG_ENTRY_FILE &&
	    (inode->val.bsd_flags & APFS_BSD_COMPRESSED) != 0);
}

/*
 * Classify one attribute record of the inode, after checking its key and
 * value header and its flags: the link's target, the compression header,
 * the resource fork, or extra data the entry model cannot carry. A
 * compression value's length is checked in either form against the common
 * bounds before anything uses it.
 */
static fsimg_status_t
attr_classify(fsimg_reader_t *reader, const apfs_inode_t *inode,
    const apfs_record_t *record, attr_scan_t *scan, fsimg_error_t *error)
{
	apfs_name_key_t key;
	apfs_xattr_val_t val;
	uint64_t length;
	bool embedded;

	if (record->ghost || !apfs_xattr_key_decode(record->key,
	    record->key_len, &key) || !apfs_xattr_val_decode(record->val,
	    record->val_len, &val))
		return (apfs_corrupt(error, ROLE_ATTRIBUTE, "is malformed"));
	if ((val.flags & ~APFS_XATTR_FLAGS_KNOWN) != 0)
		return (apfs_unsupported(error, ROLE_ATTRIBUTE,
		    "has unsupported flags"));
	embedded = (val.flags & APFS_XATTR_DATA_EMBEDDED) != 0;
	if (embedded && val.data_len > APFS_XATTR_INLINE_MAX)
		return (apfs_corrupt(error, ROLE_ATTRIBUTE,
		    "inline data is too long"));
	length = embedded ? val.data_len : val.dstream.size;

	if (inode->kind == FSIMG_ENTRY_SYMLINK &&
	    attr_named(&key, APFS_XATTR_SYMLINK))
		return (link_target(reader, &val, scan, error));
	if (is_compressed(inode) && attr_named(&key, APFS_XATTR_DECMPFS)) {
		if (length < FSIMG_DECMPFS_HEADER_BYTES ||
		    length > FSIMG_DECMPFS_HEADER_BYTES + FSIMG_LZFSE_INPUT_MAX)
			return (apfs_corrupt(error, ROLE_COMPRESSED,
			    "header has an invalid length"));
		if (embedded && !fsimg_decmpfs_header_decode(val.data,
		    val.data_len, &scan->header))
			return (apfs_corrupt(error, ROLE_COMPRESSED,
			    "header is malformed"));
		scan->header_form = embedded ? ATTR_INLINE : ATTR_STREAM;
		scan->header_length = length;
		return (FSIMG_OK);
	}
	if (is_compressed(inode) &&
	    attr_named(&key, APFS_XATTR_RESOURCE_FORK)) {
		scan->fork_form = embedded ? ATTR_INLINE : ATTR_STREAM;
		scan->fork_size = length;
		return (FSIMG_OK);
	}
	scan->extra_data = true;
	return (FSIMG_OK);
}

/* The attribute records follow the inode record under the same owner. */
static fsimg_status_t
attr_scan(fsimg_reader_t *reader, uint64_t number, const apfs_inode_t *inode,
    attr_scan_t *scan, fsimg_error_t *error)
{
	apfs_reader_t *apfs = reader->driver;
	apfs_record_t record;
	apfs_jkey_t jkey;
	bool found;
	fsimg_status_t status;

	bzero(scan, sizeof(*scan));
	for (;;) {
		status = apfs_cursor_next(&apfs->lookup, &found, error);
		if (status != FSIMG_OK || !found)
			return (status);
		apfs_cursor_record(&apfs->lookup, &record);
		if (!apfs_jkey_decode(record.key, record.key_len, &jkey))
			return (apfs_corrupt(error, ROLE_ATTRIBUTE,
			    "key is malformed"));
		if (jkey.oid != number || jkey.kind != APFS_J_XATTR)
			return (FSIMG_OK);
		status = attr_classify(reader, inode, &record, scan, error);
		if (status != FSIMG_OK)
			return (status);
	}
}

/*
 * A compressed file's size, and whether its resource fork is explained by
 * the compression and so does not set the extra-data mark. The header's
 * size governs and must equal the inode's uncompressed size when the inode
 * carries one, and the data stream must be empty. A resource-fork type's
 * header is exactly the 16 bytes and its content is in a fork held in a
 * stream; an inline type's content is at most one 64 KiB chunk, and beside
 * it only an empty fork is explained.
 */
static fsimg_status_t
compressed_size(const apfs_inode_t *inode, const attr_scan_t *scan,
    uint64_t *size, bool *fork_explained, fsimg_error_t *error)
{
	const apfs_inode_val_t *val = &inode->val;
	fsimg_decmpfs_info_t info;

	if (scan->header_form == ATTR_ABSENT)
		return (apfs_corrupt(error, ROLE_COMPRESSED,
		    "has no compression header"));
	if (inode->size != 0)
		return (apfs_corrupt(error, ROLE_COMPRESSED,
		    "has a data stream"));
	if (scan->header_form == ATTR_STREAM)
		return (compressed_size_streamed(inode, scan, size,
		    fork_explained, error));
	if (!fsimg_decmpfs_type_info(scan->header.type, &info))
		return (apfs_unsupported(error, ROLE_COMPRESSED,
		    "compression type is unknown"));
	if ((val->internal_flags & APFS_INODE_HAS_UNCOMPRESSED_SIZE) != 0 &&
	    val->uncompressed_size != scan->header.uncompressed_size)
		return (apfs_corrupt(error, ROLE_COMPRESSED,
		    "header size differs from the inode's"));
	*size = scan->header.uncompressed_size;
	if (info.kind == FSIMG_DECMPFS_INLINE &&
	    *size > FSIMG_DECMPFS_CHUNK_BYTES)
		return (apfs_unsupported(error, ROLE_COMPRESSED,
		    "inline type is larger than 64 KiB"));
	if (info.kind == FSIMG_DECMPFS_INLINE) {
		*fork_explained = scan->fork_form != ATTR_ABSENT &&
		    scan->fork_size == 0;
		return (FSIMG_OK);
	}
	if (scan->header_length != FSIMG_DECMPFS_HEADER_BYTES)
		return (apfs_corrupt(error, ROLE_COMPRESSED,
		    "resource-fork header is not 16 bytes"));
	if (scan->fork_form == ATTR_ABSENT)
		return (apfs_corrupt(error, ROLE_COMPRESSED,
		    "has no resource fork"));
	if (scan->fork_form == ATTR_INLINE)
		return (apfs_unsupported(error, ROLE_COMPRESSED,
		    "resource fork is held in its record"));
	*fork_explained = true;
	return (FSIMG_OK);
}

/*
 * A header held in a stream is read with the content
 * (specs/design-apfs-driver.md, "Directories"), which the reader does not
 * read yet. Until it does, such a file is listed with the inode's
 * uncompressed size, as native volumes store it beside an inline type,
 * and one whose type cannot be known without the header, the inode
 * carrying no size or the fork holding bytes, is refused.
 */
static fsimg_status_t
compressed_size_streamed(const apfs_inode_t *inode, const attr_scan_t *scan,
    uint64_t *size, bool *fork_explained, fsimg_error_t *error)
{
	const apfs_inode_val_t *val = &inode->val;

	if ((val->internal_flags & APFS_INODE_HAS_UNCOMPRESSED_SIZE) == 0 ||
	    (scan->fork_form != ATTR_ABSENT && scan->fork_size != 0))
		return (apfs_unsupported(error, ROLE_COMPRESSED,
		    "header held in a stream is not read"));
	if (val->uncompressed_size > INT64_MAX)
		return (apfs_corrupt(error, ROLE_COMPRESSED,
		    "size is invalid"));
	*size = val->uncompressed_size;
	*fork_explained = scan->fork_form != ATTR_ABSENT;
	return (FSIMG_OK);
}

/*
 * A time counts signed nanoseconds since 1970 in UTC: seconds are the
 * floor of the quotient, nanoseconds the remainder.
 */
static void
timestamp_set(fsimg_timestamp_t *timestamp, int64_t stored)
{
	int64_t seconds = stored / NS_PER_SECOND;
	int64_t nanoseconds = stored % NS_PER_SECOND;

	if (nanoseconds < 0) {
		nanoseconds += NS_PER_SECOND;
		seconds--;
	}
	timestamp->seconds = seconds;
	timestamp->nanoseconds = (uint32_t)nanoseconds;
	timestamp->precision_ns = 1;
	timestamp->basis = FSIMG_TIME_UTC;
	timestamp->offset_minutes = 0;
}

/* The entry's kind and metadata, which the inode alone gives. */
static void
entry_fill(fsimg_reader_t *reader, const apfs_inode_t *inode)
{
	const apfs_inode_val_t *val = &inode->val;
	fsimg_metadata_t *metadata = &reader->entry.metadata;

	reader->entry.kind = inode->kind;
	reader->entry.size = 0;
	bzero(metadata, sizeof(*metadata));
	metadata->valid = FSIMG_META_OWNER | FSIMG_META_MODE |
	    FSIMG_META_CREATED | FSIMG_META_MODIFIED | FSIMG_META_CHANGED |
	    FSIMG_META_ACCESSED;
	metadata->uid = val->owner;
	metadata->gid = val->group;
	metadata->mode = val->mode & FSIMG_MODE_BITS;
	timestamp_set(&metadata->created, val->create_time);
	timestamp_set(&metadata->modified, val->mod_time);
	timestamp_set(&metadata->changed, val->change_time);
	timestamp_set(&metadata->accessed, val->access_time);
	if (inode->has_rdev) {
		metadata->valid |= FSIMG_META_DEVICE;
		metadata->dev_major = inode->rdev >> 24;
		metadata->dev_minor = inode->rdev & UINT32_C(0xffffff);
	}
}

fsimg_status_t
apfs_entry_load(fsimg_reader_t *reader, const apfs_child_t *child,
    const char *name, fsimg_error_t *error)
{
	apfs_reader_t *apfs = reader->driver;
	apfs_inode_t inode;
	attr_scan_t scan;
	uint64_t size;
	bool fork_explained = false;
	fsimg_status_t status;

	status = apfs_inode_get(apfs, child, name, &inode, error);
	if (status != FSIMG_OK)
		return (status);
	entry_fill(reader, &inode);
	/* The scan moves the cursor; only inode's owned members remain. */
	status = attr_scan(reader, child->inode, &inode, &scan, error);
	if (status != FSIMG_OK)
		return (status);

	size = inode.size;
	if (is_compressed(&inode)) {
		status = compressed_size(&inode, &scan, &size, &fork_explained,
		    error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (inode.kind == FSIMG_ENTRY_SYMLINK && !scan.link_seen)
		return (apfs_corrupt(error, ROLE_LINK, "has no target"));
	if (scan.extra_data || (scan.fork_form != ATTR_ABSENT &&
	    !fork_explained))
		reader->entry.metadata.valid |= FSIMG_META_EXTRA_DATA;
	if (inode.kind == FSIMG_ENTRY_FILE) {
		reader->entry.size = size;
		reader->initialized = size;
	}
	apfs->current = (apfs_current_t){ .inode = child->inode,
	    .private_id = inode.val.private_id, .kind = inode.kind,
	    .compressed = is_compressed(&inode) };
	return (FSIMG_OK);
}
