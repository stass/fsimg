/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <strings.h>
#include <unistd.h>

#include "host.h"
#include "internal.h"
#include "io.h"

struct fsimg_io {
	fsimg_ctx_t	*ctx;
	fsimg_session_t	*session;
	fsimg_io_t	*root;
	uint64_t	 base;
	uint64_t	 length;
	unsigned	 access;
	int		 fd;
	fsimg_io_ops_t	 ops;
	void		*argument;
	/* An internal resolver: resolve supplies every byte, release frees its
	 * argument with the object, and backing is the file whose identity it
	 * carries for alias and stability checks.
	 */
	fsimg_io_resolve_t resolve;
	void		(*release)(void *);
	fsimg_io_t	*backing;
	bool		 known_zero;
	bool		 unsized;	/* scratch: length is only the host range */
	/* Synced since the object was made and since its last write or
	 * resize, so the host's hole reports describe what was written.
	 */
	bool		 holes_synced;
};

static fsimg_status_t io_range(fsimg_io_t *, uint64_t, uint64_t,
    fsimg_error_t *);
static fsimg_status_t io_zero(fsimg_io_t *, fsimg_session_t *, uint64_t,
    uint64_t, fsimg_error_t *);
static fsimg_status_t io_transfer(fsimg_io_t *, uint64_t, void *, size_t,
    size_t *, bool, bool, fsimg_session_t *, fsimg_error_t *);
static fsimg_status_t adapter_transfer(fsimg_io_t *, uint64_t, void *,
    size_t, size_t *, bool, fsimg_error_t *);
static fsimg_status_t callback_result(fsimg_status_t, fsimg_error_t *);
static fsimg_status_t io_from_fd(fsimg_ctx_t *, int, unsigned, bool,
    fsimg_io_t **, fsimg_error_t *);

void
fsimg_io_bind_session(fsimg_io_t *io, fsimg_session_t *session)
{
	io->root->session = session;
}

fsimg_ctx_t *
fsimg_io_context(const fsimg_io_t *io)
{
	return (io == NULL ? NULL : io->ctx);
}

fsimg_status_t
fsimg_io_check_output(fsimg_io_t *io, uint64_t length, bool durable,
    bool *resize, fsimg_error_t *error)
{
	uint64_t current;
	fsimg_status_t status;

	*resize = false;
	status = fsimg_io_get_size(io, &current, error);
	if (status != FSIMG_OK)
		return (status);
	if ((io->access & FSIMG_IO_WRITE) == 0 ||
	    (durable && (io->root->ops.flags & FSIMG_IO_DURABLE) == 0))
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IO, "sink lacks required write or durability capability"));
	*resize = io->root == io && (io->fd != -1 || io->ops.resize != NULL);
	if (!*resize && current != length)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "fixed sink length differs from planned volume"));
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_io_identity(fsimg_io_t *io, fsimg_host_file_t *identity, bool *known,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	*known = false;
	bzero(identity, sizeof(*identity));
	status = io_range(io, 0, 0, error);
	if (status != FSIMG_OK)
		return (status);
	if (io->root->backing != NULL)
		return (fsimg_io_identity(io->root->backing, identity, known, error));
	if (io->root->fd == -1)
		return (FSIMG_OK);
	if (fsimg_host_file(io->root->fd, identity) == -1)
		return (fsimg_error_set(error, FSIMG_EIO, errno,
		    FSIMG_OP_IO, "image identity query failed"));
	*known = true;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_io_adopt_empty_output(fsimg_ctx_t *ctx, int fd, fsimg_io_t **out,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	status = fsimg_io_adopt_fd(ctx, fd, FSIMG_IO_WRITE, out, error);
	if (status != FSIMG_OK)
		return (status);
	if ((*out)->length != 0) {
		fsimg_io_free(*out);
		*out = NULL;
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "fresh output requires an empty private file"));
	}
	(*out)->known_zero = true;
	return (FSIMG_OK);
}

bool
fsimg_io_proven_zero(const fsimg_io_t *io)
{
	return (io->root->known_zero);
}

