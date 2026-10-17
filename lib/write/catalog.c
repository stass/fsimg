/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "host.h"
#include "unicode.h"
#include "writer.h"

typedef struct catalog_ancestor {
	uint64_t	 id;
	size_t		 path_length;
} catalog_ancestor_t;

static int64_t decode_signed(uint64_t);
static void encode_time(unsigned char *, const fsimg_timestamp_t *);
static bool decode_time(const unsigned char *, fsimg_timestamp_t *);
static fsimg_status_t decode_entry(fsimg_writer_t *, const unsigned char *,
    size_t, fsimg_catalog_entry_t *, fsimg_error_t *);
static fsimg_status_t read_entry(fsimg_writer_t *, uint64_t, unsigned char *,
    fsimg_catalog_entry_t *, fsimg_error_t *);
static int path_compare(const void *, size_t, const void *, size_t, void *);
static int child_compare(const void *, size_t, const void *, size_t, void *);
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

static void
encode_time(unsigned char *bytes, const fsimg_timestamp_t *time)
{
	fsimg_meta_put_u64(bytes, (uint64_t)time->seconds);
	fsimg_meta_put_u64(bytes + 8, time->precision_ns);
	fsimg_meta_put_u64(bytes + 16, time->nanoseconds);
	fsimg_meta_put_u64(bytes + 24, time->basis);
	fsimg_meta_put_u64(bytes + 32, (uint64_t)(int64_t)time->offset_minutes);
}

static bool
decode_time(const unsigned char *bytes, fsimg_timestamp_t *time)
{
	uint64_t nanoseconds, basis;
	int64_t offset;

	nanoseconds = fsimg_meta_get_u64(bytes + 16);
	basis = fsimg_meta_get_u64(bytes + 24);
	offset = decode_signed(fsimg_meta_get_u64(bytes + 32));
	if (nanoseconds >= 1000000000 || basis < FSIMG_TIME_UTC ||
	    basis > FSIMG_TIME_WALL || offset <= -1440 || offset >= 1440)
		return (false);
	time->seconds = decode_signed(fsimg_meta_get_u64(bytes));
	time->precision_ns = fsimg_meta_get_u64(bytes + 8);
	time->nanoseconds = (uint32_t)nanoseconds;
	time->basis = (fsimg_time_basis_t)basis;
	time->offset_minutes = (int32_t)offset;
	return (true);
}

fsimg_status_t
fsimg_writer_collect(fsimg_writer_t *writer, const fsimg_entry_t *entry,
    fsimg_source_storage_t storage, uint64_t source_id, fsimg_error_t *error)
{
	const fsimg_metadata_t *metadata;
	uint64_t payload_bytes;
	size_t path_length;
	uint32_t depth;
	fsimg_status_t status;

	status = fsimg_path_validate(writer->ctx, entry->path, false,
	    &path_length, &depth, error);
	if (status != FSIMG_OK)
		return (status);
	if ((entry->kind != FSIMG_ENTRY_FILE && entry->kind != FSIMG_ENTRY_DIRECTORY) ||
	    (entry->kind == FSIMG_ENTRY_DIRECTORY && entry->size != 0))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_CATALOG, "invalid catalog entry kind or directory size"));
	metadata = &entry->metadata;
	status = fsimg_metadata_validate(metadata, error);
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
	fsimg_meta_put_u64(writer->record, entry->size);
	fsimg_meta_put_u64(writer->record + 8, storage);
	fsimg_meta_put_u64(writer->record + 16, source_id);
	fsimg_meta_put_u64(writer->record + 24, entry->kind);
	fsimg_meta_put_u64(writer->record + 32, metadata->valid);
	fsimg_meta_put_u64(writer->record + 40, metadata->required);
	if ((metadata->valid & FSIMG_META_ATTRIBUTES) != 0)
		fsimg_meta_put_u64(writer->record + 48, metadata->attributes);
	if ((metadata->valid & FSIMG_META_CREATED) != 0)
		encode_time(writer->record + 56, &metadata->created);
	if ((metadata->valid & FSIMG_META_MODIFIED) != 0)
		encode_time(writer->record + 96, &metadata->modified);
	if ((metadata->valid & FSIMG_META_ACCESSED) != 0)
		encode_time(writer->record + 136, &metadata->accessed);
	bcopy(entry->path, writer->record + FSIMG_CATALOG_HEADER, path_length + 1);
	status = fsimg_stream_append(writer->entries, writer->record,
	    FSIMG_CATALOG_HEADER + path_length + 1, NULL, error);
	if (status != FSIMG_OK)
		return (status);
	writer->count++;
	writer->payload_bytes = payload_bytes;
	return (FSIMG_OK);
}

