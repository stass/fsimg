/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_PRIVATE_H
#define FSIMG_PRIVATE_H

#include <stdbool.h>

/* Why a directory is not private, in the order checked. */
typedef enum fsimg_host_privacy {
	FSIMG_HOST_PRIVATE,
	FSIMG_HOST_NOT_DIRECTORY,
	FSIMG_HOST_FOREIGN_OWNER,	/* Not owned by the effective user */
	FSIMG_HOST_SHARED_ACCESS,	/* Group or others hold a forbidden permission */
	FSIMG_HOST_EXTENDED_ACL,	/* A non-trivial access control list */
	FSIMG_HOST_NOT_EMPTY
} fsimg_host_privacy_t;

/*
 * Judge an opened directory by the extraction privacy rule, using that
 * descriptor alone: a directory owned by the effective user, without group
 * or other write permission, empty, and carrying no non-trivial access
 * control list of any type the platform can report. An ACL is never
 * interpreted, only detected. Group and others may read and search a
 * directory the user supplies; one fsimg created, with mode 0700, must give
 * them no permission at all, since a filesystem that ignores the creation
 * mode, such as one whose mount options set every mode, would otherwise
 * show them what fsimg writes there. Returns 0 with the verdict, or -1 with
 * errno when a query fails, which the caller must treat as a failure rather
 * than as privacy.
 */
int fsimg_host_private_directory(int, bool, fsimg_host_privacy_t *);

#endif
