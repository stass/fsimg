/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "../lib/output/publish.h"
#include "host.h"
#include "internal.h"
#include "test.h"

typedef struct publication_fault {
	unsigned	 sync_calls;
	unsigned	 fail_sync;
	int		 link_error;
	bool		 link_before_error;
	int		 rename_error;
	bool		 rename_before_error;
	unsigned	 rename_calls;
	bool		 fail_unlink;
	bool		 interrupt_sync;
	bool		 cancel_after_sync;
	bool		 cancelled;
	unsigned	 link_calls;
	fsimg_ctx_t	*ctx;
} publication_fault_t;

static int fault_sync(void *, int);
static int fault_link(void *, int, const char *, int, const char *);
static int fault_unlink(void *, int, const char *);
static int fault_rename(void *, int, const char *, int, const char *);
static void test_rename_fallback(fsimg_ctx_t *, int, int);
static bool holds_bytes(int, const char *, const char *);
static bool host_renames_exclusively(int);
static void create_staged(int);
static void test_failures(fsimg_ctx_t *, int, int);
static void test_races(fsimg_ctx_t *, int, int);
static void test_existing_targets(fsimg_ctx_t *, int, int);
static void test_stable_identity(fsimg_ctx_t *, int, int);
static void test_image_identity(fsimg_ctx_t *, int);
static bool publication_cancelled(void *);
static void test_cancel_after_flush(int, int);

static bool
publication_cancelled(void *argument)
{
	publication_fault_t *fault = argument;

	return (fault->cancelled);
}

static void
test_cancel_after_flush(int staging, int parent)
{
	publication_fault_t fault = { .cancel_after_sync = true };
	fsimg_publish_ops_t hooks = {
		.sync_fd = fault_sync,
		.link_at = fault_link,
		.unlink_at = fault_unlink,
		.rename_at = fault_rename,
		.argument = &fault
	};
	fsimg_options_t options;
	fsimg_ctx_t *ctx;
	fsimg_error_t error;
	fsimg_stats_t stats;
	struct stat st;

	fsimg_options_init(&options);
	options.cancelled = publication_cancelled;
	options.cancel_argument = &fault;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	create_staged(staging);

	/* A successful but slow file flush can overlap cancellation. Publication
	 * must observe that cancellation before creating a final pathname.
	 */
	CHECK(fsimg_publish(ctx, staging, "image", 4, parent, "final", &hooks,
	    &error) == FSIMG_ECANCELLED);
	CHECK(error.output_state == FSIMG_OUTPUT_ABSENT);
	CHECK(fault.sync_calls == 1 && fault.link_calls == 0);
	CHECK(fstatat(parent, "final", &st, AT_SYMLINK_NOFOLLOW) == -1);
	CHECK(errno == ENOENT);
	CHECK(fstatat(staging, "image", &st, AT_SYMLINK_NOFOLLOW) == 0);
	CHECK(S_ISREG(st.st_mode) && st.st_size == 4);
	CHECK(fsimg_ctx_get_stats(ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.descriptors == 0);

	CHECK(unlinkat(staging, "image", 0) == 0);
	fsimg_ctx_free(ctx);
}

static void
test_image_identity(fsimg_ctx_t *ctx, int parent)
{
	fsimg_entry_t entry = { .path = "/content", .kind = FSIMG_ENTRY_FILE,
	    .size = 70001 };
	fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };
	fsimg_writer_t *writer;
	fsimg_reader_t *reader;
	fsimg_io_t *input;
	fsimg_verify_report_t report;
	int fd;

	for (unsigned iteration = 0; iteration < 16; iteration++) {
		CHECK(fsimg_writer_new(ctx, "fat16", NULL, &writer, NULL) == FSIMG_OK);
		CHECK(fsimg_writer_add(writer, &entry, &zero, NULL) == FSIMG_OK);
		CHECK(fsimg_writer_write_at(writer, parent, "volume", NULL) == FSIMG_OK);
		fsimg_writer_free(writer);
		fd = openat(parent, "volume", O_RDONLY | O_NOFOLLOW);
		CHECK(fd != -1);
		CHECK(fsimg_io_from_fd(ctx, fd, FSIMG_IO_READ, &input, NULL) == FSIMG_OK);
		CHECK(close(fd) == 0);
		CHECK(fsimg_reader_open(ctx, input, NULL, &reader, NULL) == FSIMG_OK);
		CHECK(fsimg_verify(reader, FSIMG_VERIFY_PAYLOAD, &report, NULL) == FSIMG_OK);
		fsimg_reader_free(reader);
		fsimg_io_free(input);
		CHECK(unlinkat(parent, "volume", 0) == 0);
	}
}

