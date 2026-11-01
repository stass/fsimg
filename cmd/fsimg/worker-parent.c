/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
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
#include <unistd.h>

#include "fsimg-endian.h"
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
 * create job of a partitioned disk or an encoded output (a container or a
 * compression) starts in WAIT_IMAGE too: its IMAGE record describes the
 * planned disk, a partitioned disk's one PARTITION record the volume's
 * place on it, and then its PLAN follows. A create job of a bare volume
 * written as it is starts in WAIT_HEADER, for its PLAN.
 * IN_ENTRY lasts from an ENTRY record to its ENTRY_END, which returns to
 * the state the record interrupted (entry_admitted says which those are).
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
	/* A stream's image: the input relay writes, the worker reads, and the
	 * coordinator closes both ends once both children hold them.
	 */
	CHANNEL_INPUT,
	CHANNEL_COUNT
} worker_channel_t;

/* The input relay runs only for a read job whose image is a stream; it is
 * otherwise recorded as already exited.
 */
typedef enum worker_process_role {
	PROCESS_PARSER,
	PROCESS_OUTPUT,
	PROCESS_INPUT,
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
	/* The state an entry record interrupted, restored at its ENTRY_END. */
	worker_output_state_t entry_return;
	uint64_t	 deadline;
	uint64_t	 name_length;
	uint64_t	 name_bytes;
	uint64_t	 entry_size;
	uint64_t	 entry_kind;
	uint64_t	 entry_count;
	/* Of entry_count, the entries neither files nor directories, which
	 * an extraction can only skip (names_match_extraction).
	 */
	uint64_t	 named_other_kinds;
	/* A create job's PLAN entries, which its DONE repeats. */
	uint64_t	 planned_entries;
	/* Metadata loss examples received; a worker sends at most
	 * FSIMG_WIRE_LOSS_EVENTS, and DONE carries the exact count.
	 */
	unsigned	 loss_examples;
	uint64_t	 output_limit;
	uint64_t	 volume_limit;
	/* The read job's input file size, or the create job's planned disk. */
	uint64_t	 disk_bytes;
	/* The create job's planned output: the most bytes its image takes. */
	uint64_t	 planned_output;
	/* Raw data bytes are expected from now on, at most raw_bound of them:
	 * CAT's selected file after its entry, a streamed image after its plan.
	 */
	uint64_t	 raw_bound;
	bool		 raw_open;
	uint64_t	 table_slots;	/* From the IMAGE record */
	uint64_t	 selected_slot;	/* From the IMAGE record */
	uint64_t	 last_partition; /* Slot of the last PARTITION record */
	bool		 raw_eof;
	bool		 stdout_available;
	bool		 producer_done;
} worker_supervisor_t;

static fsimg_status_t worker_error(fsimg_error_t *, const char *);
static fsimg_status_t queue_text(worker_supervisor_t *, const fsimg_worker_text_t *,
    bool, fsimg_error_t *);
static fsimg_status_t deliver_event(worker_supervisor_t *, fsimg_error_t *);
static bool bound_volume(worker_supervisor_t *);
static bool streaming(const fsimg_worker_request_t *);
static bool reading_stream(const fsimg_worker_request_t *);
static const char *slot_name(const fsimg_worker_request_t *, fsimg_layer_kind_t);
static bool layers_expected(const fsimg_worker_request_t *,
    const unsigned char *);
static bool read_disk_expected(const worker_supervisor_t *,
    const unsigned char *);
static bool output_length_expected(const worker_supervisor_t *, uint64_t);
static bool image_frame_expected(const worker_supervisor_t *,
    const fsimg_frame_t *);
static worker_output_state_t first_output_state(const fsimg_worker_request_t *);
static bool entry_admitted(const worker_supervisor_t *);
static bool names_match_plan(const worker_supervisor_t *, uint64_t);
static bool names_match_extraction(const worker_supervisor_t *, uint64_t,
    uint64_t);
static bool plan_frame_expected(const worker_supervisor_t *,
    const fsimg_frame_t *);
static bool image_length_expected(const worker_supervisor_t *, uint64_t);
static bool partition_frame_expected(const worker_supervisor_t *,
    const fsimg_frame_t *);
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
static fsimg_status_t observe_process(worker_process_t *, bool,
    fsimg_error_t *);
static fsimg_status_t supervise_operation(worker_supervisor_t *, fsimg_error_t *);
static void supervisor_init(worker_supervisor_t *, const fsimg_worker_request_t *,
    fsimg_worker_result_t *, void *, int, const volatile sig_atomic_t *);
