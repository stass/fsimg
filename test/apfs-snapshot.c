/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fsimg.h"
#include "apfs-gen.h"
#include "test.h"

#define BLOCK 4096u
#define CONTENT_SIZE (3 * BLOCK + 17)

/* Independent offsets in the object, APSB, object map and variable node. */
enum {
	OBJECT_XID = 16,
	APSB_MAGIC = 32,
	APSB_INCOMPAT = 56,
	APSB_ALLOC_BLOCKS = 88,
	APSB_OMAP = 128,
	APSB_EXTENTREF = 144,
	APSB_SNAPSHOTS_TREE = 152,
	APSB_FILES = 184,
	APSB_DIRECTORIES = 192,
	APSB_SYMLINKS = 200,
	APSB_OTHER_OBJECTS = 208,
	APSB_SNAPSHOTS = 216,
	APSB_UUID = 240,
	APSB_UUID_BYTES = 16,
	APSB_NAME = 704,
	APSB_NAME_BYTES = 256,
	OMAP_SNAPSHOT_TYPE = 44,
	OMAP_SNAPSHOT_TREE = 56,
	NODE_LEVEL = 34,
	NODE_RECORD_COUNT = 36,
	NODE_TABLE_OFFSET = 40,
	TOC_VALUE_OFFSET = 4,
	SNAP_META_SUPERBLOCK = 8
};

typedef struct fixture {
	apfs_gen_volume_t *volume;
	unsigned char *bytes;
	size_t size;
} fixture_t;

typedef struct session {
	fsimg_ctx_t *ctx;
	fsimg_io_t *io;
	fsimg_reader_t *reader;
} session_t;

typedef struct refusal_case {
	const char *name;
	apfs_gen_snapshot_t snapshot;
	fsimg_status_t status;
	const char *message;
} refusal_case_t;

static unsigned failures;
static const char *scenario;

#define EXPECT(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "FAIL %s line %d: %s\n", scenario, __LINE__, \
		    #condition); \
		failures++; \
	} \
} while (0)

static uint64_t
integer(const unsigned char *raw, unsigned width)
{
	uint64_t value = 0;

	for (unsigned i = 0; i < width; i++)
		value |= (uint64_t)raw[i] << (8 * i);
	return (value);
}

static void
put_integer(unsigned char *raw, uint64_t value, unsigned width)
{
	for (unsigned i = 0; i < width; i++)
		raw[i] = (unsigned char)(value >> (8 * i));
}

static void
content(unsigned char *bytes, unsigned version)
{
	char marker[] = "mod 0";

	marker[4] = (char)('0' + version);
	for (size_t i = 0; i < CONTENT_SIZE; i++)
		bytes[i] = (unsigned char)marker[i % 5];
}

static void
add_file(apfs_gen_volume_t *volume, uint64_t oid, const char *name)
{
	CHECK(apfs_gen_volume_file(volume, oid, 50, name,
	    (const unsigned char *)name, strlen(name)));
}

static fixture_t
history(const apfs_gen_snapshot_t *first)
{
	fixture_t f = { .volume = apfs_gen_volume_new() };
	unsigned char bytes[CONTENT_SIZE];
	apfs_gen_snapshot_t second = { .name = "a-second" };

	CHECK(f.volume != NULL);
	CHECK(apfs_gen_volume_layout(f.volume, 4, 1));
	CHECK(apfs_gen_volume_directory(f.volume, 50, 2, "d"));
	content(bytes, 0);
	CHECK(apfs_gen_volume_file(f.volume, 100, 50, "mod", bytes, sizeof(bytes)));
	add_file(f.volume, 101, "del");
	add_file(f.volume, 102, "keep");
	CHECK(apfs_gen_volume_snapshot(f.volume, first));
	content(bytes, 1);
	CHECK(apfs_gen_volume_rewrite(f.volume, 100, bytes, sizeof(bytes)));
	CHECK(apfs_gen_volume_remove(f.volume, 101));
	add_file(f.volume, 103, "added");
	CHECK(apfs_gen_volume_snapshot(f.volume, &second));
	content(bytes, 2);
	CHECK(apfs_gen_volume_rewrite(f.volume, 100, bytes, sizeof(bytes)));
	add_file(f.volume, 104, "live");
	CHECK(apfs_gen_volume_build(f.volume, &f.bytes, &f.size));
	return (f);
}

