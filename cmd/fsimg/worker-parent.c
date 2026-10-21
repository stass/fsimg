/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "host.h"
#include "internal.h"
#include "worker.h"

typedef enum worker_phase {
	WORKER_REQUEST,
	WORKER_REQUEST_FAILED,
	WORKER_STARTING,
	WORKER_WARNING,
	WORKER_READY_EVENT,
	WORKER_ACKNOWLEDGING,
	WORKER_STREAMING,
	WORKER_FAILED,
	WORKER_COMPLETE
} worker_phase_t;

/*
 * The order of a job's result frames. A read job starts in WAIT_IMAGE: its
 * IMAGE record leads to WAIT_HEADER, or for i without a selected volume to
 * UNSELECTED, after which only the table's PARTITION records and DONE may
 * follow. i's PARTITION records arrive in WAIT_HEADER or UNSELECTED. A
 * create job starts in WAIT_HEADER, for its PLAN.
 */
typedef enum worker_output_state {
	WORKER_WAIT_HEADER,
	WORKER_WAIT_IMAGE,
	WORKER_UNSELECTED,
	WORKER_OUTPUT_READY,
	WORKER_IN_ENTRY,
	WORKER_VERIFIED
} worker_output_state_t;

typedef enum worker_channel {
	CHANNEL_JOB,
	CHANNEL_CONTROL,
	CHANNEL_RAW,
	CHANNEL_CANCEL,
	CHANNEL_OUTPUT,
	CHANNEL_STATUS,
	CHANNEL_COUNT
} worker_channel_t;

typedef enum worker_process_role {
	PROCESS_PARSER,
	PROCESS_OUTPUT,
	PROCESS_COUNT
} worker_process_role_t;

_Static_assert(FSIMG_CLI_PROCESS_BOUND == PROCESS_COUNT + 1,
    "CLI process bound includes coordinator and every supervised role");

typedef struct worker_process {
	pid_t		 pid;
	int		 status;
	uint64_t	 finish_deadline;
	bool		 exited;
	bool		 channel_eof;
} worker_process_t;

typedef enum relay_phase {
	RELAY_IDLE,
	RELAY_HEADER,
	RELAY_PAYLOAD,
	RELAY_CONFIRM
} relay_phase_t;

/* A relay record selects stdout or stderr, then carries one bounded payload.
 * Only one record is outstanding. Acknowledgement releases its borrowed data.
 */
typedef struct worker_relay {
	unsigned char	 buffer[65536];
	unsigned char	 header[FSIMG_RELAY_HEADER_BYTES];
	unsigned char	 report[FSIMG_RELAY_STATUS_BYTES];
	fsimg_worker_text_t text;
	relay_phase_t	 phase;
	size_t		 offset;
	size_t		 report_bytes;
	uint64_t	 received;
	uint64_t	 forwarded[2];
	uint64_t	 confirmed[2];
	bool		 final;
} worker_relay_t;

/* The outbound job: one JOB frame, then SELECTION frames carrying the job's
 * NUL-terminated paths. path_index and path_offset locate the next byte of
 * that sequence; a frame may end inside a path or span several.
 */
typedef struct worker_request_cursor {
	unsigned char	 wire[FSIMG_FRAME_HEADER + FSIMG_FRAME_BYTES];
	size_t		 length;
	size_t		 used;
	uint32_t	 path_index;
	size_t		 path_offset;
} worker_request_cursor_t;

typedef struct worker_supervisor {
	const fsimg_worker_request_t *request;
	fsimg_worker_result_t *result;
	fsimg_worker_event_fn_t event;
	fsimg_output_producer_fn_t produce;
	void		*argument;
	const volatile sig_atomic_t *interrupted;
	fsimg_frame_decoder_t decoder;
	worker_request_cursor_t outbound;
	worker_relay_t	 relay;
	fsimg_output_budget_t local_budget;
	fsimg_output_budget_t *budget;
	worker_process_t processes[PROCESS_COUNT];
	int		 channels[CHANNEL_COUNT][2];
	worker_phase_t	 phase;
	worker_output_state_t output_state;
	uint64_t	 deadline;
	uint64_t	 name_length;
	uint64_t	 name_bytes;
	uint64_t	 entry_size;
	uint64_t	 entry_count;
	uint64_t	 output_limit;
	uint64_t	 volume_limit;
	uint64_t	 disk_bytes;	/* The read job's input file size */
	uint64_t	 table_slots;	/* From the IMAGE record */
	uint64_t	 last_partition; /* Slot of the last PARTITION record */
	bool		 raw_eof;
	bool		 stdout_available;
	bool		 producer_done;
} worker_supervisor_t;

static uint64_t monotonic_milliseconds(void);
static fsimg_status_t worker_error(fsimg_error_t *, const char *);
static fsimg_status_t queue_text(worker_supervisor_t *, const fsimg_worker_text_t *,
    bool, fsimg_error_t *);
static fsimg_status_t deliver_event(worker_supervisor_t *, fsimg_error_t *);
static fsimg_status_t handle_frame(worker_supervisor_t *, fsimg_error_t *);
static fsimg_status_t read_control(worker_supervisor_t *, fsimg_error_t *);
static fsimg_status_t prepare_request(worker_supervisor_t *, fsimg_error_t *);
static size_t fill_paths_frame(const fsimg_worker_request_t *,
    worker_request_cursor_t *, unsigned char *);
static void advance_request(worker_supervisor_t *);
static fsimg_status_t setup_channels(worker_supervisor_t *, fsimg_error_t *);
static fsimg_status_t start_processes(worker_supervisor_t *, const char *, int,
    fsimg_error_t *);
static fsimg_status_t read_raw(worker_supervisor_t *, fsimg_error_t *);
static fsimg_status_t write_relay(worker_supervisor_t *, fsimg_error_t *);
static fsimg_status_t read_relay(worker_supervisor_t *, fsimg_error_t *);
static fsimg_status_t advance_ready(worker_supervisor_t *, fsimg_error_t *);
static fsimg_status_t observe_process(worker_process_t *, fsimg_error_t *);
static fsimg_status_t supervise_operation(worker_supervisor_t *, fsimg_error_t *);
static void supervisor_init(worker_supervisor_t *, const fsimg_worker_request_t *,
    fsimg_worker_result_t *, void *, int, const volatile sig_atomic_t *);
static fsimg_status_t finish_operation(worker_supervisor_t *, fsimg_status_t,
    fsimg_error_t *);
static void stop_process(worker_process_t *);
static void close_channel(int *);
static void cleanup_operation(worker_supervisor_t *);

