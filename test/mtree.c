/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "internal.h"
#include "mtree.h"
#include "sha256.h"
#include "test.h"

/* The mtree(5) parser on in-memory manifests: real output of two
 * independent writers, every line form, escape and keyword class, each
 * bound at its limit and one past it, and malformed and truncated input.
 * Every refusal asserts its status and the line it names, both the byte
 * offset in the error record and the line number in the message.
 */

#define RECORDED_MAX	300u
#define RECORDED_PATH	4096u

typedef struct recorded_entry {
	char		 path[RECORDED_PATH];
	char		 link[FSIMG_SYMLINK_TARGET_MAX + 1];
	char		 contents[RECORDED_PATH];
	fsimg_mtree_hole_t holes[FSIMG_MTREE_HOLES_MAX];
	fsimg_metadata_t metadata;
	uint64_t	 size;
	uint64_t	 seed;
	size_t		 link_length;
	size_t		 hole_count;
	fsimg_entry_kind_t kind;
	fsimg_mtree_content_t content;
	bool		 has_contents;
	bool		 size_given;
	bool		 digest_given;
	unsigned char	 digest[FSIMG_SHA256_DIGEST_LENGTH];
} recorded_entry_t;

/* How a manifest is fed: the context's limits, the largest read the input
 * returns at once, and the entry whose callback fails (SIZE_MAX: none).
 */
typedef struct parse_setup {
	fsimg_options_t	 options;
	size_t		 chunk;
	size_t		 fail_entry;
	bool		 fail_without_record;
	bool		 fail_read;
	bool		 cancel;
} parse_setup_t;

typedef struct parse_result {
	recorded_entry_t entries[RECORDED_MAX];
	fsimg_error_t	 error;
	size_t		 count;
	fsimg_status_t	 status;
} parse_result_t;

typedef struct memory_input {
	const char	*bytes;
	size_t		 length;
	size_t		 offset;
	size_t		 chunk;
	bool		 fail;
} memory_input_t;

typedef struct recorder {
	parse_result_t	*result;
	size_t		 fail_entry;
	bool		 fail_without_record;
} recorder_t;

/* A manifest that must be refused with status, naming line, and with
 * mention in the message when mention is not NULL.
 */
typedef struct refusal_case {
	const char	*manifest;
	fsimg_status_t	 status;
	unsigned	 line;
	const char	*mention;
} refusal_case_t;

/* Real output for one host tree, from macOS mtree -c -K sha256digest and
 * from bsdtar 3.7.4 (libarchive) --format=mtree, by default and with
 * --options=mtree:use-set,mtree:sha256,mtree:indent. Only the header
 * comments of mtree -c and the user name were neutralized. The tree:
 *
 *	a		file "abc"
 *	link		symbolic link to "a"
 *	sp ace/f#1	file "xy"
 *	sub/empty	empty file
 *	sub/fifo	FIFO
 *	sub/deep/cafe	file "q", the e an acute e in UTF-8
 */
static const char mtree_c_manifest[] =
    "#\t   user: user\n"
    "#\tmachine: host\n"
    "#\t   tree: /tmp/tree\n"
    "#\t   date: Wed Oct 21 02:19:03 2026\n"
    "\n"
    "# .\n"
    "/set type=file uid=501 gid=0 mode=0755 nlink=1 flags=none\n"
    ".               type=dir nlink=6 size=192 time=1792574343.446780945\n"
    "    a           mode=0644 size=3 time=1792574343.402329698 \\\n"
    "                sha256digest=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"
    "    link        type=link size=1 time=1792574343.446772398 link=a\n"
    "\n"
    "# ./sp ace\n"
    "/set type=file uid=501 gid=0 mode=0644 nlink=1 flags=none\n"
    "sp\\040ace       type=dir mode=0755 nlink=3 size=96 \\\n"
    "                time=1792574343.425939751\n"
    "    f\\0431      size=2 time=1792574343.426101163 \\\n"
    "                sha256digest=769a4e6d0003189c7e96c5d9b7e810a0d11c3a12832527ec94b0f86d277f51ca\n"
    "# ./sp ace\n"
    "..\n"
    "\n"
    "\n"
    "# ./sub\n"
    "sub             type=dir mode=0755 nlink=5 size=160 \\\n"
    "                time=1792574343.468666075\n"
    "    empty       size=0 time=1792574343.425659145 \\\n"
    "                sha256digest=e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n"
    "    fifo        type=fifo size=0 time=1792574343.468574820\n"
    "\n"
    "# ./sub/deep\n"
    "deep            type=dir mode=0755 nlink=3 size=96 \\\n"
    "                time=1792574343.490535812\n"
    "    caf\\303\\251 size=1 time=1792574343.511363212 \\\n"
    "                sha256digest=8e35c2cd3bf6641bdb0e2050b76932cbb2e6034a0ddacc1d9bea82a6ba57f7cf\n"
    "# ./sub/deep\n"
    "..\n"
    "\n"
    "# ./sub\n"
    "..\n"
    "\n"
    "..\n"
    "\n";

static const char bsdtar_manifest[] =
    "#mtree\n"
    ". gname=wheel uname=user time=1792574343.446780945 mode=755 gid=0 uid=501 type=dir\n"
    "./a gname=wheel uname=user time=1792574343.402329698 mode=644 gid=0 uid=501 type=file size=3\n"
    "./link gname=wheel uname=user time=1792574343.446772398 mode=755 gid=0 uid=501 type=link link=a\n"
    "./sp\\040ace gname=wheel uname=user time=1792574343.425939751 mode=755 gid=0 uid=501 type=dir\n"
    "./sp\\040ace/f\\0431 gname=wheel uname=user time=1792574343.426101163 mode=644 gid=0 uid=501 type=file size=2\n"
    "./sub gname=wheel uname=user time=1792574343.468666075 mode=755 gid=0 uid=501 type=dir\n"
    "./sub/empty gname=wheel uname=user time=1792574343.425659145 mode=644 gid=0 uid=501 type=file size=0\n"
    "./sub/fifo gname=wheel uname=user time=1792574343.468574820 mode=644 gid=0 uid=501 type=fifo\n"
    "./sub/deep gname=wheel uname=user time=1792574343.490535812 mode=755 gid=0 uid=501 type=dir\n"
    "./sub/deep/caf\\303\\251 gname=wheel uname=user time=1792574343.511363212 mode=644 gid=0 uid=501 type=file size=1\n";

static const char bsdtar_set_manifest[] =
    "#mtree\n"
    "/set type=file uname=user uid=501 gname=wheel gid=0 mode=755\n"
    ".               time=1792574343.446780945 type=dir\n"
    "./a             time=1792574343.402329698 mode=644 size=3 \\\n"
    "                sha256digest=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"
    "./link          time=1792574343.446772398 type=link link=a\n"
    "./sp\\040ace     time=1792574343.425939751 type=dir\n"
    "./sp\\040ace/f\\0431 \\\n"
    "                time=1792574343.426101163 mode=644 size=2 \\\n"
    "                sha256digest=769a4e6d0003189c7e96c5d9b7e810a0d11c3a12832527ec94b0f86d277f51ca\n"
    "/set mode=644\n"
    "./sub           time=1792574343.468666075 mode=755 type=dir\n"
    "./sub/empty     time=1792574343.425659145 size=0 \\\n"
    "                sha256digest=e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n"
    "./sub/fifo      time=1792574343.468574820 type=fifo\n"
    "./sub/deep      time=1792574343.490535812 mode=755 type=dir\n"
    "./sub/deep/caf\\303\\251 \\\n"
    "                time=1792574343.511363212 size=1 \\\n"
    "                sha256digest=8e35c2cd3bf6641bdb0e2050b76932cbb2e6034a0ddacc1d9bea82a6ba57f7cf\n";

/* Generated content with every fsimg. keyword, for the fuzz corpus. */
static const char generated_manifest[] =
    "#mtree v2.0\n"
    "/set type=file uid=0 gid=0 mode=0644 time=946684800.000000000\n"
    ". type=dir mode=0755\n"
    "./zero fsimg.content=zero size=1048576\n"
    "./seeded fsimg.content=seeded fsimg.seed=7 size=4096 \\\n"
    "    fsimg.holes=512:1024,2048:512\n"
    "./positional fsimg.content=positional fsimg.seed=8 size=100\n"
    "./n\\303\\244me\\040\\134 type=link link=../t\\043rget\n"
    "./pipe type=fifo mode=0600\n";

/* What the host tree's three manifests describe, written out by hand: the
 * root carries no entry, a directory's or FIFO's size is zero, and a
 * file's digest is the SHA-256 of its bytes.
 */
