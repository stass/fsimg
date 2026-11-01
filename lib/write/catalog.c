/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "host.h"
#include "source.h"
#include "unicode.h"
#include "writer.h"

typedef struct catalog_ancestor {
	uint64_t	 id;
	size_t		 path_length;
} catalog_ancestor_t;

static int64_t decode_signed(uint64_t);
static int32_t decode_signed32(uint32_t);
static void encode_time(unsigned char *, const fsimg_timestamp_t *);
static bool decode_time(const unsigned char *, fsimg_timestamp_t *);
static bool entry_shape_valid(uint32_t, uint64_t, uint32_t, uint64_t);
static void encode_metadata(unsigned char *, const fsimg_metadata_t *);
static bool decode_metadata(const unsigned char *, fsimg_metadata_t *);
static bool storage_matches_origin(uint32_t, fsimg_catalog_origin_t);
static bool rank_valid(const fsimg_writer_t *, const unsigned char *,
    uint32_t);
static fsimg_status_t decode_entry(fsimg_writer_t *, const unsigned char *,
    size_t, fsimg_catalog_entry_t *, fsimg_error_t *);
static fsimg_status_t read_entry(fsimg_writer_t *, uint64_t, unsigned char *,
    fsimg_catalog_entry_t *, fsimg_error_t *);
static int path_compare(const void *, size_t, const void *, size_t, void *);
static int rank_compare(const unsigned char *, const unsigned char *);
static int entry_compare(const void *, size_t, const void *, size_t, void *);
static int child_compare(const void *, size_t, const void *, size_t, void *);
static fsimg_status_t resolve_repeated_path(fsimg_entry_kind_t,
    const unsigned char *, const unsigned char *, const fsimg_entry_t *,
    fsimg_error_t *);
static bool find_parent(const catalog_ancestor_t *, size_t *, const char *,
    const char *, size_t);
static fsimg_status_t index_entries(fsimg_writer_t *, fsimg_stream_t *,
    fsimg_error_t *);
static fsimg_status_t set_child_count(fsimg_writer_t *, uint64_t, uint64_t,
    fsimg_error_t *);

static int64_t
decode_signed(uint64_t value)
{
	if (value <= INT64_MAX)
		return ((int64_t)value);
	return (-1 - (int64_t)(UINT64_MAX - value));
}

static int32_t
decode_signed32(uint32_t value)
{
	if (value <= INT32_MAX)
		return ((int32_t)value);
	return (-1 - (int32_t)(UINT32_MAX - value));
}

static void
encode_time(unsigned char *bytes, const fsimg_timestamp_t *time)
{
	fsimg_store_le64(bytes, (uint64_t)time->seconds);
	fsimg_store_le64(bytes + 8, time->precision_ns);
	fsimg_store_le32(bytes + 16, time->nanoseconds);
	fsimg_store_le32(bytes + 20, (uint32_t)time->basis);
	fsimg_store_le32(bytes + 24, (uint32_t)time->offset_minutes);
}

/* Range checks precede every narrowing; the trailing padding must be zero. */
static bool
decode_time(const unsigned char *bytes, fsimg_timestamp_t *time)
{
	uint32_t nanoseconds, basis;
	int32_t offset;

	nanoseconds = fsimg_load_le32(bytes + 16);
	basis = fsimg_load_le32(bytes + 20);
	offset = decode_signed32(fsimg_load_le32(bytes + 24));
	if (nanoseconds >= 1000000000 || basis < FSIMG_TIME_UTC ||
	    basis > FSIMG_TIME_WALL || offset <= -1440 || offset >= 1440 ||
	    fsimg_load_le32(bytes + 28) != 0)
		return (false);
	time->seconds = decode_signed(fsimg_load_le64(bytes));
	time->precision_ns = fsimg_load_le64(bytes + 8);
	time->nanoseconds = nanoseconds;
	time->basis = (fsimg_time_basis_t)basis;
	time->offset_minutes = offset;
	return (true);
}

/* The closed relation between an entry's kind, size and source. Files and
 * symbolic links have content and need a source; a link's size is its
 * target length, and its target cannot be the zero source's NUL bytes.
 * Directories and FIFOs have neither size nor source. A zero source carries
 * no identifier.
 */