static uint64_t
monotonic_milliseconds(void)
{
	struct timespec time;

	if (clock_gettime(CLOCK_MONOTONIC, &time) == -1)
		return (0);
	return ((uint64_t)time.tv_sec * 1000 + (uint64_t)time.tv_nsec / 1000000);
}

static fsimg_status_t
worker_error(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, FSIMG_EIO, 0, FSIMG_OP_CONTEXT, message));
}

static void
close_channel(int *fd)
{
	if (*fd != -1)
		close(*fd);
	*fd = -1;
}

static fsimg_status_t
queue_text(worker_supervisor_t *state, const fsimg_worker_text_t *text,
    bool raw, fsimg_error_t *error)
{
	worker_relay_t *relay = &state->relay;
	uint64_t total, ceiling;
	bool fits;

	if (text->length == 0)
		return (FSIMG_OK);
	if (text->data == NULL || text->length > FSIMG_WORKER_TEXT_BYTES ||
	    relay->phase != RELAY_IDLE)
		return (worker_error(error, "invalid or overlapping worker text output"));
	if (!raw) {
		ceiling = state->budget->limit_bytes;
		if (ceiling < FSIMG_OUTPUT_ERROR_RESERVE)
			return (worker_error(error, "formatted output budget lacks its error reserve"));
		if (!state->budget->reporting)
			ceiling -= FSIMG_OUTPUT_ERROR_RESERVE;
		fits = fsimg_add_u64(state->budget->text_bytes, text->length, &total);
		if (!fits)
			total = UINT64_MAX;
		if (!fits || total > ceiling) {
			fsimg_error_set(error, FSIMG_ELIMIT, 0, FSIMG_OP_CONTEXT,
			    "formatted output exceeds its derived text ceiling");
			if (error != NULL) {
				error->resource = FSIMG_RESOURCE_OUTPUT;
				error->limit = ceiling;
				error->observed = total;
			}
			return (FSIMG_ELIMIT);
		}
		state->budget->text_bytes = total;
	}
	relay->text = *text;
	fsimg_wire_put64(relay->header + FSIMG_RELAY_STREAM, text->diagnostic ? 2 : 1);
	fsimg_wire_put64(relay->header + FSIMG_RELAY_LENGTH, text->length);
	relay->offset = 0;
	relay->phase = RELAY_HEADER;
	return (FSIMG_OK);
}

static fsimg_status_t
deliver_event(worker_supervisor_t *state, fsimg_error_t *error)
{
	fsimg_worker_text_t text = { 0 };
	fsimg_status_t status;

	status = state->event(state->argument, &state->decoder.frame, &text, error);
	if (status > FSIMG_OK || status < FSIMG_EDURABILITY)
		return (worker_error(error, "invalid worker event callback status"));
	if (status != FSIMG_OK) {
		if (error != NULL && error->status != status)
			fsimg_error_set(error, status, 0, FSIMG_OP_CONTEXT,
			    "worker event callback failed");
		return (status);
	}
	fsimg_error_clear(error);
	if (text.length != 0 && !text.diagnostic && !state->stdout_available)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_CONTEXT,
		    "worker text requires a standard-output descriptor"));
	if (state->request->kind == FSIMG_JOB_CAT && text.length != 0 && !text.diagnostic)
		return (worker_error(error, "CAT event text must use diagnostic output"));
	return (queue_text(state, &text, false, error));
}