static void
release_fixture(fixture_t *f)
{
	free(f->bytes);
	apfs_gen_volume_free(f->volume);
}

static fsimg_status_t
memory_size(void *argument, uint64_t *size, fsimg_error_t *error)
{
	(void)error;
	*size = ((fixture_t *)argument)->size;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *count, fsimg_error_t *error)
{
	fixture_t *f = argument;

	(void)error;
	CHECK(offset <= f->size);
	if (length > f->size - offset)
		length = f->size - offset;
	bcopy(f->bytes + offset, buffer, length);
	*count = length;
	return (FSIMG_OK);
}

static fsimg_status_t
open_view(fixture_t *f, const char *name, session_t *s, fsimg_error_t *error)
{
	fsimg_read_options_t options;
	fsimg_io_ops_t ops;

	*s = (session_t){0};
	CHECK(fsimg_ctx_new(NULL, &s->ctx, NULL) == FSIMG_OK);
	fsimg_io_ops_init(&ops);
	ops.get_size = memory_size;
	ops.read_at = memory_read;
	CHECK(fsimg_io_from_callbacks(s->ctx, &ops, f, &s->io, NULL) == FSIMG_OK);
	fsimg_read_options_init(&options);
	options.snapshot = name;
	bzero(error, sizeof(*error));
	return (fsimg_reader_open(s->ctx, s->io, &options, &s->reader, error));
}

static void
close_view(session_t *s)
{
	fsimg_reader_free(s->reader);
	fsimg_io_free(s->io);
	fsimg_ctx_free(s->ctx);
}

static bool
require_view(fixture_t *f, const char *name, session_t *s)
{
	fsimg_error_t error;
	fsimg_status_t status = open_view(f, name, s, &error);

	if (status == FSIMG_OK)
		return (true);
	fprintf(stderr, "FAIL %s: open %s status=%d: %s\n", scenario,
	    name == NULL ? "live" : name, status, error.message);
	failures++;
	close_view(s);
	return (false);
}

static int
name_compare(const void *left, const void *right)
{
	return (strcmp(left, right));
}

static void
expect_listing(fixture_t *f, const char *name, const char *expected,
    unsigned version)
{
	session_t s;
	const fsimg_entry_t *entry;
	fsimg_error_t error;
	fsimg_status_t status;
	char names[16][64], listing[sizeof(names) + 1] = "";
	unsigned char wanted[CONTENT_SIZE], actual[CONTENT_SIZE];
	size_t total = 0, count;

	if (!require_view(f, name, &s))
		return;
	content(wanted, version);
	while ((status = fsimg_reader_next(s.reader, &entry, &error)) == FSIMG_OK) {
		CHECK(total < 16 && strlen(entry->path) < sizeof(names[0]));
		strcpy(names[total++], entry->path);
		if (strcmp(entry->path, "/d") == 0) {
			EXPECT(entry->kind == FSIMG_ENTRY_DIRECTORY);
			continue;
		}
		EXPECT(entry->kind == FSIMG_ENTRY_FILE);
		CHECK(entry->size <= sizeof(actual));
		EXPECT(fsimg_reader_read_at(s.reader, 0, actual,
		    (size_t)entry->size, &count, &error) == FSIMG_OK);
		EXPECT(count == entry->size);
		if (strcmp(entry->path, "/d/mod") == 0) {
			EXPECT(entry->size == sizeof(wanted));
			EXPECT(count == sizeof(wanted) &&
			    memcmp(actual, wanted, sizeof(wanted)) == 0);
		} else {
			const char *basename = strrchr(entry->path, '/') + 1;

			EXPECT(count == strlen(basename) &&
			    memcmp(actual, basename, count) == 0);
		}
	}
	EXPECT(status == FSIMG_DONE);
	qsort(names, total, sizeof(names[0]), name_compare);
	for (size_t i = 0; i < total; i++) {
		strcat(listing, names[i]);
		strcat(listing, "\n");
	}
	EXPECT(strcmp(listing, expected) == 0);
	close_view(&s);
}

