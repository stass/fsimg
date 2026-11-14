/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "apfs.h"
#include "fsimg-endian.h"

/*
 * APFS's part of directory preparation (specs/design-apfs-driver.md,
 * "Directories"; specs/design-apfs.md, "Directory records and names").
 * A directory's records are the directory-record keys under its inode,
 * adjacent in the filesystem tree, so prepare scans them with the scan
 * cursor and admits each child with its record's facts alone; it reads no
 * inode. replay looks the child's inode up and learns the rest of the
 * entry, so a damaged inode costs its own entry and not the listing. A
 * directory inode must name the directory holding its record as its
 * parent and carry that record's name, so the namespace is a tree with no
 * visited set.
 */
#define ROLE_RECORD	"directory record"

/* The prepared payload, little endian. */
#define PAYLOAD_INODE	0
#define PAYLOAD_TYPE	8
#define PAYLOAD_SIBLING	9
#define PAYLOAD_PARENT	17
#define PAYLOAD_BYTES	25u

_Static_assert(PAYLOAD_BYTES <= FSIMG_DIRECTORY_PAYLOAD_BYTES,
    "the APFS payload fits the common bound");
_Static_assert(APFS_NAME_KEY_BYTES <= FSIMG_DIRECTORY_KEY_BYTES,
    "an APFS namespace key fits a directory key buffer");
_Static_assert(APFS_NAME_MAX_BYTES < FSIMG_DIRECTORY_NAME_BYTES,
    "an APFS name fits a directory record");

static bool		inode_ordinary(const apfs_reader_t *, uint64_t);
static fsimg_status_t	payload_decode(const apfs_reader_t *,
			    const unsigned char *, size_t, apfs_child_t *,
			    fsimg_error_t *);
static void		payload_encode(const apfs_child_t *, unsigned char *);
static fsimg_status_t	record_child(const apfs_reader_t *, uint64_t,
			    const apfs_record_t *, apfs_child_t *,
			    fsimg_error_t *);
static fsimg_status_t	record_hash(apfs_reader_t *, const apfs_drec_key_t *,
			    fsimg_error_t *);
static fsimg_status_t	record_name(apfs_reader_t *, const apfs_drec_key_t *,
			    fsimg_error_t *);
static fsimg_status_t	prepare_record(fsimg_reader_t *,
			    fsimg_directory_build_t *, uint64_t,
			    const apfs_record_t *, fsimg_error_t *);
static fsimg_status_t	prepare_scan(fsimg_reader_t *,
			    fsimg_directory_build_t *, uint64_t,
			    fsimg_error_t *);

/* An inode a directory record may name: neither reserved nor past the
 * key's 60-bit OID.
 */
static bool
inode_ordinary(const apfs_reader_t *apfs, uint64_t inode)
{
	return (inode >= apfs->inode_min && inode <= APFS_JKEY_OID_MASK);
}

static void
payload_encode(const apfs_child_t *child, unsigned char *payload)
{
	fsimg_store_le64(payload + PAYLOAD_INODE, child->inode);
	payload[PAYLOAD_TYPE] = child->type;
	fsimg_store_le64(payload + PAYLOAD_SIBLING, child->sibling_id);
	fsimg_store_le64(payload + PAYLOAD_PARENT, child->parent);
}

/* The payload came back from scratch storage, so what prepare cannot have
 * written is refused before anything uses it.
 */
static fsimg_status_t
payload_decode(const apfs_reader_t *apfs, const unsigned char *payload,
    size_t length, apfs_child_t *child, fsimg_error_t *error)
{
	if (length != PAYLOAD_BYTES)
		return (apfs_corrupt(error, ROLE_RECORD,
		    "prepared payload is invalid"));
	*child = (apfs_child_t){
		.inode = fsimg_load_le64(payload + PAYLOAD_INODE),
		.type = payload[PAYLOAD_TYPE],
		.sibling_id = fsimg_load_le64(payload + PAYLOAD_SIBLING),
		.parent = fsimg_load_le64(payload + PAYLOAD_PARENT)
	};
	/* The parent is the root or an ordinary directory. */
	if (!inode_ordinary(apfs, child->inode) ||
	    !apfs_type_kind(child->type, NULL) ||
	    (child->parent != APFS_INO_ROOT &&
	    !inode_ordinary(apfs, child->parent)))
		return (apfs_corrupt(error, ROLE_RECORD,
		    "prepared payload is invalid"));
	return (FSIMG_OK);
}

/*
 * A record's name: at most APFS_NAME_MAX_BYTES, which bounds the copy
 * prepare makes, then the name rules and the comparison form under the
 * volume's rule, left in apfs->form for record_hash().
 */
