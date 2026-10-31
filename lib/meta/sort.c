/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <strings.h>

#include "fsimg-endian.h"
#include "meta.h"

typedef struct sort_reference {
	uint64_t	 ordinal;
	size_t		 offset;
	uint32_t	 length;
} sort_reference_t;

typedef struct merge_cursor {
	uint64_t	 offset;
	uint64_t	 end;
	uint64_t	 remaining;
	uint32_t	 length;
} merge_cursor_t;

typedef struct sort_state {
	fsimg_ctx_t	*ctx;
	fsimg_session_t	*session;
	fsimg_compare_fn_t compare;
	void		*argument;
	const char	*phase;
	uint32_t	 maximum;
} sort_state_t;

typedef struct merge_group {
	sort_state_t	*sort;
	merge_cursor_t	*cursors;
	unsigned char	*records;
	uint32_t	*heap;
	uint32_t	 count;
	size_t		 stride;
} merge_group_t;

static fsimg_status_t sort_poll(sort_state_t *, fsimg_error_t *);
static fsimg_status_t compare_records(sort_state_t *, const void *, size_t,
    const void *, size_t, int *, fsimg_error_t *);
static fsimg_status_t compare_references(sort_state_t *, const unsigned char *,
    const sort_reference_t *, const sort_reference_t *, int *, fsimg_error_t *);
static fsimg_status_t sift_references(sort_state_t *, const unsigned char *,
    sort_reference_t *, size_t, size_t, fsimg_error_t *);
static fsimg_status_t order_references(sort_state_t *, const unsigned char *,
    sort_reference_t *, size_t, fsimg_error_t *);
static fsimg_status_t begin_run(fsimg_store_t *, uint64_t *, fsimg_error_t *);
static fsimg_status_t finish_run(fsimg_store_t *, uint64_t, uint64_t,
    fsimg_error_t *);
static fsimg_status_t generate_runs(sort_state_t *, fsimg_stream_t *, size_t,
    fsimg_store_t **, uint64_t *, fsimg_error_t *);
static fsimg_status_t open_run(fsimg_store_t *, uint64_t *, merge_cursor_t *,
    fsimg_error_t *);
static fsimg_status_t advance_cursor(sort_state_t *, fsimg_store_t *,
    merge_cursor_t *, unsigned char *, fsimg_error_t *);
static fsimg_status_t cursor_before(merge_group_t *, uint32_t, uint32_t,
    bool *, fsimg_error_t *);
static fsimg_status_t sift_cursors(merge_group_t *, uint32_t, fsimg_error_t *);
static fsimg_status_t merge_runs(merge_group_t *, fsimg_store_t *,
    fsimg_store_t *, uint64_t, uint64_t *, fsimg_error_t *);
static fsimg_status_t merge_passes(sort_state_t *, fsimg_store_t **,
    uint64_t, uint32_t, fsimg_error_t *);
static fsimg_status_t publish_result(sort_state_t *, fsimg_store_t *,
    uint64_t, fsimg_stream_t **, fsimg_error_t *);

static fsimg_status_t
sort_poll(sort_state_t *sort, fsimg_error_t *error)
{
	fsimg_progress_t progress;

	progress = sort->session->progress;
	progress.phase = sort->phase;
	return (fsimg_poll(sort->ctx, &progress, error));
}