static fsimg_status_t finish_operation(worker_supervisor_t *, fsimg_status_t,
    fsimg_error_t *);
static void stop_process(worker_process_t *);
static void close_channel(int *);
static void cleanup_operation(worker_supervisor_t *);

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

/* Queue one span for the relay. Formatted text is counted in the ledger;
 * raw payload is not text, and raw_bound and output_limit bound it instead.
 */
static fsimg_status_t
queue_text(worker_supervisor_t *state, const fsimg_worker_text_t *text,
    bool raw, fsimg_error_t *error)
{
	worker_relay_t *relay = &state->relay;

	if (text->length == 0)
		return (FSIMG_OK);
	if (text->data == NULL || text->length > FSIMG_WORKER_TEXT_BYTES ||
	    relay->phase != RELAY_IDLE)
		return (worker_error(error, "invalid or overlapping worker text output"));
	if (!raw && !fsimg_add_u64(state->budget->text_bytes, text->length,
	    &state->budget->text_bytes))
		return (worker_error(error, "formatted output count overflows"));
	relay->text = *text;
	fsimg_store_le64(relay->header + FSIMG_RELAY_STREAM, text->diagnostic ? 2 : 1);
	fsimg_store_le64(relay->header + FSIMG_RELAY_LENGTH, text->length);
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

/*
 * A read job's volume lies within the disk its IMAGE record states: the
 * range --offset and --length name, or else the whole disk. That bounds
 * the INFO record's volume and, without -l output, what x -O may write.
 * False when the range does not fit the disk. The library's range
 * selection applies the same rule (a zero length runs to the disk's end);
 * the coordinator repeats it so its bound does not depend on the worker
 * having applied it.
 */
static bool
bound_volume(worker_supervisor_t *state)
{
	const fsimg_worker_request_t *request = state->request;

	if (request->offset > state->disk_bytes)
		return (false);
	state->volume_limit = request->length != 0 ? request->length :
	    state->disk_bytes - request->offset;
	if (state->volume_limit > state->disk_bytes - request->offset)
		return (false);
	if (request->limits.max_output_bytes == 0)
		state->output_limit = state->volume_limit;
	return (true);
}

/* A creation that writes its image to the raw data channel, not staging. */
static bool
streaming(const fsimg_worker_request_t *request)
{
	return ((request->flags & FSIMG_JOB_STDOUT) != 0);
}

/* A read job whose image arrives on the input pipe. */
static bool
reading_stream(const fsimg_worker_request_t *request)
{
	return (request->input.kind == FSIMG_INPUT_STREAM);
}

/* The name a create request gives a layer slot, "" when it names none. */
static const char *
slot_name(const fsimg_worker_request_t *request, fsimg_layer_kind_t kind)
{
	const fsimg_stack_slot_t *slot = &request->stack.slots[kind];

	return (slot->state == FSIMG_SLOT_NAMED ? slot->name : "");
}

/*
 * Whether an IMAGE record names the layers its request forces. A named
 * scheme or container slot reports that layer and an emptied one none; a
 * create job writes an unspecified slot empty, while a read job detects
 * it, and then any layer is its answer. The compression is likewise the
 * one the request names, which a read job leaves empty for detection.
 */
static bool
layers_expected(const fsimg_worker_request_t *request,
    const unsigned char *raw)
{
	const fsimg_layer_kind_t slots[] = { FSIMG_LAYER_SCHEME,
	    FSIMG_LAYER_CONTAINER };
	const size_t fields[] = { FSIMG_WIRE_IMAGE_SCHEME,
	    FSIMG_WIRE_IMAGE_CONTAINER };
	bool reading;

	reading = !fsimg_job_has_roots(request->kind);
	for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
		if (reading && request->stack.slots[slots[i]].state ==
		    FSIMG_SLOT_UNSPECIFIED)
			continue;
		if (strcmp((const char *)raw + fields[i],
		    slot_name(request, slots[i])) != 0)
			return (false);
	}
	if (reading && request->compression[0] == '\0')
		return (true);
	return (strcmp((const char *)raw + FSIMG_WIRE_IMAGE_COMPRESSION,
	    request->compression) == 0);
}

