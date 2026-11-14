/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "reader.h"
#include "test.h"
#include "write-apfs.h"

#define BLOCK 4096u
#define FIRST_BLOCK 100u
#define TEST_XID 27u
#define MEMORY_LIMIT (256u * 1024u)

/* Records are regenerated for each pass; only the sink retains nodes. */
typedef struct stream {
	uint64_t	count;
	size_t		key_size;
	size_t		val_size;
	unsigned	variation;
	unsigned	last_extra;
	bool		ghosts;
} stream_t;

typedef struct capture {
	unsigned char	*bytes;
	uint64_t	capacity;
	uint64_t	count;
} capture_t;

typedef struct packed_tree {
	capture_t	capture;
	apfs_pack_result_t result;
} packed_tree_t;

static const apfs_pack_profile_t physical_aligned = {
	.role = "test aligned physical tree",
	.storage = APFS_OBJ_PHYSICAL,
	.subtype = APFS_SUBTYPE_FSTREE,
	.tree_flags = APFS_BTREE_SEQUENTIAL_INSERT | APFS_BTREE_PHYSICAL
};

static const apfs_pack_profile_t physical_unaligned = {
	.role = "test unaligned physical tree",
	.storage = APFS_OBJ_PHYSICAL,
	.subtype = APFS_SUBTYPE_FSTREE,
	.tree_flags = APFS_BTREE_SEQUENTIAL_INSERT | APFS_BTREE_PHYSICAL |
	    APFS_BTREE_KV_NONALIGNED
};

static const apfs_pack_profile_t physical_ghosts = {
	.role = "test physical ghost tree",
	.storage = APFS_OBJ_PHYSICAL,
	.subtype = APFS_SUBTYPE_FREE_QUEUE,
	.tree_flags = APFS_BTREE_SEQUENTIAL_INSERT | APFS_BTREE_PHYSICAL |
	    APFS_BTREE_ALLOW_GHOSTS,
	.fixed_key = APFS_FQ_KEY_BYTES,
	.fixed_val = APFS_FQ_VAL_BYTES,
	.toc_min = 144
};

/* Leaf totals also supply an independent oracle for the root footer. */
typedef struct leaf_totals {
	uint64_t	count;
	size_t		longest_key;
	size_t		longest_val;
} leaf_totals_t;

/* Count successful writes as well as the write that returns the error. */
typedef struct failing_sink {
	unsigned	calls;
	unsigned	fail_on;
} failing_sink_t;

static const char *scenario;

static void	check_status(fsimg_status_t, fsimg_status_t,
		    const fsimg_error_t *);
static fsimg_ctx_t *context_new(void);
static unsigned char *captured_node(const capture_t *, uint64_t);
static uint64_t	mapped_oid(void *, uint64_t);
static fsimg_status_t capture_node(void *, uint64_t, uint64_t,
		    const unsigned char *, fsimg_error_t *);
static void	capture_init(capture_t *, uint64_t);
static void	make_record(const apfs_pack_profile_t *, const stream_t *,
		    uint64_t, unsigned char *, unsigned char *, apfs_record_t *);
static apfs_pack_result_t pack_stream(fsimg_ctx_t *,
		    const apfs_pack_profile_t *, const stream_t *,
		    const apfs_pack_sink_t *);
static void	check_results(const apfs_pack_result_t *,
		    const apfs_pack_result_t *);
static apfs_kv_size_t node_sizes(const apfs_pack_profile_t *,
		    const apfs_node_t *);
static void	check_record(const apfs_record_t *, const apfs_record_t *);
static void	check_nodes(const packed_tree_t *, const apfs_pack_profile_t *,
		    const stream_t *);
static int	compare_bytes(const unsigned char *, size_t,
		    const unsigned char *, size_t);
static int	compare_omap(const unsigned char *, size_t,
		    const unsigned char *, size_t);
static fsimg_status_t memory_read(void *, uint64_t, void *, size_t,
		    size_t *, fsimg_error_t *);
static fsimg_status_t memory_size(void *, uint64_t *, fsimg_error_t *);
static void	check_cursor(fsimg_ctx_t *, const packed_tree_t *,
		    const apfs_pack_profile_t *, const stream_t *);
static packed_tree_t check_stream(const apfs_pack_profile_t *,
		    const stream_t *);
static void	test_empty_trees_have_native_tables_and_footers(void);
static void	test_one_record_round_trips(void);
static void	test_exactly_full_leaf_then_one_byte_more_splits(void);
static void	test_fixed_leaf_overflow_leaves_one_record_and_two_children(void);
static void	test_variable_records_round_trip_at_three_heights(void);
static void	test_odd_lengths_use_alignment_only_when_required(void);
static void	test_free_queue_preserves_ghosts(void);
static void	test_large_stream_uses_bounded_memory(void);
static void	test_invalid_records_are_refused(void);
static fsimg_status_t fail_write(void *, uint64_t, uint64_t,
		    const unsigned char *, fsimg_error_t *);
static void	test_sink_failure_is_returned_unchanged(void);
static void	test_open_allocation_failures_release_memory(void);
static void	test_nonroot_nodes_keep_room_for_a_footer(void);

static void	check_node_header(const unsigned char *, const apfs_node_t *,
		    const apfs_pack_profile_t *, uint64_t, bool);
static void	check_index_entry(const packed_tree_t *,
		    const apfs_pack_profile_t *, const unsigned char *,
		    const apfs_node_t *, const apfs_entry_t *, uint64_t);
static void	check_leaf_entry(const unsigned char *, const apfs_entry_t *,
		    const apfs_pack_profile_t *, const stream_t *, leaf_totals_t *);
static void	check_unused_bytes(const unsigned char *, const apfs_node_t *,
		    const apfs_pack_profile_t *);
static void	check_root_footer(const packed_tree_t *, const apfs_node_t *,
		    const apfs_pack_profile_t *, const leaf_totals_t *);
static unsigned	omap_capacity(unsigned);
static void	test_finish_grows_a_level_when_the_top_overflows(void);
static void	test_physical_ghosts_round_trip_through_cursor(void);
static void	test_largest_admitted_key_packs_three_records(void);
static void	test_tree_taller_than_sixteen_levels_is_refused(void);
static void	test_malformed_profiles_are_refused_at_open(void);
static void	test_successful_finish_refuses_more_operations(void);
static void	test_admission_failure_refuses_more_operations(void);
static void	test_level_allocation_failure_after_child_emission_releases_memory(void);
static void	test_third_sink_write_failure_is_returned_unchanged(void);
static void	test_aligned_exactly_full_leaf_splits_after_one_more_byte(void);

static void
check_status(fsimg_status_t actual, fsimg_status_t wanted,
    const fsimg_error_t *error)
{
	if (actual == wanted)
		return;
	fprintf(stderr, "%s: expected status %d, got %d (%s)\n",
	    scenario, wanted, actual, error->message);
	exit(1);
}

static fsimg_ctx_t *
context_new(void)
{
	fsimg_options_t options;
	fsimg_ctx_t *ctx;

	fsimg_options_init(&options);
	/* The public context API requires a memory budget of at least 16 MiB. */
	options.memory_bytes = 16u * 1024u * 1024u;
	CHECK(fsimg_ctx_new(&options, &ctx, NULL) == FSIMG_OK);
	return (ctx);
}

