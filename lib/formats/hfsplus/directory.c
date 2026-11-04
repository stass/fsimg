/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "hfsplus.h"

/* HFS Plus's part of directory preparation (specs/design-hfsplus-driver.md,
 * Directories and names). A folder's records are adjacent in the catalog: its
 * thread at (id, ""), then its children in key order. Entering a folder
 * requires its thread to name the parent and name the walker reached it by,
 * so a folder can be entered from one parent only and the namespace is a tree
 * with no visited set. Key order is what ends a scan along a corrupt sibling
 * chain.
 */

/* The prepared payload, little endian. */
#define PAYLOAD_ID		0
#define PAYLOAD_LEAF		4
#define PAYLOAD_RECORD		8
#define PAYLOAD_KIND		10
#define PAYLOAD_EXTRA		11
#define PAYLOAD_COMPRESSION	12
#define PAYLOAD_SIZE		13
#define PAYLOAD_OWNER		21
#define PAYLOAD_GROUP		25
#define PAYLOAD_MODE		29
#define PAYLOAD_FLAGS		31
#define PAYLOAD_CREATED		32
#define PAYLOAD_MODIFIED	36
#define PAYLOAD_CHANGED		40
#define PAYLOAD_ACCESSED	44
#define PAYLOAD_SPECIAL		48
#define PAYLOAD_VALENCE		52
#define PAYLOAD_FOLDER_COUNT	56
#define PAYLOAD_BYTES		60u

/* The compression byte of a child salvage admits although its attributes were
 * rejected. hfsplus_compress_known refuses it, so payload_decode rejects the
 * entry at replay, where the loss is reported under the child's own name.
 */
#define CHILD_COMPRESSION_REJECTED	0xffu

_Static_assert(PAYLOAD_BYTES <= FSIMG_DIRECTORY_PAYLOAD_BYTES,
    "the HFS Plus payload fits the common bound");
_Static_assert(HFSPLUS_KEY_BYTES_MAX <= FSIMG_DIRECTORY_KEY_BYTES,
    "an HFS Plus namespace key fits a directory key buffer");
_Static_assert(HFSPLUS_NAME_UTF8_MAX < FSIMG_DIRECTORY_NAME_BYTES,
    "an HFS Plus image path component fits a directory record");

/* Seconds from 1904-01-01 to the Unix epoch. */
#define HFSPLUS_EPOCH_OFFSET	INT64_C(2082844800)


/* One directory under preparation. cursor holds the catalog's scan buffer,
 * which keyed searches for a child leave alone. previous owns the last key
 * read, whose bytes the next leaf would overwrite.
 */
typedef struct hfsplus_prepare {
	fsimg_reader_t		*reader;
	fsimg_directory_build_t	*build;
	hfsplus_cursor_t	 cursor;
	hfsplus_ckey_t		 previous;
	uint32_t		 id;
	uint32_t		 valence;
	uint32_t		 folder_count;
	uint32_t		 children;
	uint32_t		 folders;
	bool			 check_folder_count;
	bool			 root;
	bool			 journal_info_seen;
	unsigned char		 previous_name[2 * HFSPLUS_NAME_UNITS_MAX];
	char			 path[HFSPLUS_NAME_UTF8_MAX + 1];
} hfsplus_prepare_t;

static fsimg_status_t enter_root(hfsplus_prepare_t *, fsimg_error_t *);
static fsimg_status_t enter_folder(hfsplus_prepare_t *, fsimg_error_t *);
static fsimg_status_t admit_child(hfsplus_prepare_t *, hfsplus_child_t *,
    fsimg_error_t *);
static void payload_encode(const hfsplus_child_t *, unsigned char *);
static fsimg_status_t payload_decode(const fsimg_hfsplus_reader_t *,
    const unsigned char *, size_t, hfsplus_child_t *, fsimg_error_t *);
static fsimg_status_t prepare_owned(hfsplus_prepare_t *, hfsplus_owned_t,
    const hfsplus_record_t *, fsimg_error_t *);
static fsimg_status_t attributes_classify(fsimg_reader_t *,
    const hfsplus_primary_t *, uint32_t, bool *, hfsplus_attr_scan_t *,
    fsimg_error_t *);
static fsimg_status_t prepare_named(hfsplus_prepare_t *,
    const hfsplus_record_t *, fsimg_error_t *);
