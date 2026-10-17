/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_READER_PRIVATE_H
#define FSIMG_READER_PRIVATE_H

#include "io.h"
#include "meta.h"

typedef struct fsimg_fat_reader fsimg_fat_reader_t;
typedef struct fsimg_exfat_reader fsimg_exfat_reader_t;

/* A selected driver owns only its format state. Common code owns the input,
 * session, public snapshot and lifecycle, including terminal failure cleanup.
 * close accepts a partially opened object and repeated calls after cleanup.
 */
typedef struct fsimg_reader_ops {
	fsimg_status_t (*open)(fsimg_reader_t *, const char *, fsimg_error_t *);
	void (*close)(fsimg_reader_t *);
	fsimg_status_t (*next)(fsimg_reader_t *, fsimg_error_t *);
	fsimg_status_t (*match_name)(fsimg_reader_t *, const char *, bool *,
	    fsimg_error_t *);
	fsimg_status_t (*skip_directory)(fsimg_reader_t *, fsimg_error_t *);
	fsimg_status_t (*read)(fsimg_reader_t *, uint64_t, void *, size_t,
	    size_t *, fsimg_error_t *);
	fsimg_status_t (*range)(fsimg_reader_t *, uint64_t, fsimg_range_t *,
	    fsimg_error_t *);
	fsimg_status_t (*verify)(fsimg_reader_t *, unsigned,
	    fsimg_verify_report_t *, fsimg_error_t *);
} fsimg_reader_ops_t;

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
	const fsimg_reader_ops_t *ops;
	union {
		fsimg_fat_reader_t *fat;
		fsimg_exfat_reader_t *exfat;
	} format;
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
fsimg_status_t fsimg_reader_abort(fsimg_reader_t *, fsimg_status_t);
fsimg_status_t fsimg_reader_end_scope(fsimg_reader_t *);
fsimg_status_t fsimg_reader_match_name(fsimg_reader_t *, const char *, bool *,
    fsimg_error_t *);
fsimg_status_t fsimg_reader_skip_directory(fsimg_reader_t *, fsimg_error_t *);

#endif