typedef struct expected_entry {
	const char	*path;
	fsimg_entry_kind_t kind;
	uint64_t	 size;
	uint32_t	 mode;
	uint32_t	 nanoseconds;
	const char	*bytes;
} expected_entry_t;

static const expected_entry_t host_tree[] = {
	{ "/a", FSIMG_ENTRY_FILE, 3, 0644, 402329698, "abc" },
	{ "/link", FSIMG_ENTRY_SYMLINK, 1, 0755, 446772398, "a" },
	{ "/sp ace", FSIMG_ENTRY_DIRECTORY, 0, 0755, 425939751, NULL },
	{ "/sp ace/f#1", FSIMG_ENTRY_FILE, 2, 0644, 426101163, "xy" },
	{ "/sub", FSIMG_ENTRY_DIRECTORY, 0, 0755, 468666075, NULL },
	{ "/sub/empty", FSIMG_ENTRY_FILE, 0, 0644, 425659145, "" },
	{ "/sub/fifo", FSIMG_ENTRY_FIFO, 0, 0644, 468574820, NULL },
	{ "/sub/deep", FSIMG_ENTRY_DIRECTORY, 0, 0755, 490535812, NULL },
	{ "/sub/deep/caf\303\251", FSIMG_ENTRY_FILE, 1, 0644, 511363212, "q" }
};

static parse_result_t result_storage;

static bool cancel_always(void *);
static void check_host_tree(const parse_result_t *, bool);
static const parse_result_t *check_refusal(const char *, size_t,
    const parse_setup_t *, fsimg_status_t, unsigned);
static const parse_result_t *check_refusals(const refusal_case_t *, size_t);
static const parse_result_t *expect_accepted(const char *);
static const recorded_entry_t *find_entry(const parse_result_t *,
    const char *);
static uint64_t line_start(const char *, size_t, unsigned);
static parse_result_t *parse_manifest(const char *, size_t,
    const parse_setup_t *);
static fsimg_status_t read_memory(void *, unsigned char *, size_t, size_t *,
    fsimg_error_t *);
static fsimg_status_t record_entry(void *, const fsimg_mtree_entry_t *,
    fsimg_error_t *);
static void setup_defaults(parse_setup_t *);
static void test_bounds(void);
static void test_callbacks(void);
static void test_content_keywords(void);
static void test_continuations_and_comments(void);
static void test_defaults(void);
static void test_escapes(void);
static void test_host_writers(void);
static void test_keyword_classes(void);
static void test_line_forms(void);
static void test_malformed(void);
static void test_numeric_values(void);
static void write_seed(const char *, const char *, const char *);
static void write_seeds(const char *);

static void
setup_defaults(parse_setup_t *setup)
{
	bzero(setup, sizeof(*setup));
	fsimg_options_init(&setup->options);
	setup->chunk = SIZE_MAX;
	setup->fail_entry = SIZE_MAX;
}

static fsimg_status_t
read_memory(void *argument, unsigned char *buffer, size_t capacity,
    size_t *length, fsimg_error_t *error)
{
	memory_input_t *input = argument;
	size_t count;

	if (input->fail)
		return (fsimg_error_set(error, FSIMG_EIO, 5, FSIMG_OP_IO,
		    "test read failure"));
	count = input->length - input->offset;
	if (count > capacity)
		count = capacity;
	if (count > input->chunk)
		count = input->chunk;
	memcpy(buffer, input->bytes + input->offset, count);
	input->offset += count;
	*length = count;
	return (FSIMG_OK);
}

/* Copy the borrowed entry; the parser reuses its buffers after the call. */
static fsimg_status_t
record_entry(void *argument, const fsimg_mtree_entry_t *entry,
    fsimg_error_t *error)
{
	recorder_t *recorder = argument;
	recorded_entry_t *record;

	if (recorder->result->count == recorder->fail_entry &&
	    recorder->fail_without_record)
		return (FSIMG_EIO);
	if (recorder->result->count == recorder->fail_entry)
		return (fsimg_error_set(error, FSIMG_EIO, 0, FSIMG_OP_SOURCE,
		    "test entry failure"));
	CHECK(recorder->result->count < RECORDED_MAX);
	record = &recorder->result->entries[recorder->result->count++];
	bzero(record, sizeof(*record));
	CHECK(strlen(entry->entry.path) < sizeof(record->path));
	strcpy(record->path, entry->entry.path);
	record->kind = entry->entry.kind;
	record->size = entry->entry.size;
	record->metadata = entry->entry.metadata;
	record->content = entry->content;
	record->seed = entry->seed;
	record->size_given = entry->size_given;
	record->digest_given = entry->digest_given;
	memcpy(record->digest, entry->digest, sizeof(record->digest));
	CHECK((entry->link != NULL) == (entry->entry.kind ==
	    FSIMG_ENTRY_SYMLINK));
	if (entry->link != NULL) {
		CHECK(strlen(entry->link) == entry->link_length);
		memcpy(record->link, entry->link, entry->link_length + 1);
		record->link_length = entry->link_length;
	}
	if (entry->contents != NULL) {
		CHECK(entry->contents_length < sizeof(record->contents));
		memcpy(record->contents, entry->contents,
		    entry->contents_length + 1);
		record->has_contents = true;
	}
	CHECK(entry->hole_count <= FSIMG_MTREE_HOLES_MAX);
	if (entry->hole_count != 0)
		memcpy(record->holes, entry->holes,
		    entry->hole_count * sizeof(entry->holes[0]));
	record->hole_count = entry->hole_count;
	return (FSIMG_OK);
}

static bool
cancel_always(void *argument FSIMG_UNUSED)
{
	return (true);
}

/* Parse bytes under setup. The result is too large for the stack, so each
 * parse overwrites one static result, which the caller reads before the
 * next parse. The error record starts as garbage, as a caller's might,
 * and the context's memory must return to where it was: the parser keeps
 * nothing.
 */
static parse_result_t *
parse_manifest(const char *bytes, size_t length, const parse_setup_t *setup)
{
	memory_input_t input = { .bytes = bytes, .length = length,
	    .chunk = setup->chunk, .fail = setup->fail_read };
	recorder_t recorder = { .result = &result_storage,
	    .fail_entry = setup->fail_entry,
	    .fail_without_record = setup->fail_without_record };
	fsimg_mtree_input_t callbacks = { .read = read_memory,
	    .read_argument = &input, .entry = record_entry,
	    .entry_argument = &recorder };
	fsimg_options_t options;
	fsimg_session_t session;
	fsimg_stats_t before, after;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;

	options = setup->options;
	if (setup->cancel)
		options.cancelled = cancel_always;
	CHECK(fsimg_ctx_new(&options, &ctx, &error) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(ctx, &before, &error) == FSIMG_OK);
	fsimg_session_init(&session, ctx, "manifest", 0);
	result_storage.count = 0;
	memset(&result_storage.error, 'A', sizeof(result_storage.error));
	result_storage.status = fsimg_mtree_parse(&session, &callbacks,
	    &result_storage.error);
	CHECK(fsimg_ctx_get_stats(ctx, &after, &error) == FSIMG_OK);
	CHECK(after.memory_bytes == before.memory_bytes);
	fsimg_ctx_free(ctx);
	return (&result_storage);
}

static const parse_result_t *
expect_accepted(const char *manifest)
{
	parse_setup_t setup;
	parse_result_t *result;

	setup_defaults(&setup);
	result = parse_manifest(manifest, strlen(manifest), &setup);
	if (result->status != FSIMG_OK)
		fprintf(stderr, "refused %s: %s\n", manifest,
		    result->error.message);
	CHECK(result->status == FSIMG_OK);
	return (result);
}

/* The byte offset at which physical line `line` (from one) begins. */
static uint64_t
line_start(const char *text, size_t length, unsigned line)
{
	size_t offset;
	unsigned current;

	offset = 0;
	for (current = 1; current < line; offset++) {
		CHECK(offset < length);
		if (text[offset] == '\n')
			current++;
	}
	return (offset);
}

/* Parse a manifest that must be refused with status, naming line by its
 * byte offset and its number, and return the result for further checks.
 */