static bool
entry_shape_valid(uint32_t kind, uint64_t size, uint32_t storage,
    uint64_t source_id)
{
	if (storage > FSIMG_STORAGE_GENERATED ||
	    (storage == FSIMG_STORAGE_ZERO && source_id != 0))
		return (false);
	switch (kind) {
	case FSIMG_ENTRY_FILE:
		return (storage != FSIMG_STORAGE_NONE);
	case FSIMG_ENTRY_SYMLINK:
		return (storage != FSIMG_STORAGE_NONE &&
		    storage != FSIMG_STORAGE_ZERO && size != 0 &&
		    size <= FSIMG_SYMLINK_TARGET_MAX);
	case FSIMG_ENTRY_DIRECTORY:
	case FSIMG_ENTRY_FIFO:
		return (storage == FSIMG_STORAGE_NONE && size == 0 &&
		    source_id == 0);
	default:
		return (false);
	}
}

/* Only valid fields are written; the caller zeroed the header, so absent
 * fields and padding are defined and never carry caller bytes.
 */
static void
encode_metadata(unsigned char *record, const fsimg_metadata_t *metadata)
{
	fsimg_store_le32(record + FSIMG_CATALOG_VALID, metadata->valid);
	fsimg_store_le32(record + FSIMG_CATALOG_REQUIRED, metadata->required);
	if ((metadata->valid & FSIMG_META_ATTRIBUTES) != 0)
		fsimg_store_le32(record + FSIMG_CATALOG_ATTRIBUTES,
		    metadata->attributes);
	if ((metadata->valid & FSIMG_META_OWNER) != 0) {
		fsimg_store_le32(record + FSIMG_CATALOG_UID, metadata->uid);
		fsimg_store_le32(record + FSIMG_CATALOG_GID, metadata->gid);
	}
	if ((metadata->valid & FSIMG_META_MODE) != 0)
		fsimg_store_le32(record + FSIMG_CATALOG_MODE, metadata->mode);
	if ((metadata->valid & FSIMG_META_CREATED) != 0)
		encode_time(record + FSIMG_CATALOG_CREATED, &metadata->created);
	if ((metadata->valid & FSIMG_META_MODIFIED) != 0)
		encode_time(record + FSIMG_CATALOG_MODIFIED, &metadata->modified);
	if ((metadata->valid & FSIMG_META_ACCESSED) != 0)
		encode_time(record + FSIMG_CATALOG_ACCESSED, &metadata->accessed);
	if ((metadata->valid & FSIMG_META_CHANGED) != 0)
		encode_time(record + FSIMG_CATALOG_CHANGED, &metadata->changed);
}

/* The inverse of encode_metadata. encode_metadata writes an absent field as
 * zero bytes, so any other bytes in an absent slot are corruption; absent
 * fields stay zero in out. The caller validates the masks and values with
 * fsimg_metadata_validate.
 */
static bool
decode_metadata(const unsigned char *record, fsimg_metadata_t *out)
{
	static const unsigned char zero[FSIMG_CATALOG_TIME_BYTES];
	static const struct {
		uint32_t	 field;
		unsigned	 offset;
		size_t		 length;
	} slots[] = {
		{ FSIMG_META_ATTRIBUTES, FSIMG_CATALOG_ATTRIBUTES, 4 },
		{ FSIMG_META_OWNER, FSIMG_CATALOG_UID, 8 },
		{ FSIMG_META_MODE, FSIMG_CATALOG_MODE, 4 },
		{ FSIMG_META_CREATED, FSIMG_CATALOG_CREATED, FSIMG_CATALOG_TIME_BYTES },
		{ FSIMG_META_MODIFIED, FSIMG_CATALOG_MODIFIED, FSIMG_CATALOG_TIME_BYTES },
		{ FSIMG_META_ACCESSED, FSIMG_CATALOG_ACCESSED, FSIMG_CATALOG_TIME_BYTES },
		{ FSIMG_META_CHANGED, FSIMG_CATALOG_CHANGED, FSIMG_CATALOG_TIME_BYTES }
	};

	out->valid = fsimg_load_le32(record + FSIMG_CATALOG_VALID);
	out->required = fsimg_load_le32(record + FSIMG_CATALOG_REQUIRED);
	for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
		if ((out->valid & slots[i].field) == 0 &&
		    bcmp(record + slots[i].offset, zero, slots[i].length) != 0)
			return (false);
	}
	if ((out->valid & FSIMG_META_ATTRIBUTES) != 0)
		out->attributes = fsimg_load_le32(record + FSIMG_CATALOG_ATTRIBUTES);
	if ((out->valid & FSIMG_META_OWNER) != 0) {
		out->uid = fsimg_load_le32(record + FSIMG_CATALOG_UID);
		out->gid = fsimg_load_le32(record + FSIMG_CATALOG_GID);
	}
	if ((out->valid & FSIMG_META_MODE) != 0)
		out->mode = fsimg_load_le32(record + FSIMG_CATALOG_MODE);
	if ((out->valid & FSIMG_META_CREATED) != 0 &&
	    !decode_time(record + FSIMG_CATALOG_CREATED, &out->created))
		return (false);
	if ((out->valid & FSIMG_META_MODIFIED) != 0 &&
	    !decode_time(record + FSIMG_CATALOG_MODIFIED, &out->modified))
		return (false);
	if ((out->valid & FSIMG_META_ACCESSED) != 0 &&
	    !decode_time(record + FSIMG_CATALOG_ACCESSED, &out->accessed))
		return (false);
	if ((out->valid & FSIMG_META_CHANGED) != 0 &&
	    !decode_time(record + FSIMG_CATALOG_CHANGED, &out->changed))
		return (false);
	return (true);
}

