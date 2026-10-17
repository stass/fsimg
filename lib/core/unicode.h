/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_UNICODE_H
#define FSIMG_UNICODE_H

#include "internal.h"

/* NULL output with capacity zero validates and counts without allocating. */
fsimg_status_t fsimg_utf8_to_utf16(const char *, size_t, uint16_t *, size_t,
    size_t *, fsimg_error_t *);
fsimg_status_t fsimg_utf16_to_utf8(const uint16_t *, size_t, char *, size_t,
    size_t *, fsimg_error_t *);
fsimg_status_t fsimg_path_validate(fsimg_ctx_t *, const char *, bool,
    size_t *, uint32_t *, fsimg_error_t *);
fsimg_status_t fsimg_metadata_validate(const fsimg_metadata_t *,
    fsimg_error_t *);

#endif
