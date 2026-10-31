/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#undef main
#undef waitpid

#include <sys/types.h>
#include <sys/resource.h>
#if defined(__FreeBSD__)
#include <sys/sysctl.h>
#include <sys/user.h>
#endif
#include <sys/wait.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "host.h"
#include "test.h"
#include "worker.h"

#if !defined(__FreeBSD__)
static uint64_t child_peaks;
static uint64_t children_reaped;
static bool accounting_failed;
#endif

int fsimg_measured_main(int, char **);
int fsimg_rss_qualification_main(int, char **);
pid_t fsimg_rss_waitpid(pid_t, int *, int);
#if defined(__FreeBSD__)
static void enter_vm_bound(int, char **, uint64_t);
static void check_vm_bound(void);
#else
static bool rss_bytes(const struct rusage *, uint64_t *);
#endif

#if defined(__FreeBSD__)
static void
check_vm_bound(void)
{
	struct kinfo_proc process;
	struct rlimit address;
	int mib[] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
	size_t length = sizeof(process);

	CHECK(geteuid() != 0 && getrlimit(RLIMIT_AS, &address) == 0);
	CHECK(address.rlim_cur > 0 && address.rlim_cur == address.rlim_max);
	CHECK(address.rlim_cur != RLIM_INFINITY);
	/* Exec's initial mappings do not all use the ordinary mmap limit check.
	 * Validate the complete initial map in every role before its work begins.
	 * Later mappings, heap growth and stack growth obey the inherited cap.
	 */
	CHECK(sysctl(mib, 4, &process, &length, NULL, 0) == 0);
	CHECK(length == sizeof(process) && process.ki_structsize == (int)sizeof(process));
	CHECK((uint64_t)process.ki_size <= (uint64_t)address.rlim_cur);
}

/* FreeBSD samples ru_maxrss on statistics-clock ticks. A short relay can
 * therefore have no recorded peak. Re-exec the qualification program with an
 * inherited, irrevocable virtual-memory ceiling instead. Every CLI process
 * inherits the same ceiling, so their sum bounds RSS even between samples.
 * The bootstrap is an observer, not part of the measured CLI execution.
 */
static void
enter_vm_bound(int argc, char **argv, uint64_t ceiling)
{
	struct rlimit address, stack;
	char executable[PATH_MAX];
	char **arguments;

	CHECK(geteuid() != 0);
	CHECK(ceiling <= (uint64_t)RLIM_INFINITY);
	CHECK(fsimg_host_executable(executable, sizeof(executable)) == 0);
	arguments = calloc((size_t)argc + 2, sizeof(*arguments));
	CHECK(arguments != NULL);
	arguments[0] = executable;
	arguments[1] = "--rss-vm-bound";
	for (int i = 1; i < argc; i++)
		arguments[i + 1] = argv[i];
	/* Bound stack growth as well as heap/mappings. The qualification
	 * workload uses shallow paths; this does not alter production defaults.
	 */
	stack.rlim_cur = stack.rlim_max = (rlim_t)(UINT64_C(8) << 20);
	address.rlim_cur = address.rlim_max = (rlim_t)ceiling;
	CHECK(setrlimit(RLIMIT_STACK, &stack) == 0);
	CHECK(setrlimit(RLIMIT_AS, &address) == 0);
	execv(executable, arguments);
	CHECK(false);
}
#else
static bool
rss_bytes(const struct rusage *usage, uint64_t *bytes)
{
	if (usage->ru_maxrss <= 0)
		return (false);
#if defined(__APPLE__)
	*bytes = (uint64_t)usage->ru_maxrss;
#else
	if ((uint64_t)usage->ru_maxrss > UINT64_MAX / 1024)
		return (false);
	*bytes = (uint64_t)usage->ru_maxrss * 1024;
#endif
	return (true);
}
#endif

/* The qualification build routes the existing supervisor's waitpid calls
 * here. Each terminal reap contributes that child's complete RSS peak once.
 * The child roles create no further processes. Collection never changes wait
 * results or errno, including when accounting itself cannot be represented.
 */
pid_t
fsimg_rss_waitpid(pid_t child, int *status, int options)
{
#if defined(__FreeBSD__)

	return (waitpid(child, status, options));
#else
	struct rusage usage;
	uint64_t peak;
	pid_t waited;
	int state, saved;

	waited = wait4(child, &state, options, &usage);
	saved = errno;
	if (waited > 0) {
		if (status != NULL)
			*status = state;
		if (WIFEXITED(state) || WIFSIGNALED(state)) {
			if (!rss_bytes(&usage, &peak) || child_peaks > UINT64_MAX - peak ||
			    children_reaped == UINT64_MAX)
				accounting_failed = true;
			else {
				child_peaks += peak;
				children_reaped++;
			}
		}
	}
	errno = saved;
	return (waited);
#endif
}