/*
 * Whether the file and disk a read job's IMAGE record reports fit its
 * input. The file is the input file the coordinator opened, of a length
 * it knows; a stream or a compressed input is peeled from the spool,
 * whose length only the worker sees and the spool limit bounds. Without a
 * container the disk is the file (fsimg_wire_image_frame_valid). The
 * container's registry entry says what bounds its disk: a disk that is
 * its file less a fixed trailer is checked exactly, and a spooled
 * container's disk, decoded into the spool, is held to the spool limit.
 * Any other container's disk is accepted at the length its own tables
 * declare; specs/design-security.md (Coordinator and worker) says why no
 * ceiling from the file would hold.
 */
static bool
read_disk_expected(const worker_supervisor_t *state, const unsigned char *raw)
{
	const fsimg_container_ops_t *container;
	uint64_t file, disk, spool;
	bool file_in_spool;

	file = fsimg_load_le64(raw + FSIMG_WIRE_IMAGE_FILE);
	disk = fsimg_load_le64(raw + FSIMG_WIRE_IMAGE_DISK);
	spool = state->request->limits.spool_bytes;
	file_in_spool = reading_stream(state->request) ||
	    raw[FSIMG_WIRE_IMAGE_COMPRESSION] != '\0';
	if (file_in_spool ? file > spool : file != state->disk_bytes)
		return (false);
	if (raw[FSIMG_WIRE_IMAGE_CONTAINER] == '\0')
		return (true);
	/* fsimg_wire_image_frame_valid found the named container. */
	container = fsimg_layer_find((const char *)raw +
	    FSIMG_WIRE_IMAGE_CONTAINER)->container;
	if (container->trailer_bytes != 0)
		return (file >= container->trailer_bytes &&
		    disk == file - container->trailer_bytes);
	return (!container->spooled || disk <= spool);
}

/*
 * An IMAGE record names the layers the request forces (layers_expected).
 * A read job's describes the input it opened: its disk is the input
 * file's size, or the length a container's tables declare (see
 * read_disk_expected). A create job's describes the disk it plans, its
 * own file, and selects the one partition when a scheme is named. Either
 * way the handler then takes the disk length as the bound of the
 * partitions, the volume and the image.
 * Only i reports a disk without a volume; only W verifies layers.
 */
static bool
image_frame_expected(const worker_supervisor_t *state,
    const fsimg_frame_t *frame)
{
	const unsigned char *raw = frame->payload;
	fsimg_job_kind_t kind = state->request->kind;
	uint64_t flags;

	if (state->output_state != WORKER_WAIT_IMAGE ||
	    frame->length != FSIMG_WIRE_IMAGE_BYTES ||
	    !fsimg_wire_image_frame_valid(frame))
		return (false);
	flags = fsimg_load_le64(raw + FSIMG_WIRE_IMAGE_FLAGS);
	if (((flags & FSIMG_WIRE_IMAGE_UNSELECTED) != 0 && kind != FSIMG_JOB_INFO) ||
	    ((flags & FSIMG_WIRE_IMAGE_VERIFIED) != 0 && kind != FSIMG_JOB_VERIFY))
		return (false);
	if (!layers_expected(state->request, raw))
		return (false);
	if (kind < FSIMG_JOB_CREATE)
		return (read_disk_expected(state, raw));
	return (fsimg_load_le64(raw + FSIMG_WIRE_IMAGE_FILE) ==
	    fsimg_load_le64(raw + FSIMG_WIRE_IMAGE_DISK) &&
	    fsimg_load_le64(raw + FSIMG_WIRE_IMAGE_SELECTED) ==
	    (raw[FSIMG_WIRE_IMAGE_SCHEME] != '\0' ? 1 : 0));
}

/* A read job, and a create job of a partitioned disk or an encoded output,
 * reports its image first; a bare volume's creation starts at its PLAN.
 */
static worker_output_state_t
first_output_state(const fsimg_worker_request_t *request)
{
	return (request->kind < FSIMG_JOB_CREATE ||
	    slot_name(request, FSIMG_LAYER_SCHEME)[0] != '\0' ||
	    fsimg_job_encoded(request) ? WORKER_WAIT_IMAGE : WORKER_WAIT_HEADER);
}

/*
 * Entry records arrive by job and state: a listing's and a member output's
 * once the volume is reported; under -v a creation's while it plans, before
 * its planned disk's IMAGE and PARTITION records and its PLAN, so in the
 * state the job starts in, and an extraction's after its INFO. No other
 * job sends them, a creation or extraction not asked for names included.
 */
