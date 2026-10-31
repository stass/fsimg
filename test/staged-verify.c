/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
/*
 * A whole cW whose staged image fails verification publishes nothing, leaves
 * no staging or scratch behind, and names the layer that failed. This program
 * is a test-only build of the command: Makefile.am renames the production main
 * to fsimg_test_command_main. The driver runs its own executable as the command,
 * and the command's workers are this executable too, so the creation worker can
 * flip a staged byte after reporting DONE without a hook in the shipped fsimg.
 * The CORRUPT marker switches between fault and control runs.
 */
#include "config.h"

#undef main

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "host.h"
#include "test.h"
#include "worker.h"

typedef struct verification_case {
	const char	*stack;
	const char	*output;
	const char	*verdict;
} verification_case_t;

static const verification_case_t cases[] = {
	{
		.stack = "fat12",
		.output = "volume.img",
		.verdict = "fsimg: the staged fat12 volume failed verification; nothing was published\n"
	},
	{
		.stack = "fat12/gpt/dmg",
		.output = "container.dmg",
		.verdict = "fsimg: the staged dmg container failed verification; nothing was published\n"
	},
	{
		.stack = "fat12/gpt",
		.output = "compressed.img.gz",
		.verdict = "fsimg: the staged gzip compressed image failed verification; nothing was published\n"
	}
};

int fsimg_test_command_main(int, char **);
static int fault_worker(void);
static int capture_file(int, const char *);
static bool fixture_clean(int);
static void remove_tree(int, const char *);
static bool run_case(int, int, char *, const verification_case_t *, bool);
static void test_staged_verification(void);

static int
fault_worker(void)
{
	struct stat output_st, root_st;
	unsigned char byte;
	int staging, marker, image, status;
	bool corrupt;

	staging = -1;
	corrupt = false;
	CHECK(fsimg_host_fstat(FSIMG_CHILD_OUTPUT, &output_st) == 0);
	/* An absent capability occupies a /dev/null slot until worker setup.
	 * Among the cW workers here, creation holds both a staging directory
	 * and a source root.
	 */
	if (S_ISDIR(output_st.st_mode) &&
	    fsimg_host_fstat(FSIMG_CHILD_ROOTS, &root_st) == 0 &&
	    S_ISDIR(root_st.st_mode)) {
		staging = fcntl(FSIMG_CHILD_OUTPUT, F_DUPFD_CLOEXEC,
		    FSIMG_CHILD_ROOTS + 1);
		CHECK(staging != -1);
		/* The marker enables faults without a production hook or inherited
		 * environment. Opening it leaves the source directory offset alone;
		 * the unmarked source exercises the same command as a control.
		 */
		marker = openat(FSIMG_CHILD_ROOTS, "CORRUPT", O_RDONLY |
		    O_NOFOLLOW | O_CLOEXEC);
		if (marker == -1)
			CHECK(errno == ENOENT);
		else {
			corrupt = true;
			CHECK(close(marker) == 0);
		}
	}

	status = fsimg_worker_main();
	/* The worker sends DONE before returning, and the coordinator waits
	 * for its exit before verification. The duplicate survives worker
	 * cleanup, so this same-length edit lands between the two workers.
	 */
	if (status == 0 && staging != -1 && corrupt) {
		image = openat(staging, "image", O_RDWR | O_NOFOLLOW | O_CLOEXEC);
		CHECK(image != -1);
		/* Offset 512 is a reserved FAT entry in the raw fixture and is
		 * compressed data in both wrapped fixtures. The FAT mirror or
		 * the wrapper's integrity checks detect the changed byte.
		 */
		CHECK(pread(image, &byte, 1, 512) == 1);
		byte ^= 0xff;
		CHECK(pwrite(image, &byte, 1, 512) == 1);
		CHECK(close(image) == 0);
	}
	if (staging != -1)
		CHECK(close(staging) == 0);
	return (status);
}