static void
test_snapshot_views_retain_rewritten_and_removed_payloads(fixture_t *f)
{
	scenario = "first snapshot retains removed file and original payload";
	expect_listing(f, "z-first", "/d\n/d/del\n/d/keep\n/d/mod\n", 0);
	scenario = "second snapshot retains intermediate payload";
	expect_listing(f, "a-second", "/d\n/d/added\n/d/keep\n/d/mod\n", 1);
	scenario = "live view reads latest entries and payload";
	expect_listing(f, NULL, "/d\n/d/added\n/d/keep\n/d/live\n/d/mod\n", 2);
}

static void
expect_refusal(fixture_t *f, const char *name, fsimg_status_t expected,
    const char *message)
{
	session_t s;
	fsimg_error_t error;
	fsimg_status_t status = open_view(f, name, &s, &error);

	if (status != expected || (message != NULL &&
	    strstr(error.message, message) == NULL)) {
		fprintf(stderr, "FAIL %s: %s status=%d expected=%d message=%s "
		    "expected substring=%s\n", scenario, name, status, expected,
		    error.message, message == NULL ? "(any)" : message);
		failures++;
	}
	EXPECT(status == FSIMG_OK || error.status == status);
	close_view(&s);
}

static void
test_snapshot_names_match_exact_bytes_and_prefixes(void)
{
	static const char *names[] = { "e9-a", "e9-ab", "Snapshot 1",
	    "Snapshot 10", "Snapshot 11" };
	static const char *missing[] = { "e9-", "e9-abc", "E9-A", "e9-a " };
	fixture_t f = { .volume = apfs_gen_volume_new() };
	char expected[512] = "/d\n", paths[5][64];

	CHECK(f.volume != NULL);
	CHECK(apfs_gen_volume_directory(f.volume, 50, 2, "d"));
	for (size_t i = 0; i < 5; i++) {
		apfs_gen_snapshot_t snapshot = { .name = names[i] };
		char name[8];

		CHECK(snprintf(name, sizeof(name), "f%zu", i) > 0);
		add_file(f.volume, 100 + i, name);
		CHECK(apfs_gen_volume_snapshot(f.volume, &snapshot));
		CHECK(snprintf(paths[i], sizeof(paths[i]), "/d/%s\n", name) > 0);
	}
	CHECK(apfs_gen_volume_build(f.volume, &f.bytes, &f.size));
	for (size_t i = 0; i < 5; i++) {
		scenario = names[i];
		strcat(expected, paths[i]);
		expect_listing(&f, names[i], expected, 0);
	}
	for (size_t i = 0; i < 4; i++) {
		scenario = "snapshot names require an exact byte match";
		expect_refusal(&f, missing[i], FSIMG_EINVAL,
		    "has no snapshot of that name");
	}
	release_fixture(&f);
	f = (fixture_t){ .volume = apfs_gen_volume_new() };
	CHECK(f.volume != NULL);
	CHECK(apfs_gen_volume_build(f.volume, &f.bytes, &f.size));
	scenario = "volume without snapshots has no matching name";
	expect_refusal(&f, "missing", FSIMG_EINVAL, "has no snapshot of that name");
	release_fixture(&f);
}

static uint64_t
detail(const fsimg_volume_info_t *info, const char *key)
{
	for (uint32_t i = 0; i < info->detail_count; i++) {
		if (strcmp(info->details[i].name, key) == 0)
			return (info->details[i].value);
	}
	fprintf(stderr, "FAIL %s: missing detail %s\n", scenario, key);
	failures++;
	return (UINT64_MAX);
}

