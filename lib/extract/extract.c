/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "extract.h"
#include "mtree-writer.h"
#include "selection.h"
#include "unicode.h"

#define EXTRACT_BUFFER_BYTES 65536u

typedef struct extract_directory {
	size_t		 path_length;
	struct timespec	 times[2];
	int		 fd;
	bool		 set_times;
} extract_directory_t;

typedef struct extract_state {
	fsimg_reader_t	*reader;
	fsimg_extract_options_t options;
	fsimg_selection_t *selection;
	fsimg_mtree_writer_t *manifest; /* NULL without a manifest_fd. */
	extract_directory_t *directories;
	unsigned char	*buffer;
	char		*directory_path;
	char		*entry_path;
	uint64_t	 completed_entries;
	uint64_t	 skipped_entries;
	uint32_t	 depth;
} extract_state_t;

static fsimg_status_t extract_options(fsimg_reader_t *,
    const fsimg_extract_options_t *, fsimg_extract_options_t *, fsimg_error_t *);
static fsimg_status_t metadata_loss(extract_state_t *, const fsimg_entry_t *,
    uint32_t, fsimg_error_t *);
static bool timestamp_instant(const fsimg_timestamp_t *, int32_t, int64_t *);
static fsimg_status_t prepare_times(extract_state_t *, const fsimg_entry_t *,
    struct timespec *, bool *, fsimg_error_t *);
static fsimg_status_t close_directory(extract_state_t *, fsimg_error_t *);
static fsimg_status_t select_parent(extract_state_t *, const fsimg_entry_t *,
    size_t, const char **, fsimg_error_t *);
static fsimg_status_t creation_failed(int, const char *, fsimg_error_t *);
static fsimg_status_t extract_directory(extract_state_t *, const fsimg_entry_t *,
    const char *, fsimg_error_t *);
static fsimg_status_t extract_file(extract_state_t *, const fsimg_entry_t *,
    const char *, unsigned char *, fsimg_error_t *);
static fsimg_status_t copy_content(extract_state_t *, fsimg_io_t *,
    const fsimg_entry_t *, fsimg_sha256_t *, fsimg_error_t *);
static fsimg_status_t hash_zeros(extract_state_t *, fsimg_sha256_t *,
    uint64_t, fsimg_error_t *);
static fsimg_status_t skip_unsupported(extract_state_t *, const fsimg_entry_t *,
    fsimg_error_t *);
static fsimg_status_t record_entry(extract_state_t *, const fsimg_entry_t *,
    const unsigned char *, const unsigned char *, size_t, fsimg_error_t *);
static fsimg_status_t record_skipped(extract_state_t *, const fsimg_entry_t *,
    fsimg_error_t *);
static fsimg_status_t extract_entries(extract_state_t *, fsimg_error_t *);
static void report_destination(fsimg_error_t *, const char *);

void
fsimg_extract_options_init(fsimg_extract_options_t *options)
{
	bzero(options, sizeof(*options));
	options->size = sizeof(*options);
	options->version = FSIMG_OPTIONS_VERSION;
	options->flags = FSIMG_EXTRACT_ALLOW_LOSS;
	options->manifest_fd = -1;
}

