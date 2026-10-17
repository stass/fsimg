/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_PUBLISH_H
#define FSIMG_PUBLISH_H

#include "fsimg.h"

/* Mechanism prototype: both directory FDs are borrowed, staging is private,
 * and no worker or other writer may still mutate the staged name or inode.
 * No caller should retry after OUTPUT_UNCERTAIN or OUTPUT_PUBLISHED.
 * NULL hooks use host syscalls; hooks exist for deterministic fault tests.
 */
typedef struct fsimg_publish_ops {
	int (*sync_fd)(void *, int);
	int (*link_at)(void *, int, const char *, int, const char *);
	int (*unlink_at)(void *, int, const char *);
	void *argument;
} fsimg_publish_ops_t;

fsimg_status_t fsimg_publish(fsimg_ctx_t *, int, const char *, uint64_t,
    int, const char *, const fsimg_publish_ops_t *, fsimg_error_t *);
fsimg_status_t fsimg_output_write(fsimg_writer_t *, int, const char *,
    uint64_t, const fsimg_publish_ops_t *, fsimg_error_t *);

#endif