static void
test_snapshot_information_comes_from_the_copy(fixture_t *f)
{
	session_t live, snapshot;
	fsimg_volume_info_t current, saved;
	apfs_gen_snapshot_info_t location;
	static const char *names[] = { "z-first", "a-second" };
	static const char *keys[] = { "files", "directories", "symlinks",
	    "other_objects", "volume_blocks", "snapshots" };
	static const unsigned offsets[] = { APSB_FILES, APSB_DIRECTORIES,
	    APSB_SYMLINKS, APSB_OTHER_OBJECTS, APSB_ALLOC_BLOCKS, APSB_SNAPSHOTS };

	scenario = "snapshot information uses copy counts and snapshot read XID";
	if (!require_view(f, NULL, &live))
		return;
	CHECK(fsimg_reader_get_info(live.reader, &current, NULL) == FSIMG_OK);
	EXPECT(detail(&current, "read_xid") == detail(&current, "checkpoint_xid"));
	EXPECT(detail(&current, "snapshots") == 2);
	for (size_t i = 0; i < 2; i++) {
		CHECK(apfs_gen_volume_snapshot_info(f->volume, i, &location));
		if (!require_view(f, names[i], &snapshot))
			continue;
		CHECK(fsimg_reader_get_info(snapshot.reader, &saved, NULL) == FSIMG_OK);
		EXPECT(detail(&saved, "read_xid") == location.xid);
		EXPECT(detail(&saved, "checkpoint_xid") == detail(&current, "checkpoint_xid"));
		EXPECT(strcmp(saved.label, current.label) == 0);
		EXPECT(memcmp(saved.uuid, current.uuid, sizeof(saved.uuid)) == 0);
		EXPECT(detail(&saved, "snapshots") == i);
		for (size_t j = 0; j < sizeof(keys) / sizeof(keys[0]); j++)
			EXPECT(detail(&saved, keys[j]) == integer(f->bytes +
			    location.superblock * BLOCK + offsets[j], 8));
		close_view(&snapshot);
	}
	close_view(&live);
}

static void
patch(fixture_t *f, uint64_t block, unsigned offset, uint64_t value,
    unsigned width)
{
	unsigned char *raw = f->bytes + block * BLOCK;

	CHECK(block < f->size / BLOCK && offset + width <= BLOCK);
	put_integer(raw + offset, value, width);
	CHECK(apfs_gen_seal(raw, BLOCK));
}

static void
test_exact_snapshot_name_with_wrong_stored_length_is_corrupt(void)
{
	apfs_gen_snapshot_t first = { .name = "z-first",
	    .corrupt_name_length = true };
	fixture_t f = history(&first);

	scenario = "exact name bytes with a damaged stored length are corrupt";
	expect_refusal(&f, "z-first", FSIMG_ECORRUPT,
	    "has a malformed name record");
	release_fixture(&f);
}

static void
test_snapshot_record_damage_and_flags_are_refused(void)
{
	static const refusal_case_t cases[] = {
		{ .name = "name record cannot select XID zero",
		.snapshot = { .name = "z-first", .override_name_xid = true,
		.name_xid = 0 }, .status = FSIMG_ECORRUPT,
		.message = "names a snapshot newer than the checkpoint" },
		{ .name = "name record cannot select XID above checkpoint",
		.snapshot = { .name = "z-first", .override_name_xid = true,
		.name_xid = UINT64_MAX }, .status = FSIMG_ECORRUPT,
		.message = "names a snapshot newer than the checkpoint" },
		{ .name = "metadata must carry the selected name's exact bytes",
		.snapshot = { .name = "z-first", .metadata_name = "x-first" },
		.status = FSIMG_ECORRUPT,
		.message = "metadata names another snapshot" },
		{ .name = "pending-dataless metadata flag is refused",
		.snapshot = { .name = "z-first", .meta_flags = 1 },
		.status = FSIMG_EUNSUPPORTED,
		.message = "snapshot has unsupported flags" },
		{ .name = "merge-in-progress metadata flag is refused",
		.snapshot = { .name = "z-first", .meta_flags = 2 },
		.status = FSIMG_EUNSUPPORTED,
		.message = "snapshot has unsupported flags" },
		{ .name = "object-map snapshot flags are refused",
		.snapshot = { .name = "z-first", .omap_flags = 1 },
		.status = FSIMG_EUNSUPPORTED,
		.message = "volume object map snapshot record has unsupported flags" },
		{ .name = "snapshot requires a metadata record",
		.snapshot = { .name = "z-first", .omit_metadata = true },
		.status = FSIMG_ECORRUPT,
		.message = "snapshot has no metadata record" },
		{ .name = "snapshot requires an object-map snapshot record",
		.snapshot = { .name = "z-first", .omit_omap_record = true },
		.status = FSIMG_ECORRUPT,
		.message = "volume object map has no record of the snapshot" }
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		fixture_t f = history(&cases[i].snapshot);

		scenario = cases[i].name;
		expect_refusal(&f, "z-first", cases[i].status, cases[i].message);
		release_fixture(&f);
	}
}

