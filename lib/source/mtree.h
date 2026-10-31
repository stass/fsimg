/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_MTREE_H
#define FSIMG_MTREE_H

#include "internal.h"
#include "sha256.h"

/* The mtree(5) manifest parser. It reads a manifest as untrusted bytes from
 * a caller's read callback, one bounded logical line at a time, and hands
 * each entry line to a caller's entry callback as one decoded, validated
 * entry. It holds no catalog, opens nothing and allocates nothing per
 * entry: its memory is one line, the current directory, one entry path and
 * the keyword values of the line and of the /set defaults, all sized from
 * the context's path limit and charged to the memory budget.
 *
 * The parser checks everything the manifest alone decides: the grammar,
 * the keyword classes, value syntax and ranges, which keywords apply to
 * which entry kind, path length and depth, and the image path's own
 * validity. What needs the host or the catalog (a contents file, a
 * generated digest, duplicates and missing parents) is the caller's.
 */

#define FSIMG_MTREE_HOLES_MAX	64u

typedef enum fsimg_mtree_content {
	FSIMG_MTREE_CONTENT_HOST,
	FSIMG_MTREE_CONTENT_ZERO,
	FSIMG_MTREE_CONTENT_SEEDED,
	FSIMG_MTREE_CONTENT_POSITIONAL
} fsimg_mtree_content_t;

/* One zero run inside generated content: length is positive, and the runs
 * of an entry are in increasing order, neither touching nor overlapping,
 * and end within the entry's size.
 */
typedef struct fsimg_mtree_hole {
	uint64_t	 offset;
	uint64_t	 length;
} fsimg_mtree_hole_t;

/* One entry line, borrowed by the entry callback for the call only.
 *
 * entry holds the absolute image path, the kind, the size and the owner,
 * mode and modification time the manifest gives. For a symbolic link,
 * entry.size is the target's length; for a file it is the size keyword's
 * value when size_given, else zero (a host file then supplies its length);
 * for a directory or FIFO it is zero, a size keyword there being validated
 * and not consulted.
 * link is the decoded target of a link and contents the decoded contents=
 * path of a host file, each NUL-terminated (decoding refuses a NUL byte)
 * and NULL when absent. content, seed and holes describe a file's bytes;
 * seed and holes are set only for seeded and positional content.
 * line and line_offset name the entry's line as a failure names it: the
 * number of its first physical line and the logical line's byte offset.
 */
typedef struct fsimg_mtree_entry {
	fsimg_entry_t	 entry;
	const char	*link;
	const char	*contents;
	const fsimg_mtree_hole_t *holes;
	uint64_t	 seed;
	uint64_t	 line;
	uint64_t	 line_offset;
	size_t		 link_length;
	size_t		 contents_length;
	size_t		 hole_count;
	fsimg_mtree_content_t content;
	bool		 size_given;
	bool		 digest_given;
	unsigned char	 digest[FSIMG_SHA256_DIGEST_LENGTH];
} fsimg_mtree_entry_t;

/* read stores up to capacity bytes at buffer and their count in *length;
 * zero means the end of the manifest. entry receives each entry line.
 * A status other than FSIMG_OK from either ends the parse with that
 * status and the callback's error record, to which the parser adds the
 * line.
 */
typedef fsimg_status_t (*fsimg_mtree_read_fn_t)(void *, unsigned char *,
    size_t, size_t *, fsimg_error_t *);
typedef fsimg_status_t (*fsimg_mtree_entry_fn_t)(void *,
    const fsimg_mtree_entry_t *, fsimg_error_t *);

typedef struct fsimg_mtree_input {
	fsimg_mtree_read_fn_t read;
	void		*read_argument;
	fsimg_mtree_entry_fn_t entry;
	void		*entry_argument;
} fsimg_mtree_input_t;

/* Parse a whole manifest. session, input and both callbacks are required;
 * a missing one is FSIMG_EINVAL. Each physical line is charged one unit of work
 * to session, which also polls cancellation. A failure while reading or
 * parsing a line names it: offset is the byte offset at which the logical
 * line starts, the message begins with its line number, and path is the
 * entry's image path once the line has one. A failure to size or allocate
 * the parser's buffers comes before any line and names none.
 */
fsimg_status_t fsimg_mtree_parse(fsimg_session_t *,
    const fsimg_mtree_input_t *, fsimg_error_t *);

/* The longest logical line for a context: two encoded paths (the entry's
 * name and a contents= path), an encoded link target and 4096 bytes of
 * other keywords, at four bytes per encoded byte.
 */
uint64_t fsimg_mtree_line_limit(const fsimg_ctx_t *);

/* Name a manifest line in a failure's record as the parser names the line
 * that failed: offset holds the logical line's byte offset and the message
 * gains the prefix "manifest line N: ". A caller that refuses an entry
 * after the parse, by its line and line_offset, so reports the same record.
 */
void fsimg_mtree_name_line(fsimg_error_t *, uint64_t, uint64_t);

#endif