static fsimg_status_t
record_name(apfs_reader_t *apfs, const apfs_drec_key_t *key,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	if (key->name_len > APFS_NAME_MAX_BYTES)
		return (apfs_unsupported(error, ROLE_RECORD,
		    "name is longer than 1022 bytes"));
	status = apfs_name_form(apfs->rule, key->name, key->name_len,
	    &apfs->form);
	if (status == FSIMG_ELIMIT)
		return (fsimg_error_set(error, FSIMG_ELIMIT, 0,
		    FSIMG_OP_READER, "APFS name form exceeds its bound"));
	if (status != FSIMG_OK)
		return (apfs_corrupt(error, ROLE_RECORD, "name is invalid"));
	return (FSIMG_OK);
}

/*
 * In a hashed key, the stored hash against the form record_name() derived,
 * the length having been checked by the key's decoder. A mismatch on a
 * name holding a code point Unicode 17.0.0 does not assign, or a
 * noncharacter, may be a newer macOS hashing it with newer tables, so it is
 * unsupported rather than corrupt.
 */
static fsimg_status_t
record_hash(apfs_reader_t *apfs, const apfs_drec_key_t *key,
    fsimg_error_t *error)
{
	if (!key->hashed || apfs_name_hash(&apfs->form) == key->hash)
		return (FSIMG_OK);
	if (!apfs_name_assigned(key->name, key->name_len))
		return (apfs_unsupported(error, ROLE_RECORD,
		    "name hash differs for an unassigned code point"));
	return (apfs_corrupt(error, ROLE_RECORD,
	    "name hash does not match the name"));
}

/*
 * A record's value: a type of the format design's table, an ordinary child
 * inode, and the extended fields, of which the sibling ID is the one
 * directory-record type known; an unknown one is ignored unless marked
 * data-dependent.
 */
static fsimg_status_t
record_child(const apfs_reader_t *apfs, uint64_t directory,
    const apfs_record_t *record, apfs_child_t *child, fsimg_error_t *error)
{
	apfs_drec_val_t val;
	apfs_xfield_t field;

	if (record->ghost || !apfs_drec_val_decode(record->val,
	    record->val_len, &val))
		return (apfs_corrupt(error, ROLE_RECORD, "value is malformed"));
	*child = (apfs_child_t){ .inode = val.child, .parent = directory,
	    .type = (uint8_t)(val.flags & APFS_DREC_TYPE_MASK) };
	if (!apfs_type_kind(child->type, NULL))
		return (apfs_corrupt(error, ROLE_RECORD, "type is unknown"));
	if (!inode_ordinary(apfs, child->inode))
		return (apfs_corrupt(error, ROLE_RECORD,
		    "names a reserved inode"));
	for (uint16_t i = 0; i < val.xfields.count; i++) {
		if (!apfs_xfield_get(&val.xfields, i, &field))
			return (apfs_corrupt(error, ROLE_RECORD,
			    "extended fields are malformed"));
		if (field.type == APFS_XF_DREC_SIBLING_ID) {
			if (!apfs_xfield_u64(&field, &child->sibling_id))
				return (apfs_corrupt(error, ROLE_RECORD,
				    "sibling ID is malformed"));
		} else if ((field.flags & APFS_XF_DATA_DEPENDENT) != 0)
			return (apfs_unsupported(error, ROLE_RECORD,
			    "has an unknown data-dependent field"));
	}
	return (FSIMG_OK);
}

/*
 * One record of the directory: its key variant and name, then its hash and
 * value, then admission, which charges the entry. Once the name has passed
 * its rules every refusal names the child; a refused name stays unnamed.
 */
static fsimg_status_t
prepare_record(fsimg_reader_t *reader, fsimg_directory_build_t *build,
    uint64_t directory, const apfs_record_t *record, fsimg_error_t *error)
{
	apfs_reader_t *apfs = reader->driver;
	unsigned char payload[PAYLOAD_BYTES];
	char name[APFS_NAME_MAX_BYTES + 1];
	apfs_drec_key_t key;
	apfs_child_t child;
	fsimg_status_t status;

	if (!apfs_drec_key_decode(record->key, record->key_len, apfs->hashed,
	    &key))
		return (apfs_corrupt(error, ROLE_RECORD, "key is malformed"));
	status = record_name(apfs, &key, error);
	if (status != FSIMG_OK)
		return (status);
	bcopy(key.name, name, key.name_len);
	name[key.name_len] = '\0';

	status = record_hash(apfs, &key, error);
	if (status == FSIMG_OK)
		status = record_child(apfs, directory, record, &child, error);
	if (status == FSIMG_OK) {
		payload_encode(&child, payload);
		status = fsimg_directory_admit(build, name, payload,
		    sizeof(payload), error);
	}
	if (status != FSIMG_OK)
		fsimg_directory_name_child(build, name, error);
	return (status);
}

/* Every directory record of the directory, from the first at or above its
 * bare key while the owner and kind match; the cursor bounds the walk.
 */
