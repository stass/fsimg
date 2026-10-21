/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/resource.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
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
	CHILD_RAW_CAP,
	CHILD_SPOOL_CAP,
	CHILD_CAP_COUNT
} worker_capability_slot_t;

typedef struct worker_child {
	fsimg_worker_request_t request;
	fsimg_worker_result_t result;
	fsimg_ctx_t	*ctx;
	fsimg_io_t	*input;
	fsimg_image_t	*image;		/* The input's layers */
	fsimg_io_t	*volume;	/* The selected slice of the image */
	fsimg_reader_t	*reader;
	fsimg_selection_t *selector;
	fsimg_writer_t	*writer;
	/* The job's NUL-terminated paths, back to back: the member of a read
	 * job, or one image path per root of a create job.
	 */
	char		*paths;
	uint64_t	 paths_length;
	char		*selection;
	uint64_t	 last_progress;
	unsigned	 losses_sent;
	/* Raw filesystem capabilities remain here until an owner adopts them.
	 * Roots FSIMG_CHILD_ROOTS + roots_adopted onwards are still raw.
	 */
	int		 raw_caps[CHILD_CAP_COUNT];
	uint32_t	 roots_adopted;
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
static fsimg_status_t read_paths(worker_child_t *, uint64_t, fsimg_error_t *);
static fsimg_status_t validate_paths(worker_child_t *, fsimg_error_t *);
static fsimg_status_t add_synthesized_parents(worker_child_t *, const char *,
    fsimg_error_t *);
static fsimg_status_t adopt_roots(worker_child_t *, fsimg_error_t *);
static fsimg_status_t child_setup(worker_child_t *, fsimg_sandbox_caps_t *,
    fsimg_error_t *);
static fsimg_status_t child_ready(worker_child_t *, const fsimg_sandbox_caps_t *,
    fsimg_error_t *);
static fsimg_status_t send_info(const fsimg_volume_info_t *, fsimg_error_t *);
static fsimg_status_t send_image(const fsimg_image_info_t *,
    const fsimg_layer_report_t *, const fsimg_error_t *, fsimg_error_t *);
static fsimg_status_t send_partitions(worker_child_t *, fsimg_error_t *);
static fsimg_status_t select_volume(worker_child_t *, fsimg_error_t *);
static fsimg_status_t describe_image(worker_child_t *, fsimg_image_info_t *,
    fsimg_layer_report_t *, fsimg_error_t *);
static fsimg_status_t report_unselected(worker_child_t *, fsimg_error_t *);
static fsimg_status_t send_plan(const fsimg_plan_info_t *, fsimg_error_t *);
static fsimg_status_t send_entry(const fsimg_entry_t *, fsimg_error_t *);
static fsimg_status_t read_job(worker_child_t *, fsimg_error_t *);
static int take_raw(worker_child_t *);
static fsimg_status_t create_job(worker_child_t *, fsimg_error_t *);
static fsimg_status_t list_job(worker_child_t *, fsimg_error_t *);
static fsimg_status_t cat_job(worker_child_t *, fsimg_error_t *);
static fsimg_status_t verify_job(worker_child_t *, fsimg_error_t *);
static fsimg_status_t extract_job(worker_child_t *, fsimg_error_t *);
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

/* Each job kind has one shape of filesystem authority: reads have an
 * input, creation has roots, and creation and extraction write into their
 * output directory, which is staging for one and the destination for the
 * other. A creation to a stream writes the data channel instead of
 * staging; it, and a creation with a container, also holds the raw
 * intermediate. Scratch is optional for every kind.
 */
static fsimg_status_t
prepare_capabilities(worker_child_t *child, unsigned capabilities,
    fsimg_sandbox_caps_t *caps, fsimg_error_t *error)
{
	fsimg_job_kind_t kind = child->request.kind;
	unsigned expected, optional;
	bool output, stream, reading;

	reading = !fsimg_job_has_roots(kind);
	stream = (child->request.flags & FSIMG_JOB_STDOUT) != 0;
	expected = fsimg_job_has_roots(kind) ? FSIMG_JOB_CAP_ROOTS : FSIMG_JOB_CAP_INPUT;
	/* Scratch is optional for every job, and a spool for a read job. */
	optional = FSIMG_JOB_CAP_SCRATCH | (reading ? FSIMG_JOB_CAP_SPOOL : 0);
	if ((kind == FSIMG_JOB_CREATE && !stream) || kind == FSIMG_JOB_EXTRACT)
		expected |= FSIMG_JOB_CAP_OUTPUT;
	if (kind == FSIMG_JOB_CREATE && (stream ||
	    child->request.stack.slots[FSIMG_LAYER_CONTAINER].state ==
	    FSIMG_SLOT_NAMED))
		expected |= FSIMG_JOB_CAP_RAW;
	if ((capabilities & ~optional) != expected)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_CONTEXT, "worker job has excess or missing filesystem authority"));
	output = (capabilities & FSIMG_JOB_CAP_OUTPUT) != 0;
	*caps = (fsimg_sandbox_caps_t){
		.input = (capabilities & FSIMG_JOB_CAP_INPUT) != 0 ? FSIMG_CHILD_INPUT : -1,
		.staging = output && kind == FSIMG_JOB_CREATE ? FSIMG_CHILD_OUTPUT : -1,
		.destination = output && kind == FSIMG_JOB_EXTRACT ? FSIMG_CHILD_OUTPUT : -1,
		.scratch = (capabilities & FSIMG_JOB_CAP_SCRATCH) != 0 ?
		    FSIMG_CHILD_SCRATCH : -1,
		.roots = child->request.root_count != 0 ? FSIMG_CHILD_ROOTS : -1,
		.root_count = child->request.root_count,
		.raw = (capabilities & FSIMG_JOB_CAP_RAW) != 0 ? FSIMG_CHILD_RAW : -1,
		.spool = (capabilities & FSIMG_JOB_CAP_SPOOL) != 0 ?
		    FSIMG_CHILD_SPOOL : -1,
		.input_pipe = (child->request.flags & FSIMG_JOB_STDIN) != 0,
		.job = FSIMG_CHILD_JOB, .control = FSIMG_CHILD_CONTROL,
		.data = kind == FSIMG_JOB_CAT || stream ? FSIMG_CHILD_DATA : -1,
		.cancel = FSIMG_CHILD_CANCEL
	};
	child->raw_caps[CHILD_INPUT_CAP] = caps->input;
	child->raw_caps[CHILD_OUTPUT_CAP] = output ? FSIMG_CHILD_OUTPUT : -1;
	child->raw_caps[CHILD_SCRATCH_CAP] = caps->scratch;
	child->raw_caps[CHILD_RAW_CAP] = caps->raw;
	child->raw_caps[CHILD_SPOOL_CAP] = caps->spool;
	{
		static const int numbers[CHILD_CAP_COUNT] = {
			[CHILD_INPUT_CAP] = FSIMG_CHILD_INPUT,
			[CHILD_OUTPUT_CAP] = FSIMG_CHILD_OUTPUT,
			[CHILD_SCRATCH_CAP] = FSIMG_CHILD_SCRATCH,
			[CHILD_RAW_CAP] = FSIMG_CHILD_RAW,
			[CHILD_SPOOL_CAP] = FSIMG_CHILD_SPOOL
		};

		for (unsigned i = 0; i < CHILD_CAP_COUNT; i++) {
			if (child->raw_caps[i] == -1)
				close(numbers[i]);
		}
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
	/* A stream stays a bare descriptor until the image reads it. */
	if (caps->input != -1 && !caps->input_pipe) {
		status = fsimg_fd_reserve(child->ctx, error);
		if (status != FSIMG_OK)
			return (status);
		fd = child->raw_caps[CHILD_INPUT_CAP];
		child->raw_caps[CHILD_INPUT_CAP] = -1;
		status = fsimg_io_adopt_fd(child->ctx, fd, FSIMG_IO_READ, &child->input, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (child->raw_caps[CHILD_OUTPUT_CAP] != -1) {
		status = fsimg_fd_reserve(child->ctx, error);
		if (status != FSIMG_OK)
			return (status);
		child->output_root = child->raw_caps[CHILD_OUTPUT_CAP];
		child->raw_caps[CHILD_OUTPUT_CAP] = -1;
	}
	return (FSIMG_OK);
}

/* Receive the job's path sequence. Its length was checked against the path
 * limit and the path count when the job record was decoded; validate_paths
 * checks its framing once every byte has arrived.
 */
static fsimg_status_t
read_paths(worker_child_t *child, uint64_t paths_length, fsimg_error_t *error)
{
	fsimg_frame_t frame;
	uint64_t received;
	fsimg_status_t status;
	size_t allocation;

	if (!fsimg_to_size(paths_length, &allocation))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_CONTEXT, "worker path sequence exceeds host range"));
	child->paths = fsimg_alloc(child->ctx, allocation, error);
	if (child->paths == NULL)
		return (error->status);
	received = 0;
	while (received < paths_length) {
		status = fsimg_frame_read(FSIMG_CHILD_JOB, &frame, error);
		if (status != FSIMG_OK)
			return (status);
		if (frame.type != FSIMG_FRAME_SELECTION || frame.length == 0 ||
		    frame.length > paths_length - received)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_CONTEXT, "invalid worker selection framing"));
		bcopy(frame.payload, child->paths + received, frame.length);
		received += frame.length;
	}
	child->paths_length = paths_length;
	child->selection = child->paths;
	child->request.selection = child->selection;
	return (FSIMG_OK);
}