static fsimg_status_t prepare_child(hfsplus_prepare_t *, const hfsplus_ckey_t *,
    const hfsplus_record_t *, fsimg_error_t *);
static fsimg_status_t prepare_scan(hfsplus_prepare_t *, fsimg_error_t *);
static fsimg_status_t prepare_finish(const hfsplus_prepare_t *,
    fsimg_error_t *);
static void timestamp_set(fsimg_timestamp_t *, uint32_t);

static void
payload_encode(const hfsplus_child_t *child, unsigned char *payload)
{
	fsimg_store_le32(payload + PAYLOAD_ID, child->id);
	fsimg_store_le32(payload + PAYLOAD_LEAF, child->leaf);
	fsimg_store_le16(payload + PAYLOAD_RECORD, child->record);
	payload[PAYLOAD_KIND] = (unsigned char)child->kind;
	payload[PAYLOAD_EXTRA] = child->extra_data ? 1 : 0;
	payload[PAYLOAD_COMPRESSION] = child->compression;
	fsimg_store_le64(payload + PAYLOAD_SIZE, child->size);
	fsimg_store_le32(payload + PAYLOAD_OWNER, child->owner);
	fsimg_store_le32(payload + PAYLOAD_GROUP, child->group);
	fsimg_store_le16(payload + PAYLOAD_MODE, child->mode);
	payload[PAYLOAD_FLAGS] = child->flags;
	fsimg_store_le32(payload + PAYLOAD_CREATED, child->dates.created);
	fsimg_store_le32(payload + PAYLOAD_MODIFIED,
	    child->dates.content_modified);
	fsimg_store_le32(payload + PAYLOAD_CHANGED,
	    child->dates.attribute_modified);
	fsimg_store_le32(payload + PAYLOAD_ACCESSED, child->dates.accessed);
	fsimg_store_le32(payload + PAYLOAD_SPECIAL, child->special);
	fsimg_store_le32(payload + PAYLOAD_VALENCE, child->valence);
	fsimg_store_le32(payload + PAYLOAD_FOLDER_COUNT, child->folder_count);
}

/* The payload came back from scratch storage, so what prepare cannot have
 * written is refused before anything uses it. The record index is left to the
 * cursor's seek, which checks it against the node it reads; a size and the
 * times can hold any value.
 */
static fsimg_status_t
payload_decode(const fsimg_hfsplus_reader_t *hfsplus,
    const unsigned char *payload, size_t length, hfsplus_child_t *child,
    fsimg_error_t *error)
{
	bool valid;

	if (length != PAYLOAD_BYTES)
		return (hfsplus_corrupt(error,
		    "invalid prepared HFS Plus entry payload"));
	bzero(child, sizeof(*child));
	child->id = fsimg_load_le32(payload + PAYLOAD_ID);
	child->leaf = fsimg_load_le32(payload + PAYLOAD_LEAF);
	child->record = fsimg_load_le16(payload + PAYLOAD_RECORD);
	child->kind = (fsimg_entry_kind_t)payload[PAYLOAD_KIND];
	child->extra_data = payload[PAYLOAD_EXTRA] != 0;
	child->compression = payload[PAYLOAD_COMPRESSION];
	child->size = fsimg_load_le64(payload + PAYLOAD_SIZE);
	child->owner = fsimg_load_le32(payload + PAYLOAD_OWNER);
	child->group = fsimg_load_le32(payload + PAYLOAD_GROUP);
	child->mode = fsimg_load_le16(payload + PAYLOAD_MODE);
	child->flags = payload[PAYLOAD_FLAGS];
	child->dates.created = fsimg_load_le32(payload + PAYLOAD_CREATED);
	child->dates.content_modified =
	    fsimg_load_le32(payload + PAYLOAD_MODIFIED);
	child->dates.attribute_modified =
	    fsimg_load_le32(payload + PAYLOAD_CHANGED);
	child->dates.accessed = fsimg_load_le32(payload + PAYLOAD_ACCESSED);
	child->special = fsimg_load_le32(payload + PAYLOAD_SPECIAL);
	child->valence = fsimg_load_le32(payload + PAYLOAD_VALENCE);
	child->folder_count = fsimg_load_le32(payload + PAYLOAD_FOLDER_COUNT);

	switch (child->kind) {
	case FSIMG_ENTRY_FILE:
		valid = true;
		break;
	case FSIMG_ENTRY_SYMLINK:
		valid = child->size >= 1 &&
		    child->size <= HFSPLUS_LINK_TARGET_MAX;
		break;
	case FSIMG_ENTRY_DIRECTORY:
	case FSIMG_ENTRY_FIFO:
	case FSIMG_ENTRY_SOCKET:
	case FSIMG_ENTRY_CHAR_DEVICE:
	case FSIMG_ENTRY_BLOCK_DEVICE:
		valid = child->size == 0;
		break;
	default:
		valid = false;
		break;
	}
	if (child->kind != FSIMG_ENTRY_DIRECTORY && (child->valence != 0 ||
	    child->folder_count != 0))
		valid = false;
	if (child->kind != FSIMG_ENTRY_CHAR_DEVICE &&
	    child->kind != FSIMG_ENTRY_BLOCK_DEVICE && child->special != 0)
		valid = false;
	if ((child->flags & HFSPLUS_CHILD_OWNER_MODE) == 0 &&
	    (child->owner != 0 || child->group != 0 || child->mode != 0))
		valid = false;
	if ((child->flags & HFSPLUS_CHILD_FOLDER_COUNT) != 0 ?
	    child->kind != FSIMG_ENTRY_DIRECTORY : child->folder_count != 0)
		valid = false;
	if (!valid || child->id < HFSPLUS_ID_FIRST_USER ||
	    child->leaf >= hfsplus->catalog.header.total_nodes ||
	    payload[PAYLOAD_EXTRA] > 1 ||
	    (child->compression != 0 && (child->kind != FSIMG_ENTRY_FILE ||
	    !hfsplus_compress_known(child->compression))) ||
	    child->mode > FSIMG_MODE_BITS ||
	    (child->flags & ~HFSPLUS_CHILD_FLAGS) != 0)
		return (hfsplus_corrupt(error,
		    "invalid prepared HFS Plus entry payload"));
	return (FSIMG_OK);
}

