/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include "erofs.h"

/*
 * Extended attributes at read time. The reader delivers no attribute, so it
 * checks only what an entry's own inode holds: the inline region's header
 * and the chain of inline entries up to the region's end, each entry's name
 * index included. An entry that holds any attribute, shared or inline,
 * carries the extra-data mark. The shared pool, the long-prefix table and
 * the name filter are other structures, which verification checks.
 *
 * The region follows the fixed inode and is 12 + 4 * (c - 1) bytes for an
 * attribute count c > 0, at most 262148 bytes. It is read in the pieces the
 * walk needs, never whole: the 12-byte header, then the 4-byte head of each
 * inline entry, which gives the entry's padded length.
 */

static fsimg_status_t	inline_entries_check(fsimg_reader_t *,
			    const erofs_inode_t *, uint32_t, bool *,
			    fsimg_error_t *);

/*
 * Validate inode's attribute region and report in *present whether the
 * inode holds an attribute. An inode without a region holds none.
 */
fsimg_status_t
erofs_xattr_check(fsimg_reader_t *reader, const erofs_inode_t *inode,
    bool *present, fsimg_error_t *error)
{
	unsigned char raw[EROFS_XATTR_HEADER_BYTES];
	erofs_xattr_header_t header;
	const char *what;
	bool inline_present;
	fsimg_status_t status;

	*present = false;
	if (inode->xattr_bytes == 0)
		return (FSIMG_OK);

	/*
	 * The header decoder reads the 12 header bytes and checks the
	 * shared indexes against the region's length, which it is given;
	 * the indexes themselves are not read here.
	 */
	status = erofs_body_read(reader, inode, inode->fixed_bytes, sizeof(raw),
	    raw, error);
	if (status != FSIMG_OK)
		return (status);
	status = erofs_xattr_header_decode(raw, inode->xattr_bytes, &header,
	    &what);
	if (status != FSIMG_OK)
		return (erofs_reject(error, status, what));

	status = inline_entries_check(reader, inode, header.entries_offset,
	    &inline_present, error);
	if (status != FSIMG_OK)
		return (status);

	*present = header.shared_count > 0 || inline_present;
	return (FSIMG_OK);
}

/*
 * Walk the inline entries from region offset first to the region's end.
 * Each entry, padded to 4 bytes, must end inside the region, so the walk
 * ends exactly at it; the entry decoder reads the entry's 4-byte head and
 * checks the padded length against what remains of the region. Each entry
 * is one unit of work, so a region of many tiny entries is charged.
 */
static fsimg_status_t
inline_entries_check(fsimg_reader_t *reader, const erofs_inode_t *inode,
    uint32_t first, bool *present, fsimg_error_t *error)
{
	unsigned char head[EROFS_XATTR_ENTRY_BYTES];
	erofs_xattr_t xattr;
	const char *what;
	uint32_t at, remaining;
	fsimg_status_t status;

	*present = false;
	for (at = first; at < inode->xattr_bytes; at += xattr.entry_bytes) {
		remaining = inode->xattr_bytes - at;
		if (remaining < sizeof(head))
			return (erofs_reject(error, FSIMG_ECORRUPT,
			    "attribute entry truncated"));
		status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
		if (status != FSIMG_OK)
			return (status);
		status = erofs_body_read(reader, inode,
		    (uint64_t)inode->fixed_bytes + at, sizeof(head), head,
		    error);
		if (status != FSIMG_OK)
			return (status);
		status = erofs_xattr_decode(head, remaining,
		    &erofs_state(reader)->super, &xattr, &what);
		if (status != FSIMG_OK)
			return (erofs_reject(error, status, what));
		*present = true;
	}
	return (FSIMG_OK);
}
