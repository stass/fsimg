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
#include <string.h>
#include <strings.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/stat.h>
#include <sys/syscall.h>
#endif

#include "sandbox.h"
#include "host.h"
#include "internal.h"
#include "source.h"
#include "test.h"
#include "writer.h"

/* The worker's three descriptor profiles: a read job writing beneath an
 * extraction destination, and a create job writing a staged image from one
 * directory root or one regular-file root. Every profile places the input at
 * 3, the writable work directory at 4, a root (when present) at 5 and the
 * scratch directory at 6; the read job also holds the write-only staged
 * manifest of x --mtree at 7.
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
	ATTEMPT_STATX_PATH,
	ATTEMPT_STATX_DESCRIPTOR,
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
	ATTEMPT_MANIFEST_WRITE,
	ATTEMPT_MANIFEST_REOPEN,
	ATTEMPT_MANIFEST_METADATA,
	ATTEMPT_MANIFEST_NAMESPACE,
	ATTEMPT_COUNT
} sandbox_attempt_t;

static void close_unlisted(void);
static bool read_source(sandbox_profile_t);
static bool write_manifest(void);
static bool manifest_attempt(sandbox_attempt_t);
static bool attempt_operation(sandbox_attempt_t, sandbox_profile_t);
static int run_child(const int *, sandbox_attempt_t, sandbox_profile_t, bool);
static bool manifest_denial(sandbox_attempt_t);
static int refuse_readable_manifest(const int *, int);

static void
close_unlisted(void)
{
	if (fsimg_host_closefrom(8) == -1)
		_exit(1);
	close(0);
	close(1);
	close(2);
}

/* Read the root's one file, "source" beneath a directory root or a file
 * root itself, through the library as a confined worker does: adopt the
 * scratch directory and the root, catalog the root and copy the file's
 * first byte from its source. A file root is read through the root
 * descriptor the writer keeps, which confinement must allow without
 * duplicating it.
 */
static bool
read_source(sandbox_profile_t profile)
{
	const fsimg_catalog_entry_t *entry;
	const char *image_path;
	fsimg_writer_t *writer;
	fsimg_ctx_t *ctx;
	size_t transferred;
	char byte;

	if (fsimg_ctx_new(NULL, &ctx, NULL) != FSIMG_OK)
		return (false);
	if (fsimg_fd_reserve(ctx, NULL) != FSIMG_OK)
		return (false);
	if (fsimg_ctx_adopt_scratch(ctx, 6, NULL) != FSIMG_OK)
		return (false);
	if (fsimg_writer_collect_new(ctx, &writer, NULL) != FSIMG_OK)
		return (false);

	image_path = profile == PROFILE_FILE_ROOT ? "/source" : "/";
	if (fsimg_fd_reserve(ctx, NULL) != FSIMG_OK)
		return (false);
	if (fsimg_writer_adopt_tree(writer, 5, image_path, NULL) != FSIMG_OK)
		return (false);
	if (fsimg_writer_prepare(writer, NULL, NULL) != FSIMG_OK)
		return (false);
	/* Entry 0 is the image root; the file is the only other entry. */
	if (fsimg_catalog_get(writer, 1, &entry, NULL) != FSIMG_OK ||
	    strcmp(entry->entry.path, "/source") != 0)
		return (false);

	if (fsimg_source_open(writer, 1, NULL) != FSIMG_OK)
		return (false);
	if (fsimg_source_read(writer, 0, &byte, 1, &transferred,
	    NULL) != FSIMG_OK)
		return (false);
	if (fsimg_source_finish(writer, NULL) != FSIMG_OK)
		return (false);
	fsimg_writer_free(writer);
	fsimg_ctx_free(ctx);
	return (transferred == 1 && byte == 'S');
}

/* Write the staged manifest as the extractor does: query its access mode
 * and identity, then write through a borrowed descriptor with a resize, a
 * write and a durable flush.
 */
