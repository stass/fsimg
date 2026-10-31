/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/acl.h>
#endif
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/xattr.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "private.h"
#include "test.h"

int fsimg_test_mkdirat(int, const char *, mode_t);
#if defined(__APPLE__) || defined(__FreeBSD__)
acl_t fsimg_test_acl_get_fd_np(int, acl_type_t);
#endif
#if defined(__FreeBSD__)
long fsimg_test_fpathconf(int, int);
#elif defined(__linux__)
ssize_t fsimg_test_fgetxattr(int, const char *, void *, size_t);
#endif

static void before_acl_query(int);
static void plant(const char *, mode_t, bool);
static void test_fresh(void);
static void test_taken(void);
static void test_shared_replacement(void);
static void test_populated_replacement(void);
static void test_remove_other_object(void);
static void test_link_replacement(void);
static void test_mode_raced_with_acl(void);

/* The parent every case works in, and the directory the hook renames onto
 * the name fsimg just made, as a writer of a parent without the sticky bit
 * can; NULL plants nothing. A nonzero widened mode is what the ACL hooks
 * give the directory. It stands in for another user whom an inherited ACL
 * entry lets change the directory's security, which that user can do until
 * the entry is gone; widenings counts the hook calls.
 * The mode is widened from the first ACL-interface call on, so the test
 * pins that owner and mode are read after that call, not after the last.
 * A change completed before a query is all it models; a macOS chmod
 * authorized before the ACL is removed and applied after the check is a
 * documented residual no order closes.
 */
static int parent = -1;
static const char *planted;
static bool plant_link;
static mode_t widened;
static unsigned widenings;

/* The other user's chance before an ACL query, modelled as a mode change
 * that completes before the query runs.
 */
static void
before_acl_query(int fd)
{
	if (widened != 0) {
		CHECK(fchmod(fd, widened) == 0);
		widenings++;
	}
}

#if defined(__APPLE__) || defined(__FreeBSD__)
acl_t
fsimg_test_acl_get_fd_np(int fd, acl_type_t type)
{
	before_acl_query(fd);
	return (acl_get_fd_np(fd, type));
}
#endif
#if defined(__FreeBSD__)
long
fsimg_test_fpathconf(int fd, int name)
{
	before_acl_query(fd);
	return (fpathconf(fd, name));
}
#elif defined(__linux__)
ssize_t
fsimg_test_fgetxattr(int fd, const char *name, void *value, size_t size)
{
	before_acl_query(fd);
	return (fgetxattr(fd, name, value, size));
}
#endif

int
fsimg_test_mkdirat(int at, const char *name, mode_t mode)
{
	if (mkdirat(at, name, mode) == -1)
		return (-1);
	if (planted != NULL)
		CHECK(renameat(parent, planted, at, name) == 0);
	if (plant_link) {
		CHECK(unlinkat(at, name, AT_REMOVEDIR) == 0);
		CHECK(symlinkat("elsewhere", at, name) == 0);
	}
	return (0);
}

/* A directory of the user's with mode, holding one file when populated, for
 * the hook to put in place of the next new directory.
 */
static void
plant(const char *name, mode_t mode, bool populated)
{
	int directory, fd;

	CHECK(mkdirat(parent, name, 0700) == 0);
	CHECK(fchmodat(parent, name, mode, 0) == 0);
	if (populated) {
		directory = openat(parent, name, O_RDONLY | O_DIRECTORY);
		CHECK(directory != -1);
		fd = openat(directory, "kept", O_WRONLY | O_CREAT | O_EXCL, 0600);
		CHECK(fd != -1 && close(fd) == 0);
		CHECK(close(directory) == 0);
	}
	planted = name;
}

/* A fresh directory is created with mode 0700, opened and kept. */
static void
test_fresh(void)
{
	fsimg_host_privacy_t verdict;
	struct stat st;
	bool created;
	int fd;

	CHECK(fsimg_host_private_mkdir(parent, "fresh", &fd, &verdict, &created) ==
	    FSIMG_HOST_FRESH_CREATED);
	CHECK(fd != -1 && created);
	CHECK(fstat(fd, &st) == 0 && S_ISDIR(st.st_mode));
	CHECK((st.st_mode & 0777) == 0700);
	CHECK(fsimg_host_remove_directory(parent, "fresh", fd));
	CHECK(close(fd) == 0);
}

/* A taken name is EEXIST with nothing created. */
static void
test_taken(void)
{
	fsimg_host_privacy_t verdict;
	bool created;
	int fd;

	CHECK(mkdirat(parent, "taken", 0700) == 0);
	errno = 0;
	CHECK(fsimg_host_private_mkdir(parent, "taken", &fd, &verdict, &created) ==
	    FSIMG_HOST_FRESH_TAKEN);
	CHECK(errno == EEXIST && fd == -1 && !created);
	CHECK(unlinkat(parent, "taken", AT_REMOVEDIR) == 0);
}

/* The user's own group-writable empty directory put in place of the new
 * one is refused and, being empty and the user's, removed again.
 */
