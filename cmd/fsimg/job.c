/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "internal.h"
#include "worker.h"

/* Each numeric field occupies eight little-endian bytes. Fixed strings
 * follow at byte 200: the format name in 16 bytes and the label in 49, each
 * NUL-terminated and zero after the terminator, then one byte saying
 * whether a label is present. Then come the write settings: the 16-byte
 * UUID, the byte order as one byte, and the inode capacity and reserve
 * percentage as eight little-endian bytes each, and the number of create
 * roots as eight bytes. These enums name wire positions and sizes only and
 * are independent of native struct layout or padding.
 *
 * The job's image paths follow the record in SELECTION frames, as one
 * sequence of NUL-terminated paths whose total length the record states:
 * a read job sends its one member, a create job one path per root.
 */
typedef enum job_field {
	JOB_KIND, JOB_SANDBOX, JOB_FLAGS, JOB_MEMORY, JOB_SCRATCH, JOB_ENTRIES,
	JOB_OUTPUT, JOB_WORK, JOB_DEPTH, JOB_PATH, JOB_FDS, JOB_OFFSET, JOB_LENGTH,
	JOB_WRITE_FLAGS, JOB_VOLUME, JOB_FREE, JOB_EPOCH, JOB_SECTOR, JOB_CLUSTER,
	JOB_ROOT, JOB_SERIAL, JOB_CODEPAGE, JOB_SECONDS, JOB_CAPABILITIES,
	JOB_SELECTION
} job_field_t;

typedef enum job_string_field {
	JOB_FORMAT_OFFSET = 200,
	JOB_FORMAT_BYTES = 16,
	JOB_LABEL_OFFSET = 216,
	JOB_LABEL_BYTES = 49,
	JOB_LABEL_PRESENT = JOB_LABEL_OFFSET + JOB_LABEL_BYTES,
	JOB_UUID_OFFSET = 272,
	JOB_BYTE_ORDER = 288,
	JOB_INODES_OFFSET = 296,
	JOB_RESERVE_OFFSET = 304,
	JOB_ROOTS_OFFSET = 312,
	JOB_RESERVED_OFFSET = 320
} job_string_field_t;
_Static_assert(JOB_FORMAT_OFFSET + JOB_FORMAT_BYTES == JOB_LABEL_OFFSET &&
    JOB_LABEL_PRESENT < JOB_UUID_OFFSET, "the string fields do not overlap");
_Static_assert(sizeof(((fsimg_worker_request_t *)0)->format) ==
    JOB_FORMAT_BYTES && sizeof(((fsimg_worker_request_t *)0)->label_storage) ==
    JOB_LABEL_BYTES, "the request holds each string field");

static void put_field(unsigned char *, job_field_t, uint64_t);
static uint64_t get_field(const unsigned char *, job_field_t);
static fsimg_status_t invalid_job(fsimg_error_t *);
static bool string_field_valid(const unsigned char *, size_t);

static void
put_field(unsigned char *raw, job_field_t field, uint64_t value)
{
	fsimg_wire_put64(raw + (unsigned)field * 8, value);
}

static uint64_t
get_field(const unsigned char *raw, job_field_t field)
{
	return (fsimg_wire_get64(raw + (unsigned)field * 8));
}

/* A string field holds a terminator, and only zero bytes follow it, so a
 * record has one encoding.
 */
static bool
string_field_valid(const unsigned char *field, size_t bytes)
{
	const unsigned char *end;

	end = memchr(field, '\0', bytes);
	if (end == NULL)
		return (false);
	for (; end < field + bytes; end++) {
		if (*end != 0)
			return (false);
	}
	return (true);
}

static fsimg_status_t
invalid_job(fsimg_error_t *error)
{
	return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_CONTEXT,
	    "invalid worker job description"));
}

void
fsimg_worker_request_init(fsimg_worker_request_t *request)
{
	bzero(request, sizeof(*request));
	fsimg_options_init(&request->limits);
	fsimg_write_options_init(&request->write);
	request->selection = "/";
	request->codepage = 437;
	request->input_fd = request->output_fd = -1;
	request->scratch_fd = -1;
}