static fsimg_status_t
handle_frame(worker_supervisor_t *state, fsimg_error_t *error)
{
	const fsimg_frame_t *frame;
	const unsigned char *raw;
	fsimg_job_kind_t kind;
	fsimg_worker_text_t warning;
	fsimg_error_t detail;
	uint64_t path_limit, count, flags;
	fsimg_status_t status;

	frame = &state->decoder.frame;
	raw = frame->payload;
	kind = state->request->kind;
	if (state->phase == WORKER_COMPLETE || state->phase == WORKER_FAILED)
		return (worker_error(error, "worker sent data after its final frame"));
	if (frame->type == FSIMG_FRAME_ERROR) {
		status = fsimg_wire_error_decode(raw, frame->length, &detail);
		/* Only the coordinator holds final-parent publication authority. */
		if (detail.output_state > FSIMG_OUTPUT_INCOMPLETE)
			return (worker_error(error, "worker claimed an impossible publication outcome"));
		if (error != NULL)
			*error = detail;
		state->phase = WORKER_FAILED;
		return (status);
	}
	if (state->phase == WORKER_REQUEST || state->phase == WORKER_REQUEST_FAILED)
		return (worker_error(error, "worker responded before its request was sent"));
	if (state->phase == WORKER_STARTING) {
		if (frame->type != FSIMG_FRAME_READY || frame->length != 8 ||
		    fsimg_wire_get64(raw) > 2)
			return (worker_error(error, "worker did not send valid readiness"));
		state->result->confinement = (fsimg_confinement_t)fsimg_wire_get64(raw);
		if ((state->request->sandbox == FSIMG_SANDBOX_OFF) !=
		    (state->result->confinement == FSIMG_CONFINEMENT_DISABLED))
			return (worker_error(error, "worker confinement policy is inconsistent"));
		if (state->request->sandbox == FSIMG_SANDBOX_REQUIRED &&
		    state->result->confinement != FSIMG_CONFINEMENT_ACTIVE)
			return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
			    FSIMG_OP_CONTEXT, "required OS confinement is unavailable"));
		/* Parsing is acknowledged only after visible warning delivery and
		 * the caller's readiness event have both completed in the relay.
		 */
		bzero(&warning, sizeof(warning));
		warning.diagnostic = true;
		if (state->result->confinement == FSIMG_CONFINEMENT_UNAVAILABLE)
			warning.data = "fsimg: warning: OS sandbox unavailable; continuing without confinement\n";
		else if (state->result->confinement == FSIMG_CONFINEMENT_DISABLED)
			warning.data = "fsimg: warning: OS sandbox explicitly disabled\n";
		if (warning.data != NULL)
			warning.length = strlen(warning.data);
		state->phase = WORKER_WARNING;
		return (queue_text(state, &warning, false, error));
	}

	if (state->output_state == WORKER_IN_ENTRY && frame->type != FSIMG_FRAME_NAME &&
	    frame->type != FSIMG_FRAME_ENTRY_END)
		return (worker_error(error, "worker interrupted an entry name"));
	switch (frame->type) {
	case FSIMG_FRAME_IMAGE:
		/* Only i reports a disk without a volume; only W verifies tables. */
		if (kind >= FSIMG_JOB_CREATE || state->output_state != WORKER_WAIT_IMAGE ||
		    !fsimg_wire_image_frame_valid(frame, state->disk_bytes))
			return (worker_error(error, "invalid worker image information"));
		flags = fsimg_wire_get64(raw + FSIMG_WIRE_IMAGE_FLAGS);
		if (((flags & FSIMG_WIRE_IMAGE_UNSELECTED) != 0 && kind != FSIMG_JOB_INFO) ||
		    ((flags & FSIMG_WIRE_IMAGE_VERIFIED) != 0 && kind != FSIMG_JOB_VERIFY))
			return (worker_error(error, "invalid worker image information"));
		state->table_slots = fsimg_wire_get64(raw + FSIMG_WIRE_IMAGE_SLOTS);
		state->output_state = (flags & FSIMG_WIRE_IMAGE_UNSELECTED) != 0 ?
		    WORKER_UNSELECTED : WORKER_WAIT_HEADER;
		break;
	case FSIMG_FRAME_PARTITION:
		if (kind != FSIMG_JOB_INFO || (state->output_state != WORKER_WAIT_HEADER &&
		    state->output_state != WORKER_UNSELECTED) ||
		    !fsimg_wire_partition_frame_valid(frame, state->table_slots,
		    state->disk_bytes) ||
		    fsimg_wire_get64(raw + FSIMG_WIRE_PARTITION_INDEX) <=
		    state->last_partition)
			return (worker_error(error, "invalid worker partition record"));
		state->last_partition = fsimg_wire_get64(raw + FSIMG_WIRE_PARTITION_INDEX);
		break;
	case FSIMG_FRAME_INFO:
		if (kind >= FSIMG_JOB_CREATE || state->output_state != WORKER_WAIT_HEADER ||
		    !fsimg_wire_info_frame_valid(frame, state->volume_limit))
			return (worker_error(error, "invalid worker volume information"));
		state->output_state = WORKER_OUTPUT_READY;
		break;
	case FSIMG_FRAME_PLAN:
		if (kind < FSIMG_JOB_CREATE || state->output_state != WORKER_WAIT_HEADER ||
		    !fsimg_wire_plan_frame_valid(frame))
			return (worker_error(error, "invalid worker image plan"));
		state->output_state = WORKER_OUTPUT_READY;
		state->result->image_bytes = fsimg_wire_get64(raw + FSIMG_WIRE_PLAN_VOLUME);
		break;
	case FSIMG_FRAME_ENTRY:
		path_limit = state->request->limits.max_path_bytes == 0 ? 65536 :
		    state->request->limits.max_path_bytes;
		if ((kind != FSIMG_JOB_LIST && kind != FSIMG_JOB_CAT) ||
		    state->output_state != WORKER_OUTPUT_READY ||
		    !fsimg_wire_entry_frame_valid(frame, kind == FSIMG_JOB_CAT, path_limit))
			return (worker_error(error, "invalid worker entry start"));
		if (kind == FSIMG_JOB_CAT && state->entry_count != 0)
			return (worker_error(error, "worker sent more than one CAT entry"));
		if (state->entry_count >= (state->request->limits.max_entries == 0 ?
		    10000000 : state->request->limits.max_entries)) {
			count = state->entry_count;
			fsimg_stat_add(&count, 1);
			return (fsimg_limit_error(error, FSIMG_RESOURCE_ENTRIES,
			    state->request->limits.max_entries == 0 ? 10000000 :
			    state->request->limits.max_entries, count));
		}
		state->name_length = fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_NAME_LENGTH);
		state->entry_size = fsimg_wire_get64(raw + FSIMG_WIRE_ENTRY_LENGTH);
		state->name_bytes = 0;
		state->output_state = WORKER_IN_ENTRY;
		break;
	case FSIMG_FRAME_NAME:
		if (state->output_state != WORKER_IN_ENTRY || frame->length == 0 ||
		    frame->length > state->name_length - state->name_bytes ||
		    memchr(raw, '\0', frame->length) != NULL)
			return (worker_error(error, "invalid worker name chunk"));
		state->name_bytes += frame->length;
		break;
	case FSIMG_FRAME_ENTRY_END:
		if (state->output_state != WORKER_IN_ENTRY || frame->length != 0 ||
		    state->name_bytes != state->name_length)
			return (worker_error(error, "incomplete worker entry name"));
		state->output_state = WORKER_OUTPUT_READY;
		state->entry_count++;
		break;
	case FSIMG_FRAME_VERIFY:
		if (kind != FSIMG_JOB_VERIFY || state->output_state != WORKER_OUTPUT_READY ||
		    !fsimg_wire_verify_frame_valid(frame,
		    (state->request->flags & FSIMG_JOB_PAYLOAD) != 0))
			return (worker_error(error, "invalid worker verification report"));
		state->output_state = WORKER_VERIFIED;
		break;
	case FSIMG_FRAME_PROGRESS:
		if (frame->length != 24)
			return (worker_error(error, "invalid worker progress frame"));
		break;
	case FSIMG_FRAME_LOSS:
		if (frame->length < FSIMG_WIRE_LOSS_BYTES || frame->length > FSIMG_WIRE_LOSS_BYTES + FSIMG_WIRE_LOSS_PATH_BYTES ||
		    fsimg_wire_get64(raw + FSIMG_WIRE_LOSS_LENGTH) != frame->length - FSIMG_WIRE_LOSS_BYTES ||
		    fsimg_wire_get64(raw + FSIMG_WIRE_LOSS_TRUNCATED) > 1 ||
		    fsimg_wire_get64(raw + FSIMG_WIRE_LOSS_FIELDS) == 0 ||
		    (fsimg_wire_get64(raw + FSIMG_WIRE_LOSS_FIELDS) & ~(uint64_t)FSIMG_META_KNOWN) != 0)
			return (worker_error(error, "invalid worker metadata loss frame"));
		break;
	case FSIMG_FRAME_DONE:
		if ((state->output_state != (kind == FSIMG_JOB_VERIFY ?
		    WORKER_VERIFIED : WORKER_OUTPUT_READY) &&
		    state->output_state != WORKER_UNSELECTED) ||
		    frame->length != FSIMG_WIRE_DONE_BYTES ||
		    (kind == FSIMG_JOB_CAT && state->entry_count != 1))
			return (worker_error(error, "worker completion arrived before its result"));
		count = fsimg_wire_get64(raw + FSIMG_WIRE_DONE_ENTRIES);
		if ((kind == FSIMG_JOB_LIST || kind == FSIMG_JOB_CAT) &&
		    count != state->entry_count)
			return (worker_error(error, "worker entry completion count differs"));
		state->result->entries = count;
		state->result->logical_bytes = fsimg_wire_get64(raw + FSIMG_WIRE_DONE_LOGICAL);
		state->result->metadata_losses = fsimg_wire_get64(raw + FSIMG_WIRE_DONE_LOSSES);
		state->result->skipped_entries = fsimg_wire_get64(raw + FSIMG_WIRE_DONE_SKIPPED);
		if ((kind != FSIMG_JOB_EXTRACT ||
		    (state->request->flags & FSIMG_JOB_SKIP_UNSUPPORTED) == 0) &&
		    state->result->skipped_entries != 0)
			return (worker_error(error, "worker skipped entries it was not asked to skip"));
		if (fsimg_wire_get64(raw + FSIMG_WIRE_DONE_IMAGE) != state->result->image_bytes ||
		    fsimg_wire_get64(raw + FSIMG_WIRE_DONE_RAW) != (kind == FSIMG_JOB_CAT ?
		    state->entry_size : 0))
			return (worker_error(error, "worker raw completion length differs"));
		state->phase = WORKER_COMPLETE;
		/* Completion is delivered only after successful process exit. */
		return (FSIMG_OK);
	default:
		return (worker_error(error, "worker sent an out-of-order frame"));
	}
	return (deliver_event(state, error));
}

