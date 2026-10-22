/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_SANDBOX_H
#define FSIMG_SANDBOX_H

#include "fsimg.h"

/*
 * The worker's complete filesystem authority, one field per role. The
 * profile grants each role exactly what the worker does with it:
 *
 * input	read a read-only regular image file, or under input_pipe
 *		the read end of the coordinator's input pipe
 * staging	create the staged image inside it; remove nothing
 * destination	create directories and files beneath it; remove nothing
 * scratch	create, use and unlink its own files; no directories
 * roots	read beneath each directory root, or read a file root
 * raw		read, write and resize the unlinked raw intermediate
 * spool	read, write and resize the unlinked spool
 * manifest	write and resize the staged x --mtree manifest
 * settings	read each read-only regular file a create's file settings
 *		name, admitted as the image input is
 *
 * The coordinator, not the worker, removes staging and destination
 * objects. -1 marks an absent role. roots is the first of root_count
 * consecutive descriptors.
 */
#define FSIMG_SANDBOX_SETTINGS 2u

typedef struct fsimg_sandbox_caps {
	int		 input;
	int		 staging;
	int		 destination;
	int		 scratch;
	int		 roots;
	unsigned	 root_count;
	int		 raw;
	int		 spool;
	int		 manifest;
	int		 settings[FSIMG_SANDBOX_SETTINGS];
	bool		 input_pipe;
	int		 job;
	int		 control;
	int		 data;
	int		 cancel;
} fsimg_sandbox_caps_t;

/* Mark every role absent (-1) and no roots, the only safe start: a
 * zero-initialized role would name descriptor 0. A caller sets the roles
 * it grants after this.
 */
void fsimg_sandbox_caps_init(fsimg_sandbox_caps_t *);
/* Whether a single-descriptor filesystem role (input, staging,
 * destination, scratch, raw, spool or manifest) names fd.
 */
bool fsimg_sandbox_caps_names(const fsimg_sandbox_caps_t *, int);
/* All fields are borrowed. Pipe directions and filesystem authority are
 * declared once here so worker setup cannot omit a live role. The trusted
 * coordinator supplies disjoint private output and scratch trees.
 */
fsimg_status_t fsimg_sandbox_install_caps(const fsimg_sandbox_caps_t *,
    fsimg_error_t *);
/* Compile-time backend identity; this does not probe or install confinement. */
const char *fsimg_sandbox_backend(void);

/* Prototype for a disposable, single-threaded child only. Caller closes all
 * other descriptors first, including stdio, sockets and executable handles.
 * Input is an O_RDONLY regular file, destination a private directory that
 * receives the extraction grant, and source an optional O_RDONLY directory
 * root (-1 selects the image-only profile). EUNSUPPORTED means unavailable
 * before confinement; EIO never permits retry without confinement, since
 * some restrictions may already have taken effect. This private entry point
 * is never called by in-process library operations.
 */
fsimg_status_t fsimg_sandbox_install(int, int, int, fsimg_error_t *);

#endif
