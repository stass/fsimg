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
#include <sys/syscall.h>
#endif

#include "sandbox.h"
#include "host.h"
#include "internal.h"
#include "test.h"

typedef enum sandbox_attempt {
	ATTEMPT_INPUT_READ,
	ATTEMPT_WORK_WRITE,
	ATTEMPT_SOURCE_READ,
	ATTEMPT_OUTSIDE_OPEN,
	ATTEMPT_PARENT_ESCAPE,
	ATTEMPT_INPUT_WRITE,
	ATTEMPT_INPUT_CHMOD,
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
static bool attempt_operation(sandbox_attempt_t);
static int run_child(int, int, int, sandbox_attempt_t, bool);

static void
close_unlisted(void)
{
	if (fsimg_host_closefrom(6) == -1)
		_exit(1);
	close(0);
	close(1);
	close(2);
}

/* Each attempt runs in its own child: an accidental successful exec, mutation,
 * or descriptor acquisition cannot mask any later negative assertion.
 */
static bool
attempt_operation(sandbox_attempt_t attempt)
{
	struct sockaddr_in address;
	struct stat st;
	char byte;
	char *arguments[] = { "true", NULL };
	char *environment[] = { NULL };
	int fd, result;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
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
	case ATTEMPT_SOURCE_READ:
		fd = openat(5, "source", O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
		if (fd == -1)
			return (false);
		result = (int)read(fd, &byte, 1);
		close(fd);
		return (result == 1 && byte == 'S');
	case ATTEMPT_OUTSIDE_OPEN:
		return (open("/etc/passwd", O_RDONLY) == -1);
	case ATTEMPT_PARENT_ESCAPE:
		return (openat(4, "../input", O_RDONLY) == -1);
	case ATTEMPT_INPUT_WRITE:
		return (pwrite(3, "X", 1, 0) == -1);
	case ATTEMPT_INPUT_CHMOD:
		return (fchmod(3, 0777) == -1);
	case ATTEMPT_INPUT_TRUNCATE:
		return (ftruncate(3, 0) == -1);
	case ATTEMPT_SOURCE_WRITE:
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
		return (execve("/usr/bin/true", arguments, environment) == -1);
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

static int
run_child(int input, int work, int source, sandbox_attempt_t attempt,
    bool source_profile)
{
	struct rlimit no_core = { 0, 0 };
	fsimg_status_t status;
	pid_t child;
	int wait_status;

	child = fork();
	CHECK(child != -1);
	if (child == 0) {
		if (setrlimit(RLIMIT_CORE, &no_core) == -1)
			_exit(1);
		if (dup2(input, 3) == -1)
			_exit(1);
		if (dup2(work, 4) == -1)
			_exit(1);
		if (source_profile) {
			if (dup2(source, 5) == -1)
				_exit(1);
		} else {
			close(5);
		}
		close_unlisted();
		status = fsimg_sandbox_install(3, 4, source_profile ? 5 : -1, NULL);
		if (status == FSIMG_EUNSUPPORTED)
			_exit(77);
		if (status != FSIMG_OK)
			_exit(2);
		_exit(attempt_operation(attempt) ? 0 : 1);
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
	int parent, work, source, input, fd, result, skipped;
	bool enforce;

	CHECK(mkdtemp(path) != NULL);
	parent = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(parent != -1);
	CHECK(mkdirat(parent, "work", 0700) == 0);
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
	fd = openat(parent, "source-root", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fd != -1);
	source = fcntl(fd, F_DUPFD_CLOEXEC, 10);
	CHECK(source != -1);
	CHECK(close(fd) == 0);
	fd = openat(source, "source", O_WRONLY | O_CREAT | O_EXCL, 0600);
	CHECK(fd != -1);
	CHECK(write(fd, "S", 1) == 1);
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
	for (unsigned profile = 0; enforce && profile < 2; profile++) {
		for (sandbox_attempt_t attempt = 0; attempt < ATTEMPT_COUNT; attempt++) {
			if (profile == 0 && (attempt == ATTEMPT_SOURCE_READ ||
			    attempt == ATTEMPT_SOURCE_WRITE))
				continue;
			result = run_child(input, work, source, attempt, profile != 0);
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
		}
	}
	CHECK(fstat(input, &after) == 0);
	CHECK(before.st_size == after.st_size && before.st_mode == after.st_mode);
	CHECK(pread(input, &sentinel, 1, 0) == 1 && sentinel == 'I');
	CHECK(unlinkat(source, "source", 0) == 0);
	CHECK(close(source) == 0);
	CHECK(close(work) == 0);
	CHECK(close(input) == 0);
	CHECK(unlinkat(parent, "input", 0) == 0);
	CHECK(unlinkat(parent, "work", AT_REMOVEDIR) == 0);
	CHECK(unlinkat(parent, "source-root", AT_REMOVEDIR) == 0);
	CHECK(close(parent) == 0);
	CHECK(rmdir(path) == 0);
	if (skipped != 0) {
		puts("SKIP: no supported sandbox profile; actual-kernel enforcement untested");
		return (77);
	}
	return (0);
}
