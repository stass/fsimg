/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/resource.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "extract.h"
#include "host.h"
#include "sandbox.h"
#include "source.h"
#include "selection.h"
#include "unicode.h"
#include "worker.h"

typedef enum worker_capability_slot {
	CHILD_INPUT_CAP,
	CHILD_OUTPUT_CAP,
	CHILD_SCRATCH_CAP,
	CHILD_SOURCE_CAP,
	CHILD_CAP_COUNT
} worker_capability_slot_t;

typedef struct worker_child {
	fsimg_worker_request_t request;
	fsimg_worker_result_t result;
	fsimg_ctx_t	*ctx;
	fsimg_io_t	*input;
	fsimg_io_t	*volume;
	fsimg_reader_t	*reader;
	fsimg_selection_t *selector;
	fsimg_writer_t	*writer;
	char		*selection;
	uint64_t	 last_progress;
	unsigned	 losses_sent;
	/* Raw filesystem capabilities remain here until an owner adopts them. */
	int		 raw_caps[CHILD_CAP_COUNT];
	int		 output_root;
	bool		 processing;
	bool		 channel_failed;
} worker_child_t;

static bool child_cancelled(void *);
static void child_progress(void *, const fsimg_progress_t *);
static void child_loss(void *, const char *, uint32_t);
static fsimg_status_t prepare_capabilities(worker_child_t *, unsigned,
    fsimg_sandbox_caps_t *, fsimg_error_t *);
static fsimg_status_t prepare_context(worker_child_t *, const fsimg_sandbox_caps_t *,
    fsimg_error_t *);
static fsimg_status_t read_selection(worker_child_t *, uint64_t, fsimg_error_t *);
static fsimg_status_t child_setup(worker_child_t *, fsimg_sandbox_caps_t *,
    fsimg_error_t *);
static fsimg_status_t child_ready(worker_child_t *, const fsimg_sandbox_caps_t *,
    fsimg_error_t *);
static fsimg_status_t send_info(worker_child_t *, fsimg_error_t *);
static fsimg_status_t send_plan(const fsimg_plan_info_t *, fsimg_error_t *);
static fsimg_status_t send_entry(const fsimg_entry_t *, fsimg_error_t *);
static fsimg_status_t read_job(worker_child_t *, fsimg_error_t *);
static fsimg_status_t create_job(worker_child_t *, fsimg_error_t *);
static fsimg_status_t list_job(worker_child_t *, fsimg_error_t *);
static fsimg_status_t cat_job(worker_child_t *, fsimg_error_t *);
static fsimg_status_t verify_job(worker_child_t *, fsimg_error_t *);
static fsimg_status_t extract_job(worker_child_t *, fsimg_error_t *);
static void encode_time(unsigned char *, const fsimg_timestamp_t *);
static void child_cleanup(worker_child_t *);

static bool
child_cancelled(void *argument)
{
	worker_child_t *child = argument;
	unsigned char byte;
	ssize_t count;

	if (child->channel_failed)
		return (true);
	count = read(FSIMG_CHILD_CANCEL, &byte, 1);
	if (count == -1 && (errno == EAGAIN || errno == EINTR))
		return (false);
	/* EOF also means the coordinator died. No further output may be written. */
	return (true);
}

static void
child_progress(void *argument, const fsimg_progress_t *progress)
{
	worker_child_t *child = argument;
	struct timespec time;
	unsigned char raw[24];
	uint64_t milliseconds;

	if (!child->processing || (child->request.flags & FSIMG_JOB_VERBOSE) == 0)
		return;
	if (clock_gettime(CLOCK_MONOTONIC, &time) == -1)
		return;
	milliseconds = (uint64_t)time.tv_sec * 1000 + (uint64_t)time.tv_nsec / 1000000;
	if (milliseconds - child->last_progress < 250)
		return;
	child->last_progress = milliseconds;
	fsimg_wire_put64(raw, progress->entries);
	fsimg_wire_put64(raw + 8, progress->bytes);
	fsimg_wire_put64(raw + 16, progress->work);
	if (fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_PROGRESS,
	    raw, sizeof(raw), NULL) != FSIMG_OK)
		child->channel_failed = true;
}