static fsimg_status_t
decode_entry(fsimg_writer_t *writer, const unsigned char *bytes, size_t length,
    fsimg_catalog_entry_t *out, fsimg_error_t *error)
{
	uint64_t kind, storage, valid, required, attributes;
	size_t path_length;
	uint32_t depth;
	fsimg_status_t status;

	bzero(out, sizeof(*out));
	if (length <= FSIMG_CATALOG_HEADER ||
	    strnlen((const char *)bytes + FSIMG_CATALOG_HEADER,
	    length - FSIMG_CATALOG_HEADER) != length - FSIMG_CATALOG_HEADER - 1)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_CATALOG, "invalid catalog record path framing"));
	kind = fsimg_meta_get_u64(bytes + 24);
	storage = fsimg_meta_get_u64(bytes + 8);
	valid = fsimg_meta_get_u64(bytes + 32);
	required = fsimg_meta_get_u64(bytes + 40);
	attributes = fsimg_meta_get_u64(bytes + 48);
	if ((kind != FSIMG_ENTRY_FILE && kind != FSIMG_ENTRY_DIRECTORY) ||
	    storage > FSIMG_STORAGE_HOST || (valid & ~UINT64_C(15)) != 0 ||
	    (required & ~valid) != 0 || (attributes & ~UINT64_C(15)) != 0)
		goto invalid;
	out->entry.path = (const char *)bytes + FSIMG_CATALOG_HEADER;
	out->entry.size = fsimg_meta_get_u64(bytes);
	out->storage = (fsimg_source_storage_t)storage;
	out->source_id = fsimg_meta_get_u64(bytes + 16);
	out->entry.kind = (fsimg_entry_kind_t)kind;
	out->entry.metadata.valid = (uint32_t)valid;
	out->entry.metadata.required = (uint32_t)required;
	if ((valid & FSIMG_META_ATTRIBUTES) != 0)
		out->entry.metadata.attributes = (uint32_t)attributes;
	if ((valid & FSIMG_META_CREATED) != 0 &&
	    !decode_time(bytes + 56, &out->entry.metadata.created))
		goto invalid;
	if ((valid & FSIMG_META_MODIFIED) != 0 &&
	    !decode_time(bytes + 96, &out->entry.metadata.modified))
		goto invalid;
	if ((valid & FSIMG_META_ACCESSED) != 0 &&
	    !decode_time(bytes + 136, &out->entry.metadata.accessed))
		goto invalid;
	if ((kind == FSIMG_ENTRY_DIRECTORY && (storage != FSIMG_STORAGE_NONE ||
	    out->entry.size != 0 || out->source_id != 0)) ||
	    (kind == FSIMG_ENTRY_FILE && storage == FSIMG_STORAGE_NONE) ||
	    (storage == FSIMG_STORAGE_ZERO && out->source_id != 0) ||
	    (storage == FSIMG_STORAGE_PROVIDER && !writer->provider_set))
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
child_compare(const void *a, size_t a_length, const void *b, size_t b_length,
    void *argument FSIMG_UNUSED)
{
	const unsigned char *left = a, *right = b;
	uint64_t a_parent, b_parent;
	size_t common;
	int order;

	if (a_length <= 16 || b_length <= 16)
		return ((a_length > b_length) - (a_length < b_length));
	a_parent = fsimg_meta_get_u64(left);
	b_parent = fsimg_meta_get_u64(right);
	if (a_parent != b_parent)
		return ((a_parent > b_parent) - (a_parent < b_parent));
	common = a_length < b_length ? a_length - 16 : b_length - 16;
	order = memcmp(left + 16, right + 16, common);
	if (order != 0)
		return (order);
	return ((a_length > b_length) - (a_length < b_length));
}

