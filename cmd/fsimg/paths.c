/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
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

#include "cli.h"
#include "host.h"
#include "internal.h"
#include "publish.h"

static fsimg_status_t host_error(fsimg_error_t *, const char *);
static fsimg_status_t path_error(fsimg_error_t *, const char *, const char *);
static fsimg_status_t split_output(const char *, char *, char *, fsimg_error_t *);
static fsimg_status_t private_directory(int, const char *, char *, int *, bool *,
    fsimg_error_t *);
static fsimg_status_t reject_ancestor(int, int, fsimg_error_t *);
static int clean_scratch(int);
static bool publication_cancelled(void *);

typedef struct publication_cancel {
	const volatile sig_atomic_t *interrupted;
	uint64_t	 deadline;
} publication_cancel_t;

uint64_t
fsimg_cli_clock(void)
{
	struct timespec time;

	if (clock_gettime(CLOCK_MONOTONIC, &time) == -1)
		return (0);
	return ((uint64_t)time.tv_sec * 1000 + (uint64_t)time.tv_nsec / 1000000);
}

static fsimg_status_t
host_error(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, errno == EEXIST ? FSIMG_EEXISTS : FSIMG_EIO,
	    errno, FSIMG_OP_IO, message));
}

static fsimg_status_t
path_error(fsimg_error_t *error, const char *message, const char *path)
{
	fsimg_status_t status;
	int count;

	status = host_error(error, message);
	if (error != NULL) {
		count = snprintf(error->path, sizeof(error->path), "%s", path);
		error->path_truncated = count < 0 || (size_t)count >= sizeof(error->path);
	}
	return (status);
}

static fsimg_status_t
split_output(const char *path, char *parent, char *name, fsimg_error_t *error)
{
	const char *base;
	size_t prefix, length;

	length = strlen(path);
	if (length == 0 || length >= PATH_MAX)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "output pathname is empty or exceeds the host path bound"));
	base = strrchr(path, '/');
	if (base == NULL) {
		strcpy(parent, ".");
		base = path;
	} else {
		prefix = base == path ? 1 : (size_t)(base - path);
		bcopy(path, parent, prefix);
		parent[prefix] = '\0';
		base++;
	}
	if (base[0] == '\0' || strlen(base) > NAME_MAX ||
	    strcmp(base, ".") == 0 || strcmp(base, "..") == 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "output requires a nonempty host basename"));
	strcpy(name, base);
	return (FSIMG_OK);
}

static fsimg_status_t
private_directory(int parent, const char *role, char *name, int *out,
    bool *created, fsimg_error_t *error)
{
	uint64_t random[2];

	for (unsigned attempt = 0; attempt < 128; attempt++) {
		if (fsimg_host_random(random, sizeof(random)) == -1)
			return (host_error(error, "private directory randomness failed"));
		snprintf(name, 96, ".fsimg-%s-%016llx%016llx", role,
		    (unsigned long long)random[0], (unsigned long long)random[1]);
		if (mkdirat(parent, name, 0700) == 0) {
			*created = true;
			*out = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			if (*out == -1)
				return (host_error(error, "private directory open failed"));
			return (FSIMG_OK);
		}
		if (errno != EEXIST)
			return (host_error(error, "private directory creation failed"));
	}
	return (fsimg_error_set(error, FSIMG_EEXISTS, EEXIST,
	    FSIMG_OP_IO, "private directory name attempts exhausted"));
}

/* Compare opened identities while walking toward the host root. No pathname
 * supplied by a worker participates. Both directions are checked because
 * scratch deletion authority must never contain the output tree.
 */