/* Open checked the root's folder and thread, parent 1 included, so entering it
 * only positions the cursor; the record is what the counts come from.
 */
static fsimg_status_t
enter_root(hfsplus_prepare_t *prepare, fsimg_error_t *error)
{
	const hfsplus_folder_t *root = &hfsplus_state(prepare->reader)->root;
	hfsplus_thread_t thread;

	prepare->id = HFSPLUS_ID_ROOT;
	prepare->valence = root->valence;
	prepare->folder_count = root->folder_count;
	prepare->check_folder_count =
	    (root->flags & HFSPLUS_FLAG_FOLDER_COUNT) != 0;
	return (hfsplus_thread_lookup(&prepare->cursor, HFSPLUS_ID_ROOT,
	    HFSPLUS_RECORD_FOLDER_THREAD, &thread, error));
}

/* The folder replay made current: its own record's key is the parent the
 * walker reached it from and its stored name, and the folder's one thread
 * must name both. On a static image a second parent, a cycle back to an
 * ancestor and a folder record reusing another folder's ID all fail here; a
 * source changing underneath is bounded by the depth, path and work limits
 * and reported through fsimg_reader_stable.
 */
static fsimg_status_t
enter_folder(hfsplus_prepare_t *prepare, fsimg_error_t *error)
{
	fsimg_reader_t *reader = prepare->reader;
	const hfsplus_child_t *current = &hfsplus_state(reader)->current;
	hfsplus_folder_t folder;
	hfsplus_thread_t thread;
	hfsplus_record_t record;
	hfsplus_ckey_t stored;
	hfsplus_key_t expected;
	fsimg_status_t status;

	if (current->kind != FSIMG_ENTRY_DIRECTORY)
		return (hfsplus_corrupt(error,
		    "invalid prepared HFS Plus entry payload"));
	prepare->id = current->id;
	prepare->valence = current->valence;
	prepare->folder_count = current->folder_count;
	prepare->check_folder_count =
	    (current->flags & HFSPLUS_CHILD_FOLDER_COUNT) != 0;
	status = hfsplus_cursor_seek(&prepare->cursor, current->leaf,
	    current->record, error);
	if (status == FSIMG_OK)
		status = hfsplus_cursor_record(&prepare->cursor, &record,
		    error);
	if (status != FSIMG_OK)
		return (status);
	if (!hfsplus_ckey_decode(record.key, record.key_bytes, &stored) ||
	    !hfsplus_folder_decode(record.data, record.data_bytes, &folder) ||
	    folder.id != current->id)
		return (hfsplus_corrupt(error,
		    "HFS Plus folder record changed since it was listed"));
	/* expected owns its name, which must outlive the search buffer. */
	status = hfsplus_catalog_key_init(&expected, stored.parent, stored.name,
	    stored.units, error);
	if (status == FSIMG_OK)
		status = hfsplus_thread_lookup(&prepare->cursor, current->id,
		    HFSPLUS_RECORD_FOLDER_THREAD, &thread, error);
	if (status != FSIMG_OK)
		return (status);
	if (thread.parent != expected.catalog.parent ||
	    thread.units != expected.catalog.units ||
	    bcmp(thread.name, expected.catalog.name,
	    2 * (size_t)thread.units) != 0)
		return (hfsplus_corrupt(error,
		    "HFS Plus folder thread names another parent or name"));
	return (FSIMG_OK);
}