static void
test_snapshot_superblock_damage_is_refused(fixture_t *f)
{
	apfs_gen_snapshot_info_t location;
	uint64_t live, metadata, omap;
	unsigned char original[BLOCK];
	static const unsigned own_trees[] = { APSB_OMAP, APSB_EXTENTREF, APSB_SNAPSHOTS_TREE };

	CHECK(apfs_gen_volume_snapshot_info(f->volume, 0, &location));
	CHECK(apfs_gen_volume_snapshot_roots(f->volume, &live, &metadata, &omap));
	bcopy(f->bytes + location.superblock * BLOCK, original, BLOCK);
	for (size_t i = 0; i < 3; i++) {
		scenario = "snapshot superblock cannot name trees of its own";
		patch(f, location.superblock, own_trees[i], 100, 8);
		expect_refusal(f, "z-first", FSIMG_EUNSUPPORTED,
		    "snapshot superblock names trees of its own");
		bcopy(original, f->bytes + location.superblock * BLOCK, BLOCK);
	}
	scenario = "snapshot incompatible features must agree with live";
	patch(f, location.superblock, APSB_INCOMPAT, 1, 8);
	expect_refusal(f, "z-first", FSIMG_EUNSUPPORTED,
	    "snapshot superblock has other incompatible features");
	bcopy(original, f->bytes + location.superblock * BLOCK, BLOCK);
	scenario = "snapshot superblock XID must equal selected snapshot XID";
	patch(f, location.superblock, OBJECT_XID, location.xid + 1, 8);
	expect_refusal(f, "z-first", FSIMG_ECORRUPT,
	    "snapshot superblock has the wrong XID");
	bcopy(original, f->bytes + location.superblock * BLOCK, BLOCK);
	scenario = "snapshot superblock checksum is required";
	f->bytes[location.superblock * BLOCK] ^= 1;
	expect_refusal(f, "z-first", FSIMG_ECORRUPT, "bad checksum");
	bcopy(original, f->bytes + location.superblock * BLOCK, BLOCK);

	scenario = "dataless volume permits live view but refuses snapshot view";
	patch(f, live, APSB_INCOMPAT, 2, 8);
	session_t s;

	if (require_view(f, NULL, &s))
		close_view(&s);
	expect_refusal(f, "z-first", FSIMG_EUNSUPPORTED, "dataless");
	patch(f, live, APSB_INCOMPAT, 0, 8);

	/* First metadata record is the first creation XID. A variable root's
	 * value offset is measured backward from the root footer. */
	unsigned char *raw = f->bytes + metadata * BLOCK;
	unsigned toc = GEN_APFS_NODE_HEADER +
	    (unsigned)integer(raw + NODE_TABLE_OFFSET, 2);
	unsigned value_offset = (unsigned)integer(raw + toc + TOC_VALUE_OFFSET, 2);
	unsigned value = BLOCK - GEN_APFS_FOOTER - value_offset;
	uint64_t address = integer(raw + value + SNAP_META_SUPERBLOCK, 8);

	scenario = "snapshot metadata must name a volume superblock";
	patch(f, metadata, value + SNAP_META_SUPERBLOCK, omap, 8);
	expect_refusal(f, "z-first", FSIMG_ECORRUPT, NULL);
	patch(f, metadata, value + SNAP_META_SUPERBLOCK, address, 8);
}

static void
test_second_snapshot_rejects_older_superblock_xid(fixture_t *f)
{
	apfs_gen_snapshot_info_t first, second;

	scenario = "second snapshot rejects a nonzero older copy XID";
	CHECK(apfs_gen_volume_snapshot_info(f->volume, 0, &first));
	CHECK(apfs_gen_volume_snapshot_info(f->volume, 1, &second));
	CHECK(first.xid > 0 && first.xid < second.xid);
	patch(f, second.superblock, OBJECT_XID, first.xid, 8);
	expect_refusal(f, "a-second", FSIMG_ECORRUPT,
	    "snapshot superblock has the wrong XID");
	patch(f, second.superblock, OBJECT_XID, second.xid, 8);
}