/*
 * A handed-over file: the descriptor is adopted on every outcome, and
 * accepted only as what its creator promised, checked on the descriptor
 * itself before any byte moves: a regular file, empty, with no name left
 * to reach it (link count zero), open for reading and writing without
 * O_APPEND, which would move every write to the end. Those facts make its
 * unwritten ranges proven zero.
 */
fsimg_status_t
fsimg_io_adopt_handed_over(fsimg_ctx_t *ctx, int fd, const char *role,
    fsimg_io_t **out, fsimg_error_t *error)
{
	struct stat st;
	fsimg_status_t status;
	int flags;

	*out = NULL;
	flags = fcntl(fd, F_GETFL);
	if (flags == -1 || fstat(fd, &st) == -1 || !S_ISREG(st.st_mode) ||
	    st.st_size != 0 || st.st_nlink != 0 ||
	    (flags & O_ACCMODE) != O_RDWR || (flags & O_APPEND) != 0) {
		close(fd);
		fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IO,
		    "a handed-over file must be an empty, unlinked, read/write "
		    "regular file");
		fsimg_error_path(error, role);
		return (FSIMG_EINVAL);
	}
	status = fsimg_fd_reserve(ctx, error);
	if (status != FSIMG_OK) {
		close(fd);
		return (status);
	}
	status = fsimg_io_adopt_fd(ctx, fd, FSIMG_IO_READ | FSIMG_IO_WRITE, out,
	    error);
	if (status != FSIMG_OK)
		return (status);
	(*out)->known_zero = true;
	return (FSIMG_OK);
}

/*
 * The host is asked about holes only once the file is synced, first when
 * the object makes its first lookup, since a file may arrive dirty, and
 * again after any write through the object: OpenZFS 2.2.0 and 2.2.1
 * could report dirty, unsynced data as a hole, which a caller would read
 * as zeros.
 */
fsimg_status_t
fsimg_io_next_hole(fsimg_io_t *io, uint64_t offset, uint64_t *hole_start,
    uint64_t *hole_end, fsimg_error_t *error)
{
	fsimg_io_t *root = io->root;
	fsimg_status_t status;

	*hole_start = *hole_end = io->length;
	if (root->fd == -1 || offset >= io->length)
		return (FSIMG_OK);
	if (!root->holes_synced) {
		status = fsimg_io_flush(io, false, error);
		if (status != FSIMG_OK)
			return (status);
		root->holes_synced = true;
	}
	fsimg_host_next_hole(root->fd, io->base + offset, io->base + io->length,
	    hole_start, hole_end);
	*hole_start -= io->base;
	*hole_end -= io->base;
	return (FSIMG_OK);
}

void
fsimg_io_ops_init(fsimg_io_ops_t *ops)
{
	bzero(ops, sizeof(*ops));
	ops->size = sizeof(*ops);
	ops->version = FSIMG_OPTIONS_VERSION;
	ops->max_transfer = 64 * 1024;
}

static fsimg_status_t
callback_result(fsimg_status_t status, fsimg_error_t *error)
{
	if (status == FSIMG_OK) {
		fsimg_error_clear(error);
		return (FSIMG_OK);
	}
	if (status > FSIMG_OK || status < FSIMG_EDURABILITY)
		return (fsimg_error_set(error, FSIMG_EIO, 0,
		    FSIMG_OP_IO, "invalid callback status"));
	/* A callback may return only a status, even when details were requested. */
	if (error != NULL && error->status != status)
		fsimg_error_set(error, status, 0, FSIMG_OP_IO,
		    "I/O callback failed");
	return (status);
}

