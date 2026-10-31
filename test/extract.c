/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#if defined(__linux__)
#include <sys/xattr.h>
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "extract.h"
#include "fat.h"
#include "selection.h"
#include "test.h"

#define FILE_BYTES 70001u

extern char **environ;

typedef struct extract_fixture {
	fsimg_ctx_t	*ctx;
	fsimg_io_t	*input;
	fsimg_reader_t	*reader;
	fsimg_stats_t	 baseline;
	char		 path[64];
	int		 parent;
	bool		 cancel;
} extract_fixture_t;

static void fixture_open(extract_fixture_t *);
static void fixture_open_with_entries(extract_fixture_t *,
    const fsimg_entry_t *, size_t);
static void fixture_open_union(extract_fixture_t *);
static void fixture_close(extract_fixture_t *);
static void remove_tree(int, const char *);
static void add_entry(fsimg_writer_t *, const char *, fsimg_entry_kind_t, uint64_t);
static bool cancelled(void *);
static void test_roundtrip(const char *);
static void test_existing(void);
static void test_limits(void);
static void test_times(void);
static void test_allocations(void);
static void test_selection_allocation_failures_leave_no_output(void);
static void test_repeated_root_members_exhaust_work_limit(void);
static void test_pivot(void);
static void test_alias(void);
static void test_selection_failure(void);
static void test_selection_scope(void);
static void expect_selected_entries(extract_fixture_t *, const char *const *,
    size_t, fsimg_selection_policy_t, const char *const *, size_t);
static void test_nested_members_return_subtree_once(void);
static void test_duplicate_members_return_subtree_once(void);
static void test_case_alias_members_return_subtree_once(void);
static void test_short_alias_overlap_preserves_display_name(void);
static void test_disjoint_members_return_only_subtrees_and_ancestors(void);
static void test_missing_member_fails_reader(void);
static void test_missing_nested_member_fails_before_later_selection(void);
static void test_missing_root_member_fails_after_selected_subtree(void);
static fsimg_status_t observe_last_root_entry(fsimg_reader_t *, const char *,
    const unsigned char *, size_t, fsimg_error_t *);
static void test_non_directory_component_names_member(void);
static void test_children_members_stop_below_immediate_children(void);
static void test_file_policy_refuses_several_members(void);
static void test_no_members_selects_root(void);
static void test_null_member_array_is_invalid(void);
static void test_extract_members_union_and_manifest_once(void);
static void test_null_extraction_members_create_no_destination(void);
static void test_missing_extraction_member_names_error(void);
static fsimg_status_t posix_replay(fsimg_reader_t *, const char *,
    const unsigned char *, size_t, fsimg_error_t *);
static void present_posix_kinds(extract_fixture_t *);
static void test_unsupported_kinds(void);
static void test_link_content(void);
static bool inheritable_acl(int, const char *);
static void test_inherited_acl(void);

/* The FAT driver's operations, and a copy whose replay reports two of the
 * fixture's files as kinds FAT cannot store, so the extractor's handling of
 * links and FIFOs is exercised without a format that writes them.
 */
static const fsimg_reader_ops_t *fat_reader_ops;
static fsimg_reader_ops_t posix_reader_ops;
static bool last_root_entry_seen;

/* The four-byte "short" becomes a symbolic link whose target is its content;
 * the empty root file becomes a FIFO.
 */
static fsimg_status_t
posix_replay(fsimg_reader_t *reader, const char *name,
    const unsigned char *payload, size_t length, fsimg_error_t *error)
{
	fsimg_status_t status;

	status = fat_reader_ops->replay(reader, name, payload, length, error);
	if (status != FSIMG_OK)
		return (status);
	if (strcmp(name, "short") == 0)
		reader->entry.kind = FSIMG_ENTRY_SYMLINK;
	else if (strcmp(name, "Long root name.txt") == 0)
		reader->entry.kind = FSIMG_ENTRY_FIFO;
	return (FSIMG_OK);
}

static void
present_posix_kinds(extract_fixture_t *fixture)
{
	fat_reader_ops = fixture->reader->ops;
	posix_reader_ops = *fat_reader_ops;
	posix_reader_ops.replay = posix_replay;
	fixture->reader->ops = &posix_reader_ops;
}

/* By default a link or FIFO fails extraction and names the entry; with
 * SKIP_UNSUPPORTED it is omitted, counted, and everything else is extracted.
 */
