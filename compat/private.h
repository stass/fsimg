/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_PRIVATE_H
#define FSIMG_PRIVATE_H

/* Why an extraction destination is not private, in the order checked. */
typedef enum fsimg_host_privacy {
	FSIMG_HOST_PRIVATE,
	FSIMG_HOST_NOT_DIRECTORY,
	FSIMG_HOST_FOREIGN_OWNER,	/* Not owned by the effective user */
	FSIMG_HOST_SHARED_WRITE,	/* Group or others may write */
	FSIMG_HOST_EXTENDED_ACL,	/* A non-trivial access control list */
	FSIMG_HOST_NOT_EMPTY
} fsimg_host_privacy_t;

/*
 * Judge an opened directory by the extraction privacy rule, using that
 * descriptor alone: a directory owned by the effective user, without group
 * or other write permission, empty, and carrying no non-trivial access
 * control list of any type the platform can report. Read and search bits
 * for others are allowed. An ACL is never interpreted, only detected.
 * Returns 0 with the verdict, or -1 with errno when a query fails, which
 * the caller must treat as a failure rather than as privacy.
 */
int fsimg_host_private_directory(int, fsimg_host_privacy_t *);

#endif
