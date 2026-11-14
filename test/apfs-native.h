/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_TEST_APFS_NATIVE_H
#define FSIMG_TEST_APFS_NATIVE_H

#include "fsimg.h"

/* The fixture owns writer setup, planning, emission and resource checks. */
bool apfs_native_emit(const fsimg_write_options_t *, const fsimg_io_ops_t *,
    void *);
int apfs_compare_native(const char *);
unsigned apfs_native_test_decoder(void);

#endif
