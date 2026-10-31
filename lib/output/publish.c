/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "host.h"

#include "internal.h"
#include "publish.h"

static bool valid_basename(const char *);
static int host_sync(void *, int);
static int host_link(void *, int, const char *, int, const char *);
static int host_unlink(void *, int, const char *);
static int host_rename(void *, int, const char *, int, const char *);
static bool links_unsupported(int);
static fsimg_status_t naming_error(fsimg_error_t *, int);
static fsimg_status_t settle_rename_failure(const struct stat *, int,
    const char *, int, const char *, int, fsimg_error_t *);
static fsimg_status_t publication_error(fsimg_error_t *, fsimg_status_t,
    int, fsimg_output_state_t, const char *);
static fsimg_status_t sync_output(fsimg_ctx_t *, const fsimg_publish_ops_t *,
    int, fsimg_error_t *);

static bool
valid_basename(const char *name)
{
	return (name != NULL && name[0] != '\0' &&
	    strcmp(name, ".") != 0 && strcmp(name, "..") != 0 &&
	    strchr(name, '/') == NULL);
}

static int
host_sync(void *argument FSIMG_UNUSED, int fd)
{
	return (fsync(fd));
}

static int
host_link(void *argument FSIMG_UNUSED, int source_dir, const char *source,
    int target_dir, const char *target)
{
	return (linkat(source_dir, source, target_dir, target, 0));
}

static int
host_unlink(void *argument FSIMG_UNUSED, int parent, const char *name)
{
	return (unlinkat(parent, name, 0));
}

static int
host_rename(void *argument FSIMG_UNUSED, int source_dir, const char *source,
    int target_dir, const char *target)
{
	return (fsimg_host_rename_exclusive(source_dir, source, target_dir,
	    target));
}

/* The link refusals of a filesystem without hard links, such as FAT and
 * exFAT: EPERM on Linux, ENOTSUP on macOS, EOPNOTSUPP on FreeBSD. On a
 * local filesystem each is a definite refusal; nothing was linked.
 */
static bool
links_unsupported(int error)
{
	return (error == EPERM || error == ENOTSUP || error == EOPNOTSUPP);
}

/* Classify a failed link or no-replace rename of the staged name. An
 * existing target was left alone, and a primitive the host or filesystem
 * lacks, or an invalid request, did nothing (EINVAL is both: Linux reports
 * an unsupported rename flag with it); any other failure, such as an
 * interrupted remote operation, may have taken effect.
 */
static fsimg_status_t
naming_error(fsimg_error_t *error, int saved)
{
	if (saved == EEXIST)
		return (publication_error(error, FSIMG_EEXISTS, saved,
		    FSIMG_OUTPUT_ABSENT, "destination already exists"));
	if (saved == EXDEV || saved == ENOTSUP || saved == EOPNOTSUPP ||
	    saved == ENOSYS || saved == EINVAL)
		return (publication_error(error, FSIMG_EUNSUPPORTED, saved,
		    FSIMG_OUTPUT_ABSENT, "no-replace publication unavailable"));
	return (publication_error(error, FSIMG_EIO, saved,
	    FSIMG_OUTPUT_UNCERTAIN, "publication outcome uncertain"));
}

/* A failed rename that may have taken effect, which naming_error reports
 * as FSIMG_EIO, is settled by the names: the private staged name gone and
 * the final name on the staged inode mean the rename happened, and the
 * output is published with nothing synced; anything else stays uncertain,
 * since a remote rename can still be in flight.
 */