static void
test_unsupported_kinds(void)
{
	extract_fixture_t fixture;
	fsimg_extract_options_t options;
	fsimg_error_t error;
	fsimg_stats_t stats;
	struct stat st;
	int root;

	fixture_open(&fixture);
	present_posix_kinds(&fixture);
	CHECK(mkdirat(fixture.parent, "output", 0700) == 0);
	root = openat(fixture.parent, "output", O_RDONLY | O_DIRECTORY);
	CHECK(root != -1);
	CHECK(fsimg_extract_into(fixture.reader, root, NULL, NULL, &error) ==
	    FSIMG_EUNSUPPORTED);
	CHECK(error.operation == FSIMG_OP_EXTRACT);
	CHECK(strcmp(error.path, "/A/short") == 0 ||
	    strcmp(error.path, "/Long root name.txt") == 0);
	CHECK(fstatat(root, "Long root name.txt", &st, AT_SYMLINK_NOFOLLOW) == -1);
	CHECK(close(root) == 0);
	remove_tree(fixture.parent, "output");
	CHECK(fsimg_ctx_get_stats(fixture.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.skipped_entries == 0);
	fixture_close(&fixture);

	fixture_open(&fixture);
	present_posix_kinds(&fixture);
	fsimg_extract_options_init(&options);
	options.flags |= FSIMG_EXTRACT_SKIP_UNSUPPORTED;
	CHECK(fsimg_extract_at(fixture.reader, fixture.parent, "output", &options,
	    NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.skipped_entries == 2);
	root = openat(fixture.parent, "output", O_RDONLY | O_DIRECTORY);
	CHECK(root != -1);
	CHECK(fstatat(root, "Long root name.txt", &st, AT_SYMLINK_NOFOLLOW) == -1);
	CHECK(fstatat(root, "A/short", &st, AT_SYMLINK_NOFOLLOW) == -1);
	CHECK(fstatat(root, "A/nested/file", &st, AT_SYMLINK_NOFOLLOW) == 0);
	CHECK(S_ISREG(st.st_mode) && st.st_size == FILE_BYTES);
	CHECK(fstatat(root, "A-", &st, AT_SYMLINK_NOFOLLOW) == 0);
	CHECK(close(root) == 0);
	remove_tree(fixture.parent, "output");
	fixture_close(&fixture);

	/* Selecting a link directly is the same decision for that one entry. */
	fixture_open(&fixture);
	present_posix_kinds(&fixture);
	fsimg_extract_options_init(&options);
	options.selections = (const char *const[]){ "/A/short" };
	options.selection_count = 1;
	CHECK(fsimg_extract_at(fixture.reader, fixture.parent, "output", &options,
	    &error) == FSIMG_EUNSUPPORTED);
	CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
	remove_tree(fixture.parent, "output");
	fixture_close(&fixture);
	fixture_open(&fixture);
	present_posix_kinds(&fixture);
	options.flags |= FSIMG_EXTRACT_SKIP_UNSUPPORTED;
	CHECK(fsimg_extract_at(fixture.reader, fixture.parent, "output", &options,
	    NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture.ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.skipped_entries == 1);
	root = openat(fixture.parent, "output", O_RDONLY | O_DIRECTORY);
	CHECK(root != -1);
	CHECK(fstatat(root, "A/short", &st, AT_SYMLINK_NOFOLLOW) == -1);
	CHECK(close(root) == 0);
	remove_tree(fixture.parent, "output");
	fixture_close(&fixture);
}

/* A symbolic link's content is its target, read like a file's payload; a
 * FIFO has no content.
 */
static void
test_link_content(void)
{
	extract_fixture_t fixture;
	const fsimg_entry_t *entry;
	fsimg_range_t range;
	unsigned char bytes[8];
	size_t count;
	bool link_seen, fifo_seen;

	fixture_open(&fixture);
	present_posix_kinds(&fixture);
	link_seen = fifo_seen = false;
	while (fsimg_reader_next(fixture.reader, &entry, NULL) == FSIMG_OK) {
		if (entry->kind == FSIMG_ENTRY_SYMLINK) {
			CHECK(strcmp(entry->path, "/A/short") == 0 && entry->size == 4);
			memset(bytes, 0xa5, sizeof(bytes));
			CHECK(fsimg_reader_read_at(fixture.reader, 0, bytes,
			    sizeof(bytes), &count, NULL) == FSIMG_OK);
			CHECK(count == 4 && bcmp(bytes, "\0\0\0\0", 4) == 0);
			CHECK(fsimg_reader_next_range(fixture.reader, 0, &range,
			    NULL) == FSIMG_OK);
			CHECK(range.offset == 0 && range.length == 4);
			link_seen = true;
		} else if (entry->kind == FSIMG_ENTRY_FIFO) {
			CHECK(fsimg_reader_read_at(fixture.reader, 0, bytes,
			    sizeof(bytes), &count, NULL) == FSIMG_ESTATE);
			fifo_seen = true;
		}
	}
	CHECK(link_seen && fifo_seen);
	fixture_close(&fixture);
}

static void
test_selection_scope(void)
{
	extract_fixture_t fixture;
	fsimg_extract_options_t options;
	fsimg_volume_info_t info;
	fsimg_verify_report_t report;
	const fsimg_entry_t *entry;
	struct stat st;
	uint64_t directory_offset;
	int fd, root;

	fixture_open(&fixture);
	CHECK(fsimg_reader_next(fixture.reader, &entry, NULL) == FSIMG_OK);
	CHECK(strcmp(entry->path, "/A") == 0);
	directory_offset = fsimg_fat_cluster_offset(&fsimg_fat_state(fixture.reader)->geometry,
	    fsimg_fat_state(fixture.reader)->current_cluster);
	fsimg_reader_free(fixture.reader);
	fixture.reader = NULL;
	/* Corrupt an unrelated directory before capturing the new reader identity.
	 * Selected-file success is distinct from complete structural verification.
	 */
	fd = openat(fixture.parent, "image", O_RDWR | O_NOFOLLOW);
	CHECK(fd != -1);
	CHECK(pwrite(fd, "X", 1, (off_t)directory_offset) == 1);
	CHECK(close(fd) == 0);
	CHECK(fsimg_reader_open(fixture.ctx, fixture.input, NULL, &fixture.reader, NULL) == FSIMG_OK);
	fsimg_extract_options_init(&options);
	options.selections = (const char *const[]){ "/Long root name.txt" };
	options.selection_count = 1;
	CHECK(fsimg_extract_at(fixture.reader, fixture.parent, "selected", &options, NULL) == FSIMG_OK);
	CHECK(fsimg_reader_get_info(fixture.reader, &info, NULL) == FSIMG_OK);
	CHECK(info.scope == FSIMG_VALIDATION_HEADER);
	root = openat(fixture.parent, "selected", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(root != -1);
	CHECK(fstatat(root, "Long root name.txt", &st, AT_SYMLINK_NOFOLLOW) == 0);
	CHECK(fstatat(root, "A", &st, AT_SYMLINK_NOFOLLOW) == -1);
	CHECK(close(root) == 0);
	remove_tree(fixture.parent, "selected");
	fsimg_reader_free(fixture.reader);
	CHECK(fsimg_reader_open(fixture.ctx, fixture.input, NULL, &fixture.reader, NULL) == FSIMG_OK);
	CHECK(fsimg_verify(fixture.reader, 0, &report, NULL) == FSIMG_ECORRUPT);
	fixture_close(&fixture);
}

static void
test_selection_failure(void)
{
	extract_fixture_t fixture;
	fsimg_selection_t *selection;
	const fsimg_entry_t *entry;

	fixture_open(&fixture);
	CHECK(fsimg_selection_new(fixture.reader, (const char *const[]){ "/A/short/child" },
	    1, FSIMG_SELECTION_FILE,
	    &selection, NULL) == FSIMG_OK);
	CHECK(fsimg_selection_next(selection, &entry, NULL) == FSIMG_EINVAL);
	CHECK(entry == NULL);
	CHECK(fsimg_reader_next(fixture.reader, &entry, NULL) == FSIMG_ESTATE);
	CHECK(fsimg_selection_next(selection, &entry, NULL) == FSIMG_ESTATE);
	fsimg_selection_free(selection);
	fixture_close(&fixture);
}

static void
test_alias(void)
{
	extract_fixture_t fixture;
	fsimg_extract_options_t options;
	struct stat st;
	int root;

	fixture_open(&fixture);
	fsimg_extract_options_init(&options);
	options.selections = (const char *const[]){ "/f0000000" };
	options.selection_count = 1;
	CHECK(fsimg_extract_at(fixture.reader, fixture.parent, "alias", &options, NULL) == FSIMG_OK);
	root = openat(fixture.parent, "alias", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(root != -1);
	CHECK(fstatat(root, "Long root name.txt", &st, AT_SYMLINK_NOFOLLOW) == 0);
	CHECK(st.st_size == 0);
	CHECK(fstatat(root, "f0000000", &st, AT_SYMLINK_NOFOLLOW) == -1);
	CHECK(close(root) == 0);
	remove_tree(fixture.parent, "alias");
	fixture_close(&fixture);
}

static bool
cancelled(void *argument)
{
	extract_fixture_t *fixture = argument;

	return (fixture->cancel);
}

static void
add_entry(fsimg_writer_t *writer, const char *path, fsimg_entry_kind_t kind,
    uint64_t length)
{
	fsimg_entry_t entry = { .path = path, .kind = kind, .size = length };
	fsimg_source_t zero = { .kind = FSIMG_SOURCE_ZERO };

	CHECK(fsimg_writer_add(writer, &entry,
	    kind == FSIMG_ENTRY_DIRECTORY ? NULL : &zero, NULL) == FSIMG_OK);
}

static void
fixture_open(extract_fixture_t *fixture)
{
	fixture_open_with_entries(fixture, NULL, 0);
}

static void
fixture_open_with_entries(extract_fixture_t *fixture,
    const fsimg_entry_t *extra, size_t count)
{
	fsimg_options_t options;
	fsimg_writer_t *writer;
	fsimg_io_t *output;
	int fd;

	bzero(fixture, sizeof(*fixture));
	snprintf(fixture->path, sizeof(fixture->path), "extract-fixture-XXXXXX");
	CHECK(mkdtemp(fixture->path) != NULL);
	fixture->parent = open(fixture->path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fixture->parent != -1);
	fsimg_options_init(&options);
	options.scratch_directory_fd = fixture->parent;
	options.memory_bytes = UINT64_C(16) * 1024 * 1024;
	options.cancelled = cancelled;
	options.cancel_argument = fixture;
	CHECK(fsimg_ctx_new(&options, &fixture->ctx, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &fixture->baseline, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_new(fixture->ctx, "fat16", NULL, &writer, NULL) == FSIMG_OK);
	add_entry(writer, "/Long root name.txt", FSIMG_ENTRY_FILE, 0);
	add_entry(writer, "/A", FSIMG_ENTRY_DIRECTORY, 0);
	add_entry(writer, "/A/nested", FSIMG_ENTRY_DIRECTORY, 0);
	add_entry(writer, "/A/nested/file", FSIMG_ENTRY_FILE, FILE_BYTES);
	add_entry(writer, "/A/short", FSIMG_ENTRY_FILE, 4);
	add_entry(writer, "/A-", FSIMG_ENTRY_FILE, 3);
	add_entry(writer, "/ZERO", FSIMG_ENTRY_FILE, FILE_BYTES);
	for (size_t i = 0; i < count; i++)
		add_entry(writer, extra[i].path, extra[i].kind, extra[i].size);
	/* Keep parser/extractor fixtures independent of host publication metadata. */
	fd = openat(fixture->parent, "image", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	CHECK(fd != -1);
	CHECK(fsimg_io_from_fd(fixture->ctx, fd, FSIMG_IO_WRITE, &output, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_emit(writer, output, NULL) == FSIMG_OK);
	CHECK(fsimg_io_close_output(output, NULL) == FSIMG_OK);
	CHECK(close(fd) == 0);
	fsimg_writer_free(writer);
	fd = openat(fixture->parent, "image", O_RDONLY | O_NOFOLLOW);
	CHECK(fd != -1);
	CHECK(fsimg_io_from_fd(fixture->ctx, fd, FSIMG_IO_READ, &fixture->input,
	    NULL) == FSIMG_OK);
	CHECK(close(fd) == 0);
	CHECK(fsimg_reader_open(fixture->ctx, fixture->input, NULL,
	    &fixture->reader, NULL) == FSIMG_OK);
}

/* A deeper selected branch and two siblings expose over-broad traversal. */
static void
fixture_open_union(extract_fixture_t *fixture)
{
	static const fsimg_entry_t extra[] = {
		{ .path = "/A/nested/deeper", .kind = FSIMG_ENTRY_DIRECTORY },
		{ .path = "/A/nested/deeper/leaf", .kind = FSIMG_ENTRY_FILE },
		{ .path = "/B", .kind = FSIMG_ENTRY_DIRECTORY },
		{ .path = "/B/hidden", .kind = FSIMG_ENTRY_FILE },
		{ .path = "/C", .kind = FSIMG_ENTRY_DIRECTORY },
		{ .path = "/C/other", .kind = FSIMG_ENTRY_FILE },
		{ .path = "/C/sub", .kind = FSIMG_ENTRY_DIRECTORY },
		{ .path = "/C/sub/file", .kind = FSIMG_ENTRY_FILE }
	};

	fixture_open_with_entries(fixture, extra, sizeof(extra) / sizeof(extra[0]));
}

/* An exact sequence checks walk order, membership and duplicates. */
static void
expect_selected_entries(extract_fixture_t *fixture, const char *const *members,
    size_t count, fsimg_selection_policy_t policy,
    const char *const *expected, size_t expected_count)
{
	fsimg_selection_t *selection;
	const fsimg_entry_t *entry;

	CHECK(fsimg_selection_new(fixture->reader, members, count, policy,
	    &selection, NULL) == FSIMG_OK);
	for (size_t i = 0; i < expected_count; i++) {
		CHECK(fsimg_selection_next(selection, &entry, NULL) ==
		    FSIMG_OK);
		if (strcmp(entry->path, expected[i]) != 0)
			fprintf(stderr, "entry %zu: expected %s, got %s\n", i,
			    expected[i], entry->path);
		CHECK(strcmp(entry->path, expected[i]) == 0);
	}
	CHECK(fsimg_selection_next(selection, &entry, NULL) == FSIMG_DONE);
	CHECK(entry == NULL);
	fsimg_selection_free(selection);
}

static const char *const a_subtree[] = {
	"/A",
	"/A/nested",
	"/A/nested/file",
	"/A/short"
};

static void
test_nested_members_return_subtree_once(void)
{
	const char *const members[] = { "/A", "/A/short" };
	extract_fixture_t fixture;

	fixture_open(&fixture);
	expect_selected_entries(&fixture, members, 2, FSIMG_SELECTION_TREE,
	    a_subtree, sizeof(a_subtree) / sizeof(a_subtree[0]));
	fixture_close(&fixture);
}

static void
test_duplicate_members_return_subtree_once(void)
{
	const char *const members[] = { "/A", "/A" };
	extract_fixture_t fixture;

	fixture_open(&fixture);
	expect_selected_entries(&fixture, members, 2, FSIMG_SELECTION_TREE,
	    a_subtree, sizeof(a_subtree) / sizeof(a_subtree[0]));
	fixture_close(&fixture);
}

static void
test_case_alias_members_return_subtree_once(void)
{
	const char *const members[] = { "/A", "/a" };
	extract_fixture_t fixture;

	fixture_open(&fixture);
	expect_selected_entries(&fixture, members, 2, FSIMG_SELECTION_TREE,
	    a_subtree, sizeof(a_subtree) / sizeof(a_subtree[0]));
	fixture_close(&fixture);
}

static void
test_short_alias_overlap_preserves_display_name(void)
{
	const char *const members[] = { "/Long root name.txt", "/f0000000" };
	const char *const expected[] = {
		"/Long root name.txt"
	};
	extract_fixture_t fixture;

	fixture_open(&fixture);
	expect_selected_entries(&fixture, members, 2, FSIMG_SELECTION_TREE,
	    expected, 1);
	fixture_close(&fixture);
}

static void
test_disjoint_members_return_only_subtrees_and_ancestors(void)
{
	const char *const members[] = { "/C/sub", "/A/nested" };
	const char *const expected[] = {
		"/A",
		"/A/nested",
		"/A/nested/deeper",
		"/A/nested/deeper/leaf",
		"/A/nested/file",
		"/C",
		"/C/sub",
		"/C/sub/file"
	};
	extract_fixture_t fixture;

	fixture_open_union(&fixture);
	expect_selected_entries(&fixture, members, 2, FSIMG_SELECTION_TREE,
	    expected, sizeof(expected) / sizeof(expected[0]));
	fixture_close(&fixture);
}

static void
test_missing_member_fails_reader(void)
{
	const char *const members[] = { "/A", "/a/Absent" };
	extract_fixture_t fixture;
	fsimg_selection_t *selection;
	const fsimg_entry_t *entry;
	fsimg_error_t error;

	fixture_open(&fixture);
	CHECK(fsimg_selection_new(fixture.reader, members, 2,
	    FSIMG_SELECTION_TREE, &selection, NULL) == FSIMG_OK);
	for (size_t i = 0; i < sizeof(a_subtree) / sizeof(a_subtree[0]); i++) {
		CHECK(fsimg_selection_next(selection, &entry, NULL) == FSIMG_OK);
		CHECK(strcmp(entry->path, a_subtree[i]) == 0);
	}
	CHECK(fsimg_selection_next(selection, &entry, &error) == FSIMG_EINVAL);
	CHECK(entry == NULL);
	CHECK(strcmp(error.path, "/a/Absent") == 0);
	CHECK(strcmp(error.message, "selected image path was not found") == 0);
	CHECK(fsimg_reader_next(fixture.reader, &entry, NULL) == FSIMG_ESTATE);
	CHECK(fsimg_selection_next(selection, &entry, NULL) == FSIMG_ESTATE);
	fsimg_selection_free(selection);
	fixture_close(&fixture);
}

static void
test_missing_nested_member_fails_before_later_selection(void)
{
	const char *const members[] = { "/A/Absent", "/C" };
	extract_fixture_t fixture;
	fsimg_selection_t *selection;
	const fsimg_entry_t *entry;
	fsimg_error_t error;

	fixture_open_union(&fixture);
	CHECK(fsimg_selection_new(fixture.reader, members, 2,
	    FSIMG_SELECTION_TREE, &selection, NULL) == FSIMG_OK);
	/* Only /A is selected as an ancestor; its children cannot match Absent. */
	CHECK(fsimg_selection_next(selection, &entry, NULL) == FSIMG_OK);
	CHECK(strcmp(entry->path, "/A") == 0);
	CHECK(fsimg_selection_next(selection, &entry, &error) == FSIMG_EINVAL);
	CHECK(entry == NULL);
	CHECK(strcmp(error.path, "/A/Absent") == 0);
	CHECK(strcmp(error.message, "selected image path was not found") == 0);
	fsimg_selection_free(selection);
	fixture_close(&fixture);
}

/* ZERO sorts last in the union fixture's root. Observing its replay proves
 * a root-level miss scans past the selected subtree before failing.
 */
static fsimg_status_t
observe_last_root_entry(fsimg_reader_t *reader, const char *name,
    const unsigned char *payload, size_t length, fsimg_error_t *error)
{
	if (strcmp(name, "ZERO") == 0)
		last_root_entry_seen = true;
	return (fat_reader_ops->replay(reader, name, payload, length, error));
}

static void
test_missing_root_member_fails_after_selected_subtree(void)
{
	const char *const members[] = { "/A", "/Zzz" };
	const char *const expected[] = {
		"/A",
		"/A/nested",
		"/A/nested/deeper",
		"/A/nested/deeper/leaf",
		"/A/nested/file",
		"/A/short"
	};
	extract_fixture_t fixture;
	fsimg_selection_t *selection;
	const fsimg_entry_t *entry;
	fsimg_reader_ops_t observed_ops;
	fsimg_error_t error;

	fixture_open_union(&fixture);
	fat_reader_ops = fixture.reader->ops;
	observed_ops = *fat_reader_ops;
	observed_ops.replay = observe_last_root_entry;
	fixture.reader->ops = &observed_ops;
	last_root_entry_seen = false;
	CHECK(fsimg_selection_new(fixture.reader, members, 2,
	    FSIMG_SELECTION_TREE, &selection, NULL) == FSIMG_OK);
	for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
		CHECK(fsimg_selection_next(selection, &entry, NULL) == FSIMG_OK);
		CHECK(strcmp(entry->path, expected[i]) == 0);
	}
	CHECK(!last_root_entry_seen);
	CHECK(fsimg_selection_next(selection, &entry, &error) == FSIMG_EINVAL);
	CHECK(last_root_entry_seen);
	CHECK(entry == NULL);
	CHECK(strcmp(error.path, "/Zzz") == 0);
	CHECK(strcmp(error.message, "selected image path was not found") == 0);
	fsimg_selection_free(selection);
	fixture_close(&fixture);
}

static void
test_non_directory_component_names_member(void)
{
	const char *const members[] = { "/A", "/a/short/child" };
	extract_fixture_t fixture;
	fsimg_selection_t *selection;
	const fsimg_entry_t *entry;
	fsimg_error_t error;

	fixture_open(&fixture);
	CHECK(fsimg_selection_new(fixture.reader, members, 2,
	    FSIMG_SELECTION_TREE, &selection, NULL) == FSIMG_OK);
	for (size_t i = 0; i < 3; i++) {
		CHECK(fsimg_selection_next(selection, &entry, NULL) == FSIMG_OK);
		CHECK(strcmp(entry->path, a_subtree[i]) == 0);
	}
	CHECK(fsimg_selection_next(selection, &entry, &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "/a/short/child") == 0);
	CHECK(strstr(error.message, "wrong entry kind") != NULL);
	fsimg_selection_free(selection);
	fixture_close(&fixture);
}

static void
test_children_members_stop_below_immediate_children(void)
{
	const char *const members[] = { "/A", "/A/nested" };
	const char *const expected[] = {
		"/A",
		"/A/nested",
		"/A/nested/deeper",
		"/A/nested/file",
		"/A/short"
	};
	extract_fixture_t fixture;

	fixture_open_union(&fixture);
	expect_selected_entries(&fixture, members, 2, FSIMG_SELECTION_CHILDREN,
	    expected, sizeof(expected) / sizeof(expected[0]));
	fixture_close(&fixture);
}

static void
test_file_policy_refuses_several_members(void)
{
	const char *const members[] = { "/A/short", "/Long root name.txt" };
	extract_fixture_t fixture;
	fsimg_selection_t *selection;

	fixture_open(&fixture);
	CHECK(fsimg_selection_new(fixture.reader, members, 2,
	    FSIMG_SELECTION_FILE, &selection, NULL) == FSIMG_EINVAL);
	CHECK(selection == NULL);
	fixture_close(&fixture);
}

static void
test_no_members_selects_root(void)
{
	const char *const expected[] = {
		"/A",
		"/A/nested",
		"/A/nested/file",
		"/A/short",
		"/A-",
		"/Long root name.txt",
		"/ZERO"
	};
	extract_fixture_t fixture;

	fixture_open(&fixture);
	expect_selected_entries(&fixture, NULL, 0, FSIMG_SELECTION_TREE,
	    expected, sizeof(expected) / sizeof(expected[0]));
	fixture_close(&fixture);
}

static void
test_null_member_array_is_invalid(void)
{
	extract_fixture_t fixture;
	fsimg_selection_t *selection;

	fixture_open(&fixture);
	CHECK(fsimg_selection_new(fixture.reader, NULL, 1,
	    FSIMG_SELECTION_TREE, &selection, NULL) == FSIMG_EINVAL);
	CHECK(selection == NULL);
	fixture_close(&fixture);
}

static void
test_extract_members_union_and_manifest_once(void)
{
	const char *const members[] = { "/A", "/a/short", "/Long root name.txt" };
	const char *const paths[] = { "A", "A/nested", "A/nested/file",
	    "A/short", "Long root name.txt" };
	const char *const records[] = { "./A ", "./A/nested ",
	    "./A/nested/file ", "./A/short ", "./Long\\040root\\040name.txt " };
	extract_fixture_t fixture;
	fsimg_extract_options_t options;
	struct stat st;
	char manifest[4096], *line, *next;
	unsigned seen[5] = { 0 };
	ssize_t length;
	size_t records_seen;
	int fd, root;

	fixture_open(&fixture);
	fd = openat(fixture.parent, "manifest", O_RDWR | O_CREAT | O_EXCL, 0600);
	CHECK(fd != -1);
	fsimg_extract_options_init(&options);
	options.selections = members;
	options.selection_count = 3;
	options.manifest_fd = fd;
	CHECK(fsimg_extract_at(fixture.reader, fixture.parent, "output", &options,
	    NULL) == FSIMG_OK);
	root = openat(fixture.parent, "output", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(root != -1);
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
		CHECK(fstatat(root, paths[i], &st, AT_SYMLINK_NOFOLLOW) == 0);
	CHECK(fstatat(root, "A-", &st, AT_SYMLINK_NOFOLLOW) == -1 && errno == ENOENT);
	CHECK(fstatat(root, "ZERO", &st, AT_SYMLINK_NOFOLLOW) == -1 && errno == ENOENT);
	CHECK(fstatat(root, "f0000000", &st, AT_SYMLINK_NOFOLLOW) == -1 && errno == ENOENT);
	CHECK(close(root) == 0);

	CHECK(fstat(fd, &st) == 0 && st.st_size < (off_t)sizeof(manifest));
	length = pread(fd, manifest, sizeof(manifest) - 1, 0);
	CHECK(length == st.st_size);
	manifest[length] = '\0';
	records_seen = 0;
	for (line = manifest; *line != '\0'; line = next + 1) {
		next = strchr(line, '\n');
		CHECK(next != NULL);
		if (strncmp(line, "./", 2) != 0)
			continue;
		records_seen++;
		for (size_t i = 0; i < sizeof(records) / sizeof(records[0]); i++)
			if (strncmp(line, records[i], strlen(records[i])) == 0)
				seen[i]++;
	}
	CHECK(records_seen == 5);
	for (size_t i = 0; i < sizeof(seen) / sizeof(seen[0]); i++)
		CHECK(seen[i] == 1);
	CHECK(close(fd) == 0);
	CHECK(unlinkat(fixture.parent, "manifest", 0) == 0);
	remove_tree(fixture.parent, "output");
	fixture_close(&fixture);
}

static void
test_null_extraction_members_create_no_destination(void)
{
	const char *const members[] = { "/A", NULL };
	extract_fixture_t fixture;
	fsimg_extract_options_t options;
	struct stat st;

	fixture_open(&fixture);
	fsimg_extract_options_init(&options);
	options.selections = members;
	options.selection_count = 2;
	CHECK(fsimg_extract_at(fixture.reader, fixture.parent, "invalid", &options,
	    NULL) == FSIMG_EINVAL);
	CHECK(fstatat(fixture.parent, "invalid", &st, AT_SYMLINK_NOFOLLOW) == -1 &&
	    errno == ENOENT);
	options.selections = NULL;
	options.selection_count = 1;
	CHECK(fsimg_extract_at(fixture.reader, fixture.parent, "invalid", &options,
	    NULL) == FSIMG_EINVAL);
	CHECK(fstatat(fixture.parent, "invalid", &st, AT_SYMLINK_NOFOLLOW) == -1 &&
	    errno == ENOENT);
	fixture_close(&fixture);
}

static void
test_missing_extraction_member_names_error(void)
{
	const char *const members[] = { "/A", "/Missing" };
	extract_fixture_t fixture;
	fsimg_extract_options_t options;
	fsimg_error_t error;

	fixture_open(&fixture);
	fsimg_extract_options_init(&options);
	options.selections = members;
	options.selection_count = 2;
	CHECK(fsimg_extract_at(fixture.reader, fixture.parent, "output", &options,
	    &error) == FSIMG_EINVAL);
	CHECK(strcmp(error.path, "/Missing") == 0);
	CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
	remove_tree(fixture.parent, "output");
	fixture_close(&fixture);
}

/* Test-only cleanup walks solely disposable roots created by this fixture. */
static void
remove_tree(int parent, const char *name)
{
	struct dirent *entry;
	struct stat st;
	DIR *directory;
	int fd;

	fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(fd != -1);
	directory = fdopendir(fd);
	CHECK(directory != NULL);
	while ((entry = readdir(directory)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		CHECK(fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0);
		if (S_ISDIR(st.st_mode))
			remove_tree(fd, entry->d_name);
		else
			CHECK(unlinkat(fd, entry->d_name, 0) == 0);
	}
	CHECK(closedir(directory) == 0);
	CHECK(unlinkat(parent, name, AT_REMOVEDIR) == 0);
}

static void
fixture_close(extract_fixture_t *fixture)
{
	fsimg_stats_t stats;

	fsimg_reader_free(fixture->reader);
	fsimg_io_free(fixture->input);
	CHECK(fsimg_ctx_get_stats(fixture->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == fixture->baseline.memory_bytes);
	CHECK(stats.scratch_bytes == 0);
	CHECK(stats.descriptors == fixture->baseline.descriptors);
	fsimg_ctx_free(fixture->ctx);
	CHECK(unlinkat(fixture->parent, "image", 0) == 0);
	CHECK(close(fixture->parent) == 0);
	CHECK(rmdir(fixture->path) == 0);
}

static void
test_roundtrip(const char *selection)
{
	extract_fixture_t fixture;
	fsimg_extract_options_t options;
	fsimg_error_t error;
	fsimg_host_file_t identity;
	struct stat st;
	unsigned char buffer[4096];
	fsimg_status_t status;
	int root, fd;
	ssize_t count;
	bool known;

	fixture_open(&fixture);
	fsimg_extract_options_init(&options);
	options.selections = &selection;
	options.selection_count = selection == NULL ? 0 : 1;
	status = fsimg_extract_at(fixture.reader, fixture.parent, "output", &options,
	    &error);
	if (status != FSIMG_OK) {
		fprintf(stderr, "extract %s: %d: %s\n", selection, status, error.message);
		CHECK(fsimg_io_identity(fixture.input, &identity, &known, NULL) == FSIMG_OK);
		fprintf(stderr, "image before: size=%llu mtime=%lld.%09u ctime=%lld.%09u\n",
		    (unsigned long long)fixture.reader->identity.length,
		    (long long)fixture.reader->identity.modified.seconds,
		    fixture.reader->identity.modified.nanoseconds,
		    (long long)fixture.reader->identity.changed.seconds,
		    fixture.reader->identity.changed.nanoseconds);
		fprintf(stderr, "image after: size=%llu mtime=%lld.%09u ctime=%lld.%09u\n",
		    (unsigned long long)identity.length, (long long)identity.modified.seconds,
		    identity.modified.nanoseconds, (long long)identity.changed.seconds,
		    identity.changed.nanoseconds);
	}
	CHECK(status == FSIMG_OK);
	root = openat(fixture.parent, "output", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(root != -1);
	CHECK(fstat(root, &st) == 0 && (st.st_mode & 0777) == 0700);
	fd = openat(root, "A/nested/file", O_RDONLY | O_NOFOLLOW);
	CHECK(fd != -1);
	CHECK(fstat(fd, &st) == 0 && st.st_size == FILE_BYTES);
	CHECK((st.st_mode & 0777) == 0600);
	while ((count = read(fd, buffer, sizeof(buffer))) > 0) {
		for (ssize_t i = 0; i < count; i++)
			CHECK(buffer[i] == 0);
	}
	CHECK(count == 0);
	CHECK(close(fd) == 0);
	if (strcmp(selection, "/") != 0)
		CHECK(fstatat(root, "A-", &st, AT_SYMLINK_NOFOLLOW) == -1);
	else
		CHECK(fstatat(root, "Long root name.txt", &st, AT_SYMLINK_NOFOLLOW) == 0);
	CHECK(close(root) == 0);
	remove_tree(fixture.parent, "output");
	fixture_close(&fixture);
}

static void
test_existing(void)
{
	extract_fixture_t fixture;
	fsimg_extract_options_t options;
	int fd;

	fixture_open(&fixture);
	CHECK(mkdirat(fixture.parent, "existing", 0700) == 0);
	CHECK(fsimg_extract_at(fixture.reader, fixture.parent, "existing", NULL,
	    NULL) == FSIMG_EEXISTS);
	CHECK(symlinkat("existing", fixture.parent, "link") == 0);
	CHECK(fsimg_extract_at(fixture.reader, fixture.parent, "link", NULL,
	    NULL) == FSIMG_EEXISTS);
	fd = openat(fixture.parent, "regular", O_CREAT | O_EXCL | O_WRONLY, 0600);
	CHECK(fd != -1 && close(fd) == 0);
	CHECK(fsimg_extract_at(fixture.reader, fixture.parent, "regular", NULL,
	    NULL) == FSIMG_EEXISTS);
	CHECK(fsimg_extract_at(fixture.reader, fixture.parent, "../escape", NULL,
	    NULL) == FSIMG_EINVAL);
	fsimg_extract_options_init(&options);
	options.selections = (const char *const[]){ "/A/../escape" };
	options.selection_count = 1;
	CHECK(fsimg_extract_at(fixture.reader, fixture.parent, "invalid", &options,
	    NULL) == FSIMG_EINVAL);
	CHECK(unlinkat(fixture.parent, "existing", AT_REMOVEDIR) == 0);
	CHECK(unlinkat(fixture.parent, "regular", 0) == 0);
	CHECK(unlinkat(fixture.parent, "link", 0) == 0);
	fixture_close(&fixture);
}

static void
test_limits(void)
{
	const char *const missing[] = { "/missing" };
	extract_fixture_t fixture;
	fsimg_extract_options_t options;
	fsimg_error_t error;
	fsimg_status_t status;

	for (unsigned scenario = 0; scenario < 4; scenario++) {
		fixture_open(&fixture);
		fsimg_extract_options_init(&options);
		if (scenario == 0)
			options.max_output_bytes = FILE_BYTES - 1;
		if (scenario == 1) {
			options.selections = missing;
			options.selection_count = 1;
		}
		if (scenario == 2)
			fixture.cancel = true;
		if (scenario == 3)
			fixture.ctx->options.max_open_files = fixture.ctx->stats.descriptors + 2;
		status = fsimg_extract_at(fixture.reader, fixture.parent, "limited",
		    &options, &error);
		CHECK(status == (scenario == 0 || scenario == 3 ? FSIMG_ELIMIT :
		    scenario == 1 ? FSIMG_EINVAL : FSIMG_ECANCELLED));
		CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
		remove_tree(fixture.parent, "limited");
		fixture_close(&fixture);
	}
}

static void
test_times(void)
{
	extract_fixture_t fixture;
	fsimg_extract_options_t options;
	struct stat st;
	int root, fd;

	fixture_open(&fixture);
	fsimg_extract_options_init(&options);
	options.flags |= FSIMG_EXTRACT_TIMES | FSIMG_EXTRACT_WALL_OFFSET;
	options.wall_offset_minutes = 60;
	CHECK(fsimg_extract_at(fixture.reader, fixture.parent, "times", &options,
	    NULL) == FSIMG_OK);
	root = openat(fixture.parent, "times", O_RDONLY | O_DIRECTORY);
	CHECK(root != -1);
	fd = openat(root, "A/nested/file", O_RDONLY | O_NOFOLLOW);
	CHECK(fd != -1);
	CHECK(fstat(fd, &st) == 0);
	CHECK(st.st_mtime == 315532800 - 3600);
	CHECK(close(fd) == 0);
	fd = openat(root, "A", O_RDONLY | O_DIRECTORY);
	CHECK(fd != -1);
	CHECK(fstat(fd, &st) == 0);
	CHECK(st.st_mtime == 315532800 - 3600);
	CHECK(close(fd) == 0);
	CHECK(close(root) == 0);
	remove_tree(fixture.parent, "times");
	fixture_close(&fixture);
}

static void
test_repeated_root_members_exhaust_work_limit(void)
{
	const char *members[4096];
	const size_t counts[] = { 1, sizeof(members) / sizeof(members[0]) };
	const char *const expected[] = { "/A", "/A/nested", "/A/nested/file",
	    "/A/short", "/A-", "/Long root name.txt", "/ZERO" };
	extract_fixture_t fixture;
	fsimg_selection_t *selection;
	const fsimg_entry_t *entry;
	fsimg_status_t status;
	fsimg_error_t error;
	size_t returned;

	for (size_t i = 0; i < sizeof(members) / sizeof(members[0]); i++)
		members[i] = "/";
	for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
		fixture_open(&fixture);
		/* Seven entries cost 7 or 7 * 4096 selection work units.
		 * 8192 fits the single-member walk including FAT parsing, but
		 * cannot fit the repeated roots even without parsing work.
		 */
		fixture.ctx->options.max_work = 8192;
		CHECK(fsimg_selection_new(fixture.reader, members, counts[i],
		    FSIMG_SELECTION_TREE, &selection, NULL) == FSIMG_OK);
		returned = 0;
		while ((status = fsimg_selection_next(selection, &entry,
		    &error)) == FSIMG_OK) {
			CHECK(returned < sizeof(expected) / sizeof(expected[0]));
			CHECK(strcmp(entry->path, expected[returned]) == 0);
			returned++;
		}
		if (counts[i] == 1) {
			CHECK(status == FSIMG_DONE);
			CHECK(returned == 7);
		} else {
			CHECK(status == FSIMG_ELIMIT);
			CHECK(error.status == FSIMG_ELIMIT);
			CHECK(error.resource == FSIMG_RESOURCE_WORK);
			CHECK(returned < 7);
		}
		CHECK(entry == NULL);
		fsimg_selection_free(selection);
		fixture_close(&fixture);
	}
}

static void
test_selection_allocation_failures_leave_no_output(void)
{
	const char *const members[] = { "/A", "/A/nested/file", "/ZERO" };
	extract_fixture_t fixture;
	fsimg_selection_t *selection;
	fsimg_stats_t baseline, stats;
	fsimg_status_t status;
	fsimg_error_t error;
	bool completed;

	fixture_open(&fixture);
	CHECK(fsimg_ctx_get_stats(fixture.ctx, &baseline, NULL) == FSIMG_OK);
	fsimg_test_poison_allocations(fixture.ctx, true);
	completed = false;
	for (uint64_t attempt = 1; attempt < 32; attempt++) {
		/* A non-NULL sentinel proves failure clears the caller's output. */
		selection = (fsimg_selection_t *)&fixture;
		fsimg_test_fail_alloc_after(fixture.ctx, attempt);
		status = fsimg_selection_new(fixture.reader, members, 3,
		    FSIMG_SELECTION_TREE, &selection, &error);
		fsimg_test_fail_alloc_after(fixture.ctx, 0);
		if (status == FSIMG_OK) {
			CHECK(selection != NULL);
			fsimg_selection_free(selection);
			completed = true;
		} else {
			CHECK(status == FSIMG_ENOMEM);
			CHECK(error.status == FSIMG_ENOMEM);
			CHECK(selection == NULL);
		}
		CHECK(fsimg_ctx_get_stats(fixture.ctx, &stats, NULL) == FSIMG_OK);
		CHECK(stats.memory_bytes == baseline.memory_bytes);
		CHECK(stats.spool_bytes == baseline.spool_bytes);
		CHECK(stats.descriptors == baseline.descriptors);
		if (completed) {
			CHECK(attempt > 3);
			break;
		}
	}
	CHECK(completed);
	fsimg_test_poison_allocations(fixture.ctx, false);
	fixture_close(&fixture);
}

static void
test_allocations(void)
{
	extract_fixture_t fixture;
	fsimg_status_t status;
	bool completed;

	completed = false;
	for (uint64_t attempt = 1; attempt < 128; attempt++) {
		fixture_open(&fixture);
		fsimg_test_fail_alloc_after(fixture.ctx, attempt);
		status = fsimg_extract_at(fixture.reader, fixture.parent, "allocation",
		    NULL, NULL);
		CHECK(status == FSIMG_OK || status == FSIMG_ENOMEM);
		fsimg_test_fail_alloc_after(fixture.ctx, 0);
		remove_tree(fixture.parent, "allocation");
		fixture_close(&fixture);
		if (status == FSIMG_OK) {
			CHECK(attempt > 4);
			completed = true;
			break;
		}
	}
	CHECK(completed);
}

static void
test_pivot(void)
{
	extract_fixture_t fixture;
	fsimg_error_t error;
	struct stat st;
	int root;

	fixture_open(&fixture);
	CHECK(mkdirat(fixture.parent, "private", 0700) == 0);
	root = openat(fixture.parent, "private", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(root != -1);
	CHECK(symlinkat("..", root, "A") == 0);
	CHECK(fsimg_extract_into(fixture.reader, root, NULL, NULL, &error) == FSIMG_EEXISTS);
	CHECK(fstatat(fixture.parent, "nested", &st, AT_SYMLINK_NOFOLLOW) == -1);
	CHECK(close(root) == 0);
	remove_tree(fixture.parent, "private");
	fixture_close(&fixture);
}

#if defined(__linux__)
/* Linux stores a default ACL as a system.posix_acl_default attribute: a
 * little-endian version 2 header, then entries sorted by tag of a 16-bit
 * tag, 16-bit permissions and 32-bit id. This one admits user 65534.
 */
static bool
inheritable_acl(int parent, const char *path FSIMG_UNUSED)
{
	static const unsigned char acl[] = {
	    2, 0, 0, 0,
	    0x01, 0, 7, 0, 0xff, 0xff, 0xff, 0xff,	/* owner rwx */
	    0x02, 0, 5, 0, 0xfe, 0xff, 0, 0,		/* user 65534 r-x */
	    0x04, 0, 5, 0, 0xff, 0xff, 0xff, 0xff,	/* group r-x */
	    0x10, 0, 5, 0, 0xff, 0xff, 0xff, 0xff,	/* mask r-x */
	    0x20, 0, 0, 0, 0xff, 0xff, 0xff, 0xff	/* other --- */
	};

	return (fsetxattr(parent, "system.posix_acl_default", acl, sizeof(acl),
	    0) == 0);
}
#else
/* Whether a host tool gave the directory at path an entry admitting user
 * nobody that new directories inherit: chmod +a on macOS, setfacl with an
 * NFSv4 or a POSIX.1e default entry on FreeBSD.
 */
static bool
inheritable_acl(int parent FSIMG_UNUSED, const char *path)
{
	const char *const commands[][6] = {
	    { "chmod", "+a", "user:nobody allow list,search,directory_inherit",
	      path, NULL },
	    { "setfacl", "-m", "u:nobody:rx:fd:allow", path, NULL },
	    { "setfacl", "-d", "-m", "u::rwx,g::rx,o::,m::rx,u:nobody:rx", path,
	      NULL }
	};
	posix_spawn_file_actions_t actions;
	pid_t pid;
	int status;

	CHECK(posix_spawn_file_actions_init(&actions) == 0);
	CHECK(posix_spawn_file_actions_addopen(&actions, 2, "/dev/null",
	    O_WRONLY, 0) == 0);
	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		if (posix_spawnp(&pid, commands[i][0], &actions, NULL,
		    (char *const *)commands[i], environ) != 0)
			continue;
		CHECK(waitpid(pid, &status, 0) == pid);
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
			CHECK(posix_spawn_file_actions_destroy(&actions) == 0);
			return (true);
		}
	}
	CHECK(posix_spawn_file_actions_destroy(&actions) == 0);
	return (false);
}
#endif

/* A parent whose inheritable ACL admits another user gives every new
 * directory that entry, so neither a fresh extraction destination nor the
 * private staging of a created image may be made in it: each is refused,
 * nothing is left beneath the parent, and the reader and writer are not
 * consumed.
 */
static void
test_inherited_acl(void)
{
	extract_fixture_t fixture;
	fsimg_writer_t *writer;
	fsimg_error_t error;
	char path[96];
	struct stat st;
	DIR *listing;
	struct dirent *entry;
	int parent;

	fixture_open(&fixture);
	CHECK(mkdirat(fixture.parent, "inheriting", 0700) == 0);
	parent = openat(fixture.parent, "inheriting",
	    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	CHECK(parent != -1);
	snprintf(path, sizeof(path), "%s/inheriting", fixture.path);
	if (!inheritable_acl(parent, path)) {
		puts("SKIP: inherited ACL refusal (the host or filesystem refuses ACLs)");
		CHECK(close(parent) == 0);
		CHECK(unlinkat(fixture.parent, "inheriting", AT_REMOVEDIR) == 0);
		fixture_close(&fixture);
		return;
	}
	CHECK(fsimg_extract_at(fixture.reader, parent, "output", NULL, &error) ==
	    FSIMG_EINVAL);
	CHECK(error.output_state == FSIMG_OUTPUT_ABSENT);
	CHECK(strstr(error.message, "access control list") != NULL);
	CHECK(fstatat(parent, "output", &st, AT_SYMLINK_NOFOLLOW) == -1 &&
	    errno == ENOENT);

	CHECK(fsimg_writer_new(fixture.ctx, "fat16", NULL, &writer, NULL) == FSIMG_OK);
	add_entry(writer, "/file", FSIMG_ENTRY_FILE, 3);
	CHECK(fsimg_writer_write_at(writer, parent, "image", &error) == FSIMG_EINVAL);
	CHECK(error.output_state == FSIMG_OUTPUT_ABSENT);
	CHECK(strstr(error.message, "access control list") != NULL);
	fsimg_writer_free(writer);

	listing = fdopendir(parent);
	CHECK(listing != NULL);
	while ((entry = readdir(listing)) != NULL)
		CHECK(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0);
	CHECK(closedir(listing) == 0);
	CHECK(unlinkat(fixture.parent, "inheriting", AT_REMOVEDIR) == 0);
	/* The refused reader is still untouched: extraction elsewhere works. */
	CHECK(fsimg_extract_at(fixture.reader, fixture.parent, "output", NULL,
	    NULL) == FSIMG_OK);
	remove_tree(fixture.parent, "output");
	fixture_close(&fixture);
}

int
main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--times") == 0) {
		test_times();
		return (0);
	}
	if (argc == 2 && strcmp(argv[1], "--allocations") == 0) {
		test_allocations();
		test_selection_allocation_failures_leave_no_output();
		return (0);
	}
	CHECK(argc == 1);
	test_roundtrip("/");
	test_roundtrip("/A/nested/file");
	test_roundtrip("/A");
	test_roundtrip("/a/NeStEd/FiLe");
	test_roundtrip("/a/f0000000/f0000000");
	test_alias();
	test_selection_failure();
	test_repeated_root_members_exhaust_work_limit();
	test_selection_scope();
	test_nested_members_return_subtree_once();
	test_duplicate_members_return_subtree_once();
	test_case_alias_members_return_subtree_once();
	test_short_alias_overlap_preserves_display_name();
	test_disjoint_members_return_only_subtrees_and_ancestors();
	test_missing_member_fails_reader();
	test_missing_nested_member_fails_before_later_selection();
	test_missing_root_member_fails_after_selected_subtree();
	test_non_directory_component_names_member();
	test_children_members_stop_below_immediate_children();
	test_file_policy_refuses_several_members();
	test_no_members_selects_root();
	test_null_member_array_is_invalid();
	test_extract_members_union_and_manifest_once();
	test_null_extraction_members_create_no_destination();
	test_missing_extraction_member_names_error();
	test_existing();
	test_limits();
	test_times();
	test_allocations();
	test_selection_allocation_failures_leave_no_output();
	test_pivot();
	test_unsupported_kinds();
	test_link_content();
	test_inherited_acl();
	return (0);
}
