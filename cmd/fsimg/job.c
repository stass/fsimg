/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "internal.h"
#include "worker.h"

/* Each numeric field occupies eight little-endian bytes. Fixed strings follow
 * at byte 200; the remainder is zero reserved space. This enum names wire
 * positions only and is independent of native struct layout or padding.
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
	JOB_LABEL_OFFSET = 216,
	JOB_LABEL_PRESENT = 264,
	JOB_RESERVED_OFFSET = 265
} job_string_field_t;

static void put_field(unsigned char *, job_field_t, uint64_t);
static uint64_t get_field(const unsigned char *, job_field_t);
static fsimg_status_t invalid_job(fsimg_error_t *);

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

	if (request->selection == NULL || memchr(request->format, '\0', 16) == NULL)
		return (invalid_job(error));
	label = request->write.label == NULL ? "" : request->write.label;
	label_length = strnlen(label, 48);
	if (label_length == 48)
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
	    FSIMG_JOB_VERBOSE)) != 0 || get_field(raw, JOB_CAPABILITIES) > 15 ||
	    memchr(raw + JOB_FORMAT_OFFSET, '\0', 16) == NULL ||
	    memchr(raw + JOB_LABEL_OFFSET, '\0', 48) == NULL ||
	    raw[JOB_LABEL_PRESENT] > 1 ||
	    (raw[JOB_LABEL_PRESENT] == 0 && raw[JOB_LABEL_OFFSET] != 0))
		return (invalid_job(error));
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
	if (get_field(raw, JOB_WRITE_FLAGS) > UINT32_MAX)
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
	bcopy(raw + JOB_FORMAT_OFFSET, request->format, 16);
	bcopy(raw + JOB_LABEL_OFFSET, request->label_storage, 48);
	request->write.label = raw[JOB_LABEL_PRESENT] == 0 ? NULL :
	    request->label_storage;
	*selection_length = get_field(raw, JOB_SELECTION);
	*capabilities = (unsigned)get_field(raw, JOB_CAPABILITIES);
	path_limit = request->limits.max_path_bytes == 0 ? 65536 :
	    request->limits.max_path_bytes;
	if (*selection_length == 0 || *selection_length >= path_limit)
		return (invalid_job(error));
	return (FSIMG_OK);
}
