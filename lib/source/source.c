/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "fsimg-endian.h"
#include "source.h"
#include "unicode.h"

static fsimg_status_t source_validate(fsimg_source_cursor_t *, fsimg_error_t *);
static bool is_output_root(const fsimg_writer_t *, const fsimg_host_file_t *);
static bool is_scratch(const fsimg_ctx_t *, const fsimg_host_file_t *);
static fsimg_status_t source_abort(fsimg_source_cursor_t *, fsimg_status_t);
static fsimg_status_t callback_status(fsimg_status_t, fsimg_error_t *);
static fsimg_status_t link_failure(int, bool, fsimg_error_t *);
static fsimg_status_t provider_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t provider_read(void *, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t link_read(void *, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);
static fsimg_status_t serve_link(fsimg_writer_t *, fsimg_source_cursor_t *,
    fsimg_error_t *);
static fsimg_status_t open_provider_link(fsimg_writer_t *,
    fsimg_source_cursor_t *, fsimg_error_t *);
static fsimg_status_t open_host_link(fsimg_writer_t *,
    int, const char *, fsimg_source_cursor_t *, fsimg_error_t *);
static fsimg_status_t open_root_file(fsimg_writer_t *,
    const fsimg_source_root_t *, fsimg_source_cursor_t *, fsimg_error_t *);
static fsimg_status_t open_host_source(fsimg_writer_t *, uint64_t,
    fsimg_host_kind_t, fsimg_source_cursor_t *, fsimg_error_t *);
static fsimg_status_t read_host_record(fsimg_writer_t *, uint64_t,
    fsimg_host_kind_t, fsimg_source_cursor_t *, fsimg_source_root_t **,
    char **, fsimg_error_t *);
static fsimg_status_t adopt_reopened_file(fsimg_writer_t *, int,
    const fsimg_host_file_t *, fsimg_source_cursor_t *, fsimg_error_t *);
static fsimg_status_t open_failure(int, fsimg_source_walk_t, fsimg_error_t *);
static fsimg_status_t open_generated(fsimg_writer_t *, uint64_t,
    fsimg_entry_kind_t, fsimg_source_cursor_t *, fsimg_error_t *);
static fsimg_status_t check_digest(fsimg_source_cursor_t *, fsimg_error_t *);
static bool generated_content(const fsimg_source_cursor_t *);
static int64_t source_signed(uint64_t);

/* The scan's refusal of a long host link states the ceiling in its text. */
_Static_assert(FSIMG_SYMLINK_TARGET_MAX == 1024,
    "the link-target diagnostic states the entry model's ceiling");

bool
fsimg_source_identity_equal(const fsimg_host_file_t *a, const fsimg_host_file_t *b)
{
	return (a->device == b->device && a->inode == b->inode &&
	    a->kind == b->kind && a->length == b->length &&
	    a->modified.seconds == b->modified.seconds &&
	    a->modified.nanoseconds == b->modified.nanoseconds &&
	    a->changed.seconds == b->changed.seconds &&
	    a->changed.nanoseconds == b->changed.nanoseconds);
}

void
fsimg_source_close(fsimg_writer_t *writer)
{
	fsimg_source_cursor_t *source;

	source = &writer->source;
	fsimg_io_free(source->io);
	source->io = NULL;
	source->fd = -1;
	source->active = false;
}

void
fsimg_sources_release(fsimg_writer_t *writer)
{
	fsimg_source_root_t *root;

	fsimg_source_close(writer);
	while (writer->roots != NULL) {
		root = writer->roots;
		writer->roots = root->next;
		/* A descriptorless root holds no reservation to return. */
		if (root->fd != -1)
			fsimg_fd_release(writer->ctx, root->fd);
		fsimg_dealloc(root);
	}
}

fsimg_status_t
fsimg_writer_add_tree(fsimg_writer_t *writer, int fd, const char *image_path,
    fsimg_error_t *error)
{
	fsimg_status_t status;
	size_t length;
	uint32_t depth;
	int duplicate, saved;

	status = fsimg_writer_collect_ready(writer, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_path_validate(writer->ctx, image_path, true,
	    &length, &depth, error);
	if (status != FSIMG_OK)
		return (fsimg_writer_fail(writer, status));
	status = fsimg_fd_reserve(writer->ctx, error);
	if (status != FSIMG_OK)
		return (fsimg_writer_fail(writer, status));
	duplicate = fsimg_host_dup(fd);
	if (duplicate == -1) {
		saved = errno;
		fsimg_fd_release(writer->ctx, -1);
		status = fsimg_error_set(error, FSIMG_EIO, saved,
		    FSIMG_OP_SOURCE, "source root duplication failed");
		return (fsimg_writer_fail(writer, status));
	}
	return (fsimg_writer_adopt_tree(writer, duplicate, image_path, error));
}

/* Consume the root descriptor and its reservation on every outcome. Worker
 * startup uses this boundary after acknowledgement, so confinement never needs
 * descriptor duplication or an extra untracked root capability.
 *
 * A directory root populates image_path with its tree; a regular-file root
 * is the one file at image_path, so it cannot be the image root. Either way
 * the registry owns the descriptor until the writer releases its sources.
 */
fsimg_status_t
fsimg_writer_adopt_tree(fsimg_writer_t *writer, int fd,
    const char *image_path, fsimg_error_t *error)
{
	fsimg_source_root_t *root;
	fsimg_host_file_t identity;
	fsimg_error_t detail;
	size_t length;
	uint32_t depth;
	fsimg_status_t status;
	int flags;

	root = NULL;
	status = fsimg_writer_collect_ready(writer, error);
	if (status != FSIMG_OK)
		goto fail;
	status = fsimg_path_validate(writer->ctx, image_path, true,
	    &length, &depth, error);
	if (status != FSIMG_OK)
		goto fail;
	flags = fcntl(fd, F_GETFL);
	if (flags == -1 || fsimg_host_file(fd, &identity) == -1) {
		status = fsimg_error_set(error, FSIMG_EIO, errno,
		    FSIMG_OP_SOURCE, "source root inspection failed");
		goto fail;
	}
	if ((identity.kind != FSIMG_HOST_DIRECTORY &&
	    identity.kind != FSIMG_HOST_FILE) ||
	    (flags & O_ACCMODE) != O_RDONLY || fsimg_host_unsupported_io(flags)) {
		status = fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_SOURCE,
		    "source root must be a readable directory or regular file");
		goto fail;
	}
	if (identity.kind == FSIMG_HOST_FILE && strcmp(image_path, "/") == 0) {
		status = fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_SOURCE,
		    "a regular-file source root cannot populate the image root");
		goto fail;
	}
	status = fsimg_source_check_object(writer, &identity, error);
	if (status != FSIMG_OK)
		goto fail;
	root = fsimg_alloc(writer->ctx, sizeof(*root), &detail);
	if (root == NULL) {
		status = detail.status;
		if (error != NULL)
			*error = detail;
		goto fail;
	}
	root->fd = fd;
	root->identity = identity;
	root->id = writer->root_count;
	writer->root_count++;
	root->next = writer->roots;
	writer->roots = root;
	status = fsimg_source_scan(writer, root, image_path, error);
	if (status != FSIMG_OK)
		return (fsimg_writer_fail(writer, status));
	return (FSIMG_OK);
fail:
	fsimg_dealloc(root);
	fsimg_fd_release(writer->ctx, fd);
	return (fsimg_writer_fail(writer, status));
}