fsimg_status_t
fsimg_io_from_callbacks(fsimg_ctx_t *ctx, const fsimg_io_ops_t *ops,
    void *argument, fsimg_io_t **out, fsimg_error_t *error)
{
	fsimg_error_t detail;
	fsimg_io_t *io;
	uint64_t length;
	fsimg_status_t status;

	if (out != NULL)
		*out = NULL;
	status = fsimg_ctx_ready(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL || ops == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "missing callback table or output"));
	if (ops->size != sizeof(*ops))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "unsupported callback table size"));
	if (ops->version != FSIMG_OPTIONS_VERSION ||
	    (ops->flags & ~FSIMG_IO_DURABLE) != 0 || ops->get_size == NULL ||
	    (ops->read_at == NULL && ops->write_at == NULL) ||
	    (ops->write_at == NULL && (ops->resize != NULL ||
	    ops->zero_range != NULL)) ||
	    ((ops->flags & FSIMG_IO_DURABLE) != 0 && ops->flush == NULL))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "inconsistent I/O capabilities"));
	for (size_t i = 0; i < 4; i++) {
		if (ops->reserved[i] != 0)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_IO, "nonzero reserved I/O field"));
	}
	io = fsimg_alloc(ctx, sizeof(*io), &detail);
	if (io == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	length = 0;
	fsimg_error_clear(&detail);
	ctx->in_callback = true;
	status = ops->get_size(argument, &length, &detail);
	ctx->in_callback = false;
	status = callback_result(status, &detail);
	if (status != FSIMG_OK) {
		fsimg_dealloc(io);
		if (error != NULL)
			*error = detail;
		return (status);
	}
	io->ctx = ctx;
	io->root = io;
	io->fd = -1;
	io->length = length;
	io->ops = *ops;
	if (io->ops.max_transfer == 0 || io->ops.max_transfer > SSIZE_MAX)
		io->ops.max_transfer = SSIZE_MAX;
	io->argument = argument;
	if (ops->read_at != NULL)
		io->access |= FSIMG_IO_READ;
	if (ops->write_at != NULL)
		io->access |= FSIMG_IO_WRITE;
	*out = io;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_io_resolver(fsimg_ctx_t *ctx, fsimg_io_t *backing, uint64_t length,
    fsimg_io_resolve_t resolve, void (*release)(void *), void *argument,
    fsimg_io_t **out, fsimg_error_t *error)
{
	fsimg_io_t *io;

	*out = NULL;
	io = fsimg_alloc(ctx, sizeof(*io), error);
	if (io == NULL) {
		release(argument);
		return (error->status);
	}
	bzero(io, sizeof(*io));
	io->ctx = ctx;
	io->root = io;
	io->fd = -1;
	io->length = length;
	io->access = FSIMG_IO_READ;
	io->ops.max_transfer = 64 * 1024;
	io->resolve = resolve;
	io->release = release;
	io->argument = argument;
	io->backing = backing;
	*out = io;
	return (FSIMG_OK);
}

static fsimg_status_t
io_from_fd(fsimg_ctx_t *ctx, int fd, unsigned access, bool reserved,
    fsimg_io_t **out, fsimg_error_t *error)
{
	fsimg_error_t detail;
	fsimg_io_t *io;
	struct stat st;
	fsimg_status_t status;
	int duplicate, flags, mode, saved;

	if (out != NULL)
		*out = NULL;
	status = fsimg_ctx_ready(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL || access == 0 ||
	    (access & ~(FSIMG_IO_READ | FSIMG_IO_WRITE)) != 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "invalid descriptor access or output"));
	flags = fcntl(fd, F_GETFL);
	if (flags == -1)
		return (fsimg_error_set(error, FSIMG_EIO, errno,
		    FSIMG_OP_IO, "descriptor flags unavailable"));
	mode = flags & O_ACCMODE;
	if (((access & FSIMG_IO_READ) != 0 && mode == O_WRONLY) ||
	    ((access & FSIMG_IO_WRITE) != 0 && mode == O_RDONLY) ||
	    (access == FSIMG_IO_READ && mode != O_RDONLY) ||
	    ((access & FSIMG_IO_WRITE) != 0 && (flags & O_APPEND) != 0) ||
	    fsimg_host_unsupported_io(flags))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "descriptor flags incompatible with requested access"));
	if (fstat(fd, &st) == -1)
		return (fsimg_error_set(error, FSIMG_EIO, errno,
		    FSIMG_OP_IO, "descriptor stat failed"));
	if (!S_ISREG(st.st_mode) || st.st_size < 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "descriptor is not a regular file"));
	io = fsimg_alloc(ctx, sizeof(*io), &detail);
	if (io == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	duplicate = fd;
	if (!reserved) {
		status = fsimg_fd_reserve(ctx, error);
		if (status != FSIMG_OK) {
			fsimg_dealloc(io);
			return (status);
		}
		duplicate = fsimg_host_dup(fd);
		if (duplicate == -1) {
			saved = errno;
			fsimg_fd_release(ctx, -1);
			fsimg_dealloc(io);
			return (fsimg_error_set(error, FSIMG_EIO, saved,
			    FSIMG_OP_IO, "descriptor duplication failed"));
		}
	}
	io->ctx = ctx;
	io->root = io;
	io->fd = duplicate;
	io->access = access;
	io->length = (uint64_t)st.st_size;
	io->ops.max_transfer = 64 * 1024;
	io->ops.flags = FSIMG_IO_DURABLE;
	*out = io;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_io_from_fd(fsimg_ctx_t *ctx, int fd, unsigned access, fsimg_io_t **out,
    fsimg_error_t *error)
{
	return (io_from_fd(ctx, fd, access, false, out, error));
}

fsimg_status_t
fsimg_io_adopt_fd(fsimg_ctx_t *ctx, int fd, unsigned access, fsimg_io_t **out,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	status = io_from_fd(ctx, fd, access, true, out, error);
	if (status != FSIMG_OK)
		fsimg_fd_release(ctx, fd);
	return (status);
}

fsimg_status_t
fsimg_io_adopt_scratch(fsimg_ctx_t *ctx, int fd, fsimg_io_t **out,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	status = fsimg_io_adopt_fd(ctx, fd, FSIMG_IO_READ | FSIMG_IO_WRITE, out,
	    error);
	if (status != FSIMG_OK)
		return (status);
	(*out)->length = INT64_MAX;
	(*out)->unsized = true;
	return (FSIMG_OK);
}

static fsimg_status_t
io_range(fsimg_io_t *io, uint64_t offset, uint64_t length,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	status = fsimg_ctx_ready(io == NULL ? NULL : io->ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (!fsimg_range_fits(offset, length, io->length) ||
	    !fsimg_range_fits(io->base, io->length, io->root->length))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_IO, "request outside I/O object"));
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_io_slice(fsimg_io_t *parent, uint64_t offset, uint64_t length,
    fsimg_io_t **out, fsimg_error_t *error)
{
	fsimg_error_t detail;
	fsimg_io_t *slice;
	fsimg_status_t status;

	if (out != NULL)
		*out = NULL;
	status = io_range(parent, offset, length, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "missing slice output"));
	if (parent->root->unsized)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IO, "scratch object cannot be sliced"));
	slice = fsimg_alloc(parent->ctx, sizeof(*slice), &detail);
	if (slice == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	/* Flatten translation, so nested slices never recurse during I/O. */
	slice->ctx = parent->ctx;
	slice->root = parent->root;
	slice->base = parent->base + offset;
	slice->length = length;
	slice->access = parent->access;
	slice->fd = -1;
	*out = slice;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_io_read_slice(fsimg_io_t *parent, uint64_t offset, uint64_t length,
    fsimg_io_t **out, fsimg_error_t *error)
{
	fsimg_status_t status;

	status = fsimg_io_slice(parent, offset, length, out, error);
	if (status == FSIMG_OK)
		(*out)->access &= ~(unsigned)(FSIMG_IO_WRITE | FSIMG_IO_DURABLE);
	return (status);
}

fsimg_status_t
fsimg_io_close_output(fsimg_io_t *io, fsimg_error_t *error)
{
	int result, saved;

	result = close(io->fd);
	saved = errno;
	fsimg_fd_release(io->ctx, -1);
	fsimg_dealloc(io);
	if (result == -1)
		return (fsimg_error_set(error, FSIMG_EIO, saved,
		    FSIMG_OP_IO, "output file close failed"));
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_io_set_times(fsimg_io_t *io, const struct timespec *times,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	status = io_range(io, 0, 0, error);
	if (status != FSIMG_OK)
		return (status);
	if (io->root != io || io->fd == -1 || (io->access & FSIMG_IO_WRITE) == 0)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_EXTRACT, "timestamp restoration requires an owned output file"));
	if (futimens(io->fd, times) == -1)
		return (fsimg_error_set(error, FSIMG_EIO, errno,
		    FSIMG_OP_EXTRACT, "output file timestamp restoration failed"));
	return (FSIMG_OK);
}

