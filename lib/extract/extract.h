/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_EXTRACT_PRIVATE_H
#define FSIMG_EXTRACT_PRIVATE_H

#include "reader.h"

typedef struct fsimg_extract_result {
	uint64_t	 entries; /* Completed entries, excluding the implicit root. */
	uint64_t	 logical_bytes; /* File lengths charged, including partial files. */
} fsimg_extract_result_t;

/* root is borrowed and must name the coordinator's exclusively created empty
 * private destination. The caller owns its final flush/close and error path.
 * This is not a public existing-directory extraction/merge interface.
 */
fsimg_status_t fsimg_extract_into(fsimg_reader_t *, int,
    const fsimg_extract_options_t *, fsimg_extract_result_t *, fsimg_error_t *);

#endif