static bool
is_output_root(const fsimg_writer_t *writer,
    const fsimg_host_file_t *identity)
{
	return (writer->output_root_known &&
	    identity->device == writer->output_root.device &&
	    identity->inode == writer->output_root.inode);
}

static bool
is_scratch(const fsimg_ctx_t *ctx, const fsimg_host_file_t *identity)
{
	return (ctx->options.scratch_directory_fd != -1 &&
	    identity->device == ctx->scratch_device &&
	    identity->inode == ctx->scratch_inode);
}

bool
fsimg_source_is_working_storage(const fsimg_writer_t *writer,
    const fsimg_host_file_t *identity)
{
	return (identity->kind == FSIMG_HOST_DIRECTORY &&
	    (is_output_root(writer, identity) ||
	    is_scratch(writer->ctx, identity)));
}

fsimg_status_t
fsimg_source_check_object(fsimg_writer_t *writer,
    const fsimg_host_file_t *identity, fsimg_error_t *error)
{
	if (is_output_root(writer, identity))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_SOURCE, "source reaches its output directory"));
	if (is_scratch(writer->ctx, identity))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_SOURCE, "source reaches its scratch directory"));
	if (identity->kind != FSIMG_HOST_FILE && identity->kind != FSIMG_HOST_DIRECTORY &&
	    identity->kind != FSIMG_HOST_LINK)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_SOURCE, "source object is not a regular file, directory or symbolic link"));
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_source_read_link(fsimg_writer_t *writer, int parent, const char *name,
    fsimg_host_file_t *identity, unsigned char *target, fsimg_error_t *error)
{
	fsimg_host_file_t after;
	ssize_t length;
	fsimg_status_t status;
	int saved;

	status = fsimg_session_charge(&writer->session, 0, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	if (fsimg_host_link(parent, name, identity) == -1) {
		saved = errno;
		return (link_failure(saved, saved == ENOENT, error));
	}
	if (identity->kind != FSIMG_HOST_LINK)
		return (fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
		    FSIMG_OP_SOURCE, "source object is no longer a symbolic link"));
	length = readlinkat(parent, name, (char *)target,
	    FSIMG_SYMLINK_TARGET_MAX + 1);
	if (length == -1) {
		saved = errno;
		return (link_failure(saved, saved == ENOENT || saved == EINVAL,
		    error));
	}
	/* Inspecting the link again proves the target belongs to identity. */
	if (fsimg_host_link(parent, name, &after) == -1 ||
	    !fsimg_source_identity_equal(identity, &after))
		return (fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
		    FSIMG_OP_SOURCE, "source link changed while its target was read"));
	if (length == 0 || (size_t)length > FSIMG_SYMLINK_TARGET_MAX)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_SOURCE, "symbolic link target is empty or longer than 1024 bytes"));
	identity->length = (uint64_t)length;
	return (FSIMG_OK);
}

