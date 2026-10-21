/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <netinet/in.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/stat.h>
#include <sys/syscall.h>
#endif

#include "sandbox.h"
#include "host.h"
#include "internal.h"
#include "test.h"

/* The worker's three descriptor profiles: a read job writing beneath an
 * extraction destination, and a create job writing a staged image from one
 * directory root or one regular-file root. Every profile places the input at
 * 3, the writable work directory at 4, a root (when present) at 5 and the
 * scratch directory at 6.
 */
typedef enum sandbox_profile {
	PROFILE_EXTRACT,
	PROFILE_DIRECTORY_ROOT,
	PROFILE_FILE_ROOT,
	PROFILE_COUNT
} sandbox_profile_t;

typedef enum sandbox_attempt {
	ATTEMPT_INPUT_READ,
	ATTEMPT_WORK_WRITE,
	ATTEMPT_WORK_MKDIR,
	ATTEMPT_OUTPUT_REMOVE_FILE,
	ATTEMPT_SCRATCH_REMOVE,
	ATTEMPT_SOURCE_READ,
	ATTEMPT_SOURCE_MOUNT,
	ATTEMPT_STATX_PATH,
	ATTEMPT_STATX_EMPTY_PATH,
	ATTEMPT_STATX_CWD,
	ATTEMPT_STATX_HIGH_POINTER,
	ATTEMPT_STATX_HIGH_FLAGS,
	ATTEMPT_OUTSIDE_OPEN,
	ATTEMPT_PARENT_ESCAPE,
	ATTEMPT_INPUT_WRITE,
	ATTEMPT_INPUT_CHMOD,
	ATTEMPT_INPUT_TIMES,
	ATTEMPT_INPUT_TRUNCATE,
	ATTEMPT_SOURCE_WRITE,
	ATTEMPT_NETWORK,
	ATTEMPT_EXEC,
	ATTEMPT_METADATA_QUERY,
	ATTEMPT_FD_REOPEN,
	ATTEMPT_INHERITED_FD,
	ATTEMPT_ALTERNATE_ABI,
	ATTEMPT_COUNT
} sandbox_attempt_t;

static void close_unlisted(void);
static bool attempt_operation(sandbox_attempt_t, sandbox_profile_t);
static int run_child(const int *, sandbox_attempt_t, sandbox_profile_t);

static void
close_unlisted(void)
{
	if (fsimg_host_closefrom(7) == -1)
		_exit(1);
	close(0);
	close(1);
	close(2);
}

/* Each attempt runs in its own child: an accidental successful exec, mutation,
 * or descriptor acquisition cannot mask any later negative assertion. Only an
 * extraction destination may create directories; no work directory may remove
 * names, and scratch removes files but never makes directories.
 */