static bool
entry_admitted(const worker_supervisor_t *state)
{
	const fsimg_worker_request_t *request = state->request;
	bool names = (request->flags & FSIMG_JOB_NAMES) != 0;

	switch (request->kind) {
	case FSIMG_JOB_LIST:
	case FSIMG_JOB_CAT:
		return (state->output_state == WORKER_OUTPUT_READY);
	case FSIMG_JOB_EXTRACT:
		return (names && state->output_state == WORKER_OUTPUT_READY);
	case FSIMG_JOB_CREATE:
	case FSIMG_JOB_PLAN:
		return (names && state->output_state == first_output_state(request));
	default:
		return (false);
	}
}

/* Names and counts agree. A creation named every planned entry before its
 * PLAN, so the plan's entries are the names.
 */
static bool
names_match_plan(const worker_supervisor_t *state, uint64_t entries)
{
	return ((state->request->flags & FSIMG_JOB_NAMES) == 0 ||
	    entries == state->entry_count);
}

/* An extraction named each file and directory it completed and each entry
 * of another kind it skipped.
 */
static bool
names_match_extraction(const worker_supervisor_t *state, uint64_t entries,
    uint64_t skipped)
{
	return ((state->request->flags & FSIMG_JOB_NAMES) == 0 ||
	    (entries == state->entry_count - state->named_other_kinds &&
	    skipped == state->named_other_kinds));
}

/*
 * A create job's PLAN: a planned disk holds its volume, which the one
 * PARTITION record placed when a scheme is named, and without a container
 * or compression the image is that disk, or the bare volume, exactly; an
 * encoding's largest length strictly exceeds the disk it holds, since
 * each layer adds its own metadata (the output_bytes contract of
 * fsimg_plan_info_t). The disk and that largest length are the worker's
 * own, checked here only against each other: the disk follows from
 * sources only the worker reads, so a ceiling derived from it would bound
 * nothing (specs/design-security.md, Coordinator and worker).
 */
static bool
plan_frame_expected(const worker_supervisor_t *state,
    const fsimg_frame_t *frame)
{
	const fsimg_worker_request_t *request = state->request;
	uint64_t volume, output;
	bool scheme, encoded;

	if (request->kind < FSIMG_JOB_CREATE ||
	    state->output_state != WORKER_WAIT_HEADER ||
	    !fsimg_wire_plan_frame_valid(frame))
		return (false);
	volume = fsimg_load_le64(frame->payload + FSIMG_WIRE_PLAN_VOLUME);
	output = fsimg_load_le64(frame->payload + FSIMG_WIRE_PLAN_OUTPUT);
	scheme = slot_name(request, FSIMG_LAYER_SCHEME)[0] != '\0';
	encoded = fsimg_job_encoded(request);
	if (scheme && state->last_partition == 0)
		return (false);
	if (!scheme && !encoded)
		return (output == volume);
	if (volume > state->disk_bytes)
		return (false);
	if (encoded)
		return (output > state->disk_bytes);
	return (output == state->disk_bytes);
}

/*
 * A created image's length, staged or streamed: the planned image exactly,
 * or when encoded no more than its largest encoding.
 */
static bool
output_length_expected(const worker_supervisor_t *state, uint64_t length)
{
	if (!fsimg_job_encoded(state->request))
		return (length == state->planned_output);
	return (length != 0 && length <= state->planned_output);
}

/*
 * DONE's image length: the staged file's for a creation to a file; zero
 * for every other job, a streamed image included, whose bytes the relay
 * counts instead.
 */
static bool
image_length_expected(const worker_supervisor_t *state, uint64_t length)
{
	const fsimg_worker_request_t *request = state->request;

	if (!fsimg_job_needs_staging(request))
		return (length == 0);
	return (output_length_expected(state, length));
}

/* i lists every used slot, also when no volume was selected; a create job
 * reports the one partition its IMAGE record selected.
 */
static bool
partition_frame_expected(const worker_supervisor_t *state,
    const fsimg_frame_t *frame)
{
	fsimg_job_kind_t kind = state->request->kind;
	uint64_t index;

	if (!fsimg_wire_partition_frame_valid(frame, state->table_slots,
	    state->disk_bytes))
		return (false);
	index = fsimg_load_le64(frame->payload + FSIMG_WIRE_PARTITION_INDEX);
	if (kind == FSIMG_JOB_INFO)
		return ((state->output_state == WORKER_WAIT_HEADER ||
		    state->output_state == WORKER_UNSELECTED) &&
		    index > state->last_partition);
	return (kind >= FSIMG_JOB_CREATE &&
	    state->output_state == WORKER_WAIT_HEADER &&
	    state->last_partition == 0 && index == state->selected_slot);
}