/* A link that vanished or stopped being a link changed; a process whose
 * confinement denies link inspection cannot import links at all, which is
 * a missing capability rather than an I/O failure.
 */
static fsimg_status_t
link_failure(int saved, bool changed, fsimg_error_t *error)
{
	if (changed)
		return (fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, saved,
		    FSIMG_OP_SOURCE, "source link changed while it was inspected"));
	if (saved == EPERM || saved == EACCES)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, saved,
		    FSIMG_OP_SOURCE, "symbolic link inspection is not permitted"));
	return (fsimg_error_set(error, FSIMG_EIO, saved, FSIMG_OP_SOURCE,
	    "source link inspection failed"));
}

static fsimg_status_t
callback_status(fsimg_status_t status, fsimg_error_t *error)
{
	if (status == FSIMG_OK) {
		fsimg_error_clear(error);
		return (FSIMG_OK);
	}
	if (status > FSIMG_OK || status < FSIMG_EDURABILITY)
		return (fsimg_error_set(error, FSIMG_EIO, 0,
		    FSIMG_OP_SOURCE, "invalid source callback status"));
	if (error != NULL && error->status != status)
		fsimg_error_set(error, status, 0, FSIMG_OP_SOURCE,
		    "source callback failed");
	return (status);
}

static fsimg_status_t
source_validate(fsimg_source_cursor_t *source, fsimg_error_t *error)
{
	fsimg_writer_t *writer;
	fsimg_host_file_t identity;
	fsimg_status_t status;

	writer = source->writer;
	status = fsimg_session_charge(&writer->session, 0, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	if (source->storage == FSIMG_STORAGE_PROVIDER) {
		writer->ctx->in_callback = true;
		status = writer->provider.validate(writer->provider_argument,
		    source->id, error);
		writer->ctx->in_callback = false;
		return (callback_status(status, error));
	}
	/* A link's target was read into memory under a stable identity when
	 * it was opened; the bytes copied are those bytes, so only a file's
	 * descriptor can change underneath the copy.
	 */
	if (source->storage == FSIMG_STORAGE_HOST &&
	    source->identity.kind == FSIMG_HOST_FILE) {
		if (fsimg_host_file(source->fd, &identity) == -1)
			return (fsimg_error_set(error, FSIMG_EIO, errno,
			    FSIMG_OP_SOURCE, "source file inspection failed"));
		if (!fsimg_source_identity_equal(&identity, &source->identity))
			return (fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
			    FSIMG_OP_SOURCE, "source identity, size, or change metadata differs"));
	}
	return (FSIMG_OK);
}

static fsimg_status_t
provider_size(void *argument, uint64_t *out, fsimg_error_t *error)
{
	fsimg_source_cursor_t *source = argument;

	fsimg_error_clear(error);
	*out = source->length;
	return (FSIMG_OK);
}

static fsimg_status_t
provider_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error)
{
	fsimg_source_cursor_t *source = argument;
	fsimg_writer_t *writer;

	writer = source->writer;
	return (writer->provider.read_at(writer->provider_argument, source->id,
	    offset, buffer, length, transferred, error));
}

static fsimg_status_t
link_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error)
{
	fsimg_source_cursor_t *source = argument;

	fsimg_error_clear(error);
	bcopy(source->link_target + offset, buffer, length);
	*transferred = length;
	return (FSIMG_OK);
}

/* Serve a link's content from link_target, which opening filled with the
 * whole target: later changes to the link or the provider cannot alter the
 * bytes being copied, and the content has no holes.
 */
static fsimg_status_t
serve_link(fsimg_writer_t *writer, fsimg_source_cursor_t *source,
    fsimg_error_t *error)
{
	fsimg_io_ops_t ops;

	fsimg_io_ops_init(&ops);
	ops.get_size = provider_size;
	ops.read_at = link_read;
	source->link = true;
	return (fsimg_io_from_callbacks(writer->ctx, &ops, source, &source->io,
	    error));
}

/* Read a provider link's whole target through a transient provider I/O
 * object and reject a NUL byte, so every link target any driver receives,
 * host or provider, is checked here once. Host targets cannot hold one.
 */