/* Read only the decoder's current header or payload remainder. One completed
 * frame remains stable while its borrowed formatted output is acknowledged.
 */
static fsimg_status_t
read_control(worker_supervisor_t *state, fsimg_error_t *error)
{
	fsimg_frame_decoder_t *decoder = &state->decoder;
	unsigned char buffer[4096];
	size_t needed, consumed;
	ssize_t count;
	fsimg_status_t status;

	needed = decoder->header_used < FSIMG_FRAME_HEADER ?
	    FSIMG_FRAME_HEADER - decoder->header_used : decoder->frame.length - decoder->payload_used;
	if (needed > sizeof(buffer))
		needed = sizeof(buffer);
	count = read(state->channels[CHANNEL_CONTROL][0], buffer, needed);
	if (count == -1 && (errno == EAGAIN || errno == EINTR))
		return (FSIMG_OK);
	if (count == -1)
		return (worker_error(error, "worker control read failed"));
	if (count == 0) {
		state->processes[PROCESS_PARSER].channel_eof = true;
		status = fsimg_frame_finish(decoder, error);
		if (status != FSIMG_OK)
			return (status);
		if (state->phase != WORKER_COMPLETE)
			return (worker_error(error, "worker ended before completion"));
		return (FSIMG_OK);
	}
	status = fsimg_frame_feed(decoder, buffer, (size_t)count, &consumed, error);
	if (status != FSIMG_OK || !decoder->complete)
		return (status);
	return (handle_frame(state, error));
}

static fsimg_status_t
prepare_request(worker_supervisor_t *state, fsimg_error_t *error)
{
	const fsimg_worker_request_t *request = state->request;
	worker_request_cursor_t *outbound = &state->outbound;
	fsimg_host_file_t identity;
	uint64_t started;
	fsimg_status_t status;

	started = monotonic_milliseconds();
	if (request->deadline_milliseconds == 0 &&
	    request->seconds > (UINT64_MAX - started) / 1000)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_CONTEXT,
		    "worker deadline exceeds clock range"));
	if (request->deadline_milliseconds != 0)
		state->deadline = request->deadline_milliseconds;
	else if (request->seconds != 0)
		state->deadline = started + request->seconds * 1000;
	state->output_limit = request->limits.max_output_bytes;
	if (request->input_fd != -1) {
		if (fsimg_host_file(request->input_fd, &identity) == -1)
			return (fsimg_error_set(error, FSIMG_EIO, errno,
			    FSIMG_OP_CONTEXT, "worker input identity query failed"));
		if (identity.kind != FSIMG_HOST_FILE || request->offset > identity.length)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_CONTEXT, "worker input must be a bounded regular file"));
		/* Only a range selector sets offset and length; any other
		 * volume lies within the whole file.
		 */
		state->disk_bytes = identity.length;
		state->volume_limit = request->length == 0 ? identity.length - request->offset :
		    request->length;
		if (state->volume_limit > identity.length - request->offset)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_CONTEXT, "worker input slice exceeds the regular file"));
		if (state->output_limit == 0)
			state->output_limit = state->volume_limit;
	}
	status = fsimg_job_encode(request, outbound->wire + FSIMG_FRAME_HEADER, error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_frame_header(outbound->wire, FSIMG_FRAME_JOB, FSIMG_JOB_BYTES, NULL);
	outbound->length = FSIMG_FRAME_HEADER + FSIMG_JOB_BYTES;
	return (FSIMG_OK);
}

static void
advance_request(worker_supervisor_t *state)
{
	worker_request_cursor_t *outbound = &state->outbound;
	size_t batch;
	ssize_t count;

	count = write(state->channels[CHANNEL_JOB][1], outbound->wire + outbound->used,
	    outbound->length - outbound->used);
	if (count == -1 && (errno == EINTR || errno == EAGAIN))
		return;
	if (count <= 0) {
		/* A rejected startup can close JOB before its structured ERROR is
		 * drained. Preserve that diagnostic instead of racing it with EPIPE.
		 */
		if (state->phase == WORKER_ACKNOWLEDGING)
			fsimg_frame_advance(&state->decoder);
		state->phase = WORKER_REQUEST_FAILED;
		state->processes[PROCESS_PARSER].finish_deadline =
		    monotonic_milliseconds() + 2000;
		return;
	}
	outbound->used += (size_t)count;
	if (outbound->used != outbound->length)
		return;
	if (state->phase == WORKER_ACKNOWLEDGING) {
		state->phase = WORKER_STREAMING;
		fsimg_frame_advance(&state->decoder);
		return;
	}
	batch = fill_paths_frame(state->request, outbound,
	    outbound->wire + FSIMG_FRAME_HEADER);
	if (batch == 0) {
		state->phase = WORKER_STARTING;
		return;
	}
	fsimg_frame_header(outbound->wire, FSIMG_FRAME_SELECTION, (uint32_t)batch, NULL);
	outbound->length = FSIMG_FRAME_HEADER + batch;
	outbound->used = 0;
	return;
}

/* Copy the next bytes of the job's path sequence, the member of a read job
 * or each root's image path of a create job, every path with its NUL.
 * Encoding has already checked each path against the path limit.
 */
