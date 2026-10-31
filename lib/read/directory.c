/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "reader.h"
#include "unicode.h"

/* A record is its class byte, the component from RECORD_NAME on with its
 * NUL, and the driver payload. A delivered component is validated UTF-8;
 * an omitted one is the raw bytes the driver admitted, which only keys and
 * the rendering read. fsimg_directory_admit() writes records and
 * record_parse() reads them. A key record is the 8-byte offset of its
 * entry's record, followed by the key.
 */
#define RECORD_NAME 1u
#define KEY_RECORD_BYTES (8u + FSIMG_DIRECTORY_KEY_BYTES)

typedef enum name_class {
	NAME_DELIVERED = 0,
	NAME_OMITTED = 1
} name_class_t;

/* A record in walk->record, split into its fields. */
typedef struct record_view {
	name_class_t	 class;
	const char	*name;
	size_t		 name_length;
	const unsigned char *payload;
	size_t		 payload_length;
} record_view_t;

/* One retained directory: a run of records at the top of the shared stack
 * stream, the replay cursor inside it, and its path length in reader->path.
 * hidden is set when the directory or an ancestor is omitted, so none of
 * its entries is delivered.
 */
typedef struct walk_level {
	fsimg_stream_mark_t base;
	uint64_t	 cursor;
	size_t		 path_length;
	bool		 hidden;
} walk_level_t;

/* Every retained ancestor lives in one stack stream, so traversal holds one
 * scratch store, and at most one descriptor, however deep it goes. Only the
 * deepest level is replayed, and its records always end the stream: a child
 * is stacked above its parent's unread records and rewound when exhausted.
 *
 * record holds the current entry's record, record_length bytes long, and
 * path_length is the length of its path; a zero record_length means there is
 * no current entry. omitted[i] is the class of component i of reader->path,
 * the first after the root at index 0, for the current entry and its
 * ancestors: the rendering of a walker path reads it. An advance ends the
 * current entry first, so admission and collision reporting may then use
 * record as scratch. pending marks a returned directory whose descent the
 * next advance performs. finished is set once the root is exhausted, and
 * failed once an advance fails; either way the stack is released.
 */
struct fsimg_walk {
	fsimg_stream_t	*records;
	walk_level_t	*levels;
	bool		*omitted;
	uint32_t	 depth;
	size_t		 path_length;
	size_t		 record_length;
	bool		 pending;
	bool		 finished;
	bool		 failed;
	unsigned char	 record[FSIMG_DIRECTORY_RECORD_BYTES];
};

struct fsimg_directory_build {
	fsimg_reader_t	*reader;
	fsimg_walk_t	*walk;
	fsimg_stream_t	*keys;
	uint64_t	 base;		/* offset of the directory's first record */
	size_t		 path_length;
	uint32_t	 level;
};

static fsimg_status_t classify(const fsimg_reader_t *, const char *, size_t,
    name_class_t *, fsimg_error_t *);
static fsimg_status_t keys_valid(const fsimg_directory_keys_t *,
    fsimg_error_t *);
static fsimg_status_t corrupt(fsimg_error_t *, const char *);
static int key_compare(const void *, size_t, const void *, size_t, void *);
static fsimg_status_t record_parse(const fsimg_reader_t *, size_t,
    record_view_t *, fsimg_error_t *);
static fsimg_status_t record_path(fsimg_reader_t *, uint32_t, size_t,
    const record_view_t *, fsimg_error_t *);