static unsigned char *
captured_node(const capture_t *capture, uint64_t index)
{
	CHECK(index < capture->count);
	return (capture->bytes + (FIRST_BLOCK + index) * BLOCK);
}

static uint64_t
mapped_oid(void *argument FSIMG_UNUSED, uint64_t index)
{
	return (FIRST_BLOCK + index);
}

static fsimg_status_t
capture_node(void *argument, uint64_t index, uint64_t oid FSIMG_UNUSED,
    const unsigned char *node, fsimg_error_t *error FSIMG_UNUSED)
{
	capture_t *capture = argument;

	CHECK(index == capture->count);
	CHECK(index < capture->capacity);
	capture->count++;
	bcopy(node, captured_node(capture, index), BLOCK);
	return (FSIMG_OK);
}

static void
capture_init(capture_t *capture, uint64_t capacity)
{
	*capture = (capture_t){ .capacity = capacity };
	capture->bytes = calloc(FIRST_BLOCK + capacity, BLOCK);
	CHECK(capture->bytes != NULL);
}

/* Big-endian counters sort lexically; native object-map keys sort numerically. */
static void
make_record(const apfs_pack_profile_t *profile, const stream_t *stream,
    uint64_t index, unsigned char *key, unsigned char *value,
    apfs_record_t *record)
{
	size_t key_len, val_len;
	bool ghost;

	key_len = stream->key_size + index % (stream->variation + 1);
	val_len = stream->val_size + index % (stream->variation + 1);
	if (index + 1 == stream->count)
		val_len += stream->last_extra;
	CHECK(key_len >= 8 && key_len <= BLOCK && val_len <= BLOCK);
	memset(key, 0x6b, key_len);
	memset(value, (int)(index % 251 + 1), val_len);
	for (unsigned i = 0; i < 8; i++)
		key[i] = (unsigned char)(index >> (56 - i * 8));
	if (profile == &apfs_pack_container_omap) {
		fsimg_store_le64(key, index + 1);
		fsimg_store_le64(key + 8, TEST_XID);
	}
	ghost = stream->ghosts && index % 3 != 1;
	*record = (apfs_record_t){ .key = key, .key_len = key_len,
	    .val = ghost ? NULL : value, .val_len = ghost ? 0 : val_len,
	    .ghost = ghost };
}

static apfs_pack_result_t
pack_stream(fsimg_ctx_t *ctx, const apfs_pack_profile_t *profile,
    const stream_t *stream, const apfs_pack_sink_t *sink)
{
	unsigned char key[BLOCK], value[BLOCK];
	apfs_pack_result_t result;
	fsimg_stats_t before, after;
	fsimg_error_t error;
	apfs_record_t record;
	apfs_pack_t *pack;

	CHECK(fsimg_ctx_get_stats(ctx, &before, NULL) == FSIMG_OK);
	check_status(apfs_pack_open(ctx, profile, TEST_XID, sink, &pack,
	    &error), FSIMG_OK, &error);
	for (uint64_t i = 0; i < stream->count; i++) {
		make_record(profile, stream, i, key, value, &record);
		check_status(apfs_pack_add(pack, &record, &error), FSIMG_OK,
		    &error);
	}
	check_status(apfs_pack_finish(pack, &result, &error), FSIMG_OK, &error);
	apfs_pack_free(pack);
	CHECK(fsimg_ctx_get_stats(ctx, &after, NULL) == FSIMG_OK);
	CHECK(after.memory_bytes == before.memory_bytes);
	CHECK(after.scratch_bytes == before.scratch_bytes);
	return (result);
}

static void
check_results(const apfs_pack_result_t *plan, const apfs_pack_result_t *emit)
{
	CHECK(plan->node_count == emit->node_count);
	CHECK(plan->key_count == emit->key_count);
	CHECK(plan->levels == emit->levels);
	for (unsigned i = 0; i < APFS_BTREE_LEVELS_MAX; i++)
		CHECK(plan->level_nodes[i] == emit->level_nodes[i]);
}

static apfs_kv_size_t
node_sizes(const apfs_pack_profile_t *profile, const apfs_node_t *node)
{
	return ((apfs_kv_size_t){ .key = profile->fixed_key,
	    .val = node->level == 0 ? profile->fixed_val : APFS_CHILD_OID_BYTES });
}

static void
check_record(const apfs_record_t *actual, const apfs_record_t *expected)
{
	CHECK(actual->key_len == expected->key_len);
	CHECK(bcmp(actual->key, expected->key, actual->key_len) == 0);
	CHECK(actual->ghost == expected->ghost);
	CHECK(actual->val_len == expected->val_len);
	if (actual->ghost)
		CHECK(actual->val == NULL);
	else
		CHECK(bcmp(actual->val, expected->val, actual->val_len) == 0);
}

/* Verify the node envelope independently of its entries and payload. */
static void
check_node_header(const unsigned char *raw, const apfs_node_t *node,
    const apfs_pack_profile_t *profile, uint64_t index, bool root)
{
	apfs_obj_t object;
	uint32_t entries, entry_bytes;
	uint16_t flags;

	CHECK(apfs_object_verify(raw, BLOCK));
	CHECK(apfs_obj_decode(raw, BLOCK, &object));
	CHECK(object.oid == FIRST_BLOCK + index);
	CHECK(object.xid == TEST_XID);
	CHECK(object.kind == (root ? APFS_KIND_BTREE_ROOT :
	    APFS_KIND_BTREE_NODE));
	CHECK(object.subtype == profile->subtype);
	CHECK(object.flags == profile->storage);
	CHECK(node->level < APFS_BTREE_LEVELS_MAX);
	flags = root ? APFS_NODE_ROOT : 0;
	if (node->level == 0)
		flags |= APFS_NODE_LEAF;
	if (profile->fixed_key != 0)
		flags |= APFS_NODE_FIXED_KV;
	CHECK(node->flags == flags);
	CHECK(root || node->key_count != 0);
	CHECK(node->key_free.off == APFS_FREE_LIST_EMPTY);
	CHECK(node->key_free.len == 0);
	CHECK(node->val_free.off == APFS_FREE_LIST_EMPTY);
	CHECK(node->val_free.len == 0);
	CHECK(node->toc.off == 0);
	/* Every table rounds to eight entries before applying its floor. */
	entries = (node->key_count + 7) / 8 * 8;
	if (entries < profile->toc_min)
		entries = profile->toc_min;
	entry_bytes = profile->fixed_key == 0 ? APFS_TOC_VARIABLE_BYTES :
	    APFS_TOC_FIXED_BYTES;
	CHECK(node->toc.len == entries * entry_bytes);
}

static void
check_index_entry(const packed_tree_t *tree,
    const apfs_pack_profile_t *profile, const unsigned char *raw,
    const apfs_node_t *node, const apfs_entry_t *entry, uint64_t index)
{
	const unsigned char *child_raw;
	apfs_kv_size_t sizes;
	apfs_node_t child;
	apfs_entry_t first;
	uint64_t child_index;

	CHECK(!entry->ghost && entry->val_len == APFS_CHILD_OID_BYTES);
	child_index = fsimg_load_le64(raw + entry->val_off);
	CHECK(child_index >= FIRST_BLOCK);
	child_index -= FIRST_BLOCK;
	CHECK(child_index < index);
	child_raw = captured_node(&tree->capture, child_index);
	CHECK(apfs_node_decode(child_raw, BLOCK, &child));
	CHECK(child.level + 1 == node->level);
	sizes = node_sizes(profile, &child);
	CHECK(apfs_entry_decode(child_raw, BLOCK, &child, 0, &sizes, &first));
	CHECK(first.key_len == entry->key_len);
	CHECK(bcmp(raw + entry->key_off, child_raw + first.key_off,
	    entry->key_len) == 0);
}

