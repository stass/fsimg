/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_WORKER_H
#define FSIMG_WORKER_H

#include <signal.h>

#include "protocol.h"

typedef enum fsimg_job_kind {
	FSIMG_JOB_INFO = 1,
	FSIMG_JOB_LIST,
	FSIMG_JOB_CAT,
	FSIMG_JOB_EXTRACT,
	FSIMG_JOB_VERIFY,
	FSIMG_JOB_CREATE,
	FSIMG_JOB_PLAN
} fsimg_job_kind_t;

typedef enum fsimg_sandbox_policy {
	FSIMG_SANDBOX_AUTO,
	FSIMG_SANDBOX_REQUIRED,
	FSIMG_SANDBOX_OFF
} fsimg_sandbox_policy_t;

typedef enum fsimg_confinement {
	FSIMG_CONFINEMENT_ACTIVE,
	FSIMG_CONFINEMENT_UNAVAILABLE,
	FSIMG_CONFINEMENT_DISABLED,
	FSIMG_CONFINEMENT_UNKNOWN
} fsimg_confinement_t;

#define FSIMG_JOB_RECURSIVE 0x01u
#define FSIMG_JOB_PAYLOAD 0x02u
#define FSIMG_JOB_VERBOSE 0x04u
/* Extraction omits entries of kinds the safe profile does not create. */
#define FSIMG_JOB_SKIP_UNSUPPORTED 0x08u

/* The job record's capability bits: which prepared descriptors the worker
 * receives. Scratch is optional for every kind; the others follow the kind.
 */
#define FSIMG_JOB_CAP_INPUT 0x01u
#define FSIMG_JOB_CAP_OUTPUT 0x02u
#define FSIMG_JOB_CAP_SCRATCH 0x04u
#define FSIMG_JOB_CAP_ROOTS 0x08u
/* Coordinator, at most one parser, and at most one output relay. */
#define FSIMG_CLI_PROCESS_BOUND 3u
/* Fixed sanitizer options reserve this status for runtime failures. */
#define FSIMG_WORKER_RUNTIME_EXIT 86

#define FSIMG_OUTPUT_ERROR_RESERVE 131072u
/* One coordinator-owned ledger spans processing and final reporting. Counts
 * describe queued text and confirmed stream bytes, not a sink's durability.
 * stdout_possible also covers the write-to-ack window on failed relays.
 */
typedef struct fsimg_output_budget {
	uint64_t	 limit_bytes;
	uint64_t	 text_bytes;
	uint64_t	 confirmed[2];
	bool		 stdout_possible;
	bool		 reporting;
} fsimg_output_budget_t;

void fsimg_output_budget_init(fsimg_output_budget_t *, const fsimg_options_t *);

/* Parent descriptors and selection are borrowed for run. No native struct or
 * pointer is transmitted: job.c owns the complete wire representation.
 * The trusted coordinator supplies fresh, disjoint private output and scratch
 * trees; neither is an ancestor of the other. Startup maps pipes/capabilities
 * to fixed child FDs and closes everything else.
 */
typedef struct fsimg_worker_request {
	fsimg_options_t	 limits;
	fsimg_write_options_t write;
	fsimg_job_kind_t	 kind;
	fsimg_sandbox_policy_t sandbox;
	/* The -F stack: on create its filesystem slot names the format; on
	 * read its named and empty slots are forced and the rest detected.
	 */
	fsimg_stack_t	 stack;
	/* The -o layer settings; reading uses only gpt_sector_bytes. */
	fsimg_layer_options_t layers;
	/* Read jobs: how the volume is selected. INDEX uses partition_index,
	 * NAME and TYPE partition_text, RANGE offset and length.
	 */
	fsimg_selector_kind_t selector;
	uint32_t	 partition_index;
	uint64_t	 offset;
	uint64_t	 length;
	uint64_t	 seconds;
	/* Coordinator-only absolute CLOCK_MONOTONIC deadline; never sent in JOB. */
	uint64_t	 deadline_milliseconds;
	/* Borrowed coordinator-only ledger; NULL uses a single-run local ledger. */
	fsimg_output_budget_t *output_budget;
	/* The selected member of a read job; create jobs use the roots. */
	const char	*selection;
	/* Create jobs: root_count opened roots, a directory or a regular file
	 * each, and the image path each populates, in command-line order.
	 */
	const int	*root_fds;
	const char *const *root_paths;
	uint32_t	 root_count;
	unsigned	 flags;
	uint32_t	 codepage;
	int		 input_fd;
	/* The staging directory of CREATE, or the destination of EXTRACT. */
	int		 output_fd;
	int		 scratch_fd;
	/* --partition name= or type= text; a GPT name holds at most 36 UTF-16
	 * units, so 111 bytes cover every name that can match.
	 */
	char		 partition_text[112];
	/* Decoder-owned backing only; write.label is the authoritative value.
	 * It holds the longest label any format stores, the volume
	 * information's, and its NUL.
	 */
	char		 label_storage[sizeof(((fsimg_volume_info_t *)0)->label)];
} fsimg_worker_request_t;