void
fsimg_io_free(fsimg_io_t *io)
{
	if (io == NULL || io->ctx->in_callback)
		return;
	if (io->fd != -1)
		fsimg_fd_release(io->ctx, io->fd);
	if (io->root == io && io->release != NULL)
		io->release(io->argument);
	fsimg_dealloc(io);
}

fsimg_status_t
fsimg_io_get_size(fsimg_io_t *io, uint64_t *out, fsimg_error_t *error)
{
	fsimg_status_t status;

	if (out != NULL)
		*out = 0;
	status = io_range(io, 0, 0, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "missing size output"));
	if (io->root->unsized)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IO, "scratch object has no size"));
	*out = io->length;
	return (FSIMG_OK);
}

static fsimg_status_t
adapter_transfer(fsimg_io_t *root, uint64_t offset, void *buffer,
    size_t length, size_t *transferred, bool writing, fsimg_error_t *error)
{
	off_t host_offset;
	ssize_t count;
	fsimg_status_t status;

	*transferred = 0;
	fsimg_error_clear(error);
	/* Once any write is attempted, even a failing one, unwritten ranges no
	 * longer describe the whole object. Reused sinks always take real zeroing.
	 */
	if (writing) {
		root->known_zero = false;
		root->holes_synced = false;
	}
	if (root->resolve != NULL)
		return (root->resolve(root->argument, offset, buffer, length,
		    transferred, error));
	if (root->fd != -1) {
		if (!fsimg_host_offset(offset, &host_offset))
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_IO, "offset exceeds host range"));
		if (writing)
			count = pwrite(root->fd, buffer, length, host_offset);
		else
			count = pread(root->fd, buffer, length, host_offset);
		if (count == -1)
			return (fsimg_error_set(error, FSIMG_EIO, errno,
			    FSIMG_OP_IO, "positional I/O failed"));
		*transferred = (size_t)count;
		return (FSIMG_OK);
	}
	root->ctx->in_callback = true;
	if (writing)
		status = root->ops.write_at(root->argument, offset, buffer,
		    length, transferred, error);
	else
		status = root->ops.read_at(root->argument, offset, buffer,
		    length, transferred, error);
	root->ctx->in_callback = false;
	if (*transferred > length) {
		*transferred = 0;
		return (fsimg_error_set(error, FSIMG_EIO, 0,
		    FSIMG_OP_IO, "callback over-reported transfer"));
	}
	return (callback_result(status, error));
}