/* The sequence holds exactly the job's paths, one for a read job and one
 * per root for a create job, each terminated and each a valid absolute
 * image path within the context's limits.
 */
static fsimg_status_t
validate_paths(worker_child_t *child, fsimg_error_t *error)
{
	const char *path, *end;
	size_t length, validated;
	uint32_t depth, count, expected;
	fsimg_status_t status;

	expected = fsimg_job_path_count(&child->request);
	end = child->paths + child->paths_length;
	if (child->paths_length == 0 || end[-1] != '\0')
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_CONTEXT, "invalid worker selection framing"));
	count = 0;
	for (path = child->paths; path < end; path += length + 1) {
		length = strlen(path);
		if (count == expected)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_CONTEXT, "invalid worker selection framing"));
		status = fsimg_path_validate(child->ctx, path, true, &validated,
		    &depth, error);
		if (status != FSIMG_OK)
			return (status);
		count++;
	}
	if (count != expected)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_CONTEXT, "invalid worker selection framing"));
	return (FSIMG_OK);
}

static fsimg_status_t
child_setup(worker_child_t *child, fsimg_sandbox_caps_t *caps, fsimg_error_t *error)
{
	const struct rlimit core_limit = { .rlim_cur = 0, .rlim_max = 0 };
	fsimg_frame_t frame;
	uint64_t paths_length;
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
	    &paths_length, &capabilities, error);
	if (status != FSIMG_OK)
		return (status);

	status = prepare_capabilities(child, capabilities, caps, error);
	if (status != FSIMG_OK)
		return (status);
	status = prepare_context(child, caps, error);
	if (status != FSIMG_OK)
		return (status);
	return (read_paths(child, paths_length, error));
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