static void
check_leaf_entry(const unsigned char *raw, const apfs_entry_t *entry,
    const apfs_pack_profile_t *profile, const stream_t *stream,
    leaf_totals_t *totals)
{
	unsigned char key[BLOCK], value[BLOCK];
	apfs_record_t expected, actual;

	CHECK(totals->count < stream->count);
	make_record(profile, stream, totals->count, key, value, &expected);
	totals->count++;
	actual = (apfs_record_t){ .key = raw + entry->key_off,
	    .key_len = entry->key_len, .ghost = entry->ghost,
	    .val = entry->ghost ? NULL : raw + entry->val_off,
	    .val_len = entry->val_len };
	check_record(&actual, &expected);
	if (expected.key_len > totals->longest_key)
		totals->longest_key = expected.key_len;
	if (expected.val_len > totals->longest_val)
		totals->longest_val = expected.val_len;
}

/*
 * Mark only actual fields and records. Spare table entries, alignment gaps
 * and all free bytes must be zero, without recreating the staging layout.
 */
static void
check_unused_bytes(const unsigned char *raw, const apfs_node_t *node,
    const apfs_pack_profile_t *profile)
{
	unsigned char used[BLOCK] = { 0 };
	apfs_kv_size_t sizes;
	apfs_entry_t entry;
	unsigned entry_bytes;

	memset(used, 1, APFS_NODE_HEADER_BYTES);
	entry_bytes = profile->fixed_key == 0 ? APFS_TOC_VARIABLE_BYTES :
	    APFS_TOC_FIXED_BYTES;
	memset(used + node->area.toc_start, 1, node->key_count * entry_bytes);
	if ((node->flags & APFS_NODE_ROOT) != 0)
		memset(used + BLOCK - APFS_BTREE_INFO_BYTES, 1,
		    APFS_BTREE_INFO_BYTES);
	sizes = node_sizes(profile, node);
	for (uint32_t i = 0; i < node->key_count; i++) {
		CHECK(apfs_entry_decode(raw, BLOCK, node, i, &sizes, &entry));
		memset(used + entry.key_off, 1, entry.key_len);
		if (!entry.ghost)
			memset(used + entry.val_off, 1, entry.val_len);
		if ((profile->tree_flags & APFS_BTREE_KV_NONALIGNED) == 0) {
			CHECK((entry.key_off - node->area.key_start) % 8 == 0);
			CHECK(entry.ghost ||
			    (node->area.value_end - entry.val_off) % 8 == 0);
		}
	}
	for (unsigned i = node->area.free_start; i < node->area.free_end; i++)
		CHECK(raw[i] == 0);
	for (unsigned i = 0; i < BLOCK; i++)
		CHECK(used[i] != 0 || raw[i] == 0);
}

static void
check_root_footer(const packed_tree_t *tree, const apfs_node_t *node,
    const apfs_pack_profile_t *profile, const leaf_totals_t *totals)
{
	CHECK(tree->result.levels == node->level + 1u);
	CHECK(node->info.key_count == totals->count);
	CHECK(node->info.node_count == tree->capture.count);
	CHECK(node->info.longest_key == totals->longest_key);
	CHECK(node->info.longest_val == totals->longest_val);
	CHECK(node->info.flags == profile->tree_flags);
	CHECK(node->info.node_size == BLOCK);
	CHECK(node->info.key_size == profile->fixed_key);
	CHECK(node->info.val_size == profile->fixed_val);
}

static void
check_nodes(const packed_tree_t *tree, const apfs_pack_profile_t *profile,
    const stream_t *stream)
{
	uint64_t level_nodes[APFS_BTREE_LEVELS_MAX] = { 0 };
	leaf_totals_t totals = { 0 };
	const unsigned char *raw;
	apfs_kv_size_t sizes;
	apfs_node_t node;
	apfs_entry_t entry;
	bool root;

	CHECK(tree->result.levels >= 1 &&
	    tree->result.levels <= APFS_BTREE_LEVELS_MAX);
	CHECK(tree->capture.count == tree->result.node_count);
	for (uint64_t k = 0; k < tree->capture.count; k++) {
		raw = captured_node(&tree->capture, k);
		CHECK(apfs_node_decode(raw, BLOCK, &node));
		root = k + 1 == tree->capture.count;
		check_node_header(raw, &node, profile, k, root);
		level_nodes[node.level]++;
		sizes = node_sizes(profile, &node);
		for (uint32_t i = 0; i < node.key_count; i++) {
			CHECK(apfs_entry_decode(raw, BLOCK, &node, i, &sizes,
			    &entry));
			if (node.level == 0)
				check_leaf_entry(raw, &entry, profile, stream, &totals);
			else
				check_index_entry(tree, profile, raw, &node, &entry, k);
		}
		check_unused_bytes(raw, &node, profile);
		if (root)
			check_root_footer(tree, &node, profile, &totals);
	}
	CHECK(totals.count == stream->count);
	for (unsigned i = 0; i < APFS_BTREE_LEVELS_MAX; i++)
		CHECK(level_nodes[i] == tree->result.level_nodes[i]);
}

static int
compare_bytes(const unsigned char *a, size_t a_len,
    const unsigned char *b, size_t b_len)
{
	int order;

	order = memcmp(a, b, a_len < b_len ? a_len : b_len);
	if (order != 0)
		return (order);
	return ((a_len > b_len) - (a_len < b_len));
}

static int
compare_omap(const unsigned char *a, size_t a_len,
    const unsigned char *b, size_t b_len)
{
	apfs_omap_key_t left, right;

	CHECK(apfs_omap_key_decode(a, a_len, &left));
	CHECK(apfs_omap_key_decode(b, b_len, &right));
	if (left.oid != right.oid)
		return (left.oid > right.oid ? 1 : -1);
	return ((left.xid > right.xid) - (left.xid < right.xid));
}

static fsimg_status_t
memory_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *count, fsimg_error_t *error FSIMG_UNUSED)
{
	capture_t *capture = argument;
	uint64_t bytes = (FIRST_BLOCK + capture->count) * BLOCK;

	CHECK(offset <= bytes && length <= bytes - offset);
	bcopy(capture->bytes + offset, buffer, length);
	*count = length;
	return (FSIMG_OK);
}

static fsimg_status_t
memory_size(void *argument, uint64_t *bytes,
    fsimg_error_t *error FSIMG_UNUSED)
{
	capture_t *capture = argument;

	*bytes = (FIRST_BLOCK + capture->count) * BLOCK;
	return (FSIMG_OK);
}