static void
child_loss(void *argument, const char *path, uint32_t fields)
{
	worker_child_t *child = argument;
	unsigned char raw[FSIMG_WIRE_LOSS_BYTES + FSIMG_WIRE_LOSS_PATH_BYTES];
	size_t length;

	/* Frames provide bounded examples; DONE retains the exact aggregate. */
	if (!child->processing || child->losses_sent >= FSIMG_WIRE_LOSS_EVENTS)
		return;
	child->losses_sent++;
	length = strnlen(path, FSIMG_WIRE_LOSS_PATH_BYTES);
	bzero(raw, sizeof(raw));
	fsimg_wire_put64(raw + FSIMG_WIRE_LOSS_FIELDS, fields);
	fsimg_wire_put64(raw + FSIMG_WIRE_LOSS_TRUNCATED, length == FSIMG_WIRE_LOSS_PATH_BYTES ? 1 : 0);
	fsimg_wire_put64(raw + FSIMG_WIRE_LOSS_LENGTH, length);
	bcopy(path, raw + FSIMG_WIRE_LOSS_PATH, length);
	if (fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_LOSS, raw,
	    (uint32_t)(FSIMG_WIRE_LOSS_BYTES + length), NULL) != FSIMG_OK)
		child->channel_failed = true;
}

static fsimg_status_t
prepare_capabilities(worker_child_t *child, unsigned capabilities,
    fsimg_sandbox_caps_t *caps, fsimg_error_t *error)
{
	unsigned expected;

	expected = child->request.kind >= FSIMG_JOB_CREATE ? 8 : 1;
	if (child->request.kind == FSIMG_JOB_CREATE || child->request.kind == FSIMG_JOB_EXTRACT)
		expected |= 2;
	if ((capabilities & ~4u) != expected)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_CONTEXT, "worker job has excess or missing filesystem authority"));
	*caps = (fsimg_sandbox_caps_t){
		.input = (capabilities & 1) != 0 ? FSIMG_CHILD_INPUT : -1,
		.output = (capabilities & 2) != 0 ? FSIMG_CHILD_OUTPUT : -1,
		.scratch = (capabilities & 4) != 0 ? FSIMG_CHILD_SCRATCH : -1,
		.source = (capabilities & 8) != 0 ? FSIMG_CHILD_SOURCE : -1,
		.job = FSIMG_CHILD_JOB, .control = FSIMG_CHILD_CONTROL,
		.data = child->request.kind == FSIMG_JOB_CAT ? FSIMG_CHILD_DATA : -1,
		.cancel = FSIMG_CHILD_CANCEL
	};
	child->raw_caps[CHILD_INPUT_CAP] = caps->input;
	child->raw_caps[CHILD_OUTPUT_CAP] = caps->output;
	child->raw_caps[CHILD_SCRATCH_CAP] = caps->scratch;
	child->raw_caps[CHILD_SOURCE_CAP] = caps->source;
	for (unsigned i = 0; i < CHILD_CAP_COUNT; i++) {
		if (child->raw_caps[i] == -1)
			close(FSIMG_CHILD_INPUT + (int)i);
	}
	if (caps->data == -1)
		close(FSIMG_CHILD_DATA);

	return (FSIMG_OK);
}

