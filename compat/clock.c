/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <time.h>

#include "host.h"

/*
 * The command line's one monotonic clock. Every deadline, grace period and
 * supervision wait compares against it, so a failed query must never read
 * as an early time: a reading of zero would leave every deadline in the
 * future and every bounded wait unbounded.
 */
uint64_t
fsimg_host_clock(void)
{
	struct timespec time;

	if (clock_gettime(CLOCK_MONOTONIC, &time) == -1)
		return (FSIMG_HOST_CLOCK_FAILED);
	return ((uint64_t)time.tv_sec * 1000 + (uint64_t)time.tv_nsec / 1000000);
}

uint64_t
fsimg_host_deadline(uint64_t milliseconds)
{
	uint64_t now;

	now = fsimg_host_clock();
	if (now == FSIMG_HOST_CLOCK_FAILED)
		return (FSIMG_HOST_DEADLINE_PASSED);
	return (milliseconds > UINT64_MAX - now ? UINT64_MAX : now + milliseconds);
}

bool
fsimg_host_expired(uint64_t deadline)
{
	return (deadline != 0 && fsimg_host_clock() >= deadline);
}