static fsimg_status_t
settle_rename_failure(const struct stat *staged, int staging_fd,
    const char *staged_name, int parent_fd, const char *final_name, int saved,
    fsimg_error_t *error)
{
	struct stat st;
	fsimg_status_t status;

	status = naming_error(error, saved);
	if (status != FSIMG_EIO)
		return (status);
	if (fstatat(staging_fd, staged_name, &st, AT_SYMLINK_NOFOLLOW) == -1 &&
	    errno == ENOENT &&
	    fstatat(parent_fd, final_name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
	    st.st_dev == staged->st_dev && st.st_ino == staged->st_ino)
		return (publication_error(error, FSIMG_EDURABILITY, saved,
		    FSIMG_OUTPUT_PUBLISHED,
		    "publication rename failed after taking effect"));
	return (publication_error(error, FSIMG_EIO, saved,
	    FSIMG_OUTPUT_UNCERTAIN, "publication rename outcome uncertain"));
}

static fsimg_status_t
publication_error(fsimg_error_t *error, fsimg_status_t status, int os_error,
    fsimg_output_state_t output_state, const char *message)
{
	fsimg_error_set(error, status, os_error, FSIMG_OP_PUBLISH, message);
	if (error != NULL)
		error->output_state = output_state;
	return (status);
}

static fsimg_status_t
sync_output(fsimg_ctx_t *ctx, const fsimg_publish_ops_t *hooks, int fd,
    fsimg_error_t *error)
{
	fsimg_status_t status;
	int result, saved;

	for (unsigned interruptions = 0;; interruptions++) {
		status = fsimg_poll(ctx, NULL, error);
		if (status != FSIMG_OK)
			return (status);
		ctx->in_callback = true;
		result = hooks->sync_fd(hooks->argument, fd);
		saved = errno;
		ctx->in_callback = false;
		if (result == 0)
			return (FSIMG_OK);
		if (saved != EINTR || interruptions == 1024)
			return (publication_error(error, FSIMG_EIO, saved,
			    FSIMG_OUTPUT_ABSENT, "output flush failed"));
	}
}

fsimg_status_t
fsimg_publish(fsimg_ctx_t *ctx, int staging_fd, const char *staged_name,
    uint64_t expected_size, int parent_fd, const char *final_name,
    const fsimg_publish_ops_t *hooks, fsimg_error_t *error)
{
	const fsimg_publish_ops_t host = {
		.sync_fd = host_sync,
		.link_at = host_link,
		.unlink_at = host_unlink,
		.rename_at = host_rename
	};
	struct stat st;
	fsimg_status_t status;
	int fd, result, saved;
	bool by_rename;

	status = fsimg_ctx_ready(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (!valid_basename(staged_name) || !valid_basename(final_name))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_PUBLISH, "publication requires single basenames"));
	if (hooks == NULL)
		hooks = &host;
	if (hooks->sync_fd == NULL || hooks->link_at == NULL ||
	    hooks->unlink_at == NULL || hooks->rename_at == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_PUBLISH, "incomplete publication hooks"));
	status = fsimg_poll(ctx, NULL, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_fd_reserve(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	fd = openat(staging_fd, staged_name,
	    O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY);
	if (fd == -1) {
		saved = errno;
		fsimg_fd_release(ctx, -1);
		return (publication_error(error, FSIMG_EIO, saved,
		    FSIMG_OUTPUT_ABSENT, "staged result open failed"));
	}
	if (fsimg_host_fstat(fd, &st) == -1) {
		status = publication_error(error, FSIMG_EIO, errno,
		    FSIMG_OUTPUT_ABSENT, "staged result stat failed");
		goto close_file;
	}
	if (!S_ISREG(st.st_mode) || st.st_size < 0 ||
	    (uint64_t)st.st_size != expected_size) {
		status = publication_error(error, FSIMG_EINVAL, 0,
		    FSIMG_OUTPUT_ABSENT, "unexpected staged result type or size");
		goto close_file;
	}
	status = sync_output(ctx, hooks, fd, error);
	if (status != FSIMG_OK)
		goto close_file;
	/* Close errors must be observed before a final pathname is created. */
	result = close(fd);
	saved = errno;
	fd = -1;
	fsimg_fd_release(ctx, -1);
	if (result == -1)
		return (publication_error(error, FSIMG_EIO, saved,
		    FSIMG_OUTPUT_ABSENT, "staged result close failed"));

	/* Flush and close can take time. Observe cancellation once more at the
	 * publication boundary; after linking, failures describe published output.
	 */
	status = fsimg_poll(ctx, NULL, error);
	if (status != FSIMG_OK)
		return (status);

	/*
	 * Give the staged inode its final name without replacing anything: a
	 * hard link, then removal of the staged name, or, where the filesystem
	 * has no hard links and refused the link outright, one no-replace
	 * rename, which consumes the staged name itself. No other link failure
	 * falls back, since it may have taken effect.
	 */
	ctx->in_callback = true;
	result = hooks->link_at(hooks->argument, staging_fd, staged_name,
	    parent_fd, final_name);
	saved = errno;
	by_rename = result == -1 && links_unsupported(saved);
	if (by_rename) {
		result = hooks->rename_at(hooks->argument, staging_fd,
		    staged_name, parent_fd, final_name);
		saved = errno;
	}
	ctx->in_callback = false;
	if (result == -1 && by_rename)
		return (settle_rename_failure(&st, staging_fd, staged_name,
		    parent_fd, final_name, saved, error));
	if (result == -1)
		return (naming_error(error, saved));
	if (!by_rename) {
		ctx->in_callback = true;
		result = hooks->unlink_at(hooks->argument, staging_fd, staged_name);
		saved = errno;
		ctx->in_callback = false;
		if (result == -1)
			return (publication_error(error, FSIMG_EDURABILITY, saved,
			    FSIMG_OUTPUT_PUBLISHED,
			    "published output staging cleanup failed"));
	}
	/* Neither primitive's success shows that directories can be synced. */
	status = sync_output(ctx, hooks, staging_fd, error);
	if (status != FSIMG_OK)
		return (publication_error(error, FSIMG_EDURABILITY,
		    error == NULL ? 0 : error->os_error,
		    FSIMG_OUTPUT_PUBLISHED, "staging directory sync incomplete"));
	status = sync_output(ctx, hooks, parent_fd, error);
	if (status != FSIMG_OK)
		return (publication_error(error, FSIMG_EDURABILITY,
		    error == NULL ? 0 : error->os_error,
		    FSIMG_OUTPUT_PUBLISHED, "destination directory sync incomplete"));
	return (FSIMG_OK);
close_file:
	fsimg_fd_release(ctx, fd);
	return (status);
}