static const parse_result_t *
check_refusal(const char *manifest, size_t length, const parse_setup_t *setup,
    fsimg_status_t status, unsigned line)
{
	const parse_result_t *result;
	char prefix[64];

	result = parse_manifest(manifest, length, setup);
	snprintf(prefix, sizeof(prefix), "manifest line %u: ", line);
	if (result->status != status ||
	    result->error.offset != line_start(manifest, length, line) ||
	    strncmp(result->error.message, prefix, strlen(prefix)) != 0)
		fprintf(stderr, "case %.*s: status %d offset %ju message %s\n",
		    (int)(length > 200 ? 200 : length), manifest,
		    (int)result->status, (uintmax_t)result->error.offset,
		    result->error.message);
	CHECK(result->status == status);
	CHECK(result->error.offset_valid);
	CHECK(result->error.offset == line_start(manifest, length, line));
	CHECK(strncmp(result->error.message, prefix, strlen(prefix)) == 0);
	return (result);
}

/* Check each case under the default limits; return the last result. */
static const parse_result_t *
check_refusals(const refusal_case_t *cases, size_t count)
{
	const parse_result_t *result;
	parse_setup_t setup;

	setup_defaults(&setup);
	result = NULL;
	for (size_t i = 0; i < count; i++) {
		result = check_refusal(cases[i].manifest,
		    strlen(cases[i].manifest), &setup, cases[i].status,
		    cases[i].line);
		if (cases[i].mention != NULL &&
		    strstr(result->error.message, cases[i].mention) == NULL) {
			fprintf(stderr, "case %s: message %s lacks %s\n",
			    cases[i].manifest, result->error.message,
			    cases[i].mention);
			CHECK(false);
		}
	}
	return (result);
}

static const recorded_entry_t *
find_entry(const parse_result_t *result, const char *path)
{
	for (size_t i = 0; i < result->count; i++) {
		if (strcmp(result->entries[i].path, path) == 0)
			return (&result->entries[i]);
	}
	fprintf(stderr, "no entry %s\n", path);
	CHECK(false);
	return (NULL);
}

/* Each writer's manifest yields the host tree, in its order, with owner
 * 501:0, the time to the nanosecond, and, where the writer gave one, a
 * digest that is the SHA-256 of the file's bytes.
 */
static void
check_host_tree(const parse_result_t *result, bool digests)
{
	const expected_entry_t *expected;
	const recorded_entry_t *entry;
	unsigned char digest[FSIMG_SHA256_DIGEST_LENGTH];
	fsimg_sha256_t sha;

	CHECK(result->status == FSIMG_OK);
	CHECK(result->count == sizeof(host_tree) / sizeof(host_tree[0]));
	for (size_t i = 0; i < result->count; i++) {
		expected = &host_tree[i];
		entry = &result->entries[i];
		CHECK(strcmp(entry->path, expected->path) == 0);
		CHECK(entry->kind == expected->kind);
		CHECK(entry->size == expected->size);
		CHECK(entry->metadata.valid == (FSIMG_META_OWNER |
		    FSIMG_META_MODE | FSIMG_META_MODIFIED));
		CHECK(entry->metadata.uid == 501 && entry->metadata.gid == 0);
		CHECK(entry->metadata.mode == expected->mode);
		CHECK(entry->metadata.modified.seconds == 1792574343);
		CHECK(entry->metadata.modified.nanoseconds ==
		    expected->nanoseconds);
		CHECK(entry->metadata.modified.precision_ns == 1);
		CHECK(entry->metadata.modified.basis == FSIMG_TIME_UTC);
		CHECK(entry->content == FSIMG_MTREE_CONTENT_HOST);
		CHECK(!entry->has_contents && entry->hole_count == 0);
		if (expected->kind == FSIMG_ENTRY_SYMLINK) {
			CHECK(strcmp(entry->link, expected->bytes) == 0);
			continue;
		}
		CHECK(entry->size_given == (expected->kind ==
		    FSIMG_ENTRY_FILE));
		CHECK(entry->digest_given == (digests &&
		    expected->kind == FSIMG_ENTRY_FILE));
		if (!entry->digest_given)
			continue;
		fsimg_sha256_init(&sha);
		fsimg_sha256_update(&sha, expected->bytes,
		    strlen(expected->bytes));
		fsimg_sha256_final(digest, &sha);
		CHECK(memcmp(entry->digest, digest, sizeof(digest)) == 0);
	}
}

/* The relative form of mtree -c, with continuations, comments, /set, ".."
 * and the closing ".." of the root, and the full form of bsdtar, with and
 * without /set, describe one tree; each is also read one byte, and seven
 * bytes, at a time so that every line straddles a read.
 */
static void
test_host_writers(void)
{
	static const size_t chunks[] = { SIZE_MAX, 1, 7 };
	parse_setup_t setup;

	setup_defaults(&setup);
	for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
		setup.chunk = chunks[i];
		check_host_tree(parse_manifest(mtree_c_manifest,
		    strlen(mtree_c_manifest), &setup), true);
		check_host_tree(parse_manifest(bsdtar_manifest,
		    strlen(bsdtar_manifest), &setup), false);
		check_host_tree(parse_manifest(bsdtar_set_manifest,
		    strlen(bsdtar_set_manifest), &setup), true);
	}
}

static void
test_line_forms(void)
{
	static const char manifest[] =
	    ". type=dir uid=0 gid=0 mode=0755\n"
	    "usr type=dir\n"
	    "bin type=dir\n"
	    "sh type=file\n"
	    "..\n"
	    ". type=dir mode=0700\n"
	    "lib type=dir\n"
	    "..\n"
	    "..\n"
	    "./etc type=dir\n"
	    "etc/passwd type=file\n"
	    "./usr/share type=dir\n"
	    "tmp type=dir\n"
	    "..\n"
	    "..\n";
	static const char *const paths[] = { "/usr", "/usr/bin",
	    "/usr/bin/sh", "/usr", "/usr/lib", "/etc", "/etc/passwd",
	    "/usr/share", "/tmp" };
	static const refusal_case_t refusals[] = {
		/* The root was closed, so nothing is left to leave. */
		{ ". type=dir\n..\n..\n", FSIMG_EINVAL, 3, "no directory" },
		{ "..\n", FSIMG_EINVAL, 1, "no directory" },
		{ "a type=dir\n.. type=dir\n", FSIMG_EINVAL, 2, "keywords" },
		{ ". type=file\n", FSIMG_EINVAL, 1, "image root" },
		{ "/foo type=file\n", FSIMG_EINVAL, 1, "special line" },
		{ "/a type=file\n", FSIMG_EINVAL, 1, "special line" },
		{ "a/ type=file\n", FSIMG_EINVAL, 1, NULL },
		{ "a//b type=file\n", FSIMG_EINVAL, 1, NULL },
		{ "./ type=file\n", FSIMG_EINVAL, 1, NULL },
		{ "a/./b type=file\n", FSIMG_EINVAL, 1, NULL },
		{ "a/../b type=file\n", FSIMG_EINVAL, 1, NULL },
		{ "\\056\\056 type=file\n", FSIMG_EINVAL, 1, NULL },
		{ "\\057a type=file\n", FSIMG_EINVAL, 1, NULL },
		{ "a type=file type=file\n", FSIMG_EINVAL, 1, "repeated" }
	};
	const parse_result_t *result;

	result = expect_accepted(manifest);
	CHECK(result->count == sizeof(paths) / sizeof(paths[0]));
	for (size_t i = 0; i < result->count; i++)
		CHECK(strcmp(result->entries[i].path, paths[i]) == 0);
	/* "." inside /usr is an entry for /usr itself; the root's is not. */
	CHECK(result->entries[3].kind == FSIMG_ENTRY_DIRECTORY);
	CHECK(result->entries[3].metadata.mode == 0700);
	CHECK(result->entries[0].metadata.valid == 0);
	check_refusals(refusals, sizeof(refusals) / sizeof(refusals[0]));

	/* A relative file does not become the current directory. */
	result = expect_accepted("a type=file\nb type=file\n");
	CHECK(strcmp(result->entries[1].path, "/b") == 0);
	/* Without a root line the root is current and "." is optional. */
	result = expect_accepted("a type=dir\nb type=file\n..\nc type=file\n");
	CHECK(strcmp(result->entries[1].path, "/a/b") == 0);
	CHECK(strcmp(result->entries[2].path, "/c") == 0);
	/* An error after the path is known names the entry's path, and the
	 * current directory is unchanged by the failed line.
	 */
	result = check_refusals(&(refusal_case_t){
	    "a type=dir\nb type=bogus\n", FSIMG_EINVAL, 2, "unknown type" }, 1);
	CHECK(strcmp(result->error.path, "/a/b") == 0);
	result = check_refusals(&(refusal_case_t){
	    "a type=dir\n\\377 type=file\n", FSIMG_EINVAL, 2, NULL }, 1);
	CHECK(strcmp(result->error.path, "/a/\377") == 0);
}

