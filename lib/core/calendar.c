/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
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

/* Days from 1970-01-01 to a proleptic Gregorian date, counted in 400-year
 * eras of 146097 days from 0000-03-01 so no loop runs over the years and
 * dates before 1970 count negative. The date must be valid: month 1 to 12
 * and day within the month, else false and *days untouched. Years 0
 * through 9999 cover every caller's range.
 */
bool
fsimg_civil_days(unsigned year, unsigned month, unsigned day, int64_t *days)
{
	static const unsigned month_days[] = { 31, 28, 31, 30, 31, 30,
	    31, 31, 30, 31, 30, 31 };
	unsigned last, shifted_year, era, year_of_era, day_of_year;
	bool leap;

	if (year > 9999 || month == 0 || month > 12)
		return (false);
	leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
	last = month_days[month - 1] + (month == 2 && leap ? 1 : 0);
	if (day == 0 || day > last)
		return (false);
	/* The shifted year starts in March, so February's leap day ends it. */
	shifted_year = month <= 2 ? year + 399 : year + 400;
	era = shifted_year / 400;
	year_of_era = shifted_year % 400;
	day_of_year = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 +
	    day - 1;
	*days = (int64_t)era * 146097 + (int64_t)year_of_era * 365 +
	    year_of_era / 4 - year_of_era / 100 + day_of_year -
	    146097 - 719468;
	return (true);
}

bool
fsimg_dos_time_decode(uint16_t date, uint16_t time, unsigned increments_10ms,
    uint64_t precision, fsimg_timestamp_t *out)
{
	unsigned hour, minute, second;
	int64_t days;

	hour = time >> 11;
	minute = (time >> 5) & 63;
	second = (time & 31) * 2;
	if (hour > 23 || minute > 59 || second > 58 || increments_10ms > 199 ||
	    !fsimg_civil_days(1980 + (date >> 9), (date >> 5) & 15, date & 31,
	    &days))
		return (false);
	out->seconds = days * 86400 + hour * 3600 + minute * 60 +
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
    int64_t origin, uint32_t maximum, uint32_t fallback, bool reproducible,
    uint32_t *out)
{
	bool in_range;

	*out = fallback;
	if (!present)
		return (true);
	/* The bounds move the origin across, so nothing can overflow: the sum
	 * below is taken only for seconds these comparisons admitted.
	 */
	in_range = time->seconds >= -origin &&
	    time->seconds <= (int64_t)maximum - origin;
	if (in_range && !reproducible)
		*out = (uint32_t)(time->seconds + origin);
	return (in_range && time->basis == FSIMG_TIME_UTC &&
	    time->nanoseconds == 0 && time->seconds + origin == (int64_t)*out);
}
