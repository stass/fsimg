/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_SANDBOX_H
#define FSIMG_SANDBOX_H

#include "fsimg.h"

typedef struct fsimg_sandbox_caps {
	int	input;
	int	output;
	int	scratch;
	int	source;
	int	job;
	int	control;
	int	data;
	int	cancel;
} fsimg_sandbox_caps_t;

/* All fields are borrowed; -1 means absent. Pipe directions and filesystem
 * authority are declared once here so worker setup cannot omit a live role.
 * The trusted coordinator supplies disjoint private output/scratch trees.
 * Only scratch grants deletion; output cleanup belongs to the coordinator.
 */
fsimg_status_t fsimg_sandbox_install_caps(const fsimg_sandbox_caps_t *,
    fsimg_error_t *);

/* Prototype for a disposable, single-threaded child only. Caller closes all
 * other descriptors first, including stdio, sockets and executable handles.
 * Input is an O_RDONLY regular file, work is a private writable directory,
 * source is an optional O_RDONLY source root (-1 selects image-only profile).
 * EUNSUPPORTED means unavailable before confinement; EIO never permits retry
 * without confinement, since some restrictions may already have taken effect.
 * This private entry point is never called by in-process library operations.
 */
fsimg_status_t fsimg_sandbox_install(int, int, int, fsimg_error_t *);

#endif
