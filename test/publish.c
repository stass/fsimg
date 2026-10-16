/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

#include "../lib/output/publish.h"
#include "internal.h"
#include "test.h"

typedef struct publication_fault {
	unsigned	 sync_calls;
	unsigned	 fail_sync;
	int		 link_error;
	bool		 link_before_error;
	bool		 fail_unlink;
	bool		 interrupt_sync;
	fsimg_ctx_t	*ctx;
} publication_fault_t;

static int fault_sync(void *, int);
static int fault_link(void *, int, const char *, int, const char *);
static int fault_unlink(void *, int, const char *);
static void create_staged(int);
static void test_failures(fsimg_ctx_t *, int, int);
static void test_races(fsimg_ctx_t *, int, int);
static void test_existing_targets(fsimg_ctx_t *, int, int);

static int
fault_sync(void *argument, int fd)
{
	publication_fault_t *fault = argument;
	fsimg_stats_t stats;

	fault->sync_calls++;
	if (fault->ctx != NULL)
		CHECK(fsimg_ctx_get_stats(fault->ctx, &stats, NULL) == FSIMG_ESTATE);
	if (fault->interrupt_sync) {
		/* Bound the test independently of the implementation's retry limit. */
		errno = fault->sync_calls <= 1026 ? EINTR : EIO;
		return (-1);
	}
	if (fault->sync_calls == fault->fail_sync) {
		errno = EIO;
		return (-1);
	}
	return (fsync(fd));
}

static int
fault_link(void *argument, int source_dir, const char *source,
    int target_dir, const char *target)
{
	publication_fault_t *fault = argument;
	int result;

	if (fault->link_error == 0 || fault->link_before_error) {
		result = linkat(source_dir, source, target_dir, target, 0);
		if (result == -1)
			return (-1);
	}
	if (fault->link_error != 0) {
		errno = fault->link_error;
		return (-1);
	}
	return (0);
}

static int
fault_unlink(void *argument, int parent, const char *name)
{
	publication_fault_t *fault = argument;

	if (fault->fail_unlink) {
		errno = EIO;
		return (-1);
	}
	return (unlinkat(parent, name, 0));
}

static void
create_staged(int staging)
{
	int fd;

	fd = openat(staging, "image", O_WRONLY | O_CREAT | O_EXCL, 0600);
	CHECK(fd != -1);
	CHECK(write(fd, "data", 4) == 4);
	CHECK(close(fd) == 0);
}

static void
test_failures(fsimg_ctx_t *ctx, int staging, int parent)
{
	fsimg_publish_ops_t hooks = {
		.sync_fd = fault_sync,
		.link_at = fault_link,
		.unlink_at = fault_unlink
	};
	publication_fault_t fault;
	fsimg_error_t error;
	struct stat st;
	fsimg_status_t status;

	create_staged(staging);
	CHECK(fsimg_publish(ctx, staging, "image", 5, parent, "final", NULL,
	    &error) == FSIMG_EINVAL);
	CHECK(fstatat(parent, "final", &st, AT_SYMLINK_NOFOLLOW) == -1 && errno == ENOENT);
	CHECK(fsimg_publish(ctx, staging, "../image", 4, parent, "final", NULL,
	    &error) == FSIMG_EINVAL);
	CHECK(unlinkat(staging, "image", 0) == 0);
	CHECK(symlinkat("../sentinel", staging, "image") == 0);
	CHECK(fsimg_publish(ctx, staging, "image", 4, parent, "final", NULL,
	    &error) == FSIMG_EIO);
	CHECK(unlinkat(staging, "image", 0) == 0);

	for (unsigned scenario = 0; scenario < 9; scenario++) {
		bzero(&fault, sizeof(fault));
		hooks.argument = &fault;
		if (scenario < 3)
			fault.fail_sync = scenario + 1;
		if (scenario == 3)
			fault.fail_unlink = true;
		if (scenario >= 4 && scenario <= 6)
			fault.link_error = scenario == 4 ? EOPNOTSUPP : EIO;
		if (scenario == 6)
			fault.link_before_error = true;
		if (scenario == 7)
			fault.interrupt_sync = true;
		if (scenario == 8)
			fault.ctx = ctx;
		create_staged(staging);
		status = fsimg_publish(ctx, staging, "image", 4, parent,
		    "final", &hooks, &error);
		if (scenario == 0) {
			CHECK(status == FSIMG_EIO);
			CHECK(error.output_state == FSIMG_OUTPUT_ABSENT);
		} else if (scenario <= 3) {
			CHECK(status == FSIMG_EDURABILITY);
			CHECK(error.output_state == FSIMG_OUTPUT_PUBLISHED);
		} else if (scenario == 4) {
			CHECK(status == FSIMG_EUNSUPPORTED);
			CHECK(error.output_state == FSIMG_OUTPUT_ABSENT);
		} else if (scenario <= 6) {
			CHECK(status == FSIMG_EIO);
			CHECK(error.output_state == FSIMG_OUTPUT_UNCERTAIN);
		} else if (scenario == 7) {
			CHECK(status == FSIMG_EIO && error.os_error == EINTR);
			CHECK(fault.sync_calls <= 1025);
		} else {
			CHECK(status == FSIMG_OK);
		}
		if ((scenario >= 1 && scenario <= 3) || scenario == 6 || scenario == 8) {
			CHECK(fstatat(parent, "final", &st, AT_SYMLINK_NOFOLLOW) == 0);
			CHECK(st.st_size == 4);
			CHECK(unlinkat(parent, "final", 0) == 0);
		} else {
			CHECK(fstatat(parent, "final", &st, AT_SYMLINK_NOFOLLOW) == -1);
			CHECK(errno == ENOENT);
		}
		if (scenario != 1 && scenario != 2 && scenario != 8)
			CHECK(unlinkat(staging, "image", 0) == 0);
	}
}