static fsimg_status_t
prepare_context(worker_child_t *child, const fsimg_sandbox_caps_t *caps,
    fsimg_error_t *error)
{
	fsimg_status_t status;
	int fd;

	child->request.limits.scratch_directory_fd = -1;
	child->request.limits.cancelled = child_cancelled;
	child->request.limits.cancel_argument = child;
	child->request.limits.progress = child_progress;
	child->request.limits.progress_argument = child;
	child->request.limits.loss = child_loss;
	child->request.limits.loss_argument = child;
	status = fsimg_ctx_new(&child->request.limits, &child->ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (caps->scratch != -1) {
		status = fsimg_fd_reserve(child->ctx, error);
		if (status != FSIMG_OK)
			return (status);
		fd = child->raw_caps[CHILD_SCRATCH_CAP];
		child->raw_caps[CHILD_SCRATCH_CAP] = -1;
		status = fsimg_ctx_adopt_scratch(child->ctx, fd, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (caps->input != -1) {
		status = fsimg_fd_reserve(child->ctx, error);
		if (status != FSIMG_OK)
			return (status);
		fd = child->raw_caps[CHILD_INPUT_CAP];
		child->raw_caps[CHILD_INPUT_CAP] = -1;
		status = fsimg_io_adopt_fd(child->ctx, fd, FSIMG_IO_READ, &child->input, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (caps->output != -1) {
		status = fsimg_fd_reserve(child->ctx, error);
		if (status != FSIMG_OK)
			return (status);
		child->output_root = child->raw_caps[CHILD_OUTPUT_CAP];
		child->raw_caps[CHILD_OUTPUT_CAP] = -1;
	}
	return (FSIMG_OK);
}

static fsimg_status_t
read_selection(worker_child_t *child, uint64_t selection_length, fsimg_error_t *error)
{
	fsimg_frame_t frame;
	uint64_t received;
	fsimg_status_t status;

	child->selection = fsimg_alloc(child->ctx, (size_t)selection_length + 1, error);
	if (child->selection == NULL)
		return (error->status);
	received = 0;
	while (received < selection_length) {
		status = fsimg_frame_read(FSIMG_CHILD_JOB, &frame, error);
		if (status != FSIMG_OK)
			return (status);
		if (frame.type != FSIMG_FRAME_SELECTION || frame.length == 0 ||
		    frame.length > selection_length - received ||
		    memchr(frame.payload, '\0', frame.length) != NULL)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_CONTEXT, "invalid worker selection framing"));
		bcopy(frame.payload, child->selection + received, frame.length);
		received += frame.length;
	}
	child->request.selection = child->selection;
	return (FSIMG_OK);
}

static fsimg_status_t
child_setup(worker_child_t *child, fsimg_sandbox_caps_t *caps, fsimg_error_t *error)
{
	const struct rlimit core_limit = { .rlim_cur = 0, .rlim_max = 0 };
	fsimg_frame_t frame;
	uint64_t selection_length;
	unsigned capabilities;
	fsimg_status_t status;

	if (setrlimit(RLIMIT_CORE, &core_limit) == -1)
		return (fsimg_error_set(error, FSIMG_EIO, errno,
		    FSIMG_OP_CONTEXT, "worker core-dump restriction failed"));
	if (chdir("/") == -1)
		return (fsimg_error_set(error, FSIMG_EIO, errno,
		    FSIMG_OP_CONTEXT, "worker working-directory setup failed"));
	status = fsimg_frame_read(FSIMG_CHILD_JOB, &frame, error);
	if (status != FSIMG_OK)
		return (status);
	if (frame.type != FSIMG_FRAME_JOB)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_CONTEXT, "worker startup requires a job frame"));
	status = fsimg_job_decode(frame.payload, frame.length, &child->request,
	    &selection_length, &capabilities, error);
	if (status != FSIMG_OK)
		return (status);

	status = prepare_capabilities(child, capabilities, caps, error);
	if (status != FSIMG_OK)
		return (status);
	status = prepare_context(child, caps, error);
	if (status != FSIMG_OK)
		return (status);
	return (read_selection(child, selection_length, error));
}

static fsimg_status_t
child_ready(worker_child_t *child, const fsimg_sandbox_caps_t *caps,
    fsimg_error_t *error)
{
	fsimg_frame_t acknowledgement;
	unsigned char ready[8];
	fsimg_status_t status;

	if (child->request.sandbox == FSIMG_SANDBOX_OFF)
		child->result.confinement = FSIMG_CONFINEMENT_DISABLED;
	else {
		status = fsimg_sandbox_install_caps(caps, error);
		if (status == FSIMG_EUNSUPPORTED) {
			child->result.confinement = FSIMG_CONFINEMENT_UNAVAILABLE;
			fsimg_error_clear(error);
		} else if (status != FSIMG_OK)
			return (status);
		else
			child->result.confinement = FSIMG_CONFINEMENT_ACTIVE;
	}
	fsimg_wire_put64(ready, child->result.confinement);
	status = fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_READY,
	    ready, sizeof(ready), error);
	if (status != FSIMG_OK)
		return (status);
	if (child->request.sandbox == FSIMG_SANDBOX_REQUIRED &&
	    child->result.confinement != FSIMG_CONFINEMENT_ACTIVE)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_CONTEXT, "required OS confinement is unavailable"));
	status = fsimg_frame_read(FSIMG_CHILD_JOB, &acknowledgement, error);
	if (status != FSIMG_OK)
		return (status);
	if (acknowledgement.type != FSIMG_FRAME_ACK || acknowledgement.length != 0)
		return (fsimg_error_set(error, FSIMG_EIO, 0,
		    FSIMG_OP_CONTEXT, "worker did not receive parsing acknowledgement"));
	child->processing = true;
	return (FSIMG_OK);
}

