/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_META_H
#define FSIMG_META_H

#include "internal.h"

#define FSIMG_META_PAGE_SIZE 4096u

typedef struct fsimg_store fsimg_store_t;
typedef struct fsimg_stream fsimg_stream_t;
typedef struct fsimg_array fsimg_array_t;
typedef struct fsimg_bitset fsimg_bitset_t;

/* Metadata objects borrow their containing reader/writer session, which must
 * outlive them. All page accesses and sort comparisons charge that session.
 * Private byte storage grows only; missing bytes are zero. On a failed
 * mutation discard the object. Reads never return borrowed cache pointers.
 */
fsimg_status_t fsimg_store_new(fsimg_session_t *, fsimg_store_t **,
    fsimg_error_t *);
void fsimg_store_free(fsimg_store_t *);
uint64_t fsimg_store_size(const fsimg_store_t *);
fsimg_ctx_t *fsimg_store_context(const fsimg_store_t *);
fsimg_session_t *fsimg_store_session(const fsimg_store_t *);
fsimg_status_t fsimg_store_grow(fsimg_store_t *, uint64_t, fsimg_error_t *);
fsimg_status_t fsimg_store_read(fsimg_store_t *, uint64_t, void *, size_t,
    fsimg_error_t *);
fsimg_status_t fsimg_store_write(fsimg_store_t *, uint64_t, const void *,
    size_t, fsimg_error_t *);
fsimg_status_t fsimg_store_flush(fsimg_store_t *, fsimg_error_t *);

/* Streams borrow their context. Iteration advances only after a complete
 * record, including when the payload is empty. Offsets name frame starts.
 */
struct fsimg_stream {
	fsimg_store_t	*store;
	uint64_t	 count;
	uint32_t	 max_record;
	bool		 sealed;
};

fsimg_status_t fsimg_stream_new(fsimg_session_t *, uint32_t, fsimg_stream_t **,
    fsimg_error_t *);
void fsimg_stream_free(fsimg_stream_t *);
fsimg_status_t fsimg_stream_append(fsimg_stream_t *, const void *, size_t,
    uint64_t *, fsimg_error_t *);
fsimg_status_t fsimg_stream_seal(fsimg_stream_t *, fsimg_error_t *);
fsimg_status_t fsimg_stream_next(fsimg_stream_t *, uint64_t *, void *, size_t,
    size_t *, fsimg_error_t *);

/* Records are explicitly encoded bytes, never native structs with padding. */
fsimg_status_t fsimg_array_new(fsimg_session_t *, uint32_t, uint64_t,
    fsimg_array_t **, fsimg_error_t *);
void fsimg_array_free(fsimg_array_t *);
fsimg_status_t fsimg_array_get(fsimg_array_t *, uint64_t, void *,
    fsimg_error_t *);
fsimg_status_t fsimg_array_set(fsimg_array_t *, uint64_t, const void *,
    fsimg_error_t *);
fsimg_status_t fsimg_bitset_new(fsimg_session_t *, uint64_t, fsimg_bitset_t **,
    fsimg_error_t *);
void fsimg_bitset_free(fsimg_bitset_t *);
fsimg_status_t fsimg_bitset_test_and_set(fsimg_bitset_t *, uint64_t, bool *,
    fsimg_error_t *);

typedef int (*fsimg_compare_fn_t)(const void *, size_t, const void *, size_t,
    void *);

typedef struct fsimg_sort_options {
	/* Total arena/reference allowance; zero chooses a bounded default. */
	size_t		 run_bytes;
	/* Between 2 and 64; zero selects 16, reduced to fit available memory. */
	uint32_t	 fan_in;
} fsimg_sort_options_t;

fsimg_status_t fsimg_stream_sort(fsimg_stream_t *, fsimg_compare_fn_t,
    void *, const fsimg_sort_options_t *, fsimg_stream_t **, fsimg_error_t *);

/* Shared frame codec also bounds records inside a sort run. */
void fsimg_meta_put_u64(unsigned char *, uint64_t);
uint64_t fsimg_meta_get_u64(const unsigned char *);
fsimg_status_t fsimg_record_length(fsimg_store_t *, uint64_t, uint64_t,
    uint32_t, uint32_t *, fsimg_error_t *);
fsimg_status_t fsimg_record_append(fsimg_store_t *, const void *, uint32_t,
    fsimg_error_t *);

typedef enum fsimg_meta_fault {
	FSIMG_META_OPEN = 1,
	FSIMG_META_RESIZE,
	FSIMG_META_READ,
	FSIMG_META_WRITE
} fsimg_meta_fault_t;

/* Validation controls: configure cache only when no metadata objects exist.
 * A fault fails the nth matching operation; zero disables injection.
 */
fsimg_status_t fsimg_test_meta_cache(fsimg_ctx_t *, uint32_t, fsimg_error_t *);
void fsimg_test_meta_fail_after(fsimg_ctx_t *, fsimg_meta_fault_t, uint64_t,
    int);

#endif
