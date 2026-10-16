/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */

#ifndef FSIMG_H
#define FSIMG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FSIMG_OPTIONS_VERSION 1u
#define FSIMG_IO_READ 0x01u
#define FSIMG_IO_WRITE 0x02u
#define FSIMG_IO_DURABLE 0x04u

typedef struct fsimg_ctx fsimg_ctx_t;
typedef struct fsimg_io fsimg_io_t;

typedef enum fsimg_status {
	FSIMG_OK = 0,
	FSIMG_DONE = 1,
	FSIMG_EINVAL = -1,
	FSIMG_ESTATE = -2,
	FSIMG_EIO = -3,
	FSIMG_EFORMAT = -4,
	FSIMG_ECORRUPT = -5,
	FSIMG_EUNSUPPORTED = -6,
	FSIMG_ERANGE = -7,
	FSIMG_ELIMIT = -8,
	FSIMG_ENOMEM = -9,
	FSIMG_ECANCELLED = -10,
	FSIMG_ESOURCE_CHANGED = -11,
	FSIMG_EEXISTS = -12,
	FSIMG_EDURABILITY = -13
} fsimg_status_t;

typedef enum fsimg_operation {
	FSIMG_OP_NONE,
	FSIMG_OP_CONTEXT,
	FSIMG_OP_ALLOCATE,
	FSIMG_OP_IO,
	FSIMG_OP_PUBLISH
} fsimg_operation_t;

typedef enum fsimg_resource {
	FSIMG_RESOURCE_NONE,
	FSIMG_RESOURCE_MEMORY,
	FSIMG_RESOURCE_SCRATCH,
	FSIMG_RESOURCE_DESCRIPTORS,
	FSIMG_RESOURCE_ENTRIES,
	FSIMG_RESOURCE_OUTPUT,
	FSIMG_RESOURCE_WORK
} fsimg_resource_t;

typedef enum fsimg_output_state {
	FSIMG_OUTPUT_ABSENT,
	FSIMG_OUTPUT_INCOMPLETE,
	FSIMG_OUTPUT_PUBLISHED,
	FSIMG_OUTPUT_UNCERTAIN
} fsimg_output_state_t;

typedef struct fsimg_error {
	fsimg_status_t	 status;
	int		 os_error;
	fsimg_operation_t operation;
	fsimg_resource_t resource;
	fsimg_output_state_t output_state;
	uint64_t	 offset;
	uint64_t	 limit;
	uint64_t	 observed;
	bool		 offset_valid;
	bool		 path_truncated;
	bool		 message_truncated;
	char		 path[256];
	char		 message[256];
} fsimg_error_t;

typedef struct fsimg_progress {
	uint64_t	 entries;
	uint64_t	 bytes;
	uint64_t	 work;
	const char	*phase;
} fsimg_progress_t;

typedef void (*fsimg_progress_fn_t)(void *, const fsimg_progress_t *);
typedef bool (*fsimg_cancel_fn_t)(void *);

typedef struct fsimg_options {
	size_t		 size;
	uint32_t	 version;
	uint32_t	 flags;
	uint64_t	 memory_bytes;
	uint64_t	 scratch_bytes;
	uint64_t	 max_entries;
	uint64_t	 max_output_bytes;
	uint64_t	 max_work;
	uint32_t	 max_depth;
	uint32_t	 max_path_bytes;
	uint32_t	 max_open_files;
	int		 scratch_directory_fd;
	fsimg_progress_fn_t progress;
	void		*progress_argument;
	fsimg_cancel_fn_t cancelled;
	void		*cancel_argument;
	uint64_t	 reserved[4];
} fsimg_options_t;

typedef struct fsimg_stats {
	uint64_t	 memory_bytes;
	uint64_t	 memory_peak;
	uint64_t	 scratch_bytes;
	uint64_t	 scratch_peak;
	uint64_t	 descriptors;
	uint64_t	 descriptors_peak;
	uint64_t	 read_bytes;
	uint64_t	 written_bytes;
	uint64_t	 io_calls;
} fsimg_stats_t;

/* Options and callback tables must have exactly this size and version.
 * Zero limits select defaults. Contexts are serial; callbacks cannot re-enter.
 * The context duplicates scratch_directory_fd; -1 disables scratch.
 * Child handles must be freed before their context. Destructors accept NULL.
 */