static void
encode_time(unsigned char *raw, const fsimg_timestamp_t *time)
{
	fsimg_wire_put64(raw + FSIMG_WIRE_TIME_SECONDS, (uint64_t)time->seconds);
	fsimg_wire_put64(raw + FSIMG_WIRE_TIME_PRECISION, time->precision_ns);
	fsimg_wire_put64(raw + FSIMG_WIRE_TIME_NANOSECONDS, time->nanoseconds);
	fsimg_wire_put64(raw + FSIMG_WIRE_TIME_BASIS, time->basis);
	fsimg_wire_put64(raw + FSIMG_WIRE_TIME_OFFSET, (uint64_t)(int64_t)time->offset_minutes);
}

static fsimg_status_t
send_info(worker_child_t *child, fsimg_error_t *error)
{
	fsimg_volume_info_t info;
	unsigned char raw[FSIMG_WIRE_INFO_BYTES];
	fsimg_status_t status;

	status = fsimg_reader_get_info(child->reader, &info, error);
	if (status != FSIMG_OK)
		return (status);
	bzero(raw, sizeof(raw));
	fsimg_wire_put64(raw + FSIMG_WIRE_INFO_VALID, info.valid);
	fsimg_wire_put64(raw + FSIMG_WIRE_INFO_CAPABILITIES, info.capabilities);
	fsimg_wire_put64(raw + FSIMG_WIRE_INFO_VOLUME, info.volume_bytes);
	fsimg_wire_put64(raw + FSIMG_WIRE_INFO_UNITS, info.allocation_units);
	fsimg_wire_put64(raw + FSIMG_WIRE_INFO_FREE, info.free_bytes);
	fsimg_wire_put64(raw + FSIMG_WIRE_INFO_SECTOR, info.sector_bytes);
	fsimg_wire_put64(raw + FSIMG_WIRE_INFO_CLUSTER, info.allocation_unit_bytes);
	fsimg_wire_put64(raw + FSIMG_WIRE_INFO_TIME_BASIS, info.time_basis);
	fsimg_wire_put64(raw + FSIMG_WIRE_INFO_SCOPE, info.scope);
	fsimg_wire_put64(raw + FSIMG_WIRE_INFO_CLEAN, info.clean ? 1 : 0);
	fsimg_wire_put64(raw + FSIMG_WIRE_INFO_IO_ERROR, info.io_error ? 1 : 0);
	bcopy(info.format, raw + FSIMG_WIRE_INFO_FORMAT, strlen(info.format));
	bcopy(info.profile, raw + FSIMG_WIRE_INFO_PROFILE, strlen(info.profile));
	bcopy(info.label, raw + FSIMG_WIRE_INFO_LABEL, strlen(info.label));
	return (fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_INFO, raw,
	    sizeof(raw), error));
}

