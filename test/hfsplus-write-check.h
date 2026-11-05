/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef HFSPLUS_WRITE_CHECK_H
#define HFSPLUS_WRITE_CHECK_H
#include "hfsplus-write-fixture.h"
/* Raw-byte oracle; no production format codec, comparator or planner. */
void hwc_check(const hwf_image_t *, const hwf_fixture_t *,
    const fsimg_write_options_t *, unsigned, bool);
#endif