static void name_walker_path(fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t ambiguity_error(fsimg_directory_build_t *, uint64_t,
    uint64_t, fsimg_error_t *);
static fsimg_status_t validate_namespace(fsimg_directory_build_t *,
    fsimg_error_t *);
static fsimg_status_t prepare_level(fsimg_reader_t *, uint32_t, size_t, bool,
    fsimg_error_t *);
static fsimg_status_t walk_start(fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t advance(fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t pass_omitted_root(fsimg_reader_t *, fsimg_error_t *);
static fsimg_status_t make_current(fsimg_reader_t *, walk_level_t *, size_t,
    fsimg_error_t *);

static fsimg_status_t
corrupt(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_READER,
	    message));
}

/* Class a component of length bytes, which the record's name buffer
 * bounds; format rules beyond that, such as a name length limit, belong to
 * the driver. Every component is nonempty, without a separator, and
 * neither "." nor "..". Under the QNX-family profile one outside the
 * profile is omitted; a format without a profile must name every child in
 * valid UTF-8 and delivers it.
 */
static fsimg_status_t
classify(const fsimg_reader_t *reader, const char *name, size_t length,
    name_class_t *class, fsimg_error_t *error)
{
	size_t units;

	if (length == 0 || memchr(name, '/', length) != NULL ||
	    (length == 1 && name[0] == '.') ||
	    (length == 2 && name[0] == '.' && name[1] == '.'))
		return (corrupt(error, "invalid directory entry name"));
	switch (reader->ops->name_profile) {
	case FSIMG_NAME_PROFILE_QNX:
		*class = fsimg_name_in_qnx_profile(name, length) ?
		    NAME_DELIVERED : NAME_OMITTED;
		return (FSIMG_OK);
	case FSIMG_NAME_PROFILE_NONE:
		break;
	}
	if (fsimg_utf8_to_utf16(name, length, NULL, 0, &units, NULL) !=
	    FSIMG_OK)
		return (corrupt(error, "invalid directory entry name"));
	*class = NAME_DELIVERED;
	return (FSIMG_OK);
}

/* Split the record of length bytes in walk->record into view: a class
 * byte, a name NUL-terminated within the name limit, and a payload of at
 * most the payload limit. The record came from scratch storage, so the
 * stored class must be the one the name is classed as again.
 */
static fsimg_status_t
record_parse(const fsimg_reader_t *reader, size_t length, record_view_t *view,
    fsimg_error_t *error)
{
	const unsigned char *record = reader->walk->record;
	size_t limit;
	name_class_t class;
	fsimg_status_t status;

	if (length <= RECORD_NAME || (record[0] != NAME_DELIVERED &&
	    record[0] != NAME_OMITTED))
		return (corrupt(error, "invalid prepared directory record"));
	view->name = (const char *)record + RECORD_NAME;
	limit = length - RECORD_NAME < FSIMG_DIRECTORY_NAME_BYTES ?
	    length - RECORD_NAME : FSIMG_DIRECTORY_NAME_BYTES;
	view->name_length = strnlen(view->name, limit);
	/* No NUL within the record or the name limit is a damaged frame. */
	if (view->name_length == limit)
		return (corrupt(error, "invalid prepared directory record"));
	view->payload = record + RECORD_NAME + view->name_length + 1;
	view->payload_length = length - RECORD_NAME - view->name_length - 1;
	if (view->payload_length > FSIMG_DIRECTORY_PAYLOAD_BYTES)
		return (corrupt(error, "invalid prepared directory record"));
	status = classify(reader, view->name, view->name_length, &class, error);
	if (status != FSIMG_OK || class != record[0])
		return (corrupt(error, "invalid prepared directory record"));
	view->class = class;
	return (FSIMG_OK);
}

/* Make reader->path the path of the parsed record listed at walker level
 * level, whose directory's path is the first prefix_length bytes: the
 * prefix, a separator and the record's name, its class recorded for the
 * rendering. A record from scratch storage is checked against the path
 * limit again before it is copied into the path buffer.
 */
static fsimg_status_t
record_path(fsimg_reader_t *reader, uint32_t level, size_t prefix_length,
    const record_view_t *view, fsimg_error_t *error)
{
	if (prefix_length + view->name_length + 2 >
	    reader->ctx->options.max_path_bytes)
		return (corrupt(error, "prepared directory record exceeds path limit"));
	reader->path[prefix_length] = '/';
	bcopy(view->name, reader->path + prefix_length + 1,
	    view->name_length + 1);
	reader->walk->omitted[level] = view->class == NAME_OMITTED;
	return (FSIMG_OK);
}

/* Check the driver's side of the keys contract before any key is used. */
static fsimg_status_t
keys_valid(const fsimg_directory_keys_t *keys, fsimg_error_t *error)
{
	if (keys->count == 0 || keys->count > FSIMG_DIRECTORY_KEYS)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_READER, "driver produced no or too many namespace keys"));
	for (unsigned i = 0; i < keys->count; i++) {
		if (keys->length[i] > FSIMG_DIRECTORY_KEY_BYTES)
			return (fsimg_error_set(error, FSIMG_ESTATE, 0,
			    FSIMG_OP_READER, "driver produced an oversized namespace key"));
	}
	return (FSIMG_OK);
}