static void
test_shared_replacement(void)
{
	fsimg_host_privacy_t verdict;
	struct stat st;
	bool created;
	int fd;

	plant("shared", 0770, false);
	CHECK(fsimg_host_private_mkdir(parent, "new", &fd, &verdict, &created) ==
	    FSIMG_HOST_FRESH_REFUSED);
	planted = NULL;
	CHECK(verdict == FSIMG_HOST_SHARED_WRITE && fd == -1 && !created);
	CHECK(strcmp(fsimg_host_fresh_refusal(verdict),
	    "lets another user into new directories") == 0);
	CHECK(fstatat(parent, "new", &st, AT_SYMLINK_NOFOLLOW) == -1 &&
	    errno == ENOENT);
}

/* A populated directory under the name cannot be the empty one fsimg made,
 * so whatever the rule refuses it for, it is left alone and not reported
 * as fsimg's own.
 */
static void
test_populated_replacement(void)
{
	static const struct {
		mode_t			mode;
		fsimg_host_privacy_t	verdict;
		const char		*refusal;
	} cases[] = {
		{ 0700, FSIMG_HOST_NOT_EMPTY,
		  "lets another user replace new directories" },
		{ 0770, FSIMG_HOST_SHARED_WRITE,
		  "lets another user into new directories" },
		{ 0707, FSIMG_HOST_SHARED_WRITE,
		  "lets another user into new directories" }
	};
	fsimg_host_privacy_t verdict;
	struct stat st;
	bool created;
	int fd;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		plant("populated", cases[i].mode, true);
		CHECK(fsimg_host_private_mkdir(parent, "new", &fd, &verdict,
		    &created) == FSIMG_HOST_FRESH_REFUSED);
		planted = NULL;
		CHECK(verdict == cases[i].verdict && fd == -1);
		CHECK(strcmp(fsimg_host_fresh_refusal(verdict),
		    cases[i].refusal) == 0);
		CHECK(!created);
		fd = openat(parent, "new", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
		CHECK(fd != -1);
		CHECK(fstatat(fd, "kept", &st, AT_SYMLINK_NOFOLLOW) == 0);
		CHECK(unlinkat(fd, "kept", 0) == 0);
		CHECK(close(fd) == 0);
		CHECK(unlinkat(parent, "new", AT_REMOVEDIR) == 0);
	}
}

/* A symbolic link put in place of the new directory fails the no-follow
 * reopen, and the name, no longer holding a directory fsimg made, is not
 * reported as fsimg's.
 */
static void
test_link_replacement(void)
{
	fsimg_host_privacy_t verdict;
	bool created;
	int fd;

	plant_link = true;
	CHECK(fsimg_host_private_mkdir(parent, "new", &fd, &verdict, &created) ==
	    FSIMG_HOST_FRESH_FAILED);
	plant_link = false;
	CHECK(fd == -1 && !created);
	CHECK(unlinkat(parent, "new", 0) == 0);
}

/* A mode widened before the ACL query, as another user granted the right
 * to change the directory's security through an inherited entry can do
 * before removing that entry, is seen, because the mode that passes is
 * sampled after the ACL query. The new directory is refused and, being
 * fsimg's and empty, removed.
 */
static void
test_mode_raced_with_acl(void)
{
	fsimg_host_privacy_t verdict;
	struct stat st;
	bool created;
	int fd;

	widened = 0777;
	widenings = 0;
	CHECK(fsimg_host_private_mkdir(parent, "raced", &fd, &verdict,
	    &created) == FSIMG_HOST_FRESH_REFUSED);
	widened = 0;
	CHECK(widenings > 0);
	CHECK(verdict == FSIMG_HOST_SHARED_WRITE && fd == -1 && !created);
	CHECK(fstatat(parent, "raced", &st, AT_SYMLINK_NOFOLLOW) == -1 &&
	    errno == ENOENT);
}

/* Removal spares a name that no longer holds the opened directory. */
static void
test_remove_other_object(void)
{
	int fd;

	CHECK(mkdirat(parent, "opened", 0700) == 0);
	fd = openat(parent, "opened", O_RDONLY | O_DIRECTORY);
	CHECK(fd != -1);
	CHECK(mkdirat(parent, "other", 0700) == 0);
	CHECK(renameat(parent, "other", parent, "opened") == 0);
	errno = 0;
	CHECK(!fsimg_host_remove_directory(parent, "opened", fd) && errno == EEXIST);
	CHECK(close(fd) == 0);
	CHECK(unlinkat(parent, "opened", AT_REMOVEDIR) == 0);
}

int
main(void)
{
	char path[] = "private-fixture-XXXXXX";

	CHECK(mkdtemp(path) != NULL);
	parent = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(parent != -1);
	test_fresh();
	test_taken();
	test_shared_replacement();
	test_populated_replacement();
	test_remove_other_object();
	test_link_replacement();
	test_mode_raced_with_acl();
	CHECK(close(parent) == 0);
	CHECK(rmdir(path) == 0);
	return (0);
}
