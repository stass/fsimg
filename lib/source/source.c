/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "source.h"
#include "unicode.h"

static fsimg_status_t source_validate(fsimg_source_cursor_t *, fsimg_error_t *);
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
static fsimg_status_t open_host_link(fsimg_writer_t *, fsimg_source_root_t *,
    int, const char *, fsimg_source_cursor_t *, fsimg_error_t *);
static fsimg_status_t open_host_source(fsimg_writer_t *, uint64_t,
    fsimg_host_kind_t, fsimg_source_cursor_t *, fsimg_error_t *);
static int64_t source_signed(uint64_t);

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
		fsimg_fd_release(writer->ctx, root->fd);
		fsimg_dealloc(root);
	}
}

fsimg_status_t
fsimg_writer_add_tree(fsimg_writer_t *writer, int fd, const char *image_directory,
    fsimg_error_t *error)
{
	fsimg_status_t status;
	size_t length;
	uint32_t depth;
	int duplicate, saved;

	status = fsimg_writer_collect_ready(writer, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_path_validate(writer->ctx, image_directory, true,
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
	return (fsimg_writer_adopt_tree(writer, duplicate, image_directory, error));
}

/* Consume the root descriptor and its reservation on every outcome. Worker
 * startup uses this boundary after acknowledgement, so confinement never needs
 * descriptor duplication or an extra untracked root capability.
 */
fsimg_status_t
fsimg_writer_adopt_tree(fsimg_writer_t *writer, int fd,
    const char *image_directory, fsimg_error_t *error)
{
	fsimg_source_root_t *root;
	fsimg_host_file_t identity;
	fsimg_error_t detail;
	size_t length;
	uint32_t depth;
	fsimg_status_t status;
	int flags, saved;

	root = NULL;
	status = fsimg_writer_collect_ready(writer, error);
	if (status != FSIMG_OK)
		goto fail;
	status = fsimg_path_validate(writer->ctx, image_directory, true,
	    &length, &depth, error);
	if (status != FSIMG_OK)
		goto fail;
	flags = fcntl(fd, F_GETFL);
	if (flags == -1 || fsimg_host_file(fd, &identity) == -1) {
		status = fsimg_error_set(error, FSIMG_EIO, errno,
		    FSIMG_OP_SOURCE, "source root inspection failed");
		goto fail;
	}
	if (identity.kind != FSIMG_HOST_DIRECTORY ||
	    (flags & O_ACCMODE) != O_RDONLY || fsimg_host_unsupported_io(flags)) {
		status = fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_SOURCE, "source root must be a readable directory");
		goto fail;
	}
	status = fsimg_source_check_object(writer, fd, &identity, NULL, error);
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
	if (fsimg_host_mount_id(fd, &root->mount_id) == -1) {
		saved = errno;
		status = fsimg_error_set(error, saved == ENOTSUP ?
		    FSIMG_EUNSUPPORTED : FSIMG_EIO, saved, FSIMG_OP_SOURCE,
		    "source mount identity is unavailable");
		goto fail;
	}
	root->id = writer->root_count;
	writer->root_count++;
	root->next = writer->roots;
	writer->roots = root;
	status = fsimg_source_scan(writer, root, image_directory, error);
	if (status != FSIMG_OK)
		return (fsimg_writer_fail(writer, status));
	return (FSIMG_OK);
fail:
	fsimg_dealloc(root);
	fsimg_fd_release(writer->ctx, fd);
	return (fsimg_writer_fail(writer, status));
}

fsimg_status_t
fsimg_source_check_object(fsimg_writer_t *writer, int fd,
    const fsimg_host_file_t *identity, const fsimg_source_root_t *root,
    fsimg_error_t *error)
{
	fsimg_host_file_t scratch;
	uint64_t mount_id;
	int saved;

	if (writer->output_root_known &&
	    identity->device == writer->output_root.device &&
	    identity->inode == writer->output_root.inode)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_SOURCE, "source tree encounters its output directory"));
	if (writer->ctx->options.scratch_directory_fd != -1) {
		if (fsimg_host_file(writer->ctx->options.scratch_directory_fd,
		    &scratch) == -1)
			return (fsimg_error_set(error, FSIMG_EIO, errno,
			    FSIMG_OP_SOURCE, "scratch identity inspection failed"));
		if (identity->device == scratch.device && identity->inode == scratch.inode)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_SOURCE, "source tree encounters its scratch directory"));
	}
	if (identity->kind != FSIMG_HOST_FILE && identity->kind != FSIMG_HOST_DIRECTORY &&
	    identity->kind != FSIMG_HOST_LINK)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_SOURCE, "source object is not a regular file, directory or symbolic link"));
	if (root == NULL)
		return (FSIMG_OK);
	/* A link cannot be a mount point and has no descriptor: it lives on
	 * its parent directory's file system, whose mount was checked, so its
	 * device is enough.
	 */
	if (identity->kind == FSIMG_HOST_LINK) {
		if (identity->device != root->identity.device)
			return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
			    FSIMG_OP_SOURCE, "source traversal crosses a descendant mount"));
		return (FSIMG_OK);
	}
	if (fsimg_host_mount_id(fd, &mount_id) == -1) {
		saved = errno;
		return (fsimg_error_set(error, saved == ENOTSUP ?
		    FSIMG_EUNSUPPORTED : FSIMG_EIO, saved, FSIMG_OP_SOURCE,
		    "source mount identity query failed"));
	}
	if (identity->device != root->identity.device || mount_id != root->mount_id)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_SOURCE, "source traversal crosses a descendant mount"));
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
		    FSIMG_OP_SOURCE, "symbolic link target is empty or longer than 1023 bytes"));
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
open_host_link(fsimg_writer_t *writer, fsimg_source_root_t *root, int parent,
    const char *name, fsimg_source_cursor_t *source, fsimg_error_t *error)
{
	fsimg_host_file_t opened;
	fsimg_status_t status;

	status = fsimg_source_read_link(writer, parent, name, &opened,
	    source->link_target, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_source_check_object(writer, -1, &opened, root, error);
	if (status != FSIMG_OK)
		return (status);
	if (!fsimg_source_identity_equal(&opened, &source->identity))
		return (fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
		    FSIMG_OP_SOURCE, "reopened source link differs from its recorded identity"));
	return (serve_link(writer, source, error));
}