static void
test_snapshot_identity_comes_from_live_superblock(fixture_t *f)
{
	apfs_gen_snapshot_info_t location;
	fsimg_volume_info_t current, saved;
	session_t live, snapshot;
	unsigned char original[BLOCK], *copy;

	scenario = "snapshot identity ignores the copy UUID and volume name";
	CHECK(apfs_gen_volume_snapshot_info(f->volume, 0, &location));
	copy = f->bytes + location.superblock * BLOCK;
	bcopy(copy, original, BLOCK);
	if (!require_view(f, NULL, &live))
		return;
	CHECK(fsimg_reader_get_info(live.reader, &current, NULL) == FSIMG_OK);
	close_view(&live);
	for (unsigned unterminated = 0; unterminated < 2; unterminated++) {
		scenario = unterminated ? "copy volume name need not terminate" :
		    "copy volume identity may differ from live";
		for (unsigned i = 0; i < APSB_UUID_BYTES; i++)
			copy[APSB_UUID + i] = current.uuid[i] ^ 0xff;
		memset(copy + APSB_NAME, 'x', APSB_NAME_BYTES);
		if (!unterminated)
			copy[APSB_NAME + APSB_NAME_BYTES - 1] = '\0';
		CHECK(apfs_gen_seal(copy, BLOCK));
		if (require_view(f, "z-first", &snapshot)) {
			CHECK(fsimg_reader_get_info(snapshot.reader, &saved, NULL) == FSIMG_OK);
			EXPECT(strcmp(saved.label, current.label) == 0);
			EXPECT(bcmp(saved.uuid, current.uuid, sizeof(saved.uuid)) == 0);
			close_view(&snapshot);
		}
	}
	bcopy(original, copy, BLOCK);
}

static void
test_snapshot_magic_and_object_map_damage_are_refused(fixture_t *f)
{
	apfs_gen_snapshot_info_t location;
	uint64_t live, metadata, snapshots, map;
	unsigned char original[BLOCK];

	CHECK(apfs_gen_volume_snapshot_info(f->volume, 0, &location));
	CHECK(apfs_gen_volume_snapshot_roots(f->volume, &live, &metadata, &snapshots));
	map = integer(f->bytes + live * BLOCK + APSB_OMAP, 8);
	bcopy(f->bytes + location.superblock * BLOCK, original, BLOCK);
	scenario = "snapshot copy requires APSB magic even with a valid checksum";
	patch(f, location.superblock, APSB_MAGIC, 0, 4);
	expect_refusal(f, "z-first", FSIMG_ECORRUPT, "has no APSB magic");
	bcopy(original, f->bytes + location.superblock * BLOCK, BLOCK);

	bcopy(f->bytes + map * BLOCK, original, BLOCK);
	scenario = "volume object map requires a snapshot tree address";
	patch(f, map, OMAP_SNAPSHOT_TREE, 0, 8);
	expect_refusal(f, "z-first", FSIMG_ECORRUPT,
	    "has no record of the snapshot");
	bcopy(original, f->bytes + map * BLOCK, BLOCK);
	scenario = "volume object map requires a physical snapshot tree root";
	patch(f, map, OMAP_SNAPSHOT_TYPE, 0x40000003, 4);
	expect_refusal(f, "z-first", FSIMG_ECORRUPT, "has an invalid snapshot tree");
	bcopy(original, f->bytes + map * BLOCK, BLOCK);
}

/* Cumulative files distinguish every view while names sort in reverse XID order.
 * Two records per leaf put both lookup trees across several leaf boundaries.
 */
