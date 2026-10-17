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
/* Fixed sanitizer options reserve this status for runtime failures. */
#define FSIMG_WORKER_RUNTIME_EXIT 86

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
	uint64_t	 offset;
	uint64_t	 length;
	uint64_t	 seconds;
	const char	*selection;
	unsigned	 flags;
	uint32_t	 codepage;
	int		 input_fd;
	int		 output_fd;
	int		 scratch_fd;
	int		 source_fd;
	char		 format[16];
	/* Decoder-owned backing only; write.label is the authoritative value. */
	char		 label_storage[48];
} fsimg_worker_request_t;

typedef struct fsimg_worker_result {
	uint64_t	 entries;
	uint64_t	 logical_bytes;
	uint64_t	 raw_bytes;
	uint64_t	 metadata_losses;
	uint64_t	 image_bytes;
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
fsimg_status_t fsimg_job_encode(const fsimg_worker_request_t *, unsigned char *,
    fsimg_error_t *);
fsimg_status_t fsimg_job_decode(const unsigned char *, uint32_t,
    fsimg_worker_request_t *, uint64_t *, unsigned *, fsimg_error_t *);

/* Child FD numbers are a closed startup layout, not descriptor-opening requests. */
#define FSIMG_CHILD_JOB 3
#define FSIMG_CHILD_CONTROL 4
#define FSIMG_CHILD_DATA 5
#define FSIMG_CHILD_INPUT 6
#define FSIMG_CHILD_OUTPUT 7
#define FSIMG_CHILD_SCRATCH 8
#define FSIMG_CHILD_SOURCE 9
#define FSIMG_CHILD_CANCEL 10
#define FSIMG_CHILD_LAST FSIMG_CHILD_CANCEL

#endif