static int64_t
source_signed(uint64_t value)
{
	return (value <= INT64_MAX ? (int64_t)value :
	    -1 - (int64_t)(UINT64_MAX - value));
}

/* Walk the recorded relative path from its root one component at a time,
 * never following a link, and open the final object if it still has the
 * recorded identity. kind is the host kind the catalog entry requires.
 */
static fsimg_status_t
open_host_source(fsimg_writer_t *writer, uint64_t reference,
    fsimg_host_kind_t kind, fsimg_source_cursor_t *source, fsimg_error_t *error)
{
	fsimg_source_root_t *root;
	fsimg_host_file_t opened;
	unsigned char *record;
	char *component, *separator;
	uint64_t root_id;
	size_t length;
	fsimg_status_t status;
	int parent, fd, saved;
	bool parent_owned;

	status = fsimg_stream_next(writer->host_sources, &reference, writer->source_record,
	    writer->record_capacity, &length, error);
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
	    fsimg_meta_get_u64(record + 40) >= 1000000000 ||
	    fsimg_meta_get_u64(record + 56) >= 1000000000 ||
	    fsimg_meta_get_u64(record + 64) != kind ||
	    fsimg_meta_get_u64(record + 72) != 1)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_SOURCE, "invalid source record fields or path framing"));
	root_id = fsimg_meta_get_u64(record);
	for (root = writer->roots; root != NULL; root = root->next) {
		if (root->id == root_id)
			break;
	}
	if (root == NULL)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_SOURCE, "source root is not retained"));
	source->identity.device = fsimg_meta_get_u64(record + 8);
	source->identity.inode = fsimg_meta_get_u64(record + 16);
	source->identity.length = fsimg_meta_get_u64(record + 24);
	source->identity.modified.seconds = source_signed(fsimg_meta_get_u64(record + 32));
	source->identity.modified.nanoseconds = (uint32_t)fsimg_meta_get_u64(record + 40);
	source->identity.changed.seconds = source_signed(fsimg_meta_get_u64(record + 48));
	source->identity.changed.nanoseconds = (uint32_t)fsimg_meta_get_u64(record + 56);
	source->identity.kind = kind;
	if (source->identity.length != source->length)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_SOURCE, "source and catalog lengths disagree"));
	component = (char *)record + FSIMG_SOURCE_HEADER;
	parent = root->fd;
	parent_owned = false;
	for (;;) {
		separator = strchr(component, '/');
		if (separator != NULL)
			*separator = '\0';
		if (component[0] == '\0' || strcmp(component, ".") == 0 ||
		    strcmp(component, "..") == 0) {
			status = fsimg_error_set(error, FSIMG_ECORRUPT, 0,
			    FSIMG_OP_SOURCE, "invalid relative source component");
			break;
		}
		if (separator == NULL && kind == FSIMG_HOST_LINK) {
			status = open_host_link(writer, root, parent, component,
			    source, error);
			break;
		}
		status = fsimg_session_charge(&writer->session, 0, 0, 1, error);
		if (status != FSIMG_OK)
			break;
		status = fsimg_fd_reserve(writer->ctx, error);
		if (status != FSIMG_OK)
			break;
		fd = fsimg_host_source_open(parent, component, separator != NULL);
		saved = errno;
		if (parent_owned)
			fsimg_fd_release(writer->ctx, parent);
		parent_owned = false;
		if (separator != NULL)
			*separator = '/';
		if (fd == -1) {
			fsimg_fd_release(writer->ctx, -1);
			status = fsimg_error_set(error,
			    saved == ENOENT || saved == ELOOP || saved == ENOTDIR ?
			    FSIMG_ESOURCE_CHANGED : FSIMG_EIO, saved,
			    FSIMG_OP_SOURCE, "source component cannot be reopened");
			break;
		}
		if (fsimg_host_file(fd, &opened) == -1) {
			saved = errno;
			fsimg_fd_release(writer->ctx, fd);
			status = fsimg_error_set(error, FSIMG_EIO, saved,
			    FSIMG_OP_SOURCE, "reopened source inspection failed");
			break;
		}
		status = fsimg_source_check_object(writer, fd, &opened, root, error);
		if (status != FSIMG_OK) {
			fsimg_fd_release(writer->ctx, fd);
			break;
		}
		if (separator == NULL) {
			if (!fsimg_source_identity_equal(&opened, &source->identity)) {
				fsimg_fd_release(writer->ctx, fd);
				status = fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
				    FSIMG_OP_SOURCE, "reopened source differs from its recorded identity");
				break;
			}
			source->fd = fd;
			return (fsimg_io_adopt_fd(writer->ctx, fd, FSIMG_IO_READ,
			    &source->io, error));
		}
		parent = fd;
		parent_owned = true;
		component = separator + 1;
	}
	if (parent_owned)
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
	status = source_validate(source, error);
	if (status != FSIMG_OK)
		return (source_abort(source, status));
	if (source->storage == FSIMG_STORAGE_ZERO) {
		bzero(buffer, remaining);
		*transferred = remaining;
	} else {
		status = fsimg_io_read_exact(source->io, offset, buffer, remaining,
		    transferred, error);
		if (status != FSIMG_OK)
			return (source_abort(source, status));
	}
	status = source_validate(source, error);
	if (status != FSIMG_OK)
		return (source_abort(source, status));
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
	fsimg_source_close(writer);
	if (status != FSIMG_OK)
		return (fsimg_writer_fail(source->writer, status));
	return (FSIMG_OK);
}