static fsimg_status_t
extract_options(fsimg_reader_t *reader, const fsimg_extract_options_t *input,
    fsimg_extract_options_t *options, fsimg_error_t *error)
{
	size_t length;
	uint32_t depth;
	fsimg_status_t status;

	status = fsimg_ctx_ready(reader == NULL ? NULL : reader->ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (reader->state != FSIMG_READER_NEW)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_EXTRACT, "extraction requires an untouched reader"));
	fsimg_extract_options_init(options);
	if (input != NULL) {
		if (input->size != sizeof(*input))
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_EXTRACT, "unsupported extraction options size"));
		*options = *input;
	}
	if (options->version != FSIMG_OPTIONS_VERSION ||
	    (options->flags & ~(FSIMG_EXTRACT_ALLOW_LOSS | FSIMG_EXTRACT_TIMES |
	    FSIMG_EXTRACT_WALL_OFFSET | FSIMG_EXTRACT_SKIP_UNSUPPORTED)) != 0 ||
	    options->wall_offset_minutes <= -1440 || options->wall_offset_minutes >= 1440 ||
	    ((options->flags & FSIMG_EXTRACT_WALL_OFFSET) == 0 &&
	    options->wall_offset_minutes != 0))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_EXTRACT, "invalid extraction flags or wall-time offset"));
	for (unsigned i = 0; i < 4; i++) {
		if (options->reserved[i] != 0)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_EXTRACT, "nonzero reserved extraction option"));
	}
	if (options->manifest_fd != -1) {
		if (options->manifest_fd < 0)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_EXTRACT, "invalid manifest descriptor"));
		status = fsimg_mtree_writer_check(options->manifest_fd, error);
		if (status != FSIMG_OK)
			return (status);
	}
	/* Every member is checked before the destination exists. */
	if (options->selections == NULL && options->selection_count != 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_EXTRACT, "missing extraction selections"));
	for (size_t i = 0; i < options->selection_count; i++) {
		if (options->selections[i] == NULL)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_EXTRACT, "missing extraction selection"));
		status = fsimg_path_validate(reader->ctx, options->selections[i],
		    true, &length, &depth, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}


static fsimg_status_t
metadata_loss(extract_state_t *state, const fsimg_entry_t *entry,
    uint32_t fields, fsimg_error_t *error)
{
	if (fields == 0)
		return (FSIMG_OK);
	if ((entry->metadata.required & fields) != 0 ||
	    (state->options.flags & FSIMG_EXTRACT_ALLOW_LOSS) == 0)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_EXTRACT, "safe extraction cannot preserve requested metadata"));
	fsimg_report_loss(state->reader->ctx, entry->path, fields);
	return (FSIMG_OK);
}

static fsimg_status_t
prepare_times(extract_state_t *state, const fsimg_entry_t *entry,
    struct timespec *times, bool *set_times, fsimg_error_t *error)
{
	const fsimg_timestamp_t *timestamp;
	int64_t seconds;
	uint32_t field, lost;
	bool usable;

	*set_times = false;
	lost = entry->metadata.valid;
	for (unsigned i = 0; i < 2; i++) {
		times[i].tv_sec = 0;
		times[i].tv_nsec = UTIME_OMIT;
		field = i == 0 ? FSIMG_META_ACCESSED : FSIMG_META_MODIFIED;
		if ((state->options.flags & FSIMG_EXTRACT_TIMES) == 0 ||
		    (entry->metadata.valid & field) == 0)
			continue;
		timestamp = i == 0 ? &entry->metadata.accessed : &entry->metadata.modified;
		/* A wall time needs the caller's offset to name an instant. */
		if (timestamp->basis == FSIMG_TIME_WALL &&
		    (state->options.flags & FSIMG_EXTRACT_WALL_OFFSET) == 0)
			continue;
		usable = timestamp_instant(timestamp,
		    state->options.wall_offset_minutes, &seconds);
		if (!usable || (int64_t)(time_t)seconds != seconds)
			continue;
		times[i].tv_sec = (time_t)seconds;
		times[i].tv_nsec = timestamp->nanoseconds;
		lost &= ~field;
		*set_times = true;
	}
	return (metadata_loss(state, entry, lost, error));
}

/* The UTC instant of a timestamp, reading a wall time as
 * wall_offset_minutes east of UTC; UTC and OFFSET seconds are instants
 * already. False when the adjusted seconds leave int64_t. The host times
 * the extractor sets and the times its manifest records both come from
 * here. The manifest reads a wall time at the offset even when
 * FSIMG_EXTRACT_WALL_OFFSET is absent and the offset is therefore zero,
 * where the host gets no time; a time the manifest cannot express is a
 * loss prepare_times already counted.
 */
static bool
timestamp_instant(const fsimg_timestamp_t *timestamp,
    int32_t wall_offset_minutes, int64_t *seconds)
{
	int64_t offset;

	*seconds = timestamp->seconds;
	if (timestamp->basis != FSIMG_TIME_WALL)
		return (true);
	offset = (int64_t)wall_offset_minutes * 60;
	if ((offset > 0 && *seconds < INT64_MIN + offset) ||
	    (offset < 0 && *seconds > INT64_MAX + offset))
		return (false);
	*seconds -= offset;
	return (true);
}

