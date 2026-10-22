/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

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

static void plant(const char *, mode_t, bool);
static void test_fresh(void);
static void test_taken(void);
static void test_shared_replacement(void);
static void test_populated_replacement(void);
static void test_remove_other_object(void);
static void test_link_replacement(void);

/* The parent every case works in, and the directory the hook renames onto
 * the name fsimg just made, as a writer of a parent without the sticky bit
 * can; NULL plants nothing.
 */
static int parent = -1;
static const char *planted;
static bool plant_link;

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
	CHECK(fstatat(parent, "new", &st, AT_SYMLINK_NOFOLLOW) == -1 && errno == ENOENT);
}

/* A populated directory under the name cannot be the empty one fsimg made,
 * so it is refused as a replacement, left alone, and not reported as
 * fsimg's own.
 */
static void
test_populated_replacement(void)
{
	fsimg_host_privacy_t verdict;
	struct stat st;
	bool created;
	int fd;

	plant("populated", 0700, true);
	CHECK(fsimg_host_private_mkdir(parent, "new", &fd, &verdict, &created) ==
	    FSIMG_HOST_FRESH_REFUSED);
	planted = NULL;
	CHECK(verdict == FSIMG_HOST_NOT_EMPTY && fd == -1);
	CHECK(!created);
	CHECK(strcmp(fsimg_host_fresh_refusal(verdict),
	    "lets another user replace new directories") == 0);
	fd = openat(parent, "new", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(fd != -1);
	CHECK(fstatat(fd, "kept", &st, AT_SYMLINK_NOFOLLOW) == 0);
	CHECK(unlinkat(fd, "kept", 0) == 0);
	CHECK(close(fd) == 0);
	CHECK(unlinkat(parent, "new", AT_REMOVEDIR) == 0);
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
	CHECK(close(parent) == 0);
	CHECK(rmdir(path) == 0);
	return (0);
}