static fsimg_status_t
send_plan(const fsimg_plan_info_t *info, fsimg_error_t *error)
{
	unsigned char raw[FSIMG_WIRE_PLAN_BYTES];

	bzero(raw, sizeof(raw));
	fsimg_wire_put64(raw + FSIMG_WIRE_PLAN_VOLUME, info->volume_bytes);
	fsimg_wire_put64(raw + FSIMG_WIRE_PLAN_PAYLOAD, info->payload_bytes);
	fsimg_wire_put64(raw + FSIMG_WIRE_PLAN_INITIALIZED, info->initialized_bytes);
	fsimg_wire_put64(raw + FSIMG_WIRE_PLAN_ALLOCATED, info->allocated_bytes);
	fsimg_wire_put64(raw + FSIMG_WIRE_PLAN_METADATA, info->metadata_bytes);
	fsimg_wire_put64(raw + FSIMG_WIRE_PLAN_FREE, info->free_bytes);
	fsimg_wire_put64(raw + FSIMG_WIRE_PLAN_ENTRIES, info->entries);
	fsimg_wire_put64(raw + FSIMG_WIRE_PLAN_LOSSES, info->metadata_losses);
	fsimg_wire_put64(raw + FSIMG_WIRE_PLAN_SCRATCH, info->scratch_bytes);
	fsimg_wire_put64(raw + FSIMG_WIRE_PLAN_SECTOR, info->sector_bytes);
	fsimg_wire_put64(raw + FSIMG_WIRE_PLAN_CLUSTER, info->allocation_unit_bytes);
	fsimg_wire_put64(raw + FSIMG_WIRE_PLAN_IO_FLAGS, info->required_io_flags);
	bcopy(info->format, raw + FSIMG_WIRE_PLAN_FORMAT, strlen(info->format));
	return (fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_PLAN, raw,
	    sizeof(raw), error));
}

static fsimg_status_t
send_entry(const fsimg_entry_t *entry, fsimg_error_t *error)
{
	unsigned char raw[FSIMG_WIRE_ENTRY_BYTES];
	size_t length, offset;
	uint32_t batch;
	fsimg_status_t status;

	length = strlen(entry->path);
	bzero(raw, sizeof(raw));
	fsimg_wire_put64(raw + FSIMG_WIRE_ENTRY_KIND, entry->kind);
	fsimg_wire_put64(raw + FSIMG_WIRE_ENTRY_LENGTH, entry->size);
	fsimg_wire_put64(raw + FSIMG_WIRE_ENTRY_NAME_LENGTH, length);
	fsimg_wire_put64(raw + FSIMG_WIRE_ENTRY_VALID, entry->metadata.valid);
	fsimg_wire_put64(raw + FSIMG_WIRE_ENTRY_REQUIRED, entry->metadata.required);
	fsimg_wire_put64(raw + FSIMG_WIRE_ENTRY_ATTRIBUTES, entry->metadata.attributes);
	encode_time(raw + FSIMG_WIRE_ENTRY_CREATED, &entry->metadata.created);
	encode_time(raw + FSIMG_WIRE_ENTRY_MODIFIED, &entry->metadata.modified);
	encode_time(raw + FSIMG_WIRE_ENTRY_ACCESSED, &entry->metadata.accessed);
	status = fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_ENTRY,
	    raw, sizeof(raw), error);
	if (status != FSIMG_OK)
		return (status);
	offset = 0;
	while (offset < length) {
		batch = length - offset > FSIMG_FRAME_BYTES ? FSIMG_FRAME_BYTES :
		    (uint32_t)(length - offset);
		status = fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_NAME,
		    entry->path + offset, batch, error);
		if (status != FSIMG_OK)
			return (status);
		offset += batch;
	}
	return (fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_ENTRY_END,
	    NULL, 0, error));
}


static fsimg_status_t
list_job(worker_child_t *child, fsimg_error_t *error)
{
	const fsimg_entry_t *entry;
	fsimg_selection_policy_t policy;
	fsimg_selection_relation_t relation;
	fsimg_status_t status;

	policy = (child->request.flags & FSIMG_JOB_RECURSIVE) != 0 ?
	    FSIMG_SELECTION_TREE : FSIMG_SELECTION_CHILDREN;
	status = fsimg_selection_new(child->reader, child->selection, policy,
	    &child->selector, error);
	if (status != FSIMG_OK)
		return (status);
	for (;;) {
		status = fsimg_selection_next(child->selector, &entry, &relation, error);
		if (status == FSIMG_DONE)
			return (FSIMG_OK);
		if (status != FSIMG_OK)
			return (status);
		if (relation == FSIMG_SELECTION_ANCESTOR ||
		    (relation == FSIMG_SELECTION_TARGET && entry->kind == FSIMG_ENTRY_DIRECTORY))
			continue;
		status = send_entry(entry, error);
		if (status != FSIMG_OK)
			return (status);
		child->result.entries++;
		if (entry->kind == FSIMG_ENTRY_FILE &&
		    !fsimg_add_u64(child->result.logical_bytes, entry->size,
		    &child->result.logical_bytes))
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_READER, "listing length total overflows"));
	}
}