/*
 * Validate one worker frame before it is formatted. Formatted text has no
 * ceiling of its own, so every frame that yields text must be bounded here,
 * and a new one needs a bound too: READY, IMAGE, INFO, PLAN, VERIFY, ERROR
 * and DONE arrive once by state; PARTITION records rise strictly within the
 * table's slots; ENTRY counts against the entries limit and NAME against the
 * path limit; LOSS stops at FSIMG_WIRE_LOSS_EVENTS; progress text is sampled
 * at power-of-two milestones; raw bytes stop at raw_bound.
 */
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
		    fsimg_load_le64(raw) > 2)
			return (worker_error(error, "worker did not send valid readiness"));
		state->result->confinement = (fsimg_confinement_t)fsimg_load_le64(raw);
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
		if (!image_frame_expected(state, frame))
			return (worker_error(error, "invalid worker image information"));
		flags = fsimg_load_le64(raw + FSIMG_WIRE_IMAGE_FLAGS);
		state->table_slots = fsimg_load_le64(raw + FSIMG_WIRE_IMAGE_SLOTS);
		state->selected_slot = fsimg_load_le64(raw + FSIMG_WIRE_IMAGE_SELECTED);
		/* The disk, planned or peeled, bounds what follows it. */
		state->disk_bytes = fsimg_load_le64(raw + FSIMG_WIRE_IMAGE_DISK);
		if (kind < FSIMG_JOB_CREATE && !bound_volume(state))
			return (worker_error(error, "worker image cannot hold the selected range"));
		state->output_state = (flags & FSIMG_WIRE_IMAGE_UNSELECTED) != 0 ?
		    WORKER_UNSELECTED : WORKER_WAIT_HEADER;
		break;
	case FSIMG_FRAME_PARTITION:
		if (!partition_frame_expected(state, frame))
			return (worker_error(error, "invalid worker partition record"));
		state->last_partition = fsimg_load_le64(raw + FSIMG_WIRE_PARTITION_INDEX);
		break;
	case FSIMG_FRAME_INFO:
		if (kind >= FSIMG_JOB_CREATE || state->output_state != WORKER_WAIT_HEADER ||
		    !fsimg_wire_info_frame_valid(frame, state->volume_limit))
			return (worker_error(error, "invalid worker volume information"));
		state->output_state = WORKER_OUTPUT_READY;
		break;
	case FSIMG_FRAME_PLAN:
		if (!plan_frame_expected(state, frame))
			return (worker_error(error, "invalid worker image plan"));
		state->planned_entries = fsimg_load_le64(raw + FSIMG_WIRE_PLAN_ENTRIES);
		if (!names_match_plan(state, state->planned_entries))
			return (worker_error(error, "worker plan entry count differs from its names"));
		state->output_state = WORKER_OUTPUT_READY;
		state->planned_output = fsimg_load_le64(raw + FSIMG_WIRE_PLAN_OUTPUT);
		if (streaming(state->request)) {
			/* A stream's only bound is the plan, unless -l output
			 * sets a lower one.
			 */
			if (state->output_limit == 0)
				state->output_limit = state->planned_output;
			state->raw_open = true;
			state->raw_bound = state->planned_output;
		}
		break;
	case FSIMG_FRAME_ENTRY:
		path_limit = state->request->limits.max_path_bytes == 0 ? 65536 :
		    state->request->limits.max_path_bytes;
		if (!entry_admitted(state) ||
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
		state->name_length = fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_NAME_LENGTH);
		state->entry_size = fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_LENGTH);
		state->entry_kind = fsimg_load_le64(raw + FSIMG_WIRE_ENTRY_KIND);
		state->name_bytes = 0;
		state->entry_return = state->output_state;
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
		state->output_state = state->entry_return;
		state->entry_count++;
		if (state->entry_kind != FSIMG_ENTRY_FILE &&
		    state->entry_kind != FSIMG_ENTRY_DIRECTORY)
			state->named_other_kinds++;
		if (kind == FSIMG_JOB_CAT) {
			state->raw_open = true;
			state->raw_bound = state->entry_size;
		}
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
		    fsimg_load_le64(raw + FSIMG_WIRE_LOSS_LENGTH) != frame->length - FSIMG_WIRE_LOSS_BYTES ||
		    fsimg_load_le64(raw + FSIMG_WIRE_LOSS_TRUNCATED) > 1 ||
		    fsimg_load_le64(raw + FSIMG_WIRE_LOSS_FIELDS) == 0 ||
		    (fsimg_load_le64(raw + FSIMG_WIRE_LOSS_FIELDS) & ~(uint64_t)FSIMG_META_KNOWN) != 0)
			return (worker_error(error, "invalid worker metadata loss frame"));
		if (state->loss_examples == FSIMG_WIRE_LOSS_EVENTS)
			return (worker_error(error,
			    "worker sent more metadata loss examples than allowed"));
		state->loss_examples++;
		break;
	case FSIMG_FRAME_DONE:
		if ((state->output_state != (kind == FSIMG_JOB_VERIFY ?
		    WORKER_VERIFIED : WORKER_OUTPUT_READY) &&
		    state->output_state != WORKER_UNSELECTED) ||
		    frame->length != FSIMG_WIRE_DONE_BYTES ||
		    (kind == FSIMG_JOB_CAT && state->entry_count != 1))
			return (worker_error(error, "worker completion arrived before its result"));
		count = fsimg_load_le64(raw + FSIMG_WIRE_DONE_ENTRIES);
		if ((kind == FSIMG_JOB_LIST || kind == FSIMG_JOB_CAT) &&
		    count != state->entry_count)
			return (worker_error(error, "worker entry completion count differs"));
		state->result->entries = count;
		state->result->logical_bytes = fsimg_load_le64(raw + FSIMG_WIRE_DONE_LOGICAL);
		state->result->metadata_losses = fsimg_load_le64(raw + FSIMG_WIRE_DONE_LOSSES);
		state->result->skipped_entries = fsimg_load_le64(raw + FSIMG_WIRE_DONE_SKIPPED);
		if (kind == FSIMG_JOB_EXTRACT && !names_match_extraction(state,
		    count, state->result->skipped_entries))
			return (worker_error(error, "worker extraction counts differ from its names"));
		/* A creation's completion repeats its plan's count. */
		if (kind >= FSIMG_JOB_CREATE && count != state->planned_entries)
			return (worker_error(error, "worker creation count differs from its plan"));
		if ((kind != FSIMG_JOB_EXTRACT ||
		    (state->request->flags & FSIMG_JOB_SKIP_UNSUPPORTED) == 0) &&
		    state->result->skipped_entries != 0)
			return (worker_error(error, "worker skipped entries it was not asked to skip"));
		/* Only a listing, a verification and an extraction asked to skip
		 * pass an omitted entry and succeed.
		 */
		state->result->omitted_entries = fsimg_load_le64(raw + FSIMG_WIRE_DONE_OMITTED);
		if (kind != FSIMG_JOB_LIST && kind != FSIMG_JOB_VERIFY &&
		    (kind != FSIMG_JOB_EXTRACT ||
		    (state->request->flags & FSIMG_JOB_SKIP_UNSUPPORTED) == 0) &&
		    state->result->omitted_entries != 0)
			return (worker_error(error, "worker omitted entries it could not omit"));
		if (!image_length_expected(state,
		    fsimg_load_le64(raw + FSIMG_WIRE_DONE_IMAGE)) ||
		    fsimg_load_le64(raw + FSIMG_WIRE_DONE_RAW) != (kind == FSIMG_JOB_CAT ?
		    state->entry_size : 0))
			return (worker_error(error, "worker raw completion length differs"));
		state->result->image_bytes = fsimg_load_le64(raw + FSIMG_WIRE_DONE_IMAGE);
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
	uint64_t started;
	fsimg_status_t status;

	if (request->deadline_milliseconds != 0)
		state->deadline = request->deadline_milliseconds;
	else if (request->seconds != 0) {
		started = fsimg_host_clock();
		if (started == FSIMG_HOST_CLOCK_FAILED)
			return (fsimg_error_set(error, FSIMG_EIO, errno,
			    FSIMG_OP_CONTEXT, "monotonic clock query failed"));
		if (request->seconds > (UINT64_MAX - started) / 1000)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_CONTEXT, "worker deadline exceeds clock range"));
		state->deadline = started + request->seconds * 1000;
	}
	state->output_limit = request->limits.max_output_bytes;
	if (request->input_fd != -1) {
		status = fsimg_input_check(request->input_fd, &request->input, error);
		if (status != FSIMG_OK)
			return (status);
		/* The disk is the file until the IMAGE record says a container
		 * yields another; the volume's bound follows from it there.
		 */
		state->disk_bytes = request->input.length;
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
		    fsimg_host_deadline(2000);
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

/* Copy the next bytes of the job's path sequence, the members of a read job
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
			 * are nonblocking. Borrowed sink flags never change, and the
			 * input pipe belongs to the children alone.
			 */
			if (role == CHANNEL_INPUT)
				continue;
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
	int *parser, output[5], input[4];
	unsigned parser_count;
	int null_fd, code, saved, fd;

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
	for (fsimg_capability_t c = 0; c < FSIMG_CAPABILITY_COUNT; c++) {
		fd = fsimg_job_capability_fd(request, c);
		if (fd != -1)
			parser[FSIMG_CHILD_SLOT(c)] = fd;
	}
	if (reading_stream(request))
		parser[FSIMG_CHILD_INPUT] = state->channels[CHANNEL_INPUT][0];
	for (uint32_t i = 0; i < request->root_count; i++)
		parser[FSIMG_CHILD_ROOTS + i] = request->root_fds[i];
	code = fsimg_host_spawn_worker(executable, parser, parser_count,
	    &state->processes[PROCESS_PARSER].pid);
	saved = errno;
	free(parser);
	if (code == -1) {
		close(null_fd);
		return (fsimg_error_set(error, FSIMG_EIO, saved,
		    FSIMG_OP_CONTEXT, "fresh worker execution failed"));
	}
	/* The input relay holds standard input and the pipe's write end. */
	if (reading_stream(request)) {
		input[STDIN_FILENO] = STDIN_FILENO;
		input[STDOUT_FILENO] = input[STDERR_FILENO] = null_fd;
		input[3] = state->channels[CHANNEL_INPUT][1];
		code = fsimg_host_spawn_input(executable, input, 4,
		    &state->processes[PROCESS_INPUT].pid);
		saved = errno;
		if (code == -1) {
			close(null_fd);
			return (fsimg_error_set(error, FSIMG_EIO, saved,
			    FSIMG_OP_CONTEXT, "input relay execution failed"));
		}
		state->processes[PROCESS_INPUT].exited = false;
		state->processes[PROCESS_INPUT].channel_eof = false;
	}
	close(null_fd);
	close_channel(&state->channels[CHANNEL_INPUT][0]);
	close_channel(&state->channels[CHANNEL_INPUT][1]);

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
	if (!state->raw_open)
		return (worker_error(error, "unexpected worker raw output"));
	observed = UINT64_MAX;
	if (!fsimg_add_u64(relay->received, (uint64_t)count, &observed) ||
	    observed > state->output_limit)
		return (fsimg_limit_error(error, FSIMG_RESOURCE_OUTPUT,
		    state->output_limit, observed));
	if (observed > state->raw_bound)
		return (worker_error(error, state->request->kind == FSIMG_JOB_CAT ?
		    "worker raw output exceeds its selected file" :
		    "worker image stream exceeds its plan"));
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
	confirmed[0] = fsimg_load_le64(relay->report + FSIMG_RELAY_STDOUT);
	confirmed[1] = fsimg_load_le64(relay->report + FSIMG_RELAY_STDERR);
	failure = fsimg_load_le64(relay->report + FSIMG_RELAY_ERRNO);
	marker = fsimg_load_le64(relay->report + FSIMG_RELAY_MARKER);
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
	if (state->request->kind == FSIMG_JOB_CAT || streaming(state->request))
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
 * bounded exit grace for every role. Pending output can delay channel drainage
 * after process exit, so exit alone does not start that grace.
 * Queued control errors are drained before final exit status is interpreted.
 * A child that has not both exited and closed its channel when its grace
 * ends is an error, unless the caller marks it stoppable: then it is
 * stopped, and supervision goes on.
 */
static fsimg_status_t
observe_process(worker_process_t *process, bool stoppable, fsimg_error_t *error)
{
	pid_t waited;

	if (!process->exited) {
		waited = waitpid(process->pid, &process->status, WNOHANG);
		if (waited == process->pid)
			process->exited = true;
		else if (waited == -1 && errno != EINTR)
			return (worker_error(error, "child process supervision failed"));
	}
	if (process->exited && process->channel_eof)
		return (FSIMG_OK);
	if (process->channel_eof && process->finish_deadline == 0)
		process->finish_deadline = fsimg_host_deadline(2000);
	if (!fsimg_host_expired(process->finish_deadline))
		return (FSIMG_OK);
	if (!stoppable)
		return (worker_error(error, "child exit and channel closure did not complete"));
	stop_process(process);
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

	for (;;) {
		if ((state->interrupted != NULL && *state->interrupted != 0) ||
		    fsimg_host_expired(state->deadline))
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

		/* The input relay's only channel is the worker's input pipe,
		 * spent once the worker exits. A relay still running then, blocked
		 * on a slow standard input, can deliver nothing more: after the
		 * exit grace any child gets, it is stopped rather than reported
		 * as a stuck child. A worker that failed is then reported by its
		 * own frames; a worker that claimed success still fails below,
		 * as a stream read succeeds only after the relay exited
		 * successfully.
		 */
		if (state->processes[PROCESS_PARSER].exited)
			state->processes[PROCESS_INPUT].channel_eof = true;
		finished = state->raw_eof;
		for (unsigned role = 0; role < PROCESS_COUNT; role++) {
			status = observe_process(&state->processes[role],
			    role == PROCESS_INPUT, error);
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
		/* Hold raw bytes in their kernel pipe until the complete CAT entry
		 * or the streamed image's plan; any other job's raw bytes are an
		 * error. A pending text/raw record also retains its one borrowed
		 * buffer.
		 */
		if (!state->raw_eof && relay->phase == RELAY_IDLE &&
		    (state->raw_open || (state->request->kind != FSIMG_JOB_CAT &&
		    !streaming(state->request))))
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
	/* A relay that failed may have ended the stream early, whatever the
	 * worker made of the bytes it had, and one stopped after the worker
	 * exited never finished. A relay that never ran is recorded as
	 * exited with status zero.
	 */
	if (!WIFEXITED(state->processes[PROCESS_INPUT].status) ||
	    WEXITSTATUS(state->processes[PROCESS_INPUT].status) != 0)
		return (worker_error(error, "standard input could not be relayed"));
	for (unsigned role = 0; role < PROCESS_COUNT; role++) {
		if (!WIFEXITED(state->processes[role].status) ||
		    WEXITSTATUS(state->processes[role].status) != 0)
			return (worker_error(error, "child process exited unsuccessfully"));
	}
	/* Every raw byte received was confirmed written, CAT received its
	 * whole file, and a stream the planned length: exactly, without a
	 * container or compression, else no more than the largest
	 * encoding. A truncated encoding within that bound is caught only by
	 * the worker's own failure.
	 */
	if (state->phase != WORKER_COMPLETE || !relay->final ||
	    state->result->raw_bytes != relay->received ||
	    (state->request->kind == FSIMG_JOB_CAT &&
	    relay->received != state->entry_size) ||
	    (streaming(state->request) &&
	    !output_length_expected(state, relay->received)))
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
	stopping = fsimg_host_deadline(2000);
	for (;;) {
		waited = waitpid(process->pid, &process->status, WNOHANG);
		if (waited == process->pid) {
			process->exited = true;
			return;
		}
		if (waited == -1 && errno != EINTR)
			return;
		if (fsimg_host_expired(stopping)) {
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
	state->output_state = first_output_state(request);
	if (request->output_budget == NULL)
		state->budget = &state->local_budget;
	else
		state->budget = request->output_budget;
	for (unsigned role = 0; role < CHANNEL_COUNT; role++)
		state->channels[role][0] = state->channels[role][1] = -1;
	for (unsigned role = 0; role < PROCESS_COUNT; role++)
		state->processes[role].pid = -1;
	state->processes[PROCESS_INPUT].exited = true;
	state->processes[PROCESS_INPUT].channel_eof = true;
}

static fsimg_status_t
finish_operation(worker_supervisor_t *state, fsimg_status_t status,
    fsimg_error_t *error)
{
	cleanup_operation(state);
	if (state->relay.forwarded[0] != 0)
		state->budget->stdout_possible = true;
	/* An input relay that could not read standard input ended the stream
	 * early; that, not whatever the worker concluded from the bytes it
	 * had, is the cause. The relay closes the worker's pipe only by
	 * exiting, so by the time the worker can have seen that early end,
	 * the relay's status is final.
	 */
	if (state->processes[PROCESS_INPUT].exited &&
	    WIFEXITED(state->processes[PROCESS_INPUT].status) &&
	    WEXITSTATUS(state->processes[PROCESS_INPUT].status) ==
	    FSIMG_INPUT_UNREADABLE_EXIT)
		status = fsimg_error_set(error, FSIMG_EIO, 0, FSIMG_OP_IO,
		    "standard input could not be read");
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
	status = setup_channels(&state, error);
	if (status == FSIMG_OK)
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
	status = setup_channels(&state, error);
	if (status == FSIMG_OK)
		status = start_processes(&state, executable, standard_output, error);
	if (status == FSIMG_OK)
		status = supervise_operation(&state, error);
	return (finish_operation(&state, status, error));
}