static fixture_t
split_history(const apfs_gen_snapshot_t *last)
{
	fixture_t f = { .volume = apfs_gen_volume_new() };
	static const char *names[] = { "s5", "s4", "s3", "s2", "s1", "s0" };

	CHECK(f.volume != NULL);
	CHECK(apfs_gen_volume_snapshot_layout(f.volume, 2));
	CHECK(apfs_gen_volume_directory(f.volume, 50, 2, "d"));
	for (size_t i = 0; i < 6; i++) {
		apfs_gen_snapshot_t snapshot = { .name = names[i] };
		char filename[8];

		CHECK(snprintf(filename, sizeof(filename), "f%zu", i) > 0);
		add_file(f.volume, 100 + i, filename);
		CHECK(apfs_gen_volume_snapshot(f.volume, i == 5 ? last : &snapshot));
	}
	add_file(f.volume, 106, "live");
	CHECK(apfs_gen_volume_build(f.volume, &f.bytes, &f.size));
	return (f);
}

static void
test_snapshot_trees_cross_leaf_boundaries(void)
{
	static const char *names[] = { "s5", "s4", "s3", "s2", "s1", "s0" };
	apfs_gen_snapshot_t last = { .name = "s0" };
	fixture_t f = split_history(&last);
	uint64_t live, metadata, omap;
	char expected[256] = "/d\n";

	scenario = "snapshot lookup trees have index roots over several leaves";
	CHECK(apfs_gen_volume_snapshot_roots(f.volume, &live, &metadata, &omap));
	EXPECT(integer(f.bytes + metadata * BLOCK + NODE_LEVEL, 2) == 1);
	EXPECT(integer(f.bytes + omap * BLOCK + NODE_LEVEL, 2) == 1);
	EXPECT(integer(f.bytes + metadata * BLOCK + NODE_RECORD_COUNT, 4) == 6);
	EXPECT(integer(f.bytes + omap * BLOCK + NODE_RECORD_COUNT, 4) == 3);
	for (size_t i = 0; i < 6; i++) {
		char path[16];

		scenario = names[i];
		CHECK(snprintf(path, sizeof(path), "/d/f%zu\n", i) > 0);
		strcat(expected, path);
		expect_listing(&f, names[i], expected, 0);
	}
	release_fixture(&f);
}

static void
test_later_snapshot_requires_exact_records_after_le_seek(void)
{
	static const refusal_case_t cases[] = {
		{ .name = "LE metadata predecessor is not the selected snapshot",
		.snapshot = { .name = "s0", .omit_metadata = true },
		.status = FSIMG_ECORRUPT,
		.message = "snapshot has no metadata record" },
		{ .name = "LE object-map predecessor is not the selected snapshot",
		.snapshot = { .name = "s0", .omit_omap_record = true },
		.status = FSIMG_ECORRUPT,
		.message = "volume object map has no record of the snapshot" }
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		fixture_t f = split_history(&cases[i].snapshot);

		scenario = cases[i].name;
		/* Confirm that the immediately preceding snapshot remains intact. */
		expect_listing(&f, "s1", "/d\n/d/f0\n/d/f1\n/d/f2\n/d/f3\n/d/f4\n", 0);
		expect_refusal(&f, "s0", cases[i].status, cases[i].message);
		release_fixture(&f);
	}
}

static void
test_snapshot_names_accept_255_bytes_and_match_utf8_exactly(void)
{
	char longest[256];
	static const char composed[] = "caf\xc3\xa9";
	static const char decomposed[] = "cafe\xcc\x81";
	apfs_gen_snapshot_t snapshot = { .name = longest };
	fixture_t f = { .volume = apfs_gen_volume_new() };

	CHECK(f.volume != NULL);
	CHECK(apfs_gen_volume_directory(f.volume, 50, 2, "d"));
	memset(longest, 'a', sizeof(longest) - 1);
	longest[sizeof(longest) - 1] = '\0';
	add_file(f.volume, 100, "first");
	CHECK(apfs_gen_volume_snapshot(f.volume, &snapshot));
	add_file(f.volume, 101, "second");
	snapshot.name = composed;
	CHECK(apfs_gen_volume_snapshot(f.volume, &snapshot));
	add_file(f.volume, 102, "live");
	CHECK(apfs_gen_volume_build(f.volume, &f.bytes, &f.size));
	scenario = "255-byte snapshot name opens its frozen view";
	expect_listing(&f, longest, "/d\n/d/first\n", 0);
	scenario = "UTF-8 snapshot name matches its exact bytes";
	expect_listing(&f, composed, "/d\n/d/first\n/d/second\n", 0);
	scenario = "NFD spelling does not match the composed snapshot name";
	expect_refusal(&f, decomposed, FSIMG_EINVAL, "has no snapshot of that name");
	release_fixture(&f);
}