static fsimg_status_t
compare_records(sort_state_t *sort, const void *a, size_t a_length,
    const void *b, size_t b_length, int *result, fsimg_error_t *error)
{
	fsimg_status_t status;

	status = fsimg_session_charge(sort->session, 0, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	sort->ctx->in_callback = true;
	*result = sort->compare(a, a_length, b, b_length, sort->argument);
	sort->ctx->in_callback = false;
	return (FSIMG_OK);
}

static fsimg_status_t
compare_references(sort_state_t *sort, const unsigned char *arena,
    const sort_reference_t *a, const sort_reference_t *b, int *result,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	status = compare_records(sort, arena + a->offset, a->length,
	    arena + b->offset, b->length, result, error);
	if (status != FSIMG_OK)
		return (status);
	if (*result == 0)
		*result = (a->ordinal > b->ordinal) - (a->ordinal < b->ordinal);
	return (FSIMG_OK);
}

static fsimg_status_t
sift_references(sort_state_t *sort, const unsigned char *arena,
    sort_reference_t *references, size_t count, size_t root,
    fsimg_error_t *error)
{
	sort_reference_t temporary;
	size_t child;
	fsimg_status_t status;
	int order;

	/* A max heap moves its largest record to the end at each removal. */
	while (root < count / 2) {
		child = root * 2 + 1;
		if (child + 1 < count) {
			status = compare_references(sort, arena, &references[child],
			    &references[child + 1], &order, error);
			if (status != FSIMG_OK)
				return (status);
			if (order < 0)
				child++;
		}
		status = compare_references(sort, arena, &references[root],
		    &references[child], &order, error);
		if (status != FSIMG_OK)
			return (status);
		if (order >= 0)
			break;
		temporary = references[root];
		references[root] = references[child];
		references[child] = temporary;
		root = child;
	}
	return (FSIMG_OK);
}

static fsimg_status_t
order_references(sort_state_t *sort, const unsigned char *arena,
    sort_reference_t *references, size_t count, fsimg_error_t *error)
{
	sort_reference_t temporary;
	fsimg_status_t status;

	for (size_t i = count / 2; i != 0; i--) {
		if (i % 256 == 0) {
			status = sort_poll(sort, error);
			if (status != FSIMG_OK)
				return (status);
		}
		status = sift_references(sort, arena, references, count, i - 1, error);
		if (status != FSIMG_OK)
			return (status);
	}
	for (size_t end = count; end > 1; end--) {
		if (end % 256 == 0) {
			status = sort_poll(sort, error);
			if (status != FSIMG_OK)
				return (status);
		}
		temporary = references[0];
		references[0] = references[end - 1];
		references[end - 1] = temporary;
		status = sift_references(sort, arena, references, end - 1, 0, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (sort_poll(sort, error));
}

static fsimg_status_t
begin_run(fsimg_store_t *store, uint64_t *start, fsimg_error_t *error)
{
	uint64_t end;

	*start = fsimg_store_size(store);
	if (!fsimg_add_u64(*start, 16, &end))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_METADATA, "sort run header overflow"));
	return (fsimg_store_grow(store, end, error));
}

static fsimg_status_t
finish_run(fsimg_store_t *store, uint64_t start, uint64_t count,
    fsimg_error_t *error)
{
	unsigned char header[16];

	fsimg_store_le64(header, fsimg_store_size(store) - start - 16);
	fsimg_store_le64(header + 8, count);
	return (fsimg_store_write(store, start, header, sizeof(header), error));
}

static fsimg_status_t
generate_runs(sort_state_t *sort, fsimg_stream_t *input, size_t run_bytes,
    fsimg_store_t **out, uint64_t *run_count, fsimg_error_t *error)
{
	fsimg_error_t detail;
	sort_reference_t *references;
	unsigned char *arena;
	fsimg_store_t *runs;
	uint64_t cursor, end, start, seen;
	size_t arena_size, capacity, count, used;
	uint32_t length;
	fsimg_status_t status;

	*out = NULL;
	*run_count = 0;
	capacity = run_bytes / 2 / sizeof(*references);
	arena_size = run_bytes - capacity * sizeof(*references);
	if (capacity == 0 || arena_size < sort->maximum)
		return (fsimg_limit_error(error, FSIMG_RESOURCE_MEMORY,
		    run_bytes, (uint64_t)sort->maximum * 2 +
		    2 * sizeof(*references)));
	references = fsimg_alloc(sort->ctx, capacity * sizeof(*references),
	    &detail);
	if (references == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	arena = fsimg_alloc(sort->ctx, arena_size, &detail);
	if (arena == NULL) {
		fsimg_dealloc(references);
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	status = fsimg_store_new(sort->session, &runs, error);
	if (status != FSIMG_OK)
		goto done;
	cursor = 0;
	seen = 0;
	end = input->end;
	while (cursor < end) {
		count = 0;
		used = 0;
		while (cursor < end && count < capacity) {
			status = fsimg_record_length(input->store, cursor, end,
			    sort->maximum, &length, error);
			if (status != FSIMG_OK)
				goto done;
			if (length > arena_size - used)
				break;
			references[count].offset = used;
			references[count].ordinal = seen;
			references[count].length = length;
			status = fsimg_store_read(input->store, cursor + 8,
			    arena + used, length, error);
			if (status != FSIMG_OK)
				goto done;
			used += length;
			count++;
			seen++;
			cursor += 8 + (uint64_t)length;
		}
		status = order_references(sort, arena, references, count, error);
		if (status != FSIMG_OK)
			goto done;
		status = begin_run(runs, &start, error);
		if (status != FSIMG_OK)
			goto done;
		for (size_t i = 0; i < count; i++) {
			status = fsimg_record_append(runs,
			    arena + references[i].offset, references[i].length, error);
			if (status != FSIMG_OK)
				goto done;
		}
		status = finish_run(runs, start, count, error);
		if (status != FSIMG_OK)
			goto done;
		(*run_count)++;
	}
	if (seen != input->count) {
		status = fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_METADATA, "stream record count mismatch");
		goto done;
	}
	*out = runs;
	runs = NULL;
done:
	fsimg_store_free(runs);
	fsimg_dealloc(arena);
	fsimg_dealloc(references);
	return (status);
}

static fsimg_status_t
open_run(fsimg_store_t *store, uint64_t *next, merge_cursor_t *cursor,
    fsimg_error_t *error)
{
	unsigned char header[16];
	uint64_t bytes;
	fsimg_status_t status;

	status = fsimg_store_read(store, *next, header, sizeof(header), error);
	if (status != FSIMG_OK)
		return (status);
	bytes = fsimg_load_le64(header);
	cursor->offset = *next + sizeof(header);
	cursor->remaining = fsimg_load_le64(header + 8);
	if (cursor->remaining == 0 ||
	    !fsimg_range_fits(cursor->offset, bytes, fsimg_store_size(store)) ||
	    cursor->remaining > bytes / 8)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_METADATA, "invalid sort run length or count"));
	cursor->end = cursor->offset + bytes;
	*next = cursor->end;
	return (FSIMG_OK);
}

