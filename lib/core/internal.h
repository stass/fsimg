/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_INTERNAL_H
#define FSIMG_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fsimg.h"

typedef struct fsimg_meta_cache fsimg_meta_cache_t;

struct fsimg_ctx {
	fsimg_options_t	 options;
	fsimg_stats_t	 stats;
	uint64_t	 allocation_attempts;
	uint64_t	 fail_allocation;
	fsimg_meta_cache_t *meta_cache;
	uint64_t	 scratch_sequence;
	uint64_t	 meta_fault_after;
	uint32_t	 meta_cache_pages;
	unsigned	 meta_fault_point;
	int		 meta_fault_errno;
	bool		 in_callback;
};

/* One session is embedded in each reader/writer; never reset per API call. */
typedef struct fsimg_session {
	fsimg_ctx_t	*ctx;
	fsimg_progress_t progress;
	uint64_t	 output_bytes;
	uint64_t	 output_limit;
} fsimg_session_t;

bool fsimg_add_u64(uint64_t, uint64_t, uint64_t *);
bool fsimg_mul_u64(uint64_t, uint64_t, uint64_t *);
bool fsimg_to_size(uint64_t, size_t *);
bool fsimg_range_fits(uint64_t, uint64_t, uint64_t);
void fsimg_error_clear(fsimg_error_t *);
fsimg_status_t fsimg_error_set(fsimg_error_t *, fsimg_status_t, int,
    fsimg_operation_t, const char *);
fsimg_status_t fsimg_limit_error(fsimg_error_t *, fsimg_resource_t,
    uint64_t, uint64_t);
fsimg_status_t fsimg_ctx_ready(fsimg_ctx_t *, fsimg_error_t *);
fsimg_status_t fsimg_poll(fsimg_ctx_t *, const fsimg_progress_t *,
    fsimg_error_t *);
void *fsimg_alloc(fsimg_ctx_t *, size_t, fsimg_error_t *);
void fsimg_dealloc(void *);
fsimg_status_t fsimg_fd_reserve(fsimg_ctx_t *, fsimg_error_t *);
void fsimg_fd_release(fsimg_ctx_t *, int);
/* Internal files reserve a slot before open. Adoption consumes both the FD
 * and that reservation on every outcome, avoiding duplication in a worker.
 */
fsimg_status_t fsimg_io_adopt_fd(fsimg_ctx_t *, int, unsigned,
    fsimg_io_t **, fsimg_error_t *);
/* Bind a freshly constructed internal adapter to its operation's work quota. */
void fsimg_io_bind_session(fsimg_io_t *, fsimg_session_t *);
void fsimg_session_init(fsimg_session_t *, fsimg_ctx_t *, const char *,
    uint64_t);
fsimg_status_t fsimg_session_charge(fsimg_session_t *, uint64_t, uint64_t,
    uint64_t, fsimg_error_t *);
void fsimg_stat_add(uint64_t *, uint64_t);
/* Private fault injection: zero disables; n fails the nth subsequent attempt. */
void fsimg_test_fail_alloc_after(fsimg_ctx_t *, uint64_t);

#endif