static fsimg_status_t
send_info(const fsimg_volume_info_t *info, fsimg_error_t *error)
{
	unsigned char raw[FSIMG_WIRE_INFO_BYTES];

	fsimg_wire_info_encode(raw, info);
	return (fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_INFO, raw,
	    sizeof(raw), error));
}

/*
 * The IMAGE record, from the image's own report: its stack is the
 * container and scheme found or forced and the selected volume's
 * filesystem, none when no volume was selected. A range or disk selection
 * bypasses the scheme, so its image carries no table.
 */
static fsimg_status_t
send_image(const fsimg_image_info_t *info, const fsimg_layer_report_t *report,
    const fsimg_error_t *unselected, fsimg_error_t *error)
{
	unsigned char raw[FSIMG_WIRE_IMAGE_BYTES];
	uint64_t flags;
	int count;

	bzero(raw, sizeof(raw));
	bcopy(info->stack, raw + FSIMG_WIRE_IMAGE_STACK,
	    strnlen(info->stack, FSIMG_WIRE_IMAGE_STACK_BYTES - 1));
	bcopy(info->container, raw + FSIMG_WIRE_IMAGE_CONTAINER,
	    strnlen(info->container, sizeof(info->container) - 1));
	bcopy(info->codecs, raw + FSIMG_WIRE_IMAGE_CODECS,
	    strnlen(info->codecs, FSIMG_WIRE_IMAGE_CODECS_BYTES - 1));
	bcopy(info->container_guid, raw + FSIMG_WIRE_IMAGE_CONTAINER_GUID,
	    sizeof(info->container_guid));
	fsimg_wire_put64(raw + FSIMG_WIRE_IMAGE_CHUNKS, info->chunk_count);
	fsimg_wire_put64(raw + FSIMG_WIRE_IMAGE_FILE, info->file_bytes);
	bcopy(info->compression, raw + FSIMG_WIRE_IMAGE_COMPRESSION,
	    strnlen(info->compression, sizeof(info->compression) - 1));
	flags = 0;
	if (unselected != NULL) {
		/* A reason cut here or by the library ends in "...". */
		flags |= FSIMG_WIRE_IMAGE_UNSELECTED;
		count = snprintf((char *)raw + FSIMG_WIRE_IMAGE_REASON,
		    FSIMG_WIRE_IMAGE_REASON_BYTES, "%s%s", unselected->message,
		    unselected->message_truncated ? "..." : "");
		if (count < 0 || (size_t)count >= FSIMG_WIRE_IMAGE_REASON_BYTES)
			bcopy("...", raw + FSIMG_WIRE_IMAGE_REASON +
			    FSIMG_WIRE_IMAGE_REASON_BYTES - 4, 3);
	}
	if (report != NULL) {
		flags |= FSIMG_WIRE_IMAGE_VERIFIED;
		fsimg_wire_put64(raw + FSIMG_WIRE_IMAGE_CHECKED, report->partitions);
	}
	fsimg_wire_put64(raw + FSIMG_WIRE_IMAGE_FLAGS, flags);
	fsimg_wire_put64(raw + FSIMG_WIRE_IMAGE_DISK, info->disk_bytes);
	fsimg_wire_put64(raw + FSIMG_WIRE_IMAGE_SECTOR, info->sector_bytes);
	fsimg_wire_put64(raw + FSIMG_WIRE_IMAGE_SLOTS, info->partition_count);
	fsimg_wire_put64(raw + FSIMG_WIRE_IMAGE_SELECTED, info->selected);
	fsimg_wire_put64(raw + FSIMG_WIRE_IMAGE_DISK_ID, info->mbr_disk_id);
	bcopy(info->disk_guid, raw + FSIMG_WIRE_IMAGE_DISK_GUID,
	    sizeof(info->disk_guid));
	bcopy(info->scheme, raw + FSIMG_WIRE_IMAGE_SCHEME,
	    strnlen(info->scheme, sizeof(info->scheme) - 1));
	return (fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_IMAGE, raw,
	    sizeof(raw), error));
}