static void
test_stable_identity(fsimg_ctx_t *ctx, int staging, int parent)
{
	fsimg_host_file_t before, after;
	char byte;
	int fd;

	/* A published image's identity must already be stable when readers begin.
	 * Repeated reads allow queued filesystem metadata work to run as well.
	 */
	for (unsigned iteration = 0; iteration < 16; iteration++) {
		create_staged(staging);
		CHECK(fsimg_publish(ctx, staging, "image", 4, parent, "final", NULL, NULL) == FSIMG_OK);
		fd = openat(parent, "final", O_RDONLY | O_NOFOLLOW);
		CHECK(fd != -1);
		CHECK(fsimg_host_file(fd, &before) == 0);
		for (unsigned sample = 0; sample < 50; sample++) {
			CHECK(pread(fd, &byte, 1, 0) == 1 && byte == 'd');
			poll(NULL, 0, 1);
			CHECK(fsimg_host_file(fd, &after) == 0);
			CHECK(before.inode == after.inode && before.length == after.length);
			CHECK(before.modified.seconds == after.modified.seconds &&
			    before.modified.nanoseconds == after.modified.nanoseconds);
			CHECK(before.changed.seconds == after.changed.seconds &&
			    before.changed.nanoseconds == after.changed.nanoseconds);
		}
		CHECK(close(fd) == 0);
		CHECK(unlinkat(parent, "final", 0) == 0);
	}
}

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
	if (fault->cancel_after_sync)
		fault->cancelled = true;
	return (fsync(fd));
}

