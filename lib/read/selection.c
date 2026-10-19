/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "selection.h"
#include "unicode.h"

struct fsimg_selection {
	fsimg_reader_t	*reader;
	char		*requested;
	char		*resolved;
	size_t		 requested_length;
	size_t		 resolved_length;
	size_t		 next_component;
	fsimg_selection_policy_t policy;
};

static bool selection_found(const fsimg_selection_t *);
static fsimg_status_t relate_entry(fsimg_selection_t *,
    fsimg_selection_relation_t *, fsimg_error_t *);
static fsimg_status_t advance_selection(fsimg_selection_t *, const fsimg_entry_t **,
    fsimg_selection_relation_t *, fsimg_error_t *);

static bool
selection_found(const fsimg_selection_t *selection)
{
	return (selection->next_component == selection->requested_length);
}

fsimg_status_t
fsimg_selection_new(fsimg_reader_t *reader, const char *path,
    fsimg_selection_policy_t policy, fsimg_selection_t **out, fsimg_error_t *error)
{
	fsimg_selection_t *selection;
	fsimg_error_t detail;
	size_t length;
	uint32_t depth;
	fsimg_status_t status;

	if (out != NULL)
		*out = NULL;
	if (reader == NULL || out == NULL || policy < FSIMG_SELECTION_TREE ||
	    policy > FSIMG_SELECTION_FILE)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_READER, "invalid reader selection arguments"));
	status = fsimg_ctx_ready(reader->ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (reader->state != FSIMG_READER_NEW)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_READER, "selection requires an untouched reader"));
	if (path == NULL)
		path = "/";
	status = fsimg_path_validate(reader->ctx, path, true, &length, &depth, error);
	if (status != FSIMG_OK)
		return (status);
	if (policy == FSIMG_SELECTION_FILE && length == 1)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_READER, "selection requires a regular file"));

	selection = fsimg_alloc(reader->ctx, sizeof(*selection), &detail);
	if (selection == NULL)
		goto failed;
	selection->reader = reader;
	selection->policy = policy;
	selection->requested = fsimg_alloc(reader->ctx, length + 1, &detail);
	if (selection->requested == NULL)
		goto failed;
	/* An alias can be shorter than its displayed path. Bound the resolved
	 * spelling by the reader's path ceiling rather than the query length.
	 */
	selection->resolved = fsimg_alloc(reader->ctx,
	    reader->ctx->options.max_path_bytes, &detail);
	if (selection->resolved == NULL)
		goto failed;
	bcopy(path, selection->requested, length + 1);
	selection->requested_length = length;
	selection->next_component = 1;
	*out = selection;
	return (FSIMG_OK);
failed:
	fsimg_selection_free(selection);
	if (error != NULL)
		*error = detail;
	return (detail.status);
}

/* The resolved prefix starts empty for the implicit root. Only a child of
 * that exact displayed prefix can resolve the next requested component.
 * Once the target resolves, component-boundary prefix checks identify its
 * descendants without comparing aliases or query spellings again.
 */
static fsimg_status_t
relate_entry(fsimg_selection_t *selection, fsimg_selection_relation_t *relation,
    fsimg_error_t *error)
{
	fsimg_reader_t *reader = selection->reader;
	const fsimg_entry_t *entry = &reader->entry;
	const char *name;
	char *component, *separator;
	size_t parent_length, length;
	bool matches;
	fsimg_status_t status;

	*relation = FSIMG_SELECTION_OUTSIDE;
	if (entry->path == NULL || entry->path[0] != '/')
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_READER, "reader supplied a non-absolute image path"));
	if (selection_found(selection)) {
		if (selection->resolved_length == 0 ||
		    (strncmp(selection->resolved, entry->path, selection->resolved_length) == 0 &&
		    entry->path[selection->resolved_length] == '/'))
			*relation = FSIMG_SELECTION_DESCENDANT;
		return (FSIMG_OK);
	}
	name = strrchr(entry->path, '/');
	parent_length = (size_t)(name - entry->path);
	if (parent_length != selection->resolved_length ||
	    bcmp(entry->path, selection->resolved, parent_length) != 0)
		return (FSIMG_OK);
	component = selection->requested + selection->next_component;
	separator = strchr(component, '/');
	if (separator != NULL)
		*separator = '\0';
	status = fsimg_reader_match_name(reader, component, &matches, error);
	if (separator != NULL)
		*separator = '/';
	if (status != FSIMG_OK || !matches)
		return (status);
	if ((separator != NULL && entry->kind != FSIMG_ENTRY_DIRECTORY) ||
	    (separator == NULL && selection->policy == FSIMG_SELECTION_FILE &&
	    entry->kind != FSIMG_ENTRY_FILE && entry->kind != FSIMG_ENTRY_SYMLINK))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_READER, "selected image path has the wrong entry kind"));

	length = strlen(entry->path);
	if (length >= reader->ctx->options.max_path_bytes)
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_READER, "resolved image path exceeds its reader buffer"));
	bcopy(entry->path, selection->resolved, length + 1);
	selection->resolved_length = length;
	selection->next_component = separator == NULL ? selection->requested_length :
	    (size_t)(separator - selection->requested) + 1;
	*relation = selection_found(selection) ? FSIMG_SELECTION_TARGET : FSIMG_SELECTION_ANCESTOR;
	return (FSIMG_OK);
}

