/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
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
 * "/a/b" and "." populates the image root. An operand "@name" is a
 * manifest: host_path is the name after the "@", the -C directory, when
 * one is in force, is also its content directory, and it has no image
 * path, since a manifest's entries are rooted at the image root.
 */
typedef struct fsimg_cli_operand {
	/* Borrowed -C argument; NULL opens the operand relative to the
	 * working directory, and then a manifest has no content directory.
	 */
	const char	*directory;
	const char	*host_path;	/* Borrowed command word, after the "@" of a manifest. */
	char		*image_path;	/* Owned; "/" for an operand without components; NULL for a manifest. */
	bool		 manifest;
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
	const char	*mtree;		/* x: --mtree argument, or NULL. */
	const char	*scratch;	/* -T, TMPDIR or /tmp; "-" disables scratch. */
	char		**words;	/* Owned rewrite of a dashless first-argument bundle. */
	fsimg_cli_operand_t *operands;	/* Owned create operands in command-line order. */
	unsigned	 operand_count;
	char		*member;	/* Owned absolute member path of a read mode, or NULL. */
	/* c: the -o file settings' host paths, borrowed from argv, in
	 * fsimg_file_setting_t order; NULL for an absent setting.
	 */
	const char	*setting_paths[FSIMG_FILE_SETTINGS];
	char		 stem[NAME_MAX + 1]; /* x: the fresh destination's name. */
	unsigned	 verbosity;	/* Number of -v flags, at most two. */
	bool		 json;
	bool		 syntax_error;	/* A failed parse was invalid syntax (exit 2). */
	bool		 overview;	/* A syntax error shows the overview, not a mode synopsis. */
	bool		 verify_staged;	/* cW: verify the staged image before publication. */
	bool		 extract_here;	/* -E: extract into the -C directory itself. */
	bool		 random_uuid;	/* paths generates the volume UUID */
	const volatile sig_atomic_t *interrupted; /* Borrowed coordinator signal */
} fsimg_cli_options_t;

/* What a staged output is called; private to paths.c. */
struct fsimg_cli_staged_kind;

/*
 * One output the coordinator stages and publishes: a created image, or the
 * manifest of x --mtree. Its file is created in a fresh private directory
 * beside its final name, which must not exist yet. Publication links the
 * file there without replacement; cleanup then removes the staged file and
 * the private directory, unless a publication that is unsupported or
 * uncertain retains them for inspection. published means the final name
 * holds the output, even when a later step fails. retained names the
 * private directory while it exists; once cleanup has run it names what
 * remains: nothing, the private directory, or the staged file in it when
 * publication retained that.
 */
typedef struct fsimg_cli_staged {
	const struct fsimg_cli_staged_kind *kind;	/* NULL when unused. */
	const char	*path;		/* Final pathname as given. */
	int		 parent;	/* The final name's directory. */
	int		 directory;	/* The private directory. */
	char		 name[NAME_MAX + 1];	/* The final basename. */
	char		 directory_name[96];
	char		 retained[PATH_MAX];
	bool		 created;	/* directory_name was created in parent. */
	bool		 published;
	bool		 keep_staged;	/* Publication left the staged file. */
} fsimg_cli_staged_t;

typedef struct fsimg_cli_paths {
	/* A staged creation lends image.directory to its worker as
	 * request.output_fd, which nothing closes but the image's cleanup in
	 * fsimg_cli_cleanup; the manifest's staged file goes to the worker as
	 * request.manifest_fd, which the request owns.
	 */
	fsimg_cli_staged_t image;
	fsimg_cli_staged_t manifest;
	int		 destination_parent;	/* x: the -C directory of a fresh destination. */
	int		 scratch_parent;
	/* Owned root descriptors, one per host operand and one or two per
	 * manifest operand, and an owned array of their borrowed job paths:
	 * the operand's image path, or for a manifest's descriptors
	 * FSIMG_JOB_MANIFEST, then FSIMG_JOB_CONTENTS when it has a content
	 * directory.
	 */
	int		*root_fds;
	const char	**root_paths;
	char		 scratch_name[96];
	char		 retained_destination[PATH_MAX];
	char		 retained_scratch[PATH_MAX];
	bool		 scratch_created;
	bool		 extraction_prepared;	/* A destination was created or checked. */
	bool		 verification_failed;	/* cW rejected the staged image. */
} fsimg_cli_paths_t;

typedef enum fsimg_cli_output_phase {
	FSIMG_CLI_OUTPUT_RECORDS,
	FSIMG_CLI_OUTPUT_LAYERS,	/* --formats: schemes, containers, compressions */
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
	/* The read job's IMAGE record: its stack heads the volume report,
	 * and W names the verified table.
	 */
	unsigned char	 image[FSIMG_WIRE_IMAGE_BYTES];
	/* The read job's VOLUME record, whose named details W adds for an
	 * ImageFS.
	 */
	unsigned char	 volume[FSIMG_WIRE_INFO_BYTES];
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
/* Paths holding nothing, every descriptor -1; fsimg_cli_prepare starts
 * from this state itself.
 */
void fsimg_cli_paths_init(fsimg_cli_paths_t *);
fsimg_status_t fsimg_cli_prepare(fsimg_cli_options_t *, fsimg_cli_paths_t *,
    fsimg_error_t *);
/* Build the cW verification job; its input descriptor belongs to the
 * caller, which closes it after the worker exits.
 */
fsimg_status_t fsimg_cli_verification_request(const fsimg_cli_options_t *,
    const fsimg_cli_paths_t *, fsimg_worker_request_t *, fsimg_error_t *);
/* Publish the staged image with its worker's reported length, or the
 * staged manifest with its file's own, under the command's limits,
 * deadline and interrupt.
 */
fsimg_status_t fsimg_cli_publish(const fsimg_cli_options_t *, fsimg_cli_paths_t *,
    const fsimg_worker_result_t *, const volatile sig_atomic_t *, fsimg_error_t *);
fsimg_status_t fsimg_cli_publish_mtree(const fsimg_cli_options_t *,
    fsimg_cli_paths_t *, const volatile sig_atomic_t *, fsimg_error_t *);
/* Whether any output, the image or the --mtree manifest, was published:
 * a later failure then reports the output published, since it exists and
 * must not be recreated blindly.
 */
bool fsimg_cli_published(const fsimg_cli_paths_t *);
fsimg_status_t fsimg_cli_cleanup(fsimg_cli_options_t *, fsimg_cli_paths_t *,
    fsimg_status_t, fsimg_error_t *);
void fsimg_cli_display_init(fsimg_cli_display_t *, const fsimg_cli_options_t *,
    const fsimg_worker_result_t *, const fsimg_cli_paths_t *);
fsimg_status_t fsimg_cli_event(void *, const fsimg_frame_t *,
    fsimg_worker_text_t *, fsimg_error_t *);
fsimg_status_t fsimg_cli_final(void *, fsimg_worker_text_t *, fsimg_error_t *);

#endif