bool
hfsplus_owned_find(const fsimg_hfsplus_reader_t *hfsplus,
    const hfsplus_ckey_t *key, hfsplus_owned_t *owned)
{
	bool journaled = hfsplus->journal != HFSPLUS_JOURNAL_NONE;

	if (key->parent != HFSPLUS_ID_ROOT)
		return (false);
	for (int i = 0; i < HFSPLUS_OWNED_COUNT; i++) {
		if (!journaled && (i == HFSPLUS_OWNED_JOURNAL ||
		    i == HFSPLUS_OWNED_JOURNAL_INFO))
			continue;
		if (key->units == hfsplus_owned_names[i].count &&
		    bcmp(key->name, hfsplus_owned_names[i].units,
		    2 * (size_t)key->units) == 0) {
			*owned = (hfsplus_owned_t)i;
			return (true);
		}
	}
	return (false);
}

/* Validate a format-owned child, which is counted but never admitted. A private
 * folder must be a folder. Open located the journal through .journal's record,
 * so only the info block's file is left to check against the header.
 */
static fsimg_status_t
prepare_owned(hfsplus_prepare_t *prepare, hfsplus_owned_t owned,
    const hfsplus_record_t *record, fsimg_error_t *error)
{
	fsimg_reader_t *reader = prepare->reader;
	const fsimg_hfsplus_reader_t *hfsplus = hfsplus_state(reader);
	hfsplus_primary_t primary;
	hfsplus_extent_t extent;
	fsimg_status_t status;

	status = hfsplus_record_validate(reader, record, &primary, error);
	if (status != FSIMG_OK)
		return (status);
	switch (owned) {
	case HFSPLUS_OWNED_PRIVATE_DATA:
	case HFSPLUS_OWNED_PRIVATE_DIRECTORY:
		if (primary.kind != FSIMG_ENTRY_DIRECTORY)
			return (hfsplus_corrupt(error,
			    "HFS Plus private folder is not a folder"));
		return (FSIMG_OK);
	case HFSPLUS_OWNED_JOURNAL:
		if (primary.kind != FSIMG_ENTRY_FILE)
			return (hfsplus_corrupt(error,
			    "HFS Plus journal file is not a regular file"));
		return (FSIMG_OK);
	case HFSPLUS_OWNED_JOURNAL_INFO:
		if (primary.kind != FSIMG_ENTRY_FILE)
			return (hfsplus_corrupt(error,
			    "HFS Plus journal info file is not a regular file"));
		status = hfsplus_fork_one_extent(reader,
		    &primary.file.data_fork, &extent, error);
		if (status != FSIMG_OK)
			return (status);
		if (extent.start != hfsplus->journal_info_block)
			return (hfsplus_corrupt(error,
			    "HFS Plus .journal_info_block disagrees with the header"));
		prepare->journal_info_seen = true;
		return (FSIMG_OK);
	default:
		return (hfsplus_corrupt(error, "unknown HFS Plus owned name"));
	}
}

/* Whether the child with this effective record is compressed, and what its
 * attributes hold: the scan runs only for a record whose flags say to look, and
 * is empty otherwise.
 */
