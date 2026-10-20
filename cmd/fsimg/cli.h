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
	FSIMG_CLI_USAGE,
	FSIMG_CLI_VERSION
} fsimg_cli_command_t;

/* One create operand: a host path, opened relative to the -C directory in
 * force when it appeared, and the image path it populates. The image path
 * is the host path without empty and "." components, so "a//./b" populates
 * "/a/b" and "." populates the image root.
 */
typedef struct fsimg_cli_operand {
	const char	*directory;	/* Borrowed -C argument; NULL is the working directory. */
	const char	*host_path;	/* Borrowed command word. */
	char		*image_path;	/* Owned; "/" for an operand without components. */
} fsimg_cli_operand_t;

/* The parsed command. Borrowed strings point into argv or the environment
 * and stay valid until main returns; owned storage is released by
 * fsimg_cli_release, which also accepts a failed or partial parse.
 */
typedef struct fsimg_cli_options {
	fsimg_worker_request_t request;
	fsimg_cli_command_t command;
	const char	*mode;		/* "c", "cW", "x", "t", "W", "i", "formats"; "" before a mode. */
	const char	*image;		/* -f argument, or NULL. */
	const char	*directory;	/* x: -C argument; NULL is the working directory. */
	const char	*scratch;	/* -T, TMPDIR or /tmp; "-" disables scratch. */
	char		**words;	/* Owned rewrite of a dashless first-argument bundle. */
	fsimg_cli_operand_t *operands;	/* Owned create operands in command-line order. */
	unsigned	 operand_count;
	char		*member;	/* Owned absolute member path of a read mode, or NULL. */
	uint64_t	 spool_bytes;	/* -l spool; zero selects the default. */
	char		 stem[NAME_MAX + 1]; /* x: the fresh destination's name. */
	unsigned	 verbosity;	/* Number of -v flags, at most two. */
	bool		 json;
	bool		 syntax_error;	/* A failed parse was invalid syntax (exit 2). */
	bool		 overview;	/* A syntax error shows the overview, not a mode synopsis. */
	bool		 verify_staged;	/* cW: verify the staged image before publication. */
	bool		 extract_here;	/* -E: extract into the -C directory itself. */
	bool		 uuid_given;	/* -o uuid= was given */
	bool		 random_uuid;	/* paths generates the volume UUID */
} fsimg_cli_options_t;

typedef struct fsimg_cli_paths {
	int		 parent;
	int		 scratch_parent;
	int		*root_fds;	/* Owned; one opened root per create operand. */
	const char	**root_paths;	/* Owned array of borrowed image paths. */
	char		 basename[NAME_MAX + 1];
	char		 staging_name[96];
	char		 scratch_name[96];
	char		 retained_output[PATH_MAX];
	char		 retained_scratch[PATH_MAX];
	bool		 staging_created;
	bool		 scratch_created;
	bool		 extraction_prepared;	/* A destination was created or checked. */
	bool		 published;
	bool		 retain_staging;
	bool		 verification_failed;	/* cW rejected the staged image. */
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
	/* The result is announced only after worker exit and finalization. */
	unsigned char	 verification[FSIMG_WIRE_VERIFY_BYTES];
	/* Human listing suffix for the open entry's kind. */
	const char	*entry_marker;
	fsimg_validation_scope_t scope;
	bool		 syntax;
	bool		 entry_open;
	bool		 overflow;
	bool		 progress_started;
	/* cW: events come from the verification worker of the staged image. */
	bool		 verifying_staged;
} fsimg_cli_display_t;

fsimg_status_t fsimg_cli_number(const char *, bool, uint64_t *, fsimg_error_t *);
fsimg_status_t fsimg_cli_epoch(const char *, int64_t *, fsimg_error_t *);
fsimg_status_t fsimg_cli_parse(int, char **, fsimg_cli_options_t *, fsimg_error_t *);
void fsimg_cli_release(fsimg_cli_options_t *);
/* The extraction directory name of an image path: its basename without one
 * compression, then one container, then one generic image suffix. False
 * when nothing was stripped or the rest is not one valid host name.
 */
bool fsimg_cli_stem(const char *, char *, size_t);
uint64_t fsimg_cli_clock(void);
fsimg_status_t fsimg_cli_prepare(fsimg_cli_options_t *, fsimg_cli_paths_t *,
    fsimg_error_t *);
/* Build the cW verification job; its input descriptor belongs to the
 * caller, which closes it after the worker exits.
 */
fsimg_status_t fsimg_cli_verification_request(const fsimg_cli_options_t *,
    fsimg_worker_request_t *, fsimg_error_t *);
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