static bool
attempt_operation(sandbox_attempt_t attempt, sandbox_profile_t profile)
{
	struct sockaddr_in address;
	struct stat st;
	char byte;
	char *arguments[] = { "false", NULL };
	char *environment[] = { NULL };
	int fd, result;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	uint64_t mount_id;
#ifdef __linux__
	struct statx extended;
#endif
	size_t transferred;

	switch (attempt) {
	case ATTEMPT_INPUT_READ:
		return (pread(3, &byte, 1, 0) == 1 && byte == 'I');
	case ATTEMPT_WORK_WRITE:
		if (fsimg_ctx_new(NULL, &ctx, NULL) != FSIMG_OK)
			return (false);
		if (fsimg_fd_reserve(ctx, NULL) != FSIMG_OK)
			return (false);
		fd = openat(4, "output", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd == -1)
			return (false);
		if (fsimg_io_adopt_fd(ctx, fd, FSIMG_IO_READ | FSIMG_IO_WRITE,
		    &io, NULL) != FSIMG_OK)
			return (false);
		if (fsimg_io_resize(io, 1, NULL) != FSIMG_OK)
			return (false);
		if (fsimg_io_write_exact(io, 0, "O", 1, &transferred, NULL) != FSIMG_OK)
			return (false);
		if (fsimg_io_flush(io, true, NULL) != FSIMG_OK)
			return (false);
		fsimg_io_free(io);
		fsimg_ctx_free(ctx);
		return (transferred == 1);
	case ATTEMPT_WORK_MKDIR:
		if (profile != PROFILE_EXTRACT)
			return (mkdirat(4, "work-directory", 0700) == -1);
		if (mkdirat(4, "work-directory", 0700) == -1)
			return (false);
		return (unlinkat(4, "work-directory", AT_REMOVEDIR) == -1);
	case ATTEMPT_SOURCE_READ:
		if (profile == PROFILE_FILE_ROOT)
			return (pread(5, &byte, 1, 0) == 1 && byte == 'S');
		fd = openat(5, "source", O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
		if (fd == -1)
			return (false);
		result = (int)read(fd, &byte, 1);
		close(fd);
		return (result == 1 && byte == 'S');
	case ATTEMPT_OUTPUT_REMOVE_FILE:
		fd = openat(4, "delete-file", O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (fd == -1)
			return (false);
		if (close(fd) == -1)
			return (false);
		return (unlinkat(4, "delete-file", 0) == -1);
	case ATTEMPT_SCRATCH_REMOVE:
		fd = openat(6, "scratch-file", O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (fd == -1)
			return (false);
		if (close(fd) == -1)
			return (false);
		if (unlinkat(6, "scratch-file", 0) == -1)
			return (false);
		return (mkdirat(6, "scratch-directory", 0700) == -1);
	case ATTEMPT_SOURCE_MOUNT:
		return (fsimg_host_mount_id(5, &mount_id) == 0 && mount_id != 0);
	case ATTEMPT_STATX_PATH:
#ifdef __linux__
		return (syscall(__NR_statx, AT_FDCWD, "/etc/passwd", 0,
		    STATX_BASIC_STATS, &extended) == -1 && errno == EPERM);
#else
		return (true);
#endif
	case ATTEMPT_STATX_EMPTY_PATH:
#ifdef __linux__
		return (syscall(__NR_statx, 5, "", AT_EMPTY_PATH,
		    STATX_MNT_ID, &extended) == -1 && errno == EPERM);
#else
		return (true);
#endif
	case ATTEMPT_STATX_CWD:
#ifdef __linux__
		return (syscall(__NR_statx, AT_FDCWD, NULL, AT_EMPTY_PATH,
		    STATX_MNT_ID, &extended) == -1 && errno == EPERM);
#else
		return (true);
#endif
	case ATTEMPT_STATX_HIGH_POINTER:
#ifdef __linux__
		return (syscall(__NR_statx, 5, (void *)(uintptr_t)(UINT64_C(1) << 32),
		    AT_EMPTY_PATH, STATX_MNT_ID, &extended) == -1 && errno == EPERM);
#else
		return (true);
#endif
	case ATTEMPT_STATX_HIGH_FLAGS:
#ifdef __linux__
		return (syscall(__NR_statx, 5, NULL, (UINT64_C(1) << 32) | AT_EMPTY_PATH,
		    STATX_MNT_ID, &extended) == -1 && errno == EPERM);
#else
		return (true);
#endif
	case ATTEMPT_OUTSIDE_OPEN:
		return (open("/etc/passwd", O_RDONLY) == -1);
	case ATTEMPT_PARENT_ESCAPE:
		return (openat(4, "../input", O_RDONLY) == -1);
	case ATTEMPT_INPUT_WRITE:
		return (pwrite(3, "X", 1, 0) == -1);
	case ATTEMPT_INPUT_CHMOD:
		return (fchmod(3, 0777) == -1);
	case ATTEMPT_INPUT_TIMES:
		return (futimens(3, NULL) == -1);
	case ATTEMPT_INPUT_TRUNCATE:
		return (ftruncate(3, 0) == -1);
	case ATTEMPT_SOURCE_WRITE:
		if (profile == PROFILE_FILE_ROOT)
			return (pwrite(5, "X", 1, 0) == -1 && ftruncate(5, 0) == -1);
		return (openat(5, "source", O_WRONLY | O_TRUNC) == -1);
	case ATTEMPT_NETWORK:
		fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd == -1)
			return (true);
		bzero(&address, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_port = htons(9);
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		result = connect(fd, (struct sockaddr *)&address, sizeof(address));
		close(fd);
		/* ECONNREFUSED would show that network authority survived. */
		return (result == -1 && (errno == EPERM || errno == EACCES
#ifdef __FreeBSD__
		    || errno == ECAPMODE || errno == ENOTCAPABLE
#endif
		    ));
	case ATTEMPT_EXEC:
		/* A permitted exec must exit unsuccessfully in the parent harness. */
		return (execve("/usr/bin/false", arguments, environment) == -1);
	case ATTEMPT_METADATA_QUERY:
#ifdef __linux__
		return (fstatat(AT_FDCWD, "/etc/passwd", &st, 0) == -1);
#else
		return (stat("/etc/passwd", &st) == -1);
#endif
	case ATTEMPT_FD_REOPEN:
		return (open("/proc/self/fd/3", O_RDWR) == -1);
	case ATTEMPT_INHERITED_FD:
		return (fcntl(10, F_GETFL) == -1 && errno == EBADF);
	case ATTEMPT_ALTERNATE_ABI:
#if defined(__linux__) && defined(__x86_64__)
		syscall(0x40000000u | __NR_getpid);
		return (false);
#else
		return (true);
#endif
	case ATTEMPT_COUNT:
		break;
	}
	return (false);
}

/* fds holds the input, work directory, directory root, file root and scratch
 * directory; the child installs them at the profile's numbers and enforces.
 */
static int
run_child(const int *fds, sandbox_attempt_t attempt, sandbox_profile_t profile)
{
	struct rlimit no_core = { 0, 0 };
	fsimg_sandbox_caps_t caps = {
		.input = 3, .staging = 4, .destination = -1, .scratch = 6,
		.roots = 5, .root_count = 1, .raw = -1, .spool = -1,
		.job = -1, .control = -1, .data = -1, .cancel = -1
	};
	fsimg_status_t status;
	pid_t child;
	int wait_status, root;

	if (profile == PROFILE_EXTRACT) {
		caps.staging = -1;
		caps.destination = 4;
		caps.roots = -1;
		caps.root_count = 0;
	}
	root = profile == PROFILE_FILE_ROOT ? fds[3] : fds[2];
	child = fork();
	CHECK(child != -1);
	if (child == 0) {
		if (setrlimit(RLIMIT_CORE, &no_core) == -1)
			_exit(1);
		if (dup2(fds[0], 3) == -1 || dup2(fds[1], 4) == -1)
			_exit(1);
		if (profile == PROFILE_EXTRACT)
			close(5);
		else if (dup2(root, 5) == -1)
			_exit(1);
		if (dup2(fds[4], 6) == -1)
			_exit(1);
		close_unlisted();
		status = fsimg_sandbox_install_caps(&caps, NULL);
		if (status == FSIMG_EUNSUPPORTED)
			_exit(77);
		if (status != FSIMG_OK)
			_exit(2);
		_exit(attempt_operation(attempt, profile) ? 0 : 1);
	}
	CHECK(waitpid(child, &wait_status, 0) == child);
	if (!WIFEXITED(wait_status)) {
#if defined(__linux__) && defined(__x86_64__)
		if (attempt == ATTEMPT_ALTERNATE_ABI &&
		    WIFSIGNALED(wait_status) && WTERMSIG(wait_status) == SIGSYS)
			return (0);
#endif
		fprintf(stderr, "sandbox attempt %d terminated by signal\n", attempt);
		return (1);
	}
	return (WEXITSTATUS(wait_status));
}

int
main(void)
{
	char path[] = "sandbox-fixture-XXXXXX";
	struct stat before, after;
	char sentinel;
	int parent, work, source, source_file, scratch, input, fd, result, skipped;
	bool enforce;

	CHECK(mkdtemp(path) != NULL);
	parent = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(parent != -1);
	CHECK(mkdirat(parent, "work", 0700) == 0);
	CHECK(mkdirat(parent, "scratch", 0700) == 0);
	CHECK(mkdirat(parent, "source-root", 0700) == 0);
	fd = openat(parent, "input", O_WRONLY | O_CREAT | O_EXCL, 0600);
	CHECK(fd != -1);
	CHECK(write(fd, "I", 1) == 1);
	CHECK(close(fd) == 0);
	fd = openat(parent, "input", O_RDONLY | O_CLOEXEC);
	CHECK(fd != -1);
	input = fcntl(fd, F_DUPFD_CLOEXEC, 10);
	CHECK(input != -1);
	CHECK(close(fd) == 0);
	CHECK(fstat(input, &before) == 0);
	fd = openat(parent, "work", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fd != -1);
	work = fcntl(fd, F_DUPFD_CLOEXEC, 10);
	CHECK(work != -1);
	CHECK(close(fd) == 0);
	fd = openat(parent, "scratch", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fd != -1);
	scratch = fcntl(fd, F_DUPFD_CLOEXEC, 10);
	CHECK(scratch != -1);
	CHECK(close(fd) == 0);
	fd = openat(parent, "source-root", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fd != -1);
	source = fcntl(fd, F_DUPFD_CLOEXEC, 10);
	CHECK(source != -1);
	CHECK(close(fd) == 0);
	fd = openat(source, "source", O_WRONLY | O_CREAT | O_EXCL, 0600);
	CHECK(fd != -1);
	CHECK(write(fd, "S", 1) == 1);
	CHECK(close(fd) == 0);
	fd = openat(source, "source", O_RDONLY | O_CLOEXEC);
	CHECK(fd != -1);
	source_file = fcntl(fd, F_DUPFD_CLOEXEC, 10);
	CHECK(source_file != -1);
	CHECK(close(fd) == 0);

	skipped = 0;
	enforce = false;
#if defined(__linux__) || defined(__FreeBSD__)
	enforce = true;
#endif
	if (!enforce) {
		CHECK(fsimg_sandbox_install(input, work, -1, NULL) == FSIMG_EUNSUPPORTED);
		skipped = 1;
	}
	for (sandbox_profile_t profile = 0; enforce && profile < PROFILE_COUNT;
	    profile++) {
		for (sandbox_attempt_t attempt = 0; attempt < ATTEMPT_COUNT; attempt++) {
			const int fds[] = { input, work, source, source_file, scratch };

			if (profile == PROFILE_EXTRACT && (attempt == ATTEMPT_SOURCE_READ ||
			    attempt == ATTEMPT_SOURCE_MOUNT || attempt == ATTEMPT_SOURCE_WRITE))
				continue;
			result = run_child(fds, attempt, profile);
			if (result == 77) {
				skipped++;
				continue;
			}
			if (result != 0)
				fprintf(stderr, "sandbox profile %u attempt %d failed (%d)\n",
				    profile, attempt, result);
			CHECK(result == 0);
			if (attempt == ATTEMPT_WORK_WRITE)
				CHECK(unlinkat(work, "output", 0) == 0);
			if (attempt == ATTEMPT_OUTPUT_REMOVE_FILE)
				CHECK(unlinkat(work, "delete-file", 0) == 0);
			if (attempt == ATTEMPT_WORK_MKDIR && profile == PROFILE_EXTRACT)
				CHECK(unlinkat(work, "work-directory", AT_REMOVEDIR) == 0);
		}
	}
	CHECK(fstat(input, &after) == 0);
	CHECK(before.st_size == after.st_size && before.st_mode == after.st_mode);
	CHECK(pread(input, &sentinel, 1, 0) == 1 && sentinel == 'I');
	CHECK(fstat(source_file, &after) == 0 && after.st_size == 1);
	CHECK(pread(source_file, &sentinel, 1, 0) == 1 && sentinel == 'S');
	CHECK(close(source_file) == 0);
	CHECK(unlinkat(source, "source", 0) == 0);
	CHECK(close(source) == 0);
	CHECK(close(scratch) == 0);
	CHECK(close(work) == 0);
	CHECK(close(input) == 0);
	CHECK(unlinkat(parent, "input", 0) == 0);
	CHECK(unlinkat(parent, "work", AT_REMOVEDIR) == 0);
	CHECK(unlinkat(parent, "scratch", AT_REMOVEDIR) == 0);
	CHECK(unlinkat(parent, "source-root", AT_REMOVEDIR) == 0);
	CHECK(close(parent) == 0);
	CHECK(rmdir(path) == 0);
	if (skipped != 0) {
		puts("SKIP: no supported sandbox profile; actual-kernel enforcement untested");
		return (77);
	}
	return (0);
}
