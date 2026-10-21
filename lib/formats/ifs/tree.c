/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "ifs.h"
#include "unicode.h"

/*
 * The record table is a flat list of full relative paths, and this file is
 * the only place that reads a path as a hierarchy. It turns the table pass's
 * path stream into one group of children per directory in three bounded
 * steps over scratch storage:
 *
 * 1. Path sort. The common stable external sort orders the paths byte by
 *    byte with the separator ranked below every other byte. That order is
 *    a preorder of the tree: a path sorts after each of its prefixes, every
 *    subtree is contiguous, and equal paths stay in table order.
 *
 * 2. Child pass. One sequential read of the sorted run assigns node
 *    indexes, the root 0 and every other node in the order it is met, and
 *    emits a child record (parent, node, kind, table offset, component) for
 *    each. Of a run of equal paths only the last record in table order is
 *    the entry; the earlier ones are shadowed, counted and never emitted.
 *    An ancestor stack holds the nodes of the last emitted path, one level
 *    per component; a new path pops it to the components the two share,
 *    whose top is the path's nearest existing ancestor, synthesizes a
 *    directory for each further component but the last, and then emits
 *    the path's own node. Every emitted node is pushed whatever its kind,
 *    so a path below a file, link or FIFO meets that node on the stack and
 *    is shadowed there, as procnto never reaches a record stored below a
 *    link: nothing is synthesized or emitted for it, and the stack keeps
 *    the non-directory on top for the rest of its subtree. A node is
 *    pushed only onto a directory, so only the top of the stack can be a
 *    non-directory.
 *
 * 3. Group sort and table. A stable sort of the child stream by parent
 *    makes each directory's children one contiguous run, still in
 *    component order, and one pass records each run's offset and count in
 *    the paged group table.
 *
 * Nothing holds the table or the tree in RAM: only the stack, at most 513
 * levels for a 1024-byte path, and the previous path are resident during
 * the child pass. Every node has exactly one parent, the one its path
 * names, so the result is a tree by construction.
 */

/* One level of the ancestor stack: a node, its kind, whether it or an
 * ancestor lies outside the name profile, and where its component ends in
 * the stack's path.
 */
typedef struct tree_level {
	uint64_t	 node;
	size_t		 path_end;
	ifs_kind_t	 kind;
	bool		 hidden;
} tree_level_t;

/* A decoded path-stream record. */
typedef struct path_entry {
	const unsigned char *path;
	size_t		 path_length;
	uint32_t	 table_offset;
	uint32_t	 inode;
	ifs_kind_t	 kind;
} path_entry_t;

/* The child pass. levels[0] is the root and levels[1 .. depth] hold the
 * components of path, the last emitted node's path. pending holds the
 * sorted record read last, whose fate waits on the record after it.
 */
typedef struct tree_pass {
	fsimg_reader_t	*reader;
	fsimg_stream_t	*children;
	uint64_t	 next_node;
	uint32_t	 depth;
	size_t		 path_length;
	bool		 root_done;
	tree_level_t	 levels[IFS_DEPTH_MAX + 1];
	unsigned char	 path[IFS_PATH_MAX];
	unsigned char	 pending[IFS_PATH_ENTRY_BYTES];
	unsigned char	 record[IFS_PATH_ENTRY_BYTES];
	unsigned char	 child[IFS_CHILD_BYTES];
} tree_pass_t;

static int path_compare(const void *, size_t, const void *, size_t, void *);
static int parent_compare(const void *, size_t, const void *, size_t, void *);
static fsimg_status_t path_entry_decode(const unsigned char *, size_t,
    path_entry_t *, fsimg_error_t *);
static void match_references(fsimg_ifs_reader_t *, uint32_t, ifs_kind_t,
    bool);
static void shadow_record(fsimg_ifs_reader_t *, uint32_t);
static fsimg_status_t emit_node(tree_pass_t *, const unsigned char *, size_t,
    const path_entry_t *, fsimg_error_t *);
