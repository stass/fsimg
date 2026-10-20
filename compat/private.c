/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "host.h"
#include "private.h"

static int directory_empty(int, bool *);
static int nontrivial_acl(int, bool *);

/* Whether the directory holds no name besides "." and "..", read through
 * a fresh descriptor for the same directory so the caller's descriptor
 * keeps its position.
 */
static int
directory_empty(int fd, bool *empty)
{
	unsigned char buffer[4096];
	const char *name;
	size_t used, cursor, length;
	int directory, code, saved;

	directory = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (directory == -1)
		return (-1);
	*empty = true;
	code = 0;
	while (*empty) {
		code = fsimg_host_directory_read(directory, buffer, sizeof(buffer), &used);
		if (code == -1 && errno == EINTR)
			continue;
		if (code == -1 || used == 0)
			break;
		cursor = 0;
		while ((code = fsimg_host_directory_name(buffer, used, &cursor, &name,
		    &length)) > 0) {
			if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
				*empty = false;
				break;
			}
		}
		if (code == -1)
			break;
	}
	saved = errno;
	close(directory);
	errno = saved;
	return (code == -1 ? -1 : 0);
}

#if defined(__APPLE__)
/* macOS reports one ACL type for files, the extended one; any entry in it
 * is non-trivial. A file system without ACL support has none.
 */
static int
nontrivial_acl(int fd, bool *present)
{
	acl_entry_t entry;
	acl_t acl;

	*present = false;
	acl = acl_get_fd_np(fd, ACL_TYPE_EXTENDED);
	if (acl == NULL)
		return (errno == ENOENT || errno == ENOTSUP || errno == EOPNOTSUPP ?
		    0 : -1);
	*present = acl_get_entry(acl, ACL_FIRST_ENTRY, &entry) == 0;
	acl_free(acl);
	return (0);
}
#elif defined(__FreeBSD__)
static int acl_type_nontrivial(int, acl_type_t, bool *);
static int acl_supported(int, int, bool *);

/* An access or NFSv4 ACL is non-trivial when acl_is_trivial_np says so; a
 * default ACL, which only directories have, is non-trivial when it has
 * any entry, since every entry is inherited by new objects.
 */
static int
acl_type_nontrivial(int fd, acl_type_t type, bool *present)
{
	acl_entry_t entry;
	acl_t acl;
	int trivial, found, saved;

	acl = acl_get_fd_np(fd, type);
	if (acl == NULL)
		return (-1);
	if (type == ACL_TYPE_DEFAULT) {
		found = acl_get_entry(acl, ACL_FIRST_ENTRY, &entry);
		if (found == -1)
			goto fail;
		*present = found == 1;
	} else {
		if (acl_is_trivial_np(acl, &trivial) == -1)
			goto fail;
		*present = trivial == 0;
	}
	acl_free(acl);
	return (0);
fail:
	saved = errno;
	acl_free(acl);
	errno = saved;
	return (-1);
}

static int
acl_supported(int fd, int name, bool *supported)
{
	long value;

	errno = 0;
	value = fpathconf(fd, name);
	if (value == -1 && errno != 0 && errno != EINVAL)
		return (-1);
	*supported = value > 0;
	return (0);
}

/* FreeBSD reports POSIX.1e access and default ACLs and NFSv4 ACLs, each on
 * the file systems whose pathconf says they are supported.
 */
static int
nontrivial_acl(int fd, bool *present)
{
	bool supported;

	*present = false;
	if (acl_supported(fd, _PC_ACL_NFS4, &supported) == -1)
		return (-1);
	if (supported && acl_type_nontrivial(fd, ACL_TYPE_NFS4, present) == -1)
		return (-1);
	if (*present)
		return (0);
	if (acl_supported(fd, _PC_ACL_EXTENDED, &supported) == -1)
		return (-1);
	if (!supported)
		return (0);
	if (acl_type_nontrivial(fd, ACL_TYPE_ACCESS, present) == -1)
		return (-1);
	if (*present)
		return (0);
	return (acl_type_nontrivial(fd, ACL_TYPE_DEFAULT, present));
}
#elif defined(__linux__)
/* Linux stores a POSIX ACL as a system.posix_acl_access or
 * system.posix_acl_default attribute only when it is non-trivial. An NFS
 * client lists system.nfs4_acl for every inode of a server with ACL
 * support, which cannot be shown trivial without decoding it, so its
 * presence counts as non-trivial too.
 */
static int
nontrivial_acl(int fd, bool *present)
{
	static const char *const names[] = { "system.posix_acl_access",
	    "system.posix_acl_default", "system.nfs4_acl" };
	char *list;
	ssize_t size, listed;
	int saved;

	*present = false;
	for (unsigned attempt = 0;; attempt++) {
		size = flistxattr(fd, NULL, 0);
		if (size == -1)
			return (errno == ENOTSUP ? 0 : -1);
		if (size == 0)
			return (0);
		list = malloc((size_t)size);
		if (list == NULL)
			return (-1);
		listed = flistxattr(fd, list, (size_t)size);
		if (listed != -1)
			break;
		saved = errno;
		free(list);
		/* The list grew between the two calls; ask again, boundedly. */
		if (saved != ERANGE || attempt == 8) {
			errno = saved;
			return (-1);
		}
	}
	/* The list is NUL-separated names; a final name without its NUL is
	 * still bounded by the listed length.
	 */
	for (size_t offset = 0, length; offset < (size_t)listed; offset += length + 1) {
		length = strnlen(list + offset, (size_t)listed - offset);
		for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			if (length == strlen(names[i]) &&
			    memcmp(list + offset, names[i], length) == 0)
				*present = true;
		}
	}
	free(list);
	return (0);
}
#else
/* A host whose ACL interface fsimg does not know cannot prove a directory
 * private, so the check fails closed rather than trusting the mode bits.
 */
static int
nontrivial_acl(int fd FSIMG_UNUSED, bool *present)
{
	*present = false;
	errno = ENOTSUP;
	return (-1);
}
#endif

int
fsimg_host_private_directory(int fd, fsimg_host_privacy_t *verdict)
{
	struct stat st;
	bool present, empty;

	if (fstat(fd, &st) == -1)
		return (-1);
	if (!S_ISDIR(st.st_mode))
		*verdict = FSIMG_HOST_NOT_DIRECTORY;
	else if (st.st_uid != geteuid())
		*verdict = FSIMG_HOST_FOREIGN_OWNER;
	else if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0)
		*verdict = FSIMG_HOST_SHARED_WRITE;
	else {
		if (nontrivial_acl(fd, &present) == -1)
			return (-1);
		if (present)
			*verdict = FSIMG_HOST_EXTENDED_ACL;
		else {
			if (directory_empty(fd, &empty) == -1)
				return (-1);
			*verdict = empty ? FSIMG_HOST_PRIVATE : FSIMG_HOST_NOT_EMPTY;
		}
	}
	return (0);
}
