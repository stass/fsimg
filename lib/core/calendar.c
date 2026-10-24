/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 * Copyright (c) 2026, fsimg contributors
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "config.h"

#include "unicode.h"

bool
fsimg_dos_time_decode(uint16_t date, uint16_t time, unsigned increments_10ms,
    uint64_t precision, fsimg_timestamp_t *out)
{
	static const unsigned month_days[] = { 31, 28, 31, 30, 31, 30,
	    31, 31, 30, 31, 30, 31 };
	unsigned year, month, day, days_in_month, hour, minute, second;
	int64_t days;

	year = 1980 + (date >> 9);
	month = (date >> 5) & 15;
	day = date & 31;
	hour = time >> 11;
	minute = (time >> 5) & 63;
	second = (time & 31) * 2;
	if (month == 0 || month > 12 || hour > 23 || minute > 59 ||
	    second > 58 || increments_10ms > 199)
		return (false);
	days_in_month = month_days[month - 1];
	if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))
		days_in_month++;
	if (day == 0 || day > days_in_month)
		return (false);
	days = 0;
	for (unsigned y = 1970; y < year; y++)
		days += 365 + (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
	for (unsigned m = 1; m < month; m++) {
		days += month_days[m - 1];
		if (m == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))
			days++;
	}
	out->seconds = (days + day - 1) * 86400 + hour * 3600 + minute * 60 +
	    second + increments_10ms / 100;
	out->nanoseconds = (increments_10ms % 100) * 10000000u;
	out->precision_ns = precision;
	out->basis = FSIMG_TIME_WALL;
	out->offset_minutes = 0;
	return (true);
}

/* The bounded Gregorian walk avoids host timezone and time_t semantics. */
bool
fsimg_dos_time_encode(int64_t seconds, uint32_t *packed, unsigned char *odd)
{
	static const unsigned month_days[] = {
		31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
	};
	uint64_t days;
	unsigned year, month, span, clock;
	bool leap;

	if (seconds < 315532800 || seconds > 4354819199LL)
		return (false);
	days = (uint64_t)seconds / 86400;
	clock = (unsigned)(seconds % 86400);
	year = 1970;
	for (;;) {
		leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
		span = leap ? 366 : 365;
		if (days < span)
			break;
		days -= span;
		year++;
	}
	month = 0;
	for (;;) {
		span = month_days[month] + (month == 1 && leap ? 1 : 0);
		if (days < span)
			break;
		days -= span;
		month++;
	}
	*packed = (uint32_t)((year - 1980) << 25 | (month + 1) << 21 |
	    (days + 1) << 16 | (clock / 3600) << 11 |
	    (clock / 60 % 60) << 5 | (clock % 60 / 2));
	*odd = (unsigned char)(seconds % 2);
	return (true);
}

/* A present time keeps its whole seconds when they lie in 0 through
 * maximum, dropping any fraction, and otherwise takes the fallback rather
 * than an endpoint; reproducible mode stores the fallback, the epoch, for
 * every time. No offset is applied: the stored value is a UTC instant, so
 * a wall-clock or known-offset time, a fraction, or a stored value other
 * than the time's own seconds is not preserved.
 */
bool
fsimg_time_select_u32(bool present, const fsimg_timestamp_t *time,
    uint32_t maximum, uint32_t fallback, bool reproducible, uint32_t *out)
{
	bool in_range;

	*out = fallback;
	if (!present)
		return (true);
	in_range = time->seconds >= 0 && time->seconds <= (int64_t)maximum;
	if (in_range && !reproducible)
		*out = (uint32_t)time->seconds;
	return (in_range && time->basis == FSIMG_TIME_UTC &&
	    time->nanoseconds == 0 && time->seconds == (int64_t)*out);
}