static fsimg_status_t
reject_ancestor(int descendant, int ancestor, fsimg_error_t *error)
{
	fsimg_host_file_t target, current, parent;
	fsimg_status_t status;
	int fd, next, saved;

	if (fsimg_host_file(ancestor, &target) == -1)
		return (host_error(error, "private directory identity query failed"));
	fd = fsimg_host_dup(descendant);
	if (fd == -1)
		return (host_error(error, "private directory ancestry open failed"));
	status = FSIMG_OK;
	for (unsigned depth = 0;; depth++) {
		if (fsimg_host_file(fd, &current) == -1) {
			status = host_error(error, "private directory ancestry query failed");
			break;
		}
		if (current.device == target.device && current.inode == target.inode) {
			status = fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_IO, "scratch and output trees overlap");
			break;
		}
		if (depth == PATH_MAX) {
			status = fsimg_error_set(error, FSIMG_ELIMIT, 0,
			    FSIMG_OP_IO, "private directory ancestry exceeds host path bound");
			break;
		}
		next = openat(fd, "..", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (next == -1) {
			status = host_error(error, "private directory parent open failed");
			break;
		}
		if (fsimg_host_file(next, &parent) == -1) {
			saved = errno;
			close(next);
			errno = saved;
			status = host_error(error, "private directory parent query failed");
			break;
		}
		close(fd);
		fd = next;
		if (current.device == parent.device && current.inode == parent.inode)
			break;
	}
	close(fd);
	return (status);
}

fsimg_status_t
fsimg_cli_prepare(fsimg_cli_options_t *options, fsimg_cli_paths_t *paths,
    fsimg_error_t *error)
{
	fsimg_worker_request_t *request = &options->request;
	fsimg_host_file_t identity;
	struct stat st;
	char parent_path[PATH_MAX];
	fsimg_status_t status;
	int count;

	bzero(paths, sizeof(*paths));
	paths->parent = paths->scratch_parent = -1;
	/* Prepare scratch before creating output. A -T path that would exist only
	 * inside the new extraction root cannot acquire cleanup authority there.
	 */
	if (strcmp(options->scratch, "-") != 0) {
		if (strlen(options->scratch) + sizeof(paths->scratch_name) + 1 >= PATH_MAX)
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_IO, "scratch parent leaves no room for a private pathname"));
		paths->scratch_parent = open(options->scratch, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (paths->scratch_parent == -1)
			return (path_error(error, "cannot open scratch directory", options->scratch));
		status = private_directory(paths->scratch_parent, "scratch", paths->scratch_name,
		    &request->scratch_fd, &paths->scratch_created, error);
		if (paths->scratch_created)
			snprintf(paths->retained_scratch, sizeof(paths->retained_scratch),
			    "%s/%s", options->scratch, paths->scratch_name);
		if (status != FSIMG_OK)
			return (status);
	}
	if (request->kind == FSIMG_JOB_CREATE || request->kind == FSIMG_JOB_EXTRACT) {
		status = split_output(options->output, parent_path, paths->basename, error);
		if (status != FSIMG_OK)
			return (status);
		if (strlen(parent_path) + sizeof(paths->staging_name) + 7 >= PATH_MAX)
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_IO, "output parent leaves no room for a private pathname"));
		paths->parent = open(parent_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (paths->parent == -1)
			return (path_error(error, "cannot open output directory", parent_path));
		if (fstatat(paths->parent, paths->basename, &st, AT_SYMLINK_NOFOLLOW) == 0) {
			errno = EEXIST;
			return (path_error(error, "cannot create output", options->output));
		}
		if (errno != ENOENT)
			return (path_error(error, "cannot inspect output", options->output));
		if (request->kind == FSIMG_JOB_CREATE) {
			status = private_directory(paths->parent, "output", paths->staging_name,
			    &request->output_fd, &paths->staging_created, error);
			if (!paths->staging_created)
				return (status);
			/* A post-create open failure still needs a retained staging path. */
			count = snprintf(paths->retained_output, sizeof(paths->retained_output),
			    "%s/%s/image", parent_path, paths->staging_name);
			if (status != FSIMG_OK)
				return (status);
		} else {
			if (mkdirat(paths->parent, paths->basename, 0700) == -1)
				return (path_error(error, "cannot create extraction directory", options->output));
			paths->extraction_created = true;
			count = snprintf(paths->retained_output, sizeof(paths->retained_output),
			    "%s", options->output);
			request->output_fd = openat(paths->parent, paths->basename,
			    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			if (request->output_fd == -1)
				return (path_error(error, "cannot open extraction directory", options->output));
		}
		if (count < 0 || (size_t)count >= sizeof(paths->retained_output))
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_IO, "private output pathname exceeds host path bound"));
		if (request->scratch_fd != -1) {
			status = reject_ancestor(request->output_fd, request->scratch_fd, error);
			if (status != FSIMG_OK)
				return (status);
			status = reject_ancestor(request->scratch_fd, request->output_fd, error);
			if (status != FSIMG_OK)
				return (status);
		}
	}
	if (request->kind >= FSIMG_JOB_CREATE) {
		request->source_fd = fsimg_host_source_open(AT_FDCWD, options->source, true);
		if (request->source_fd == -1)
			return (path_error(error, "cannot open source directory", options->source));
		if ((request->write.flags & FSIMG_WRITE_REPRODUCIBLE) == 0 &&
		    fsimg_host_random(&request->write.volume_serial,
		    sizeof(request->write.volume_serial)) == -1)
			return (host_error(error, "volume identifier generation failed"));
		/* A random version 4 UUID, which option validation requests. */
		if (options->random_uuid) {
			if (fsimg_host_random(request->write.volume_uuid,
			    sizeof(request->write.volume_uuid)) == -1)
				return (host_error(error,
				    "volume identifier generation failed"));
			request->write.volume_uuid[6] =
			    (unsigned char)((request->write.volume_uuid[6] & 0x0f) | 0x40);
			request->write.volume_uuid[8] =
			    (unsigned char)((request->write.volume_uuid[8] & 0x3f) | 0x80);
		}
	} else {
		request->input_fd = fsimg_host_source_open(AT_FDCWD, options->input, false);
		if (request->input_fd == -1)
			return (path_error(error, "cannot open image", options->input));
		if (fsimg_host_file(request->input_fd, &identity) == -1)
			return (host_error(error, "image identity query failed"));
		if (identity.kind != FSIMG_HOST_FILE)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_IO, "input image must be a seekable regular file"));
		if (request->offset > identity.length)
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_IO, "volume slice starts past the input image"));
		if (request->length == 0)
			request->length = identity.length - request->offset;
		if (request->length > identity.length - request->offset)
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_IO, "volume slice exceeds the input image"));
		if (request->limits.max_output_bytes == 0)
			request->limits.max_output_bytes = request->length;
	}
	return (FSIMG_OK);
}

