/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
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

typedef enum fsimg_selection_relation {
	FSIMG_SELECTION_OUTSIDE,
	FSIMG_SELECTION_ANCESTOR,
	FSIMG_SELECTION_TARGET,
	FSIMG_SELECTION_DESCENDANT
} fsimg_selection_relation_t;

/* The selector borrows an untouched reader and owns two charged path buffers.
 * next owns traversal: it resolves one component at a time using the driver's
 * validated namespace, prunes unrelated directories, and returns displayed
 * paths. TREE and CHILDREN return ancestors, the target, and descendants;
 * CHILDREN prunes below immediate children. The implicit root is never an
 * entry. FILE hides ancestors and returns only the selected regular file.
 * OUTSIDE is an empty/error result, never a successful entry. DONE consumes the
 * selected scope and clears the reader's current entry, without claiming full
 * structural verification. Free before destroying its borrowed reader.
 */
fsimg_status_t fsimg_selection_new(fsimg_reader_t *, const char *,
    fsimg_selection_policy_t, fsimg_selection_t **, fsimg_error_t *);
fsimg_status_t fsimg_selection_next(fsimg_selection_t *, const fsimg_entry_t **,
    fsimg_selection_relation_t *, fsimg_error_t *);
void fsimg_selection_free(fsimg_selection_t *);

#endif