static fsimg_status_t
prepare_scan(fsimg_reader_t *reader, fsimg_directory_build_t *build,
    uint64_t directory, fsimg_error_t *error)
{
	apfs_reader_t *apfs = reader->driver;
	unsigned char start[APFS_JKEY_BYTES];
	apfs_jkey_t jkey = { .oid = directory, .kind = APFS_J_DREC };
	apfs_record_t record;
	bool found;
	fsimg_status_t status;

	if (!apfs_jkey_encode(start, sizeof(start), &jkey))
		return (apfs_corrupt(error, ROLE_RECORD,
		    "directory number is invalid"));
	status = apfs_cursor_seek(&apfs->scan, start, sizeof(start),
	    APFS_SEEK_GE, &found, error);
	while (status == FSIMG_OK && found) {
		apfs_cursor_record(&apfs->scan, &record);
		if (!apfs_jkey_decode(record.key, record.key_len, &jkey))
			return (apfs_corrupt(error, ROLE_RECORD,
			    "key is malformed"));
		if (jkey.oid != directory || jkey.kind != APFS_J_DREC)
			break;
		status = prepare_record(reader, build, directory, &record,
		    error);
		if (status == FSIMG_OK)
			status = apfs_cursor_next(&apfs->scan, &found, error);
	}
	return (status);
}

/*
 * The root at level 0, whose inode is checked here as a directory under
 * inode 1; otherwise the directory replay made current, which replay has
 * already proved.
 */
fsimg_status_t
apfs_prepare(fsimg_reader_t *reader, fsimg_directory_build_t *build,
    fsimg_error_t *error)
{
	apfs_reader_t *apfs = reader->driver;
	apfs_child_t root = { .inode = APFS_INO_ROOT,
	    .parent = APFS_INO_ROOT_PARENT, .type = APFS_DT_DIRECTORY };
	apfs_inode_t inode;
	fsimg_status_t status;

	if (fsimg_directory_level(build) == 0) {
		status = apfs_inode_get(apfs, &root, NULL, &inode, error);
		if (status != FSIMG_OK)
			return (status);
		return (prepare_scan(reader, build, APFS_INO_ROOT, error));
	}
	if (apfs->current.kind != FSIMG_ENTRY_DIRECTORY)
		return (apfs_corrupt(error, ROLE_RECORD,
		    "prepared directory is not a directory"));
	return (prepare_scan(reader, build, apfs->current.inode, error));
}

/* Make a prepared child the current entry. The name came back from scratch
 * storage with the payload, so it is judged again before the inode is.
 */
fsimg_status_t
apfs_replay(fsimg_reader_t *reader, const char *name,
    const unsigned char *payload, size_t length, fsimg_error_t *error)
{
	apfs_reader_t *apfs = reader->driver;
	apfs_child_t child;
	fsimg_status_t status;

	apfs->current = (apfs_current_t){ 0 };
	status = payload_decode(apfs, payload, length, &child, error);
	if (status != FSIMG_OK)
		return (status);
	if (apfs_name_form(apfs->rule, name, strlen(name), &apfs->form) !=
	    FSIMG_OK)
		return (apfs_corrupt(error, ROLE_RECORD,
		    "prepared name is invalid"));
	return (apfs_entry_load(reader, &child, name, error));
}

/* One key per entry: the comparison form as UTF-8. */
fsimg_status_t
apfs_keys(fsimg_reader_t *reader, const char *name,
    const unsigned char *payload FSIMG_UNUSED, size_t length FSIMG_UNUSED,
    fsimg_directory_keys_t *keys, fsimg_error_t *error)
{
	apfs_reader_t *apfs = reader->driver;
	fsimg_status_t status;

	keys->count = 0;
	status = apfs_name_form(apfs->rule, name, strlen(name), &apfs->form);
	if (status == FSIMG_OK)
		status = apfs_name_key(&apfs->form, (char *)keys->key[0],
		    sizeof(keys->key[0]), &keys->length[0]);
	if (status != FSIMG_OK)
		return (apfs_corrupt(error, ROLE_RECORD,
		    "prepared name is invalid"));
	keys->count = 1;
	return (FSIMG_OK);
}

/* A selection query component, converted as a stored name is; any
 * component no APFS name can be is FSIMG_EINVAL.
 */
fsimg_status_t
apfs_key(fsimg_reader_t *reader, const char *component, unsigned char *key,
    size_t *length, fsimg_error_t *error)
{
	apfs_reader_t *apfs = reader->driver;
	fsimg_status_t status;

	status = apfs_name_form(apfs->rule, component, strlen(component),
	    &apfs->form);
	if (status == FSIMG_OK)
		status = apfs_name_key(&apfs->form, (char *)key,
		    FSIMG_DIRECTORY_KEY_BYTES, length);
	if (status != FSIMG_OK)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_READER, "name cannot be an APFS name"));
	return (FSIMG_OK);
}
