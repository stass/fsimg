/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_INPUT_H
#define FSIMG_INPUT_H

#include "fsimg.h"

/* Prepared image authority, independent of descriptor numbers and wire layout.
 * Only positional kinds have geometry; files use a one-byte quantum.
 */
typedef enum fsimg_input_kind {
	FSIMG_INPUT_ABSENT,
	FSIMG_INPUT_FILE,
	FSIMG_INPUT_DEVICE,
	FSIMG_INPUT_STREAM
} fsimg_input_kind_t;

typedef struct fsimg_input {
	uint64_t	 length;
	uint64_t	 quantum;
	fsimg_input_kind_t kind;
} fsimg_input_t;

/* Optional preparation checkpoint: coordinator deadline/signal or context poll.
 * Queries occur only here, never when adopting inside a confined worker.
 */
typedef fsimg_status_t (*fsimg_input_checkpoint_t)(void *, fsimg_error_t *);
fsimg_status_t fsimg_input_prepare(int, fsimg_input_checkpoint_t, void *,
    fsimg_input_t *, fsimg_error_t *);
fsimg_status_t fsimg_input_validate(const fsimg_input_t *, fsimg_error_t *);
/* Recheck positional descriptor class/access and regular-file length only. */
fsimg_status_t fsimg_input_check(int, const fsimg_input_t *, fsimg_error_t *);

#endif
