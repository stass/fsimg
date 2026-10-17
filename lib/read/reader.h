/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_READER_PRIVATE_H
#define FSIMG_READER_PRIVATE_H

#include "io.h"
#include "meta.h"

typedef struct fsimg_fat_reader fsimg_fat_reader_t;

typedef enum fsimg_reader_state {
	FSIMG_READER_NEW,
	FSIMG_READER_ITERATING,
	FSIMG_READER_DONE,
	FSIMG_READER_FAILED
} fsimg_reader_state_t;

struct fsimg_reader {
	fsimg_ctx_t	*ctx;
	fsimg_session_t	 session;
	fsimg_io_t	*input;
	fsimg_fat_reader_t *fat;
	fsimg_host_file_t identity;
	fsimg_volume_info_t info;
	fsimg_entry_t	 entry;
	fsimg_reader_state_t state;
	char		*path;
	bool		 host_identity;
};

fsimg_status_t fsimg_reader_bytes(fsimg_reader_t *, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);
fsimg_status_t fsimg_reader_stable(fsimg_reader_t *, fsimg_error_t *);

#endif