static fsimg_status_t
advance_cursor(sort_state_t *sort, fsimg_store_t *store,
    merge_cursor_t *cursor, unsigned char *record, fsimg_error_t *error)
{
	fsimg_status_t status;

	if (cursor->remaining == 0) {
		if (cursor->offset != cursor->end)
			return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
			    FSIMG_OP_METADATA, "sort run has trailing bytes"));
		return (FSIMG_DONE);
	}
	status = fsimg_record_length(store, cursor->offset, cursor->end,
	    sort->maximum, &cursor->length, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_store_read(store, cursor->offset + 8, record,
	    cursor->length, error);
	if (status != FSIMG_OK)
		return (status);
	cursor->offset += 8 + (uint64_t)cursor->length;
	cursor->remaining--;
	return (FSIMG_OK);
}

static fsimg_status_t
cursor_before(merge_group_t *group, uint32_t a, uint32_t b, bool *before,
    fsimg_error_t *error)
{
	fsimg_status_t status;
	int result;

	status = compare_records(group->sort, group->records + a * group->stride,
	    group->cursors[a].length, group->records + b * group->stride,
	    group->cursors[b].length, &result, error);
	if (status != FSIMG_OK)
		return (status);
	*before = result < 0 || (result == 0 && a < b);
	return (FSIMG_OK);
}

static fsimg_status_t
sift_cursors(merge_group_t *group, uint32_t root, fsimg_error_t *error)
{
	fsimg_status_t status;
	uint32_t child, temporary;
	bool before;

	/* A min heap exposes the earliest current record, then repairs one path. */
	while (root < group->count / 2) {
		child = root * 2 + 1;
		if (child + 1 < group->count) {
			status = cursor_before(group, group->heap[child + 1],
			    group->heap[child], &before, error);
			if (status != FSIMG_OK)
				return (status);
			if (before)
				child++;
		}
		status = cursor_before(group, group->heap[child],
		    group->heap[root], &before, error);
		if (status != FSIMG_OK)
			return (status);
		if (!before)
			break;
		temporary = group->heap[root];
		group->heap[root] = group->heap[child];
		group->heap[child] = temporary;
		root = child;
	}
	return (FSIMG_OK);
}