static fsimg_status_t
index_entries(fsimg_writer_t *writer, fsimg_stream_t *children,
    fsimg_error_t *error)
{
	catalog_ancestor_t *ancestors;
	fsimg_catalog_entry_t entry;
	fsimg_error_t detail;
	char *directory_path;
	unsigned char index[FSIMG_CATALOG_INDEX_SIZE];
	uint64_t cursor, reference, capacity, stack_bytes;
	size_t length, directory_length, parent_length, top, name_length, allocation_size;
	fsimg_status_t status;
	const char *name;

	capacity = writer->count < writer->ctx->options.max_depth ?
	    writer->count + 1 : (uint64_t)writer->ctx->options.max_depth + 1;
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
	directory_path = fsimg_alloc(writer->ctx, writer->ctx->options.max_path_bytes,
	    &detail);
	if (directory_path == NULL) {
		fsimg_dealloc(ancestors);
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	directory_path[0] = '/';
	ancestors[0].path_length = 1;
	fsimg_meta_put_u64(index, UINT64_MAX);
	fsimg_meta_put_u64(index + 8, 0);
	fsimg_meta_put_u64(index + 16, 0);
	status = fsimg_array_set(writer->index, 0, index, error);
	if (status != FSIMG_OK)
		goto done;
	cursor = 0;
	top = 0;
	for (uint64_t id = 1; id <= writer->count; id++) {
		reference = cursor;
		status = fsimg_stream_next(writer->ordered, &cursor, writer->record,
		    writer->record_capacity, &length, error);
		if (status != FSIMG_OK)
			goto done;
		status = decode_entry(writer, writer->record, length, &entry, error);
		if (status != FSIMG_OK)
			goto done;
		name = strrchr(entry.entry.path, '/') + 1;
		parent_length = name - entry.entry.path - 1;
		/* A root child's last slash is at index zero; its parent is '/'. */
		if (parent_length == 0)
			parent_length = 1;
		while (top != 0 && (ancestors[top].path_length != parent_length ||
		    bcmp(directory_path, entry.entry.path, parent_length) != 0))
			top--;
		if (ancestors[top].path_length != parent_length ||
		    bcmp(directory_path, entry.entry.path, parent_length) != 0) {
			status = fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_CATALOG, "catalog entry has no explicit directory parent");
			goto done;
		}
		fsimg_meta_put_u64(index, reference);
		fsimg_meta_put_u64(index + 8, ancestors[top].id);
		fsimg_meta_put_u64(index + 16, 0);
		status = fsimg_array_set(writer->index, id, index, error);
		if (status != FSIMG_OK)
			goto done;
		/* Preserve directory traversal state before reusing the record buffer.
		 * A long component's collision key can overlap the borrowed pathname.
		 */
		if (entry.entry.kind == FSIMG_ENTRY_DIRECTORY) {
			directory_length = strlen(entry.entry.path);
			bcopy(entry.entry.path, directory_path, directory_length + 1);
		}
		name_length = strlen(name) + 1;
		/* Copy the name before reusing the record buffer for its key. */
		bcopy(name, writer->record + 16, name_length);
		fsimg_meta_put_u64(writer->record, ancestors[top].id);
		fsimg_meta_put_u64(writer->record + 8, id);
		status = fsimg_stream_append(children, writer->record,
		    16 + name_length, NULL, error);
		if (status != FSIMG_OK)
			goto done;
		if (entry.entry.kind == FSIMG_ENTRY_DIRECTORY) {
			top++;
			ancestors[top].id = id;
			ancestors[top].path_length = directory_length;
		}
	}
	status = fsimg_stream_next(writer->ordered, &cursor, writer->record,
	    writer->record_capacity, &length, error);
	if (status == FSIMG_DONE)
		status = FSIMG_OK;
	else if (status == FSIMG_OK)
		status = fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_CATALOG, "catalog entry count mismatch");
done:
	fsimg_dealloc(directory_path);
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
	fsimg_meta_put_u64(index + 16, count);
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
	child->parent_id = fsimg_meta_get_u64(writer->record);
	child->id = fsimg_meta_get_u64(writer->record + 8);
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
	int order;

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
		if (count != 0) {
			order = strcmp(previous, child.name);
			if (order >= 0) {
				status = fsimg_error_set(error,
				    order == 0 ? FSIMG_EINVAL : FSIMG_ECORRUPT, 0,
				    FSIMG_OP_CATALOG, order == 0 ?
				    "duplicate catalog path or conflicting entry kind" :
				    "catalog child names are not ordered");
				goto done;
			}
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

	status = fsimg_stream_sort(writer->entries, path_compare, NULL, options,
	    &writer->ordered, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_array_new(&writer->session, FSIMG_CATALOG_INDEX_SIZE,
	    writer->count + 1, &writer->index, error);
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
		cursor = fsimg_meta_get_u64(index);
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
	out->parent_id = fsimg_meta_get_u64(index + 8);
	out->children = fsimg_meta_get_u64(index + 16);
	if ((id == 0 && (out->parent_id != 0 ||
	    fsimg_meta_get_u64(index) != UINT64_MAX)) ||
	    (id != 0 && out->parent_id >= id) ||
	    out->children > writer->count ||
	    (out->entry.kind == FSIMG_ENTRY_FILE && out->children != 0)) {
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
