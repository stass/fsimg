/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_DIRECTORY_H
#define FSIMG_DIRECTORY_H

#include "meta.h"

/* Prepared directories and the common walker (specs/design-directories.md).
 * A driver prepares one directory at a time by admitting each child with its
 * name and a private payload. Common code owns everything else: the child's
 * class, admission limits and charging, namespace validation, traversal
 * order, deferred descent, paths and name matching.
 */

/* How preparation classes the children of a format, as its descriptor
 * declares. Without a profile the driver derives validated UTF-8 names by
 * its own rules and every child is delivered. Under the QNX-family profile
 * a child whose natively legal name lies outside fsimg_name_in_qnx_profile()
 * is omitted: validated, charged and keyed like any other, but never
 * delivered to a caller. Common code branches on this property, never on a
 * format name, and no driver chooses a class.
 */
typedef enum fsimg_name_profile {
	FSIMG_NAME_PROFILE_NONE = 0,
	FSIMG_NAME_PROFILE_QNX
} fsimg_name_profile_t;

/* A component and its NUL: room for any component of at most 1024 bytes,
 * the longest an ImageFS path stores, which includes every UTF-8 component
 * of at most 255 UTF-16 units (765 bytes). Each driver bounds its own names.
 */
#define FSIMG_DIRECTORY_NAME_BYTES 1025u
#define FSIMG_DIRECTORY_PAYLOAD_BYTES 64u
/* A record is a class byte, the component and its NUL, and the payload. */
#define FSIMG_DIRECTORY_RECORD_BYTES \
	(1u + FSIMG_DIRECTORY_NAME_BYTES + FSIMG_DIRECTORY_PAYLOAD_BYTES)
/* A namespace key: opaque bytes the driver derives from one spelling. Every
 * key buffer handed to a driver holds this many bytes: the exact bytes of a
 * 1024-byte component, and four bytes for each of a component's at most 255
 * UTF-16 units.
 */
#define FSIMG_DIRECTORY_KEY_BYTES 1024u
#define FSIMG_DIRECTORY_KEYS 2u

/* The keys one entry answers to, such as a long name and its alias. */
typedef struct fsimg_directory_keys {
	unsigned	 count;
	size_t		 length[FSIMG_DIRECTORY_KEYS];
	unsigned char	 key[FSIMG_DIRECTORY_KEYS][FSIMG_DIRECTORY_KEY_BYTES];
} fsimg_directory_keys_t;

/* One directory under preparation, private to fsimg_reader_ops_t.prepare. */
typedef struct fsimg_directory_build fsimg_directory_build_t;
typedef struct fsimg_walk fsimg_walk_t;

/* Level 0 is the root; a deeper level is the current entry's directory. */
uint32_t fsimg_directory_level(const fsimg_directory_build_t *);
/* Admit one child whose name passed the driver's native checks: a nonempty
 * NUL-terminated component without a slash, neither "." nor "..", and for
 * a format without a name profile valid UTF-8. Preparation classes it by
 * the format's profile, enforces the depth and path limits, charges the
 * session, spools its record and records its namespace keys, whatever its
 * class, so a duplicate of an omitted name is still a duplicate.
 */
fsimg_status_t fsimg_directory_admit(fsimg_directory_build_t *, const char *,
    const void *, size_t, fsimg_error_t *);

#endif
