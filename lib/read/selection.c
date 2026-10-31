/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "selection.h"
#include "unicode.h"

/* How an entry relates to a member, ordered by precedence: its relation to
 * several members is the greatest any of them gives it. Only an entry
 * outside every member is passed over, and under FILE one that only leads
 * to the member.
 */
typedef enum selection_relation {
	SELECTION_OUTSIDE,
	SELECTION_ANCESTOR,
	SELECTION_TARGET,
	SELECTION_DESCENDANT
} selection_relation_t;

/*
 * One member's progress through the walk. path, length bytes long, is its
 * spelling in the selection's buffer; next is the offset of its first
 * unresolved component, length once it has resolved, and depth counts the
 * components resolved so far. While the member is live, every entry the
 * walk returns starts with those components: the walk is depth first, so
 * it stays below the member's resolved prefix until it returns an entry at
 * that depth or less, and never returns below the prefix again. The member
 * stops being live there, or as soon as it resolves to an entry other than
 * a directory, whose scope is the entry itself. A member that is not live
 * selects nothing more, and one that stops being live unresolved has
 * matched nothing.
 */
typedef struct selection_member {
	char		*path;
	size_t		 length;
	size_t		 next;
	uint32_t	 depth;
	bool		 live;
} selection_member_t;

/* What one member, or the union of all of them, makes of the current
 * entry: its relation; whether the member selects a directory's children,
 * so they are in scope; and whether it needs them, which an unresolved
 * member also does to resolve its next component there.
 */
typedef struct selection_verdict {
	selection_relation_t relation;
	bool		 selects_children;
	bool		 needs_children;
} selection_verdict_t;

struct fsimg_selection {
	fsimg_reader_t	*reader;
	selection_member_t *members;
	size_t		 count;
	char		*paths;
	fsimg_selection_policy_t policy;
};

static bool member_resolved(const selection_member_t *);
static bool selection_live(const fsimg_selection_t *);
static fsimg_status_t member_not_found(const selection_member_t *,
    fsimg_error_t *);
static fsimg_status_t resolve_component(fsimg_selection_t *,
    selection_member_t *, selection_verdict_t *, fsimg_error_t *);
static fsimg_status_t relate_member(fsimg_selection_t *, selection_member_t *,
    uint32_t, selection_verdict_t *, fsimg_error_t *);
static fsimg_status_t relate_entry(fsimg_selection_t *, selection_verdict_t *,
    fsimg_error_t *);
static fsimg_status_t finish_selection(fsimg_selection_t *, fsimg_error_t *);
static fsimg_status_t advance_selection(fsimg_selection_t *, const fsimg_entry_t **,
    fsimg_error_t *);

static bool
member_resolved(const selection_member_t *member)
{
	return (member->next == member->length);
}

/* Whether some member can still select an entry. */
static bool
selection_live(const fsimg_selection_t *selection)
{
	for (size_t i = 0; i < selection->count; i++) {
		if (selection->members[i].live)
			return (true);
	}
	return (false);
}

static fsimg_status_t
member_not_found(const selection_member_t *member, fsimg_error_t *error)
{
	fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_READER,
	    "selected image path was not found");
	fsimg_error_path(error, member->path);
	return (FSIMG_EINVAL);
}

fsimg_status_t
fsimg_selection_new(fsimg_reader_t *reader, const char *const *members,
    size_t count, fsimg_selection_policy_t policy, fsimg_selection_t **out,
    fsimg_error_t *error)
{
	static const char *const whole_volume[] = { "/" };
	fsimg_selection_t *selection;
	fsimg_error_t detail;
	uint64_t total, bytes;
	size_t length, members_size, paths_size, used;
	uint32_t depth;
	fsimg_status_t status;

	if (out != NULL)
		*out = NULL;
	if (reader == NULL || out == NULL || (members == NULL && count != 0) ||
	    policy < FSIMG_SELECTION_TREE || policy > FSIMG_SELECTION_FILE)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_READER, "invalid reader selection arguments"));
	status = fsimg_ctx_ready(reader->ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (reader->state != FSIMG_READER_NEW)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_READER, "selection requires an untouched reader"));
	if (count == 0) {
		members = whole_volume;
		count = 1;
	}
	if (policy == FSIMG_SELECTION_FILE && count != 1)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_READER, "file selection takes exactly one member"));
	/* Every member is validated, and the copies sized, before anything is
	 * allocated.
	 */
	total = 0;
	for (size_t i = 0; i < count; i++) {
		if (members[i] == NULL)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_READER, "invalid reader selection arguments"));
		status = fsimg_path_validate(reader->ctx, members[i], true,
		    &length, &depth, error);
		if (status != FSIMG_OK)
			return (status);
		if (policy == FSIMG_SELECTION_FILE && length == 1)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_READER, "selection requires a regular file"));
		if (!fsimg_add_u64(total, (uint64_t)length + 1, &total))
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_READER, "selection size overflow"));
	}
	if (!fsimg_mul_u64(count, sizeof(selection_member_t), &bytes) ||
	    !fsimg_to_size(bytes, &members_size) ||
	    !fsimg_to_size(total, &paths_size))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_READER, "selection size overflow"));

	selection = fsimg_alloc(reader->ctx, sizeof(*selection), &detail);
	if (selection == NULL)
		goto failed;
	*selection = (fsimg_selection_t){ .reader = reader, .policy = policy };
	selection->members = fsimg_alloc(reader->ctx, members_size, &detail);
	if (selection->members == NULL)
		goto failed;
	selection->paths = fsimg_alloc(reader->ctx, paths_size, &detail);
	if (selection->paths == NULL)
		goto failed;
	/* Every member starts live at the root, its "/" resolved. The copy is
	 * bounded by the space validation measured, whatever the borrowed
	 * strings hold now.
	 */
	used = 0;
	for (size_t i = 0; i < count; i++) {
		length = strnlen(members[i], paths_size - used);
		if (length == paths_size - used) {
			fsimg_error_set(&detail, FSIMG_EINVAL, 0,
			    FSIMG_OP_READER,
			    "selection member changed while it was copied");
			goto failed;
		}
		bcopy(members[i], selection->paths + used, length + 1);
		selection->members[i] = (selection_member_t){
		    .path = selection->paths + used, .length = length,
		    .next = 1, .depth = 0, .live = true };
		used += length + 1;
	}
	selection->count = count;
	*out = selection;
	return (FSIMG_OK);
