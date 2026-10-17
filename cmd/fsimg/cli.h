/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_CLI_H
#define FSIMG_CLI_H

#include <limits.h>

#include "worker.h"

typedef enum fsimg_cli_command {
	FSIMG_CLI_PROCESS,
	FSIMG_CLI_FORMATS,
	FSIMG_CLI_HELP,
	FSIMG_CLI_VERSION
} fsimg_cli_command_t;

/* Operand strings borrow argv or the environment until main returns. */
typedef struct fsimg_cli_options {
	fsimg_worker_request_t request;
	fsimg_cli_command_t command;
	const char	*name;
	const char	*input;
	const char	*output;
	const char	*source;
	const char	*scratch;
	bool		 json;
} fsimg_cli_options_t;

typedef struct fsimg_cli_paths {
	int		 parent;
	int		 scratch_parent;
	char		 basename[NAME_MAX + 1];
	char		 staging_name[96];
	char		 scratch_name[96];
	char		 retained_output[PATH_MAX];
	char		 retained_scratch[PATH_MAX];
	bool		 staging_created;
	bool		 scratch_created;
	bool		 extraction_created;
	bool		 published;
	bool		 retain_staging;
} fsimg_cli_paths_t;

typedef enum fsimg_cli_output_phase {
	FSIMG_CLI_OUTPUT_RECORDS,
	FSIMG_CLI_OUTPUT_CAPABILITIES,
	FSIMG_CLI_OUTPUT_SUMMARY,
	FSIMG_CLI_OUTPUT_DONE
} fsimg_cli_output_phase_t;

typedef struct fsimg_cli_display {
	const fsimg_cli_options_t *options;
	const fsimg_worker_result_t *result;
	const fsimg_cli_paths_t *paths;
	const fsimg_error_t *failure;
	char		 buffer[FSIMG_WORKER_TEXT_BYTES];
	size_t		 used;
	size_t		 format_cursor;
	/* RECORDS is also the initial state of a single-record command. */
	fsimg_cli_output_phase_t phase;
	unsigned char	 utf8[4];
	unsigned	 utf8_used;
	unsigned	 utf8_needed;
	unsigned	 progress_levels[3];
	uint64_t	 progress_records;
	fsimg_validation_scope_t scope;
	bool		 entry_open;
	bool		 overflow;
	bool		 progress_started;
} fsimg_cli_display_t;

fsimg_status_t fsimg_cli_number(const char *, bool, uint64_t *, fsimg_error_t *);
fsimg_status_t fsimg_cli_epoch(const char *, int64_t *, fsimg_error_t *);
fsimg_status_t fsimg_cli_parse(int, char **, fsimg_cli_options_t *, fsimg_error_t *);
uint64_t fsimg_cli_clock(void);
fsimg_status_t fsimg_cli_prepare(fsimg_cli_options_t *, fsimg_cli_paths_t *,
    fsimg_error_t *);
fsimg_status_t fsimg_cli_publish(const fsimg_cli_options_t *, fsimg_cli_paths_t *,
    const fsimg_worker_result_t *, const volatile sig_atomic_t *, fsimg_error_t *);
fsimg_status_t fsimg_cli_cleanup(fsimg_cli_options_t *, fsimg_cli_paths_t *,
    fsimg_status_t, fsimg_error_t *);
void fsimg_cli_display_init(fsimg_cli_display_t *, const fsimg_cli_options_t *,
    const fsimg_worker_result_t *, const fsimg_cli_paths_t *);
fsimg_status_t fsimg_cli_event(void *, const fsimg_frame_t *,
    fsimg_worker_text_t *, fsimg_error_t *);
fsimg_status_t fsimg_cli_final(void *, fsimg_worker_text_t *, fsimg_error_t *);

#endif