uint32_t
fsimg_directory_level(const fsimg_directory_build_t *build)
{
	return (build->level);
}

fsimg_status_t
fsimg_directory_admit(fsimg_directory_build_t *build, const char *name,
    const void *payload, size_t payload_length, fsimg_error_t *error)
{
	fsimg_reader_t *reader = build->reader;
	fsimg_walk_t *walk = build->walk;
	fsimg_directory_keys_t keys;
	unsigned char key[KEY_RECORD_BYTES];
	uint64_t offset;
	size_t length;
	name_class_t class;
	fsimg_status_t status;

	length = strnlen(name, FSIMG_DIRECTORY_NAME_BYTES);
	if (length == FSIMG_DIRECTORY_NAME_BYTES ||
	    payload_length > FSIMG_DIRECTORY_PAYLOAD_BYTES)
		return (corrupt(error, "invalid directory entry name or payload"));
	status = classify(reader, name, length, &class, error);
	if (status != FSIMG_OK)
		return (status);
	/* A child's path is the directory path, a separator and the name. */
	if (build->path_length + length + 2 > reader->ctx->options.max_path_bytes)
		return (fsimg_limit_error(error, FSIMG_RESOURCE_PATH,
		    reader->ctx->options.max_path_bytes,
		    (uint64_t)build->path_length + length + 2));
	if (build->level >= reader->ctx->options.max_depth)
		return (fsimg_limit_error(error, FSIMG_RESOURCE_DEPTH,
		    reader->ctx->options.max_depth, (uint64_t)build->level + 1));
	status = fsimg_session_charge(&reader->session, 1, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	status = reader->ops->keys(reader, name, payload, payload_length, &keys,
	    error);
	if (status != FSIMG_OK)
		return (status);
	status = keys_valid(&keys, error);
	if (status != FSIMG_OK)
		return (status);
	/* No current entry exists during preparation, so the record buffer
	 * stages the new record.
	 */
	walk->record[0] = (unsigned char)class;
	bcopy(name, walk->record + RECORD_NAME, length + 1);
	if (payload_length != 0)
		bcopy(payload, walk->record + RECORD_NAME + length + 1,
		    payload_length);
	status = fsimg_stream_append(walk->records, walk->record,
	    RECORD_NAME + length + 1 + payload_length, &offset, error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_store_le64(key, offset);
	for (unsigned i = 0; i < keys.count; i++) {
		bcopy(keys.key[i], key + 8, keys.length[i]);
		status = fsimg_stream_append(build->keys, key, 8 + keys.length[i],
		    NULL, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

/* Order key records by key bytes, then by entry, so equal keys are adjacent
 * and a collision is found by comparing neighbours.
 */
static int
key_compare(const void *a, size_t alen, const void *b, size_t blen,
    void *argument FSIMG_UNUSED)
{
	const unsigned char *left = a, *right = b;
	size_t length;
	int comparison;

	if (alen < 8 || blen < 8)
		return ((alen > blen) - (alen < blen));
	length = alen < blen ? alen - 8 : blen - 8;
	comparison = memcmp(left + 8, right + 8, length);
	if (comparison != 0 || alen != blen)
		return (comparison != 0 ? comparison : (alen > blen) - (alen < blen));
	return ((fsimg_load_le64(left) > fsimg_load_le64(right)) -
	    (fsimg_load_le64(left) < fsimg_load_le64(right)));
}

/* Name the walker path in reader->path in the error: rendered with the
 * classes of its components, so an omitted name never leaves raw.
 */
static void
name_walker_path(fsimg_reader_t *reader, fsimg_error_t *error)
{
	if (error == NULL)
		return;
	error->path_truncated = fsimg_path_render(reader->path,
	    reader->walk->omitted, error->path, sizeof(error->path));
}

/* Report two entries of the directory sharing a key, first before second
 * in on-disk order, as the key sort orders them. Names travel only in path
 * fields: the error's path names the second entry, and the message gives
 * the first's position among the directory's admitted entries, counted
 * from 1, so no name or key byte enters the message. Preparation has no
 * current entry, so reader->path past the directory's own path and
 * walk->record are free to use. Both offsets came from scratch storage and
 * must name records of this directory's run.
 */
static fsimg_status_t
ambiguity_error(fsimg_directory_build_t *build, uint64_t first,
    uint64_t second, fsimg_error_t *error)
{
	fsimg_reader_t *reader = build->reader;
	fsimg_walk_t *walk = build->walk;
	char message[sizeof(((fsimg_error_t *)NULL)->message)];
	uint64_t cursor, offset, position;
	size_t length;
	record_view_t view;
	fsimg_status_t status;

	if (first < build->base || second <= first)
		return (corrupt(error, "namespace key names no entry"));
	/* The first entry's position: its record's ordinal in the run. */
	cursor = build->base;
	position = 0;
	do {
		offset = cursor;
		status = fsimg_stream_next(walk->records, &cursor, walk->record,
		    sizeof(walk->record), &length, error);
		if (status == FSIMG_DONE)
			return (corrupt(error, "namespace key names no entry"));
		if (status != FSIMG_OK)
			return (status);
		position++;
	} while (offset < first);
	if (offset != first)
		return (corrupt(error, "namespace key names no entry"));

	/* The second entry's path, its component classed like any other. */
	cursor = second;
	status = fsimg_stream_next(walk->records, &cursor, walk->record,
	    sizeof(walk->record), &length, error);
	if (status == FSIMG_DONE)
		status = corrupt(error, "namespace key names no entry");
	if (status == FSIMG_OK)
		status = record_parse(reader, length, &view, error);
	if (status == FSIMG_OK)
		status = record_path(reader, build->level, build->path_length,
		    &view, error);
	if (status != FSIMG_OK)
		return (status);

	(void)snprintf(message, sizeof(message),
	    "%s: collides with entry %" PRIu64 " of its directory",
	    reader->ops->ambiguous, position);
	status = corrupt(error, message);
	name_walker_path(reader, error);
	return (status);
}

/* Sort the keys externally and reject one key reaching two entries. One
 * entry can answer to the same key twice, as when a long name equals its
 * alias.
 */
static fsimg_status_t
validate_namespace(fsimg_directory_build_t *build, fsimg_error_t *error)
{
	fsimg_stream_t *sorted;
	unsigned char current[KEY_RECORD_BYTES], previous[KEY_RECORD_BYTES];
	uint64_t cursor;
	size_t length, previous_length;
	fsimg_status_t status;

	sorted = NULL;
	status = fsimg_stream_seal(build->keys, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_stream_sort(build->keys, key_compare, NULL, NULL, &sorted,
	    error);
	if (status != FSIMG_OK)
		return (status);
	cursor = 0;
	previous_length = 0;
	for (;;) {
		status = fsimg_stream_next(sorted, &cursor, current, sizeof(current),
		    &length, error);
		if (status == FSIMG_DONE) {
			status = FSIMG_OK;
			break;
		}
		if (status != FSIMG_OK)
			break;
		if (length < 8) {
			status = corrupt(error, "invalid namespace key record");
			break;
		}
		if (previous_length == length &&
		    bcmp(previous + 8, current + 8, length - 8) == 0 &&
		    fsimg_load_le64(previous) != fsimg_load_le64(current)) {
			status = ambiguity_error(build, fsimg_load_le64(previous),
			    fsimg_load_le64(current), error);
			break;
		}
		bcopy(current, previous, length);
		previous_length = length;
	}
	fsimg_stream_free(sorted);
	return (status);
}

/* Prepare the directory at level: the root, or the current entry's
 * directory, which is hidden when it or an ancestor is omitted. Its records
 * are stacked on the walker stream and its keys go to a stream of their
 * own, released once the namespace is validated. The level becomes visible
 * only after validation succeeds.
 */
static fsimg_status_t
prepare_level(fsimg_reader_t *reader, uint32_t level, size_t path_length,
    bool hidden, fsimg_error_t *error)
{
	fsimg_walk_t *walk = reader->walk;
	fsimg_directory_build_t build;
	fsimg_stream_mark_t base;
	fsimg_status_t status;

	base = fsimg_stream_mark(walk->records);
	build = (fsimg_directory_build_t){ .reader = reader, .walk = walk,
	    .base = base.offset, .path_length = path_length, .level = level };
	status = fsimg_stream_new(&reader->session, KEY_RECORD_BYTES, &build.keys,
	    error);
	if (status == FSIMG_OK)
		status = reader->ops->prepare(reader, &build, error);
	if (status == FSIMG_OK)
		status = validate_namespace(&build, error);
	fsimg_stream_free(build.keys);
	if (status != FSIMG_OK) {
		/* A partial directory leaves nothing on the stack. The caller also
		 * poisons the walker, so this rewind cannot fail in a way that
		 * matters: base is a mark of this unsealed stream.
		 */
		(void)fsimg_stream_rewind(walk->records, base, NULL);
		return (status);
	}
	walk->levels[level] = (walk_level_t){ .base = base,
	    .cursor = base.offset, .path_length = path_length,
	    .hidden = hidden };
	walk->depth = level;
	return (FSIMG_OK);
}

static fsimg_status_t
walk_start(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_walk_t *walk;
	fsimg_error_t detail;
	uint64_t bytes, levels;
	size_t allocation, classes;
	fsimg_status_t status;

	/* A path has at most one component per level of the depth limit. */
	levels = (uint64_t)reader->ctx->options.max_depth + 1;
	if (!fsimg_mul_u64(levels, sizeof(walk_level_t), &bytes) ||
	    !fsimg_to_size(bytes, &allocation) ||
	    !fsimg_mul_u64(levels, sizeof(*walk->omitted), &bytes) ||
	    !fsimg_to_size(bytes, &classes))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_READER, "directory stack size overflow"));
	walk = fsimg_alloc(reader->ctx, sizeof(*walk), &detail);
	if (walk == NULL)
		goto failed;
	walk->levels = fsimg_alloc(reader->ctx, allocation, &detail);
	if (walk->levels == NULL)
		goto failed;
	walk->omitted = fsimg_alloc(reader->ctx, classes, &detail);
	if (walk->omitted == NULL)
		goto failed;
	status = fsimg_stream_new(&reader->session, FSIMG_DIRECTORY_RECORD_BYTES,
	    &walk->records, error);
	if (status != FSIMG_OK)
		goto released;
	/* Only a complete walker is published; preparing the root needs it. */
	reader->walk = walk;
	return (prepare_level(reader, 0, 0, false, error));
failed:
	status = detail.status;
	if (error != NULL)
		*error = detail;
released:
	if (walk != NULL) {
		fsimg_dealloc(walk->levels);
		fsimg_dealloc(walk->omitted);
	}
	fsimg_dealloc(walk);
	return (status);
}

/* The current entry is an omitted subtree root. Within the omission scope
 * it is counted, in the operation's count and the context statistics, and
 * an extraction that cannot leave it out fails naming it.
 */
static fsimg_status_t
pass_omitted_root(fsimg_reader_t *reader, fsimg_error_t *error)
{
	if (reader->walk->depth < reader->omission.scope)
		return (FSIMG_OK);
	fsimg_stat_add(&reader->omission.count, 1);
	fsimg_stat_add(&reader->ctx->stats.omitted_entries, 1);
	if (!reader->omission.refuse)
		return (FSIMG_OK);
	fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0, FSIMG_OP_EXTRACT,
	    "safe extraction cannot deliver a name outside the format's name profile");
	name_walker_path(reader, error);
	return (FSIMG_EUNSUPPORTED);
}

/* Make the record just read from level the current entry. The record came
 * from scratch storage, so its framing, class, name and path length are
 * checked again before the driver re-validates the payload. An omitted
 * entry is replayed like any other; only its delivery differs. A directory
 * is descended on the next advance unless it is not delivered and the
 * operation, not full verification, delivers entries.
 */
static fsimg_status_t
make_current(fsimg_reader_t *reader, walk_level_t *level, size_t length,
    fsimg_error_t *error)
{
	fsimg_walk_t *walk = reader->walk;
	record_view_t view;
	bool delivered;
	fsimg_status_t status;

	status = record_parse(reader, length, &view, error);
	if (status == FSIMG_OK)
		status = record_path(reader, walk->depth, level->path_length,
		    &view, error);
	if (status != FSIMG_OK)
		return (status);
	bzero(&reader->entry, sizeof(reader->entry));
	reader->initialized = 0;
	status = reader->ops->replay(reader, view.name, view.payload,
	    view.payload_length, error);
	if (status != FSIMG_OK)
		return (status);
	/* No content exists past the logical length, whatever the driver set. */
	if (reader->initialized > reader->entry.size)
		reader->initialized = reader->entry.size;
	reader->entry.path = reader->path;
	walk->path_length = level->path_length + 1 + view.name_length;
	walk->record_length = length;
	delivered = !level->hidden && view.class == NAME_DELIVERED;
	walk->pending = reader->entry.kind == FSIMG_ENTRY_DIRECTORY &&
	    (delivered || reader->omission.descend);
	if (!level->hidden && view.class == NAME_OMITTED)
		return (pass_omitted_root(reader, error));
	return (FSIMG_OK);
}

/* Every failure of a published walker is terminal: a failed advance
 * releases the stack, so no later call can replay a partial directory or a
 * level left behind by the failure. A start that fails before publishing
 * leaves nothing behind and may be retried.
 */
fsimg_status_t
fsimg_walk_next(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_walk_t *walk = reader->walk;
	fsimg_status_t status;

	if (walk != NULL && walk->failed)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_READER, "directory traversal has failed"));
	status = advance(reader, error);
	walk = reader->walk;
	if (status != FSIMG_OK && status != FSIMG_DONE && walk != NULL) {
		fsimg_stream_free(walk->records);
		walk->records = NULL;
		walk->record_length = 0;
		walk->pending = false;
		walk->failed = true;
	}
	return (status);
}

/* Advance to the next entry. A walker exists only once complete, so a
 * failure before it is published leaves nothing to release.
 */
static fsimg_status_t
advance(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_walk_t *walk = reader->walk;
	walk_level_t *level;
	size_t length;
	fsimg_status_t status;

	if (walk == NULL) {
		status = walk_start(reader, error);
		if (status != FSIMG_OK)
			return (status);
		walk = reader->walk;
	} else if (walk->finished) {
		return (FSIMG_DONE);
	} else {
		/* The current entry ends when an advance begins. */
		walk->record_length = 0;
	}
	if (walk->pending) {
		/* Descent happens on the advance after returning the directory,
		 * so the caller can prune it first.
		 */
		walk->pending = false;
		if (walk->depth >= reader->ctx->options.max_depth)
			return (fsimg_limit_error(error, FSIMG_RESOURCE_DEPTH,
			    reader->ctx->options.max_depth, (uint64_t)walk->depth + 1));
		status = prepare_level(reader, walk->depth + 1, walk->path_length,
		    walk->levels[walk->depth].hidden || walk->omitted[walk->depth],
		    error);
		if (status != FSIMG_OK)
			return (status);
	}
	for (;;) {
		level = &walk->levels[walk->depth];
		status = fsimg_stream_next(walk->records, &level->cursor,
		    walk->record, sizeof(walk->record), &length, error);
		if (status == FSIMG_OK)
			return (make_current(reader, level, length, error));
		if (status != FSIMG_DONE)
			return (status);
		/* An exhausted directory releases its records to the stack. */
		status = fsimg_stream_rewind(walk->records, level->base, error);
		if (status != FSIMG_OK)
			return (status);
		if (walk->depth == 0) {
			fsimg_stream_free(walk->records);
			walk->records = NULL;
			walk->finished = true;
			return (FSIMG_DONE);
		}
		walk->depth--;
	}
}

/* A query component matches when its key equals one of the current entry's
 * namespace keys: exactly the keys that namespace validation proved unique.
 */
fsimg_status_t
fsimg_walk_match(fsimg_reader_t *reader, const char *component, bool *matches,
    fsimg_error_t *error)
{
	fsimg_walk_t *walk = reader->walk;
	fsimg_directory_keys_t keys;
	unsigned char query[FSIMG_DIRECTORY_KEY_BYTES];
	size_t query_length;
	record_view_t view;
	fsimg_status_t status;

	*matches = false;
	if (walk == NULL || walk->record_length == 0)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_READER, "name matching requires a current entry"));
	status = reader->ops->key(reader, component, query, &query_length, error);
	if (status != FSIMG_OK)
		return (status);
	if (query_length > sizeof(query))
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_READER, "driver produced an oversized query key"));
	status = record_parse(reader, walk->record_length, &view, error);
	if (status != FSIMG_OK)
		return (status);
	status = reader->ops->keys(reader, view.name, view.payload,
	    view.payload_length, &keys, error);
	/* EINVAL is reserved for an unrepresentable query, the one failure
	 * that leaves the reader usable; failing to key the entry is not one.
	 */
	if (status == FSIMG_EINVAL)
		status = fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_READER,
		    "driver could not derive the current entry's keys");
	if (status == FSIMG_OK)
		status = keys_valid(&keys, error);
	if (status != FSIMG_OK)
		return (status);
	for (unsigned i = 0; i < keys.count; i++) {
		if (keys.length[i] == query_length &&
		    bcmp(keys.key[i], query, query_length) == 0)
			*matches = true;
	}
	return (FSIMG_OK);
}