static fsimg_status_t
open_provider_link(fsimg_writer_t *writer, fsimg_source_cursor_t *source,
    fsimg_error_t *error)
{
	fsimg_io_ops_t ops;
	fsimg_io_t *io;
	size_t transferred;
	fsimg_status_t status;

	if (source->length == 0 || source->length > FSIMG_SYMLINK_TARGET_MAX)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_SOURCE, "catalog link target length is out of range"));
	fsimg_io_ops_init(&ops);
	ops.max_transfer = writer->provider.max_transfer;
	ops.get_size = provider_size;
	ops.read_at = provider_read;
	status = fsimg_io_from_callbacks(writer->ctx, &ops, source, &io, error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_io_bind_session(io, &writer->session);
	status = fsimg_io_read_exact(io, 0, source->link_target,
	    (size_t)source->length, &transferred, error);
	fsimg_io_free(io);
	if (status != FSIMG_OK)
		return (status);
	if (memchr(source->link_target, '\0', (size_t)source->length) != NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_SOURCE,
		    "provider symbolic link target contains a NUL byte"));
	return (serve_link(writer, source, error));
}

/* Reopen a link: read its target again and require the recorded identity,
 * including the target length, then serve the bytes read.
 */
static fsimg_status_t
open_host_link(fsimg_writer_t *writer, int parent, const char *name,
    fsimg_source_cursor_t *source, fsimg_error_t *error)
{
	fsimg_host_file_t opened;
	fsimg_status_t status;

	status = fsimg_source_read_link(writer, parent, name, &opened,
	    source->link_target, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_source_check_object(writer, &opened, error);
	if (status != FSIMG_OK)
		return (status);
	if (!fsimg_source_identity_equal(&opened, &source->identity))
		return (fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
		    FSIMG_OP_SOURCE, "reopened source link differs from its recorded identity"));
	return (serve_link(writer, source, error));
}

/* Open a regular-file root's source without reopening a name: the root
 * descriptor, which the registry keeps open, must still describe the
 * recorded identity, checked as for a reopened file. Its bytes are read
 * through an I/O object that borrows that descriptor. The registry holds
 * the root until the writer releases its sources, which closes the current
 * source first, so the descriptor outlives the object; freeing the object
 * leaves it open.
 */
static fsimg_status_t
open_root_file(fsimg_writer_t *writer, const fsimg_source_root_t *root,
    fsimg_source_cursor_t *source, fsimg_error_t *error)
{
	fsimg_host_file_t opened;
	fsimg_status_t status;

	status = fsimg_session_charge(&writer->session, 0, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	if (fsimg_host_file(root->fd, &opened) == -1)
		return (fsimg_error_set(error, FSIMG_EIO, errno,
		    FSIMG_OP_SOURCE, "source root file inspection failed"));
	status = fsimg_source_check_object(writer, &opened, error);
	if (status != FSIMG_OK)
		return (status);
	if (!fsimg_source_identity_equal(&opened, &source->identity))
		return (fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
		    FSIMG_OP_SOURCE, "source root file differs from its recorded identity"));
	status = fsimg_io_borrow_fd(writer->ctx, root->fd, FSIMG_IO_READ,
	    &source->io, error);
	if (status != FSIMG_OK)
		return (status);
	source->fd = root->fd;
	return (FSIMG_OK);
}

static int64_t
source_signed(uint64_t value)
{
	return (value <= INT64_MAX ? (int64_t)value :
	    -1 - (int64_t)(UINT64_MAX - value));
}

/* Read and validate the host source record at reference: its framing, its
 * recorded identity, which goes to the cursor, and its digest claim, which
 * starts the cursor's digest. Return the record's root and its relative
 * path, which stays in the writer's source record buffer.
 */
static fsimg_status_t
read_host_record(fsimg_writer_t *writer, uint64_t reference,
    fsimg_host_kind_t kind, fsimg_source_cursor_t *source,
    fsimg_source_root_t **rootp, char **pathp, fsimg_error_t *error)
{
	static const unsigned char no_digest[FSIMG_SHA256_DIGEST_LENGTH];
	fsimg_source_root_t *root;
	unsigned char *record;
	uint64_t digest_flag, root_id;
	size_t length;
	fsimg_status_t status;

	status = fsimg_stream_next(writer->host_sources, &reference,
	    writer->source_record, writer->source_record_capacity, &length, error);
	if (status == FSIMG_DONE)
		status = fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_SOURCE, "source reference points to the end of its stream");
	if (status != FSIMG_OK)
		return (status);
	record = writer->source_record;
	if (length <= FSIMG_SOURCE_HEADER ||
	    length - FSIMG_SOURCE_HEADER > writer->ctx->options.max_path_bytes ||
	    strnlen((const char *)record + FSIMG_SOURCE_HEADER,
	    length - FSIMG_SOURCE_HEADER) != length - FSIMG_SOURCE_HEADER - 1 ||
	    fsimg_load_le64(record + 40) >= 1000000000 ||
	    fsimg_load_le64(record + 56) >= 1000000000 ||
	    fsimg_load_le64(record + 64) != kind ||
	    fsimg_load_le64(record + 72) != 1)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_SOURCE, "invalid source record fields or path framing"));
	root_id = fsimg_load_le64(record);
	for (root = writer->roots; root != NULL; root = root->next) {
		if (root->id == root_id)
			break;
	}
	if (root == NULL)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_SOURCE, "source root is not retained"));
	source->identity.device = fsimg_load_le64(record + 8);
	source->identity.inode = fsimg_load_le64(record + 16);
	source->identity.length = fsimg_load_le64(record + 24);
	source->identity.modified.seconds = source_signed(fsimg_load_le64(record + 32));
	source->identity.modified.nanoseconds = (uint32_t)fsimg_load_le64(record + 40);
	source->identity.changed.seconds = source_signed(fsimg_load_le64(record + 48));
	source->identity.changed.nanoseconds = (uint32_t)fsimg_load_le64(record + 56);
	source->identity.kind = kind;
	if (source->identity.length != source->length)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_SOURCE, "source and catalog lengths disagree"));
	digest_flag = fsimg_load_le64(record + 80);
	if (digest_flag > 1 || (digest_flag == 1 && kind != FSIMG_HOST_FILE) ||
	    (digest_flag == 0 && bcmp(record + FSIMG_SOURCE_DIGEST, no_digest,
	    sizeof(no_digest)) != 0))
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_SOURCE, "invalid source record digest fields"));
	if (digest_flag == 1) {
		source->digest_claimed = true;
		bcopy(record + FSIMG_SOURCE_DIGEST, source->digest,
		    sizeof(source->digest));
		fsimg_sha256_init(&source->digest_state);
	}
	*rootp = root;
	*pathp = (char *)record + FSIMG_SOURCE_HEADER;
	return (FSIMG_OK);
}