static void
test_continuations_and_comments(void)
{
	static const char continued_link[] =
	    "l type=link link=x\\\\\\\n"
	    "\n"
	    "m type=file\n";
	/* macOS mtree -c -k type for a file "f\" and a directory "d\" holding
	 * "x": its comments quote the directory's path raw, so they end in a
	 * backslash, which must not join the directory's line to them.
	 */
	static const char backslash_names[] =
	    "# .\n"
	    "/set type=file\n"
	    ".               type=dir\n"
	    "    f\\134      \n"
	    "\n"
	    "# ./d\\\\\n"
	    "d\\134           type=dir\n"
	    "    x          \n"
	    "# ./d\\\\\n"
	    "..\n"
	    "\n"
	    "..\n";
	static const refusal_case_t refusals[] = {
		/* The continuation's leading blank is the only separator. */
		{ "a type=file\\\nsize=3\n", FSIMG_EINVAL, 1, "unknown type" },
		/* An error names the first physical line of its logical line,
		 * and counting continues past joined lines.
		 */
		{ "a type=file \\\n  bogus=1\n", FSIMG_EINVAL, 1, "\"bogus\"" },
		{ "a type=file \\\n size=1\nb type=bogus\n", FSIMG_EINVAL, 3,
		    NULL },
		{ "# c\n\n  \t\n#x\nb type=bogus\n", FSIMG_EINVAL, 5, NULL },
		{ "a#b type=file\n", FSIMG_EINVAL, 1, "'#'" },
		{ "a type=file #c\n", FSIMG_EINVAL, 1, "'#'" },
		{ "a type=file size=\\043\n", FSIMG_EINVAL, 1, "backslash" }
	};
	const parse_result_t *result;

	result = expect_accepted("a type=file \\\n    size=3 \\\n\tmode=0600\n");
	CHECK(result->count == 1 && result->entries[0].size == 3);
	CHECK(result->entries[0].metadata.mode == 0600);

	/* Comments, the signatures, blank lines and any non-NUL bytes in a
	 * comment, such as a raw UTF-8 path that mtree -c writes there.
	 */
	result = expect_accepted("#mtree\n#mtree v2.0\n   # ./caf\303\251\r\x01\n"
	    "\n \t \n# a\\b\n");
	CHECK(result->count == 0);
	result = expect_accepted("");
	CHECK(result->count == 0);
	result = expect_accepted("a\ttype=file\t \tsize=1\n");
	CHECK(result->count == 1);

	/* An unescaped backslash, an odd run, ending a physical line continues
	 * it: "x\\\" keeps the escaped "\\" and the empty second line ends the
	 * logical line. An even run is an escaped backslash ending a name.
	 */
	result = expect_accepted(continued_link);
	CHECK(result->count == 2);
	CHECK(strcmp(result->entries[0].link, "x\\") == 0);
	result = expect_accepted("/set type=file\nfoo\\\\\n123 type=dir\n");
	CHECK(result->count == 2);
	CHECK(strcmp(result->entries[0].path, "/foo\\") == 0);
	CHECK(result->entries[0].kind == FSIMG_ENTRY_FILE);
	CHECK(strcmp(result->entries[1].path, "/123") == 0);
	CHECK(result->entries[1].kind == FSIMG_ENTRY_DIRECTORY);

	/* A comment never continues, whatever it ends with. Whether a line is
	 * a comment is decided by its first kept byte that is not a blank, so
	 * a continuation backslash at its start, removed by the join, does not
	 * hide the '#' after it.
	 */
	result = expect_accepted("# x\\\na type=file\n");
	CHECK(result->count == 1);
	result = expect_accepted("\\\n# x\\\na type=file\n");
	CHECK(result->count == 1);
	result = expect_accepted("   \\\n# x\\\na type=file\n");
	CHECK(result->count == 1);
	result = expect_accepted("\t\\\n\\\n#c\\\nb type=file\n");
	CHECK(result->count == 1 && strcmp(result->entries[0].path, "/b") == 0);
	result = expect_accepted("\\\n#x\n");
	CHECK(result->count == 0);
	/* An escaped backslash is kept, so the '#' after it is in a word. */
	check_refusals(&(refusal_case_t){ "\\\\\\\n# c\\\nx\n", FSIMG_EINVAL, 1,
	    "'#'" }, 1);
	result = expect_accepted(backslash_names);
	CHECK(result->count == 3);
	CHECK(strcmp(result->entries[0].path, "/f\\") == 0);
	CHECK(strcmp(result->entries[1].path, "/d\\") == 0);
	CHECK(result->entries[1].kind == FSIMG_ENTRY_DIRECTORY);
	CHECK(strcmp(result->entries[2].path, "/d\\/x") == 0);
	check_refusals(refusals, sizeof(refusals) / sizeof(refusals[0]));
}

/* Every escape: each byte value 001 through 377 as a link target, "\\",
 * and names with spaces, '#', '\' and UTF-8. Then each malformed escape.
 */
static void
test_escapes(void)
{
	static const refusal_case_t refusals[] = {
		{ "a\\s type=file\n", FSIMG_EINVAL, 1, "escape" },
		{ "a\\n type=file\n", FSIMG_EINVAL, 1, "escape" },
		{ "a\\8 type=file\n", FSIMG_EINVAL, 1, "escape" },
		{ "a\\01 type=file\n", FSIMG_EINVAL, 1, "escape" },
		{ "a\\0 type=file\n", FSIMG_EINVAL, 1, "escape" },
		{ "a\\ type=file\n", FSIMG_EINVAL, 1, "escape" },
		{ "a\\018 type=file\n", FSIMG_EINVAL, 1, "escape" },
		{ "a\\400 type=file\n", FSIMG_EINVAL, 1, "one byte" },
		{ "a\\000 type=file\n", FSIMG_EINVAL, 1, "NUL" },
		{ "l type=link link=a\\000\n", FSIMG_EINVAL, 1, "NUL" },
		{ "l type=link link=a\\x\n", FSIMG_EINVAL, 1, "escape" },
		/* A name must be UTF-8, as every image path must. */
		{ "a\\377 type=file\n", FSIMG_EINVAL, 1, NULL },
		{ "a type=file uname=\\141 uid=0 gid=0\n", FSIMG_EINVAL, 1,
		    "backslash" }
	};
	const parse_result_t *result;
	const recorded_entry_t *entry;
	char *manifest, *cursor;
	size_t capacity;

	capacity = 256 * 40;
	manifest = malloc(capacity);
	CHECK(manifest != NULL);
	cursor = manifest;
	for (unsigned value = 1; value <= 0377; value++)
		cursor += snprintf(cursor, capacity - (size_t)(cursor -
		    manifest), "l%03o type=link link=\\%03o\n", value, value);
	result = expect_accepted(manifest);
	CHECK(result->count == 255);
	for (unsigned value = 1; value <= 0377; value++) {
		entry = &result->entries[value - 1];
		CHECK(entry->link_length == 1);
		CHECK((unsigned char)entry->link[0] == value);
		CHECK(entry->size == 1);
	}
	free(manifest);

	result = expect_accepted(
	    "sp\\040a\\043ce\\134 type=file\n"
	    "caf\\303\\251 type=file\n"
	    "l type=link link=a\\\\b/\\056\\056/c\n"
	    "a\\057b type=file\n"
	    "d\\040\\057e type=file\n");
	CHECK(strcmp(result->entries[0].path, "/sp a#ce\\") == 0);
	CHECK(strcmp(result->entries[1].path, "/caf\303\251") == 0);
	CHECK(strcmp(result->entries[2].link, "a\\b/../c") == 0);
	/* A decoded '/' makes a full entry, wherever it came from. */
	CHECK(strcmp(result->entries[3].path, "/a/b") == 0);
	CHECK(strcmp(result->entries[4].path, "/d /e") == 0);
	check_refusals(refusals, sizeof(refusals) / sizeof(refusals[0]));
}

/* The four classes: carried (here and in the content test), about the
 * host object, unsupported and unknown. A refusal names the keyword.
 */
