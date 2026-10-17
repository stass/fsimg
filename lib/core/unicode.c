/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>

#include "unicode.h"

typedef struct fsimg_case_pair {
	uint32_t	 scalar;
	uint32_t	 upper;
} fsimg_case_pair_t;

#include "case-tables.h"

static bool utf8_scalar(const unsigned char *, size_t, size_t *, uint32_t *);
static bool timestamp_valid(const fsimg_timestamp_t *);

static bool
utf8_scalar(const unsigned char *bytes, size_t length, size_t *used,
    uint32_t *scalar)
{
	uint32_t value, minimum;
	size_t count;

	if (length == 0)
		return (false);
	if (bytes[0] < 0x80) {
		*scalar = bytes[0];
		*used = 1;
		return (true);
	}
	if (bytes[0] >= 0xc2 && bytes[0] <= 0xdf) {
		count = 2;
		value = bytes[0] & 0x1f;
		minimum = 0x80;
	} else if (bytes[0] >= 0xe0 && bytes[0] <= 0xef) {
		count = 3;
		value = bytes[0] & 0x0f;
		minimum = 0x800;
	} else if (bytes[0] >= 0xf0 && bytes[0] <= 0xf4) {
		count = 4;
		value = bytes[0] & 0x07;
		minimum = 0x10000;
	} else {
		return (false);
	}
	if (count > length)
		return (false);
	for (size_t i = 1; i < count; i++) {
		if ((bytes[i] & 0xc0) != 0x80)
			return (false);
		value = (value << 6) | (bytes[i] & 0x3f);
	}
	if (value < minimum || value > 0x10ffff ||
	    (value >= 0xd800 && value <= 0xdfff))
		return (false);
	*scalar = value;
	*used = count;
	return (true);
}

fsimg_status_t
fsimg_utf8_to_utf16(const char *input, size_t length, uint16_t *output,
    size_t capacity, size_t *written, fsimg_error_t *error)
{
	size_t offset, used, count, units;
	uint32_t scalar;

	fsimg_error_clear(error);
	if (written != NULL)
		*written = 0;
	if (written == NULL || (input == NULL && length != 0) ||
	    (output == NULL && capacity != 0))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_CATALOG, "invalid UTF conversion argument"));
	offset = 0;
	count = 0;
	while (offset < length) {
		if (!utf8_scalar((const unsigned char *)input + offset,
		    length - offset, &used, &scalar))
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_CATALOG, "invalid UTF-8 encoding"));
		units = scalar > 0xffff ? 2 : 1;
		if (count > SIZE_MAX - units ||
		    (output != NULL && units > capacity - count))
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_CATALOG, "UTF-16 output exceeds buffer"));
		if (output != NULL) {
			if (units == 1) {
				output[count] = (uint16_t)scalar;
			} else {
				scalar -= 0x10000;
				output[count] = (uint16_t)(0xd800 + (scalar >> 10));
				output[count + 1] = (uint16_t)(0xdc00 + (scalar & 0x3ff));
			}
		}
		count += units;
		offset += used;
	}
	*written = count;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_utf16_to_utf8(const uint16_t *input, size_t length, char *output,
    size_t capacity, size_t *written, fsimg_error_t *error)
{
	size_t count, units;
	uint32_t scalar;

	fsimg_error_clear(error);
	if (written != NULL)
		*written = 0;
	if (written == NULL || (input == NULL && length != 0) ||
	    (output == NULL && capacity != 0))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_CATALOG, "invalid UTF conversion argument"));
	count = 0;
	for (size_t i = 0; i < length; i++) {
		scalar = input[i];
		if (scalar >= 0xd800 && scalar <= 0xdbff) {
			if (i + 1 == length || input[i + 1] < 0xdc00 ||
			    input[i + 1] > 0xdfff)
				return (fsimg_error_set(error, FSIMG_EINVAL, 0,
				    FSIMG_OP_CATALOG, "unpaired UTF-16 high surrogate"));
			i++;
			scalar = 0x10000 + ((scalar - 0xd800) << 10) +
			    (input[i] - 0xdc00);
		} else if (scalar >= 0xdc00 && scalar <= 0xdfff) {
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_CATALOG, "unpaired UTF-16 low surrogate"));
		}
		units = scalar < 0x80 ? 1 : scalar < 0x800 ? 2 :
		    scalar < 0x10000 ? 3 : 4;
		if (count > SIZE_MAX - units ||
		    (output != NULL && units > capacity - count))
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_CATALOG, "UTF-8 output exceeds buffer"));
		if (output != NULL) {
			if (units == 1) {
				output[count] = (char)scalar;
			} else {
				for (size_t j = units - 1; j != 0; j--) {
					output[count + j] = (char)(0x80 | (scalar & 0x3f));
					scalar >>= 6;
				}
				output[count] = (char)((units == 2 ? 0xc0 :
				    units == 3 ? 0xe0 : 0xf0) | scalar);
			}
		}
		count += units;
	}
	*written = count;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_path_validate(fsimg_ctx_t *ctx, const char *path, bool allow_root,
    size_t *length, uint32_t *depth, fsimg_error_t *error)
{
	size_t bytes, start, units;
	uint32_t components;
	fsimg_status_t status;

	*length = 0;
	*depth = 0;
	if (path == NULL || path[0] != '/')
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_CATALOG, "image path must be absolute"));
	bytes = strnlen(path, ctx->options.max_path_bytes);
	if (bytes == ctx->options.max_path_bytes)
		return (fsimg_limit_error(error, FSIMG_RESOURCE_PATH,
		    ctx->options.max_path_bytes, (uint64_t)bytes + 1));
	if (bytes == 1 && allow_root) {
		*length = 1;
		return (FSIMG_OK);
	}
	status = fsimg_utf8_to_utf16(path, bytes, NULL, 0, &units, error);
	if (status != FSIMG_OK)
		return (status);
	start = 1;
	components = 0;
	for (size_t i = 1; i <= bytes; i++) {
		if (i != bytes && path[i] != '/')
			continue;
		if (i == start || (i - start == 1 && path[start] == '.') ||
		    (i - start == 2 && path[start] == '.' && path[start + 1] == '.'))
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_CATALOG, "invalid image path component"));
		components++;
		if (components > ctx->options.max_depth)
			return (fsimg_limit_error(error, FSIMG_RESOURCE_DEPTH,
			    ctx->options.max_depth, components));
		start = i + 1;
	}
	*length = bytes;
	*depth = components;
	return (FSIMG_OK);
}