/* Walk the recorded relative path from its root one component at a time,
 * never following a link, and open the final object if it still has the
 * recorded identity. kind is the host kind the catalog entry requires.
 */
static fsimg_status_t
open_host_source(fsimg_writer_t *writer, uint64_t reference,
    fsimg_host_kind_t kind, fsimg_source_cursor_t *source, fsimg_error_t *error)
{
	fsimg_source_root_t *root = NULL;
	fsimg_host_file_t opened;
	char *name, *path = NULL;
	fsimg_status_t status;
	int fd, parent;

	status = read_host_record(writer, reference, kind, source, &root, &path,
	    error);
	if (status != FSIMG_OK)
		return (status);
	/* Only a regular-file root's record has an empty relative path; a
	 * directory root's empty first component fails in the walk. A
	 * descriptorless root has no path to walk at all.
	 */
	if (root->identity.kind == FSIMG_HOST_FILE) {
		if (path[0] != '\0' || kind != FSIMG_HOST_FILE)
			return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
			    FSIMG_OP_SOURCE, "invalid regular-file root source record"));
		return (open_root_file(writer, root, source, error));
	}
	if (root->fd == -1)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_SOURCE, "source record names a descriptorless root"));

	status = fsimg_source_walk(writer, root, path, FSIMG_WALK_REOPEN,
	    &parent, &name, error);
	if (status != FSIMG_OK)
		return (status);
	if (kind == FSIMG_HOST_LINK) {
		status = open_host_link(writer, parent, name, source, error);
	} else {
		status = fsimg_source_open_child(writer, parent, name, false,
		    FSIMG_WALK_REOPEN, &fd, &opened, error);
		if (status == FSIMG_OK)
			status = adopt_reopened_file(writer, fd, &opened, source,
			    error);
	}
	if (parent != root->fd)
		fsimg_fd_release(writer->ctx, parent);
	return (status);
}

/* Take a reopened file as the current source if it still has the recorded
 * identity; the descriptor is consumed either way.
 */
static fsimg_status_t
adopt_reopened_file(fsimg_writer_t *writer, int fd,
    const fsimg_host_file_t *opened, fsimg_source_cursor_t *source,
    fsimg_error_t *error)
{
	if (!fsimg_source_identity_equal(opened, &source->identity)) {
		fsimg_fd_release(writer->ctx, fd);
		return (fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
		    FSIMG_OP_SOURCE, "reopened source differs from its recorded identity"));
	}
	source->fd = fd;
	return (fsimg_io_adopt_fd(writer->ctx, fd, FSIMG_IO_READ, &source->io,
	    error));
}

/* A failed open beneath a root, reported for why the path was walked. A
 * recorded path whose object vanished, became a link, stopped being a
 * directory or became an object no open reads (ENXIO) names a changed
 * source. A manifest's contents path that has a link is unsupported, as
 * the scanner cannot record one there, and one that names nothing, or a
 * name its file system cannot hold, is invalid input.
 */