static void
test_keyword_classes(void)
{
	static const refusal_case_t refusals[] = {
		{ "a type=file cksum=1\n", FSIMG_EUNSUPPORTED, 1, "\"cksum\"" },
		{ "a type=file md5=1\n", FSIMG_EUNSUPPORTED, 1, "\"md5\"" },
		{ "a type=file md5digest=1\n", FSIMG_EUNSUPPORTED, 1, NULL },
		{ "a type=file sha1=1\n", FSIMG_EUNSUPPORTED, 1, NULL },
		{ "a type=file sha1digest=1\n", FSIMG_EUNSUPPORTED, 1, NULL },
		{ "a type=file rmd160=1\n", FSIMG_EUNSUPPORTED, 1, NULL },
		{ "a type=file rmd160digest=1\n", FSIMG_EUNSUPPORTED, 1, NULL },
		{ "a type=file ripemd160digest=1\n", FSIMG_EUNSUPPORTED, 1,
		    NULL },
		{ "a type=file sha384=1\n", FSIMG_EUNSUPPORTED, 1, NULL },
		{ "a type=file sha384digest=1\n", FSIMG_EUNSUPPORTED, 1, NULL },
		{ "a type=file sha512=1\n", FSIMG_EUNSUPPORTED, 1, NULL },
		{ "a type=file sha512digest=1\n", FSIMG_EUNSUPPORTED, 1,
		    "\"sha512digest\"" },
		{ "a type=file optional\n", FSIMG_EUNSUPPORTED, 1,
		    "\"optional\"" },
		{ "a type=file ignore\n", FSIMG_EUNSUPPORTED, 1, "\"ignore\"" },
		{ "a type=file nochange\n", FSIMG_EUNSUPPORTED, 1, NULL },
		{ "/set md5=1\n", FSIMG_EUNSUPPORTED, 1, "\"md5\"" },
		{ "a type=file flags=uchg\n", FSIMG_EUNSUPPORTED, 1, "flags" },
		{ "a type=block\n", FSIMG_EUNSUPPORTED, 1, "\"type\"" },
		{ "a type=char\n", FSIMG_EUNSUPPORTED, 1, "\"type\"" },
		{ "a type=socket\n", FSIMG_EUNSUPPORTED, 1, "\"type\"" },
		{ "a type=file mode=u+rwx\n", FSIMG_EUNSUPPORTED, 1,
		    "symbolic" },
		{ "a type=file uname=root\n", FSIMG_EUNSUPPORTED, 1,
		    "\"uname\"" },
		{ "a type=file gname=wheel uid=0\n", FSIMG_EUNSUPPORTED, 1,
		    "\"gname\"" },
		{ "a type=file foo=1\n", FSIMG_EINVAL, 1, "\"foo\"" },
		{ "a type=file fsimg.other=1\n", FSIMG_EINVAL, 1,
		    "\"fsimg.other\"" },
		{ "a type=file Type=file\n", FSIMG_EINVAL, 1, "\"Type\"" },
		{ "/unset foo\n", FSIMG_EINVAL, 1, "\"foo\"" },
		{ "a type=file uid\n", FSIMG_EINVAL, 1, "without a value" },
		{ "a type=file uid=\n", FSIMG_EINVAL, 1, "empty value" },
		{ "a type=file =1\n", FSIMG_EINVAL, 1, "unknown keyword" },
		{ "a type=file uid=1 uid=1 gid=0\n", FSIMG_EINVAL, 1,
		    "repeated" },
		{ "/set uid=1 uid=1\n", FSIMG_EINVAL, 1, "repeated" },
		{ "/unset uid uid\n", FSIMG_EINVAL, 1, "repeated" },
		{ "/unset all all\n", FSIMG_EINVAL, 1, "repeated" },
		{ "/unset md5\n", FSIMG_EUNSUPPORTED, 1, "\"md5\"" },
		{ "/unset uid ignore\n", FSIMG_EUNSUPPORTED, 1, "\"ignore\"" },
		{ "a type=file sha256=" "e3b0c44298fc1c149afbf4c8996fb924"
		    "27ae41e4649b934ca495991b7852b855 sha256digest="
		    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b"
		    "7852b855\n", FSIMG_EINVAL, 1, "repeated" },
		{ "a type=file nlink=x\n", FSIMG_EINVAL, 1, NULL },
		{ "a type=file inode=-1\n", FSIMG_EINVAL, 1, NULL },
		{ "a type=file device=a,,b\n", FSIMG_EINVAL, 1, "device" },
		{ "a type=file device=1,2,3,4,5\n", FSIMG_EINVAL, 1, "device" },
		{ "a type=file resdevice=native,1,\n", FSIMG_EINVAL, 1,
		    "device" },
		{ "a type=file device=1:2\n", FSIMG_EINVAL, 1, "device" },
		{ "a type=bogus\n", FSIMG_EINVAL, 1, "unknown type" },
		{ "a type=File\n", FSIMG_EINVAL, 1, "unknown type" }
	};
	const parse_result_t *result;

	/* What mtree -c and bsdtar emit about the host object, accepted
	 * and not consulted.
	 */
	result = expect_accepted(
	    "a type=file nlink=2 inode=12 device=native,1,4 resdevice=88 "
	    "tags=a,b uname=root gname=wheel uid=0 gid=0 flags=none\n"
	    "d type=dir size=4096 nlink=3\n"
	    "p type=fifo size=0\n"
	    "/unset nlink tags\n");
	CHECK(result->count == 3);
	CHECK(result->entries[0].metadata.valid == FSIMG_META_OWNER);
	CHECK(result->entries[1].size == 0 && result->entries[2].size == 0);
	check_refusals(refusals, sizeof(refusals) / sizeof(refusals[0]));
}

/* Each carried number at its boundaries, as the design's value table
 * gives them, and the 20-digit decimal and 22-digit octal bounds.
 */
static void
test_numeric_values(void)
{
	static const refusal_case_t refusals[] = {
		{ "a type=file uid=4294967296 gid=0\n", FSIMG_ERANGE, 1, NULL },
		{ "a type=file uid=0 gid=4294967296\n", FSIMG_ERANGE, 1, NULL },
		{ "a type=file uid=-1 gid=0\n", FSIMG_EINVAL, 1, NULL },
		{ "a type=file uid=+1 gid=0\n", FSIMG_EINVAL, 1, NULL },
		{ "a type=file mode=10000\n", FSIMG_ERANGE, 1, "07777" },
		{ "a type=file mode=0008\n", FSIMG_EINVAL, 1, "octal" },
		{ "a type=file mode=00000000000000000000000\n", FSIMG_ERANGE, 1,
		    "22" },
		{ "a type=file mode=zz\n", FSIMG_EINVAL, 1, "malformed mode" },
		{ "a type=file size=18446744073709551616\n", FSIMG_ERANGE, 1,
		    NULL },
		{ "a type=file size=000000000000000000001\n", FSIMG_ERANGE, 1,
		    "20 digits" },
		{ "a type=file size=12a\n", FSIMG_EINVAL, 1, "malformed" },
		{ "a type=file size=0x10\n", FSIMG_EINVAL, 1, "malformed" },
		{ "a type=file time=9223372036854775808\n", FSIMG_ERANGE, 1,
		    NULL },
		{ "a type=file time=-9223372036854775809\n", FSIMG_ERANGE, 1,
		    NULL },
		{ "a type=file time=1.0000000000\n", FSIMG_EINVAL, 1,
		    "nine digits" },
		{ "a type=file time=1.\n", FSIMG_EINVAL, 1, "nine digits" },
		{ "a type=file time=.5\n", FSIMG_EINVAL, 1, NULL },
		{ "a type=file time=1.2.3\n", FSIMG_EINVAL, 1, NULL },
		{ "a type=file time=1.-5\n", FSIMG_EINVAL, 1, NULL },
		{ "a type=file time=-\n", FSIMG_EINVAL, 1, NULL },
		{ "a type=file sha256digest=e3b0c44298fc1c149afbf4c8996fb924"
		    "27ae41e4649b934ca495991b7852b85\n", FSIMG_EINVAL, 1,
		    "64" },
		{ "a type=file sha256digest=g3b0c44298fc1c149afbf4c8996fb924"
		    "27ae41e4649b934ca495991b7852b855\n", FSIMG_EINVAL, 1,
		    "64" }
	};
	const parse_result_t *result;
	const recorded_entry_t *entry;

	result = expect_accepted(
	    "a type=file uid=4294967295 gid=0 mode=07777 size=18446744073709551615 "
	    "time=9223372036854775807.999999999\n"
	    "b type=file uid=0 gid=4294967295 mode=0000000000000000007777 "
	    "time=-9223372036854775808.5 size=00000000000000000001\n"
	    "c type=file mode=644 time=-1.000000001 sha256digest="
	    "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855\n"
	    "d type=file time=-0\n");
	entry = find_entry(result, "/a");
	CHECK(entry->metadata.uid == UINT32_MAX && entry->metadata.mode == 07777);
	CHECK(entry->size == UINT64_MAX);
	CHECK(entry->metadata.modified.seconds == INT64_MAX);
	CHECK(entry->metadata.modified.nanoseconds == 999999999);
	entry = find_entry(result, "/b");
	CHECK(entry->metadata.gid == UINT32_MAX && entry->size == 1);
	CHECK(entry->metadata.modified.seconds == INT64_MIN);
	CHECK(entry->metadata.modified.nanoseconds == 500000000);
	entry = find_entry(result, "/c");
	CHECK(entry->metadata.mode == 0644);
	CHECK(entry->metadata.modified.seconds == -1);
	CHECK(entry->metadata.modified.nanoseconds == 1);
	CHECK(entry->digest[0] == 0xE3 && entry->digest[31] == 0x55);
	CHECK(entry->metadata.valid == (FSIMG_META_MODE |
	    FSIMG_META_MODIFIED));
	entry = find_entry(result, "/d");
	CHECK(entry->metadata.modified.seconds == 0);
	check_refusals(refusals, sizeof(refusals) / sizeof(refusals[0]));
}

