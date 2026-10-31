/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_SELECTION_H
#define FSIMG_SELECTION_H

#include "reader.h"

typedef struct fsimg_selection fsimg_selection_t;

typedef enum fsimg_selection_policy {
	FSIMG_SELECTION_TREE,
	FSIMG_SELECTION_CHILDREN,
	FSIMG_SELECTION_FILE
} fsimg_selection_policy_t;

/* The selector borrows an untouched reader and copies its members, count
 * absolute image paths, into charged storage; no member means "/". next
 * owns traversal: it walks the reader once, resolving every member one
 * component at a time using the driver's validated namespace, prunes the
 * directories no member needs, and returns displayed paths, each entry
 * once: the directories leading to each member, the member, and what it
 * selects below it. TREE selects each member's subtree, CHILDREN only a
 * directory member's immediate children. The implicit root is never an
 * entry. FILE takes one member, hides the directories leading to it and
 * returns only the selected entry with content: a regular file or a
 * symbolic link, whose content is its target. next fails naming a member
 * that matched nothing once the walk has passed every place it could
 * match. Once no member can select anything more, it returns DONE: that
 * consumes the selected scope and clears the reader's current entry,
 * without claiming full structural verification. Free before destroying
 * its borrowed reader.
 */
fsimg_status_t fsimg_selection_new(fsimg_reader_t *, const char *const *,
    size_t, fsimg_selection_policy_t, fsimg_selection_t **, fsimg_error_t *);
fsimg_status_t fsimg_selection_next(fsimg_selection_t *, const fsimg_entry_t **,
    fsimg_error_t *);
void fsimg_selection_free(fsimg_selection_t *);

#endif