static fsimg_status_t
cat_job(worker_child_t *child, fsimg_error_t *error)
{
	const fsimg_entry_t *entry;
	fsimg_selection_relation_t relation;
	unsigned char *buffer;
	uint64_t offset;
	size_t batch, transferred, done;
	ssize_t written;
	unsigned interruptions;
	fsimg_status_t status;

	status = fsimg_selection_new(child->reader, child->selection,
	    FSIMG_SELECTION_FILE, &child->selector, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_selection_next(child->selector, &entry, &relation, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_session_charge(&child->reader->session, 0, entry->size, 1, error);
	if (status != FSIMG_OK)
		return (status);
	status = send_entry(entry, error);
	if (status != FSIMG_OK)
		return (status);
	child->result.entries = 1;
	child->result.logical_bytes = entry->size;
	buffer = fsimg_alloc(child->ctx, 65536, error);
	if (buffer == NULL)
		return (error->status);
	offset = 0;
	while (offset < entry->size) {
		batch = entry->size - offset > 65536 ? 65536 : (size_t)(entry->size - offset);
		status = fsimg_reader_read_at(child->reader, offset, buffer, batch,
		    &transferred, error);
		if (status != FSIMG_OK)
			break;
		if (transferred != batch) {
			status = fsimg_error_set(error, FSIMG_ECORRUPT, 0,
			    FSIMG_OP_READER, "selected file ended prematurely");
			break;
		}
		done = 0;
		interruptions = 0;
		while (done < batch) {
			status = fsimg_session_charge(&child->reader->session, 0, 0, 1, error);
			if (status != FSIMG_OK)
				break;
			written = write(FSIMG_CHILD_DATA, buffer + done, batch - done);
			if (written == -1 && errno == EINTR && interruptions++ < 1024)
				continue;
			if (written <= 0) {
				status = fsimg_error_set(error, FSIMG_EIO, written == -1 ? errno : 0,
				    FSIMG_OP_IO, "raw data channel write failed");
				break;
			}
			done += (size_t)written;
			child->result.raw_bytes += (uint64_t)written;
			interruptions = 0;
		}
		if (status != FSIMG_OK)
			break;
		offset += batch;
	}
	fsimg_dealloc(buffer);
	return (status);
}

static fsimg_status_t
verify_job(worker_child_t *child, fsimg_error_t *error)
{
	fsimg_verify_report_t report;
	unsigned char raw[FSIMG_WIRE_VERIFY_BYTES];
	fsimg_status_t status;

	status = fsimg_verify(child->reader,
	    (child->request.flags & FSIMG_JOB_PAYLOAD) != 0 ? FSIMG_VERIFY_PAYLOAD : 0,
	    &report, error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_wire_put64(raw + FSIMG_WIRE_VERIFY_SCOPE, report.scope);
	fsimg_wire_put64(raw + FSIMG_WIRE_VERIFY_COMPLETE, report.complete ? 1 : 0);
	fsimg_wire_put64(raw + FSIMG_WIRE_VERIFY_FILES, report.files);
	fsimg_wire_put64(raw + FSIMG_WIRE_VERIFY_DIRECTORIES, report.directories);
	fsimg_wire_put64(raw + FSIMG_WIRE_VERIFY_LOGICAL, report.logical_bytes);
	fsimg_wire_put64(raw + FSIMG_WIRE_VERIFY_PAYLOAD, report.payload_bytes_read);
	fsimg_wire_put64(raw + FSIMG_WIRE_VERIFY_ALLOCATED, report.allocated_clusters);
	fsimg_wire_put64(raw + FSIMG_WIRE_VERIFY_FREE, report.free_clusters);
	fsimg_wire_put64(raw + FSIMG_WIRE_VERIFY_BAD_CLUSTERS, report.bad_clusters);
	child->result.entries = report.files + report.directories;
	child->result.logical_bytes = report.logical_bytes;
	return (fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_VERIFY,
	    raw, sizeof(raw), error));
}

static fsimg_status_t
extract_job(worker_child_t *child, fsimg_error_t *error)
{
	fsimg_extract_options_t options;
	fsimg_extract_result_t result;
	fsimg_status_t status;
	int fd, closed, saved;

	fsimg_extract_options_init(&options);
	options.selection = child->selection;
	fd = child->output_root;
	status = fsimg_extract_into(child->reader, fd, &options, &result, error);
	child->result.entries = result.entries;
	child->result.logical_bytes = result.logical_bytes;
	if (status == FSIMG_OK && fsync(fd) == -1)
		status = fsimg_error_set(error, FSIMG_EIO, errno,
		    FSIMG_OP_EXTRACT, "worker extraction root flush failed");
	child->output_root = -1;
	closed = close(fd);
	saved = errno;
	fsimg_fd_release(child->ctx, -1);
	if (status == FSIMG_OK && closed == -1)
		status = fsimg_error_set(error, FSIMG_EIO, saved,
		    FSIMG_OP_EXTRACT, "worker extraction root close failed");
	if (status != FSIMG_OK)
		error->output_state = FSIMG_OUTPUT_INCOMPLETE;
	return (status);
}

static fsimg_status_t
read_job(worker_child_t *child, fsimg_error_t *error)
{
	fsimg_read_options_t options;
	uint64_t length, input_length;
	fsimg_status_t status;

	status = fsimg_io_get_size(child->input, &input_length, error);
	if (status != FSIMG_OK)
		return (status);
	length = child->request.length;
	if (child->request.offset > input_length)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_IO, "input slice starts past EOF"));
	if (length == 0)
		length = input_length - child->request.offset;
	status = fsimg_io_slice(child->input, child->request.offset, length,
	    &child->volume, error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_read_options_init(&options);
	options.format = child->request.format[0] == '\0' ? NULL : child->request.format;
	options.fat_codepage = child->request.codepage;
	status = fsimg_reader_open(child->ctx, child->volume, &options, &child->reader, error);
	if (status != FSIMG_OK)
		return (status);
	status = send_info(child, error);
	if (status != FSIMG_OK)
		return (status);
	switch (child->request.kind) {
	case FSIMG_JOB_INFO:
		return (FSIMG_OK);
	case FSIMG_JOB_LIST:
		return (list_job(child, error));
	case FSIMG_JOB_CAT:
		return (cat_job(child, error));
	case FSIMG_JOB_VERIFY:
		return (verify_job(child, error));
	case FSIMG_JOB_EXTRACT:
		return (extract_job(child, error));
	default:
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_CONTEXT, "invalid worker read operation"));
	}
}