static fsimg_status_t
open_failure(int saved, fsimg_source_walk_t walk, fsimg_error_t *error)
{
	if (walk == FSIMG_WALK_REOPEN)
		return (fsimg_error_set(error,
		    saved == ENOENT || saved == ELOOP || saved == ENOTDIR ||
		    saved == ENXIO ?
		    FSIMG_ESOURCE_CHANGED : FSIMG_EIO, saved,
		    FSIMG_OP_SOURCE, "source component cannot be reopened"));
	if (saved == ELOOP)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, saved,
		    FSIMG_OP_SOURCE, "contents path has a symbolic link"));
	if (saved == ENOENT || saved == ENOTDIR || saved == ENAMETOOLONG)
		return (fsimg_error_set(error, FSIMG_EINVAL, saved,
		    FSIMG_OP_SOURCE, "contents path names no file"));
	return (fsimg_error_set(error, FSIMG_EIO, saved, FSIMG_OP_SOURCE,
	    "contents path component cannot be opened"));
}

/* A manifest, not a directory listing, names the final object of an import
 * walk, so it may name anything; it is opened as every object is and then
 * checked, as specs/design-mtree.md ("Host bytes") explains.
 */
fsimg_status_t
fsimg_source_open_child(fsimg_writer_t *writer, int parent,
    const char *name, bool directory, fsimg_source_walk_t walk,
    int *out, fsimg_host_file_t *identity, fsimg_error_t *error)
{
	fsimg_status_t status;
	int fd, saved;
	bool manifest_file;

	/* The final name of an import walk is a file a manifest names. */
	manifest_file = walk == FSIMG_WALK_IMPORT && !directory;
	*out = -1;
	status = fsimg_session_charge(&writer->session, 0, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_fd_reserve(writer->ctx, error);
	if (status != FSIMG_OK)
		return (status);
	fd = fsimg_host_source_open(parent, name, directory);
	if (fd == -1) {
		saved = errno;
		fsimg_fd_release(writer->ctx, -1);
		/* The open itself refuses a socket or a device without a
		 * driver (ENXIO, fsimg_host_source_open): no regular file.
		 */
		if (manifest_file && saved == ENXIO)
			return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, saved,
			    FSIMG_OP_SOURCE, "contents path does not name a regular file"));
		return (open_failure(saved, walk, error));
	}
	if (fsimg_host_file(fd, identity) == -1) {
		saved = errno;
		fsimg_fd_release(writer->ctx, fd);
		return (fsimg_error_set(error, FSIMG_EIO, saved,
		    FSIMG_OP_SOURCE, "opened source object inspection failed"));
	}
	if (manifest_file && identity->kind != FSIMG_HOST_FILE) {
		fsimg_fd_release(writer->ctx, fd);
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_SOURCE, "contents path does not name a regular file"));
	}
	status = fsimg_source_check_object(writer, identity, error);
	if (status != FSIMG_OK) {
		fsimg_fd_release(writer->ctx, fd);
		return (status);
	}
	*out = fd;
	return (FSIMG_OK);
}

/* Each directory is opened relative to the one before it, so the walk
 * holds at most two descriptors: the previous directory is released as
 * soon as its child is open. A separator is cut only while the component
 * before it is opened, so path is unchanged on every return.
 */
fsimg_status_t
fsimg_source_walk(fsimg_writer_t *writer, const fsimg_source_root_t *root,
    char *path, fsimg_source_walk_t walk, int *parent_out, char **name_out,
    fsimg_error_t *error)
{
	fsimg_host_file_t opened;
	char *component, *separator;
	fsimg_status_t status;
	int fd, parent;

	parent = root->fd;
	component = path;
	for (;;) {
		separator = strchr(component, '/');
		if (separator != NULL)
			*separator = '\0';
		if (component[0] == '\0' || strcmp(component, ".") == 0 ||
		    strcmp(component, "..") == 0) {
			status = fsimg_error_set(error,
			    walk == FSIMG_WALK_REOPEN ? FSIMG_ECORRUPT : FSIMG_EINVAL,
			    0, FSIMG_OP_SOURCE, "invalid relative source component");
			break;
		}
		if (separator == NULL) {
			*parent_out = parent;
			*name_out = component;
			return (FSIMG_OK);
		}
		status = fsimg_source_open_child(writer, parent, component,
		    true, walk, &fd, &opened, error);
		*separator = '/';
		if (status != FSIMG_OK)
			break;
		if (parent != root->fd)
			fsimg_fd_release(writer->ctx, parent);
		parent = fd;
		component = separator + 1;
	}
	if (separator != NULL)
		*separator = '/';
	if (parent != root->fd)
		fsimg_fd_release(writer->ctx, parent);
	return (status);
}

