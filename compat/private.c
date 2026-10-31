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
#include <string.h>
#include <unistd.h>

#include "host.h"
#include "private.h"

static int directory_empty(int, bool *);
static int nontrivial_acl(int, bool *);
static bool discard_refused(int, const char *, int);

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
 * client reports system.nfs4_acl for every inode of a server with ACL
 * support, which cannot be shown trivial without decoding it, so its
 * presence counts as non-trivial too. A size query needs no buffer: an
 * absent attribute is ENODATA, and a file system without attributes or ACLs
 * reports ENOTSUP for each.
 */
static int
nontrivial_acl(int fd, bool *present)
{
	static const char *const names[] = { "system.posix_acl_access",
	    "system.posix_acl_default", "system.nfs4_acl" };

	*present = false;
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (fgetxattr(fd, names[i], NULL, 0) != -1) {
			*present = true;
			return (0);
		}
		if (errno != ENODATA && errno != ENOTSUP)
			return (-1);
	}
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
	struct stat first, st;
	bool present, empty;

	/*
	 * The order closes the ordinary interleavings; a refusal is safe
	 * whenever it is found. A file's type never changes. An ACL entry can
	 * let another user change the directory's mode, owner or ACL, so the
	 * owner and mode that pass are sampled only after the ACL query finds
	 * no entry, and emptiness, which depends on who can write, is judged
	 * last. The first sample also refuses a foreign owner at once, so an
	 * ACL its owner keeps unreadable does not hide that cause. No order
	 * closes one macOS race: a change to the mode or ACL is authorized
	 * there before it is applied, so one an ACL entry authorized can land
	 * after the entry is removed and the check has passed
	 * (specs/design-security.md#safe-extraction).
	 */
	if (fsimg_host_fstat(fd, &first) == -1)
		return (-1);
	if (!S_ISDIR(first.st_mode)) {
		*verdict = FSIMG_HOST_NOT_DIRECTORY;
		return (0);
	}
	if (first.st_uid != geteuid()) {
		*verdict = FSIMG_HOST_FOREIGN_OWNER;
		return (0);
	}
	if (nontrivial_acl(fd, &present) == -1)
		return (-1);
	if (present) {
		*verdict = FSIMG_HOST_EXTENDED_ACL;
		return (0);
	}
	if (fsimg_host_fstat(fd, &st) == -1)
		return (-1);
	if (st.st_uid != geteuid()) {
		*verdict = FSIMG_HOST_FOREIGN_OWNER;
		return (0);
	}
	if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
		*verdict = FSIMG_HOST_SHARED_WRITE;
		return (0);
	}
	if (directory_empty(fd, &empty) == -1)
		return (-1);
	*verdict = empty ? FSIMG_HOST_PRIVATE : FSIMG_HOST_NOT_EMPTY;
	return (0);
}

const char *
fsimg_host_fresh_refusal(fsimg_host_privacy_t verdict)
{
	switch (verdict) {
	case FSIMG_HOST_SHARED_WRITE:
		return ("lets another user into new directories");
	case FSIMG_HOST_EXTENDED_ACL:
		return ("gives new directories a non-trivial access control list");
	default:
		return ("lets another user replace new directories");
	}
}

bool
fsimg_host_remove_directory(int parent, const char *name, int fd)
{
	struct stat opened, named;

	if (fsimg_host_fstat(fd, &opened) == -1 ||
	    fstatat(parent, name, &named, AT_SYMLINK_NOFOLLOW) == -1)
		return (false);
	if (opened.st_dev != named.st_dev || opened.st_ino != named.st_ino ||
	    opened.st_uid != geteuid()) {
		errno = EEXIST;
		return (false);
	}
	return (unlinkat(parent, name, AT_REMOVEDIR) == 0);
}

/*
 * Discard the directory opened as fd under name, which the check refused
 * or could not judge, and return whether nothing of fsimg's is left there:
 * true once it is removed, or when the name holds another object or
 * nothing. A populated directory, which rmdir refuses with ENOTEMPTY, is
 * not reported either, whatever the check refused it for: fsimg made the
 * directory empty and wrote nothing into it, so a populated one is normally
 * the user's own, put under the name in place of the new one, and otherwise
 * fsimg's own filled by another user the check kept out, whose files fsimg
 * does not vouch for. False means a directory fsimg made may remain, for
 * the caller to report: removal failed for another reason, such as EACCES
 * from an ACL that denies deletion, or EPERM, which leaves the
 * directory's contents uninspected, populated or not.
 */
static bool
discard_refused(int parent, const char *name, int fd)
{
	if (fsimg_host_remove_directory(parent, name, fd))
		return (true);
	return (errno == EEXIST || errno == ENOENT || errno == ENOTEMPTY);
}

fsimg_host_fresh_t
fsimg_host_private_mkdir(int parent, const char *name, int *fd,
    fsimg_host_privacy_t *verdict, bool *created)
{
	bool judged;
	int saved;

	*fd = -1;
	*verdict = FSIMG_HOST_PRIVATE;
	*created = false;
	if (mkdirat(parent, name, 0700) == -1)
		return (errno == EEXIST ? FSIMG_HOST_FRESH_TAKEN :
		    FSIMG_HOST_FRESH_FAILED);
	*fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (*fd == -1) {
		/* A name that no longer reaches a directory, gone or replaced
		 * by another kind of object (ELOOP, or EMLINK on FreeBSD, for a
		 * symbolic link), holds nothing of fsimg's.
		 */
		*created = errno != ENOENT && errno != ENOTDIR && errno != ELOOP &&
		    errno != EMLINK;
		return (FSIMG_HOST_FRESH_FAILED);
	}
	*created = true;
	judged = fsimg_host_private_directory(*fd, verdict) == 0;
	if (judged && *verdict == FSIMG_HOST_PRIVATE)
		return (FSIMG_HOST_FRESH_CREATED);
	saved = errno;
	*created = !discard_refused(parent, name, *fd);
	close(*fd);
	*fd = -1;
	errno = saved;
	return (judged ? FSIMG_HOST_FRESH_REFUSED : FSIMG_HOST_FRESH_FAILED);
}