static bool level_names(const tree_pass_t *, uint32_t, const unsigned char *,
    size_t);
static fsimg_status_t place_entry(tree_pass_t *, const path_entry_t *,
    fsimg_error_t *);
static fsimg_status_t child_pass(tree_pass_t *, fsimg_stream_t *,
    fsimg_error_t *);
static fsimg_status_t group_table(fsimg_reader_t *, fsimg_error_t *);

/* Component order as one byte loop: the separator ranks below every other
 * byte, and a path never holds a NUL, so mapping '/' to 0 orders a path
 * before its siblings' longer names ("a/b" before "a-b") and after its own
 * prefixes. Records too short to hold the fixed fields order by length so
 * the order stays total; path_entry_decode() rejects them later.
 */
static int
path_compare(const void *a, size_t a_length, const void *b, size_t b_length,
    void *argument FSIMG_UNUSED)
{
	const unsigned char *left = a, *right = b;
	size_t length;

	if (a_length < IFS_PATH_ENTRY_FIXED || b_length < IFS_PATH_ENTRY_FIXED)
		return ((a_length > b_length) - (a_length < b_length));
	length = a_length < b_length ? a_length : b_length;
	for (size_t i = IFS_PATH_ENTRY_FIXED; i < length; i++) {
		unsigned l = left[i] == '/' ? 0 : left[i];
		unsigned r = right[i] == '/' ? 0 : right[i];

		if (l != r)
			return (l < r ? -1 : 1);
	}
	return ((a_length > b_length) - (a_length < b_length));
}

/* Child records ordered by parent node; the stable sort keeps component
 * order within each parent.
 */
static int
parent_compare(const void *a, size_t a_length, const void *b, size_t b_length,
    void *argument FSIMG_UNUSED)
{
	uint64_t left, right;

	if (a_length < IFS_CHILD_FIXED || b_length < IFS_CHILD_FIXED)
		return ((a_length > b_length) - (a_length < b_length));
	left = fsimg_meta_get_u64(a);
	right = fsimg_meta_get_u64(b);
	return ((left > right) - (left < right));
}

static fsimg_status_t
path_entry_decode(const unsigned char *bytes, size_t length,
    path_entry_t *entry, fsimg_error_t *error)
{
	if (length < IFS_PATH_ENTRY_FIXED || length > IFS_PATH_ENTRY_BYTES ||
	    bytes[IFS_PATH_ENTRY_KIND] < IFS_KIND_DIRECTORY ||
	    bytes[IFS_PATH_ENTRY_KIND] > IFS_KIND_FIFO)
		return (ifs_corrupt(error, "invalid ImageFS path record"));
	entry->table_offset = fsimg_meta_get_u32(bytes +
	    IFS_PATH_ENTRY_TABLE_OFFSET);
	entry->inode = fsimg_meta_get_u32(bytes + IFS_PATH_ENTRY_INODE);
	entry->kind = (ifs_kind_t)bytes[IFS_PATH_ENTRY_KIND];
	entry->path = bytes + IFS_PATH_ENTRY_FIXED;
	entry->path_length = length - IFS_PATH_ENTRY_FIXED;
	return (FSIMG_OK);
}

/* A node with a record matches each nonzero boot or script reference that
 * carries its full inode word. A match below or at a name outside the
 * profile names a node fsimg cannot deliver, which info reports.
 */
static void
match_references(fsimg_ifs_reader_t *ifs, uint32_t inode, ifs_kind_t kind,
    bool hidden)
{
	uint32_t reference;

	for (unsigned i = 0; i < IFS_REFERENCES; i++) {
		reference = ifs_reference(&ifs->header, i);
		if (reference == 0 || reference != inode)
			continue;
		ifs->counts.reference_matches[i]++;
		ifs->counts.reference_kind[i] = kind;
		if (hidden)
			ifs->counts.omitted_references++;
	}
}