static fsimg_status_t
io_transfer(fsimg_io_t *io, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, bool writing, bool exact, fsimg_session_t *session,
    fsimg_error_t *error)
{
	fsimg_error_t detail;
	fsimg_progress_t progress;
	fsimg_io_t *root;
	size_t batch, count, done;
	fsimg_status_t status;
	unsigned interruptions;

	if (transferred != NULL)
		*transferred = 0;
	status = io_range(io, offset, length, error);
	if (status != FSIMG_OK)
		return (status);
	if (transferred == NULL || (buffer == NULL && length != 0))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "missing transfer output or buffer"));
	if ((io->access & (writing ? FSIMG_IO_WRITE : FSIMG_IO_READ)) == 0)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IO, "I/O access not supported"));
	if (length == 0)
		return (FSIMG_OK);

	root = io->root;
	if (session == NULL)
		session = root->session;
	done = 0;
	interruptions = 0;
	bzero(&progress, sizeof(progress));
	progress.phase = writing ? "write" : "read";
	while (done < length) {
		progress.bytes = done;
		status = fsimg_poll(io->ctx, &progress, &detail);
		if (status != FSIMG_OK)
			break;
		batch = length - done;
		if (batch > root->ops.max_transfer)
			batch = root->ops.max_transfer;
		if (session != NULL) {
			status = fsimg_session_charge(session, 0, 0, 1, &detail);
			if (status != FSIMG_OK)
				break;
		}
		status = adapter_transfer(root, io->base + offset + done,
		    (unsigned char *)buffer + done, batch, &count, writing,
		    &detail);
		fsimg_stat_add(&io->ctx->stats.io_calls, 1);
		done += count;
		if (writing)
			fsimg_stat_add(&io->ctx->stats.written_bytes, count);
		else
			fsimg_stat_add(&io->ctx->stats.read_bytes, count);
		if (status == FSIMG_EIO && detail.os_error == EINTR) {
			/* Confirmed progress survives interruption; never replay it. */
			if (count != 0)
				interruptions = 0;
			else if (++interruptions > 1024)
				break;
			status = FSIMG_OK;
			fsimg_error_clear(&detail);
			continue;
		}
		if (status != FSIMG_OK)
			break;
		interruptions = 0;
		if (count == 0) {
			if (writing || exact)
				status = fsimg_error_set(&detail, FSIMG_EIO, 0,
				    FSIMG_OP_IO, "unexpected EOF or zero progress");
			break;
		}
		if (!exact)
			break;
	}
	*transferred = done;
	if (status != FSIMG_OK) {
		if (error != NULL) {
			*error = detail;
			error->offset = offset + done;
			error->offset_valid = true;
			if (writing && done != 0)
				error->output_state = FSIMG_OUTPUT_INCOMPLETE;
		}
		return (status);
	}
	fsimg_error_clear(error);
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_io_read_at(fsimg_io_t *io, uint64_t offset, void *buffer, size_t length,
    size_t *transferred, fsimg_error_t *error)
{
	return (io_transfer(io, offset, buffer, length, transferred, false,
	    false, NULL, error));
}