static size_t
fill_paths_frame(const fsimg_worker_request_t *request,
    worker_request_cursor_t *outbound, unsigned char *payload)
{
	const char *path;
	size_t used, length, batch;
	uint32_t count;

	count = fsimg_job_path_count(request);
	used = 0;
	while (used < FSIMG_FRAME_BYTES && outbound->path_index < count) {
		path = fsimg_job_path(request, outbound->path_index);
		length = strlen(path) + 1;
		batch = length - outbound->path_offset;
		if (batch > FSIMG_FRAME_BYTES - used)
			batch = FSIMG_FRAME_BYTES - used;
		bcopy(path + outbound->path_offset, payload + used, batch);
		used += batch;
		outbound->path_offset += batch;
		if (outbound->path_offset == length) {
			outbound->path_index++;
			outbound->path_offset = 0;
		}
	}
	return (used);
}

static fsimg_status_t
setup_channels(worker_supervisor_t *state, fsimg_error_t *error)
{
	bool nonblocking;
	int fd, flags;

	for (unsigned role = 0; role < CHANNEL_COUNT; role++) {
		if (pipe(state->channels[role]) == -1)
			return (fsimg_error_set(error, FSIMG_EIO, errno,
			    FSIMG_OP_CONTEXT, "worker pipe creation failed"));
		for (unsigned end = 0; end < 2; end++) {
			fd = state->channels[role][end];
			if (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
				return (fsimg_error_set(error, FSIMG_EIO, errno,
				    FSIMG_OP_CONTEXT, "worker pipe inheritance setup failed"));
			/* Only owned supervisor ends and the child's cancellation read
			 * are nonblocking. Borrowed sink flags never change.
			 */
			nonblocking = end == 0 ? role != CHANNEL_JOB && role != CHANNEL_OUTPUT :
			    role == CHANNEL_JOB || role == CHANNEL_OUTPUT;
			if (!nonblocking)
				continue;
			flags = fcntl(fd, F_GETFL);
			if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
				return (fsimg_error_set(error, FSIMG_EIO, errno,
				    FSIMG_OP_CONTEXT, "worker pipe nonblocking setup failed"));
		}
	}
	return (FSIMG_OK);
}

static fsimg_status_t
start_processes(worker_supervisor_t *state, const char *executable, int standard_output,
    fsimg_error_t *error)
{
	const fsimg_worker_request_t *request = state->request;
	int *parser, output[5];
	unsigned parser_count;
	fsimg_status_t status;
	int null_fd, code, saved;

	status = setup_channels(state, error);
	if (status != FSIMG_OK)
		return (status);
	null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	if (null_fd == -1)
		return (fsimg_error_set(error, FSIMG_EIO, errno,
		    FSIMG_OP_CONTEXT, "worker standard-stream setup failed"));
	for (unsigned i = 0; i < 5; i++)
		output[i] = null_fd;
	output[STDOUT_FILENO] = standard_output == -1 ? null_fd : standard_output;
	output[STDERR_FILENO] = STDERR_FILENO;
	output[3] = state->channels[CHANNEL_OUTPUT][0];
	output[4] = state->channels[CHANNEL_STATUS][1];
	code = fsimg_host_spawn_output(executable, output, 5,
	    &state->processes[PROCESS_OUTPUT].pid);
	if (code == -1) {
		saved = errno;
		close(null_fd);
		return (fsimg_error_set(error, FSIMG_EIO, saved,
		    FSIMG_OP_CONTEXT, "output relay execution failed"));
	}
	if (state->produce != NULL) {
		/* Output-only commands share the same relay and supervision states.
		 * The absent parser has already completed both of its streams.
		 */
		state->processes[PROCESS_PARSER].exited = true;
		state->processes[PROCESS_PARSER].channel_eof = true;
		state->raw_eof = true;
		state->phase = WORKER_COMPLETE;
		close(null_fd);
		close_channel(&state->channels[CHANNEL_OUTPUT][0]);
		close_channel(&state->channels[CHANNEL_STATUS][1]);
		return (FSIMG_OK);
	}
	/* The fixed layout, then one descriptor per create root. */
	if (request->root_count > INT_MAX - FSIMG_CHILD_ROOTS) {
		close(null_fd);
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_CONTEXT,
		    "worker descriptor layout exceeds the descriptor range"));
	}
	parser_count = FSIMG_CHILD_ROOTS + request->root_count;
	parser = calloc(parser_count, sizeof(*parser));
	if (parser == NULL) {
		close(null_fd);
		return (fsimg_error_set(error, FSIMG_ENOMEM, ENOMEM,
		    FSIMG_OP_CONTEXT, "worker descriptor table allocation failed"));
	}
	for (unsigned i = 0; i < parser_count; i++)
		parser[i] = null_fd;
	parser[FSIMG_CHILD_JOB] = state->channels[CHANNEL_JOB][0];
	parser[FSIMG_CHILD_CONTROL] = state->channels[CHANNEL_CONTROL][1];
	parser[FSIMG_CHILD_DATA] = state->channels[CHANNEL_RAW][1];
	parser[FSIMG_CHILD_CANCEL] = state->channels[CHANNEL_CANCEL][0];
	if (request->input_fd != -1)
		parser[FSIMG_CHILD_INPUT] = request->input_fd;
	if (request->output_fd != -1)
		parser[FSIMG_CHILD_OUTPUT] = request->output_fd;
	if (request->scratch_fd != -1)
		parser[FSIMG_CHILD_SCRATCH] = request->scratch_fd;
	for (uint32_t i = 0; i < request->root_count; i++)
		parser[FSIMG_CHILD_ROOTS + i] = request->root_fds[i];
	code = fsimg_host_spawn_worker(executable, parser, parser_count,
	    &state->processes[PROCESS_PARSER].pid);
	saved = errno;
	free(parser);
	close(null_fd);
	if (code == -1)
		return (fsimg_error_set(error, FSIMG_EIO, saved,
		    FSIMG_OP_CONTEXT, "fresh worker execution failed"));

	close_channel(&state->channels[CHANNEL_JOB][0]);
	close_channel(&state->channels[CHANNEL_CONTROL][1]);
	close_channel(&state->channels[CHANNEL_RAW][1]);
	close_channel(&state->channels[CHANNEL_CANCEL][0]);
	close_channel(&state->channels[CHANNEL_OUTPUT][0]);
	close_channel(&state->channels[CHANNEL_STATUS][1]);
	return (FSIMG_OK);
}