static int
fault_link(void *argument, int source_dir, const char *source,
    int target_dir, const char *target)
{
	publication_fault_t *fault = argument;
	int result;

	fault->link_calls++;
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

/* Rename without replacing, as the host does, unless rename_error is set:
 * then fail with it, after renaming when rename_before_error is set.
 */
static int
fault_rename(void *argument, int source_dir, const char *source,
    int target_dir, const char *target)
{
	publication_fault_t *fault = argument;

	fault->rename_calls++;
	if (fault->rename_error == 0 || fault->rename_before_error) {
		if (fsimg_host_rename_exclusive(source_dir, source, target_dir,
		    target) == -1)
			return (-1);
	}
	if (fault->rename_error != 0) {
		errno = fault->rename_error;
		return (-1);
	}
	return (0);
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
		.unlink_at = fault_unlink,
		.rename_at = fault_rename
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
			fault.link_error = scenario == 4 ? EXDEV : EIO;
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

/* Whether parent/name is a regular file holding exactly bytes. */
static bool
holds_bytes(int parent, const char *name, const char *bytes)
{
	char buffer[16];
	ssize_t count;
	int fd;

	fd = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd == -1)
		return (false);
	count = read(fd, buffer, sizeof(buffer));
	CHECK(close(fd) == 0);
	return (count == (ssize_t)strlen(bytes) &&
	    bcmp(buffer, bytes, (size_t)count) == 0);
}

/* Whether the host's no-replace rename works in dir; a host or filesystem
 * without it is reported and the cases that need it are skipped.
 */
static bool
host_renames_exclusively(int dir)
{
	int fd, saved;

	fd = openat(dir, "probe", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	CHECK(fd != -1 && close(fd) == 0);
	if (fsimg_host_rename_exclusive(dir, "probe", dir, "probed") == 0) {
		CHECK(unlinkat(dir, "probed", 0) == 0);
		return (true);
	}
	saved = errno;
	CHECK(saved == ENOSYS || saved == EINVAL || saved == ENOTSUP ||
	    saved == EOPNOTSUPP);
	CHECK(unlinkat(dir, "probe", 0) == 0);
	printf("SKIP: no exclusive rename here (errno %d); host rename cases\n",
	    saved);
	return (false);
}

/*
 * A filesystem without hard links, such as FAT or exFAT, refuses linkat
 * with EPERM (Linux) or ENOTSUP (macOS); publication then renames the
 * staged name without replacing. The rename consumes the staged name, so
 * no unlink follows. Any other link failure never falls back: EEXIST is an
 * existing target, EXDEV or EINVAL unsupported, anything else uncertain. A
 * rename that finds the target (a race after the link attempt) leaves it
 * and the staged file untouched; an unsupported rename retains the staged
 * file; an ambiguous one is settled by the staged name. After the rename,
 * directory sync failures still describe published output.
 */
static void
test_rename_fallback(fsimg_ctx_t *ctx, int staging, int parent)
{
	static const struct {
		const char	*name;
		int		 link_error;
		int		 rename_error;	/* Injected; 0 renames for real */
		bool		 rename_before_error;
		bool		 existing;	/* "final" exists beforehand */
		unsigned	 fail_sync;	/* 2 staging, 3 parent directory */
		fsimg_status_t	 status;
		fsimg_output_state_t state;
		int		 os_error;	/* The failed call's errno */
		bool		 renames;	/* The rename hook runs */
		bool		 renamed;	/* "final" holds the staged data */
	} cases[] = {
		{ .name = "EPERM link renames", .link_error = EPERM,
		  .status = FSIMG_OK, .renames = true, .renamed = true },
		{ .name = "ENOTSUP link renames", .link_error = ENOTSUP,
		  .status = FSIMG_OK, .renames = true, .renamed = true },
		{ .name = "EOPNOTSUPP link renames", .link_error = EOPNOTSUPP,
		  .status = FSIMG_OK, .renames = true, .renamed = true },
		{ .name = "existing target survives the rename",
		  .os_error = EEXIST,
		  .link_error = EPERM, .existing = true,
		  .status = FSIMG_EEXISTS, .state = FSIMG_OUTPUT_ABSENT,
		  .renames = true },
		{ .name = "rename finds the target",
		  .os_error = EEXIST, .link_error = EPERM,
		  .rename_error = EEXIST, .status = FSIMG_EEXISTS,
		  .state = FSIMG_OUTPUT_ABSENT, .renames = true },
		{ .name = "rename flag unsupported",
		  .os_error = EINVAL, .link_error = EPERM,
		  .rename_error = EINVAL, .status = FSIMG_EUNSUPPORTED,
		  .state = FSIMG_OUTPUT_ABSENT, .renames = true },
		{ .name = "rename unsupported",
		  .os_error = ENOTSUP, .link_error = EPERM,
		  .rename_error = ENOTSUP, .status = FSIMG_EUNSUPPORTED,
		  .state = FSIMG_OUTPUT_ABSENT, .renames = true },
		{ .name = "no exclusive rename",
		  .os_error = ENOSYS, .link_error = EPERM,
		  .rename_error = ENOSYS, .status = FSIMG_EUNSUPPORTED,
		  .state = FSIMG_OUTPUT_ABSENT, .renames = true },
		{ .name = "rename fails, staged name stays",
		  .os_error = EIO, .link_error = EPERM,
		  .rename_error = EIO, .status = FSIMG_EIO,
		  .state = FSIMG_OUTPUT_UNCERTAIN, .renames = true },
		{ .name = "rename fails after taking effect",
		  .os_error = EIO, .link_error = EPERM,
		  .rename_error = EIO, .rename_before_error = true,
		  .status = FSIMG_EDURABILITY, .state = FSIMG_OUTPUT_PUBLISHED,
		  .renames = true, .renamed = true },
		{ .name = "staging sync fails after the rename",
		  .link_error = EPERM, .fail_sync = 2,
		  .status = FSIMG_EDURABILITY, .state = FSIMG_OUTPUT_PUBLISHED,
		  .renames = true, .renamed = true },
		{ .name = "parent sync fails after the rename",
		  .link_error = EPERM, .fail_sync = 3,
		  .status = FSIMG_EDURABILITY, .state = FSIMG_OUTPUT_PUBLISHED,
		  .renames = true, .renamed = true },
		{ .name = "ambiguous link",
		  .os_error = EIO, .link_error = EIO,
		  .status = FSIMG_EIO, .state = FSIMG_OUTPUT_UNCERTAIN },
		{ .name = "cross-device link",
		  .os_error = EXDEV, .link_error = EXDEV,
		  .status = FSIMG_EUNSUPPORTED, .state = FSIMG_OUTPUT_ABSENT },
		{ .name = "invalid link",
		  .os_error = EINVAL, .link_error = EINVAL,
		  .status = FSIMG_EUNSUPPORTED, .state = FSIMG_OUTPUT_ABSENT },
		{ .name = "link finds the target",
		  .os_error = EEXIST, .link_error = EEXIST,
		  .status = FSIMG_EEXISTS, .state = FSIMG_OUTPUT_ABSENT },
	};
	fsimg_publish_ops_t hooks = {
		.sync_fd = fault_sync,
		.link_at = fault_link,
		.unlink_at = fault_unlink,
		.rename_at = fault_rename
	};
	publication_fault_t fault;
	fsimg_error_t error;
	fsimg_status_t status;
	bool host_renames;
	int fd;

	host_renames = host_renames_exclusively(staging);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		/* A real rename runs unless an error is injected first. */
		if (!host_renames && cases[i].renames &&
		    (cases[i].rename_error == 0 || cases[i].rename_before_error))
			continue;
		printf("rename fallback: %s\n", cases[i].name);
		bzero(&fault, sizeof(fault));
		bzero(&error, sizeof(error));
		fault.link_error = cases[i].link_error;
		fault.rename_error = cases[i].rename_error;
		fault.rename_before_error = cases[i].rename_before_error;
		fault.fail_sync = cases[i].fail_sync;
		/* A renamed staged name needs no unlink; any unlink fails. */
		fault.fail_unlink = true;
		hooks.argument = &fault;
		create_staged(staging);
		if (cases[i].existing) {
			fd = openat(parent, "final", O_WRONLY | O_CREAT | O_EXCL, 0600);
			CHECK(fd != -1);
			CHECK(write(fd, "keep", 4) == 4);
			CHECK(close(fd) == 0);
		}
		status = fsimg_publish(ctx, staging, "image", 4, parent, "final",
		    &hooks, &error);
		CHECK(status == cases[i].status);
		if (status != FSIMG_OK)
			CHECK(error.output_state == cases[i].state);
		if (cases[i].os_error != 0)
			CHECK(error.os_error == cases[i].os_error);
		CHECK(fault.link_calls == 1);
		CHECK(fault.rename_calls == (cases[i].renames ? 1u : 0u));
		if (cases[i].existing)
			CHECK(holds_bytes(parent, "final", "keep"));
		else
			CHECK(holds_bytes(parent, "final", "data") == cases[i].renamed);
		CHECK(holds_bytes(staging, "image", "data") == !cases[i].renamed);
		if (cases[i].renamed || cases[i].existing)
			CHECK(unlinkat(parent, "final", 0) == 0);
		if (!cases[i].renamed)
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
main(int argc, char **argv)
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
	if (argc == 2 && strcmp(argv[1], "--identity") == 0) {
		test_stable_identity(ctx, staging, parent);
		test_image_identity(ctx, parent);
	} else {
		CHECK(argc == 1);
		test_failures(ctx, staging, parent);
		test_rename_fallback(ctx, staging, parent);
		test_existing_targets(ctx, staging, parent);
		test_races(ctx, staging, parent);
		test_cancel_after_flush(staging, parent);
	}
	CHECK(fsimg_ctx_get_stats(ctx, &after, NULL) == FSIMG_OK);
	CHECK(after.memory_bytes == before.memory_bytes && after.descriptors == 0);
	fsimg_ctx_free(ctx);
	CHECK(close(staging) == 0);
	CHECK(unlinkat(parent, "stage", AT_REMOVEDIR) == 0);
	CHECK(close(parent) == 0);
	CHECK(rmdir(path) == 0);
	return (0);
}