fsimg_status_t
fsimg_source_open(fsimg_writer_t *writer, uint64_t entry_id,
    fsimg_error_t *error)
{
	fsimg_source_cursor_t *source;
	fsimg_catalog_entry_t entry;
	fsimg_io_ops_t ops;
	fsimg_status_t status;

	status = fsimg_ctx_ready(writer == NULL ? NULL : writer->ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (!writer->catalog_ready || writer->state == FSIMG_WRITER_FAILED ||
	    writer->source.active)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_SOURCE, "source requires a live sealed file entry"));
	status = fsimg_catalog_read_source(writer, entry_id, &entry, error);
	if (status != FSIMG_OK)
		return (status);
	if (entry.entry.kind != FSIMG_ENTRY_FILE &&
	    entry.entry.kind != FSIMG_ENTRY_SYMLINK)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_SOURCE, "only files and symbolic links have a content source"));
	source = &writer->source;
	bzero(source, sizeof(*source));
	source->fd = -1;
	source->writer = writer;
	source->storage = entry.storage;
	source->id = entry.source_id;
	source->length = entry.entry.size;
	source->active = true;
	if (source->storage == FSIMG_STORAGE_HOST) {
		status = open_host_source(writer, source->id,
		    entry.entry.kind == FSIMG_ENTRY_FILE ? FSIMG_HOST_FILE :
		    FSIMG_HOST_LINK, source, error);
		if (status != FSIMG_OK)
			return (source_abort(source, status));
	} else if (source->storage == FSIMG_STORAGE_GENERATED) {
		status = open_generated(writer, source->id, entry.entry.kind,
		    source, error);
		if (status != FSIMG_OK)
			return (source_abort(source, status));
	} else if (source->storage == FSIMG_STORAGE_PROVIDER &&
	    entry.entry.kind == FSIMG_ENTRY_SYMLINK) {
		status = open_provider_link(writer, source, error);
		if (status != FSIMG_OK)
			return (source_abort(source, status));
	} else if (source->storage == FSIMG_STORAGE_PROVIDER) {
		fsimg_io_ops_init(&ops);
		ops.max_transfer = writer->provider.max_transfer;
		ops.get_size = provider_size;
		ops.read_at = provider_read;
		status = fsimg_io_from_callbacks(writer->ctx, &ops, source,
		    &source->io, error);
		if (status != FSIMG_OK)
			return (source_abort(source, status));
	} else if (source->storage != FSIMG_STORAGE_ZERO) {
		status = fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_SOURCE, "invalid file source storage");
		return (source_abort(source, status));
	}
	if (source->io != NULL)
		fsimg_io_bind_session(source->io, &writer->session);
	status = source_validate(source, error);
	if (status != FSIMG_OK)
		return (source_abort(source, status));
	return (FSIMG_OK);
}

static fsimg_status_t
source_abort(fsimg_source_cursor_t *source, fsimg_status_t status)
{
	fsimg_source_close(source->writer);
	return (fsimg_writer_fail(source->writer, status));
}

fsimg_status_t
fsimg_source_read(fsimg_writer_t *writer, uint64_t offset, void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error)
{
	fsimg_source_cursor_t *source;
	fsimg_error_t detail;
	fsimg_status_t status;
	size_t remaining;

	if (transferred != NULL)
		*transferred = 0;
	status = fsimg_ctx_ready(writer == NULL ? NULL : writer->ctx, error);
	if (status != FSIMG_OK)
		return (status);
	source = &writer->source;
	if (!source->active || transferred == NULL || (buffer == NULL && length != 0))
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_SOURCE, "inactive source or missing read argument"));
	if (offset > source->length)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_SOURCE, "source read starts past declared length"));
	remaining = source->length - offset < length ?
	    (size_t)(source->length - offset) : length;
	if (remaining == 0)
		return (FSIMG_OK);
	if (source->storage == FSIMG_STORAGE_ZERO) {
		bzero(buffer, remaining);
		*transferred = remaining;
	} else if (generated_content(source)) {
		fsimg_generated_read(&source->generated, offset, buffer,
		    remaining);
		*transferred = remaining;
	} else {
		status = fsimg_io_read_exact(source->io, offset, buffer, remaining,
		    transferred, error);
		/* Identity is validated at open and at finish, not around each
		 * read, so the bytes read are trusted only once finish succeeds.
		 * A read that fails with an I/O error, such as one cut short by
		 * truncation, reports a changed file as changed instead, keeping
		 * where the read stopped.
		 */
		if (status == FSIMG_EIO) {
			fsimg_error_clear(&detail);
			if (source_validate(source, &detail) == FSIMG_ESOURCE_CHANGED) {
				status = FSIMG_ESOURCE_CHANGED;
				if (error != NULL) {
					detail.offset = error->offset;
					detail.offset_valid = error->offset_valid;
					*error = detail;
				}
			}
		}
		if (status != FSIMG_OK)
			return (source_abort(source, status));
	}
	/* Only a read continuing the hashed prefix extends it; finishing
	 * tells a prefix that stopped short from one that covers the file.
	 */
	if (source->digest_claimed && offset == source->digest_offset) {
		fsimg_sha256_update(&source->digest_state, buffer, *transferred);
		source->digest_offset += *transferred;
	}
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_source_range(fsimg_writer_t *writer, uint64_t offset,
    fsimg_range_t *range, fsimg_error_t *error)
{
	fsimg_source_cursor_t *source;
	fsimg_status_t status;

	if (range != NULL)
		bzero(range, sizeof(*range));
	status = fsimg_ctx_ready(writer == NULL ? NULL : writer->ctx, error);
	if (status != FSIMG_OK)
		return (status);
	source = &writer->source;
	if (!source->active || range == NULL)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_SOURCE, "inactive source or missing range output"));
	if (offset > source->length)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_SOURCE, "source range starts past declared length"));
	if (offset == source->length)
		return (FSIMG_DONE);
	if (generated_content(source)) {
		fsimg_generated_range(&source->generated, offset, range);
		return (FSIMG_OK);
	}
	if (source->storage != FSIMG_STORAGE_PROVIDER || source->link ||
	    writer->provider.next_range == NULL) {
		range->offset = offset;
		range->length = source->length - offset;
		range->kind = source->storage == FSIMG_STORAGE_ZERO ?
		    FSIMG_RANGE_ZERO : FSIMG_RANGE_DATA;
		return (FSIMG_OK);
	}
	status = fsimg_session_charge(&writer->session, 0, 0, 1, error);
	if (status != FSIMG_OK)
		return (source_abort(source, status));
	writer->ctx->in_callback = true;
	status = writer->provider.next_range(writer->provider_argument,
	    source->id, offset, range, error);
	writer->ctx->in_callback = false;
	status = callback_status(status, error);
	if (status == FSIMG_OK && (range->offset != offset || range->length == 0 ||
	    range->length > source->length - offset ||
	    (range->kind != FSIMG_RANGE_DATA && range->kind != FSIMG_RANGE_ZERO)))
		status = fsimg_error_set(error, FSIMG_EIO, 0,
		    FSIMG_OP_SOURCE, "provider returned an invalid logical range");
	if (status != FSIMG_OK) {
		bzero(range, sizeof(*range));
		return (source_abort(source, status));
	}
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_source_finish(fsimg_writer_t *writer, fsimg_error_t *error)
{
	fsimg_source_cursor_t *source;
	fsimg_status_t status;

	status = fsimg_ctx_ready(writer == NULL ? NULL : writer->ctx, error);
	if (status != FSIMG_OK)
		return (status);
	source = &writer->source;
	if (!source->active)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_SOURCE, "source cursor is not active"));
	status = source_validate(source, error);
	if (status == FSIMG_OK)
		status = check_digest(source, error);
	fsimg_source_close(writer);
	if (status != FSIMG_OK)
		return (fsimg_writer_fail(source->writer, status));
	return (FSIMG_OK);
}

