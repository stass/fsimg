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
 * percentage as eight little-endian bytes each. The remainder is zero
 * reserved space. These enums name wire positions and sizes only and are
 * independent of native struct layout or padding.
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
	JOB_RESERVED_OFFSET = 312
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
	request->scratch_fd = request->source_fd = -1;
}

fsimg_status_t
fsimg_job_encode(const fsimg_worker_request_t *request, unsigned char *raw,
    fsimg_error_t *error)
{
	uint64_t capabilities;
	uint32_t path_limit;
	size_t selection_length, label_length;
	const char *label;

	if (request->selection == NULL ||
	    memchr(request->format, '\0', JOB_FORMAT_BYTES) == NULL)
		return (invalid_job(error));
	label = request->write.label == NULL ? "" : request->write.label;
	label_length = strnlen(label, JOB_LABEL_BYTES);
	if (label_length == JOB_LABEL_BYTES)
		return (invalid_job(error));
	path_limit = request->limits.max_path_bytes == 0 ?
	    65536 : request->limits.max_path_bytes;
	selection_length = strnlen(request->selection, path_limit);
	if (selection_length >= path_limit)
		return (fsimg_limit_error(error, FSIMG_RESOURCE_PATH,
		    path_limit, (uint64_t)selection_length + 1));
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
	capabilities = (request->input_fd != -1 ? 1u : 0u) |
	    (request->output_fd != -1 ? 2u : 0u) |
	    (request->scratch_fd != -1 ? 4u : 0u) | (request->source_fd != -1 ? 8u : 0u);
	put_field(raw, JOB_CAPABILITIES, capabilities);
	put_field(raw, JOB_SELECTION, selection_length);
	bcopy(request->format, raw + JOB_FORMAT_OFFSET, strlen(request->format));
	bcopy(label, raw + JOB_LABEL_OFFSET, label_length);
	raw[JOB_LABEL_PRESENT] = request->write.label != NULL ? 1 : 0;
	bcopy(request->write.volume_uuid, raw + JOB_UUID_OFFSET,
	    sizeof(request->write.volume_uuid));
	raw[JOB_BYTE_ORDER] = (unsigned char)request->write.byte_order;
	fsimg_wire_put64(raw + JOB_INODES_OFFSET, request->write.inode_capacity);
	fsimg_wire_put64(raw + JOB_RESERVE_OFFSET, request->write.reserve_percent);
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_job_decode(const unsigned char *raw, uint32_t length,
    fsimg_worker_request_t *request, uint64_t *selection_length,
    unsigned *capabilities, fsimg_error_t *error)
{
	uint64_t epoch;
	uint32_t path_limit;

	fsimg_worker_request_init(request);
	if (length != FSIMG_JOB_BYTES)
		return (invalid_job(error));
	if (get_field(raw, JOB_KIND) < FSIMG_JOB_INFO ||
	    get_field(raw, JOB_KIND) > FSIMG_JOB_PLAN ||
	    get_field(raw, JOB_SANDBOX) > FSIMG_SANDBOX_OFF ||
	    (get_field(raw, JOB_FLAGS) & ~(FSIMG_JOB_RECURSIVE | FSIMG_JOB_PAYLOAD |
	    FSIMG_JOB_VERBOSE | FSIMG_JOB_SKIP_UNSUPPORTED)) != 0 ||
	    get_field(raw, JOB_CAPABILITIES) > 15 ||
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
	path_limit = request->limits.max_path_bytes == 0 ? 65536 :
	    request->limits.max_path_bytes;
	if (*selection_length == 0 || *selection_length >= path_limit)
		return (invalid_job(error));
	return (FSIMG_OK);
}
