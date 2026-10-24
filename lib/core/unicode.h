/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_UNICODE_H
#define FSIMG_UNICODE_H

#include "internal.h"

uint32_t fsimg_unicode_upper(uint32_t);
bool fsimg_dos_time_decode(uint16_t, uint16_t, unsigned, uint64_t,
    fsimg_timestamp_t *);
/* Pack seconds, 315532800 through 4354819199 read as calendar seconds, as
 * a DOS date (upper half) and time (lower half) and the odd second the
 * time drops. Outside that range return false and leave both untouched.
 */
bool fsimg_dos_time_encode(int64_t, uint32_t *, unsigned char *);
/* fsimg_time_select_u32(present, time, maximum, fallback, reproducible,
 * out) stores in out the unsigned whole seconds a UTC-seconds format keeps
 * for a timestamp and returns whether they preserve it. time is read only
 * when present; an absent time takes fallback and has nothing to lose, so
 * the result is true. maximum is the format's inclusive limit, and
 * fallback a value within it: the epoch when reproducible is set.
 */
bool fsimg_time_select_u32(bool, const fsimg_timestamp_t *, uint32_t,
    uint32_t, bool, uint32_t *);

/* NULL output with capacity zero validates and counts without allocating. */
fsimg_status_t fsimg_utf8_to_utf16(const char *, size_t, uint16_t *, size_t,
    size_t *, fsimg_error_t *);
fsimg_status_t fsimg_utf16_to_utf8(const uint16_t *, size_t, char *, size_t,
    size_t *, fsimg_error_t *);
/* Whether a component of length bytes lies in the QNX-family name profile:
 * nonempty strict UTF-8 without NUL, slash, control bytes or DEL, and
 * neither "." nor "..". Each format bounds the length itself.
 */
bool fsimg_name_in_qnx_profile(const char *, size_t);
fsimg_status_t fsimg_path_validate(fsimg_ctx_t *, const char *, bool,
    size_t *, uint32_t *, fsimg_error_t *);
fsimg_status_t fsimg_metadata_validate(const fsimg_metadata_t *,
    fsimg_error_t *);

#endif