/* Each selected directory is opened once, owns one stack frame, and is
 * finalized after all selected descendants. Its exact path prefix identifies
 * the frame; host name equivalence never substitutes another directory.
 */
static fsimg_status_t
close_directory(extract_state_t *state, fsimg_error_t *error)
{
	extract_directory_t *directory;
	fsimg_ctx_t *ctx;
	fsimg_status_t status;
	int result, saved;

	directory = &state->directories[state->depth];
	ctx = state->reader->ctx;
	status = FSIMG_OK;
	if (directory->set_times && futimens(directory->fd, directory->times) == -1)
		status = fsimg_error_set(error, FSIMG_EIO, errno,
		    FSIMG_OP_EXTRACT, "output directory timestamp restoration failed");
	if (status == FSIMG_OK && fsync(directory->fd) == -1)
		status = fsimg_error_set(error, FSIMG_EIO, errno,
		    FSIMG_OP_EXTRACT, "output directory flush failed");
	result = close(directory->fd);
	saved = errno;
	fsimg_fd_release(ctx, -1);
	state->depth--;
	state->directory_path[state->directories[state->depth].path_length] = '\0';
	if (status == FSIMG_OK && result == -1)
		status = fsimg_error_set(error, FSIMG_EIO, saved,
		    FSIMG_OP_EXTRACT, "output directory close failed");
	return (status);
}

static fsimg_status_t
select_parent(extract_state_t *state, const fsimg_entry_t *entry,
    size_t length, const char **name, fsimg_error_t *error)
{
	char *separator;
	size_t parent_length;
	fsimg_status_t status;

	bcopy(entry->path, state->entry_path, length + 1);
	separator = strrchr(state->entry_path, '/');
	parent_length = (size_t)(separator - state->entry_path);
	*name = separator + 1;
	while (state->depth != 0 &&
	    (state->directories[state->depth].path_length != parent_length ||
	    strncmp(state->directory_path, entry->path, parent_length) != 0)) {
		status = close_directory(state, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (state->directories[state->depth].path_length != parent_length ||
	    strncmp(state->directory_path, entry->path, parent_length) != 0)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_EXTRACT, "reader traversal does not supply the selected parent"));
	return (FSIMG_OK);
}

/* Classify a failed exclusive creation of an entry. A name the host
 * filesystem cannot represent, such as one longer than its component
 * limit, is an unsupported entry rather than an I/O failure.
 */
static fsimg_status_t
creation_failed(int saved, const char *message, fsimg_error_t *error)
{
	fsimg_status_t status;

	switch (saved) {
	case EEXIST:
		status = FSIMG_EEXISTS;
		break;
	case ENAMETOOLONG:
	case EILSEQ:
		status = FSIMG_EUNSUPPORTED;
		break;
	default:
		status = FSIMG_EIO;
		break;
	}
	return (fsimg_error_set(error, status, saved, FSIMG_OP_EXTRACT, message));
}

