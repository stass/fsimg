/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>

#include "ifs.h"
#include "unicode.h"

/* A stored path is the empty path, the root, or components separated by
 * single slashes. mkifs writes an absolute path, an empty component and a
 * "." or ".." component verbatim, so each is a shape fsimg does not
 * represent rather than corruption. The profile is not applied here: a
 * component outside it omits its node when the hierarchy is derived, and
 * never decides the image's fate.
 */
fsimg_status_t
ifs_path_check(const unsigned char *path, size_t length, fsimg_error_t *error)
{
	size_t start, end;

	if (length == 0)
		return (FSIMG_OK);
	if (path[0] == '/')
		return (ifs_unsupported(error, "ImageFS path is absolute"));
	for (start = 0; start <= length; start = end + 1) {
		end = ifs_component_end(path, length, start);
		if (end == start)
			return (ifs_unsupported(error,
			    "ImageFS path has an empty component"));
		if ((end - start == 1 && path[start] == '.') ||
		    (end - start == 2 && path[start] == '.' &&
		    path[start + 1] == '.'))
			return (ifs_unsupported(error,
			    "ImageFS path has a dot component"));
	}
	return (FSIMG_OK);
}

/* The end of the component of path that starts at start: the offset of the
 * next slash, or length.
 */
size_t
ifs_component_end(const unsigned char *path, size_t length, size_t start)
{
	const unsigned char *slash;

	slash = memchr(path + start, '/', length - start);
	return (slash == NULL ? length : (size_t)(slash - path));
}

/* A mountpoint is reported when its bytes follow the profile's byte rules
 * with slashes allowed: strict UTF-8 without control bytes or DEL. The dot
 * names are component rules, so "." is a valid mountpoint, and so is the
 * empty one.
 */
bool
ifs_mountpoint_valid(const unsigned char *mountpoint, size_t length)
{
	size_t units;

	for (size_t i = 0; i < length; i++)
		if (mountpoint[i] < 0x20 || mountpoint[i] == 0x7f)
			return (false);
	return (fsimg_utf8_to_utf16((const char *)mountpoint, length, NULL, 0,
	    &units, NULL) == FSIMG_OK);
}