bool
fsimg_walk_delivered(const fsimg_reader_t *reader)
{
	const fsimg_walk_t *walk = reader->walk;

	return (walk != NULL && walk->record_length != 0 &&
	    !walk->levels[walk->depth].hidden && !walk->omitted[walk->depth]);
}

void
fsimg_walk_scope_nothing(fsimg_reader_t *reader)
{
	reader->omission.scope = UINT32_MAX;
}

/* The current entry's children are listed one level deeper. */
void
fsimg_walk_scope_below_current(fsimg_reader_t *reader)
{
	reader->omission.scope = reader->walk->depth + 1;
}

fsimg_status_t
fsimg_walk_skip(fsimg_reader_t *reader, fsimg_error_t *error)
{
	if (reader->walk == NULL || !reader->walk->pending)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_READER, "directory descent is not pending"));
	reader->walk->pending = false;
	return (FSIMG_OK);
}

void
fsimg_test_walk_cursor(const fsimg_reader_t *reader, fsimg_store_t **store,
    uint64_t *offset)
{
	const fsimg_walk_t *walk = reader->walk;

	*store = NULL;
	*offset = 0;
	if (walk == NULL || walk->records == NULL)
		return;
	*store = walk->records->store;
	*offset = walk->levels[walk->depth].cursor;
}

void
fsimg_walk_free(fsimg_reader_t *reader)
{
	fsimg_walk_t *walk = reader->walk;

	if (walk == NULL)
		return;
	fsimg_stream_free(walk->records);
	fsimg_dealloc(walk->levels);
	fsimg_dealloc(walk->omitted);
	fsimg_dealloc(walk);
	reader->walk = NULL;
}