static fsimg_status_t
read_raw(worker_supervisor_t *state, fsimg_error_t *error)
{
	worker_relay_t *relay = &state->relay;
	fsimg_worker_text_t text;
	uint64_t observed;
	ssize_t count;

	count = read(state->channels[CHANNEL_RAW][0], relay->buffer, sizeof(relay->buffer));
	if (count == -1 && (errno == EAGAIN || errno == EINTR))
		return (FSIMG_OK);
	if (count == -1)
		return (worker_error(error, "worker raw channel read failed"));
	if (count == 0) {
		state->raw_eof = true;
		return (FSIMG_OK);
	}
	if (state->request->kind != FSIMG_JOB_CAT)
		return (worker_error(error, "unexpected worker raw output"));
	observed = UINT64_MAX;
	if (!fsimg_add_u64(relay->received, (uint64_t)count, &observed) ||
	    observed > state->output_limit)
		return (fsimg_limit_error(error, FSIMG_RESOURCE_OUTPUT,
		    state->output_limit, observed));
	if (observed > state->entry_size)
		return (worker_error(error, "worker raw output exceeds its selected file"));
	relay->received = observed;
	text = (fsimg_worker_text_t){ .data = relay->buffer, .length = (size_t)count };
	return (queue_text(state, &text, true, error));
}

static fsimg_status_t
write_relay(worker_supervisor_t *state, fsimg_error_t *error)
{
	worker_relay_t *relay = &state->relay;
	const unsigned char *data;
	size_t length;
	unsigned stream;
	ssize_t count;

	data = relay->phase == RELAY_HEADER ? relay->header : relay->text.data;
	length = relay->phase == RELAY_HEADER ? sizeof(relay->header) : relay->text.length;
	count = write(state->channels[CHANNEL_OUTPUT][1], data + relay->offset,
	    length - relay->offset);
	if (count == -1 && (errno == EAGAIN || errno == EINTR))
		return (FSIMG_OK);
	if (count <= 0)
		return (worker_error(error, "output relay write failed"));
	if (relay->phase == RELAY_PAYLOAD) {
		stream = relay->text.diagnostic ? 1 : 0;
		if (!fsimg_add_u64(relay->forwarded[stream], (uint64_t)count,
		    &relay->forwarded[stream]))
			return (worker_error(error, "output relay byte count overflow"));
	}
	relay->offset += (size_t)count;
	if (relay->offset == length) {
		relay->offset = 0;
		relay->phase = relay->phase == RELAY_HEADER ? RELAY_PAYLOAD : RELAY_CONFIRM;
	}
	return (FSIMG_OK);
}

static fsimg_status_t
read_relay(worker_supervisor_t *state, fsimg_error_t *error)
{
	worker_relay_t *relay = &state->relay;
	uint64_t confirmed[2], totals[2], failure, marker;
	ssize_t count;

	count = read(state->channels[CHANNEL_STATUS][0], relay->report + relay->report_bytes,
	    sizeof(relay->report) - relay->report_bytes);
	if (count == -1 && (errno == EAGAIN || errno == EINTR))
		return (FSIMG_OK);
	if (count == -1)
		return (worker_error(error, "output relay status read failed"));
	if (count == 0) {
		if (!relay->final || relay->report_bytes != 0)
			return (worker_error(error, "output relay ended before completion"));
		state->processes[PROCESS_OUTPUT].channel_eof = true;
		return (FSIMG_OK);
	}
	if (relay->final)
		return (worker_error(error, "output relay sent status after completion"));
	relay->report_bytes += (size_t)count;
	if (relay->report_bytes != sizeof(relay->report))
		return (FSIMG_OK);
	confirmed[0] = fsimg_wire_get64(relay->report + FSIMG_RELAY_STDOUT);
	confirmed[1] = fsimg_wire_get64(relay->report + FSIMG_RELAY_STDERR);
	failure = fsimg_wire_get64(relay->report + FSIMG_RELAY_ERRNO);
	marker = fsimg_wire_get64(relay->report + FSIMG_RELAY_MARKER);
	if (failure > INT_MAX || marker > 2 || (failure != 0 && marker != 2))
		return (worker_error(error, "invalid output relay status"));
	for (unsigned stream = 0; stream < 2; stream++) {
		if (confirmed[stream] < relay->confirmed[stream] ||
		    confirmed[stream] > relay->forwarded[stream])
			return (worker_error(error, "invalid output relay byte count"));
		if (!fsimg_add_u64(state->budget->confirmed[stream],
		    confirmed[stream] - relay->confirmed[stream], &totals[stream]))
			return (worker_error(error, "aggregate output byte count overflow"));
	}
	for (unsigned stream = 0; stream < 2; stream++) {
		state->budget->confirmed[stream] = totals[stream];
		relay->confirmed[stream] = confirmed[stream];
	}
	if (state->request->kind == FSIMG_JOB_CAT)
		state->result->raw_bytes = confirmed[0];
	relay->report_bytes = 0;
	if (failure != 0)
		return (fsimg_error_set(error, FSIMG_EIO, (int)failure,
		    FSIMG_OP_CONTEXT, "output write failed"));
	if (marker != 0) {
		if (confirmed[0] != relay->forwarded[0] || confirmed[1] != relay->forwarded[1] ||
		    (marker == 1 && relay->phase != RELAY_CONFIRM) ||
		    (marker == 2 && state->channels[CHANNEL_OUTPUT][1] != -1))
			return (worker_error(error, "premature output relay acknowledgement"));
		relay->phase = RELAY_IDLE;
		relay->final = marker == 2;
	}
	return (FSIMG_OK);
}

static fsimg_status_t
advance_ready(worker_supervisor_t *state, fsimg_error_t *error)
{
	fsimg_status_t status;

	if (state->relay.phase != RELAY_IDLE)
		return (FSIMG_OK);
	if (state->phase == WORKER_WARNING) {
		status = deliver_event(state, error);
		if (status != FSIMG_OK)
			return (status);
		state->phase = WORKER_READY_EVENT;
	}
	if (state->phase == WORKER_READY_EVENT && state->relay.phase == RELAY_IDLE) {
		fsimg_frame_header(state->outbound.wire, FSIMG_FRAME_ACK, 0, NULL);
		state->outbound.length = FSIMG_FRAME_HEADER;
		state->outbound.used = 0;
		state->phase = WORKER_ACKNOWLEDGING;
	}
	return (FSIMG_OK);
}

/* Channel EOF and process exit may arrive in either order. EOF starts the same
 * bounded exit grace for both roles. Pending output can delay channel drainage
 * after process exit, so exit alone does not start that grace.
 * Queued control errors are drained before final exit status is interpreted.
 */
static fsimg_status_t
observe_process(worker_process_t *process, fsimg_error_t *error)
{
	pid_t waited;
	uint64_t now;

	if (!process->exited) {
		waited = waitpid(process->pid, &process->status, WNOHANG);
		if (waited == process->pid)
			process->exited = true;
		else if (waited == -1 && errno != EINTR)
			return (worker_error(error, "child process supervision failed"));
	}
	if (process->exited && process->channel_eof)
		return (FSIMG_OK);
	now = monotonic_milliseconds();
	if (process->channel_eof && process->finish_deadline == 0)
		process->finish_deadline = now + 2000;
	if (process->finish_deadline != 0 && now >= process->finish_deadline)
		return (worker_error(error, "child exit and channel closure did not complete"));
	return (FSIMG_OK);
}