static bool
write_manifest(void)
{
	struct stat st;
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	size_t transferred;
	int flags;

	flags = fcntl(7, F_GETFL);
	if (flags == -1 || (flags & O_ACCMODE) != O_WRONLY || fstat(7, &st) == -1 ||
	    !S_ISREG(st.st_mode))
		return (false);
	if (fsimg_ctx_new(NULL, &ctx, NULL) != FSIMG_OK)
		return (false);
	if (fsimg_io_borrow_fd(ctx, 7, FSIMG_IO_WRITE, &io, NULL) != FSIMG_OK)
		return (false);
	if (fsimg_io_resize(io, 1, NULL) != FSIMG_OK)
		return (false);
	if (fsimg_io_write_exact(io, 0, "M", 1, &transferred, NULL) != FSIMG_OK)
		return (false);
	if (fsimg_io_flush(io, true, NULL) != FSIMG_OK)
		return (false);
	fsimg_io_free(io);
	fsimg_ctx_free(ctx);
	return (transferred == 1);
}

/* The staged manifest grants write, resize and flush and nothing else: it
 * cannot be reopened with other access or flags, its mode, owner and times
 * cannot change, and it cannot gain a name. That it cannot be read follows
 * from its write-only access mode, which the sandbox requires
 * (refuse_readable_manifest). Each denial returns true.
 */
static bool
manifest_attempt(sandbox_attempt_t attempt)
{
	switch (attempt) {
	case ATTEMPT_MANIFEST_WRITE:
		return (write_manifest());
	case ATTEMPT_MANIFEST_REOPEN:
		return (open("/proc/self/fd/7", O_RDWR) == -1 &&
		    open("/dev/fd/7", O_RDONLY) == -1 &&
		    fcntl(7, F_SETFL, O_APPEND) == -1);
	case ATTEMPT_MANIFEST_METADATA:
		return (fchmod(7, 0644) == -1 && fchown(7, getuid(), getgid()) == -1 &&
		    futimens(7, NULL) == -1);
	case ATTEMPT_MANIFEST_NAMESPACE:
#ifdef AT_EMPTY_PATH
		return (linkat(7, "", 4, "manifest-link", AT_EMPTY_PATH) == -1);
#else
		return (true);
#endif
	default:
		return (false);
	}
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
		return (read_source(profile));
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
	case ATTEMPT_STATX_PATH:
#ifdef __linux__
		return (syscall(__NR_statx, AT_FDCWD, "/etc/passwd", 0,
		    STATX_BASIC_STATS, &extended) == -1 && errno == EPERM);
#else
		return (true);
#endif
	case ATTEMPT_STATX_DESCRIPTOR:
		/* Even a descriptor-only statx is denied; fstat serves. */
#ifdef __linux__
		return (syscall(__NR_statx, 5, NULL, AT_EMPTY_PATH,
		    STATX_BASIC_STATS, &extended) == -1 && errno == EPERM);
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
	case ATTEMPT_MANIFEST_WRITE:
	case ATTEMPT_MANIFEST_REOPEN:
	case ATTEMPT_MANIFEST_METADATA:
	case ATTEMPT_MANIFEST_NAMESPACE:
		return (manifest_attempt(attempt));
	case ATTEMPT_COUNT:
		break;
	}
	return (false);
}

/* The attempts denied only by the sandbox, each first run unconfined as a
 * control that must succeed, so the denial is shown to be the sandbox's.
 */
static bool
manifest_denial(sandbox_attempt_t attempt)
{
	return (attempt == ATTEMPT_MANIFEST_REOPEN ||
	    attempt == ATTEMPT_MANIFEST_METADATA ||
	    attempt == ATTEMPT_MANIFEST_NAMESPACE);
}

/* The sandbox refuses a manifest descriptor that could read: fd, opened
 * for reading and writing, takes the manifest's place among fds. It
 * fails before anything is installed, so a confining host is not needed.
 */
static int
refuse_readable_manifest(const int *fds, int fd)
{
	fsimg_sandbox_caps_t caps;
	pid_t child;
	int wait_status;

	fsimg_sandbox_caps_init(&caps);
	caps.input = 3;
	caps.destination = 4;
	caps.scratch = 6;
	caps.manifest = 7;
	child = fork();
	CHECK(child != -1);
	if (child == 0) {
		if (dup2(fds[0], 3) == -1 || dup2(fds[1], 4) == -1 ||
		    dup2(fds[4], 6) == -1 || dup2(fd, 7) == -1)
			_exit(1);
		_exit(fsimg_sandbox_install_caps(&caps, NULL) == FSIMG_EINVAL ? 0 : 1);
	}
	CHECK(waitpid(child, &wait_status, 0) == child);
	return (WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : 1);
}

/* fds holds the input, work directory, directory root, file root, scratch
 * directory and staged manifest; the child installs them at the profile's
 * numbers and, when confine is set, enforces.
 */