fsimg_status_t
fsimg_io_write_at(fsimg_io_t *io, uint64_t offset, const void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error)
{
	return (io_transfer(io, offset, (void *)buffer, length, transferred,
	    true, false, NULL, error));
}

fsimg_status_t
fsimg_io_read_exact(fsimg_io_t *io, uint64_t offset, void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error)
{
	return (io_transfer(io, offset, buffer, length, transferred, false,
	    true, NULL, error));
}

fsimg_status_t
fsimg_io_write_exact(fsimg_io_t *io, uint64_t offset, const void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error)
{
	return (io_transfer(io, offset, (void *)buffer, length, transferred,
	    true, true, NULL, error));
}

fsimg_status_t
fsimg_io_read_session(fsimg_io_t *io, fsimg_session_t *session,
    uint64_t offset, void *buffer, size_t length, size_t *transferred,
    fsimg_error_t *error)
{
	return (io_transfer(io, offset, buffer, length, transferred, false,
	    true, session, error));
}

fsimg_status_t
fsimg_io_write_session(fsimg_io_t *io, fsimg_session_t *session,
    uint64_t offset, const void *buffer, size_t length, size_t *transferred,
    fsimg_error_t *error)
{
	return (io_transfer(io, offset, (void *)buffer, length, transferred, true,
	    true, session, error));
}

fsimg_status_t
fsimg_io_resize(fsimg_io_t *io, uint64_t length, fsimg_error_t *error)
{
	off_t host_length;
	fsimg_status_t status;

	status = io_range(io, 0, 0, error);
	if (status != FSIMG_OK)
		return (status);
	if (io->root != io || (io->access & FSIMG_IO_WRITE) == 0 ||
	    io->root->unsized || (io->fd == -1 && io->ops.resize == NULL))
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IO, "I/O object cannot resize"));
	status = fsimg_poll(io->ctx, NULL, error);
	if (status != FSIMG_OK)
		return (status);
	if (io->fd != -1) {
		if (!fsimg_host_offset(length, &host_length))
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_IO, "length exceeds host range"));
		fsimg_stat_add(&io->ctx->stats.io_calls, 1);
		io->holes_synced = false;
		if (ftruncate(io->fd, host_length) == -1)
			return (fsimg_error_set(error, FSIMG_EIO, errno,
			    FSIMG_OP_IO, "resize failed"));
	} else {
		io->ctx->in_callback = true;
		status = io->ops.resize(io->argument, length, error);
		io->ctx->in_callback = false;
		status = callback_result(status, error);
		fsimg_stat_add(&io->ctx->stats.io_calls, 1);
		if (status != FSIMG_OK)
			return (status);
	}
	io->length = length;
	return (FSIMG_OK);
}