static void
check_cursor(fsimg_ctx_t *ctx, const packed_tree_t *tree,
    const apfs_pack_profile_t *profile, const stream_t *stream)
{
	unsigned char key[BLOCK], value[BLOCK];
	apfs_tree_profile_t codec;
	apfs_container_t container;
	fsimg_reader_t reader;
	fsimg_io_ops_t ops;
	fsimg_io_t *io;
	apfs_cursor_t cursor;
	apfs_record_t actual, expected;
	fsimg_error_t error;
	bool found;

	codec = (apfs_tree_profile_t){ .role = profile->role,
	    .compare = profile == &apfs_pack_container_omap ?
	    compare_omap : compare_bytes, .subtype = profile->subtype,
	    .key_min = 8, .key_max = BLOCK, .val_max = BLOCK,
	    .fixed_key = profile->fixed_key, .fixed_val = profile->fixed_val };
	fsimg_io_ops_init(&ops);
	ops.read_at = memory_read;
	ops.get_size = memory_size;
	CHECK(fsimg_io_from_callbacks(ctx, &ops, (void *)&tree->capture, &io,
	    &error) == FSIMG_OK);
	bzero(&reader, sizeof(reader));
	reader.ctx = ctx;
	reader.input = io;
	reader.slice_bytes = (FIRST_BLOCK + tree->capture.count) * BLOCK;
	reader.state = FSIMG_READER_OPENING;
	fsimg_read_options_init(&reader.options);
	fsimg_session_init(&reader.session, ctx, "packer test", 0);
	container = (apfs_container_t){ .reader = &reader,
	    .block_size = BLOCK, .block_count = FIRST_BLOCK + tree->capture.count,
	    .xid = TEST_XID };
	apfs_cursor_init(&cursor, &container, &codec, APFS_OBJ_PHYSICAL,
	    tree->result.root_oid, TEST_XID, NULL);
	check_status(apfs_cursor_first(&cursor, &found, &error), FSIMG_OK,
	    &error);
	for (uint64_t i = 0; i < stream->count; i++) {
		CHECK(found);
		apfs_cursor_record(&cursor, &actual);
		make_record(profile, stream, i, key, value, &expected);
		check_record(&actual, &expected);
		check_status(apfs_cursor_next(&cursor, &found, &error), FSIMG_OK,
		    &error);
	}
	CHECK(!found);
	apfs_cursor_free(&cursor);
	fsimg_io_free(io);
}