static fsimg_status_t
merge_runs(merge_group_t *group, fsimg_store_t *input, fsimg_store_t *output,
    uint64_t start, uint64_t *records, fsimg_error_t *error)
{
	merge_cursor_t *cursor;
	unsigned char *record;
	fsimg_status_t status;
	uint32_t first;

	*records = 0;
	for (uint32_t i = 0; i < group->count; i++) {
		status = open_run(input, &start, &group->cursors[i], error);
		if (status != FSIMG_OK)
			return (status);
		status = advance_cursor(group->sort, input, &group->cursors[i],
		    group->records + i * group->stride, error);
		if (status != FSIMG_OK)
			return (status);
		group->heap[i] = i;
	}
	for (uint32_t i = group->count / 2; i != 0; i--) {
		status = sift_cursors(group, i - 1, error);
		if (status != FSIMG_OK)
			return (status);
	}
	while (group->count != 0) {
		status = sort_poll(group->sort, error);
		if (status != FSIMG_OK)
			return (status);
		first = group->heap[0];
		cursor = &group->cursors[first];
		record = group->records + first * group->stride;
		status = fsimg_record_append(output, record, cursor->length, error);
		if (status != FSIMG_OK)
			return (status);
		(*records)++;
		status = advance_cursor(group->sort, input, cursor, record, error);
		if (status == FSIMG_DONE) {
			group->count--;
			group->heap[0] = group->heap[group->count];
		} else if (status != FSIMG_OK) {
			return (status);
		}
		status = sift_cursors(group, 0, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

static fsimg_status_t
merge_passes(sort_state_t *sort, fsimg_store_t **runs, uint64_t run_count,
    uint32_t fan_in, fsimg_error_t *error)
{
	merge_group_t group = { .sort = sort };
	fsimg_error_t detail;
	fsimg_store_t *replacement;
	unsigned char *storage;
	uint64_t next, start, records, new_count, remaining, header_offset;
	size_t stride, available, bytes;
	fsimg_status_t status;
	uint32_t count;

	if (run_count < 2)
		return (FSIMG_OK);
	group.stride = sort->maximum == 0 ? 1 : sort->maximum;
	stride = sizeof(*group.cursors) + sizeof(*group.heap) + group.stride;
	available = sort->ctx->options.memory_bytes - sort->ctx->stats.memory_bytes;
	/* Leave room for allocation headers and the replacement store handle. */
	available = available > 1024 ? available - 1024 : 0;
	if (fan_in > available / stride)
		fan_in = (uint32_t)(available / stride);
	if (fan_in < 2)
		return (fsimg_limit_error(error, FSIMG_RESOURCE_MEMORY,
		    available, stride * 2));
	bytes = fan_in * stride;
	storage = fsimg_alloc(sort->ctx, bytes, &detail);
	if (storage == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	group.cursors = (merge_cursor_t *)storage;
	group.heap = (uint32_t *)(storage + fan_in * sizeof(*group.cursors));
	group.records = storage + fan_in *
	    (sizeof(*group.cursors) + sizeof(*group.heap));
	status = FSIMG_OK;
	replacement = NULL;
	while (run_count > 1) {
		sort->phase = "metadata merge";
		status = sort_poll(sort, error);
		if (status != FSIMG_OK)
			break;
		status = fsimg_store_new(sort->session, &replacement, error);
		if (status != FSIMG_OK)
			break;
		next = 0;
		new_count = 0;
		remaining = run_count;
		while (remaining != 0) {
			count = remaining < fan_in ? (uint32_t)remaining : fan_in;
			group.count = count;
			start = next;
			/* Discover only this group's headers; no global run registry. */
			for (uint32_t i = 0; i < count; i++) {
				status = open_run(*runs, &next, &group.cursors[i], error);
				if (status != FSIMG_OK)
					goto done;
			}
			status = begin_run(replacement, &header_offset, error);
			if (status != FSIMG_OK)
				goto done;
			status = merge_runs(&group, *runs, replacement, start,
			    &records, error);
			if (status != FSIMG_OK)
				goto done;
			status = finish_run(replacement, header_offset, records, error);
			if (status != FSIMG_OK)
				goto done;
			remaining -= count;
			new_count++;
		}
		if (next != fsimg_store_size(*runs)) {
			status = fsimg_error_set(error, FSIMG_ECORRUPT, 0,
			    FSIMG_OP_METADATA, "sort run stream has trailing bytes");
			break;
		}
		fsimg_store_free(*runs);
		*runs = replacement;
		replacement = NULL;
		run_count = new_count;
	}
done:
	fsimg_store_free(replacement);
	fsimg_dealloc(storage);
	return (status);
}

static fsimg_status_t
publish_result(sort_state_t *sort, fsimg_store_t *runs, uint64_t count,
    fsimg_stream_t **out, fsimg_error_t *error)
{
	merge_cursor_t cursor;
	fsimg_stream_t *result;
	fsimg_error_t detail;
	unsigned char *record;
	uint64_t next;
	fsimg_status_t status;

	status = fsimg_stream_new(sort->session, sort->maximum, &result, error);
	if (status != FSIMG_OK)
		return (status);
	record = fsimg_alloc(sort->ctx, sort->maximum == 0 ? 1 : sort->maximum,
	    &detail);
	if (record == NULL) {
		status = detail.status;
		if (error != NULL)
			*error = detail;
		goto done;
	}
	if (count != 0) {
		next = 0;
		status = open_run(runs, &next, &cursor, error);
		if (status != FSIMG_OK)
			goto done;
		if (next != fsimg_store_size(runs) || cursor.remaining != count) {
			status = fsimg_error_set(error, FSIMG_ECORRUPT, 0,
			    FSIMG_OP_METADATA, "final sort run count mismatch");
			goto done;
		}
		for (;;) {
			status = advance_cursor(sort, runs, &cursor, record, error);
			if (status == FSIMG_DONE)
				break;
			if (status != FSIMG_OK)
				goto done;
			status = fsimg_stream_append(result, record, cursor.length,
			    NULL, error);
			if (status != FSIMG_OK)
				goto done;
		}
	}
	status = fsimg_stream_seal(result, error);
	if (status != FSIMG_OK)
		goto done;
	*out = result;
	result = NULL;
done:
	fsimg_dealloc(record);
	fsimg_stream_free(result);
	return (status);
}

fsimg_status_t
fsimg_stream_sort(fsimg_stream_t *input, fsimg_compare_fn_t compare,
    void *argument, const fsimg_sort_options_t *options, fsimg_stream_t **out,
    fsimg_error_t *error)
{
	sort_state_t sort = { .compare = compare, .argument = argument };
	fsimg_store_t *runs;
	uint64_t run_count;
	size_t run_bytes;
	uint32_t fan_in;
	fsimg_status_t status;

	if (out != NULL)
		*out = NULL;
	if (input == NULL || compare == NULL || out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_METADATA, "missing sort argument"));
	sort.ctx = fsimg_store_context(input->store);
	sort.session = fsimg_store_session(input->store);
	sort.maximum = input->max_record;
	sort.phase = "metadata runs";
	status = fsimg_ctx_ready(sort.ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (!input->sealed)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_METADATA, "sort input must be sealed"));
	run_bytes = options == NULL ? 0 : options->run_bytes;
	if (run_bytes == 0) {
		run_bytes = sort.ctx->options.memory_bytes / 8;
		if (run_bytes > 4 * 1024 * 1024)
			run_bytes = 4 * 1024 * 1024;
	}
	fan_in = options == NULL ? 0 : options->fan_in;
	if (fan_in == 0)
		fan_in = 16;
	if (fan_in < 2 || fan_in > 64)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_METADATA, "sort fan-in must be between 2 and 64"));
	status = sort_poll(&sort, error);
	if (status != FSIMG_OK)
		return (status);
	status = generate_runs(&sort, input, run_bytes, &runs, &run_count, error);
	if (status != FSIMG_OK)
		return (status);
	status = merge_passes(&sort, &runs, run_count, fan_in, error);
	if (status == FSIMG_OK)
		status = publish_result(&sort, runs, input->count, out, error);
	fsimg_store_free(runs);
	return (status);
}
