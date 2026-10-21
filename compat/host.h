/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_HOST_H
#define FSIMG_HOST_H

#include <sys/types.h>

#include <stdbool.h>
#include <stdint.h>

#include "fsimg.h"

typedef enum fsimg_host_kind {
	FSIMG_HOST_OTHER,
	FSIMG_HOST_FILE,
	FSIMG_HOST_DIRECTORY,
	FSIMG_HOST_LINK
} fsimg_host_kind_t;

typedef struct fsimg_host_file {
	uint64_t	 device;
	uint64_t	 inode;
	uint64_t	 length;
	fsimg_host_kind_t kind;
	fsimg_timestamp_t modified;
	fsimg_timestamp_t changed;
	fsimg_metadata_t metadata;
} fsimg_host_file_t;

#if defined(__GNUC__) || defined(__clang__)
#define FSIMG_UNUSED __attribute__((unused))
#else
#define FSIMG_UNUSED
#endif

#if defined(__linux__)
#define FSIMG_HOST_GETOPT_PREFIX "+"
#else
#define FSIMG_HOST_GETOPT_PREFIX ""
#endif

void fsimg_host_getopt_reset(void);
int fsimg_host_random(void *, size_t);

bool fsimg_host_offset(uint64_t, off_t *);
int fsimg_host_dup(int);
/* The host's first hole at or after an offset: its start and the data
 * after it, UINT64_MAX where the host answers ENXIO, and the file length
 * observed after both seeks; -1 with errno when the host cannot say. The
 * callback, when not NULL, runs between the two seeks. The descriptor's
 * file offset is restored.
 */
int fsimg_host_next_hole(int, uint64_t, void (*)(void *), void *, uint64_t *,
    uint64_t *, uint64_t *);
int fsimg_host_spawn_worker(const char *, const int *, unsigned, pid_t *);
int fsimg_host_spawn_output(const char *, const int *, unsigned, pid_t *);
int fsimg_host_spawn_input(const char *, const int *, unsigned, pid_t *);
int fsimg_host_executable(char *, size_t);
/* Failure is fatal to worker startup; no resource-limit-bounded fallback. */
int fsimg_host_closefrom(int);
bool fsimg_host_unsupported_io(int);
int fsimg_host_file(int, fsimg_host_file_t *);
/* Inspect parent/name without following a final symbolic link. */
int fsimg_host_link(int, const char *, fsimg_host_file_t *);
/* Inspect parent/name as fsimg_host_link does where the host inspects a
 * name before opening it: 1 with the object described, 0 when this host
 * opens the name without inspecting it, -1 with errno on failure.
 */
int fsimg_host_inspect_unopened(int, const char *, fsimg_host_file_t *);
/* Source opens never follow links or acquire a controlling terminal/block on
 * a FIFO. ELOOP identifies a no-follow symlink refusal on every host. The
 * O_NONBLOCK that keeps a FIFO's open from waiting leaves reads of a
 * regular file unchanged.
 */
int fsimg_host_source_open(int, const char *, bool);
/* Descriptor-based mount identity; ENOTSUP means no enforceable identity. */
int fsimg_host_mount_id(int, uint64_t *);
/* Directory readers use only the supplied buffer, never a libc DIR buffer.
 * Names borrow that buffer until its next fill. Malformed host records fail.
 */
int fsimg_host_directory_read(int, void *, size_t, size_t *);
int fsimg_host_directory_name(void *, size_t, size_t *, const char **,
    size_t *);

#endif