static bool
timestamp_valid(const fsimg_timestamp_t *timestamp)
{
	if (timestamp->nanoseconds >= 1000000000 || timestamp->precision_ns == 0)
		return (false);
	switch (timestamp->basis) {
	case FSIMG_TIME_UTC:
	case FSIMG_TIME_WALL:
		return (timestamp->offset_minutes == 0);
	case FSIMG_TIME_OFFSET:
		return (timestamp->offset_minutes > -1440 &&
		    timestamp->offset_minutes < 1440);
	default:
		return (false);
	}
}

fsimg_status_t
fsimg_metadata_validate(const fsimg_metadata_t *metadata, fsimg_error_t *error)
{
	fsimg_error_clear(error);
	if ((metadata->valid & ~0x0fu) != 0 ||
	    (metadata->required & ~metadata->valid) != 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_CATALOG, "invalid metadata validity or requirements"));
	if (((metadata->valid & FSIMG_META_CREATED) != 0 &&
	    !timestamp_valid(&metadata->created)) ||
	    ((metadata->valid & FSIMG_META_MODIFIED) != 0 &&
	    !timestamp_valid(&metadata->modified)) ||
	    ((metadata->valid & FSIMG_META_ACCESSED) != 0 &&
	    !timestamp_valid(&metadata->accessed)))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_CATALOG, "invalid timestamp basis or precision"));
	if ((metadata->valid & FSIMG_META_ATTRIBUTES) != 0 &&
	    (metadata->attributes & ~0x0fu) != 0)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_CATALOG, "unsupported metadata attribute"));
	return (FSIMG_OK);
}

uint32_t
fsimg_unicode_upper(uint32_t scalar)
{
	size_t low, high, middle;

	low = 0;
	high = sizeof(fsimg_upper) / sizeof(fsimg_upper[0]);
	while (low < high) {
		middle = low + (high - low) / 2;
		if (fsimg_upper[middle].scalar < scalar)
			low = middle + 1;
		else
			high = middle;
	}
	if (low < sizeof(fsimg_upper) / sizeof(fsimg_upper[0]) &&
	    fsimg_upper[low].scalar == scalar)
		return (fsimg_upper[low].upper);
	return (scalar);
}