/* One PARTITION record per used slot, in slot order. */
static fsimg_status_t
send_partitions(worker_child_t *child, fsimg_error_t *error)
{
	fsimg_partition_info_t partition;
	unsigned char raw[FSIMG_WIRE_PARTITION_BYTES];
	fsimg_status_t status;

	while ((status = fsimg_image_next_partition(child->image, &partition,
	    error)) == FSIMG_OK) {
		if (!partition.used)
			continue;
		fsimg_wire_partition_encode(raw, &partition);
		status = fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_PARTITION,
		    raw, sizeof(raw), error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (status == FSIMG_DONE ? FSIMG_OK : status);
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
	fsimg_wire_put64(raw + FSIMG_WIRE_PLAN_OUTPUT, info->output_bytes);
	fsimg_wire_put64(raw + FSIMG_WIRE_PLAN_PADDING, info->padding_bytes);
	bcopy(info->codec_version, raw + FSIMG_WIRE_PLAN_CODEC,
	    strnlen(info->codec_version, FSIMG_WIRE_PLAN_CODEC_BYTES - 1));
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
	fsimg_wire_entry_encode(raw, entry, length);
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
	if ((child->request.flags & FSIMG_JOB_SKIP_UNSUPPORTED) != 0)
		options.flags |= FSIMG_EXTRACT_SKIP_UNSUPPORTED;
	fd = child->output_root;
	status = fsimg_extract_into(child->reader, fd, &options, &result, error);
	child->result.entries = result.entries;
	child->result.skipped_entries = result.skipped_entries;
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

/* Open the image over the input with the forced stack and select the
 * request's volume into child->volume. A range or disk selection never
 * consults a partition table, so its image forces the scheme slot empty:
 * a damaged table cannot block it, and describing or verifying the image
 * reads only its container.
 */
static fsimg_status_t
select_volume(worker_child_t *child, fsimg_error_t *error)
{
	const fsimg_worker_request_t *request = &child->request;
	fsimg_image_options_t options;
	fsimg_selector_t selector;
	fsimg_status_t status;

	fsimg_image_options_init(&options);
	options.stack = request->stack;
	options.layers.gpt_sector_bytes = request->layers.gpt_sector_bytes;
	if ((request->flags & FSIMG_JOB_GZIP) != 0)
		options.flags |= FSIMG_IMAGE_GZIP;
	if (request->selector == FSIMG_SELECT_RANGE ||
	    request->selector == FSIMG_SELECT_DISK) {
		bzero(&options.stack.slots[FSIMG_LAYER_SCHEME],
		    sizeof(options.stack.slots[FSIMG_LAYER_SCHEME]));
		options.stack.slots[FSIMG_LAYER_SCHEME].state = FSIMG_SLOT_EMPTY;
		options.layers.gpt_sector_bytes = 0;
	}
	/* The image adopts the spool on every outcome. */
	options.spool_fd = child->raw_caps[CHILD_SPOOL_CAP];
	child->raw_caps[CHILD_SPOOL_CAP] = -1;
	if (child->input != NULL)
		status = fsimg_image_open(child->ctx, child->input, &options,
		    &child->image, error);
	else
		status = fsimg_image_open_stream(child->ctx,
		    child->raw_caps[CHILD_INPUT_CAP], &options, &child->image,
		    error);
	if (status != FSIMG_OK)
		return (status);
	bzero(&selector, sizeof(selector));
	selector.kind = request->selector;
	selector.index = request->partition_index;
	selector.text = request->partition_text;
	selector.offset = request->offset;
	selector.length = request->length;
	return (fsimg_image_select(child->image, &selector, &child->volume, error));
}

/*
 * The layers around the selected volume, which W also verifies
 * (report->complete): the container, then the partition table unless the
 * selection was a range or the whole disk, whose image forces the scheme
 * empty. W -d adds the container's read-through checks.
 */
static fsimg_status_t
describe_image(worker_child_t *child, fsimg_image_info_t *info,
    fsimg_layer_report_t *report, fsimg_error_t *error)
{
	unsigned flags;
	fsimg_status_t status;

	bzero(info, sizeof(*info));
	bzero(report, sizeof(*report));
	if (child->request.kind == FSIMG_JOB_VERIFY) {
		flags = (child->request.flags & FSIMG_JOB_PAYLOAD) != 0 ?
		    FSIMG_VERIFY_PAYLOAD : 0;
		status = fsimg_image_verify(child->image, flags, report, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (fsimg_image_get_info(child->image, info, error));
}

/*
 * i on a partitioned disk where automatic selection found no single
 * filesystem: report the table without a volume, so the user can choose.
 * A disk without a table has nothing to show and fails with the reason.
 */
static fsimg_status_t
report_unselected(worker_child_t *child, fsimg_error_t *error)
{
	fsimg_image_info_t info;
	fsimg_error_t reason;
	fsimg_status_t status;

	reason = *error;
	if (child->image == NULL)
		return (reason.status);
	status = fsimg_image_get_info(child->image, &info, error);
	if (status != FSIMG_OK)
		return (status);
	if (info.scheme[0] == '\0') {
		*error = reason;
		return (reason.status);
	}
	status = send_image(&info, NULL, &reason, error);
	if (status != FSIMG_OK)
		return (status);
	return (send_partitions(child, error));
}

static fsimg_status_t
read_job(worker_child_t *child, fsimg_error_t *error)
{
	fsimg_read_options_t options;
	fsimg_image_info_t image;
	fsimg_layer_report_t report;
	fsimg_volume_info_t volume;
	const char *format;
	fsimg_status_t status;

	status = select_volume(child, error);
	if (status == FSIMG_EFORMAT && child->request.kind == FSIMG_JOB_INFO &&
	    child->request.selector == FSIMG_SELECT_AUTOMATIC)
		return (report_unselected(child, error));
	if (status == FSIMG_OK)
		status = describe_image(child, &image, &report, error);
	if (status != FSIMG_OK)
		return (status);
	format = fsimg_job_format(&child->request);
	fsimg_read_options_init(&options);
	options.format = format[0] == '\0' ? NULL : format;
	options.fat_codepage = child->request.codepage;
	status = fsimg_reader_open(child->ctx, child->volume, &options, &child->reader, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_reader_get_info(child->reader, &volume, error);
	if (status != FSIMG_OK)
		return (status);
	status = send_image(&image, report.complete ? &report : NULL, NULL, error);
	if (status == FSIMG_OK && child->request.kind == FSIMG_JOB_INFO &&
	    image.scheme[0] != '\0')
		status = send_partitions(child, error);
	if (status == FSIMG_OK)
		status = send_info(&volume, error);
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

/*
 * Give every root to the writer at its image path. A root at "/" adds its
 * contents only; any other root also adds its own entry, and its ancestors
 * that no root names are added as synthesized directories with no metadata
 * field, so the format's defaults apply to them. The writer merges a
 * directory that several roots or synthesized parents supply and fails on
 * any other repeated path.
 */
static fsimg_status_t
adopt_roots(worker_child_t *child, fsimg_error_t *error)
{
	const char *path;
	fsimg_status_t status;

	path = child->paths;
	for (uint32_t i = 0; i < child->request.root_count; i++) {
		status = fsimg_fd_reserve(child->ctx, error);
		if (status != FSIMG_OK)
			return (status);
		/* Adoption consumes the descriptor on every outcome. */
		child->roots_adopted++;
		status = fsimg_writer_adopt_tree(child->writer, FSIMG_CHILD_ROOTS + (int)i,
		    path, error);
		if (status != FSIMG_OK)
			return (status);
		status = add_synthesized_parents(child, path, error);
		if (status != FSIMG_OK)
			return (status);
		path += strlen(path) + 1;
	}
	return (FSIMG_OK);
}

/* Add each proper ancestor of path below the image root, "/a" and "/a/b"
 * for "/a/b/c", as a directory entry without metadata.
 */
static fsimg_status_t
add_synthesized_parents(worker_child_t *child, const char *path,
    fsimg_error_t *error)
{
	fsimg_entry_t entry;
	char *ancestor;
	const char *separator;
	size_t length;
	fsimg_status_t status;

	length = strlen(path);
	ancestor = fsimg_alloc(child->ctx, length + 1, error);
	if (ancestor == NULL)
		return (error->status);
	status = FSIMG_OK;
	for (separator = strchr(path + 1, '/'); separator != NULL;
	    separator = strchr(separator + 1, '/')) {
		bcopy(path, ancestor, (size_t)(separator - path));
		ancestor[separator - path] = '\0';
		bzero(&entry, sizeof(entry));
		entry.path = ancestor;
		entry.kind = FSIMG_ENTRY_DIRECTORY;
		status = fsimg_writer_add(child->writer, &entry, NULL, error);
		if (status != FSIMG_OK)
			break;
	}
	fsimg_dealloc(ancestor);
	return (status);
}

/* Hand the raw intermediate, or -1 when the job has none, to its user. */
static int
take_raw(worker_child_t *child)
{
	int fd = child->raw_caps[CHILD_RAW_CAP];

	child->raw_caps[CHILD_RAW_CAP] = -1;
	return (fd);
}

/*
 * Plan the image and report it: for a partitioned disk or a container the
 * IMAGE record, for a partitioned disk the one PARTITION record, the
 * volume's place on it, then the PLAN; for a bare volume the PLAN alone. A
 * create job then writes the image: into the staged file, or under
 * FSIMG_JOB_STDOUT to the data channel, through the raw intermediate when
 * the coordinator handed one over.
 */
static fsimg_status_t
create_job(worker_child_t *child, fsimg_error_t *error)
{
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	unsigned char raw[FSIMG_WIRE_PARTITION_BYTES];
	char stack[FSIMG_WIRE_IMAGE_STACK_BYTES];
	fsimg_sink_t sink;
	fsimg_io_t *output;
	fsimg_status_t status;
	int fd, saved;

	if (fsimg_stack_format(&child->request.stack, stack, sizeof(stack)) >=
	    sizeof(stack))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_CONTEXT,
		    "invalid worker stack"));
	child->request.write.layers = &child->request.layers;
	status = fsimg_writer_new(child->ctx, stack, &child->request.write,
	    &child->writer, error);
	if (status != FSIMG_OK)
		return (status);
	if (child->output_root != -1) {
		if (fsimg_host_file(child->output_root, &child->writer->output_root) == -1)
			return (fsimg_error_set(error, FSIMG_EIO, errno,
			    FSIMG_OP_SOURCE, "prepared output identity query failed"));
		child->writer->output_root_known = true;
	}
	status = adopt_roots(child, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_writer_plan(child->writer, &plan, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_plan_get_info(plan, &info, error);
	if (status != FSIMG_OK)
		return (status);
	child->result.entries = info.entries;
	child->result.logical_bytes = info.payload_bytes;
	if (info.image.scheme[0] != '\0' || info.image.container[0] != '\0') {
		status = send_image(&info.image, NULL, NULL, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (info.image.scheme[0] != '\0') {
		fsimg_wire_partition_encode(raw, &info.partition);
		status = fsimg_frame_write(FSIMG_CHILD_CONTROL,
		    FSIMG_FRAME_PARTITION, raw, sizeof(raw), error);
		if (status != FSIMG_OK)
			return (status);
	}
	status = send_plan(&info, error);
	if (status != FSIMG_OK || child->request.kind == FSIMG_JOB_PLAN)
		return (status);
	/* The raw intermediate stays in raw_caps, which the worker's
	 * cleanup closes, until fsimg_writer_output takes it over.
	 */
	if ((child->request.flags & FSIMG_JOB_STDOUT) != 0) {
		/* The coordinator counts what reaches stdout; DONE reports
		 * no staged image.
		 */
		fsimg_sink_init(&sink, FSIMG_SINK_STREAM);
		sink.stream_fd = FSIMG_CHILD_DATA;
		sink.raw_fd = take_raw(child);
		return (fsimg_writer_output(child->writer, &sink, error));
	}
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
	/* The staged file ends at the image's length, which a container
	 * knows only once it has encoded the disk.
	 */
	fsimg_sink_init(&sink, FSIMG_SINK_IO);
	sink.io = output;
	sink.raw_fd = take_raw(child);
	status = fsimg_writer_output(child->writer, &sink, error);
	if (status == FSIMG_OK)
		status = fsimg_io_get_size(output, &child->result.image_bytes,
		    error);
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
	fsimg_image_free(child->image);
	fsimg_io_free(child->input);
	fsimg_writer_free(child->writer);
	fsimg_dealloc(child->paths);
	if (child->output_root != -1)
		fsimg_fd_release(child->ctx, child->output_root);
	fsimg_ctx_free(child->ctx);
	for (unsigned i = 0; i < CHILD_CAP_COUNT; i++) {
		if (child->raw_caps[i] != -1)
			close(child->raw_caps[i]);
	}
	for (uint32_t i = child->roots_adopted; i < child->request.root_count; i++)
		close(FSIMG_CHILD_ROOTS + (int)i);
}

int
fsimg_worker_main(void)
{
	worker_child_t child;
	fsimg_sandbox_caps_t caps;
	fsimg_error_t error;
	fsimg_stats_t stats;
	unsigned char raw[FSIMG_WIRE_ERROR_BYTES];
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
		status = validate_paths(&child, &error);
	if (status == FSIMG_OK) {
		if (fsimg_job_has_roots(child.request.kind))
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
	fsimg_wire_put64(raw + FSIMG_WIRE_DONE_SKIPPED, child.result.skipped_entries);
	status = fsimg_frame_write(FSIMG_CHILD_CONTROL, FSIMG_FRAME_DONE, raw, FSIMG_WIRE_DONE_BYTES, NULL);
	return (status == FSIMG_OK ? 0 : 1);
}