fsimg_status_t
fsimg_writer_collect(fsimg_writer_t *writer, const fsimg_entry_t *entry,
    fsimg_source_storage_t storage, uint64_t source_id,
    const fsimg_source_root_t *root, fsimg_error_t *error)
{
	uint64_t payload_bytes;
	size_t path_length;
	uint32_t depth;
	fsimg_status_t status;

	status = fsimg_path_validate(writer->ctx, entry->path, false,
	    &path_length, &depth, error);
	if (status != FSIMG_OK)
		return (status);
	if (!entry_shape_valid(entry->kind, entry->size, storage, source_id) ||
	    !storage_matches_origin(storage, root != NULL ?
	    FSIMG_ORIGIN_SCANNED : FSIMG_ORIGIN_PROGRAMMATIC))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_CATALOG, "entry kind, size and source disagree"));
	status = fsimg_metadata_validate(&entry->metadata, error);
	if (status != FSIMG_OK)
		return (status);
	if (!fsimg_add_u64(writer->payload_bytes, entry->size, &payload_bytes))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_CATALOG, "catalog payload total overflow"));
	status = fsimg_session_charge(&writer->session, 1, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);

	/* Every absent field and all representation bytes have defined contents. */
	bzero(writer->record, FSIMG_CATALOG_HEADER);
	fsimg_store_le64(writer->record + FSIMG_CATALOG_SIZE, entry->size);
	fsimg_store_le64(writer->record + FSIMG_CATALOG_SOURCE_ID, source_id);
	fsimg_store_le32(writer->record + FSIMG_CATALOG_STORAGE, storage);
	fsimg_store_le32(writer->record + FSIMG_CATALOG_KIND, entry->kind);
	encode_metadata(writer->record, &entry->metadata);
	if (root != NULL) {
		fsimg_store_le32(writer->record + FSIMG_CATALOG_ORIGIN,
		    FSIMG_ORIGIN_SCANNED);
		fsimg_store_le64(writer->record + FSIMG_CATALOG_ORDER, root->id);
	} else {
		fsimg_store_le32(writer->record + FSIMG_CATALOG_ORIGIN,
		    FSIMG_ORIGIN_PROGRAMMATIC);
	}
	bcopy(entry->path, writer->record + FSIMG_CATALOG_HEADER, path_length + 1);
	status = fsimg_stream_append(writer->entries, writer->record,
	    FSIMG_CATALOG_HEADER + path_length + 1, NULL, error);
	if (status != FSIMG_OK)
		return (status);
	writer->collected++;
	writer->payload_bytes = payload_bytes;
	return (FSIMG_OK);
}

/* Which storage each origin can carry: a root's scan or manifest yields no
 * storage, the zero source, a host object or generated content, never the
 * caller's provider; a programmatic record has no storage, the zero source
 * or the provider, never a host object or generated content, which only a
 * root can describe.
 */
static bool
storage_matches_origin(uint32_t storage, fsimg_catalog_origin_t origin)
{
	switch (storage) {
	case FSIMG_STORAGE_NONE:
	case FSIMG_STORAGE_ZERO:
		return (true);
	case FSIMG_STORAGE_PROVIDER:
		return (origin == FSIMG_ORIGIN_PROGRAMMATIC);
	case FSIMG_STORAGE_HOST:
	case FSIMG_STORAGE_GENERATED:
		return (origin == FSIMG_ORIGIN_SCANNED);
	default:
		return (false);
	}
}

/* The rank fields are closed, mirroring fsimg_writer_collect: zero padding;
 * a scanned record names a registered root, a programmatic record has
 * order zero, and the storage is one the origin can carry.
 */
