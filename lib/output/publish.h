/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_PUBLISH_H
#define FSIMG_PUBLISH_H

#include "fsimg.h"
#include "host.h"

/* Mechanism prototype: both directory FDs are borrowed, staging is private,
 * and no worker or other writer may still mutate the staged name or inode.
 * No caller should retry after OUTPUT_UNCERTAIN or OUTPUT_PUBLISHED.
 * NULL hooks use host syscalls; hooks exist for deterministic fault tests.
 * link_at and rename_at never replace an existing name.
 */
typedef struct fsimg_publish_ops {
	int (*sync_fd)(void *, int);
	int (*link_at)(void *, int, const char *, int, const char *);
	int (*unlink_at)(void *, int, const char *);
	int (*rename_at)(void *, int, const char *, int, const char *);
	void *argument;
} fsimg_publish_ops_t;

fsimg_status_t fsimg_publish(fsimg_ctx_t *, int, const char *, uint64_t,
    int, const char *, const fsimg_publish_ops_t *, fsimg_error_t *);

/* Fill a fresh staged file, whose descriptor the output coordinator owns
 * and closes; the fill writes, resizes and flushes it but neither closes
 * nor publishes it, and reports the length the staged file ends at, which
 * publication checks.
 */
typedef fsimg_status_t (*fsimg_output_fill_t)(void *, fsimg_io_t *,
    uint64_t *, fsimg_error_t *);

/* When an output needs the sink's raw intermediate: never (wrap, unwrap
 * and compose, which stage no raw disk), only for a STREAM sink (a writer
 * without a container), or always (a writer with one).
 */
typedef enum fsimg_raw_need {
	FSIMG_RAW_NEVER,
	FSIMG_RAW_FOR_STREAM,
	FSIMG_RAW_ALWAYS
} fsimg_raw_need_t;

fsimg_status_t fsimg_sink_adopt(const fsimg_sink_t *, fsimg_raw_need_t, int *,
    fsimg_error_t *);
fsimg_status_t fsimg_sink_identity(const fsimg_sink_t *, fsimg_host_file_t *,
    bool *, fsimg_error_t *);
fsimg_status_t fsimg_output_stage(fsimg_ctx_t *, int, const char *,
    fsimg_output_fill_t, void *, const fsimg_publish_ops_t *, fsimg_error_t *);
fsimg_status_t fsimg_output_write(fsimg_writer_t *, int, const char *,
    const fsimg_publish_ops_t *, fsimg_error_t *);

#endif