static fsimg_status_t
extract_directory(extract_state_t *state, const fsimg_entry_t *entry,
    const char *name, fsimg_error_t *error)
{
	extract_directory_t *directory;
	fsimg_ctx_t *ctx;
	struct stat st;
	fsimg_status_t status;
	int parent, fd, saved;

	ctx = state->reader->ctx;
	if (state->depth >= ctx->options.max_depth)
		return (fsimg_limit_error(error, FSIMG_RESOURCE_DEPTH,
		    ctx->options.max_depth, (uint64_t)state->depth + 1));
	directory = &state->directories[state->depth + 1];
	status = prepare_times(state, entry, directory->times, &directory->set_times, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_session_charge(&state->reader->session, 0, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_fd_reserve(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	parent = state->directories[state->depth].fd;
	if (mkdirat(parent, name, 0700) == -1) {
		saved = errno;
		fsimg_fd_release(ctx, -1);
		return (creation_failed(saved,
		    "exclusive output directory creation failed", error));
	}
	fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd == -1) {
		saved = errno;
		fsimg_fd_release(ctx, -1);
		return (fsimg_error_set(error, FSIMG_EIO, saved,
		    FSIMG_OP_EXTRACT, "output directory open failed"));
	}
	if (fsimg_host_fstat(fd, &st) == -1 || !S_ISDIR(st.st_mode)) {
		saved = errno;
		fsimg_fd_release(ctx, fd);
		return (fsimg_error_set(error, FSIMG_EIO, saved,
		    FSIMG_OP_EXTRACT, "opened output directory has unexpected type"));
	}
	directory->fd = fd;
	directory->path_length = strlen(entry->path);
	bcopy(entry->path, state->directory_path, directory->path_length + 1);
	state->depth++;
	return (FSIMG_OK);
}

/* Copy a file's bytes into output, which is already sized: a ZERO range
 * is left as the hole the resize made. With digest, every logical byte,
 * zero ranges included, is hashed in order as it is copied, so the digest
 * describes exactly the file written.
 */
static fsimg_status_t
copy_content(extract_state_t *state, fsimg_io_t *output,
    const fsimg_entry_t *entry, fsimg_sha256_t *digest, fsimg_error_t *error)
{
	fsimg_range_t range;
	uint64_t offset, remaining;
	size_t batch, read_bytes, written;
	fsimg_status_t status;

	offset = 0;
	while (offset < entry->size) {
		status = fsimg_reader_next_range(state->reader, offset, &range, error);
		if (status != FSIMG_OK)
			return (status);
		if (range.kind == FSIMG_RANGE_ZERO) {
			if (range.length > entry->size - offset)
				range.length = entry->size - offset;
			if (digest != NULL) {
				status = hash_zeros(state, digest, range.length,
				    error);
				if (status != FSIMG_OK)
					return (status);
			}
			offset += range.length;
			continue;
		}
		remaining = range.length;
		while (remaining != 0) {
			batch = remaining > EXTRACT_BUFFER_BYTES ? EXTRACT_BUFFER_BYTES :
			    (size_t)remaining;
			status = fsimg_reader_read_at(state->reader, offset, state->buffer,
			    batch, &read_bytes, error);
			if (status != FSIMG_OK)
				return (status);
			if (read_bytes != batch)
				return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
				    FSIMG_OP_EXTRACT, "file content ended before its declared length"));
			status = fsimg_io_write_session(output, &state->reader->session,
			    offset, state->buffer, batch, &written, error);
			if (status != FSIMG_OK)
				return (status);
			if (digest != NULL)
				fsimg_sha256_update(digest, state->buffer, batch);
			offset += batch;
			remaining -= batch;
		}
	}
	return (FSIMG_OK);
}

/* Hash length zero bytes, one buffer at a time. Nothing is read or
 * written, so each buffer is charged one unit of work, which also polls
 * cancellation: a large hole costs time in proportion to its length.
 */
static fsimg_status_t
hash_zeros(extract_state_t *state, fsimg_sha256_t *digest, uint64_t length,
    fsimg_error_t *error)
{
	size_t batch;
	fsimg_status_t status;

	bzero(state->buffer, EXTRACT_BUFFER_BYTES);
	while (length != 0) {
		status = fsimg_session_charge(&state->reader->session, 0, 0, 1,
		    error);
		if (status != FSIMG_OK)
			return (status);
		batch = length > EXTRACT_BUFFER_BYTES ? EXTRACT_BUFFER_BYTES :
		    (size_t)length;
		fsimg_sha256_update(digest, state->buffer, batch);
		length -= batch;
	}
	return (FSIMG_OK);
}

/* Create and fill one file. A non-NULL digest receives the SHA-256 of the
 * bytes written; NULL hashes nothing.
 */