typedef struct fsimg_worker_result {
	uint64_t	 entries;
	uint64_t	 logical_bytes;
	uint64_t	 raw_bytes;
	uint64_t	 metadata_losses;
	uint64_t	 image_bytes;
	uint64_t	 skipped_entries;
	fsimg_confinement_t confinement; /* UNKNOWN until a valid READY arrives. */
} fsimg_worker_result_t;

/* One event can produce bounded text. Storage remains borrowed until the next
 * event; the supervisor sends it through its output relay. Callbacks perform
 * only bounded formatting/inspection, never writes to a potentially blocking
 * sink. Raw CAT bytes bypass this text interface.
 */
#define FSIMG_WORKER_TEXT_BYTES 131072u
typedef struct fsimg_worker_text {
	const void	*data;
	size_t		 length;
	bool		 diagnostic;
} fsimg_worker_text_t;

typedef fsimg_status_t (*fsimg_worker_event_fn_t)(void *, const fsimg_frame_t *,
    fsimg_worker_text_t *, fsimg_error_t *);

/* A producer returns one nonempty bounded span, DONE, or an error. The next
 * call follows acknowledgement, so its storage may be reused on that call.
 */
typedef fsimg_status_t (*fsimg_output_producer_fn_t)(void *,
    fsimg_worker_text_t *, fsimg_error_t *);

/* The command entry point ignores SIGPIPE before starting a worker. A nonzero
 * interrupted flag requests termination. A zero seconds limit has no wall-clock
 * deadline; a live silent child then requires an explicit interrupt. The output descriptor is borrowed
 * stdout for CAT bytes and formatted event text; -1 disables stdout text.
 * Diagnostic spans use borrowed stderr. Neither sink's status flags change.
 * The coordinator uses about 100 KiB of fixed stack storage and runs on the
 * command's main thread. Event callbacks perform bounded formatting only.
 * Events are validated before delivery; their storage is borrowed for the call.
 * run visibly reports unavailable/disabled confinement before acknowledging
 * parsing. Completion is returned in result only after successful child exit;
 * DONE is never delivered as an event callback.
 */
void fsimg_worker_request_init(fsimg_worker_request_t *);
fsimg_status_t fsimg_worker_run(const char *, const fsimg_worker_request_t *,
    fsimg_worker_event_fn_t, void *, int, const volatile sig_atomic_t *,
    fsimg_worker_result_t *, fsimg_error_t *);
int fsimg_worker_main(void);
int fsimg_output_main(void);
fsimg_status_t fsimg_output_run(const char *, fsimg_output_producer_fn_t, void *,
    int, uint64_t, const volatile sig_atomic_t *, fsimg_output_budget_t *, fsimg_error_t *);
/* A create or plan job reads source roots and carries one image path per
 * root; every other kind reads an image and carries its one selection.
 * These are the job's paths in wire order.
 */
bool fsimg_job_has_roots(fsimg_job_kind_t);
/* The filesystem the stack names, or "" when its slot is not named. */
const char *fsimg_job_format(const fsimg_worker_request_t *);
uint32_t fsimg_job_path_count(const fsimg_worker_request_t *);
const char *fsimg_job_path(const fsimg_worker_request_t *, uint32_t);
fsimg_status_t fsimg_job_paths_length(const fsimg_worker_request_t *,
    uint64_t *, fsimg_error_t *);
fsimg_status_t fsimg_job_encode(const fsimg_worker_request_t *, unsigned char *,
    fsimg_error_t *);
fsimg_status_t fsimg_job_decode(const unsigned char *, uint32_t,
    fsimg_worker_request_t *, uint64_t *, unsigned *, fsimg_error_t *);

/* Child FD numbers are a closed startup layout, not descriptor-opening
 * requests. A create job's roots follow the fixed descriptors in order:
 * root i is FSIMG_CHILD_ROOTS + i, and the job record states their count.
 */
#define FSIMG_CHILD_JOB 3
#define FSIMG_CHILD_CONTROL 4
#define FSIMG_CHILD_DATA 5
#define FSIMG_CHILD_INPUT 6
#define FSIMG_CHILD_OUTPUT 7
#define FSIMG_CHILD_SCRATCH 8
#define FSIMG_CHILD_CANCEL 9
#define FSIMG_CHILD_ROOTS 10

#endif
