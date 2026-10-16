/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_HOST_H
#define FSIMG_HOST_H

#include <sys/types.h>

#include <stdbool.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define FSIMG_UNUSED __attribute__((unused))
#else
#define FSIMG_UNUSED
#endif

bool fsimg_host_offset(uint64_t, off_t *);
int fsimg_host_dup(int);
/* Failure is fatal to worker startup; no resource-limit-bounded fallback. */
int fsimg_host_closefrom(int);
bool fsimg_host_unsupported_io(int);

#endif