static fsimg_status_t
extract_file(extract_state_t *state, const fsimg_entry_t *entry,
    const char *name, unsigned char *digest, fsimg_error_t *error)
{
	fsimg_io_t *output;
	fsimg_sha256_t hash;
	struct timespec times[2];
	fsimg_ctx_t *ctx;
	fsimg_status_t status;
	int fd, saved;
	bool set_times;

	ctx = state->reader->ctx;
	status = prepare_times(state, entry, times, &set_times, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_session_charge(&state->reader->session, 0, entry->size, 1, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_fd_reserve(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	fd = openat(state->directories[state->depth].fd, name,
	    O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd == -1) {
		saved = errno;
		fsimg_fd_release(ctx, -1);
		return (creation_failed(saved,
		    "exclusive output file creation failed", error));
	}
	status = fsimg_io_adopt_empty_output(ctx, fd, &output, error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_sha256_init(&hash);
	status = fsimg_io_resize(output, entry->size, error);
	if (status == FSIMG_OK)
		status = copy_content(state, output, entry,
		    digest != NULL ? &hash : NULL, error);
	/* Content writes change mtime, so restore times only after the final byte. */
	if (status == FSIMG_OK && set_times)
		status = fsimg_io_set_times(output, times, error);
	if (status == FSIMG_OK)
		status = fsimg_io_flush(output, false, error);
	if (status != FSIMG_OK) {
		fsimg_io_free(output);
		return (status);
	}
	if (digest != NULL)
		fsimg_sha256_final(digest, &hash);
	return (fsimg_io_close_output(output, error));
}

/* The safe profile creates only regular files and directories. Any other
 * kind, such as a symbolic link or FIFO, fails the extraction naming the
 * entry, unless the caller asked to skip such entries; a skip is counted in
 * the context statistics, so the omission is never silent. Such kinds have
 * no children, so skipping one never skips a subtree.
 */
static fsimg_status_t
skip_unsupported(extract_state_t *state, const fsimg_entry_t *entry,
    fsimg_error_t *error)
{
	if ((state->options.flags & FSIMG_EXTRACT_SKIP_UNSUPPORTED) == 0) {
		fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0, FSIMG_OP_EXTRACT,
		    "safe extraction creates only regular files and directories");
		fsimg_error_path(error, entry->path);
		return (FSIMG_EUNSUPPORTED);
	}
	state->skipped_entries++;
	fsimg_stat_add(&state->reader->ctx->stats.skipped_entries, 1);
	return (FSIMG_OK);
}

/* Write an entry's manifest line, when there is a manifest. digest is a
 * created file's, link a symbolic link's target, each NULL otherwise.
 */
static fsimg_status_t
record_entry(extract_state_t *state, const fsimg_entry_t *entry,
    const unsigned char *digest, const unsigned char *link, size_t link_length,
    fsimg_error_t *error)
{
	fsimg_mtree_record_t record;

	if (state->manifest == NULL)
		return (FSIMG_OK);
	bzero(&record, sizeof(record));
	record.entry = entry;
	record.digest = digest;
	record.link = link;
	record.link_length = link_length;
	if ((entry->metadata.valid & FSIMG_META_MODIFIED) != 0) {
		record.modified_valid = timestamp_instant(&entry->metadata.modified,
		    state->options.wall_offset_minutes, &record.modified_seconds);
		record.modified_nanoseconds = entry->metadata.modified.nanoseconds;
	}
	return (fsimg_mtree_writer_add(state->manifest, &record, error));
}

/* A skipped entry is recorded with everything but a digest, since the
 * manifest is the only record of it. A symbolic link's line carries its
 * target, read from the image into the copy buffer, which the line
 * borrows until it is written. The manifest cannot hold a target that is
 * empty, longer than FSIMG_SYMLINK_TARGET_MAX bytes or holds a NUL byte,
 * so such a target is corrupt.
 */
static fsimg_status_t
record_skipped(extract_state_t *state, const fsimg_entry_t *entry,
    fsimg_error_t *error)
{
	size_t length;
	fsimg_status_t status;

	if (state->manifest == NULL)
		return (FSIMG_OK);
	if (entry->kind != FSIMG_ENTRY_SYMLINK)
		return (record_entry(state, entry, NULL, NULL, 0, error));
	if (entry->size == 0 || entry->size > FSIMG_SYMLINK_TARGET_MAX)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_EXTRACT,
		    "symbolic link target length cannot be recorded"));
	status = fsimg_reader_read_at(state->reader, 0, state->buffer,
	    (size_t)entry->size, &length, error);
	if (status != FSIMG_OK)
		return (status);
	if (length != entry->size)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_EXTRACT,
		    "symbolic link target ended before its declared length"));
	if (memchr(state->buffer, '\0', length) != NULL)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_EXTRACT,
		    "symbolic link target holds a NUL byte"));
	return (record_entry(state, entry, NULL, state->buffer, length, error));
}