/* /set, /unset and /unset all, and the rule that a default which does
 * not apply to an entry is passed over while the same keyword on the
 * entry's own line is refused.
 */
static void
test_defaults(void)
{
	static const refusal_case_t refusals[] = {
		{ "/set type=file\n/unset all\na size=1\n", FSIMG_EINVAL, 3,
		    "no type" },
		{ "/set uid=1 gid=2\n/unset gid\na type=file\n", FSIMG_EINVAL, 3,
		    "together" },
		{ "/set fsimg.seed=7\nd type=dir fsimg.seed=7\n", FSIMG_EINVAL,
		    2, "\"fsimg.seed\"" },
		{ "/set type=file mode=bad\n", FSIMG_EINVAL, 1, "mode" },
		/* A link's own size must match its target. */
		{ "/set type=file size=5\nl type=link link=x size=2\n",
		    FSIMG_EINVAL, 2, "size" }
	};
	const parse_result_t *result;
	const recorded_entry_t *entry;

	result = expect_accepted(
	    "/set type=file uid=1 gid=2 mode=0600\n"
	    "a size=1\n"
	    "/set mode=0644\n"
	    "b size=2 mode=0400\n"
	    "/unset mode\n"
	    "c size=3\n"
	    "/unset all\n"
	    "d type=dir\n");
	entry = find_entry(result, "/a");
	CHECK(entry->kind == FSIMG_ENTRY_FILE && entry->metadata.mode == 0600);
	CHECK(entry->metadata.uid == 1 && entry->metadata.gid == 2);
	CHECK(find_entry(result, "/b")->metadata.mode == 0400);
	entry = find_entry(result, "/c");
	CHECK(entry->metadata.valid == FSIMG_META_OWNER);
	CHECK(find_entry(result, "/d")->metadata.valid == 0);

	/* A size default describes files; a link takes its target's length. */
	result = expect_accepted("/set type=file size=5\nl type=link link=ab\n");
	CHECK(result->entries[0].size == 2);

	result = expect_accepted(
	    "/set type=file fsimg.content=seeded fsimg.seed=7 link=x "
	    "sha256digest=e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934c"
	    "a495991b7852b855 fsimg.holes=1:1\n"
	    "./d type=dir\n"
	    "l type=link\n"
	    "p type=fifo\n"
	    "f size=4\n"
	    "h fsimg.content=host\n"
	    "z fsimg.content=zero size=0\n");
	CHECK(result->count == 6);
	CHECK(strcmp(find_entry(result, "/l")->link, "x") == 0);
	entry = find_entry(result, "/f");
	CHECK(entry->content == FSIMG_MTREE_CONTENT_SEEDED);
	CHECK(entry->seed == 7 && entry->hole_count == 1);
	CHECK(entry->digest_given);
	entry = find_entry(result, "/h");
	CHECK(entry->content == FSIMG_MTREE_CONTENT_HOST);
	CHECK(entry->hole_count == 0 && !entry->size_given);
	CHECK(entry->digest_given);
	CHECK(find_entry(result, "/z")->content == FSIMG_MTREE_CONTENT_ZERO);
	check_refusals(refusals, sizeof(refusals) / sizeof(refusals[0]));
}

/* How a file's bytes are given: host contents, zero, seeded and
 * positional, holes, and the keywords that do not apply to a kind.
 */
static void
test_content_keywords(void)
{
	static const refusal_case_t refusals[] = {
		{ "d type=dir link=x\n", FSIMG_EINVAL, 1, "\"link\"" },
		{ "f type=file link=x\n", FSIMG_EINVAL, 1, "\"link\"" },
		{ "l type=link\n", FSIMG_EINVAL, 1, "without link" },
		{ "l type=link link=ab size=1\n", FSIMG_EINVAL, 1, "size" },
		{ "d type=dir contents=a\n", FSIMG_EINVAL, 1, "\"contents\"" },
		{ "f type=file fsimg.content=zero size=0 contents=a\n",
		    FSIMG_EINVAL, 1, "\"contents\"" },
		{ "f type=file contents=/etc/passwd\n", FSIMG_EINVAL, 1,
		    "contents" },
		{ "f type=file contents=a/../b\n", FSIMG_EINVAL, 1,
		    "contents" },
		{ "f type=file contents=a//b\n", FSIMG_EINVAL, 1, "contents" },
		{ "f type=file contents=./a\n", FSIMG_EINVAL, 1, "contents" },
		{ "f type=file contents=a/\n", FSIMG_EINVAL, 1, "contents" },
		{ "f type=file contents=\\056\\056\n", FSIMG_EINVAL, 1,
		    "contents" },
		{ "d type=dir sha256digest=e3b0c44298fc1c149afbf4c8996fb924"
		    "27ae41e4649b934ca495991b7852b855\n", FSIMG_EINVAL, 1,
		    "\"sha256digest\"" },
		{ "d type=dir fsimg.content=zero\n", FSIMG_EINVAL, 1,
		    "\"fsimg.content\"" },
		{ "f type=file fsimg.content=random size=1\n", FSIMG_EINVAL, 1,
		    "fsimg.content" },
		{ "f type=file fsimg.seed=1\n", FSIMG_EINVAL, 1,
		    "\"fsimg.seed\"" },
		{ "f type=file fsimg.content=zero size=1 fsimg.seed=1\n",
		    FSIMG_EINVAL, 1, "\"fsimg.seed\"" },
		{ "f type=file fsimg.content=zero size=4 fsimg.holes=0:1\n",
		    FSIMG_EINVAL, 1, "\"fsimg.holes\"" },
		{ "f type=file fsimg.holes=0:1\n", FSIMG_EINVAL, 1,
		    "\"fsimg.holes\"" },
		{ "f type=file fsimg.content=seeded size=1\n", FSIMG_EINVAL, 1,
		    "fsimg.seed" },
		{ "f type=file fsimg.content=zero\n", FSIMG_EINVAL, 1,
		    "without size" },
		{ "f type=file fsimg.content=seeded fsimg.seed=1 size=4 "
		    "fsimg.holes=0:1,1:1\n", FSIMG_EINVAL, 1, "touching" },
		{ "f type=file fsimg.content=seeded fsimg.seed=1 size=4 "
		    "fsimg.holes=0:2,1:1\n", FSIMG_EINVAL, 1, "overlapping" },
		{ "f type=file fsimg.content=seeded fsimg.seed=1 size=4 "
		    "fsimg.holes=2:1,0:1\n", FSIMG_EINVAL, 1, "order" },
		{ "f type=file fsimg.content=seeded fsimg.seed=1 size=4 "
		    "fsimg.holes=1:0\n", FSIMG_EINVAL, 1, "zero length" },
		{ "f type=file fsimg.content=seeded fsimg.seed=1 size=4 "
		    "fsimg.holes=2:3\n", FSIMG_EINVAL, 1, "past size" },
		{ "f type=file fsimg.content=positional fsimg.seed=1 size=4 "
		    "fsimg.holes=1:1,\n", FSIMG_EINVAL, 1, "offset:length" },
		{ "f type=file fsimg.content=positional fsimg.seed=1 size=4 "
		    "fsimg.holes=1\n", FSIMG_EINVAL, 1, "offset:length" },
		{ "f type=file fsimg.content=positional fsimg.seed=1 size=4 "
		    "fsimg.holes=1:2:3\n", FSIMG_EINVAL, 1, NULL },
		{ "f type=file fsimg.content=positional fsimg.seed=1 size=4 "
		    "fsimg.holes=18446744073709551615:1\n", FSIMG_ERANGE, 1,
		    NULL },
		{ "f type=file uid=1\n", FSIMG_EINVAL, 1, "together" }
	};
	const parse_result_t *result;
	const recorded_entry_t *entry;
	char *manifest, *cursor;
	size_t capacity;

	result = expect_accepted(
	    "h type=file contents=dir/caf\\303\\251\\040x size=5\n"
	    "z type=file fsimg.content=zero size=0\n"
	    "s type=file fsimg.content=seeded fsimg.seed=18446744073709551615 "
	    "size=10 fsimg.holes=0:1,2:3,9:1\n"
	    "p type=file fsimg.content=positional fsimg.seed=0 size=16\n"
	    "l type=link link=../t size=4\n");
	entry = find_entry(result, "/h");
	CHECK(entry->has_contents);
	CHECK(strcmp(entry->contents, "dir/caf\303\251 x") == 0);
	CHECK(entry->size_given && entry->size == 5);
	entry = find_entry(result, "/z");
	CHECK(entry->content == FSIMG_MTREE_CONTENT_ZERO && entry->size_given);
	entry = find_entry(result, "/s");
	CHECK(entry->seed == UINT64_MAX && entry->hole_count == 3);
	CHECK(entry->holes[1].offset == 2 && entry->holes[1].length == 3);
	CHECK(entry->holes[2].offset + entry->holes[2].length == entry->size);
	CHECK(find_entry(result, "/p")->content ==
	    FSIMG_MTREE_CONTENT_POSITIONAL);
	CHECK(find_entry(result, "/l")->size == 4);
	check_refusals(refusals, sizeof(refusals) / sizeof(refusals[0]));

	/* 64 holes are accepted and a 65th is refused. */
	capacity = 64 * 16 + 128;
	manifest = malloc(capacity);
	CHECK(manifest != NULL);
	for (unsigned holes = 64; holes <= 65; holes++) {
		cursor = manifest + snprintf(manifest, capacity,
		    "f type=file fsimg.content=seeded fsimg.seed=1 size=200 "
		    "fsimg.holes=");
		for (unsigned i = 0; i < holes; i++)
			cursor += snprintf(cursor, capacity - (size_t)(cursor -
			    manifest), "%s%u:1", i == 0 ? "" : ",", 2 * i);
		snprintf(cursor, capacity - (size_t)(cursor - manifest), "\n");
		if (holes == 64) {
			result = expect_accepted(manifest);
			CHECK(result->entries[0].hole_count == 64);
		} else {
			check_refusals(&(refusal_case_t){ manifest,
			    FSIMG_EINVAL, 1, "64 holes" }, 1);
		}
	}
	free(manifest);
}