static int
run_child(const int *fds, sandbox_attempt_t attempt, sandbox_profile_t profile,
    bool confine)
{
	struct rlimit no_core = { 0, 0 };
	fsimg_sandbox_caps_t caps;
	fsimg_status_t status;
	pid_t child;
	int wait_status, root;

	fsimg_sandbox_caps_init(&caps);
	caps.input = 3;
	caps.staging = 4;
	caps.scratch = 6;
	caps.roots = 5;
	caps.root_count = 1;
	if (profile == PROFILE_EXTRACT) {
		caps.staging = -1;
		caps.destination = 4;
		caps.roots = -1;
		caps.root_count = 0;
		caps.manifest = 7;
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
		if (profile == PROFILE_EXTRACT && dup2(fds[5], 7) == -1)
			_exit(1);
		if (profile != PROFILE_EXTRACT)
			close(7);
		close_unlisted();
		if (!confine)
			_exit(attempt_operation(attempt, profile) ? 0 : 1);
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

#ifdef __FreeBSD__
/*
 * The Capsicum sync rights the worker installs: the raw intermediate
 * keeps CAP_FSYNC for the writer's flush of its sink, while the spool,
 * which answers its zero ranges from its own record and is never flushed,
 * and the read job's image input, which no read job asks for zero ranges,
 * cannot sync in capability mode. Returns 77 when the kernel lacks
 * Capsicum.
 */
static int
test_sync_rights(int parent, int input)
{
	struct rlimit no_core = { 0, 0 };
	fsimg_status_t status;
	pid_t child;
	int raw, spool, wait_status;

	raw = openat(parent, "raw", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	spool = openat(parent, "spool", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC,
	    0600);
	CHECK(raw != -1 && spool != -1);
	CHECK(unlinkat(parent, "raw", 0) == 0 && unlinkat(parent, "spool", 0) == 0);
	child = fork();
	CHECK(child != -1);
	if (child == 0) {
		fsimg_sandbox_caps_t caps = {
			.input = 5, .staging = -1, .destination = -1,
			.scratch = -1, .roots = -1, .root_count = 0, .raw = 3,
			.spool = 4, .job = -1, .control = -1, .data = -1,
			.cancel = -1
		};

		if (setrlimit(RLIMIT_CORE, &no_core) == -1 ||
		    dup2(raw, 3) == -1 || dup2(spool, 4) == -1 ||
		    dup2(input, 5) == -1)
			_exit(1);
		close_unlisted();
		status = fsimg_sandbox_install_caps(&caps, NULL);
		if (status == FSIMG_EUNSUPPORTED)
			_exit(77);
		if (status != FSIMG_OK)
			_exit(2);
		if (fsync(3) != 0)
			_exit(3);
		if (fsync(4) != -1 || errno != ENOTCAPABLE)
			_exit(4);
		if (fsync(5) != -1 || errno != ENOTCAPABLE)
			_exit(5);
		_exit(0);
	}
	CHECK(waitpid(child, &wait_status, 0) == child);
	CHECK(WIFEXITED(wait_status) && (WEXITSTATUS(wait_status) == 0 ||
	    WEXITSTATUS(wait_status) == 77));
	CHECK(close(raw) == 0 && close(spool) == 0);
	return (WEXITSTATUS(wait_status));
}
#endif

int
main(void)
{
	char path[] = "sandbox-fixture-XXXXXX";
	struct stat before, after;
	char sentinel;
	unsigned unsupported_profiles;
	int parent, work, source, source_file, scratch, input, manifest, readable;
	int fd, result;
	bool enforce, unsupported;

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
	fd = openat(parent, "manifest", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	CHECK(fd != -1);
	manifest = fcntl(fd, F_DUPFD_CLOEXEC, 10);
	CHECK(manifest != -1);
	CHECK(close(fd) == 0);
	fd = openat(parent, "manifest", O_RDWR | O_CLOEXEC);
	CHECK(fd != -1);
	readable = fcntl(fd, F_DUPFD_CLOEXEC, 10);
	CHECK(readable != -1);
	CHECK(close(fd) == 0);
	{
		const int fds[] = { input, work, source, source_file, scratch };

		CHECK(refuse_readable_manifest(fds, readable) == 0);
		/* The control: the write-only manifest passes the same check. */
		CHECK(refuse_readable_manifest(fds, manifest) == 1);
	}
	CHECK(close(readable) == 0);

	/* A profile the kernel cannot confine, such as any profile on a
	 * Linux kernel without Landlock ABI 3, skips each of its attempts;
	 * the others still run.
	 */
	unsupported_profiles = 0;
	enforce = false;
#if defined(__linux__) || defined(__FreeBSD__)
	enforce = true;
#endif
	if (!enforce) {
		CHECK(fsimg_sandbox_install(input, work, -1, NULL) == FSIMG_EUNSUPPORTED);
		unsupported_profiles = PROFILE_COUNT;
	}
	for (sandbox_profile_t profile = 0; enforce && profile < PROFILE_COUNT;
	    profile++) {
		unsupported = false;
		for (sandbox_attempt_t attempt = 0; attempt < ATTEMPT_COUNT; attempt++) {
			const int fds[] = { input, work, source, source_file, scratch,
			    manifest };

			if (profile == PROFILE_EXTRACT && (attempt == ATTEMPT_SOURCE_READ ||
			    attempt == ATTEMPT_SOURCE_WRITE))
				continue;
			if (profile != PROFILE_EXTRACT && attempt >= ATTEMPT_MANIFEST_WRITE)
				continue;
			/* The control: unconfined, the operation is not denied.
			 * An unprivileged linkat of a descriptor is refused by
			 * some kernels anyway, where that attempt shows nothing.
			 */
			if (manifest_denial(attempt) &&
			    run_child(fds, attempt, profile, false) == 0) {
				CHECK(attempt == ATTEMPT_MANIFEST_NAMESPACE);
				printf("note: linkat of a descriptor is refused unconfined "
				    "here; the namespace attempt shows nothing\n");
			}
			/* Undo what a control changed: the shared flags, the
			 * mode and a new name.
			 */
			if (manifest_denial(attempt)) {
				CHECK(fcntl(manifest, F_SETFL, 0) == 0);
				CHECK(fchmod(manifest, 0600) == 0);
				if (unlinkat(work, "manifest-link", 0) == -1)
					CHECK(errno == ENOENT);
			}
			result = run_child(fds, attempt, profile, true);
			if (result == 77) {
				unsupported = true;
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
			if (attempt == ATTEMPT_MANIFEST_WRITE) {
				CHECK(fstat(manifest, &after) == 0 && after.st_size == 1);
				CHECK(ftruncate(manifest, 0) == 0);
			}
		}
		if (unsupported)
			unsupported_profiles++;
	}
#ifdef __FreeBSD__
	if (test_sync_rights(parent, input) == 77)
		skipped++;
#endif
	CHECK(fstat(input, &after) == 0);
	CHECK(before.st_size == after.st_size && before.st_mode == after.st_mode);
	CHECK(pread(input, &sentinel, 1, 0) == 1 && sentinel == 'I');
	CHECK(fstat(source_file, &after) == 0 && after.st_size == 1);
	CHECK(pread(source_file, &sentinel, 1, 0) == 1 && sentinel == 'S');
	CHECK(close(source_file) == 0);
	CHECK(unlinkat(source, "source", 0) == 0);
	CHECK(close(source) == 0);
	CHECK(close(scratch) == 0);
	CHECK(fstat(manifest, &after) == 0 && after.st_size == 0 && after.st_nlink == 1);
	CHECK((after.st_mode & 07777) == 0600);
	CHECK(close(manifest) == 0);
	CHECK(faccessat(work, "manifest-link", F_OK, AT_SYMLINK_NOFOLLOW) == -1);
	CHECK(unlinkat(parent, "manifest", 0) == 0);
	CHECK(close(work) == 0);
	CHECK(close(input) == 0);
	CHECK(unlinkat(parent, "input", 0) == 0);
	CHECK(unlinkat(parent, "work", AT_REMOVEDIR) == 0);
	CHECK(unlinkat(parent, "scratch", AT_REMOVEDIR) == 0);
	CHECK(unlinkat(parent, "source-root", AT_REMOVEDIR) == 0);
	CHECK(close(parent) == 0);
	CHECK(rmdir(path) == 0);
	if (unsupported_profiles != 0) {
		printf("SKIP: %u of %u sandbox profiles unsupported on this host; "
		    "their actual-kernel enforcement is untested\n",
		    unsupported_profiles, (unsigned)PROFILE_COUNT);
		return (77);
	}
	return (0);
}