static fsimg_status_t
attributes_classify(fsimg_reader_t *reader, const hfsplus_primary_t *primary,
    uint32_t id, bool *compressed, hfsplus_attr_scan_t *scan,
    fsimg_error_t *error)
{
	uint16_t flags = primary->kind == FSIMG_ENTRY_DIRECTORY ?
	    primary->folder.flags : primary->file.flags;

	*compressed = primary->kind == FSIMG_ENTRY_FILE &&
	    (primary->file.bsd.owner_flags & HFSPLUS_OWNER_COMPRESSED) != 0;
	if (!*compressed && (flags & HFSPLUS_FLAG_ATTRIBUTES) == 0) {
		bzero(scan, sizeof(*scan));
		return (FSIMG_OK);
	}
	return (hfsplus_attr_scan(reader, id, *compressed, NULL, scan, error));
}

static fsimg_status_t
admit_child(hfsplus_prepare_t *prepare, hfsplus_child_t *child,
    fsimg_error_t *error)
{
	unsigned char payload[PAYLOAD_BYTES];

	payload_encode(child, payload);
	return (fsimg_directory_admit(prepare->build, prepare->path, payload,
	    sizeof(payload), error));
}

/* Step 4 of the driver design and step 3's record check, for a child whose name
 * is in prepare->path: classify the record, then admit the child or refuse it.
 * A stub takes its indirect node's record for everything but the name. A
 * compressed file's size is the attribute's, the data fork being empty, and the
 * resource fork that holds its chunks is content, not extra data. At size 0 no
 * read reaches its fork or payload, so whatever they hold is extra data. Under
 * salvage, attributes that fail to decode cost this child rather than its
 * directory's listing: a compressed file is admitted for replay to reject, and any
 * other child is admitted with its attributes counted as extra data.
 */
static fsimg_status_t
prepare_named(hfsplus_prepare_t *prepare, const hfsplus_record_t *record,
    fsimg_error_t *error)
{
	fsimg_reader_t *reader = prepare->reader;
	const hfsplus_cursor_t *location = &prepare->cursor;
	hfsplus_attr_scan_t scan;
	hfsplus_primary_t primary;
	hfsplus_cursor_t link;
	hfsplus_child_t child;
	bool compressed;
	fsimg_status_t status;

	status = hfsplus_record_validate(reader, record, &primary, error);
	if (status != FSIMG_OK)
		return (status);
	if (primary.link == HFSPLUS_LINK_DIRECTORY)
		return (hfsplus_unsupported(error,
		    "HFS Plus directory hard link is not supported"));
	if (primary.link == HFSPLUS_LINK_FILE) {
		status = hfsplus_link_resolve(reader, &primary, &link, error);
		if (status != FSIMG_OK)
			return (status);
		location = &link;
	}
	if (primary.kind == FSIMG_ENTRY_DIRECTORY)
		prepare->folders++;
	status = hfsplus_child_fill(&primary, &child, error);
	if (status != FSIMG_OK)
		return (status);
	child.leaf = location->number;
	child.record = location->record;
	status = attributes_classify(reader, &primary, child.id, &compressed,
	    &scan, error);
	if (status == FSIMG_OK && compressed)
		status = hfsplus_compress_check(&primary.file, &scan, error);
	if (status != FSIMG_OK) {
		if (!fsimg_reader_decode_rejected(reader, error, status))
			return (status);
		fsimg_error_clear(error);
		if (compressed) {
			child.compression = CHILD_COMPRESSION_REJECTED;
			return (admit_child(prepare, &child, error));
		}
		scan.stored = true;
	} else if (compressed) {
		child.size = scan.cmpf.uncompressed_size;
		child.compression = (uint8_t)scan.cmpf.type;
	}
	child.extra_data = scan.stored ||
	    (primary.kind != FSIMG_ENTRY_DIRECTORY &&
	    primary.file.resource_fork.logical_size > 0 &&
	    (child.size == 0 ||
	    !hfsplus_compress_consumes_fork(child.compression))) ||
	    (compressed && child.size == 0 && scan.payload_bytes != 0);
	return (admit_child(prepare, &child, error));
}

/* Steps 3 and 4 of the driver design: validate the name and set the
 * format-owned children aside. Every failure past that point fails the
 * directory because of this one child, so it is named here, once.
 */