/* Count one shadowed record, and mark each nonzero boot or script
 * reference that carries its inode word: a shadowed record is never
 * matched, so a startup that found references by scanning the table could
 * load it instead of the delivered node, and info reports how many
 * references are exposed. Both kinds of shadowing pass through here.
 */
static void
shadow_record(fsimg_ifs_reader_t *ifs, uint32_t inode)
{
	uint32_t reference;

	ifs->counts.shadowed++;
	for (unsigned i = 0; i < IFS_REFERENCES; i++) {
		reference = ifs_reference(&ifs->header, i);
		if (reference != 0 && reference == inode)
			ifs->counts.reference_shadowed[i] = true;
	}
}

/* Emit a child of the stack's top for component name and push it: the
 * node of entry, the record at its path, or a synthesized directory when
 * entry is NULL. The profile decides here, by the same function
 * preparation applies, whether the node is hidden: the root of an omitted
 * subtree is counted once, and nothing below it is.
 */
static fsimg_status_t
emit_node(tree_pass_t *pass, const unsigned char *name, size_t length,
    const path_entry_t *entry, fsimg_error_t *error)
{
	fsimg_ifs_reader_t *ifs = ifs_state(pass->reader);
	tree_level_t *parent = &pass->levels[pass->depth];
	tree_level_t *level;
	ifs_kind_t kind;
	uint32_t table_offset, path_length;
	size_t separator;
	bool outside;
	fsimg_status_t status;

	/* The stack's path is a prefix of a path of at most 1024 bytes. */
	separator = pass->depth > 0 ? 1 : 0;
	if (pass->depth >= IFS_DEPTH_MAX ||
	    pass->path_length + separator + length > IFS_PATH_MAX)
		return (ifs_corrupt(error, "ImageFS path order is inconsistent"));
	status = fsimg_session_charge(&pass->reader->session, 0, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	kind = entry == NULL ? IFS_KIND_DIRECTORY : entry->kind;
	table_offset = entry == NULL ? 0 : entry->table_offset;
	path_length = entry == NULL ? 0 : (uint32_t)entry->path_length;
	fsimg_meta_put_u64(pass->child + IFS_CHILD_PARENT, parent->node);
	fsimg_meta_put_u64(pass->child + IFS_CHILD_NODE, pass->next_node);
	pass->child[IFS_CHILD_KIND] = (unsigned char)kind;
	fsimg_meta_put_u32(pass->child + IFS_CHILD_TABLE_OFFSET, table_offset);
	pass->child[IFS_CHILD_PATH_LENGTH] = (unsigned char)(path_length & 0xff);
	pass->child[IFS_CHILD_PATH_LENGTH + 1] =
	    (unsigned char)(path_length >> 8);
	bcopy(name, pass->child + IFS_CHILD_FIXED, length);
	status = fsimg_stream_append(pass->children, pass->child,
	    IFS_CHILD_FIXED + length, NULL, error);
	if (status != FSIMG_OK)
		return (status);

	outside = !fsimg_name_in_qnx_profile((const char *)name, length);
	if (outside && !parent->hidden)
		ifs->counts.omitted++;
	if (separator != 0)
		pass->path[pass->path_length++] = '/';
	bcopy(name, pass->path + pass->path_length, length);
	pass->path_length += length;
	level = &pass->levels[++pass->depth];
	level->node = pass->next_node++;
	level->path_end = pass->path_length;
	level->kind = kind;
	level->hidden = parent->hidden || outside;
	if (entry == NULL)
		ifs->counts.implicit_directories++;
	else
		match_references(ifs, entry->inode, kind, level->hidden);
	return (FSIMG_OK);
}

/* Whether stack level level, 1 to depth, holds the component name. */
static bool
level_names(const tree_pass_t *pass, uint32_t level, const unsigned char *name,
    size_t length)
{
	size_t start;

	start = level == 1 ? 0 : pass->levels[level - 1].path_end + 1;
	return (pass->levels[level].path_end - start == length &&
	    bcmp(pass->path + start, name, length) == 0);
}

/* Place one entry, the last record of its path, in the tree, or count it
 * shadowed when its nearest ancestor with a node is not a directory.
 */
static fsimg_status_t
place_entry(tree_pass_t *pass, const path_entry_t *entry,
    fsimg_error_t *error)
{
	fsimg_ifs_reader_t *ifs = ifs_state(pass->reader);
	size_t start, end;
	uint32_t shared;
	fsimg_status_t status;

	/* The empty path sorts first, so it can only be the first entry. */
	if (entry->path_length == 0) {
		if (pass->root_done)
			return (ifs_corrupt(error,
			    "ImageFS path order is inconsistent"));
		pass->root_done = true;
		match_references(ifs, entry->inode, entry->kind, false);
		return (FSIMG_OK);
	}
	if (!pass->root_done) {
		pass->root_done = true;
		ifs->counts.implicit_directories++;
	}

	/* Pop the stack to the components the path shares with it. In sorted
	 * order a path never equals a node already emitted, so its last
	 * component never matches.
	 */
	shared = 0;
	start = 0;
	end = ifs_component_end(entry->path, entry->path_length, start);
	while (shared < pass->depth && level_names(pass, shared + 1,
	    entry->path + start, end - start)) {
		if (end == entry->path_length)
			return (ifs_corrupt(error,
			    "ImageFS path order is inconsistent"));
		shared++;
		start = end + 1;
		end = ifs_component_end(entry->path, entry->path_length, start);
	}
	pass->depth = shared;
	pass->path_length = pass->levels[shared].path_end;
	if (pass->levels[shared].kind != IFS_KIND_DIRECTORY) {
		shadow_record(ifs, entry->inode);
		return (FSIMG_OK);
	}

	/* Synthesize the parents the table omits, then the entry itself. */
	while (end != entry->path_length) {
		status = emit_node(pass, entry->path + start, end - start, NULL,
		    error);
		if (status != FSIMG_OK)
			return (status);
		start = end + 1;
		end = ifs_component_end(entry->path, entry->path_length, start);
	}
	return (emit_node(pass, entry->path + start, end - start, entry,
	    error));
}

/* Read the sorted path run once with one record of lookahead: a record
 * whose successor has the same path is shadowed. A record with the empty
 * path must be a directory, whether it is the entry or shadowed.
 */
static fsimg_status_t
child_pass(tree_pass_t *pass, fsimg_stream_t *sorted, fsimg_error_t *error)
{
	fsimg_ifs_reader_t *ifs = ifs_state(pass->reader);
	path_entry_t entry, pending;
	uint64_t cursor;
	size_t length;
	bool have_pending;
	fsimg_status_t status;

	pass->levels[0] = (tree_level_t){ .node = 0, .path_end = 0,
	    .kind = IFS_KIND_DIRECTORY, .hidden = false };
	pass->next_node = 1;
	cursor = 0;
	have_pending = false;
	for (;;) {
		status = fsimg_stream_next(sorted, &cursor, pass->record,
		    sizeof(pass->record), &length, error);
		if (status == FSIMG_DONE)
			break;
		if (status == FSIMG_OK)
			status = fsimg_session_charge(&pass->reader->session, 0,
			    0, 1, error);
		if (status == FSIMG_OK)
			status = path_entry_decode(pass->record, length, &entry,
			    error);
		if (status != FSIMG_OK)
			return (status);
		if (entry.path_length == 0 && entry.kind != IFS_KIND_DIRECTORY)
			return (ifs_unsupported(error,
			    "ImageFS root record is not a directory"));
		if (have_pending) {
			if (pending.path_length == entry.path_length &&
			    bcmp(pending.path, entry.path,
			    entry.path_length) == 0) {
				shadow_record(ifs, pending.inode);
			} else {
				status = place_entry(pass, &pending, error);
				if (status != FSIMG_OK)
					return (status);
			}
		}
		bcopy(pass->record, pass->pending, length);
		status = path_entry_decode(pass->pending, length, &pending,
		    error);
		if (status != FSIMG_OK)
			return (status);
		have_pending = true;
	}
	if (have_pending) {
		status = place_entry(pass, &pending, error);
		if (status != FSIMG_OK)
			return (status);
	}
	/* An empty table has only the synthesized root. */
	if (!pass->root_done)
		ifs->counts.implicit_directories++;
	ifs->node_count = pass->next_node;
	return (FSIMG_OK);
}

/* One pass over the sorted child stream records each directory's run:
 * group[node] is the stream offset of its first child and the count.
 * Nodes without children keep the array's zeros.
 */
static fsimg_status_t
group_table(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_ifs_reader_t *ifs = ifs_state(reader);
	unsigned char record[IFS_CHILD_BYTES], group[IFS_GROUP_BYTES];
	ifs_child_t child;
	uint64_t cursor, offset, run_parent, run_offset, run_count;
	size_t length;
	fsimg_status_t status;

	status = fsimg_array_new(&reader->session, IFS_GROUP_BYTES,
	    ifs->node_count, &ifs->groups, error);
	if (status != FSIMG_OK)
		return (status);
	cursor = 0;
	run_parent = 0;
	run_offset = 0;
	run_count = 0;
	for (;;) {
		offset = cursor;
		status = fsimg_stream_next(ifs->children, &cursor, record,
		    sizeof(record), &length, error);
		if (status == FSIMG_DONE)
			break;
		if (status == FSIMG_OK)
			status = fsimg_session_charge(&reader->session, 0, 0, 1,
			    error);
		if (status == FSIMG_OK)
			status = ifs_child_decode(record, length, &child, error);
		if (status == FSIMG_OK && (child.parent >= ifs->node_count ||
		    child.parent < run_parent))
			status = ifs_corrupt(error,
			    "ImageFS child order is inconsistent");
		if (status != FSIMG_OK)
			return (status);
		if (run_count != 0 && child.parent != run_parent) {
			fsimg_meta_put_u64(group, run_offset);
			fsimg_meta_put_u64(group + 8, run_count);
			status = fsimg_array_set(ifs->groups, run_parent, group,
			    error);
			if (status != FSIMG_OK)
				return (status);
			run_count = 0;
		}
		if (run_count == 0) {
			run_parent = child.parent;
			run_offset = offset;
		}
		run_count++;
	}
	if (run_count == 0)
		return (FSIMG_OK);
	fsimg_meta_put_u64(group, run_offset);
	fsimg_meta_put_u64(group + 8, run_count);
	return (fsimg_array_set(ifs->groups, run_parent, group, error));
}

/* Derive the hierarchy from the sealed path stream, which this call
 * consumes and frees on every outcome. On success the driver holds the
 * sealed, parent-ordered child stream and the group table.
 */
fsimg_status_t
ifs_tree_build(fsimg_reader_t *reader, fsimg_stream_t *paths,
    fsimg_error_t *error)
{
	fsimg_ifs_reader_t *ifs = ifs_state(reader);
	fsimg_stream_t *sorted, *children;
	tree_pass_t *pass;
	fsimg_error_t detail;
	fsimg_status_t status;

	sorted = NULL;
	children = NULL;
	status = fsimg_stream_sort(paths, path_compare, NULL, NULL, &sorted,
	    error);
	fsimg_stream_free(paths);
	if (status != FSIMG_OK)
		return (status);

	pass = fsimg_alloc(reader->ctx, sizeof(*pass), &detail);
	if (pass == NULL) {
		fsimg_stream_free(sorted);
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	bzero(pass, sizeof(*pass));
	pass->reader = reader;
	status = fsimg_stream_new(&reader->session, IFS_CHILD_BYTES,
	    &pass->children, error);
	if (status == FSIMG_OK)
		status = child_pass(pass, sorted, error);
	if (status == FSIMG_OK)
		status = fsimg_stream_seal(pass->children, error);
	children = pass->children;
	fsimg_dealloc(pass);
	fsimg_stream_free(sorted);

	if (status == FSIMG_OK)
		status = fsimg_stream_sort(children, parent_compare, NULL, NULL,
		    &ifs->children, error);
	fsimg_stream_free(children);
	if (status == FSIMG_OK)
		status = group_table(reader, error);
	return (status);
}

/* Split a child record. Its bytes came from scratch, so every field is
 * checked: the kind, a component of 1 to 1024 bytes within the record's
 * path length, and a synthesized record (table offset zero) being a
 * directory without a path length.
 */
fsimg_status_t
ifs_child_decode(const unsigned char *bytes, size_t length, ifs_child_t *child,
    fsimg_error_t *error)
{
	if (length <= IFS_CHILD_FIXED || length > IFS_CHILD_BYTES ||
	    bytes[IFS_CHILD_KIND] < IFS_KIND_DIRECTORY ||
	    bytes[IFS_CHILD_KIND] > IFS_KIND_FIFO)
		return (ifs_corrupt(error, "invalid ImageFS child record"));
	child->parent = fsimg_meta_get_u64(bytes + IFS_CHILD_PARENT);
	child->node = fsimg_meta_get_u64(bytes + IFS_CHILD_NODE);
	child->kind = (ifs_kind_t)bytes[IFS_CHILD_KIND];
	child->table_offset = fsimg_meta_get_u32(bytes +
	    IFS_CHILD_TABLE_OFFSET);
	child->path_length = (uint32_t)bytes[IFS_CHILD_PATH_LENGTH] |
	    (uint32_t)bytes[IFS_CHILD_PATH_LENGTH + 1] << 8;
	child->name = bytes + IFS_CHILD_FIXED;
	child->name_length = length - IFS_CHILD_FIXED;
	/* Preorder numbering makes every child stream link point forward, so
	 * no child record read back from scratch can close a cycle. The
	 * walker's depth and work limits bound everything else.
	 */
	if (child->node <= child->parent)
		return (ifs_corrupt(error, "invalid ImageFS child record"));
	if (child->table_offset == 0) {
		/* A synthesized directory has no record to name a path. */
		if (child->kind != IFS_KIND_DIRECTORY || child->path_length != 0)
			return (ifs_corrupt(error,
			    "invalid ImageFS child record"));
	} else if (child->path_length > IFS_PATH_MAX ||
	    child->path_length < child->name_length)
		return (ifs_corrupt(error, "invalid ImageFS child record"));
	return (FSIMG_OK);
}

/* The children of node: their run's offset in the child stream and their
 * count, both checked against the stream and the node count.
 */
fsimg_status_t
ifs_group(fsimg_reader_t *reader, uint64_t node, uint64_t *offset,
    uint64_t *count, fsimg_error_t *error)
{
	fsimg_ifs_reader_t *ifs = ifs_state(reader);
	unsigned char group[IFS_GROUP_BYTES];
	fsimg_status_t status;

	*offset = 0;
	*count = 0;
	if (node >= ifs->node_count)
		return (ifs_corrupt(error, "invalid prepared ImageFS node"));
	status = fsimg_array_get(ifs->groups, node, group, error);
	if (status != FSIMG_OK)
		return (status);
	*offset = fsimg_meta_get_u64(group);
	*count = fsimg_meta_get_u64(group + 8);
	if (*offset > ifs->children->end || *count >= ifs->node_count)
		return (ifs_corrupt(error, "invalid ImageFS child group"));
	return (FSIMG_OK);
}
