/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
#if defined(__FreeBSD__)
#include <sys/sysctl.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "host.h"

#if defined(__linux__)
#define FSIMG_CHILD_ASAN_PLATFORM_OPTIONS ":check_initialization_order=1"
#else
#define FSIMG_CHILD_ASAN_PLATFORM_OPTIONS ""
#endif

/* Child descriptor i receives descriptors[i]. A source numbered below count
 * could be the target of an earlier dup2 action, so it is first duplicated
 * at or above count; a source already there is never a target and is used
 * directly. No source then equals its target, so every target receives a
 * fresh duplicate without close-on-exec, and a parent holding many roots
 * above the child's layout needs no second copy of each.
 * Apple closes all unspecified FDs by spawn policy. Other hosts require their
 * closefrom file action; an unsupported host fails rather than guessing a loop
 * bound from a resource limit that can be below an already-open descriptor.
 */
static int
spawn_entry(const char *executable, const char *entry, const int *descriptors,
    unsigned count, pid_t *child)
{
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attributes;
	sigset_t defaults, mask;
	char *arguments[] = { (char *)executable, (char *)entry, NULL };
	/* These are fixed values, never inherited user configuration. Instrumented
	 * children cannot run leak-scanner tracing after confinement. Other memory
	 * checks stay active and use a reserved exit status visible to supervision.
	 */
	char *environment[] = {
		"LC_ALL=C", "LANG=C", "TZ=UTC",
		"ASAN_OPTIONS=detect_stack_use_after_return=1:strict_string_checks=1:"
		    "detect_leaks=0:abort_on_error=0:exitcode=86:symbolize=0"
		    FSIMG_CHILD_ASAN_PLATFORM_OPTIONS,
		"LSAN_OPTIONS=detect_leaks=0:abort_on_error=0:exitcode=86:symbolize=0",
		"UBSAN_OPTIONS=halt_on_error=1:abort_on_error=0:exitcode=86:symbolize=0",
		"MSAN_OPTIONS=halt_on_error=1:abort_on_error=0:exitcode=86:symbolize=0",
		NULL
	};
	int *copies, code;
	short flags;
	bool have_actions, have_attributes;

	if (count == 0 || count > INT_MAX || executable == NULL ||
	    executable[0] != '/') {
		errno = EINVAL;
		return (-1);
	}
	copies = calloc(count, sizeof(*copies));
	if (copies == NULL)
		return (-1);
	have_actions = have_attributes = false;
	for (unsigned i = 0; i < count; i++)
		copies[i] = -1;
	for (unsigned i = 0; i < count; i++) {
		if (descriptors[i] >= (int)count)
			continue;
		copies[i] = fcntl(descriptors[i], F_DUPFD_CLOEXEC, (int)count);
		if (copies[i] == -1) {
			code = errno;
			goto done;
		}
	}
	code = posix_spawn_file_actions_init(&actions);
	if (code != 0)
		goto done;
	have_actions = true;
	for (unsigned i = 0; i < count; i++) {
		code = posix_spawn_file_actions_adddup2(&actions,
		    copies[i] != -1 ? copies[i] : descriptors[i], (int)i);
		if (code != 0)
			goto done;
	}
	flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#if HAVE_DECL_POSIX_SPAWN_CLOEXEC_DEFAULT
	flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#elif HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDCLOSEFROM_NP
	code = posix_spawn_file_actions_addclosefrom_np(&actions, (int)count);
	if (code != 0)
		goto done;
#else
	code = ENOTSUP;
	goto done;
#endif
	code = posix_spawnattr_init(&attributes);
	if (code != 0)
		goto done;
	have_attributes = true;
	sigemptyset(&mask);
	sigemptyset(&defaults);
	sigaddset(&defaults, SIGHUP);
	sigaddset(&defaults, SIGINT);
	sigaddset(&defaults, SIGPIPE);
	sigaddset(&defaults, SIGQUIT);
	sigaddset(&defaults, SIGTERM);
	code = posix_spawnattr_setsigmask(&attributes, &mask);
	if (code != 0)
		goto done;
	code = posix_spawnattr_setsigdefault(&attributes, &defaults);
	if (code != 0)
		goto done;
	code = posix_spawnattr_setflags(&attributes, flags);
	if (code != 0)
		goto done;
	code = posix_spawn(child, executable, &actions, &attributes,
	    arguments, environment);
done:
	if (have_attributes)
		posix_spawnattr_destroy(&attributes);
	if (have_actions)
		posix_spawn_file_actions_destroy(&actions);
	for (unsigned i = 0; i < count; i++) {
		if (copies[i] != -1)
			close(copies[i]);
	}
	free(copies);
	if (code != 0) {
		errno = code;
		return (-1);
	}
	return (0);
}

int
fsimg_host_spawn_worker(const char *executable, const int *descriptors,
    unsigned count, pid_t *child)
{
	return (spawn_entry(executable, "--internal-worker", descriptors, count, child));
}

int
fsimg_host_spawn_output(const char *executable, const int *descriptors,
    unsigned count, pid_t *child)
{
	return (spawn_entry(executable, "--internal-output", descriptors, count, child));
}

int
fsimg_host_spawn_input(const char *executable, const int *descriptors,
    unsigned count, pid_t *child)
{
	return (spawn_entry(executable, "--internal-input", descriptors, count, child));
}

int
fsimg_host_executable(char *path, size_t capacity)
{
#if defined(__APPLE__)
	char *resolved;
	uint32_t size;

	if (capacity > UINT32_MAX) {
		errno = ERANGE;
		return (-1);
	}
	size = (uint32_t)capacity;
	if (_NSGetExecutablePath(path, &size) == -1) {
		errno = ENAMETOOLONG;
		return (-1);
	}
	/* Resolve relative loader paths before the worker switches to '/'. */
	resolved = realpath(path, NULL);
	if (resolved == NULL)
		return (-1);
	if (strlen(resolved) >= capacity) {
		free(resolved);
		errno = ENAMETOOLONG;
		return (-1);
	}
	strcpy(path, resolved);
	free(resolved);
	return (0);
#elif defined(__linux__)
	ssize_t length;

	length = readlink("/proc/self/exe", path, capacity);
	if (length == -1)
		return (-1);
	if ((size_t)length >= capacity) {
		errno = ENAMETOOLONG;
		return (-1);
	}
	path[length] = '\0';
	return (0);
#elif defined(__FreeBSD__)
	int mib[] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };

	return (sysctl(mib, 4, path, &capacity, NULL, 0));
#else
	errno = ENOTSUP;
	return (-1);
#endif
}