static fsimg_status_t
prepare_child(hfsplus_prepare_t *prepare, const hfsplus_ckey_t *key,
    const hfsplus_record_t *record, fsimg_error_t *error)
{
	hfsplus_owned_t owned;
	size_t bytes;
	fsimg_status_t status;

	if (hfsplus_owned_find(hfsplus_state(prepare->reader), key, &owned)) {
		status = prepare_owned(prepare, owned, record, error);
		if (status == FSIMG_OK &&
		    (owned == HFSPLUS_OWNED_PRIVATE_DATA ||
		    owned == HFSPLUS_OWNED_PRIVATE_DIRECTORY))
			prepare->folders++;
		return (status);
	}
	status = hfsplus_name_admit(key->name, key->units, error);
	if (status == FSIMG_OK)
		status = hfsplus_name_to_path(key->name, key->units,
		    prepare->path, &bytes, error);
	if (status != FSIMG_OK)
		return (status);
	prepare->path[bytes] = '\0';

	status = prepare_named(prepare, record, error);
	if (status != FSIMG_OK)
		fsimg_directory_name_child(prepare->build, prepare->path,
		    error);
	return (status);
}

/* Step 2: walk from the thread while the parent is this directory. Each key
 * must exceed the one before it, across the end of the directory too, so
 * order bounds the scan whatever the sibling links say.
 */
static fsimg_status_t
prepare_scan(hfsplus_prepare_t *prepare, fsimg_error_t *error)
{
	hfsplus_compare_t compare =
	    hfsplus_state(prepare->reader)->catalog.compare;
	hfsplus_cursor_t *cursor = &prepare->cursor;
	hfsplus_record_t record;
	hfsplus_ckey_t key;
	fsimg_status_t status;

	prepare->previous = (hfsplus_ckey_t){
		.name = prepare->previous_name,
		.parent = prepare->id,
		.units = 0
	};
	for (;;) {
		status = hfsplus_cursor_next(cursor, error);
		if (status != FSIMG_OK || !cursor->valid)
			return (status);
		status = hfsplus_cursor_record(cursor, &record, error);
		if (status != FSIMG_OK)
			return (status);
		if (!hfsplus_ckey_decode(record.key, record.key_bytes, &key))
			return (hfsplus_corrupt(error,
			    "HFS Plus key is malformed"));
		if (hfsplus_ckey_compare(compare, &prepare->previous,
		    &key) >= 0)
			return (hfsplus_corrupt(error,
			    "HFS Plus catalog keys are out of order"));
		if (key.parent != prepare->id)
			return (FSIMG_OK);
		prepare->children++;
		status = prepare_child(prepare, &key, &record, error);
		if (status != FSIMG_OK)
			return (status);
		bcopy(key.name, prepare->previous_name, 2 * (size_t)key.units);
		prepare->previous.units = key.units;
	}
}

/* Step 5, and on the root the journal files a journaled volume must hold. */
static fsimg_status_t
prepare_finish(const hfsplus_prepare_t *prepare, fsimg_error_t *error)
{
	const fsimg_hfsplus_reader_t *hfsplus =
	    hfsplus_state(prepare->reader);

	if (prepare->children != prepare->valence)
		return (hfsplus_corrupt(error,
		    "HFS Plus folder's valence disagrees with its children"));
	if (prepare->check_folder_count &&
	    prepare->folders != prepare->folder_count)
		return (hfsplus_corrupt(error,
		    "HFS Plus folder count disagrees with its child folders"));
	if (prepare->root && hfsplus->journal != HFSPLUS_JOURNAL_NONE &&
	    !prepare->journal_info_seen)
		return (hfsplus_corrupt(error,
		    "HFS Plus journaled volume is missing a journal file"));
	return (FSIMG_OK);
}

