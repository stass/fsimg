/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_PRIVATE_H
#define FSIMG_PRIVATE_H

#include <stdbool.h>

/* Why a directory fsimg writes into is not private, in the order checked. */
typedef enum fsimg_host_privacy {
	FSIMG_HOST_PRIVATE,
	FSIMG_HOST_NOT_DIRECTORY,
	FSIMG_HOST_EXTENDED_ACL,	/* A non-trivial access control list */
	FSIMG_HOST_FOREIGN_OWNER,	/* Not owned by the effective user */
	FSIMG_HOST_SHARED_WRITE,	/* Group or others may write */
	FSIMG_HOST_NOT_EMPTY
} fsimg_host_privacy_t;

/*
 * Judge an opened directory by the private-directory rule, using that
 * descriptor alone: a directory owned by the effective user, without group
 * or other write permission, empty, and carrying no non-trivial access
 * control list of any type the platform can report. The rule keeps other
 * writers out, so read and search bits for others are allowed. An ACL is
 * never interpreted, only detected. The rule trusts what the descriptor
 * reports, so a filesystem that ignores ownership or permissions defeats it.
 * The emptiness check reads through one transient second descriptor.
 * Returns 0 with the verdict, or -1 with errno when a query fails, which
 * the caller must treat as a failure rather than as privacy.
 */
int fsimg_host_private_directory(int, fsimg_host_privacy_t *);

/* What an attempt to create a fresh private directory came to. */
typedef enum fsimg_host_fresh {
	FSIMG_HOST_FRESH_CREATED,	/* Created, opened and proven private */
	FSIMG_HOST_FRESH_TAKEN,		/* The name exists; nothing was created */
	FSIMG_HOST_FRESH_REFUSED,	/* The directory there is not private */
	FSIMG_HOST_FRESH_FAILED		/* A host call failed, as errno says */
} fsimg_host_fresh_t;

/*
 * Create name beneath parent as a fresh private directory: mkdirat with
 * mode 0700, a no-follow reopen, then the rule above on the opened
 * descriptor. This is how fsimg makes every directory it writes into. Mode
 * 0700 alone proves nothing: inheritable access control entries of parent
 * can admit other users, and a writer of parent without the sticky bit can
 * put another directory under the name before the reopen.
 *
 * *fd is the opened directory when it is CREATED and -1 otherwise; *verdict
 * says why when it is REFUSED; errno is set when it is TAKEN or FAILED. A
 * directory that is refused, or whose check failed, is removed again as
 * fsimg_host_remove_directory() allows. On every outcome *created says
 * whether a directory fsimg made still stands under name as fsimg left it,
 * empty, which the caller then owns and reports; a populated directory there
 * is never reported, since fsimg wrote nothing into it.
 */
fsimg_host_fresh_t fsimg_host_private_mkdir(int, const char *, int *,
    fsimg_host_privacy_t *, bool *);

/* What a parent directory does to the new directory a verdict refused, as a
 * predicate for a message naming that parent.
 */
const char *fsimg_host_fresh_refusal(fsimg_host_privacy_t);

/*
 * Remove the directory fsimg created under name beneath parent, and opened
 * as fd, when the name still holds that object, owned by the effective
 * user, and it is empty, which rmdir itself requires; a directory someone
 * else put there is never removed. Returns false otherwise with errno saying
 * why: EEXIST when the name holds another object, ENOTEMPTY for a populated
 * directory. A writer of
 * a parent without the sticky bit can still put another empty directory
 * under the name between the check and the removal; it gains nothing it
 * could not do itself.
 */
bool fsimg_host_remove_directory(int, const char *, int);

#endif