/* Each bound at its limit and one past it, under small context limits:
 * a path limit of 64 bytes and a depth of 4 make the logical line limit
 * 8 * 64 + 4 * 1024 + 4096 = 8704 bytes.
 */
static void
test_bounds(void)
{
	const parse_result_t *result;
	parse_setup_t setup;
	char *manifest;
	char name[256];
	size_t limit, length;

	setup_defaults(&setup);
	setup.options.max_path_bytes = 64;
	setup.options.max_depth = 4;
	limit = 8 * 64 + 4 * 1024 + 4096;
	CHECK(limit == 8704);
	manifest = malloc(2 * limit + 64);
	CHECK(manifest != NULL);

	/* An entry line of exactly the limit is accepted and one byte more
	 * is refused, naming the line and the limit. The entry is the word
	 * "a type=file" padded with blanks to the length under test.
	 */
	for (length = limit; length <= limit + 1; length++) {
		memset(manifest, ' ', length);
		memcpy(manifest, "a type=file", 11);
		manifest[length] = '\n';
		if (length == limit) {
			result = parse_manifest(manifest, length + 1, &setup);
			CHECK(result->status == FSIMG_OK && result->count == 1);
		} else {
			result = check_refusal(manifest, length + 1, &setup,
			    FSIMG_ELIMIT, 1);
			CHECK(result->error.resource == FSIMG_RESOURCE_PATH);
			CHECK(result->error.limit == limit);
		}
	}
	/* The same line split by a continuation after its first 100 bytes:
	 * the backslash and LF are removed, so the joined length counts.
	 */
	for (length = limit; length <= limit + 1; length++) {
		memset(manifest, ' ', length + 3);
		memcpy(manifest, "a type=file", 11);
		memcpy(manifest + 100, "\\\n", 2);
		manifest[length + 2] = '\n';
		if (length == limit) {
			result = parse_manifest(manifest, length + 3, &setup);
			CHECK(result->status == FSIMG_OK && result->count == 1);
		} else {
			check_refusal(manifest, length + 3, &setup, FSIMG_ELIMIT,
			    1);
		}
	}
	/* A line far past the limit fails as soon as the buffer fills. */
	memset(manifest, 'a', 2 * limit);
	result = check_refusal(manifest, 2 * limit, &setup, FSIMG_ELIMIT, 1);
	CHECK(result->error.observed == limit + 1);

	/* 64 words are split and read, so "/unset" and 63 "all" fail on the
	 * repeated keyword; a 65th word fails the word bound first. No line
	 * of 64 words is valid, since fewer than 63 keywords can be given.
	 */
	length = (size_t)snprintf(manifest, limit, "/unset");
	for (unsigned i = 0; i < 63; i++)
		length += (size_t)snprintf(manifest + length, limit - length,
		    " all");
	memcpy(manifest + length, "\n", 2);
	result = check_refusal(manifest, length + 1, &setup, FSIMG_EINVAL, 1);
	CHECK(strstr(result->error.message, "repeated") != NULL);
	memcpy(manifest + length, " all\n", 6);
	result = check_refusal(manifest, length + 5, &setup, FSIMG_EINVAL, 1);
	CHECK(strstr(result->error.message, "64 words") != NULL);

	/* A keyword name of 32 bytes is looked up; 33 is refused unread. */
	snprintf(manifest, limit, "a type=file %032d=1\n", 0);
	result = check_refusal(manifest, strlen(manifest), &setup,
	    FSIMG_EINVAL, 1);
	CHECK(strstr(result->error.message, "unknown") != NULL);
	snprintf(manifest, limit, "a type=file %033d=1\n", 0);
	result = check_refusal(manifest, strlen(manifest), &setup,
	    FSIMG_EINVAL, 1);
	CHECK(strstr(result->error.message, "32 bytes") != NULL);

	/* An image path of 63 bytes fits a 64-byte limit with its NUL; 64
	 * does not, in relative and in full form; likewise a contents= path.
	 */
	memset(name, 'n', sizeof(name) - 1);
	name[sizeof(name) - 1] = '\0';
	snprintf(manifest, limit, "%.62s type=file\n", name);
	CHECK(parse_manifest(manifest, strlen(manifest), &setup)->status ==
	    FSIMG_OK);
	snprintf(manifest, limit, "%.63s type=file\n", name);
	result = check_refusal(manifest, strlen(manifest), &setup,
	    FSIMG_ELIMIT, 1);
	CHECK(result->error.resource == FSIMG_RESOURCE_PATH);
	CHECK(result->error.observed == 65);
	snprintf(manifest, limit, "d type=dir\n%.60s type=file\n", name);
	CHECK(parse_manifest(manifest, strlen(manifest), &setup)->status ==
	    FSIMG_OK);
	snprintf(manifest, limit, "d type=dir\n%.61s type=file\n", name);
	check_refusal(manifest, strlen(manifest), &setup, FSIMG_ELIMIT, 2);
	snprintf(manifest, limit, "./d/%.60s type=file\n", name);
	CHECK(parse_manifest(manifest, strlen(manifest), &setup)->status ==
	    FSIMG_OK);
	snprintf(manifest, limit, "./d/%.61s type=file\n", name);
	check_refusal(manifest, strlen(manifest), &setup, FSIMG_ELIMIT, 1);
	snprintf(manifest, limit, "%.200s/a type=file\n", name);
	check_refusal(manifest, strlen(manifest), &setup, FSIMG_ELIMIT, 1);
	snprintf(manifest, limit, "f type=file contents=%.63s\n", name);
	CHECK(parse_manifest(manifest, strlen(manifest), &setup)->status ==
	    FSIMG_OK);
	snprintf(manifest, limit, "f type=file contents=%.64s\n", name);
	check_refusal(manifest, strlen(manifest), &setup, FSIMG_ELIMIT, 1);

	/* Depth 4 is accepted and 5 refused, in both forms. */
	snprintf(manifest, limit, "a/b/c/d type=file\n");
	CHECK(parse_manifest(manifest, strlen(manifest), &setup)->status ==
	    FSIMG_OK);
	snprintf(manifest, limit, "a/b/c/d/e type=file\n");
	result = check_refusal(manifest, strlen(manifest), &setup,
	    FSIMG_ELIMIT, 1);
	CHECK(result->error.resource == FSIMG_RESOURCE_DEPTH);
	snprintf(manifest, limit, "a type=dir\nb type=dir\nc type=dir\n"
	    "d type=file\nd type=dir\ne type=file\n");
	result = check_refusal(manifest, strlen(manifest), &setup,
	    FSIMG_ELIMIT, 6);
	CHECK(result->error.resource == FSIMG_RESOURCE_DEPTH);

	/* A link target of 1024 bytes is accepted, 1025 is not. */
	length = (size_t)snprintf(manifest, limit, "l type=link link=");
	memset(manifest + length, 't', 1025);
	memcpy(manifest + length + 1024, "\n", 1);
	result = parse_manifest(manifest, length + 1025, &setup);
	CHECK(result->status == FSIMG_OK);
	CHECK(result->entries[0].link_length == 1024);
	memcpy(manifest + length + 1024, "t\n", 2);
	check_refusal(manifest, length + 1026, &setup, FSIMG_EINVAL, 1);

	/* Each physical line, blank, comment and continued lines included,
	 * is one unit of work: three lines fit a budget of three and a fourth
	 * does not, even when it only continues the line before.
	 */
	setup.options.max_work = 3;
	CHECK(parse_manifest("#\n\na type=file\n", 15, &setup)->status ==
	    FSIMG_OK);
	result = check_refusal("#\n\na type=file\n#\n", 17, &setup,
	    FSIMG_ELIMIT, 4);
	CHECK(result->error.resource == FSIMG_RESOURCE_WORK);
	result = check_refusal("#\n\na type=file \\\n size=1\n", 25, &setup,
	    FSIMG_ELIMIT, 3);
	CHECK(result->error.resource == FSIMG_RESOURCE_WORK);
	free(manifest);

	/* A run of lines that only continue grows no line, so only the work
	 * charge stops it.
	 */
	setup.options.max_work = 10;
	length = 100000 * 2;
	manifest = malloc(length + 16);
	CHECK(manifest != NULL);
	for (size_t i = 0; i < length; i += 2)
		memcpy(manifest + i, "\\\n", 2);
	memcpy(manifest + length, "a type=file\n", 12);
	result = check_refusal(manifest, length + 12, &setup, FSIMG_ELIMIT, 1);
	CHECK(result->error.resource == FSIMG_RESOURCE_WORK);
	setup.options.max_work = 0;
	free(manifest);

	/* The line limit holds a full-length encoded name and a full-length
	 * encoded contents= path together: with a 4096-byte path limit, a
	 * 4094-byte path and a 4095-byte contents path, every byte "\156",
	 * make a line of over 32 KiB, which a limit counting one path refuses.
	 */
	setup.options.max_path_bytes = 4096;
	manifest = malloc(2 * 4 * 4096 + 64);
	CHECK(manifest != NULL);
	length = (size_t)snprintf(manifest, 8, "./");
	for (unsigned i = 0; i < 4093; i++)
		length += (size_t)snprintf(manifest + length, 8, "\\156");
	length += (size_t)snprintf(manifest + length, 32,
	    " type=file contents=");
	for (unsigned i = 0; i < 4095; i++)
		length += (size_t)snprintf(manifest + length, 8, "\\156");
	memcpy(manifest + length, "\n", 2);
	CHECK(length > 32768);
	result = parse_manifest(manifest, length + 1, &setup);
	CHECK(result->status == FSIMG_OK && result->count == 1);
	CHECK(strlen(result->entries[0].path) == 4094);
	CHECK(strlen(result->entries[0].contents) == 4095);
	free(manifest);

	/* The buffers are charged to the memory budget: a 2 MiB path limit
	 * makes them exceed the 16 MiB minimum.
	 */
	setup.options.memory_bytes = 16 * 1024 * 1024;
	setup.options.max_path_bytes = 2 * 1024 * 1024;
	result = parse_manifest("a type=file\n", 12, &setup);
	CHECK(result->status == FSIMG_ELIMIT);
	CHECK(result->error.resource == FSIMG_RESOURCE_MEMORY);
}