/* Compare planning, an OID trace, and clean versus poisoned emissions. */
static packed_tree_t
check_stream(const apfs_pack_profile_t *profile, const stream_t *stream)
{
	apfs_pack_result_t plan, trace_result, repeat_result;
	packed_tree_t tree, poisoned;
	capture_t trace, repeat;
	apfs_pack_sink_t sink;
	apfs_node_t planned_node, emitted_node;
	fsimg_stats_t before, stats;
	fsimg_ctx_t *ctx;

	ctx = context_new();
	CHECK(fsimg_ctx_get_stats(ctx, &before, NULL) == FSIMG_OK);
	plan = pack_stream(ctx, profile, stream, NULL);
	capture_init(&trace, plan.node_count);
	sink = (apfs_pack_sink_t){ .arg = &trace, .write = capture_node };
	trace_result = pack_stream(ctx, profile, stream, &sink);
	check_results(&plan, &trace_result);
	CHECK(plan.root_oid == plan.node_count - 1);
	CHECK(trace_result.root_oid == plan.root_oid);
	capture_init(&tree.capture, plan.node_count);
	sink.arg = &tree.capture;
	sink.node_oid = mapped_oid;
	tree.result = pack_stream(ctx, profile, stream, &sink);
	check_results(&plan, &tree.result);
	CHECK(tree.result.root_oid == FIRST_BLOCK + plan.root_oid);
	CHECK(trace.count == tree.capture.count);
	for (uint64_t i = 0; i < trace.count; i++) {
		CHECK(apfs_node_decode(captured_node(&trace, i), BLOCK,
		    &planned_node));
		CHECK(apfs_node_decode(captured_node(&tree.capture, i), BLOCK,
		    &emitted_node));
		CHECK(planned_node.level == emitted_node.level);
		CHECK(planned_node.key_count == emitted_node.key_count);
	}
	capture_init(&repeat, plan.node_count);
	sink.arg = &repeat;
	fsimg_test_poison_allocations(ctx, true);
	repeat_result = pack_stream(ctx, profile, stream, &sink);
	fsimg_test_poison_allocations(ctx, false);
	check_results(&tree.result, &repeat_result);
	CHECK(tree.result.root_oid == repeat_result.root_oid);
	CHECK(repeat.count == tree.capture.count);
	for (uint64_t i = 0; i < repeat.count; i++)
		CHECK(bcmp(captured_node(&tree.capture, i),
		    captured_node(&repeat, i), BLOCK) == 0);
	check_nodes(&tree, profile, stream);
	poisoned = (packed_tree_t){ .capture = repeat, .result = repeat_result };
	check_nodes(&poisoned, profile, stream);
	if (profile->storage == APFS_OBJ_PHYSICAL)
		check_cursor(ctx, &tree, profile, stream);
	CHECK(fsimg_ctx_get_stats(ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == before.memory_bytes);
	CHECK(stats.scratch_bytes == 0);
	fsimg_ctx_free(ctx);
	free(trace.bytes);
	free(repeat.bytes);
	return (tree);
}

static void
test_empty_trees_have_native_tables_and_footers(void)
{
	const apfs_pack_profile_t *profiles[] = {
		&apfs_pack_fs_tree, &apfs_pack_container_omap, &apfs_pack_free_queue
	};
	const uint32_t footer_flags[] = { 0x42, 0x12, 0xe };
	const uint16_t table_lengths[] = { 0, 448, 576 };
	packed_tree_t tree;
	apfs_node_t node;
	stream_t stream = { 0 };

	scenario = __func__;
	for (unsigned i = 0; i < 3; i++) {
		tree = check_stream(profiles[i], &stream);
		CHECK(tree.result.node_count == 1 && tree.result.levels == 1);
		CHECK(apfs_node_decode(captured_node(&tree.capture, 0), BLOCK,
		    &node));
		CHECK(node.key_count == 0);
		CHECK(node.flags == (APFS_NODE_ROOT | APFS_NODE_LEAF |
		    (i == 0 ? 0 : APFS_NODE_FIXED_KV)));
		CHECK(node.info.flags == footer_flags[i]);
		CHECK(node.toc.len == table_lengths[i]);
		free(tree.capture.bytes);
	}
}

static void
test_one_record_round_trips(void)
{
	packed_tree_t tree;
	stream_t stream = { .count = 1, .key_size = 16, .val_size = 16 };

	scenario = __func__;
	tree = check_stream(&apfs_pack_container_omap, &stream);
	CHECK(tree.result.node_count == 1 && tree.result.levels == 1);
	free(tree.capture.bytes);
	stream.key_size = 9;
	stream.val_size = 13;
	tree = check_stream(&apfs_pack_fs_tree, &stream);
	CHECK(tree.result.node_count == 1 && tree.result.levels == 1);
	free(tree.capture.bytes);
}

static void
test_exactly_full_leaf_then_one_byte_more_splits(void)
{
	packed_tree_t tree;
	apfs_node_t root, leaf;
	stream_t stream = { .count = 2, .key_size = 8, .val_size = 1960 };

	scenario = __func__;
	/* 64 TOC bytes + two 8-byte keys + two 1960-byte values = 4000. */
	tree = check_stream(&apfs_pack_fs_tree, &stream);
	CHECK(tree.result.levels == 1 && tree.result.node_count == 1);
	CHECK(apfs_node_decode(captured_node(&tree.capture, 0), BLOCK, &root));
	CHECK(root.area.free_start == root.area.free_end);
	free(tree.capture.bytes);
	stream.last_extra = 1;
	tree = check_stream(&apfs_pack_fs_tree, &stream);
	CHECK(tree.result.levels == 2 && tree.result.node_count == 3);
	CHECK(apfs_node_decode(captured_node(&tree.capture, 2), BLOCK, &root));
	CHECK(root.key_count == 2);
	for (unsigned i = 0; i < 2; i++) {
		CHECK(apfs_node_decode(captured_node(&tree.capture, i), BLOCK,
		    &leaf));
		CHECK(leaf.key_count == 1 && leaf.level == 0);
	}
	free(tree.capture.bytes);
}

static void
test_fixed_leaf_overflow_leaves_one_record_and_two_children(void)
{
	packed_tree_t tree;
	apfs_node_t node;
	stream_t stream = { .count = 111, .key_size = 16, .val_size = 16 };

	scenario = __func__;
	/* A 448-byte TOC leaves 3552 bytes: 111 pairs of 16-byte fields. */
	tree = check_stream(&apfs_pack_container_omap, &stream);
	CHECK(tree.result.levels == 1);
	free(tree.capture.bytes);
	stream.count++;
	tree = check_stream(&apfs_pack_container_omap, &stream);
	CHECK(tree.result.levels == 2 && tree.result.node_count == 3);
	CHECK(apfs_node_decode(captured_node(&tree.capture, 0), BLOCK, &node));
	CHECK(node.key_count == 111);
	CHECK(apfs_node_decode(captured_node(&tree.capture, 1), BLOCK, &node));
	CHECK(node.key_count == 1);
	CHECK(apfs_node_decode(captured_node(&tree.capture, 2), BLOCK, &node));
	CHECK(node.key_count == 2 && node.level == 1);
	free(tree.capture.bytes);
}

static void
test_variable_records_round_trip_at_three_heights(void)
{
	const apfs_pack_profile_t *profiles[] = {
		&physical_aligned, &physical_unaligned, &apfs_pack_fs_tree
	};
	const uint64_t counts[] = { 1, 20, 200 };
	packed_tree_t tree;
	stream_t stream = { .key_size = 257, .val_size = 129, .variation = 6 };

	scenario = __func__;
	/* Large separators give a small fanout, reaching level 2 cheaply. */
	for (unsigned p = 0; p < 3; p++) {
		for (unsigned height = 1; height <= 3; height++) {
			stream.count = counts[height - 1];
			tree = check_stream(profiles[p], &stream);
			CHECK(tree.result.levels == height);
			free(tree.capture.bytes);
		}
	}
	stream = (stream_t){ .count = 20000, .key_size = 16, .val_size = 16 };
	tree = check_stream(&apfs_pack_container_omap, &stream);
	CHECK(tree.result.levels == 3);
	free(tree.capture.bytes);
}

static void
test_odd_lengths_use_alignment_only_when_required(void)
{
	packed_tree_t tree;
	stream_t stream = { .count = 101, .key_size = 9, .val_size = 13 };

	scenario = __func__;
	/* 101 aligned pairs take 3232 bytes plus 832 TOC bytes, over 4000. */
	tree = check_stream(&physical_aligned, &stream);
	CHECK(tree.result.levels == 2);
	free(tree.capture.bytes);
	/* Without rounding the same pairs and TOC occupy only 3054 bytes. */
	tree = check_stream(&physical_unaligned, &stream);
	CHECK(tree.result.levels == 1);
	free(tree.capture.bytes);
}

static void
test_free_queue_preserves_ghosts(void)
{
	packed_tree_t tree;
	stream_t stream = { .count = 500, .key_size = 16, .val_size = 8,
	    .ghosts = true };

	scenario = __func__;
	tree = check_stream(&apfs_pack_free_queue, &stream);
	CHECK(tree.result.levels == 2);
	free(tree.capture.bytes);
}

static void
test_large_stream_uses_bounded_memory(void)
{
	stream_t stream = { .count = 2000000, .key_size = 8, .val_size = 8 };
	apfs_pack_result_t result;
	fsimg_stats_t before, stats;
	fsimg_ctx_t *ctx;

	scenario = __func__;
	ctx = context_new();
	CHECK(fsimg_ctx_get_stats(ctx, &before, NULL) == FSIMG_OK);
	result = pack_stream(ctx, &apfs_pack_fs_tree, &stream, NULL);
	CHECK(result.key_count == stream.count);
	CHECK(result.levels >= 3 && result.levels <= APFS_BTREE_LEVELS_MAX);
	CHECK(fsimg_ctx_get_stats(ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_peak <= MEMORY_LIMIT);
	CHECK(stats.memory_bytes == before.memory_bytes);
	CHECK(stats.scratch_bytes == 0);
	fsimg_ctx_free(ctx);
}

static void
test_invalid_records_are_refused(void)
{
	unsigned char key[BLOCK] = { 0 }, value[BLOCK] = { 0 };
	const apfs_pack_profile_t *profiles[] = {
		&apfs_pack_fs_tree, &apfs_pack_container_omap,
		&apfs_pack_container_omap, &apfs_pack_fs_tree, &apfs_pack_fs_tree,
		&apfs_pack_fs_tree, &apfs_pack_fs_tree, &apfs_pack_fs_tree,
		&apfs_pack_fs_tree
	};
	apfs_record_t records[] = {
		{ .key = key, .key_len = 8, .ghost = true },
		{ .key = key, .key_len = 15, .val = value, .val_len = 16 },
		{ .key = key, .key_len = 16, .val = value, .val_len = 15 },
		{ .key = key, .key_len = 8, .val = value, .val_len = 4000 },
		{ .key = key, .key_len = 3936, .val = value, .val_len = 0 },
		{ .key = key, .key_len = 1965, .val = value, .val_len = 0 },
		{ .key = key, .key_len = 0 },
		{ .key = key, .key_len = 65544 },
		{ .key = key, .key_len = SIZE_MAX }
	};
	fsimg_stats_t before, stats;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	apfs_pack_t *pack;

	scenario = __func__;
	ctx = context_new();
	CHECK(fsimg_ctx_get_stats(ctx, &before, NULL) == FSIMG_OK);
	CHECK(sizeof(profiles) / sizeof(profiles[0]) ==
	    sizeof(records) / sizeof(records[0]));
	/* A 1965-byte leaf key fits, but two keys with child OIDs do not. */
	for (unsigned i = 0; i < sizeof(records) / sizeof(records[0]); i++) {
		check_status(apfs_pack_open(ctx, profiles[i], TEST_XID, NULL,
		    &pack, &error), FSIMG_OK, &error);
		check_status(apfs_pack_add(pack, &records[i], &error),
		    FSIMG_EINVAL, &error);
		apfs_pack_free(pack);
		CHECK(fsimg_ctx_get_stats(ctx, &stats, NULL) == FSIMG_OK);
		CHECK(stats.memory_bytes == before.memory_bytes);
	}
	apfs_pack_free(NULL);
	fsimg_ctx_free(ctx);
}

static fsimg_status_t
fail_write(void *argument, uint64_t index,
    uint64_t oid FSIMG_UNUSED, const unsigned char *node FSIMG_UNUSED,
    fsimg_error_t *error)
{
	failing_sink_t *sink = argument;

	CHECK(index == sink->calls);
	sink->calls++;
	if (sink->calls < sink->fail_on)
		return (FSIMG_OK);
	return (fsimg_error_set(error, FSIMG_EIO, 73, FSIMG_OP_WRITER,
	    "test sink failure"));
}

static void
test_sink_failure_is_returned_unchanged(void)
{
	unsigned char key[BLOCK], value[BLOCK];
	stream_t stream = { .count = 2, .key_size = 8, .val_size = 2000 };
	apfs_pack_result_t result;
	apfs_pack_sink_t sink;
	apfs_record_t record;
	fsimg_error_t error;
	fsimg_stats_t before, stats;
	fsimg_ctx_t *ctx;
	apfs_pack_t *pack;
	failing_sink_t failure;

	scenario = __func__;
	ctx = context_new();
	CHECK(fsimg_ctx_get_stats(ctx, &before, NULL) == FSIMG_OK);
	/* Exercise both emission during add and the final root write. */
	for (unsigned during_add = 0; during_add < 2; during_add++) {
		failure = (failing_sink_t){ .fail_on = 1 };
		sink = (apfs_pack_sink_t){ .arg = &failure, .write = fail_write };
		check_status(apfs_pack_open(ctx, &apfs_pack_fs_tree, TEST_XID,
		    &sink, &pack, &error), FSIMG_OK, &error);
		make_record(&apfs_pack_fs_tree, &stream, 0, key, value, &record);
		check_status(apfs_pack_add(pack, &record, &error), FSIMG_OK,
		    &error);
		if (during_add != 0) {
			make_record(&apfs_pack_fs_tree, &stream, 1, key, value,
			    &record);
			check_status(apfs_pack_add(pack, &record, &error),
			    FSIMG_EIO, &error);
		} else {
			check_status(apfs_pack_finish(pack, &result, &error),
			    FSIMG_EIO, &error);
		}
		CHECK(failure.calls == 1);
		CHECK(error.status == FSIMG_EIO && error.os_error == 73);
		CHECK(error.operation == FSIMG_OP_WRITER);
		CHECK(strcmp(error.message, "test sink failure") == 0);
		check_status(apfs_pack_add(pack, &record, &error),
		    FSIMG_ESTATE, &error);
		check_status(apfs_pack_finish(pack, &result, &error),
		    FSIMG_ESTATE, &error);
		apfs_pack_free(pack);
		CHECK(fsimg_ctx_get_stats(ctx, &stats, NULL) == FSIMG_OK);
		CHECK(stats.memory_bytes == before.memory_bytes);
	}
	fsimg_ctx_free(ctx);
}

static void
test_open_allocation_failures_release_memory(void)
{
	fsimg_stats_t before, after;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	apfs_pack_t *pack;

	scenario = __func__;
	ctx = context_new();
	CHECK(fsimg_ctx_get_stats(ctx, &before, NULL) == FSIMG_OK);
	/* Both the pack object and its initial leaf buffer can fail. */
	for (unsigned attempt = 1; attempt <= 2; attempt++) {
		fsimg_test_fail_alloc_after(ctx, attempt);
		check_status(apfs_pack_open(ctx, &apfs_pack_fs_tree, TEST_XID,
		    NULL, &pack, &error), FSIMG_ENOMEM, &error);
		CHECK(pack == NULL);
		CHECK(fsimg_ctx_get_stats(ctx, &after, NULL) == FSIMG_OK);
		CHECK(after.memory_bytes == before.memory_bytes);
		CHECK(after.scratch_bytes == before.scratch_bytes);
	}
	fsimg_test_fail_alloc_after(ctx, 0);
	fsimg_ctx_free(ctx);
}

/*
 * Every node is packed against a root's capacity, so a full node that is
 * not the root keeps the footer's 40 bytes in its free space and would
 * still fit as a root.
 */
static void
test_nonroot_nodes_keep_room_for_a_footer(void)
{
	stream_t stream = { .count = 112, .key_size = 16, .val_size = 16 };
	packed_tree_t tree;
	apfs_node_t node;

	scenario = __func__;
	tree = check_stream(&apfs_pack_container_omap, &stream);
	CHECK(tree.result.node_count == 3);
	CHECK(apfs_node_decode(captured_node(&tree.capture, 0), BLOCK, &node));
	CHECK(node.key_count == 111);
	CHECK(node.area.value_end == BLOCK);
	CHECK(node.free.len == APFS_BTREE_INFO_BYTES);
	free(tree.capture.bytes);
}

/* Find the fixed tree's capacity directly from the on-disk size rule. */
static unsigned
omap_capacity(unsigned value_bytes)
{
	unsigned count, table_entries;

	for (count = 1; ; count++) {
		table_entries = (count + 7) / 8 * 8;
		if (table_entries < 112)
			table_entries = 112;
		if (table_entries * APFS_TOC_FIXED_BYTES +
		    count * (APFS_OMAP_KEY_BYTES + value_bytes) >
		    APFS_PACK_CAPACITY)
			return (count - 1);
	}
}

static void
test_finish_grows_a_level_when_the_top_overflows(void)
{
	const apfs_pack_profile_t *profile = &apfs_pack_container_omap;
	stream_t stream = { .key_size = 16, .val_size = 16 };
	unsigned char key[BLOCK], value[BLOCK];
	unsigned leaf_capacity, index_capacity;
	packed_tree_t tree;
	apfs_pack_sink_t sink;
	apfs_pack_t *pack;
	apfs_record_t record;
	apfs_node_t node;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;

	scenario = __func__;
	/*
	 * 4*max(roundup(n,8),112) + n*(16+value) <= 4000 gives
	 * 111 leaf records and 142 index children. Thus 111*142 = 15762
	 * records fill a level-1 root; record 15763 needs a 143rd child.
	 * Before finish only 142 leaves have closed, so that child is still
	 * pending and the top has not overflowed.
	 */
	leaf_capacity = omap_capacity(APFS_OMAP_VAL_BYTES);
	index_capacity = omap_capacity(APFS_CHILD_OID_BYTES);
	CHECK(leaf_capacity == 111 && index_capacity == 142);
	stream.count = leaf_capacity * index_capacity;
	CHECK(stream.count == 15762);
	tree = check_stream(profile, &stream);
	CHECK(tree.result.levels == 2);
	CHECK(tree.result.level_nodes[0] == index_capacity);
	CHECK(apfs_node_decode(captured_node(&tree.capture,
	    tree.capture.count - 1), BLOCK, &node));
	CHECK(node.level == 1 && node.key_count == index_capacity);
	free(tree.capture.bytes);

	stream.count++;
	CHECK(stream.count == 15763);
	ctx = context_new();
	capture_init(&tree.capture, index_capacity + 4);
	sink = (apfs_pack_sink_t){ .arg = &tree.capture,
	    .node_oid = mapped_oid, .write = capture_node };
	check_status(apfs_pack_open(ctx, profile, TEST_XID, &sink, &pack,
	    &error), FSIMG_OK, &error);
	for (uint64_t i = 0; i < stream.count; i++) {
		make_record(profile, &stream, i, key, value, &record);
		check_status(apfs_pack_add(pack, &record, &error), FSIMG_OK,
		    &error);
	}
	CHECK(tree.capture.count == index_capacity);
	for (uint64_t i = 0; i < tree.capture.count; i++) {
		CHECK(apfs_node_decode(captured_node(&tree.capture, i), BLOCK,
		    &node));
		CHECK(node.level == 0);
	}
	check_status(apfs_pack_finish(pack, &tree.result, &error), FSIMG_OK,
	    &error);
	CHECK(tree.result.levels == 3);
	CHECK(tree.result.level_nodes[1] == 2);
	CHECK(apfs_node_decode(captured_node(&tree.capture,
	    tree.capture.count - 1), BLOCK, &node));
	CHECK(node.level == 2 && node.key_count == 2);
	check_nodes(&tree, profile, &stream);
	check_cursor(ctx, &tree, profile, &stream);
	apfs_pack_free(pack);
	free(tree.capture.bytes);
	fsimg_ctx_free(ctx);
}

static void
test_physical_ghosts_round_trip_through_cursor(void)
{
	stream_t stream = { .count = 500, .key_size = 16, .val_size = 8,
	    .ghosts = true };
	packed_tree_t tree;
	apfs_node_t leaf;

	scenario = __func__;
	tree = check_stream(&physical_ghosts, &stream);
	CHECK(tree.result.levels == 2);
	CHECK(apfs_node_decode(captured_node(&tree.capture, 0), BLOCK, &leaf));
	CHECK(leaf.key_count > 144);
	free(tree.capture.bytes);
}

static void
test_largest_admitted_key_packs_three_records(void)
{
	stream_t stream = { .count = 3, .val_size = 0 };
	packed_tree_t tree;

	scenario = __func__;
	/* Eight variable TOC entries take 64 bytes: 64 + 2*(k+8) = 4000. */
	stream.key_size = (APFS_PACK_CAPACITY - 8 * APFS_TOC_VARIABLE_BYTES) /
	    2 - APFS_CHILD_OID_BYTES;
	CHECK(stream.key_size == 1960);
	tree = check_stream(&apfs_pack_fs_tree, &stream);
	CHECK(tree.result.levels == 2 && tree.result.node_count == 3);
	free(tree.capture.bytes);
}

/*
 * With 1960-byte keys and 1976-byte values a leaf holds one record and an
 * index node two, so 2^15 records make exactly 16 levels and one more
 * record needs a seventeenth, whether add or finish reaches it.
 */
static void
test_tree_taller_than_sixteen_levels_is_refused(void)
{
	unsigned char key[BLOCK], value[BLOCK];
	stream_t stream = { .count = 32768, .key_size = 1960,
	    .val_size = 1976 };
	apfs_pack_result_t result;
	apfs_record_t record;
	fsimg_stats_t before, stats;
	fsimg_status_t status = FSIMG_OK;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	apfs_pack_t *pack;

	scenario = __func__;
	ctx = context_new();
	result = pack_stream(ctx, &apfs_pack_fs_tree, &stream, NULL);
	CHECK(result.levels == APFS_BTREE_LEVELS_MAX);
	CHECK(result.level_nodes[0] == stream.count);

	stream.count++;
	CHECK(fsimg_ctx_get_stats(ctx, &before, NULL) == FSIMG_OK);
	check_status(apfs_pack_open(ctx, &apfs_pack_fs_tree, TEST_XID, NULL,
	    &pack, &error), FSIMG_OK, &error);
	for (uint64_t i = 0; i < stream.count && status == FSIMG_OK; i++) {
		make_record(&apfs_pack_fs_tree, &stream, i, key, value, &record);
		status = apfs_pack_add(pack, &record, &error);
	}
	if (status == FSIMG_OK)
		status = apfs_pack_finish(pack, &result, &error);
	check_status(status, FSIMG_ERANGE, &error);
	check_status(apfs_pack_add(pack, &record, &error), FSIMG_ESTATE,
	    &error);
	apfs_pack_free(pack);
	CHECK(fsimg_ctx_get_stats(ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == before.memory_bytes);
	fsimg_ctx_free(ctx);
}

static void
test_malformed_profiles_are_refused_at_open(void)
{
	apfs_pack_profile_t profiles[9];
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	apfs_pack_t *pack;
	fsimg_stats_t before, after;

	scenario = __func__;
	for (unsigned i = 0; i < 9; i++)
		profiles[i] = physical_aligned;
	profiles[0].role = NULL;
	profiles[1].fixed_key = APFS_OMAP_KEY_BYTES;
	profiles[2].fixed_val = APFS_OMAP_VAL_BYTES;
	profiles[3].toc_min = 1000;
	profiles[4].tree_flags &= ~APFS_BTREE_PHYSICAL;
	profiles[5].storage = APFS_OBJ_VIRTUAL;
	profiles[6].tree_flags |= 0x200;
	profiles[7].tree_flags |= APFS_BTREE_HASHED;
	profiles[8].storage = 0x12345678;

	ctx = context_new();
	CHECK(fsimg_ctx_get_stats(ctx, &before, NULL) == FSIMG_OK);
	for (unsigned i = 0; i < 9; i++) {
		/* A non-NULL input proves open clears the output on refusal. */
		pack = (apfs_pack_t *)&profiles[i];
		check_status(apfs_pack_open(ctx, &profiles[i], TEST_XID, NULL,
		    &pack, &error), FSIMG_EINVAL, &error);
		CHECK(pack == NULL);
		CHECK(fsimg_ctx_get_stats(ctx, &after, NULL) == FSIMG_OK);
		CHECK(after.memory_bytes == before.memory_bytes);
	}
	fsimg_ctx_free(ctx);
}

static void
test_successful_finish_refuses_more_operations(void)
{
	unsigned char key[8] = { 0 };
	apfs_record_t record = { .key = key, .key_len = sizeof(key) };
	apfs_pack_result_t result;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	apfs_pack_t *pack;

	scenario = __func__;
	ctx = context_new();
	check_status(apfs_pack_open(ctx, &apfs_pack_fs_tree, TEST_XID, NULL,
	    &pack, &error), FSIMG_OK, &error);
	check_status(apfs_pack_add(pack, &record, &error), FSIMG_OK, &error);
	check_status(apfs_pack_finish(pack, &result, &error), FSIMG_OK, &error);
	check_status(apfs_pack_finish(pack, &result, &error), FSIMG_ESTATE,
	    &error);
	check_status(apfs_pack_add(pack, &record, &error), FSIMG_ESTATE, &error);
	apfs_pack_free(pack);
	fsimg_ctx_free(ctx);
}

static void
test_level_allocation_failure_after_child_emission_releases_memory(void)
{
	stream_t stream = { .count = 2, .key_size = 8, .val_size = 2000 };
	unsigned char key[BLOCK], value[BLOCK];
	capture_t capture;
	apfs_pack_sink_t sink;
	apfs_pack_result_t result;
	apfs_record_t record;
	fsimg_stats_t before, after;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	apfs_pack_t *pack;

	scenario = __func__;
	ctx = context_new();
	CHECK(fsimg_ctx_get_stats(ctx, &before, NULL) == FSIMG_OK);
	capture_init(&capture, 1);
	sink = (apfs_pack_sink_t){ .arg = &capture, .write = capture_node };
	/* Open uses two attempts (pack and leaf); the next opens level 1. */
	fsimg_test_fail_alloc_after(ctx, 3);
	check_status(apfs_pack_open(ctx, &apfs_pack_fs_tree, TEST_XID, &sink,
	    &pack, &error), FSIMG_OK, &error);
	CHECK(ctx->allocation_attempts == 2);
	make_record(&apfs_pack_fs_tree, &stream, 0, key, value, &record);
	check_status(apfs_pack_add(pack, &record, &error), FSIMG_OK, &error);
	make_record(&apfs_pack_fs_tree, &stream, 1, key, value, &record);
	check_status(apfs_pack_add(pack, &record, &error), FSIMG_ENOMEM,
	    &error);
	CHECK(ctx->allocation_attempts == 3);
	CHECK(capture.count == 1);
	check_status(apfs_pack_add(pack, &record, &error), FSIMG_ESTATE, &error);
	check_status(apfs_pack_finish(pack, &result, &error), FSIMG_ESTATE,
	    &error);
	apfs_pack_free(pack);
	CHECK(fsimg_ctx_get_stats(ctx, &after, NULL) == FSIMG_OK);
	CHECK(after.memory_bytes == before.memory_bytes);
	CHECK(after.scratch_bytes == before.scratch_bytes);
	fsimg_test_fail_alloc_after(ctx, 0);
	free(capture.bytes);
	fsimg_ctx_free(ctx);
}

static void
test_third_sink_write_failure_is_returned_unchanged(void)
{
	stream_t stream = { .count = 4, .key_size = 8, .val_size = 2000 };
	unsigned char key[BLOCK], value[BLOCK];
	failing_sink_t failure = { .fail_on = 3 };
	apfs_pack_sink_t sink = { .arg = &failure, .write = fail_write };
	apfs_record_t record;
	apfs_pack_result_t result;
	fsimg_stats_t before, after;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	apfs_pack_t *pack;

	scenario = __func__;
	ctx = context_new();
	CHECK(fsimg_ctx_get_stats(ctx, &before, NULL) == FSIMG_OK);
	check_status(apfs_pack_open(ctx, &apfs_pack_fs_tree, TEST_XID, &sink,
	    &pack, &error), FSIMG_OK, &error);
	for (uint64_t i = 0; i < 3; i++) {
		make_record(&apfs_pack_fs_tree, &stream, i, key, value, &record);
		check_status(apfs_pack_add(pack, &record, &error), FSIMG_OK,
		    &error);
	}
	CHECK(failure.calls == 2);
	make_record(&apfs_pack_fs_tree, &stream, 3, key, value, &record);
	check_status(apfs_pack_add(pack, &record, &error), FSIMG_EIO, &error);
	CHECK(failure.calls == 3);
	CHECK(error.status == FSIMG_EIO && error.os_error == 73);
	CHECK(error.operation == FSIMG_OP_WRITER);
	CHECK(strcmp(error.message, "test sink failure") == 0);
	check_status(apfs_pack_add(pack, &record, &error), FSIMG_ESTATE, &error);
	check_status(apfs_pack_finish(pack, &result, &error), FSIMG_ESTATE,
	    &error);
	apfs_pack_free(pack);
	CHECK(fsimg_ctx_get_stats(ctx, &after, NULL) == FSIMG_OK);
	CHECK(after.memory_bytes == before.memory_bytes);
	CHECK(after.scratch_bytes == before.scratch_bytes);
	fsimg_ctx_free(ctx);
}

static void
test_aligned_exactly_full_leaf_splits_after_one_more_byte(void)
{
	stream_t stream = { .count = 2, .key_size = 9, .val_size = 1952 };
	packed_tree_t tree;
	apfs_node_t node;

	scenario = __func__;
	/* 64 TOC + 2*(roundup(9,8) + 1952) = 4000 bytes. */
	CHECK(8 * APFS_TOC_VARIABLE_BYTES + 2 * (16 + 1952) ==
	    APFS_PACK_CAPACITY);
	tree = check_stream(&physical_aligned, &stream);
	CHECK(tree.result.levels == 1 && tree.result.node_count == 1);
	CHECK(apfs_node_decode(captured_node(&tree.capture, 0), BLOCK, &node));
	CHECK(node.area.free_start == node.area.free_end);
	free(tree.capture.bytes);
	/* 1953 rounds to 1960, making the same pair eight bytes too large. */
	stream.last_extra = 1;
	tree = check_stream(&physical_aligned, &stream);
	CHECK(tree.result.levels == 2 && tree.result.node_count == 3);
	CHECK(apfs_node_decode(captured_node(&tree.capture, 2), BLOCK, &node));
	CHECK(node.key_count == 2);
	free(tree.capture.bytes);
}

static void
test_admission_failure_refuses_more_operations(void)
{
	unsigned char key[1965] = { 0 };
	apfs_record_t record = { .key = key, .key_len = sizeof(key) };
	apfs_pack_result_t result;
	fsimg_status_t add_status, finish_status;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	apfs_pack_t *pack;

	scenario = __func__;
	ctx = context_new();
	check_status(apfs_pack_open(ctx, &apfs_pack_fs_tree, TEST_XID, NULL,
	    &pack, &error), FSIMG_OK, &error);
	check_status(apfs_pack_add(pack, &record, &error), FSIMG_EINVAL,
	    &error);
	/* The rejected key fits a leaf but cannot form two index entries. */
	record.key_len = 8;
	add_status = apfs_pack_add(pack, &record, &error);
	finish_status = apfs_pack_finish(pack, &result, &error);
	apfs_pack_free(pack);
	fsimg_ctx_free(ctx);
	if (add_status != FSIMG_ESTATE || finish_status != FSIMG_ESTATE) {
		fprintf(stderr, "%s: after rejecting a 1965-byte fs-tree key, "
		    "expected add/finish FSIMG_ESTATE (%d); got %d/%d\n",
		    scenario, FSIMG_ESTATE, add_status, finish_status);
		exit(1);
	}
}

int
main(void)
{
	test_empty_trees_have_native_tables_and_footers();
	test_one_record_round_trips();
	test_exactly_full_leaf_then_one_byte_more_splits();
	test_fixed_leaf_overflow_leaves_one_record_and_two_children();
	test_variable_records_round_trip_at_three_heights();
	test_odd_lengths_use_alignment_only_when_required();
	test_free_queue_preserves_ghosts();
	test_large_stream_uses_bounded_memory();
	test_invalid_records_are_refused();
	test_sink_failure_is_returned_unchanged();
	test_open_allocation_failures_release_memory();
	test_nonroot_nodes_keep_room_for_a_footer();
	test_finish_grows_a_level_when_the_top_overflows();
	test_physical_ghosts_round_trip_through_cursor();
	test_largest_admitted_key_packs_three_records();
	test_tree_taller_than_sixteen_levels_is_refused();
	test_malformed_profiles_are_refused_at_open();
	test_successful_finish_refuses_more_operations();
	test_level_allocation_failure_after_child_emission_releases_memory();
	test_third_sink_write_failure_is_returned_unchanged();
	test_aligned_exactly_full_leaf_splits_after_one_more_byte();
	test_admission_failure_refuses_more_operations();
	return (0);
}