bool
fsimg_job_has_roots(fsimg_job_kind_t kind)
{
	return (kind == FSIMG_JOB_CREATE || kind == FSIMG_JOB_PLAN);
}

uint32_t
fsimg_job_path_count(const fsimg_worker_request_t *request)
{
	return (fsimg_job_has_roots(request->kind) ? request->root_count : 1);
}

const char *
fsimg_job_path(const fsimg_worker_request_t *request, uint32_t index)
{
	return (fsimg_job_has_roots(request->kind) ? request->root_paths[index] :
	    request->selection);
}

/* The bytes of the job's path sequence, each path with its terminator.
 * Every path must be shorter than the path limit; since there are at most
 * UINT32_MAX paths of at most UINT32_MAX bytes, the total fits a uint64_t.
 */
fsimg_status_t
fsimg_job_paths_length(const fsimg_worker_request_t *request, uint64_t *total,
    fsimg_error_t *error)
{
	const char *path;
	uint32_t path_limit, count;
	size_t length;

	path_limit = request->limits.max_path_bytes == 0 ?
	    65536 : request->limits.max_path_bytes;
	count = fsimg_job_path_count(request);
	*total = 0;
	for (uint32_t i = 0; i < count; i++) {
		path = fsimg_job_path(request, i);
		if (path == NULL)
			return (invalid_job(error));
		length = strnlen(path, path_limit);
		if (length >= path_limit)
			return (fsimg_limit_error(error, FSIMG_RESOURCE_PATH,
			    path_limit, (uint64_t)length + 1));
		*total += (uint64_t)length + 1;
	}
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_job_encode(const fsimg_worker_request_t *request, unsigned char *raw,
    fsimg_error_t *error)
{
	uint64_t capabilities, paths_length;
	size_t label_length;
	const char *label;
	fsimg_status_t status;

	if (memchr(request->format, '\0', JOB_FORMAT_BYTES) == NULL ||
	    fsimg_job_has_roots(request->kind) != (request->root_count != 0))
		return (invalid_job(error));
	label = request->write.label == NULL ? "" : request->write.label;
	label_length = strnlen(label, JOB_LABEL_BYTES);
	if (label_length == JOB_LABEL_BYTES)
		return (invalid_job(error));
	status = fsimg_job_paths_length(request, &paths_length, error);
	if (status != FSIMG_OK)
		return (status);
	bzero(raw, FSIMG_JOB_BYTES);
	put_field(raw, JOB_KIND, request->kind);
	put_field(raw, JOB_SANDBOX, request->sandbox);
	put_field(raw, JOB_FLAGS, request->flags);
	put_field(raw, JOB_MEMORY, request->limits.memory_bytes);
	put_field(raw, JOB_SCRATCH, request->limits.scratch_bytes);
	put_field(raw, JOB_ENTRIES, request->limits.max_entries);
	put_field(raw, JOB_OUTPUT, request->limits.max_output_bytes);
	put_field(raw, JOB_WORK, request->limits.max_work);
	put_field(raw, JOB_DEPTH, request->limits.max_depth);
	put_field(raw, JOB_PATH, request->limits.max_path_bytes);
	put_field(raw, JOB_FDS, request->limits.max_open_files);
	put_field(raw, JOB_OFFSET, request->offset);
	put_field(raw, JOB_LENGTH, request->length);
	put_field(raw, JOB_WRITE_FLAGS, request->write.flags);
	put_field(raw, JOB_VOLUME, request->write.volume_bytes);
	put_field(raw, JOB_FREE, request->write.min_free_bytes);
	put_field(raw, JOB_EPOCH, (uint64_t)request->write.epoch);
	put_field(raw, JOB_SECTOR, request->write.sector_bytes);
	put_field(raw, JOB_CLUSTER, request->write.allocation_unit_bytes);
	put_field(raw, JOB_ROOT, request->write.fat_root_entries);
	put_field(raw, JOB_SERIAL, request->write.volume_serial);
	put_field(raw, JOB_CODEPAGE, request->codepage);
	put_field(raw, JOB_SECONDS, request->seconds);
	capabilities = (request->input_fd != -1 ? FSIMG_JOB_CAP_INPUT : 0) |
	    (request->output_fd != -1 ? FSIMG_JOB_CAP_OUTPUT : 0) |
	    (request->scratch_fd != -1 ? FSIMG_JOB_CAP_SCRATCH : 0) |
	    (request->root_count != 0 ? FSIMG_JOB_CAP_ROOTS : 0);
	put_field(raw, JOB_CAPABILITIES, capabilities);
	put_field(raw, JOB_SELECTION, paths_length);
	bcopy(request->format, raw + JOB_FORMAT_OFFSET, strlen(request->format));
	bcopy(label, raw + JOB_LABEL_OFFSET, label_length);
	raw[JOB_LABEL_PRESENT] = request->write.label != NULL ? 1 : 0;
	bcopy(request->write.volume_uuid, raw + JOB_UUID_OFFSET,
	    sizeof(request->write.volume_uuid));
	raw[JOB_BYTE_ORDER] = (unsigned char)request->write.byte_order;
	fsimg_wire_put64(raw + JOB_INODES_OFFSET, request->write.inode_capacity);
	fsimg_wire_put64(raw + JOB_RESERVE_OFFSET, request->write.reserve_percent);
	fsimg_wire_put64(raw + JOB_ROOTS_OFFSET, request->root_count);
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_job_decode(const unsigned char *raw, uint32_t length,
    fsimg_worker_request_t *request, uint64_t *selection_length,
    unsigned *capabilities, fsimg_error_t *error)
{
	uint64_t epoch, roots, count;
	uint32_t path_limit;

	fsimg_worker_request_init(request);
	if (length != FSIMG_JOB_BYTES)
		return (invalid_job(error));
	if (get_field(raw, JOB_KIND) < FSIMG_JOB_INFO ||
	    get_field(raw, JOB_KIND) > FSIMG_JOB_PLAN ||
	    get_field(raw, JOB_SANDBOX) > FSIMG_SANDBOX_OFF ||
	    (get_field(raw, JOB_FLAGS) & ~(FSIMG_JOB_RECURSIVE | FSIMG_JOB_PAYLOAD |
	    FSIMG_JOB_VERBOSE | FSIMG_JOB_SKIP_UNSUPPORTED)) != 0 ||
	    get_field(raw, JOB_CAPABILITIES) > (FSIMG_JOB_CAP_INPUT |
	    FSIMG_JOB_CAP_OUTPUT | FSIMG_JOB_CAP_SCRATCH | FSIMG_JOB_CAP_ROOTS) ||
	    !string_field_valid(raw + JOB_FORMAT_OFFSET, JOB_FORMAT_BYTES) ||
	    !string_field_valid(raw + JOB_LABEL_OFFSET, JOB_LABEL_BYTES) ||
	    raw[JOB_LABEL_PRESENT] > 1 || raw[JOB_BYTE_ORDER] > FSIMG_BYTE_ORDER_BIG ||
	    (raw[JOB_LABEL_PRESENT] == 0 && raw[JOB_LABEL_OFFSET] != 0))
		return (invalid_job(error));
	for (unsigned i = JOB_LABEL_PRESENT + 1; i < JOB_UUID_OFFSET; i++) {
		if (raw[i] != 0)
			return (invalid_job(error));
	}
	for (unsigned i = JOB_BYTE_ORDER + 1; i < JOB_INODES_OFFSET; i++) {
		if (raw[i] != 0)
			return (invalid_job(error));
	}
	for (unsigned i = JOB_RESERVED_OFFSET; i < FSIMG_JOB_BYTES; i++) {
		if (raw[i] != 0)
			return (invalid_job(error));
	}
	for (job_field_t field = JOB_DEPTH; field <= JOB_FDS; field++) {
		if (get_field(raw, field) > UINT32_MAX)
			return (invalid_job(error));
	}
	for (job_field_t field = JOB_SECTOR; field <= JOB_CODEPAGE; field++) {
		if (get_field(raw, field) > UINT32_MAX)
			return (invalid_job(error));
	}
	if (get_field(raw, JOB_WRITE_FLAGS) > UINT32_MAX ||
	    fsimg_wire_get64(raw + JOB_INODES_OFFSET) > UINT32_MAX ||
	    fsimg_wire_get64(raw + JOB_RESERVE_OFFSET) > UINT32_MAX)
		return (invalid_job(error));
	/* Roots exist exactly for create jobs, at most one per permitted
	 * descriptor, and arrive with the roots capability.
	 */
	roots = fsimg_wire_get64(raw + JOB_ROOTS_OFFSET);
	if (fsimg_job_has_roots((fsimg_job_kind_t)get_field(raw, JOB_KIND)) !=
	    (roots != 0) || roots > get_field(raw, JOB_FDS) ||
	    ((get_field(raw, JOB_CAPABILITIES) & FSIMG_JOB_CAP_ROOTS) != 0) !=
	    (roots != 0))
		return (invalid_job(error));
	request->kind = (fsimg_job_kind_t)get_field(raw, JOB_KIND);
	request->sandbox = (fsimg_sandbox_policy_t)get_field(raw, JOB_SANDBOX);
	request->flags = (unsigned)get_field(raw, JOB_FLAGS);
	request->limits.memory_bytes = get_field(raw, JOB_MEMORY);
	request->limits.scratch_bytes = get_field(raw, JOB_SCRATCH);
	request->limits.max_entries = get_field(raw, JOB_ENTRIES);
	request->limits.max_output_bytes = get_field(raw, JOB_OUTPUT);
	request->limits.max_work = get_field(raw, JOB_WORK);
	request->limits.max_depth = (uint32_t)get_field(raw, JOB_DEPTH);
	request->limits.max_path_bytes = (uint32_t)get_field(raw, JOB_PATH);
	request->limits.max_open_files = (uint32_t)get_field(raw, JOB_FDS);
	request->offset = get_field(raw, JOB_OFFSET);
	request->length = get_field(raw, JOB_LENGTH);
	request->seconds = get_field(raw, JOB_SECONDS);
	request->write.flags = (uint32_t)get_field(raw, JOB_WRITE_FLAGS);
	request->write.volume_bytes = get_field(raw, JOB_VOLUME);
	request->write.min_free_bytes = get_field(raw, JOB_FREE);
	epoch = get_field(raw, JOB_EPOCH);
	request->write.epoch = epoch <= INT64_MAX ? (int64_t)epoch :
	    -1 - (int64_t)(UINT64_MAX - epoch);
	request->write.sector_bytes = (uint32_t)get_field(raw, JOB_SECTOR);
	request->write.allocation_unit_bytes = (uint32_t)get_field(raw, JOB_CLUSTER);
	request->write.fat_root_entries = (uint32_t)get_field(raw, JOB_ROOT);
	request->write.volume_serial = (uint32_t)get_field(raw, JOB_SERIAL);
	request->codepage = (uint32_t)get_field(raw, JOB_CODEPAGE);
	bcopy(raw + JOB_FORMAT_OFFSET, request->format, JOB_FORMAT_BYTES);
	bcopy(raw + JOB_LABEL_OFFSET, request->label_storage, JOB_LABEL_BYTES);
	request->write.label = raw[JOB_LABEL_PRESENT] == 0 ? NULL :
	    request->label_storage;
	bcopy(raw + JOB_UUID_OFFSET, request->write.volume_uuid,
	    sizeof(request->write.volume_uuid));
	request->write.byte_order = (fsimg_byte_order_t)raw[JOB_BYTE_ORDER];
	request->write.inode_capacity =
	    (uint32_t)fsimg_wire_get64(raw + JOB_INODES_OFFSET);
	request->write.reserve_percent =
	    (uint32_t)fsimg_wire_get64(raw + JOB_RESERVE_OFFSET);
	*selection_length = get_field(raw, JOB_SELECTION);
	*capabilities = (unsigned)get_field(raw, JOB_CAPABILITIES);
	/* Each path is at least "/" and its terminator, and shorter than the
	 * path limit.
	 */
	path_limit = request->limits.max_path_bytes == 0 ? 65536 :
	    request->limits.max_path_bytes;
	count = roots == 0 ? 1 : roots;
	if (*selection_length < 2 * count ||
	    *selection_length > count * path_limit)
		return (invalid_job(error));
	/* Last, so a refused record leaves no roots for cleanup to close. */
	request->root_count = (uint32_t)roots;
	return (FSIMG_OK);
}