static fsimg_status_t
advance_selection(fsimg_selection_t *selection, const fsimg_entry_t **out,
    fsimg_selection_relation_t *relation, fsimg_error_t *error)
{
	fsimg_reader_t *reader = selection->reader;
	const fsimg_entry_t *entry;
	fsimg_status_t status;

	*out = NULL;
	*relation = FSIMG_SELECTION_OUTSIDE;
	/* A selected entry other than a directory has no subtree to visit. */
	if (selection_found(selection) && reader->entry.path != NULL &&
	    reader->entry.kind != FSIMG_ENTRY_DIRECTORY &&
	    strcmp(reader->entry.path, selection->resolved) == 0)
		return (fsimg_reader_end_scope(reader));
	for (;;) {
		status = fsimg_reader_next(reader, &entry, error);
		if (status == FSIMG_DONE && !selection_found(selection))
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_READER, "selected image path was not found"));
		if (status != FSIMG_OK)
			return (status);
		status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
		if (status != FSIMG_OK)
			return (status);
		status = relate_entry(selection, relation, error);
		if (status != FSIMG_OK)
			return (status);
		if (*relation == FSIMG_SELECTION_OUTSIDE && selection_found(selection))
			return (fsimg_reader_end_scope(reader));
		if (entry->kind == FSIMG_ENTRY_DIRECTORY &&
		    (*relation == FSIMG_SELECTION_OUTSIDE ||
		    (*relation == FSIMG_SELECTION_DESCENDANT &&
		    selection->policy == FSIMG_SELECTION_CHILDREN))) {
			status = fsimg_reader_skip_directory(reader, error);
			if (status != FSIMG_OK)
				return (status);
		}
		if (*relation == FSIMG_SELECTION_OUTSIDE ||
		    (*relation == FSIMG_SELECTION_ANCESTOR && selection->policy == FSIMG_SELECTION_FILE))
			continue;
		*out = entry;
		return (FSIMG_OK);
	}
}

fsimg_status_t
fsimg_selection_next(fsimg_selection_t *selection, const fsimg_entry_t **out,
    fsimg_selection_relation_t *relation, fsimg_error_t *error)
{
	fsimg_status_t status;

	*out = NULL;
	*relation = FSIMG_SELECTION_OUTSIDE;
	status = fsimg_ctx_ready(selection->reader->ctx, error);
	if (status != FSIMG_OK)
		return (status);
	status = advance_selection(selection, out, relation, error);
	/* The traversal owner closes the failed reader at one boundary; callers
	 * cannot accidentally keep a current entry after a failed selection.
	 */
	if (status != FSIMG_OK && status != FSIMG_DONE) {
		*out = NULL;
		*relation = FSIMG_SELECTION_OUTSIDE;
		return (fsimg_reader_abort(selection->reader, status));
	}
	return (status);
}

void
fsimg_selection_free(fsimg_selection_t *selection)
{
	if (selection == NULL)
		return;
	fsimg_dealloc(selection->requested);
	fsimg_dealloc(selection->resolved);
	fsimg_dealloc(selection);
}