/* While a descriptor is polled, its next event wakes the poll, and the
 * timeout only bounds how late cancellation and the deadline are noticed.
 * When none is polled, only a child's exit, which no descriptor reports, and
 * the clocks can end the wait; a short timeout reaps the children promptly
 * instead of adding the full interval to every command. A child that closes
 * its channels without exiting keeps the loop waking each millisecond only
 * until the exit grace observe_process armed at its channel EOF expires.
 */
static int
poll_milliseconds(const struct pollfd *channels)
{
	for (unsigned role = 0; role < CHANNEL_COUNT; role++) {
		if (channels[role].fd != -1)
			return (100);
	}
	return (1);
}

static fsimg_status_t
supervise_operation(worker_supervisor_t *state, fsimg_error_t *error)
{
	struct pollfd channels[CHANNEL_COUNT];
	worker_relay_t *relay = &state->relay;
	fsimg_worker_text_t text;
	fsimg_status_t status;
	bool finished, control_allowed;
	uint64_t now;

	for (;;) {
		now = monotonic_milliseconds();
		if ((state->interrupted != NULL && *state->interrupted != 0) ||
		    (state->deadline != 0 && now >= state->deadline))
			return (fsimg_error_set(error, FSIMG_ECANCELLED, 0,
			    FSIMG_OP_CONTEXT, "worker cancelled or deadline exceeded"));
		if (!state->producer_done && relay->phase == RELAY_IDLE) {
			bzero(&text, sizeof(text));
			status = state->produce(state->argument, &text, error);
			if (status == FSIMG_DONE)
				state->producer_done = true;
			else {
				if (status != FSIMG_OK)
					return (status);
				if (text.length == 0 ||
				    (!text.diagnostic && !state->stdout_available))
					return (worker_error(error, "invalid output producer span"));
				status = queue_text(state, &text, false, error);
				if (status != FSIMG_OK)
					return (status);
			}
		}
		status = advance_ready(state, error);
		if (status != FSIMG_OK)
			return (status);
		if (state->decoder.complete && relay->phase == RELAY_IDLE &&
		    (state->phase == WORKER_STREAMING || state->phase == WORKER_COMPLETE))
			fsimg_frame_advance(&state->decoder);

		finished = state->raw_eof;
		for (unsigned role = 0; role < PROCESS_COUNT; role++) {
			status = observe_process(&state->processes[role], error);
			if (status != FSIMG_OK)
				return (status);
			finished = finished && state->processes[role].channel_eof &&
			    state->processes[role].exited;
		}
		if (finished)
			break;
		if (state->processes[PROCESS_PARSER].channel_eof && state->raw_eof &&
		    relay->phase == RELAY_IDLE && state->producer_done)
			close_channel(&state->channels[CHANNEL_OUTPUT][1]);

		for (unsigned role = 0; role < CHANNEL_COUNT; role++)
			channels[role] = (struct pollfd){ .fd = -1 };
		if ((state->phase == WORKER_REQUEST && state->decoder.header_used == 0) ||
		    state->phase == WORKER_ACKNOWLEDGING)
			channels[CHANNEL_JOB] = (struct pollfd){
			    .fd = state->channels[CHANNEL_JOB][1], .events = POLLOUT };
		/* A completed frame stays borrowed until its output is confirmed.
		 * Stop control reads so the decoder cannot replace that storage.
		 */
		control_allowed = !state->decoder.complete && relay->phase == RELAY_IDLE &&
		    !state->processes[PROCESS_PARSER].channel_eof;
		if (control_allowed)
			channels[CHANNEL_CONTROL] = (struct pollfd){
			    .fd = state->channels[CHANNEL_CONTROL][0], .events = POLLIN };
		/* Hold raw bytes in their kernel pipe until the complete CAT entry.
		 * A pending text/raw record also retains its one borrowed buffer.
		 */
		if (!state->raw_eof && relay->phase == RELAY_IDLE &&
		    (state->request->kind != FSIMG_JOB_CAT || state->entry_count == 1))
			channels[CHANNEL_RAW] = (struct pollfd){
			    .fd = state->channels[CHANNEL_RAW][0], .events = POLLIN };
		if (relay->phase == RELAY_HEADER || relay->phase == RELAY_PAYLOAD)
			channels[CHANNEL_OUTPUT] = (struct pollfd){
			    .fd = state->channels[CHANNEL_OUTPUT][1], .events = POLLOUT };
		if (!state->processes[PROCESS_OUTPUT].channel_eof)
			channels[CHANNEL_STATUS] = (struct pollfd){
			    .fd = state->channels[CHANNEL_STATUS][0], .events = POLLIN };
		if (poll(channels, CHANNEL_COUNT, poll_milliseconds(channels)) == -1) {
			if (errno == EINTR)
				continue;
			return (worker_error(error, "worker channel polling failed"));
		}
		/* Drain queued errors before attempting another backpressured write. */
		if (channels[CHANNEL_CONTROL].revents != 0) {
			status = read_control(state, error);
			if (status != FSIMG_OK)
				return (status);
		}
		if (channels[CHANNEL_STATUS].revents != 0) {
			status = read_relay(state, error);
			if (status != FSIMG_OK)
				return (status);
		}
		if (channels[CHANNEL_JOB].revents != 0 &&
		    (state->phase == WORKER_ACKNOWLEDGING || state->decoder.header_used == 0)) {
			advance_request(state);
		}
		if (channels[CHANNEL_OUTPUT].revents != 0) {
			status = write_relay(state, error);
			if (status != FSIMG_OK)
				return (status);
		}
		if (channels[CHANNEL_RAW].revents != 0 && relay->phase == RELAY_IDLE) {
			status = read_raw(state, error);
			if (status != FSIMG_OK)
				return (status);
		}
	}
	for (unsigned role = 0; role < PROCESS_COUNT; role++) {
		if (!WIFEXITED(state->processes[role].status) ||
		    WEXITSTATUS(state->processes[role].status) != 0)
			return (worker_error(error, "child process exited unsuccessfully"));
	}
	if (state->phase != WORKER_COMPLETE || !relay->final ||
	    (state->request->kind == FSIMG_JOB_CAT &&
	    state->result->raw_bytes != state->entry_size))
		return (worker_error(error, "worker or raw output was incomplete"));
	return (FSIMG_OK);
}

