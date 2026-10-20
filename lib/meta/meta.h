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
 * end is the stream's length; the store beneath can be longer after a
 * rewind, and bytes past end are never read.
 */
struct fsimg_stream {
	fsimg_store_t	*store;
	uint64_t	 count;
	uint64_t	 end;
	uint32_t	 max_record;
	bool		 sealed;
};

/* An unsealed stream can serve as a stack: a mark records its current end,
 * and rewinding to the mark discards every record appended after it. Later
 * appends reuse the storage, so a stack of nested record runs holds one
 * scratch store however deep it grows.
 */
typedef struct fsimg_stream_mark {
	uint64_t	 offset;
	uint64_t	 count;
} fsimg_stream_mark_t;

fsimg_status_t fsimg_stream_new(fsimg_session_t *, uint32_t, fsimg_stream_t **,
    fsimg_error_t *);
void fsimg_stream_free(fsimg_stream_t *);
fsimg_status_t fsimg_stream_append(fsimg_stream_t *, const void *, size_t,
    uint64_t *, fsimg_error_t *);
fsimg_status_t fsimg_stream_seal(fsimg_stream_t *, fsimg_error_t *);
fsimg_status_t fsimg_stream_next(fsimg_stream_t *, uint64_t *, void *, size_t,
    size_t *, fsimg_error_t *);
fsimg_stream_mark_t fsimg_stream_mark(const fsimg_stream_t *);
fsimg_status_t fsimg_stream_rewind(fsimg_stream_t *, fsimg_stream_mark_t,
    fsimg_error_t *);

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
fsimg_status_t fsimg_bitset_test(fsimg_bitset_t *, uint64_t, bool *,
    fsimg_error_t *);
fsimg_status_t fsimg_bitset_test_and_set(fsimg_bitset_t *, uint64_t, bool *,
    fsimg_error_t *);
/* Set bits [first, first + count). *was_set reports whether any of them was
 * already set; on success every bit of the range is set afterwards.
 */
fsimg_status_t fsimg_bitset_test_and_set_range(fsimg_bitset_t *, uint64_t,
    uint64_t, bool *, fsimg_error_t *);

/* One physical run of an object's storage: length bytes at image_offset hold
 * the object's bytes [logical_offset, logical_offset + length).
 */
typedef struct fsimg_extent {
	uint64_t	 logical_offset;
	uint64_t	 image_offset;
	uint64_t	 length;
} fsimg_extent_t;

/* The image range [offset, offset + length) that an object's storage may
 * occupy, in whole units counted from offset, such as a cluster heap.
 */
typedef struct fsimg_extent_domain {
	uint64_t	 offset;
	uint64_t	 length;
	uint32_t	 unit_bytes;
} fsimg_extent_domain_t;

/* An optional extent index for resolving logical offsets. A driver appends
 * extents in logical order from zero; the covered length is their sum.
 * Physically adjacent extents merge, so callers may append unmerged runs. Its
 * records are private scratch, decoded and checked against the covered length
 * and the domain on every read. NULL is the empty index of an object without
 * storage: its length is zero, a walk visits nothing, and every lookup fails
 * with FSIMG_ECORRUPT, as does any lookup at or beyond the covered length. On
 * a failed append discard the index. Creation fails with FSIMG_EINVAL
 * unless the domain has a nonzero unit, a length in whole units, and an end
 * that fits in uint64_t.
 */
typedef struct fsimg_extent_index fsimg_extent_index_t;
typedef fsimg_status_t (*fsimg_extent_visit_fn_t)(void *,
    const fsimg_extent_t *, fsimg_error_t *);

fsimg_status_t fsimg_extent_index_new(fsimg_session_t *,
    const fsimg_extent_domain_t *, fsimg_extent_index_t **, fsimg_error_t *);
void fsimg_extent_index_free(fsimg_extent_index_t *);
uint64_t fsimg_extent_index_length(const fsimg_extent_index_t *);
fsimg_status_t fsimg_extent_index_append(fsimg_extent_index_t *,
    const fsimg_extent_t *, fsimg_error_t *);
fsimg_status_t fsimg_extent_index_lookup(fsimg_extent_index_t *, uint64_t,
    fsimg_extent_t *, fsimg_error_t *);
fsimg_status_t fsimg_extent_index_walk(fsimg_extent_index_t *,
    fsimg_extent_visit_fn_t, void *, fsimg_error_t *);
/* Validation controls: the record count and the private record store. */
uint64_t fsimg_test_extent_count(const fsimg_extent_index_t *);
fsimg_store_t *fsimg_test_extent_records(const fsimg_extent_index_t *);

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
void fsimg_meta_put_u32(unsigned char *, uint32_t);
uint32_t fsimg_meta_get_u32(const unsigned char *);
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