static void
test_malformed(void)
{
	static const char nul_entry[] = "a type=file\nb type=\0file\n";
	static const char nul_comment[] = "# a\0b\n";
	static const refusal_case_t refusals[] = {
		{ "a type=file\r\n", FSIMG_EINVAL, 1, "printable" },
		{ "a\x80 type=file\n", FSIMG_EINVAL, 1, "printable" },
		{ "a type=file\x0b size=1\n", FSIMG_EINVAL, 1, "printable" },
		{ "a type=file\x7f\n", FSIMG_EINVAL, 1, "printable" },
		/* Input that stops inside a line was cut short. */
		{ "a type=file", FSIMG_EINVAL, 1, "inside a line" },
		{ "a type=file\nb type=fi", FSIMG_EINVAL, 2, "inside a line" },
		{ "a type=file \\\n", FSIMG_EINVAL, 1, "inside a line" },
		{ "a type=file\n   ", FSIMG_EINVAL, 2, "inside a line" },
		{ "#mtree", FSIMG_EINVAL, 1, "inside a line" },
		{ "a\\04\n", FSIMG_EINVAL, 1, "escape" },
		{ "a\n", FSIMG_EINVAL, 1, "no type" },
		{ "/set\n/unset\n=\n", FSIMG_EINVAL, 3, NULL }
	};
	const parse_result_t *result;
	parse_setup_t setup;

	setup_defaults(&setup);
	result = check_refusal(nul_entry, sizeof(nul_entry) - 1, &setup,
	    FSIMG_EINVAL, 2);
	CHECK(strstr(result->error.message, "NUL") != NULL);
	check_refusal(nul_comment, sizeof(nul_comment) - 1, &setup,
	    FSIMG_EINVAL, 1);
	check_refusals(refusals, sizeof(refusals) / sizeof(refusals[0]));
}

/* A failing read or entry callback ends the parse with its own status,
 * to which the parser adds the line; a callback that fails without
 * filling the error record gets a generic one; cancellation is polled
 * per line.
 */
static void
test_callbacks(void)
{
	static const char manifest[] = "a type=dir\nb type=file\n";
	const parse_result_t *result;
	parse_setup_t setup;

	setup_defaults(&setup);
	setup.fail_entry = 1;
	result = check_refusal(manifest, strlen(manifest), &setup, FSIMG_EIO,
	    2);
	CHECK(strcmp(result->error.path, "/a/b") == 0);
	CHECK(strstr(result->error.message, "test entry failure") != NULL);
	CHECK(result->count == 1);

	setup.fail_without_record = true;
	result = check_refusal(manifest, strlen(manifest), &setup,
	    FSIMG_EIO, 2);
	CHECK(strstr(result->error.message, "without an error record") !=
	    NULL);
	CHECK(strcmp(result->error.path, "/a/b") == 0);

	setup_defaults(&setup);
	setup.fail_read = true;
	result = check_refusal(manifest, strlen(manifest), &setup, FSIMG_EIO,
	    1);
	CHECK(result->error.os_error == 5);

	setup_defaults(&setup);
	setup.cancel = true;
	result = check_refusal(manifest, strlen(manifest), &setup,
	    FSIMG_ECANCELLED, 1);
	CHECK(result->count == 0);
}

static void
write_seed(const char *directory, const char *name, const char *manifest)
{
	char path[1024];
	FILE *file;

	snprintf(path, sizeof(path), "%s/%s", directory, name);
	file = fopen(path, "wb");
	CHECK(file != NULL);
	CHECK(fwrite(manifest, 1, strlen(manifest), file) == strlen(manifest));
	CHECK(fclose(file) == 0);
}

/* The fuzz corpus: the host writers' manifests and generated content. */
static void
write_seeds(const char *directory)
{
	write_seed(directory, "mtree-c.seed", mtree_c_manifest);
	write_seed(directory, "bsdtar.seed", bsdtar_manifest);
	write_seed(directory, "bsdtar-set.seed", bsdtar_set_manifest);
	write_seed(directory, "generated.seed", generated_manifest);
}

int
main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--seed") == 0) {
		write_seeds(argv[2]);
		return (0);
	}
	CHECK(argc == 1);
	CHECK(expect_accepted(generated_manifest)->count == 5);
	test_host_writers();
	test_line_forms();
	test_continuations_and_comments();
	test_escapes();
	test_keyword_classes();
	test_numeric_values();
	test_defaults();
	test_content_keywords();
	test_bounds();
	test_malformed();
	test_callbacks();
	return (0);
}