void fsimg_options_init(fsimg_options_t *);
fsimg_status_t fsimg_ctx_new(const fsimg_options_t *, fsimg_ctx_t **,
    fsimg_error_t *);
void fsimg_ctx_free(fsimg_ctx_t *);
fsimg_status_t fsimg_ctx_get_stats(fsimg_ctx_t *, fsimg_stats_t *,
    fsimg_error_t *);
const char *fsimg_status_string(fsimg_status_t);

typedef fsimg_status_t (*fsimg_read_at_fn_t)(void *, uint64_t, void *,
    size_t, size_t *, fsimg_error_t *);
typedef fsimg_status_t (*fsimg_write_at_fn_t)(void *, uint64_t, const void *,
    size_t, size_t *, fsimg_error_t *);
typedef fsimg_status_t (*fsimg_get_size_fn_t)(void *, uint64_t *,
    fsimg_error_t *);
typedef fsimg_status_t (*fsimg_resize_fn_t)(void *, uint64_t, fsimg_error_t *);
typedef fsimg_status_t (*fsimg_flush_fn_t)(void *, fsimg_error_t *);
typedef fsimg_status_t (*fsimg_zero_fn_t)(void *, uint64_t, uint64_t,
    fsimg_error_t *);

typedef struct fsimg_io_ops {
	size_t		 size;
	uint32_t	 version;
	unsigned	 flags;
	size_t		 max_transfer;
	fsimg_read_at_fn_t read_at;
	fsimg_write_at_fn_t write_at;
	fsimg_get_size_fn_t get_size;
	fsimg_resize_fn_t resize;
	fsimg_flush_fn_t	 flush;
	fsimg_zero_fn_t	 zero_range;
	uint64_t	 reserved[4];
} fsimg_io_ops_t;

/* Tables are copied; their argument and buffers remain caller-owned.
 * Callback transfers may be short, including progress on failure, but must
 * never exceed the request. EINTR with confirmed progress is retried only
 * after advancing. Other errors are terminal. Zero-length requests do no I/O.
 * At most 1024 consecutive interruptions without progress are retried;
 * cancellation is checked between retries, including flush retries.
 * FD adapters duplicate regular-file FDs and preserve shared flags/position.
 * Read-only authority requires O_RDONLY. Keep shared flags and storage stable.
 * A slice borrows its parent, has fixed length, and cannot resize its parent.
 * Raw writes can leave partial output. No-flush sinks give logical completion;
 * durable completion requires FSIMG_IO_DURABLE plus a flush callback.
 */
void fsimg_io_ops_init(fsimg_io_ops_t *);
fsimg_status_t fsimg_io_from_fd(fsimg_ctx_t *, int, unsigned, fsimg_io_t **,
    fsimg_error_t *);
fsimg_status_t fsimg_io_from_callbacks(fsimg_ctx_t *, const fsimg_io_ops_t *,
    void *, fsimg_io_t **, fsimg_error_t *);
fsimg_status_t fsimg_io_slice(fsimg_io_t *, uint64_t, uint64_t, fsimg_io_t **,
    fsimg_error_t *);
void fsimg_io_free(fsimg_io_t *);
fsimg_status_t fsimg_io_get_size(fsimg_io_t *, uint64_t *, fsimg_error_t *);
fsimg_status_t fsimg_io_read_at(fsimg_io_t *, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);
fsimg_status_t fsimg_io_write_at(fsimg_io_t *, uint64_t, const void *, size_t,
    size_t *, fsimg_error_t *);
fsimg_status_t fsimg_io_read_exact(fsimg_io_t *, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);
fsimg_status_t fsimg_io_write_exact(fsimg_io_t *, uint64_t, const void *,
    size_t, size_t *, fsimg_error_t *);
fsimg_status_t fsimg_io_resize(fsimg_io_t *, uint64_t, fsimg_error_t *);
fsimg_status_t fsimg_io_zero_range(fsimg_io_t *, uint64_t, uint64_t,
    fsimg_error_t *);
fsimg_status_t fsimg_io_flush(fsimg_io_t *, bool, fsimg_error_t *);

#ifdef __cplusplus
}
#endif
#endif