/* List the root at level 0, otherwise the directory replay made current. */
fsimg_status_t
hfsplus_prepare(fsimg_reader_t *reader, fsimg_directory_build_t *build,
    fsimg_error_t *error)
{
	fsimg_hfsplus_reader_t *hfsplus = hfsplus_state(reader);
	hfsplus_prepare_t *prepare;
	fsimg_error_t detail;
	fsimg_status_t status;

	prepare = fsimg_alloc(reader->ctx, sizeof(*prepare), &detail);
	if (prepare == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	bzero(prepare, sizeof(*prepare));
	prepare->reader = reader;
	prepare->build = build;
	prepare->root = fsimg_directory_level(build) == 0;
	hfsplus_cursor_init(&prepare->cursor, &hfsplus->catalog,
	    hfsplus->catalog_scan_node);
	status = prepare->root ? enter_root(prepare, error) :
	    enter_folder(prepare, error);
	if (status == FSIMG_OK)
		status = prepare_scan(prepare, error);
	if (status == FSIMG_OK)
		status = prepare_finish(prepare, error);
	fsimg_dealloc(prepare);
	return (status);
}

static void
timestamp_set(fsimg_timestamp_t *timestamp, uint32_t stored)
{
	timestamp->seconds = (int64_t)stored - HFSPLUS_EPOCH_OFFSET;
	timestamp->nanoseconds = 0;
	timestamp->precision_ns = UINT64_C(1000000000);
	timestamp->basis = FSIMG_TIME_UTC;
	timestamp->offset_minutes = 0;
}

/* Fill the current entry from the payload alone: listing a directory reads no
 * node twice. A later resolve or prepare reads the record again by the location
 * the payload holds.
 */
fsimg_status_t
hfsplus_replay(fsimg_reader_t *reader, const char *name,
    const unsigned char *payload, size_t length, fsimg_error_t *error)
{
	fsimg_hfsplus_reader_t *hfsplus = hfsplus_state(reader);
	unsigned char key[HFSPLUS_KEY_BYTES_MAX];
	fsimg_metadata_t *metadata = &reader->entry.metadata;
	hfsplus_child_t child;
	fsimg_error_t refused;
	size_t key_bytes;
	fsimg_status_t status;

	fsimg_extent_index_free(hfsplus->file);
	hfsplus->file = NULL;
	hfsplus_compress_reset(hfsplus);
	status = payload_decode(hfsplus, payload, length, &child, error);
	if (status != FSIMG_OK)
		return (status);
	if (hfsplus_query_key(hfsplus->catalog.compare, name, strlen(name), key,
	    &key_bytes, &refused) != FSIMG_OK)
		return (hfsplus_corrupt(error,
		    "invalid prepared HFS Plus file name"));
	hfsplus->current = child;

	reader->entry.kind = child.kind;
	reader->entry.size = child.size;
	bzero(metadata, sizeof(*metadata));
	metadata->valid = FSIMG_META_CREATED | FSIMG_META_MODIFIED |
	    FSIMG_META_CHANGED | FSIMG_META_ACCESSED;
	timestamp_set(&metadata->created, child.dates.created);
	timestamp_set(&metadata->modified, child.dates.content_modified);
	timestamp_set(&metadata->changed, child.dates.attribute_modified);
	timestamp_set(&metadata->accessed, child.dates.accessed);
	if ((child.flags & HFSPLUS_CHILD_OWNER_MODE) != 0) {
		metadata->valid |= FSIMG_META_OWNER | FSIMG_META_MODE;
		metadata->uid = child.owner;
		metadata->gid = child.group;
		metadata->mode = child.mode;
	}
	if (child.kind == FSIMG_ENTRY_CHAR_DEVICE ||
	    child.kind == FSIMG_ENTRY_BLOCK_DEVICE) {
		metadata->valid |= FSIMG_META_DEVICE;
		metadata->dev_major = child.special >> 24;
		metadata->dev_minor = child.special & UINT32_C(0xffffff);
	}
	if (child.extra_data)
		metadata->valid |= FSIMG_META_EXTRA_DATA;
	reader->initialized = child.size;
	return (FSIMG_OK);
}

/* One key per entry. A name prepare admitted maps back to itself, so its key
 * is the query mapping of what it displays as; a failure means the record
 * came back from scratch storage changed.
 */
fsimg_status_t
hfsplus_keys(fsimg_reader_t *reader, const char *name,
    const unsigned char *payload FSIMG_UNUSED, size_t length FSIMG_UNUSED,
    fsimg_directory_keys_t *keys, fsimg_error_t *error)
{
	fsimg_error_t refused;

	keys->count = 0;
	if (hfsplus_query_key(hfsplus_state(reader)->catalog.compare, name,
	    strlen(name), keys->key[0], &keys->length[0], &refused) != FSIMG_OK)
		return (hfsplus_corrupt(error, "invalid HFS Plus file name"));
	keys->count = 1;
	return (FSIMG_OK);
}

fsimg_status_t
hfsplus_key(fsimg_reader_t *reader, const char *component, unsigned char *key,
    size_t *length, fsimg_error_t *error)
{
	return (hfsplus_query_key(hfsplus_state(reader)->catalog.compare,
	    component, strlen(component), key, length, error));
}