int
fsimg_rss_qualification_main(int argc, char **argv)
{
	uint64_t budget, combined, allowance;
	char **command, *end;
	int status;
	bool internal_child;
#if defined(__FreeBSD__)
	struct rlimit address, stack;
	uint64_t vm_ceiling;
	bool vm_bounded;
#else
	struct rusage usage;
	uint64_t coordinator;
#endif

	internal_child = argc == 2 && (strcmp(argv[1], "--internal-worker") == 0 ||
	    strcmp(argv[1], "--internal-output") == 0);
#if defined(__FreeBSD__)
	vm_bounded = argc > 1 && strcmp(argv[1], "--rss-vm-bound") == 0;
	if (internal_child || vm_bounded)
		check_vm_bound();
#endif
	/* Fresh same-executable children enter the production main unchanged. */
	if (internal_child)
		return (fsimg_measured_main(argc, argv));
#if defined(__FreeBSD__)
	if (vm_bounded) {
		argv[1] = argv[0];
		argv++;
		argc--;
	}
#endif
	CHECK(argc >= 3 && argv[1][0] != '\0');
	CHECK(strspn(argv[1], "0123456789") == strlen(argv[1]));
	errno = 0;
	budget = strtoull(argv[1], &end, 10);
	CHECK(errno == 0 && *end == '\0' && budget >= (UINT64_C(16) << 20));
	CHECK(budget <= UINT64_MAX - (UINT64_C(64) << 20));
	allowance = budget + (UINT64_C(64) << 20);
#if defined(__FreeBSD__)
	vm_ceiling = allowance / FSIMG_CLI_PROCESS_BOUND;
	if (!vm_bounded)
		enter_vm_bound(argc, argv, vm_ceiling);
	CHECK(getrlimit(RLIMIT_AS, &address) == 0 && getrlimit(RLIMIT_STACK, &stack) == 0);
	CHECK((uint64_t)address.rlim_cur == vm_ceiling && address.rlim_cur == address.rlim_max);
	CHECK(stack.rlim_cur == (rlim_t)(UINT64_C(8) << 20) && stack.rlim_cur == stack.rlim_max);
#endif
	command = calloc((size_t)argc + 2, sizeof(*command));
	CHECK(command != NULL);
	command[0] = argv[0];
	command[1] = argv[2];
	command[2] = "-m";
	command[3] = argv[1];
	for (int i = 3; i < argc; i++)
		command[i + 1] = argv[i];

	status = fsimg_measured_main(argc + 1, command);
	free(command);
#if defined(__FreeBSD__)
	CHECK(status == 0);
	combined = vm_ceiling * FSIMG_CLI_PROCESS_BOUND;
	CHECK(fprintf(stderr, "CLI RSS command=%s method=vm-ceiling"
	    " per_process_vm_ceiling=%" PRIu64 " process_bound=%u"
	    " combined_upper_bound=%" PRIu64 " budget_plus_allowance=%" PRIu64 "\n",
	    argv[2], vm_ceiling, FSIMG_CLI_PROCESS_BOUND, combined, allowance) >= 0);
#else
	if (status != 0 || accounting_failed || children_reaped == 0)
		fprintf(stderr, "RSS qualification status=%d accounting_failed=%s reaped=%" PRIu64
		    " child_peaks=%" PRIu64 "\n", status, accounting_failed ? "true" : "false",
		    children_reaped, child_peaks);
	CHECK(status == 0 && !accounting_failed && children_reaped != 0);
	CHECK(getrusage(RUSAGE_SELF, &usage) == 0 && rss_bytes(&usage, &coordinator));
	CHECK(coordinator <= UINT64_MAX - child_peaks);
	combined = coordinator + child_peaks;
	/* Sum peaks even across sequential relay phases. This is a conservative
	 * simultaneous-RSS bound, with no sampling interval that can miss a peak.
	 */
	CHECK(fprintf(stderr, "CLI RSS command=%s coordinator_peak=%" PRIu64
	    " child_peaks_sum=%" PRIu64 " children_reaped=%" PRIu64
	    " combined_upper_bound=%" PRIu64 " budget_plus_allowance=%" PRIu64
	    "\n", argv[2], coordinator, child_peaks, children_reaped,
	    combined, allowance) >= 0);
#endif
	CHECK(fflush(stderr) == 0 && combined <= allowance);
	return (0);
}

#ifndef FSIMG_RSS_GUARD
int
main(int argc, char **argv)
{
	return (fsimg_rss_qualification_main(argc, argv));
}
#endif
