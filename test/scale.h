/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_TEST_SCALE_H
#define FSIMG_TEST_SCALE_H

#include <sys/types.h>
#include <sys/resource.h>
#if defined(__FreeBSD__)
#include <sys/sysctl.h>
#include <sys/user.h>
#endif

#include <stdint.h>
#include <unistd.h>

#include "meta.h"
#include "test.h"

/* Reader scale budget and default writer scale budget. */
#define SCALE_BUDGET		(UINT64_C(16) << 20)

/* Cache maximum, one default sort run, and the component's state allowance. */
static inline uint64_t
scale_memory(uint64_t state)
{
	return (fsimg_meta_cache_max_bytes(SCALE_BUDGET) +
	    fsimg_sort_run_bytes(SCALE_BUDGET) + state);
}

#if defined(__FreeBSD__)
/*
 * The current resident set in bytes. ki_rssize also counts each thread's kernel
 * stack pages, which ru_maxrss omits: a few KiB, harmless for a growth bound.
 */
static inline uint64_t
scale_resident_bytes(void)
{
	struct kinfo_proc process;
	size_t length = sizeof(process);
	long page_size;
	int mib[] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };

	CHECK(sysctl(mib, 4, &process, &length, NULL, 0) == 0);
	CHECK(length == sizeof(process) &&
	    process.ki_structsize == (int)sizeof(process));
	page_size = sysconf(_SC_PAGESIZE);
	CHECK(page_size > 0 && process.ki_rssize >= 0);
	CHECK((uint64_t)process.ki_rssize <= UINT64_MAX / (uint64_t)page_size);
	return ((uint64_t)process.ki_rssize * (uint64_t)page_size);
}
#endif

/*
 * The process's peak resident set observed so far, in bytes. FreeBSD updates
 * ru_maxrss only on statclock ticks, but fork inherits resident mappings
 * immediately. Include the current resident set so a child's baseline accounts
 * for those mappings even before its first tick.
 */
static inline uint64_t
scale_peak_rss(void)
{
	struct rusage usage;
#if defined(__FreeBSD__)
	uint64_t resident;
#endif
	uint64_t rss;

	CHECK(getrusage(RUSAGE_SELF, &usage) == 0 && usage.ru_maxrss >= 0);
	rss = (uint64_t)usage.ru_maxrss;
#if !defined(__APPLE__)
	CHECK(rss <= UINT64_MAX / 1024);
	rss *= 1024;
#endif
#if defined(__FreeBSD__)
	resident = scale_resident_bytes();
	if (resident > rss)
		rss = resident;
#endif
	return (rss);
}

/*
 * Retain a running peak. On FreeBSD the instantaneous resident set falls when
 * the fixture is freed, and a statclock tick may have missed the operation's
 * peak. Sample the operation before teardown as well as after it.
 */
static inline void
scale_rss_sample(uint64_t *peak)
{
	uint64_t rss;

	rss = scale_peak_rss();
	if (rss > *peak)
		*peak = rss;
}

#endif