static fsimg_status_t
io_zero(fsimg_io_t *io, fsimg_session_t *session, uint64_t offset,
    uint64_t length, fsimg_error_t *error)
{
	fsimg_error_t detail;
	fsimg_io_t *root;
	unsigned char *zeros;
	uint64_t done;
	size_t batch, transferred;
	fsimg_status_t status;
	bool mutation_attempted;

	status = io_range(io, offset, length, error);
	if (status != FSIMG_OK)
		return (status);
	if ((io->access & FSIMG_IO_WRITE) == 0)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IO, "I/O object is not writable"));
	if (length == 0)
		return (FSIMG_OK);
	root = io->root;
	if (session == NULL)
		session = root->session;
	if (root->known_zero) {
		if (session != NULL)
			return (fsimg_session_charge(session, 0, 0, 1, error));
		return (fsimg_poll(io->ctx, NULL, error));
	}
	zeros = NULL;
	if (root->ops.zero_range == NULL) {
		zeros = fsimg_alloc(io->ctx, 64 * 1024, &detail);
		if (zeros == NULL) {
			if (error != NULL)
				*error = detail;
			return (detail.status);
		}
	}
	done = 0;
	mutation_attempted = false;
	while (done < length) {
		batch = length - done > 64 * 1024 ? 64 * 1024 :
		    (size_t)(length - done);
		status = fsimg_poll(io->ctx, NULL, error);
		if (status != FSIMG_OK)
			break;
		mutation_attempted = true;
		if (zeros != NULL) {
			status = io_transfer(io, offset + done, zeros, batch,
			    &transferred, true, true, session, error);
		} else {
			if (session != NULL) {
				status = fsimg_session_charge(session, 0, 0, 1, error);
				if (status != FSIMG_OK)
					break;
			}
			io->ctx->in_callback = true;
			status = root->ops.zero_range(root->argument,
			    io->base + offset + done, batch, error);
			io->ctx->in_callback = false;
			status = callback_result(status, error);
			fsimg_stat_add(&io->ctx->stats.io_calls, 1);
			if (status == FSIMG_OK)
				fsimg_stat_add(&io->ctx->stats.written_bytes, batch);
		}
		if (status != FSIMG_OK)
			break;
		done += batch;
	}
	fsimg_dealloc(zeros);
	if (status != FSIMG_OK && mutation_attempted && error != NULL)
		error->output_state = FSIMG_OUTPUT_INCOMPLETE;
	return (status);
}

fsimg_status_t
fsimg_io_zero_range(fsimg_io_t *io, uint64_t offset, uint64_t length,
    fsimg_error_t *error)
{
	return (io_zero(io, NULL, offset, length, error));
}

fsimg_status_t
fsimg_io_zero_session(fsimg_io_t *io, fsimg_session_t *session,
    uint64_t offset, uint64_t length, fsimg_error_t *error)
{
	return (io_zero(io, session, offset, length, error));
}

fsimg_status_t
fsimg_io_flush(fsimg_io_t *io, bool durable, fsimg_error_t *error)
{
	fsimg_error_t detail;
	fsimg_io_t *root;
	fsimg_status_t status;

	status = io_range(io, 0, 0, error);
	if (status != FSIMG_OK)
		return (status);
	root = io->root;
	if (durable && (root->ops.flags & FSIMG_IO_DURABLE) == 0)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IO, "durable flush not supported"));
	for (unsigned interruptions = 0;; interruptions++) {
		status = fsimg_poll(io->ctx, NULL, &detail);
		if (status != FSIMG_OK)
			break;
		fsimg_error_clear(&detail);
		if (root->fd != -1) {
			if (fsync(root->fd) == 0)
				return (FSIMG_OK);
			status = fsimg_error_set(&detail, FSIMG_EIO, errno,
			    FSIMG_OP_IO, "flush failed");
		} else {
			if (root->ops.flush == NULL)
				return (FSIMG_OK);
			io->ctx->in_callback = true;
			status = root->ops.flush(root->argument, &detail);
			io->ctx->in_callback = false;
			status = callback_result(status, &detail);
		}
		if (status != FSIMG_EIO || detail.os_error != EINTR ||
		    interruptions == 1024)
			break;
	}
	if (error != NULL)
		*error = detail;
	return (status);
}