static void
stop_process(worker_process_t *process)
{
	uint64_t stopping;
	pid_t waited;

	if (process->pid == -1 || process->exited)
		return;
	kill(process->pid, SIGTERM);
	stopping = monotonic_milliseconds() + 2000;
	for (;;) {
		waited = waitpid(process->pid, &process->status, WNOHANG);
		if (waited == process->pid) {
			process->exited = true;
			return;
		}
		if (waited == -1 && errno != EINTR)
			return;
		if (monotonic_milliseconds() >= stopping) {
			kill(process->pid, SIGKILL);
			while (waitpid(process->pid, &process->status, 0) == -1 && errno == EINTR)
				;
			process->exited = true;
			return;
		}
		poll(NULL, 0, 20);
	}
}

static void
cleanup_operation(worker_supervisor_t *state)
{
	close_channel(&state->channels[CHANNEL_CANCEL][1]);
	/* Signal both before waiting, so neither depends on the other's progress. */
	for (unsigned role = 0; role < PROCESS_COUNT; role++) {
		if (state->processes[role].pid != -1 && !state->processes[role].exited)
			kill(state->processes[role].pid, SIGTERM);
	}
	for (unsigned role = 0; role < PROCESS_COUNT; role++)
		stop_process(&state->processes[role]);
	for (unsigned role = 0; role < CHANNEL_COUNT; role++) {
		for (unsigned end = 0; end < 2; end++)
			close_channel(&state->channels[role][end]);
	}
}

static void
supervisor_init(worker_supervisor_t *state, const fsimg_worker_request_t *request,
    fsimg_worker_result_t *result, void *argument, int standard_output,
    const volatile sig_atomic_t *interrupted)
{
	bzero(state, sizeof(*state));
	state->request = request;
	state->result = result;
	state->argument = argument;
	state->interrupted = interrupted;
	state->stdout_available = standard_output != -1;
	state->producer_done = true;
	state->output_state = request->kind < FSIMG_JOB_CREATE ? WORKER_WAIT_IMAGE :
	    WORKER_WAIT_HEADER;
	if (request->output_budget == NULL) {
		fsimg_output_budget_init(&state->local_budget, &request->limits);
		state->budget = &state->local_budget;
	} else
		state->budget = request->output_budget;
	for (unsigned role = 0; role < CHANNEL_COUNT; role++)
		state->channels[role][0] = state->channels[role][1] = -1;
	for (unsigned role = 0; role < PROCESS_COUNT; role++)
		state->processes[role].pid = -1;
}

static fsimg_status_t
finish_operation(worker_supervisor_t *state, fsimg_status_t status,
    fsimg_error_t *error)
{
	cleanup_operation(state);
	if (state->relay.forwarded[0] != 0)
		state->budget->stdout_possible = true;
	/* Runtime failures are never an expected malformed-input result, even if
	 * channel EOF or a structured error was observed before process reaping.
	 */
	for (unsigned role = 0; role < PROCESS_COUNT; role++) {
		if (state->processes[role].exited && WIFEXITED(state->processes[role].status) &&
		    WEXITSTATUS(state->processes[role].status) == FSIMG_WORKER_RUNTIME_EXIT)
			status = fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_CONTEXT,
			    "worker sanitizer runtime reported failure");
	}
	/* Prepared output roots remain private on every failed operation, even
	 * when a dead or compromised worker sends no trustworthy outcome frame.
	 */
	if (status != FSIMG_OK && error != NULL &&
	    (state->budget->stdout_possible || state->request->output_fd != -1))
		error->output_state = FSIMG_OUTPUT_INCOMPLETE;
	if (status == FSIMG_OK)
		fsimg_error_clear(error);
	return (status);
}

fsimg_status_t
fsimg_worker_run(const char *executable, const fsimg_worker_request_t *request,
    fsimg_worker_event_fn_t event, void *argument, int standard_output,
    const volatile sig_atomic_t *interrupted, fsimg_worker_result_t *result,
    fsimg_error_t *error)
{
	worker_supervisor_t state;
	fsimg_worker_request_t selected;
	fsimg_status_t status;

	fsimg_error_clear(error);
	if (result != NULL) {
		bzero(result, sizeof(*result));
		result->confinement = FSIMG_CONFINEMENT_UNKNOWN;
	}
	if (request == NULL || result == NULL || event == NULL || executable == NULL ||
	    (request->kind == FSIMG_JOB_CAT && standard_output == -1))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_CONTEXT, "missing worker request, callback, output, or result"));
	selected = *request;
	supervisor_init(&state, &selected, result, argument, standard_output, interrupted);
	state.event = event;
	status = prepare_request(&state, error);
	if (status == FSIMG_OK)
		status = start_processes(&state, executable, standard_output, error);
	if (status == FSIMG_OK)
		status = supervise_operation(&state, error);
	return (finish_operation(&state, status, error));
}

fsimg_status_t
fsimg_output_run(const char *executable, fsimg_output_producer_fn_t produce,
    void *argument, int standard_output, uint64_t deadline,
    const volatile sig_atomic_t *interrupted, fsimg_output_budget_t *budget,
    fsimg_error_t *error)
{
	worker_supervisor_t state;
	fsimg_worker_request_t request;
	fsimg_worker_result_t result = { 0 };
	fsimg_status_t status;

	fsimg_error_clear(error);
	if (executable == NULL || produce == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_CONTEXT, "missing output executable or producer"));
	fsimg_worker_request_init(&request);
	request.kind = FSIMG_JOB_INFO;
	request.output_budget = budget;
	supervisor_init(&state, &request, &result, argument, standard_output, interrupted);
	state.produce = produce;
	state.producer_done = false;
	state.deadline = deadline;
	status = start_processes(&state, executable, standard_output, error);
	if (status == FSIMG_OK)
		status = supervise_operation(&state, error);
	return (finish_operation(&state, status, error));
}

void
fsimg_output_budget_init(fsimg_output_budget_t *budget, const fsimg_options_t *options)
{
	fsimg_options_t defaults;
	uint64_t entries, path, per_entry, variable;

	bzero(budget, sizeof(*budget));
	fsimg_options_init(&defaults);
	entries = options->max_entries == 0 ? defaults.max_entries : options->max_entries;
	path = options->max_path_bytes == 0 ? defaults.max_path_bytes : options->max_path_bytes;
	/* path is a uint32 field, so this per-entry calculation fits uint64. */
	per_entry = path * 6 + 4096;
	if (!fsimg_mul_u64(entries, per_entry, &variable)) {
		budget->limit_bytes = UINT64_MAX;
		return;
	}
	if (!fsimg_add_u64(variable, 1048576, &budget->limit_bytes))
		budget->limit_bytes = UINT64_MAX;
}