static fsimg_status_t
extract_entries(extract_state_t *state, fsimg_error_t *error)
{
	unsigned char digest[FSIMG_SHA256_DIGEST_LENGTH];
	const fsimg_entry_t *entry;
	unsigned char *file_digest;
	const char *name;
	fsimg_selection_relation_t relation;
	size_t path_length;
	uint32_t depth;
	fsimg_status_t status;

	for (;;) {
		status = fsimg_selection_next(state->selection, &entry, &relation, error);
		if (status == FSIMG_DONE)
			break;
		if (status != FSIMG_OK)
			return (status);
		/* Every path is validated at the extraction boundary, before
		 * anything is created, skipped or recorded for it.
		 */
		status = fsimg_path_validate(state->reader->ctx, entry->path, false,
		    &path_length, &depth, error);
		if (status != FSIMG_OK)
			return (status);
		if (entry->kind != FSIMG_ENTRY_FILE && entry->kind != FSIMG_ENTRY_DIRECTORY) {
			status = skip_unsupported(state, entry, error);
			if (status == FSIMG_OK)
				status = record_skipped(state, entry, error);
			if (status != FSIMG_OK)
				return (status);
			continue;
		}
		status = select_parent(state, entry, path_length, &name, error);
		if (status != FSIMG_OK)
			return (status);
		/* A file is hashed exactly when there is a manifest to record
		 * its digest; record_entry writes nothing without one.
		 */
		file_digest = NULL;
		if (entry->kind == FSIMG_ENTRY_DIRECTORY) {
			status = extract_directory(state, entry, name, error);
		} else {
			file_digest = state->manifest != NULL ? digest : NULL;
			status = extract_file(state, entry, name, file_digest,
			    error);
		}
		if (status == FSIMG_OK)
			status = record_entry(state, entry, file_digest, NULL, 0,
			    error);
		if (status != FSIMG_OK)
			return (status);
		state->completed_entries++;
	}
	while (state->depth != 0) {
		status = close_directory(state, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_extract_into(fsimg_reader_t *reader, int root,
    const fsimg_extract_options_t *options, fsimg_extract_result_t *result,
    fsimg_error_t *error)
{
	extract_state_t state;
	fsimg_error_t detail;
	uint64_t stack_bytes, limit;
	fsimg_status_t status;

	bzero(&state, sizeof(state));
	if (result != NULL)
		bzero(result, sizeof(*result));
	status = extract_options(reader, options, &state.options, error);
	if (status != FSIMG_OK)
		return (status);
	state.reader = reader;
	status = fsimg_selection_new(reader, state.options.selections,
	    state.options.selection_count, FSIMG_SELECTION_TREE,
	    &state.selection, error);
	if (status != FSIMG_OK)
		goto done;
	/* The walker fails at an omitted entry in scope unless asked to skip:
	 * a partial output is never success.
	 */
	reader->omission.refuse =
	    (state.options.flags & FSIMG_EXTRACT_SKIP_UNSUPPORTED) == 0;
	limit = state.options.max_output_bytes;
	if (limit != 0) {
		if (reader->ctx->options.max_output_bytes != 0 &&
		    limit > reader->ctx->options.max_output_bytes)
			limit = reader->ctx->options.max_output_bytes;
		reader->session.output_limit = limit;
	}
	reader->session.progress.phase = "safe extraction";
	stack_bytes = ((uint64_t)reader->ctx->options.max_depth + 1) *
	    sizeof(*state.directories);
	state.directories = fsimg_alloc(reader->ctx, (size_t)stack_bytes, &detail);
	if (state.directories == NULL) {
		status = detail.status;
		if (error != NULL)
			*error = detail;
		goto done;
	}
	state.directories[0].fd = root;
	/* The implicit root has no component and uses an empty internal prefix.
	 * A top-level entry's parent is therefore path_length zero, not '/'.
	 */
	state.buffer = fsimg_alloc(reader->ctx, EXTRACT_BUFFER_BYTES, &detail);
	if (state.buffer == NULL) {
		status = detail.status;
		if (error != NULL)
			*error = detail;
		goto done;
	}
	state.directory_path = fsimg_alloc(reader->ctx, reader->ctx->options.max_path_bytes,
	    &detail);
	if (state.directory_path == NULL) {
		status = detail.status;
		if (error != NULL)
			*error = detail;
		goto done;
	}
	state.entry_path = fsimg_alloc(reader->ctx, reader->ctx->options.max_path_bytes,
	    &detail);
	if (state.entry_path == NULL) {
		status = detail.status;
		if (error != NULL)
			*error = detail;
		goto done;
	}
	if (state.options.manifest_fd != -1) {
		status = fsimg_mtree_writer_open(reader->ctx, &reader->session,
		    state.options.manifest_fd, &state.manifest, error);
		if (status != FSIMG_OK)
			goto done;
	}
	status = extract_entries(&state, error);
	/* The manifest is complete only once every entry is extracted. */
	if (status == FSIMG_OK && state.manifest != NULL) {
		status = fsimg_mtree_writer_finish(state.manifest, error);
		state.manifest = NULL;
	}
done:
	if (result != NULL) {
		result->entries = state.completed_entries;
		result->skipped_entries = state.skipped_entries;
		result->omitted_entries = reader->omission.count;
		result->logical_bytes = reader->session.output_bytes;
	}
	while (state.depth != 0) {
		fsimg_fd_release(reader->ctx, state.directories[state.depth].fd);
		state.depth--;
	}
	fsimg_mtree_writer_free(state.manifest);
	fsimg_dealloc(state.buffer);
	fsimg_dealloc(state.directory_path);
	fsimg_dealloc(state.entry_path);
	fsimg_dealloc(state.directories);
	fsimg_selection_free(state.selection);
	if (status != FSIMG_OK)
		return (fsimg_reader_abort(reader, status));
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_extract_at(fsimg_reader_t *reader, int parent, const char *name,
    const fsimg_extract_options_t *options, fsimg_error_t *error)
{
	fsimg_extract_options_t selected;
	fsimg_ctx_t *ctx;
	fsimg_status_t status;
	int root, result, saved;
	bool created;

	status = extract_options(reader, options, &selected, error);
	if (status != FSIMG_OK)
		return (status);
	ctx = reader->ctx;
	if (name == NULL || name[0] == '\0' || strcmp(name, ".") == 0 ||
	    strcmp(name, "..") == 0 || strchr(name, '/') != NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_EXTRACT, "destination requires one basename"));
	status = fsimg_private_mkdir(ctx, parent, name, FSIMG_OP_EXTRACT, &root,
	    &created, error);
	if (status != FSIMG_OK) {
		/* Extraction has not begun, so the reader stays untouched. */
		if (created)
			report_destination(error, name);
		return (status);
	}
	status = fsimg_extract_into(reader, root, &selected, NULL, error);
	if (status == FSIMG_OK && fsync(root) == -1)
		status = fsimg_error_set(error, FSIMG_EIO, errno,
		    FSIMG_OP_EXTRACT, "extraction root flush failed");
	result = close(root);
	saved = errno;
	fsimg_fd_release(ctx, -1);
	if (status == FSIMG_OK && result == -1)
		status = fsimg_error_set(error, FSIMG_EIO, saved,
		    FSIMG_OP_EXTRACT, "extraction root close failed");
	if (status == FSIMG_OK)
		return (FSIMG_OK);
	fsimg_reader_abort(reader, status);
	report_destination(error, name);
	return (status);
}

/* Mark a failed extraction's output incomplete. An error that already
 * names an image entry keeps it; any other names the destination.
 */
static void
report_destination(fsimg_error_t *error, const char *name)
{
	if (error != NULL) {
		error->output_state = FSIMG_OUTPUT_INCOMPLETE;
		if (error->path[0] == '\0')
			fsimg_error_path(error, name);
	}
}