static bool
rank_valid(const fsimg_writer_t *writer, const unsigned char *record,
    uint32_t storage)
{
	uint64_t order;

	if (fsimg_load_le32(record + FSIMG_CATALOG_ORIGIN + 4) != 0)
		return (false);
	order = fsimg_load_le64(record + FSIMG_CATALOG_ORDER);
	switch (fsimg_load_le32(record + FSIMG_CATALOG_ORIGIN)) {
	case FSIMG_ORIGIN_SCANNED:
		return (order < writer->root_count &&
		    storage_matches_origin(storage, FSIMG_ORIGIN_SCANNED));
	case FSIMG_ORIGIN_PROGRAMMATIC:
		return (order == 0 &&
		    storage_matches_origin(storage, FSIMG_ORIGIN_PROGRAMMATIC));
	default:
		return (false);
	}
}

static fsimg_status_t
decode_entry(fsimg_writer_t *writer, const unsigned char *bytes, size_t length,
    fsimg_catalog_entry_t *out, fsimg_error_t *error)
{
	uint32_t kind, storage;
	size_t path_length;
	uint32_t depth;
	fsimg_status_t status;

	bzero(out, sizeof(*out));
	if (length <= FSIMG_CATALOG_HEADER ||
	    strnlen((const char *)bytes + FSIMG_CATALOG_HEADER,
	    length - FSIMG_CATALOG_HEADER) != length - FSIMG_CATALOG_HEADER - 1)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_CATALOG, "invalid catalog record path framing"));
	kind = fsimg_load_le32(bytes + FSIMG_CATALOG_KIND);
	storage = fsimg_load_le32(bytes + FSIMG_CATALOG_STORAGE);
	out->entry.path = (const char *)bytes + FSIMG_CATALOG_HEADER;
	out->entry.size = fsimg_load_le64(bytes + FSIMG_CATALOG_SIZE);
	out->source_id = fsimg_load_le64(bytes + FSIMG_CATALOG_SOURCE_ID);
	if (!entry_shape_valid(kind, out->entry.size, storage, out->source_id) ||
	    !rank_valid(writer, bytes, storage) ||
	    (storage == FSIMG_STORAGE_PROVIDER && !writer->provider_set))
		goto invalid;
	out->entry.kind = (fsimg_entry_kind_t)kind;
	out->storage = (fsimg_source_storage_t)storage;
	if (!decode_metadata(bytes, &out->entry.metadata))
		goto invalid;
	status = fsimg_path_validate(writer->ctx, out->entry.path, false,
	    &path_length, &depth, error);
	if (status != FSIMG_OK)
		goto invalid;
	status = fsimg_metadata_validate(&out->entry.metadata, error);
	if (status == FSIMG_OK)
		return (FSIMG_OK);
invalid:
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
	    FSIMG_OP_CATALOG, "invalid encoded catalog fields"));
}

static int
path_compare(const void *a, size_t a_length, const void *b, size_t b_length,
    void *argument FSIMG_UNUSED)
{
	const unsigned char *left = a, *right = b;
	size_t common;
	unsigned av, bv;

	/* Delimiters sort before component bytes, independently of ASCII '/'.
	 * This keeps every parent's descendants ahead of its longer siblings.
	 */
	if (a_length <= FSIMG_CATALOG_HEADER || b_length <= FSIMG_CATALOG_HEADER)
		return ((a_length > b_length) - (a_length < b_length));
	left += FSIMG_CATALOG_HEADER;
	right += FSIMG_CATALOG_HEADER;
	a_length -= FSIMG_CATALOG_HEADER;
	b_length -= FSIMG_CATALOG_HEADER;
	common = a_length < b_length ? a_length : b_length;
	for (size_t i = 0; i < common; i++) {
		av = left[i] == 0 ? 0 : left[i] == '/' ? 1 : left[i] + 2u;
		bv = right[i] == 0 ? 0 : right[i] == '/' ? 1 : right[i] + 2u;
		if (av != bv)
			return ((av > bv) - (av < bv));
	}
	return ((a_length > b_length) - (a_length < b_length));
}

static int
rank_compare(const unsigned char *a, const unsigned char *b)
{
	uint64_t a_order, b_order;
	uint32_t a_origin, b_origin;

	a_origin = fsimg_load_le32(a + FSIMG_CATALOG_ORIGIN);
	b_origin = fsimg_load_le32(b + FSIMG_CATALOG_ORIGIN);
	if (a_origin != b_origin)
		return ((a_origin > b_origin) - (a_origin < b_origin));
	a_order = fsimg_load_le64(a + FSIMG_CATALOG_ORDER);
	b_order = fsimg_load_le64(b + FSIMG_CATALOG_ORDER);
	return ((a_order > b_order) - (a_order < b_order));
}