static void
test_races(fsimg_ctx_t *ctx, int staging, int parent)
{
	fsimg_error_t error;
	struct stat staged_identity, final_identity;
	fsimg_status_t status;
	pid_t child;
	char buffer[4];
	int channel[2], fd, wait_status;

	for (unsigned i = 0; i < 32; i++) {
		create_staged(staging);
		CHECK(fstatat(staging, "image", &staged_identity, 0) == 0);
		CHECK(pipe(channel) == 0);
		child = fork();
		CHECK(child != -1);
		if (child == 0) {
			close(channel[1]);
			if (read(channel[0], buffer, 1) != 1)
				_exit(1);
			fd = openat(parent, "final", O_WRONLY | O_CREAT | O_EXCL, 0600);
			if (fd != -1) {
				if (write(fd, "race", 4) != 4)
					_exit(1);
				if (close(fd) == -1)
					_exit(1);
			} else if (errno != EEXIST) {
				_exit(1);
			}
			_exit(0);
		}
		CHECK(close(channel[0]) == 0);
		CHECK(write(channel[1], "!", 1) == 1);
		CHECK(close(channel[1]) == 0);
		status = fsimg_publish(ctx, staging, "image", 4, parent,
		    "final", NULL, &error);
		CHECK(status == FSIMG_OK || status == FSIMG_EEXISTS);
		CHECK(waitpid(child, &wait_status, 0) == child);
		CHECK(WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0);
		fd = openat(parent, "final", O_RDONLY | O_NOFOLLOW);
		CHECK(fd != -1);
		CHECK(fstat(fd, &final_identity) == 0);
		CHECK(read(fd, buffer, 4) == 4);
		CHECK(close(fd) == 0);
		if (status == FSIMG_OK) {
			CHECK(bcmp(buffer, "data", 4) == 0);
			CHECK(final_identity.st_ino == staged_identity.st_ino);
		} else {
			CHECK(bcmp(buffer, "race", 4) == 0);
			CHECK(unlinkat(staging, "image", 0) == 0);
		}
		CHECK(unlinkat(parent, "final", 0) == 0);
	}
}

static void
test_existing_targets(fsimg_ctx_t *ctx, int staging, int parent)
{
	struct stat before, after;
	fsimg_error_t error;
	char bytes[4];
	int fd;

	fd = openat(parent, "sentinel", O_WRONLY | O_CREAT | O_EXCL, 0600);
	CHECK(fd != -1);
	CHECK(write(fd, "safe", 4) == 4);
	CHECK(close(fd) == 0);
	for (unsigned kind = 0; kind < 3; kind++) {
		if (kind == 0) {
			fd = openat(parent, "final", O_WRONLY | O_CREAT | O_EXCL, 0600);
			CHECK(fd != -1);
			CHECK(write(fd, "keep", 4) == 4);
			CHECK(close(fd) == 0);
		} else if (kind == 1) {
			CHECK(mkdirat(parent, "final", 0700) == 0);
		} else {
			CHECK(symlinkat("sentinel", parent, "final") == 0);
		}
		CHECK(fstatat(parent, "final", &before, AT_SYMLINK_NOFOLLOW) == 0);
		create_staged(staging);
		CHECK(fsimg_publish(ctx, staging, "image", 4, parent, "final",
		    NULL, &error) == FSIMG_EEXISTS);
		CHECK(fstatat(parent, "final", &after, AT_SYMLINK_NOFOLLOW) == 0);
		CHECK(before.st_ino == after.st_ino && before.st_mode == after.st_mode);
		CHECK(before.st_size == after.st_size);
		CHECK(unlinkat(staging, "image", 0) == 0);
		CHECK(unlinkat(parent, "final", kind == 1 ? AT_REMOVEDIR : 0) == 0);
	}
	fd = openat(parent, "sentinel", O_RDONLY | O_NOFOLLOW);
	CHECK(fd != -1);
	CHECK(read(fd, bytes, 4) == 4);
	CHECK(bcmp(bytes, "safe", 4) == 0);
	CHECK(close(fd) == 0);
	CHECK(unlinkat(parent, "sentinel", 0) == 0);
}

int
main(void)
{
	char path[] = "publish-fixture-XXXXXX";
	fsimg_ctx_t *ctx;
	fsimg_stats_t before, after;
	int staging, parent;

	CHECK(mkdtemp(path) != NULL);
	parent = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(parent != -1);
	CHECK(mkdirat(parent, "stage", 0700) == 0);
	staging = openat(parent, "stage", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(staging != -1);
	CHECK(fsimg_ctx_new(NULL, &ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(ctx, &before, NULL) == FSIMG_OK);
	test_failures(ctx, staging, parent);
	test_existing_targets(ctx, staging, parent);
	test_races(ctx, staging, parent);
	CHECK(fsimg_ctx_get_stats(ctx, &after, NULL) == FSIMG_OK);
	CHECK(after.memory_bytes == before.memory_bytes && after.descriptors == 0);
	fsimg_ctx_free(ctx);
	CHECK(close(staging) == 0);
	CHECK(unlinkat(parent, "stage", AT_REMOVEDIR) == 0);
	CHECK(close(parent) == 0);
	CHECK(rmdir(path) == 0);
	return (0);
}
