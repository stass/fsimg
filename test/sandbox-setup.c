/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "sandbox.h"
#include "test.h"

#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/landlock.h>

#if (defined(__x86_64__) || defined(__aarch64__)) && \
    defined(LANDLOCK_ACCESS_FS_TRUNCATE)
typedef enum setup_step {
	SETUP_ABI,
	SETUP_RULESET,
	SETUP_RULE,
	SETUP_NO_NEW_PRIVS,
	SETUP_RESTRICT,
	SETUP_FILTER,
	SETUP_COMPLETE
} setup_step_t;

static setup_step_t failing_step;
static int failure_errno, abi_result, create_calls, work_root;

static int fail_at(setup_step_t);
static void check_setup(setup_step_t, int, int, fsimg_status_t);

/* This target alone renames the two setup entry points at compile time.
 * The mocks never consume variadic arguments: operation identity and call
 * order suffice, and no kernel restriction is installed in the test process.
 */
static int
fail_at(setup_step_t step)
{
	if (step != failing_step)
		return (0);
	errno = failure_errno;
	return (-1);
}

long
fsimg_test_syscall(long number, ...)
{
	if (number == __NR_landlock_create_ruleset) {
		if (create_calls++ == 0) {
			if (fail_at(SETUP_ABI) == -1)
				return (-1);
			return (abi_result);
		}
		if (fail_at(SETUP_RULESET) == -1)
			return (-1);
		/* Production closes this descriptor on every later outcome. */
		return (fcntl(work_root, F_DUPFD_CLOEXEC, 32));
	}
	if (number == __NR_landlock_add_rule)
		return (fail_at(SETUP_RULE));
	CHECK(number == __NR_landlock_restrict_self);
	return (fail_at(SETUP_RESTRICT));
}

int
fsimg_test_prctl(int option, ...)
{
	if (option == PR_SET_NO_NEW_PRIVS)
		return (fail_at(SETUP_NO_NEW_PRIVS));
	CHECK(option == PR_SET_SECCOMP);
	return (fail_at(SETUP_FILTER));
}

static void
check_setup(setup_step_t step, int failure, int abi, fsimg_status_t expected)
{
	fsimg_error_t error;

	failing_step = step;
	failure_errno = failure;
	abi_result = abi;
	create_calls = 0;
	CHECK(fsimg_sandbox_install(-1, work_root, -1, &error) == expected);
	CHECK(error.status == expected);
	if (expected != FSIMG_OK)
		CHECK(error.os_error == failure);
}

int
main(void)
{
	char directory[] = "sandbox-setup-XXXXXX";

	CHECK(mkdtemp(directory) != NULL);
	work_root = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(work_root != -1);
	check_setup(SETUP_ABI, ENOSYS, 3, FSIMG_EUNSUPPORTED);
	check_setup(SETUP_ABI, EOPNOTSUPP, 3, FSIMG_EUNSUPPORTED);
	check_setup(SETUP_ABI, EPERM, 3, FSIMG_EIO);
	check_setup(SETUP_COMPLETE, 0, 2, FSIMG_EUNSUPPORTED);
	for (setup_step_t step = SETUP_RULESET; step <= SETUP_FILTER; step++) {
		check_setup(step, ENOSYS, 3, FSIMG_EIO);
		check_setup(step, EPERM, 3, FSIMG_EIO);
	}
	check_setup(SETUP_COMPLETE, 0, 3, FSIMG_OK);
	CHECK(close(work_root) == 0);
	CHECK(rmdir(directory) == 0);
	return (0);
}
#else
int
main(void)
{
	return (77);
}
#endif
#else
int
main(void)
{
	return (77);
}
#endif