/* The catalog sort key: the path's component tuple, then the rank, lowest
 * first, so records of one path are adjacent with the winning record
 * first. Equal paths have equal lengths, so records of equal path and rank
 * compare by their header bytes and differ only when the records differ:
 * the order never depends on insertion order or the sort's stability, run
 * size or merge fan-in.
 */
static int
entry_compare(const void *a, size_t a_length, const void *b, size_t b_length,
    void *argument)
{
	int order;

	order = path_compare(a, a_length, b, b_length, argument);
	if (order != 0 || a_length <= FSIMG_CATALOG_HEADER ||
	    b_length <= FSIMG_CATALOG_HEADER)
		return (order);
	order = rank_compare(a, b);
	if (order != 0)
		return (order);
	return (memcmp(a, b, FSIMG_CATALOG_HEADER));
}

static int
child_compare(const void *a, size_t a_length, const void *b, size_t b_length,
    void *argument FSIMG_UNUSED)
{
	const unsigned char *left = a, *right = b;
	uint64_t a_parent, b_parent;
	size_t common;
	int order;

	if (a_length <= 16 || b_length <= 16)
		return ((a_length > b_length) - (a_length < b_length));
	a_parent = fsimg_load_le64(left);
	b_parent = fsimg_load_le64(right);
	if (a_parent != b_parent)
		return ((a_parent > b_parent) - (a_parent < b_parent));
	common = a_length < b_length ? a_length - 16 : b_length - 16;
	order = memcmp(left + 16, right + 16, common);
	if (order != 0)
		return (order);
	return ((a_length > b_length) - (a_length < b_length));
}

/* Decide a record whose path equals the last kept record's path, given
 * both records' encoded headers. Two directories merge: the later record is
 * dropped, and its children, which keep their own paths, attach to the kept
 * directory, so the merged directory holds the union of the children. A
 * dropped record's metadata never reaches the image, so a record that
 * requires metadata cannot be dropped, and a record of equal rank must
 * encode exactly as the kept one: nothing would justify keeping either's
 * metadata. Anything else is a collision named by its path.
 */
static fsimg_status_t
resolve_repeated_path(fsimg_entry_kind_t kept_kind,
    const unsigned char *kept_header, const unsigned char *record_header,
    const fsimg_entry_t *entry, fsimg_error_t *error)
{
	fsimg_status_t status;
	bool outranked, identical;

	/* The sort places the kept record first, so a different rank is a
	 * higher one.
	 */
	outranked = rank_compare(kept_header, record_header) != 0;
	identical = bcmp(kept_header, record_header, FSIMG_CATALOG_HEADER) == 0;
	if (kept_kind == FSIMG_ENTRY_DIRECTORY &&
	    entry->kind == FSIMG_ENTRY_DIRECTORY &&
	    entry->metadata.required == 0 && (outranked || identical))
		return (FSIMG_OK);
	status = fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_CATALOG,
	    "image path is supplied by more than one source");
	fsimg_error_path(error, entry->path);
	return (status);
}

/* Pop the ancestor stack down to the directory that is path's parent and
 * report whether it was found. Every stack entry's path is a prefix of
 * kept_path, the last kept record's path, so an entry is path's parent
 * when its length is parent_length and those bytes of kept_path and path
 * agree.
 */
static bool
find_parent(const catalog_ancestor_t *ancestors, size_t *top,
    const char *kept_path, const char *path, size_t parent_length)
{
	while (*top != 0 && (ancestors[*top].path_length != parent_length ||
	    bcmp(kept_path, path, parent_length) != 0))
		(*top)--;
	return (ancestors[*top].path_length == parent_length &&
	    bcmp(kept_path, path, parent_length) == 0);
}

/* Assign stable IDs in sorted order and append one child record per entry.
 *
 * The sort places the records of one path next to each other, lowest rank
 * first. The first record of such a run is kept and resolve_repeated_path
 * decides each later one, so a run needs only the last kept record's path,
 * kind and encoded header. A dropped record gets no ID: IDs stay dense and
 * writer->count, the number of catalog entries, counts kept records only.
 *
 * Parents come from an ancestor stack holding each open directory's ID and
 * path length. The open directories are the last kept record (when it is a
 * directory) and its ancestors, so their paths are prefixes of kept_path.
 *
 * Planning's capability summaries, the unions and the longest link target,
 * are taken here, over kept records only, and each kept record is reported
 * to the entry callback here, so the calls equal writer->count.
 */