static bool
publication_cancelled(void *argument)
{
	publication_cancel_t *cancel = argument;

	return (*cancel->interrupted != 0 ||
	    (cancel->deadline != 0 && fsimg_cli_clock() >= cancel->deadline));
}

fsimg_status_t
fsimg_cli_publish(const fsimg_cli_options_t *options, fsimg_cli_paths_t *paths,
    const fsimg_worker_result_t *result, const volatile sig_atomic_t *interrupted,
    fsimg_error_t *error)
{
	publication_cancel_t cancel = { .interrupted = interrupted,
	    .deadline = options->request.deadline_milliseconds };
	fsimg_options_t limits = options->request.limits;
	fsimg_ctx_t *ctx;
	fsimg_status_t status;

	limits.scratch_directory_fd = -1;
	limits.cancelled = publication_cancelled;
	limits.cancel_argument = &cancel;
	status = fsimg_ctx_new(&limits, &ctx, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_publish(ctx, options->request.output_fd, "image", result->image_bytes,
	    paths->parent, paths->basename, NULL, error);
	paths->published = status == FSIMG_OK || error->output_state == FSIMG_OUTPUT_PUBLISHED;
	paths->retain_staging = status == FSIMG_EUNSUPPORTED ||
	    error->output_state == FSIMG_OUTPUT_UNCERTAIN;
	fsimg_ctx_free(ctx);
	return (status);
}

/* Scratch uses flat files. Never recurse into a directory left by a failed
 * or compromised worker. A finite cleanup limit bounds coordinator work;
 * anything retained is private and reported, rather than silently ignored.
 */
static int
clean_scratch(int root)
{
	unsigned char buffer[16384];
	const char *name;
	size_t used, cursor, length;
	unsigned entries, interruptions;
	int fd, code, saved;

	fd = openat(root, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd == -1)
		return (errno);
	entries = interruptions = 0;
	saved = 0;
	for (;;) {
		code = fsimg_host_directory_read(fd, buffer, sizeof(buffer), &used);
		if (code == -1 && errno == EINTR && interruptions++ < 1024)
			continue;
		if (code == -1) {
			saved = errno;
			break;
		}
		if (used == 0)
			break;
		interruptions = 0;
		cursor = 0;
		while ((code = fsimg_host_directory_name(buffer, used, &cursor, &name, &length)) > 0) {
			if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
				continue;
			if (entries++ == 4096) {
				saved = E2BIG;
				break;
			}
			if (unlinkat(root, name, 0) == -1) {
				saved = errno;
				break;
			}
		}
		if (code == -1)
			saved = errno;
		if (saved != 0)
			break;
	}
	if (close(fd) == -1 && saved == 0)
		saved = errno;
	return (saved);
}

fsimg_status_t
fsimg_cli_cleanup(fsimg_cli_options_t *options, fsimg_cli_paths_t *paths,
    fsimg_status_t status, fsimg_error_t *error)
{
	fsimg_worker_request_t *request = &options->request;
	char *last_component;
	int cleanup, failure;
	int *descriptors[] = { &request->input_fd, &request->source_fd,
	    &request->output_fd, &request->scratch_fd, &paths->parent, &paths->scratch_parent };

	cleanup = 0;
	if (paths->staging_created && !paths->retain_staging) {
		/* Cleanup can remove the image yet retain other directory entries.
		 * Report the private directory, not an image that may no longer exist.
		 */
		last_component = strrchr(paths->retained_output, '/');
		if (last_component != NULL)
			*last_component = '\0';
		if (request->output_fd != -1 &&
		    unlinkat(request->output_fd, "image", 0) == -1 && errno != ENOENT)
			cleanup = errno;
		if (unlinkat(paths->parent, paths->staging_name, AT_REMOVEDIR) == -1) {
			if (cleanup == 0)
				cleanup = errno;
		} else
			paths->retained_output[0] = '\0';
	}
	if (paths->scratch_created) {
		failure = request->scratch_fd == -1 ? 0 : clean_scratch(request->scratch_fd);
		if (failure == 0 &&
		    unlinkat(paths->scratch_parent, paths->scratch_name, AT_REMOVEDIR) == -1)
			failure = errno;
		if (failure == 0)
			paths->retained_scratch[0] = '\0';
		else if (cleanup == 0)
			cleanup = failure;
	}
	if (status == FSIMG_OK && paths->parent != -1 && fsync(paths->parent) == -1 &&
	    cleanup == 0)
		cleanup = errno;
	for (unsigned i = 0; i < sizeof(descriptors) / sizeof(descriptors[0]); i++) {
		if (*descriptors[i] != -1 && close(*descriptors[i]) == -1 && cleanup == 0)
			cleanup = errno;
		*descriptors[i] = -1;
	}
	if (cleanup != 0) {
		if (status == FSIMG_OK)
			status = fsimg_error_set(error, paths->published ? FSIMG_EDURABILITY : FSIMG_EIO,
			    cleanup, FSIMG_OP_IO, "private capability cleanup incomplete");
		error->cleanup_error = cleanup;
	}
	if (status != FSIMG_OK) {
		if (paths->published)
			error->output_state = FSIMG_OUTPUT_PUBLISHED;
		else if (error->output_state != FSIMG_OUTPUT_UNCERTAIN) {
			if (paths->extraction_created)
				error->output_state = FSIMG_OUTPUT_INCOMPLETE;
			else if (request->kind == FSIMG_JOB_CREATE)
				error->output_state = FSIMG_OUTPUT_ABSENT;
		}
	}
	return (status);
}