/* Whether the cursor computes its bytes from source->generated: generated
 * file content has neither descriptor nor I/O object. A generated link is
 * not, since its target is served from link_target like every link's.
 */
static bool
generated_content(const fsimg_source_cursor_t *source)
{
	return (source->storage == FSIMG_STORAGE_GENERATED && !source->link);
}

/* Compare a host file's claimed digest with the bytes served. Finishing
 * asserts that the cursor served the file: one that served less than the
 * whole file in order from offset zero cannot check the claim, which is a
 * defect of its reader, not of the file. A caller that only opens a source
 * to inspect it closes it instead of finishing it.
 */
static fsimg_status_t
check_digest(fsimg_source_cursor_t *source, fsimg_error_t *error)
{
	unsigned char digest[FSIMG_SHA256_DIGEST_LENGTH];

	if (!source->digest_claimed)
		return (FSIMG_OK);
	if (source->digest_offset != source->length)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_SOURCE, "source with a digest was not read in order to its end"));
	fsimg_sha256_final(digest, &source->digest_state);
	if (bcmp(digest, source->digest, sizeof(digest)) != 0)
		return (fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
		    FSIMG_OP_SOURCE, "source bytes do not match the manifest's sha256digest"));
	return (FSIMG_OK);
}

/* Read a generated record. A link's target is copied out of the record
 * buffer, which later lookups reuse, and served as every link target is;
 * file content keeps its generator in the cursor.
 */
static fsimg_status_t
open_generated(fsimg_writer_t *writer, uint64_t reference,
    fsimg_entry_kind_t kind, fsimg_source_cursor_t *source,
    fsimg_error_t *error)
{
	size_t length;
	fsimg_status_t status;

	status = fsimg_stream_next(writer->host_sources, &reference,
	    writer->source_record, writer->source_record_capacity, &length,
	    error);
	if (status == FSIMG_DONE)
		status = fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_SOURCE, "source reference points to the end of its stream");
	if (status != FSIMG_OK)
		return (status);
	if (!fsimg_generated_decode(writer->source_record, length,
	    &source->generated) ||
	    source->generated.generator == FSIMG_GENERATOR_ZERO ||
	    source->generated.length != source->length ||
	    (source->generated.generator == FSIMG_GENERATOR_LINK) !=
	    (kind == FSIMG_ENTRY_SYMLINK))
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_SOURCE, "invalid generated source record"));
	if (kind != FSIMG_ENTRY_SYMLINK)
		return (FSIMG_OK);
	bcopy(source->generated.target, source->link_target,
	    (size_t)source->length);
	source->generated.target = NULL;
	return (serve_link(writer, source, error));
}