static fsimg_status_t
index_entries(fsimg_writer_t *writer, fsimg_stream_t *children,
    fsimg_error_t *error)
{
	catalog_ancestor_t *ancestors;
	fsimg_catalog_entry_t entry;
	fsimg_entry_kind_t kept_kind;
	fsimg_error_t detail;
	char *kept_path;
	unsigned char kept_header[FSIMG_CATALOG_HEADER];
	unsigned char index[FSIMG_CATALOG_INDEX_SIZE];
	uint64_t cursor, reference, capacity, stack_bytes, id, records;
	size_t length, path_length, parent_length, top, name_length, allocation_size;
	fsimg_status_t status;
	const char *name;

	capacity = writer->collected < writer->ctx->options.max_depth ?
	    writer->collected + 1 : (uint64_t)writer->ctx->options.max_depth + 1;
	/* One slot per possible ancestor plus the implicit root, never per child. */
	if (!fsimg_mul_u64(capacity, sizeof(*ancestors), &stack_bytes) ||
	    !fsimg_to_size(stack_bytes, &allocation_size))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_CATALOG, "catalog ancestor stack exceeds host range"));
	ancestors = fsimg_alloc(writer->ctx, allocation_size, &detail);
	if (ancestors == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	kept_path = fsimg_alloc(writer->ctx, writer->ctx->options.max_path_bytes,
	    &detail);
	if (kept_path == NULL) {
		fsimg_dealloc(ancestors);
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	/* The implicit root is the first open directory. */
	kept_path[0] = '/';
	kept_path[1] = '\0';
	ancestors[0].id = 0;
	ancestors[0].path_length = 1;
	fsimg_store_le64(index, UINT64_MAX);
	fsimg_store_le64(index + 8, 0);
	fsimg_store_le64(index + 16, 0);
	status = fsimg_array_set(writer->index, 0, index, error);
	if (status != FSIMG_OK)
		goto done;

	cursor = 0;
	top = 0;
	id = 0;
	records = 0;
	kept_kind = FSIMG_ENTRY_DIRECTORY;
	for (;;) {
		reference = cursor;
		status = fsimg_stream_next(writer->ordered, &cursor, writer->record,
		    writer->record_capacity, &length, error);
		if (status == FSIMG_DONE)
			break;
		if (status != FSIMG_OK)
			goto done;
		records++;
		if (records > writer->collected)
			break;
		status = decode_entry(writer, writer->record, length, &entry, error);
		if (status != FSIMG_OK)
			goto done;
		/* Before the first kept record kept_path holds the implicit root,
		 * which no record repeats.
		 */
		if (id != 0 && strcmp(kept_path, entry.entry.path) == 0) {
			status = resolve_repeated_path(kept_kind, kept_header,
			    writer->record, &entry.entry, error);
			if (status != FSIMG_OK)
				goto done;
			continue;
		}

		id++;
		name = strrchr(entry.entry.path, '/') + 1;
		parent_length = name - entry.entry.path - 1;
		/* A root child's last slash is at index zero; its parent is '/'. */
		if (parent_length == 0)
			parent_length = 1;
		if (!find_parent(ancestors, &top, kept_path, entry.entry.path,
		    parent_length)) {
			status = fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_CATALOG, "catalog entry has no explicit directory parent");
			fsimg_error_path(error, entry.entry.path);
			goto done;
		}
		fsimg_store_le64(index, reference);
		fsimg_store_le64(index + 8, ancestors[top].id);
		fsimg_store_le64(index + 16, 0);
		status = fsimg_array_set(writer->index, id, index, error);
		if (status != FSIMG_OK)
			goto done;
		if (entry.entry.kind == FSIMG_ENTRY_SYMLINK &&
		    entry.entry.size > writer->catalog_link_target_max)
			writer->catalog_link_target_max = entry.entry.size;
		writer->catalog_kinds |= (uint32_t)entry.entry.kind;
		writer->catalog_fields |= entry.entry.metadata.valid;
		writer->catalog_required |= entry.entry.metadata.required;
		/* The record is the catalog's entry for its path: report it,
		 * while the record buffer still holds its path. A repeated
		 * record after it adds nothing to report.
		 */
		fsimg_report_entry(writer->ctx, &entry.entry);

		/* Preserve the merge and traversal state before reusing the record
		 * buffer. A long component's collision key can overlap the
		 * borrowed pathname.
		 */
		path_length = strlen(entry.entry.path);
		bcopy(entry.entry.path, kept_path, path_length + 1);
		bcopy(writer->record, kept_header, sizeof(kept_header));
		kept_kind = entry.entry.kind;
		name_length = strlen(name) + 1;
		/* Copy the name before reusing the record buffer for its key. */
		bcopy(name, writer->record + 16, name_length);
		fsimg_store_le64(writer->record, ancestors[top].id);
		fsimg_store_le64(writer->record + 8, id);
		status = fsimg_stream_append(children, writer->record,
		    16 + name_length, NULL, error);
		if (status != FSIMG_OK)
			goto done;
		if (entry.entry.kind == FSIMG_ENTRY_DIRECTORY) {
			top++;
			ancestors[top].id = id;
			ancestors[top].path_length = path_length;
		}
	}
	/* The sorted stream holds exactly the collected records. */
	if (records != writer->collected) {
		status = fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_CATALOG, "catalog entry count mismatch");
		goto done;
	}
	writer->count = id;
	status = FSIMG_OK;