static void
expect_same_report(const fsimg_verify_report_t *a,
    const fsimg_verify_report_t *b)
{
	EXPECT(a->scope == b->scope);
	EXPECT(a->complete == b->complete);
	EXPECT(a->files == b->files);
	EXPECT(a->directories == b->directories);
	EXPECT(a->logical_bytes == b->logical_bytes);
	EXPECT(a->payload_bytes_read == b->payload_bytes_read);
	EXPECT(a->allocated_clusters == b->allocated_clusters);
	EXPECT(a->free_clusters == b->free_clusters);
	EXPECT(a->bad_clusters == b->bad_clusters);
	EXPECT(a->omitted_entries == b->omitted_entries);
}

static void
test_verification_uses_live_view_even_when_snapshot_selected(fixture_t *f)
{
	static const unsigned flags[] = { 0, FSIMG_VERIFY_PAYLOAD };

	for (size_t i = 0; i < 2; i++) {
		session_t live, snapshot;
		fsimg_verify_report_t a = {0}, b = {0};
		fsimg_volume_info_t current, saved;
		fsimg_error_t ea, eb;
		fsimg_status_t sa, sb;

		scenario = i == 0 ? "structural verification ignores snapshot selection" :
		    "payload verification ignores snapshot selection";
		if (!require_view(f, NULL, &live))
			continue;
		CHECK(fsimg_reader_get_info(live.reader, &current, NULL) == FSIMG_OK);
		bzero(&ea, sizeof(ea));
		sa = fsimg_verify(live.reader, flags[i], &a, &ea);
		EXPECT(sa == FSIMG_EUNSUPPORTED);
		EXPECT(strstr(ea.message,
		    "has snapshots, whose allocation verification does not judge") != NULL);
		EXPECT(a.files == detail(&current, "files"));
		if (require_view(f, "z-first", &snapshot)) {
			CHECK(fsimg_reader_get_info(snapshot.reader, &saved, NULL) == FSIMG_OK);
			EXPECT(detail(&saved, "files") != detail(&current, "files"));
			bzero(&eb, sizeof(eb));
			sb = fsimg_verify(snapshot.reader, flags[i], &b, &eb);
			EXPECT(sb == FSIMG_EUNSUPPORTED);
			EXPECT(strstr(eb.message,
			    "has snapshots, whose allocation verification does not judge") != NULL);
			EXPECT(b.files == detail(&current, "files"));
			EXPECT(sa == sb);
			EXPECT(strcmp(ea.message, eb.message) == 0);
			expect_same_report(&a, &b);
			close_view(&snapshot);
		}
		close_view(&live);
	}
}

int
main(void)
{
	apfs_gen_snapshot_t first = { .name = "z-first" };
	fixture_t f = history(&first);

	test_snapshot_views_retain_rewritten_and_removed_payloads(&f);
	test_snapshot_names_match_exact_bytes_and_prefixes();
	test_snapshot_information_comes_from_the_copy(&f);
	test_exact_snapshot_name_with_wrong_stored_length_is_corrupt();
	test_snapshot_record_damage_and_flags_are_refused();
	test_snapshot_superblock_damage_is_refused(&f);
	test_second_snapshot_rejects_older_superblock_xid(&f);
	test_snapshot_identity_comes_from_live_superblock(&f);
	test_snapshot_magic_and_object_map_damage_are_refused(&f);
	test_snapshot_trees_cross_leaf_boundaries();
	test_later_snapshot_requires_exact_records_after_le_seek();
	test_snapshot_names_accept_255_bytes_and_match_utf8_exactly();
	test_verification_uses_live_view_even_when_snapshot_selected(&f);
	release_fixture(&f);
	printf("APFS snapshot failures=%u\n", failures);
	return (failures == 0 ? 0 : 1);
}