/* Unlinked captures keep command output out of the fixture directory scan. */
static int
capture_file(int directory, const char *name)
{
	int fd;

	fd = openat(directory, name, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	CHECK(fd != -1);
	CHECK(unlinkat(directory, name, 0) == 0);
	return (fd);
}

static bool
fixture_clean(int directory)
{
	struct dirent *entry;
	DIR *scan;
	int fd;
	bool clean;

	fd = openat(directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fd != -1);
	scan = fdopendir(fd);
	CHECK(scan != NULL);
	clean = true;
	errno = 0;
	while ((entry = readdir(scan)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0 ||
		    strcmp(entry->d_name, "source") == 0)
			continue;
		fprintf(stderr, "unexpected fixture entry: %s\n", entry->d_name);
		clean = false;
	}
	CHECK(errno == 0);
	CHECK(closedir(scan) == 0);
	return (clean);
}

/* Cleanup is confined to the disposable fixture, after residuals are reported. */
static void
remove_tree(int parent, const char *name)
{
	struct dirent *entry;
	struct stat st;
	DIR *scan;
	int fd;

	fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	CHECK(fd != -1);
	scan = fdopendir(fd);
	CHECK(scan != NULL);
	for (;;) {
		errno = 0;
		entry = readdir(scan);
		if (entry == NULL) {
			CHECK(errno == 0);
			break;
		}
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		CHECK(fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0);
		if (S_ISDIR(st.st_mode))
			remove_tree(fd, entry->d_name);
		else
			CHECK(unlinkat(fd, entry->d_name, 0) == 0);
	}
	CHECK(closedir(scan) == 0);
	CHECK(unlinkat(parent, name, AT_REMOVEDIR) == 0);
}

/*
 * One run asserts exit 1, the exact verdict at a line start, and no publication
 * for a fault; exit 0 and a published regular nonempty image for a control.
 * Both require empty stdout and a fixture holding only source after cleanup.
 */
static bool
run_case(int directory, int source, char *executable,
    const verification_case_t *scenario, bool corrupt)
{
	/* -a picks gzip from .gz and nothing for the other names; -T . puts the
	 * scratch tree in the fixture, so cleanliness also checks scratch cleanup.
	 */
	char *const arguments[] = { executable, "cWf", (char *)scenario->output,
	    "-F", (char *)scenario->stack, "-a", "-T", ".", "-C", "source", ".", NULL };
	struct stat st;
	struct timespec deadline, now, delay = { .tv_nsec = 20000000 };
	char diagnostic[8192], output_byte;
	const char *verdict;
	ssize_t count, output_count;
	pid_t child, waited;
	int output, errors, marker, status, exit_status;
	bool published, passed, clean, timed_out;

	if (corrupt) {
		marker = openat(source, "CORRUPT", O_WRONLY | O_CREAT | O_EXCL |
		    O_CLOEXEC, 0600);
		CHECK(marker != -1);
		CHECK(close(marker) == 0);
	}
	output = capture_file(directory, "stdout");
	errors = capture_file(directory, "stderr");
	/* The driver bounds the command and its workers as one process group. */
	CHECK(clock_gettime(CLOCK_MONOTONIC, &deadline) == 0);
	deadline.tv_sec += 30;
	child = fork();
	CHECK(child != -1);
	if (child == 0) {
		if (setpgid(0, 0) != 0)
			_exit(127);
		if (fchdir(directory) != 0)
			_exit(127);
		if (dup2(output, STDOUT_FILENO) != STDOUT_FILENO)
			_exit(127);
		if (dup2(errors, STDERR_FILENO) != STDERR_FILENO)
			_exit(127);
		execv(executable, arguments);
		_exit(127);
	}
	/* Either side can establish the group before the child execs. */
	CHECK(setpgid(child, child) == 0 || errno == EACCES);
	for (;;) {
		waited = waitpid(child, &status, WNOHANG);
		CHECK(waited != -1 || errno == EINTR);
		if (waited == child)
			break;
		CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
		if (now.tv_sec > deadline.tv_sec ||
		    (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
			break;
		CHECK(nanosleep(&delay, NULL) == 0 || errno == EINTR);
	}
	timed_out = waited != child;
	if (timed_out) {
		CHECK(killpg(child, SIGKILL) == 0 || errno == ESRCH);
		do {
			waited = waitpid(child, &status, 0);
		} while (waited == -1 && errno == EINTR);
		CHECK(waited == child);
		fprintf(stderr, "%s %s: command timed out after 30 seconds\n",
		    scenario->stack, corrupt ? "fault" : "control");
	}
	/* Stop workers left behind by an early command exit before cleanup. */
	CHECK(killpg(child, SIGKILL) == 0 || errno == ESRCH);
	count = pread(errors, diagnostic, sizeof(diagnostic) - 1, 0);
	CHECK(count >= 0 && (size_t)count < sizeof(diagnostic) - 1);
	diagnostic[count] = '\0';
	output_count = pread(output, &output_byte, 1, 0);
	CHECK(output_count >= 0);
	CHECK(close(errors) == 0);
	CHECK(close(output) == 0);
	if (corrupt)
		CHECK(unlinkat(source, "CORRUPT", 0) == 0);

	exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	passed = !timed_out && output_count == 0;
	published = fstatat(directory, scenario->output, &st,
	    AT_SYMLINK_NOFOLLOW) == 0;
	if (!published)
		CHECK(errno == ENOENT);
	if (corrupt) {
		verdict = strstr(diagnostic, scenario->verdict);
		if (exit_status != 1 || verdict == NULL ||
		    (verdict != diagnostic && verdict[-1] != '\n') ||
		    published)
			passed = false;
	} else if (exit_status != 0 || !published || !S_ISREG(st.st_mode) ||
	    st.st_size == 0) {
		passed = false;
	}
	if (published)
		CHECK(unlinkat(directory, scenario->output, 0) == 0);
	clean = fixture_clean(directory);
	if (!passed || !clean) {
		if (corrupt)
			fprintf(stderr, "%s fault: exit=%d; expected exit 1, no "
			    "publication, empty stdout, only source in fixture, and "
			    "verdict at a line start: %.*s\n%s", scenario->stack,
			    exit_status, (int)strcspn(scenario->verdict, "\n"),
			    scenario->verdict, diagnostic);
		else
			fprintf(stderr, "%s control: exit=%d; expected exit 0, a "
			    "published regular nonempty image, empty stdout, and "
			    "only source in fixture\n%s", scenario->stack,
			    exit_status, diagnostic);
	}
	return (passed && clean);
}

static void
test_staged_verification(void)
{
	char fixture[] = "staged-verify-XXXXXX", executable[PATH_MAX];
	int directory, source, payload;
	bool passed;

	CHECK(fsimg_host_executable(executable, sizeof(executable)) == 0);
	CHECK(mkdtemp(fixture) != NULL);
	directory = open(fixture, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(directory != -1);
	CHECK(mkdirat(directory, "source", 0700) == 0);
	source = openat(directory, "source", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(source != -1);
	payload = openat(source, "HELLO.TXT", O_WRONLY | O_CREAT | O_EXCL |
	    O_CLOEXEC, 0600);
	CHECK(payload != -1);
	CHECK(write(payload, "payload", 7) == 7);
	CHECK(close(payload) == 0);

	passed = true;
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		passed = run_case(directory, source, executable, &cases[i], false);
		if (!passed)
			break;
		passed = run_case(directory, source, executable, &cases[i], true);
		if (!passed)
			break;
	}
	CHECK(close(source) == 0);
	CHECK(close(directory) == 0);
	remove_tree(AT_FDCWD, fixture);
	CHECK(passed);
}

int
main(int argc, char **argv)
{
	if (argc == 1) {
		test_staged_verification();
		return (0);
	}
	if (strcmp(argv[1], "--internal-worker") == 0)
		return (fault_worker());
	return (fsimg_test_command_main(argc, argv));
}