done:
	fsimg_dealloc(kept_path);
	fsimg_dealloc(ancestors);
	return (status);
}

static fsimg_status_t
set_child_count(fsimg_writer_t *writer, uint64_t parent, uint64_t count,
    fsimg_error_t *error)
{
	unsigned char index[FSIMG_CATALOG_INDEX_SIZE];
	fsimg_status_t status;

	status = fsimg_array_get(writer->index, parent, index, error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_store_le64(index + 16, count);
	return (fsimg_array_set(writer->index, parent, index, error));
}

fsimg_status_t
fsimg_catalog_decode_child(fsimg_writer_t *writer, size_t length,
    fsimg_catalog_child_t *child, fsimg_error_t *error)
{
	size_t units;
	fsimg_status_t status;

	bzero(child, sizeof(*child));
	if (length < 18 || length - 16 > writer->ctx->options.max_path_bytes)
		goto invalid;
	child->parent_id = fsimg_load_le64(writer->record);
	child->id = fsimg_load_le64(writer->record + 8);
	child->name = (const char *)writer->record + 16;
	child->name_length = strnlen(child->name, length - 16);
	if (child->id == 0 || child->id > writer->count ||
	    child->parent_id >= child->id || child->name_length != length - 17 ||
	    memchr(child->name, '/', child->name_length) != NULL ||
	    strcmp(child->name, ".") == 0 || strcmp(child->name, "..") == 0)
		goto invalid;
	status = fsimg_utf8_to_utf16(child->name, child->name_length,
	    NULL, 0, &units, error);
	if (status != FSIMG_OK)
		goto invalid;
	return (FSIMG_OK);
invalid:
	bzero(child, sizeof(*child));
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
	    FSIMG_OP_CATALOG, "invalid catalog child record"));
}

fsimg_status_t
fsimg_catalog_count_children(fsimg_writer_t *writer, fsimg_error_t *error)
{
	fsimg_catalog_child_t child;
	fsimg_error_t detail;
	char *previous;
	uint64_t cursor, current_parent, count, seen;
	size_t length;
	fsimg_status_t status;

	if (writer->state != FSIMG_WRITER_PREPARING || writer->catalog_ready)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_CATALOG, "child counts require unpublished catalog preparation"));
	previous = fsimg_alloc(writer->ctx, writer->ctx->options.max_path_bytes,
	    &detail);
	if (previous == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	cursor = 0;
	current_parent = 0;
	count = 0;
	seen = 0;
	for (;;) {
		status = fsimg_stream_next(writer->children, &cursor, writer->record,
		    writer->record_capacity, &length, error);
		if (status == FSIMG_DONE)
			break;
		if (status != FSIMG_OK)
			goto done;
		status = fsimg_catalog_decode_child(writer, length, &child, error);
		if (status != FSIMG_OK)
			goto done;
		if (child.parent_id < current_parent) {
			status = fsimg_error_set(error, FSIMG_ECORRUPT, 0,
			    FSIMG_OP_CATALOG, "catalog child parents are not ordered");
			goto done;
		}
		if (child.parent_id != current_parent) {
			status = set_child_count(writer, current_parent, count, error);
			if (status != FSIMG_OK)
				goto done;
			current_parent = child.parent_id;
			count = 0;
		}
		/* Indexing kept one record per path, so a parent's children have
		 * distinct names; an equal name is corruption like a misordered one.
		 */
		if (count != 0 && strcmp(previous, child.name) >= 0) {
			status = fsimg_error_set(error, FSIMG_ECORRUPT, 0,
			    FSIMG_OP_CATALOG,
			    "catalog child names are not strictly ordered");
			goto done;
		}
		bcopy(child.name, previous, child.name_length + 1);
		count++;
		seen++;
	}
	if (seen != writer->count) {
		status = fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_CATALOG, "catalog child stream has a different entry count");
		goto done;
	}
	status = set_child_count(writer, current_parent, count, error);
