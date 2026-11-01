/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_TEST_SCALE_H
#define FSIMG_TEST_SCALE_H

#include <stdint.h>

#include "meta.h"

/* Reader scale budget and default writer scale budget. */
#define SCALE_BUDGET		(UINT64_C(16) << 20)

/* Cache maximum, one default sort run, and the component's state allowance. */
static inline uint64_t
scale_memory(uint64_t state)
{
	return (fsimg_meta_cache_max_bytes(SCALE_BUDGET) +
	    fsimg_sort_run_bytes(SCALE_BUDGET) + state);
}

#endif