failed:
	fsimg_selection_free(selection);
	if (error != NULL)
		*error = detail;
	return (detail.status);
}

/*
 * The current entry is a child of the member's resolved prefix: if its name
 * matches the member's next component, the member resolves one component
 * further, to an ancestor of what it names or to its target.
 */
static fsimg_status_t
resolve_component(fsimg_selection_t *selection, selection_member_t *member,
    selection_verdict_t *verdict, fsimg_error_t *error)
{
	fsimg_reader_t *reader = selection->reader;
	const fsimg_entry_t *entry = &reader->entry;
	char *component, *separator;
	bool matches;
	fsimg_status_t status;

	component = member->path + member->next;
	separator = strchr(component, '/');
	if (separator != NULL)
		*separator = '\0';
	status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
	if (status == FSIMG_OK)
		status = fsimg_reader_match_name(reader, component, &matches,
		    error);
	if (separator != NULL)
		*separator = '/';
	/* An unrepresentable component is the member's fault, not the
	 * image's.
	 */
	if (status == FSIMG_EINVAL)
		fsimg_error_path(error, member->path);
	if (status != FSIMG_OK || !matches)
		return (status);
	if ((separator != NULL && entry->kind != FSIMG_ENTRY_DIRECTORY) ||
	    (separator == NULL && selection->policy == FSIMG_SELECTION_FILE &&
	    entry->kind != FSIMG_ENTRY_FILE &&
	    entry->kind != FSIMG_ENTRY_SYMLINK)) {
		fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_READER,
		    "selected image path has the wrong entry kind");
		fsimg_error_path(error, member->path);
		return (FSIMG_EINVAL);
	}

	member->depth++;
	member->next = separator == NULL ? member->length :
	    (size_t)(separator - member->path) + 1;
	if (!member_resolved(member)) {
		*verdict = (selection_verdict_t){
		    .relation = SELECTION_ANCESTOR,
		    .needs_children = true };
		return (FSIMG_OK);
	}
	*verdict = (selection_verdict_t){ .relation = SELECTION_TARGET,
	    .selects_children = true, .needs_children = true };
	if (entry->kind != FSIMG_ENTRY_DIRECTORY)
		member->live = false;
	return (FSIMG_OK);
}

/*
 * Relate the current entry, depth components below the root, to one
 * member. A resolved member's scope is its target's subtree, or under
 * CHILDREN the target's immediate children, whose own children it neither
 * selects nor needs.
 */
static fsimg_status_t
relate_member(fsimg_selection_t *selection, selection_member_t *member,
    uint32_t depth, selection_verdict_t *verdict, fsimg_error_t *error)
{
	*verdict = (selection_verdict_t){ .relation = SELECTION_OUTSIDE };
	if (!member->live)
		return (FSIMG_OK);
	/* The walk has left the member's resolved prefix for good. */
	if (depth <= member->depth) {
		member->live = false;
		return (member_resolved(member) ? FSIMG_OK :
		    member_not_found(member, error));
	}
	if (member_resolved(member)) {
		if (selection->policy != FSIMG_SELECTION_CHILDREN)
			*verdict = (selection_verdict_t){
			    .relation = SELECTION_DESCENDANT,
			    .selects_children = true, .needs_children = true };
		else if (depth == member->depth + 1)
			verdict->relation = SELECTION_DESCENDANT;
		return (FSIMG_OK);
	}
	if (depth != member->depth + 1)
		return (FSIMG_OK);
	return (resolve_component(selection, member, verdict, error));
}