done:
	fsimg_dealloc(previous);
	return (status);
}

fsimg_status_t
fsimg_catalog_prepare(fsimg_writer_t *writer, const fsimg_sort_options_t *options,
    fsimg_error_t *error)
{
	fsimg_stream_t *children;
	fsimg_status_t status;

	status = fsimg_stream_sort(writer->entries, entry_compare, NULL, options,
	    &writer->ordered, error);
	if (status != FSIMG_OK)
		return (status);
	/* Merging can only shrink the catalog, so every kept ID fits. */
	status = fsimg_array_new(&writer->session, FSIMG_CATALOG_INDEX_SIZE,
	    writer->collected + 1, &writer->index, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_stream_new(&writer->session, writer->record_capacity,
	    &children, error);
	if (status != FSIMG_OK)
		return (status);
	status = index_entries(writer, children, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_seal(children, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_sort(children, child_compare, NULL, options,
		    &writer->children, error);
	fsimg_stream_free(children);
	if (status != FSIMG_OK)
		return (status);
	return (fsimg_catalog_count_children(writer, error));
}

void
fsimg_catalog_discard(fsimg_writer_t *writer)
{
	fsimg_array_free(writer->index);
	writer->index = NULL;
	fsimg_stream_free(writer->ordered);
	writer->ordered = NULL;
	fsimg_stream_free(writer->children);
	writer->children = NULL;
	writer->catalog_ready = false;
}

static fsimg_status_t
read_entry(fsimg_writer_t *writer, uint64_t id, unsigned char *record,
    fsimg_catalog_entry_t *out, fsimg_error_t *error)
{
	unsigned char index[FSIMG_CATALOG_INDEX_SIZE];
	uint64_t cursor;
	size_t length;
	fsimg_status_t status;

	if (!writer->catalog_ready)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_CATALOG, "catalog is not available"));
	if (id > writer->count)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_CATALOG, "catalog ID is outside its index"));
	status = fsimg_array_get(writer->index, id, index, error);
	if (status != FSIMG_OK)
		return (fsimg_writer_fail(writer, status));
	bzero(out, sizeof(*out));
	if (id != 0) {
		cursor = fsimg_load_le64(index);
		status = fsimg_stream_next(writer->ordered, &cursor, record,
		    writer->record_capacity, &length, error);
		if (status == FSIMG_DONE)
			status = fsimg_error_set(error, FSIMG_ECORRUPT, 0,
			    FSIMG_OP_CATALOG, "catalog index references the end of its stream");
		if (status != FSIMG_OK)
			return (fsimg_writer_fail(writer, status));
		status = decode_entry(writer, record, length, out, error);
		if (status != FSIMG_OK)
			return (fsimg_writer_fail(writer, status));
	} else {
		out->entry.path = "/";
		out->entry.kind = FSIMG_ENTRY_DIRECTORY;
	}
	out->id = id;
	out->parent_id = fsimg_load_le64(index + 8);
	out->children = fsimg_load_le64(index + 16);
	if ((id == 0 && (out->parent_id != 0 ||
	    fsimg_load_le64(index) != UINT64_MAX)) ||
	    (id != 0 && out->parent_id >= id) ||
	    out->children > writer->count ||
	    (out->entry.kind != FSIMG_ENTRY_DIRECTORY && out->children != 0)) {
		status = fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_CATALOG, "invalid catalog index links or child count");
		return (fsimg_writer_fail(writer, status));
	}
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_catalog_get(fsimg_writer_t *writer, uint64_t id,
    const fsimg_catalog_entry_t **out, fsimg_error_t *error)
{
	fsimg_status_t status;

	if (out != NULL)
		*out = NULL;
	status = fsimg_ctx_ready(writer == NULL ? NULL : writer->ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_CATALOG, "missing catalog snapshot output"));
	status = read_entry(writer, id, writer->record, &writer->current, error);
	if (status != FSIMG_OK)
		return (status);
	*out = &writer->current;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_catalog_read_source(fsimg_writer_t *writer, uint64_t id,
    fsimg_catalog_entry_t *out, fsimg_error_t *error)
{
	fsimg_status_t status;

	status = fsimg_ctx_ready(writer == NULL ? NULL : writer->ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (writer->state == FSIMG_WRITER_FAILED || out == NULL)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_CATALOG, "source catalog lookup is not available"));
	return (read_entry(writer, id, writer->source_record, out, error));
}