static fsimg_status_t
create_job(worker_child_t *child, fsimg_error_t *error)
{
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_io_t *output;
	fsimg_status_t status;
	int fd, saved;

	status = fsimg_writer_new(child->ctx, child->request.format,
	    &child->request.write, &child->writer, error);
	if (status != FSIMG_OK)
		return (status);
	if (child->request.kind == FSIMG_JOB_CREATE) {
		if (fsimg_host_file(FSIMG_CHILD_OUTPUT, &child->writer->output_root) == -1)
			return (fsimg_error_set(error, FSIMG_EIO, errno,
			    FSIMG_OP_SOURCE, "prepared output identity query failed"));
		child->writer->output_root_known = true;
	}
	status = fsimg_fd_reserve(child->ctx, error);
	if (status != FSIMG_OK)
		return (status);
	fd = child->raw_caps[CHILD_SOURCE_CAP];
	child->raw_caps[CHILD_SOURCE_CAP] = -1;
	status = fsimg_writer_adopt_tree(child->writer, fd, "/", error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_writer_plan(child->writer, &plan, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_plan_get_info(plan, &info, error);
	if (status != FSIMG_OK)
		return (status);
	child->result.image_bytes = info.volume_bytes;
	child->result.entries = info.entries;
	child->result.logical_bytes = info.payload_bytes;
	status = send_plan(&info, error);
	if (status != FSIMG_OK || child->request.kind == FSIMG_JOB_PLAN)
		return (status);
	status = fsimg_fd_reserve(child->ctx, error);
	if (status != FSIMG_OK)
		return (status);
	fd = openat(FSIMG_CHILD_OUTPUT, "image", O_RDWR | O_CREAT | O_EXCL |
	    O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd == -1) {
		saved = errno;
		fsimg_fd_release(child->ctx, -1);
		return (fsimg_error_set(error, FSIMG_EIO, saved,
		    FSIMG_OP_IO, "worker staged image creation failed"));
	}
	status = fsimg_io_adopt_empty_output(child->ctx, fd, &output, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_writer_emit(child->writer, output, error);
	if (status != FSIMG_OK) {
		fsimg_io_free(output);
		return (status);
	}
	return (fsimg_io_close_output(output, error));
}

static void
child_cleanup(worker_child_t *child)
{
	fsimg_selection_free(child->selector);
	fsimg_reader_free(child->reader);
	fsimg_io_free(child->volume);
	fsimg_io_free(child->input);
	fsimg_writer_free(child->writer);
	fsimg_dealloc(child->selection);
	if (child->output_root != -1)
		fsimg_fd_release(child->ctx, child->output_root);
	fsimg_ctx_free(child->ctx);
	for (unsigned i = 0; i < CHILD_CAP_COUNT; i++) {
		if (child->raw_caps[i] != -1)
			close(child->raw_caps[i]);
	}
}

int
fsimg_worker_main(void)
{
	worker_child_t child;
	fsimg_sandbox_caps_t caps;
	fsimg_error_t error;
	fsimg_stats_t stats;
	unsigned char raw[FSIMG_WIRE_ERROR_BYTES];
	size_t length;
	uint32_t depth;
	fsimg_status_t status;

	bzero(&child, sizeof(child));
	child.result.confinement = FSIMG_CONFINEMENT_UNKNOWN;
	child.output_root = -1;
	for (unsigned i = 0; i < CHILD_CAP_COUNT; i++)
		child.raw_caps[i] = -1;
	status = child_setup(&child, &caps, &error);
	if (status == FSIMG_OK)
		status = child_ready(&child, &caps, &error);
	if (status == FSIMG_OK)
		status = fsimg_path_validate(child.ctx, child.selection, true,
		    &length, &depth, &error);
	if (status == FSIMG_OK) {
		if (child.request.kind >= FSIMG_JOB_CREATE)
			status = create_job(&child, &error);
		else
			status = read_job(&child, &error);
	}
	if (child.ctx != NULL && fsimg_ctx_get_stats(child.ctx, &stats, NULL) == FSIMG_OK)
		child.result.metadata_losses = stats.metadata_losses;
	child_cleanup(&child);
	if (status != FSIMG_OK) {
		fsimg_wire_error_encode(raw, &error);
		fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_ERROR, raw,
		    FSIMG_WIRE_ERROR_BYTES, NULL);
		return (1);
	}
	fsimg_wire_put64(raw + FSIMG_WIRE_DONE_ENTRIES, child.result.entries);
	fsimg_wire_put64(raw + FSIMG_WIRE_DONE_LOGICAL, child.result.logical_bytes);
	fsimg_wire_put64(raw + FSIMG_WIRE_DONE_LOSSES, child.result.metadata_losses);
	fsimg_wire_put64(raw + FSIMG_WIRE_DONE_RAW, child.result.raw_bytes);
	fsimg_wire_put64(raw + FSIMG_WIRE_DONE_IMAGE, child.result.image_bytes);
	status = fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_DONE, raw, FSIMG_WIRE_DONE_BYTES, NULL);
	return (status == FSIMG_OK ? 0 : 1);
}
