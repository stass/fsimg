/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_HOST_H
#define FSIMG_HOST_H

#include <sys/types.h>

#include <stdbool.h>
#include <stdint.h>

#include "fsimg.h"

struct stat;

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
/* Rename from_dir/from to to_dir/to only if no object has that name, as
 * one atomic operation: Linux renameat2 RENAME_NOREPLACE, macOS
 * renameatx_np RENAME_EXCL, FreeBSD renameat2 AT_RENAME_NOREPLACE. EEXIST
 * means the target exists. A host without the primitive fails with ENOSYS;
 * a filesystem without it, with the errno its host gives, such as EINVAL
 * on Linux or ENOTSUP on macOS.
 */
int fsimg_host_rename_exclusive(int, const char *, int, const char *);
bool fsimg_host_unsupported_io(int);
int fsimg_host_file(int, fsimg_host_file_t *);
/*
 * fstat(2), and the only way fsimg calls it. On Linux x86_64 and aarch64,
 * where the confined worker's seccomp filter applies, it issues the fstat
 * system call itself: glibc 2.33 to 2.38 implement fstat() as
 * newfstatat(fd, "", AT_EMPTY_PATH), which the filter denies, since a
 * filter cannot tell an empty path from another one.
 */
int fsimg_host_fstat(int, struct stat *);
/* Inspect parent/name without following a final symbolic link. */
int fsimg_host_link(int, const char *, fsimg_host_file_t *);
/* Source opens never follow links or acquire a controlling terminal/block on
 * a FIFO. ELOOP identifies a no-follow symlink refusal on every host, and
 * ENXIO an object of a kind no open reads, a socket or a device without a
 * driver, in place of the host's own errno, which is not kept. The
 * O_NONBLOCK that keeps a FIFO's open from waiting leaves reads of a
 * regular file unchanged.
 */
int fsimg_host_source_open(int, const char *, bool);
/* Directory readers use only the supplied buffer, never a libc DIR buffer.
 * Names borrow that buffer until its next fill. Malformed host records fail.
 */
int fsimg_host_directory_read(int, void *, size_t, size_t *);
int fsimg_host_directory_name(void *, size_t, size_t *, const char **,
    size_t *);

#endif