/* Every member relates to every entry, so members that name one entry,
 * through repetition, nesting or a format's aliases, all resolve on it and
 * it is still returned once.
 */
static fsimg_status_t
relate_entry(fsimg_selection_t *selection, selection_verdict_t *verdict,
    fsimg_error_t *error)
{
	const fsimg_entry_t *entry = &selection->reader->entry;
	selection_verdict_t member;
	uint32_t depth;
	fsimg_status_t status;

	*verdict = (selection_verdict_t){ .relation = SELECTION_OUTSIDE };
	if (entry->path == NULL || entry->path[0] != '/')
		return (fsimg_error_set(error, FSIMG_ECORRUPT, 0,
		    FSIMG_OP_READER, "reader supplied a non-absolute image path"));
	/* A delivered component holds no '/', so separators count components. */
	depth = 0;
	for (const char *c = entry->path; *c != '\0'; c++) {
		if (*c == '/')
			depth++;
	}
	for (size_t i = 0; i < selection->count; i++) {
		status = relate_member(selection, &selection->members[i], depth,
		    &member, error);
		if (status != FSIMG_OK)
			return (status);
		if (member.relation > verdict->relation)
			verdict->relation = member.relation;
		verdict->selects_children |= member.selects_children;
		verdict->needs_children |= member.needs_children;
	}
	return (FSIMG_OK);
}

/* The walk is over, or no member can select anything more. A member still
 * unresolved matched nothing.
 */
static fsimg_status_t
finish_selection(fsimg_selection_t *selection, fsimg_error_t *error)
{
	for (size_t i = 0; i < selection->count; i++) {
		if (!member_resolved(&selection->members[i]))
			return (member_not_found(&selection->members[i], error));
	}
	return (fsimg_reader_end_scope(selection->reader));
}

static fsimg_status_t
advance_selection(fsimg_selection_t *selection, const fsimg_entry_t **out,
    fsimg_error_t *error)
{
	fsimg_reader_t *reader = selection->reader;
	const fsimg_entry_t *entry;
	selection_verdict_t verdict;
	bool root_selected;
	fsimg_status_t status;

	*out = NULL;
	/* Omitted entries count only where a member selects them: in the root
	 * only for a member "/". The root's scope is set as the walk starts,
	 * so a selection never used leaves the reader as it was.
	 */
	if (reader->state == FSIMG_READER_NEW) {
		root_selected = false;
		for (size_t i = 0; i < selection->count; i++) {
			if (selection->members[i].length == 1)
				root_selected = true;
		}
		fsimg_walk_scope_children(reader, root_selected);
	}
	for (;;) {
		if (!selection_live(selection))
			return (finish_selection(selection, error));
		status = fsimg_reader_next(reader, &entry, error);
		if (status == FSIMG_DONE)
			return (finish_selection(selection, error));
		if (status != FSIMG_OK)
			return (status);
		/* Every member relates to every entry, so each entry costs
		 * one work unit per member.
		 */
		status = fsimg_session_charge(&reader->session, 0, 0,
		    selection->count, error);
		if (status != FSIMG_OK)
			return (status);
		status = relate_entry(selection, &verdict, error);
		if (status != FSIMG_OK)
			return (status);
		/* A directory no member needs is pruned unopened. One that is
		 * descended counts its omitted children only when some member
		 * selects them.
		 */
		if (entry->kind == FSIMG_ENTRY_DIRECTORY) {
			if (!verdict.needs_children) {
				status = fsimg_reader_skip_directory(reader,
				    error);
				if (status != FSIMG_OK)
					return (status);
			} else {
				fsimg_walk_scope_children(reader,
				    verdict.selects_children);
			}
		}
		if (verdict.relation == SELECTION_OUTSIDE ||
		    (verdict.relation == SELECTION_ANCESTOR &&
		    selection->policy == FSIMG_SELECTION_FILE))
			continue;
		*out = entry;
		return (FSIMG_OK);
	}
}

fsimg_status_t
fsimg_selection_next(fsimg_selection_t *selection, const fsimg_entry_t **out,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	*out = NULL;
	status = fsimg_ctx_ready(selection->reader->ctx, error);
	if (status != FSIMG_OK)
		return (status);
	/* A failed selection stays failed, whatever its members' state. */
	if (selection->reader->state == FSIMG_READER_FAILED)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_READER, "reader has failed"));
	status = advance_selection(selection, out, error);
	/* The traversal owner closes the failed reader at one boundary; callers
	 * cannot accidentally keep a current entry after a failed selection.
	 */
	if (status != FSIMG_OK && status != FSIMG_DONE) {
		*out = NULL;
		return (fsimg_reader_abort(selection->reader, status));
	}
	return (status);
}

void
fsimg_selection_free(fsimg_selection_t *selection)
{
	if (selection == NULL)
		return;
	fsimg_dealloc(selection->members);
	fsimg_dealloc(selection->paths);
	fsimg_dealloc(selection);
}
