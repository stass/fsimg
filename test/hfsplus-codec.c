/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "hfsplus.h"
#include "test.h"

/* All disk positions below come from specs/design-hfsplus.md's tables. */
static void
put_be(unsigned char *p, uint64_t value, size_t width)
{
	size_t i;

	for (i = 0; i < width; i++)
		p[width - i - 1] = (unsigned char)(value >> (8 * i));
}

static void
put_le(unsigned char *p, uint64_t value, size_t width)
{
	size_t i;

	for (i = 0; i < width; i++)
		p[i] = (unsigned char)(value >> (8 * i));
}

static void
case_check(bool condition, const char *name, size_t length)
{
	if (!condition)
		fprintf(stderr, "codec case=%s length=%zu\n", name, length);
	CHECK(condition);
}

/* Adapters preserve the decoder's typed output; no function-pointer casts. */
typedef bool (*decode_t)(const unsigned char *, size_t, void *);
#define ADAPTER(name) \
static bool \
decode_##name(const unsigned char *p, size_t n, void *out) \
{ \
	return (hfsplus_##name##_decode(p, n, out)); \
}
ADAPTER(header)
ADAPTER(fork)
ADAPTER(extents)
ADAPTER(node)
ADAPTER(btree)
ADAPTER(ekey)
ADAPTER(ckey)
ADAPTER(folder)
ADAPTER(file)
ADAPTER(thread)
ADAPTER(akey)
ADAPTER(attr)
ADAPTER(jib)
ADAPTER(journal)
ADAPTER(mdb)
ADAPTER(cmpf)
ADAPTER(rsrc)
ADAPTER(rsrc_table)
#undef ADAPTER

static bool
decode_type(const unsigned char *p, size_t n, void *out)
{
	return (hfsplus_catalog_type(p, n, out));
}

static bool
decode_pair(const unsigned char *p, size_t n, void *out)
{
	return (hfsplus_chunk_pair_decode(p, n, 0, out));
}

static bool
decode_word(const unsigned char *p, size_t n, void *out)
{
	return (hfsplus_chunk_word_decode(p, n, 0, out));
}

/* Codecs reject bad framing without modifying any byte of their output. */
static void
reject(decode_t decode, const unsigned char *p, size_t n, size_t out_size,
    const char *name)
{
	unsigned char *out, *before;

	out = malloc(out_size);
	before = malloc(out_size);
	CHECK(out != NULL && before != NULL);
	memset(out, 0xa5, out_size);
	bcopy(out, before, out_size);
	case_check(!decode(p, n, out), name, n);
	case_check(bcmp(out, before, out_size) == 0, name, n);
	free(before);
	free(out);
}

/* Each short input has its own allocation, including a zero-length span. */
static void
check_span(decode_t decode, const unsigned char *p, size_t size,
    size_t out_size, bool exact, const char *name)
{
	unsigned char *short_copy, *long_copy;
	void *out;
	size_t n;

	for (n = 0; n < size; n++) {
		short_copy = malloc(n == 0 ? 1 : n);
		CHECK(short_copy != NULL);
		bcopy(p, short_copy, n);
		reject(decode, short_copy, n, out_size, name);
		free(short_copy);
	}
	out = malloc(out_size);
	long_copy = malloc(size + 1);
	CHECK(out != NULL && long_copy != NULL);
	bcopy(p, long_copy, size);
	long_copy[size] = 0xdb;
	case_check(decode(p, size, out), name, size);
	if (exact)
		reject(decode, long_copy, size + 1, out_size, name);
	else
		case_check(decode(long_copy, size + 1, out), name, size + 1);
	free(long_copy);
	free(out);
}

/* Forks carry a full unsigned 64-bit size and eight independent extents. */
static void
make_extents(unsigned char *p, uint32_t seed)
{
	size_t i;

	for (i = 0; i < 8; i++) {
		put_be(p + 8 * i, seed + 2 * i, 4);
		put_be(p + 8 * i + 4, seed + 2 * i + 1, 4);
	}
}

static void
check_extents(const hfsplus_extent_t *extents, uint32_t seed)
{
	size_t i;

	for (i = 0; i < 8; i++) {
		case_check(extents[i].start == seed + 2 * i, "extent start", i);
		case_check(extents[i].count == seed + 2 * i + 1,
		    "extent count", i);
	}
}

static void
make_fork(unsigned char *p, uint32_t seed)
{
	put_be(p, UINT64_C(0x8123456789abcdef) + seed, 8);
	put_be(p + 8, 0xdeadbeef, 4); /* Ignored clump hint. */
	put_be(p + 12, seed, 4);
	make_extents(p + 16, seed + 0x100);
}

static void
check_fork(const hfsplus_fork_t *fork, uint32_t seed)
{
	CHECK(fork->logical_size == UINT64_C(0x8123456789abcdef) + seed);
	CHECK(fork->total_blocks == seed);
	check_extents(fork->extents, seed + 0x100);
}

/* The volume header's scalar fields precede its five 80-byte forks. */
static void
test_header_and_forks(void)
{
	unsigned char p[512] = {0}, f[80] = {0}, e[64] = {0};
	hfsplus_header_t h;
	hfsplus_fork_t fork;
	hfsplus_extent_t extents[8];
	size_t i;

	put_be(p, 0x482b, 2);
	put_be(p + 2, 4, 2);
	for (i = 4; i < 72; i += 4)
		put_be(p + i, 0x10203000 + i, 4);
	put_be(p + 72, UINT64_C(0x8192a3b4c5d6e7f8), 8);
	for (i = 0; i < 8; i++)
		put_be(p + 80 + 4 * i, 0x90102030 + i, 4);
	for (i = 0; i < 5; i++)
		make_fork(p + 112 + 80 * i, 0x1000 + 0x200 * i);
	check_span(decode_header, p, 512, sizeof(h), false, "header");
	CHECK(hfsplus_header_decode(p, sizeof(p), &h));
	CHECK(h.signature == 0x482b && h.version == 4);
#define HEADER_FIELD(field, offset) CHECK(h.field == 0x10203000u + (offset))
	HEADER_FIELD(attributes, 4);
	HEADER_FIELD(last_mounted_version, 8);
	HEADER_FIELD(journal_info_block, 12);
	HEADER_FIELD(create_date, 16);
	HEADER_FIELD(modify_date, 20);
	HEADER_FIELD(backup_date, 24);
	HEADER_FIELD(checked_date, 28);
	HEADER_FIELD(file_count, 32);
	HEADER_FIELD(folder_count, 36);
	HEADER_FIELD(block_size, 40);
	HEADER_FIELD(total_blocks, 44);
	HEADER_FIELD(free_blocks, 48);
	HEADER_FIELD(next_allocation, 52);
	HEADER_FIELD(rsrc_clump_size, 56);
	HEADER_FIELD(data_clump_size, 60);
	HEADER_FIELD(next_catalog_id, 64);
	HEADER_FIELD(write_count, 68);
#undef HEADER_FIELD
	CHECK(h.encodings_bitmap == UINT64_C(0x8192a3b4c5d6e7f8));
	for (i = 0; i < 8; i++)
		case_check(h.finder_info[i] == 0x90102030u + i, "finder", i);
	check_fork(&h.allocation_file, 0x1000);
	check_fork(&h.extents_file, 0x1200);
	check_fork(&h.catalog_file, 0x1400);
	check_fork(&h.attributes_file, 0x1600);
	check_fork(&h.startup_file, 0x1800);
	make_fork(f, 0x20304050);
	check_span(decode_fork, f, 80, sizeof(fork), false, "fork");
	CHECK(hfsplus_fork_decode(f, sizeof(f), &fork));
	check_fork(&fork, 0x20304050);
	make_extents(e, 0x31425360);
	check_span(decode_extents, e, 64, sizeof(extents), false, "extents");
	CHECK(hfsplus_extents_decode(e, sizeof(e), extents));
	check_extents(extents, 0x31425360);
}

/* Node kinds are signed; the B-tree header has unaligned scalar fields. */
static void
test_tree_headers(void)
{
	unsigned char p[106] = {0}, n[14] = {0};
	hfsplus_node_t node;
	hfsplus_btree_t tree;
	static const int kinds[] = {-1, 0, 1, 2};
	size_t i;

	put_be(n, 0x12345678, 4);
	put_be(n + 4, 0x91abcdef, 4);
	n[9] = 0x52;
	put_be(n + 10, 0x6374, 2);
	for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
		n[8] = (unsigned char)kinds[i];
		check_span(decode_node, n, 14, sizeof(node), false, "node");
		CHECK(hfsplus_node_decode(n, sizeof(n), &node));
		case_check(node.kind == kinds[i], "signed node kind", i);
		CHECK(node.forward_link == 0x12345678);
		CHECK(node.backward_link == 0x91abcdef);
		CHECK(node.height == 0x52 && node.records == 0x6374);
	}
	put_be(p, 0x1020, 2);
	put_be(p + 2, 0x21324354, 4);
	put_be(p + 6, 0x32435465, 4);
	put_be(p + 10, 0x43546576, 4);
	put_be(p + 14, 0x54657687, 4);
	put_be(p + 18, 0x6576, 2);
	put_be(p + 20, 0x7687, 2);
	put_be(p + 22, 0x8798a9ba, 4);
	put_be(p + 26, 0x98a9bacb, 4);
	put_be(p + 32, 0xa9bacbdc, 4);
	p[36] = 0xba;
	p[37] = 0xcb;
	put_be(p + 38, 0xdcedfe0f, 4);
	check_span(decode_btree, p, 106, sizeof(tree), false, "btree");
	CHECK(hfsplus_btree_decode(p, sizeof(p), &tree));
	CHECK(tree.tree_depth == 0x1020 && tree.root_node == 0x21324354);
	CHECK(tree.leaf_records == 0x32435465);
	CHECK(tree.first_leaf_node == 0x43546576);
	CHECK(tree.last_leaf_node == 0x54657687);
	CHECK(tree.node_size == 0x6576 && tree.max_key_length == 0x7687);
	CHECK(tree.total_nodes == 0x8798a9ba && tree.free_nodes == 0x98a9bacb);
	CHECK(tree.clump_size == 0xa9bacbdc && tree.attributes == 0xdcedfe0f);
	CHECK(tree.btree_type == 0xba && tree.key_compare_type == 0xcb);
}

/* Key lengths frame the name; catalog thread keys alone permit zero units. */
static void
test_keys(void)
{
	unsigned char p[526] = {0}, e[12] = {0};
	hfsplus_ckey_t c;
	hfsplus_akey_t a;
	hfsplus_ekey_t ext;
	static const size_t counts[] = {0, 1, 127, 128, 255, 256};
	size_t i, j, n, size, fixed, limit, out_size;
	decode_t decode;

	put_be(e, 10, 2);
	put_be(e + 4, 0x89abcdef, 4);
	put_be(e + 8, 0x12345678, 4);
	for (i = 0; i < 2; i++) {
		e[2] = i == 0 ? 0 : 0xff;
		check_span(decode_ekey, e, 12, sizeof(ext), false, "ekey");
		CHECK(hfsplus_ekey_decode(e, 12, &ext));
		CHECK(ext.file_id == 0x89abcdef && ext.start_block == 0x12345678);
		CHECK(ext.fork_type == e[2]);
	}
	put_be(e, 9, 2);
	reject(decode_ekey, e, 12, sizeof(ext), "ekey length 9");
	put_be(e, 11, 2);
	reject(decode_ekey, e, 12, sizeof(ext), "ekey length 11");
	put_be(e, 10, 2);
	e[3] = 1;
	reject(decode_ekey, e, 12, sizeof(ext), "ekey pad");
	for (j = 0; j < 2; j++) {
		fixed = j == 0 ? 8 : 14;
		limit = j == 0 ? 255 : 127;
		decode = j == 0 ? decode_ckey : decode_akey;
		out_size = j == 0 ? sizeof(c) : sizeof(a);
		for (i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
			n = counts[i];
			size = fixed + 2 * n;
			bzero(p, sizeof(p));
			put_be(p, size - 2, 2);
			put_be(p + (j == 0 ? 2 : 4), 0x89abcdef, 4);
			if (j != 0)
				put_be(p + 8, 0x12345678, 4);
			put_be(p + fixed - 2, n, 2);
			for (size_t k = 0; k < n; k++)
				put_be(p + fixed + 2 * k, 0x3100 + k, 2);
			if (n > limit || (j != 0 && n == 0)) {
				reject(decode, p, size, out_size, "key unit bound");
				continue;
			}
			check_span(decode, p, size, out_size, false,
			    j == 0 ? "ckey" : "akey");
			if (j == 0) {
				case_check(hfsplus_ckey_decode(p, size, &c), "ckey fields", n);
				case_check(c.parent == 0x89abcdef && c.units == n,
				    "catalog key fields", n);
				case_check(c.name == p + 8, "catalog name pointer", n);
			} else {
				case_check(hfsplus_akey_decode(p, size, &a), "akey fields", n);
				case_check(a.file_id == 0x89abcdef && a.units == n,
				    "attribute key fields", n);
				case_check(a.start_block == 0x12345678 && a.name == p + 14,
				    "attribute key block and pointer", n);
			}
			put_be(p, size, 2);
			reject(decode, p, size + 2, out_size, "key length plus two");
			put_be(p, size - 4, 2);
			reject(decode, p, size, out_size, "key length minus two");
			put_be(p, 0xffff, 2);
			reject(decode, p, size, out_size, "key length ffff");
			put_be(p, size - 2, 2);
			put_be(p + fixed - 2, 0xffff, 2);
			reject(decode, p, size, out_size, "name length ffff");
		}
	}
}

/* Dates, BSD information and Finder bytes have the same catalog positions. */
static void
make_catalog(unsigned char *p, uint16_t type)
{
	size_t i;

	put_be(p, type, 2);
	put_be(p + 2, 0x1234, 2);
	for (i = 4; i < 40; i += 4)
		put_be(p + i, 0x21324300 + i, 4);
	p[40] = 0x51;
	p[41] = 0x62;
	put_be(p + 42, 0x7384, 2);
	put_be(p + 44, 0x95a6b7c8, 4);
	for (i = 0; i < 32; i++)
		p[48 + i] = (unsigned char)(0x80 + i);
	put_be(p + 80, 0xa1b2c3d4, 4);
	put_be(p + 84, 0xb2c3d4e5, 4);
}

static void
check_catalog(const hfsplus_dates_t *d, const hfsplus_bsd_t *b,
    const unsigned char *finder)
{
	size_t i;

	CHECK(d->created == 0x2132430c && d->content_modified == 0x21324310);
	CHECK(d->attribute_modified == 0x21324314);
	CHECK(d->accessed == 0x21324318 && d->backed_up == 0x2132431c);
	CHECK(b->owner_id == 0x21324320 && b->group_id == 0x21324324);
	CHECK(b->admin_flags == 0x51 && b->owner_flags == 0x62);
	CHECK(b->file_mode == 0x7384 && b->special == 0x95a6b7c8);
	for (i = 0; i < 32; i++)
		case_check(finder[i] == 0x80 + i, "catalog finder", i);
}

/* Folder, file and thread records require their exact type and length. */
static void
test_catalog_records(void)
{
	unsigned char p[249] = {0}, t[523] = {0};
	hfsplus_folder_t folder;
	hfsplus_file_t file;
	hfsplus_thread_t thread;
	uint16_t type;
	size_t i, n;
	static const size_t counts[] = {0, 1, 255};

	make_catalog(p, 1);
	check_span(decode_folder, p, 88, sizeof(folder), true, "folder");
	CHECK(hfsplus_folder_decode(p, 88, &folder));
	CHECK(folder.flags == 0x1234 && folder.valence == 0x21324304);
	CHECK(folder.id == 0x21324308 && folder.text_encoding == 0xa1b2c3d4);
	CHECK(folder.folder_count == 0xb2c3d4e5);
	check_catalog(&folder.dates, &folder.bsd, folder.finder_info);
	put_be(p, 2, 2);
	reject(decode_folder, p, 88, sizeof(folder), "wrong folder type");
	make_catalog(p, 2);
	make_fork(p + 88, 0x10111213);
	make_fork(p + 168, 0x20212223);
	check_span(decode_file, p, 248, sizeof(file), true, "file");
	CHECK(hfsplus_file_decode(p, 248, &file));
	CHECK(file.flags == 0x1234 && file.newest_link == 0x21324304);
	CHECK(file.id == 0x21324308 && file.text_encoding == 0xa1b2c3d4);
	check_catalog(&file.dates, &file.bsd, file.finder_info);
	check_fork(&file.data_fork, 0x10111213);
	check_fork(&file.resource_fork, 0x20212223);
	put_be(p, 1, 2);
	reject(decode_file, p, 248, sizeof(file), "wrong file type");
	check_span(decode_type, p, 2, sizeof(type), false, "catalog type");
	put_be(p, 0x91ab, 2);
	CHECK(hfsplus_catalog_type(p, 2, &type) && type == 0x91ab);
	for (type = 3; type <= 4; type++) {
		for (i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
			n = counts[i];
			put_be(t, type, 2);
			put_be(t + 4, 0x89abcdef, 4);
			put_be(t + 8, n, 2);
			for (size_t k = 0; k < n; k++)
				put_be(t + 10 + 2 * k, 0x4100 + k, 2);
			check_span(decode_thread, t, 10 + 2 * n,
			    sizeof(thread), true, "thread");
			case_check(hfsplus_thread_decode(t, 10 + 2 * n, &thread),
			    "thread decode", n);
			case_check(thread.parent == 0x89abcdef && thread.units == n,
			    "thread fields", n);
			case_check(thread.type == type && thread.name == t + 10,
			    "thread type and name", n);
			put_be(t, 2, 2);
			reject(decode_thread, t, 10 + 2 * n,
			    sizeof(thread), "wrong thread type");
		}
	}
	put_be(t, 3, 2);
	put_be(t + 8, 256, 2);
	reject(decode_thread, t, 522, sizeof(thread), "thread 256 units");
}

/* Inline sizes fit the span, and unknown attribute types expose only type. */
static void
test_attributes(void)
{
	unsigned char p[88] = {0};
	hfsplus_attr_t attr, expected;

	put_be(p, 0x10, 4);
	check_span(decode_attr, p, 16, sizeof(attr), false, "inline empty");
	CHECK(hfsplus_attr_decode(p, 16, &attr));
	CHECK(attr.type == 0x10 && attr.data_size == 0 && attr.data_offset == 16);
	put_be(p + 12, 3, 4);
	p[16] = 0x12;
	p[17] = 0x34;
	p[18] = 0x56;
	check_span(decode_attr, p, 19, sizeof(attr), false, "inline odd");
	CHECK(hfsplus_attr_decode(p, 20, &attr));
	CHECK(attr.type == 0x10 && attr.data_size == 3 && attr.data_offset == 16);
	put_be(p + 12, 5, 4);
	reject(decode_attr, p, 20, sizeof(attr), "inline one past");
	put_be(p + 12, 0xffffffff, 4);
	reject(decode_attr, p, 20, sizeof(attr), "inline ffffffff");
	put_be(p, 0x20, 4);
	make_fork(p + 8, 0x31323334);
	check_span(decode_attr, p, 88, sizeof(attr), false, "attribute fork");
	CHECK(hfsplus_attr_decode(p, 88, &attr));
	CHECK(attr.type == 0x20 && attr.data_size == 0 && attr.data_offset == 0);
	check_fork(&attr.fork, 0x31323334);
	put_be(p, 0x30, 4);
	make_extents(p + 8, 0x45464748);
	check_span(decode_attr, p, 72, sizeof(attr), false, "attribute extents");
	CHECK(hfsplus_attr_decode(p, 72, &attr));
	CHECK(attr.type == 0x30 && attr.data_size == 0 && attr.data_offset == 0);
	check_extents(attr.extents, 0x45464748);
	put_be(p, 0xabcdef01, 4);
	check_span(decode_attr, p, 4, sizeof(attr), false, "unknown attribute");
	memset(&attr, 0xa5, sizeof(attr));
	CHECK(hfsplus_attr_decode(p, 4, &attr));
	bzero(&expected, sizeof(expected));
	expected.type = 0xabcdef01;
	CHECK(bcmp(&attr, &expected, sizeof(attr)) == 0);
}

/* The wrapper and journal info block contain big-endian location fields. */
static void
test_locations(void)
{
	unsigned char j[180] = {0}, m[130] = {0};
	hfsplus_jib_t jib;
	hfsplus_mdb_t mdb;

	put_be(j, 0x12345678, 4);
	memset(j + 4, 0x93, 32);
	put_be(j + 36, UINT64_C(0x8192a3b4c5d6e7f8), 8);
	put_be(j + 44, UINT64_C(0x92a3b4c5d6e7f809), 8);
	check_span(decode_jib, j, 180, sizeof(jib), false, "jib");
	CHECK(hfsplus_jib_decode(j, 180, &jib));
	CHECK(jib.flags == 0x12345678);
	CHECK(jib.offset == UINT64_C(0x8192a3b4c5d6e7f8));
	CHECK(jib.size == UINT64_C(0x92a3b4c5d6e7f809));
	put_be(m, 0x4244, 2);
	put_be(m + 0x14, 0x12345678, 4);
	put_be(m + 0x1c, 0x91ab, 2);
	put_be(m + 0x7c, 0x482b, 2);
	put_be(m + 0x7e, 0xa2bc, 2);
	put_be(m + 0x80, 0xb3cd, 2);
	check_span(decode_mdb, m, 130, sizeof(mdb), false, "mdb");
	CHECK(hfsplus_mdb_decode(m, 130, &mdb));
	CHECK(mdb.signature == 0x4244 && mdb.embedded_signature == 0x482b);
	CHECK(mdb.allocation_block_size == 0x12345678);
	CHECK(mdb.first_block_sector == 0x91ab);
	CHECK(mdb.embedded_start == 0xa2bc && mdb.embedded_count == 0xb3cd);
}

/* Journal checksum covers 44 bytes, treating its own word as zero. */
static uint32_t
journal_checksum(const unsigned char *p)
{
	uint32_t s;
	size_t i;
	unsigned char c;

	s = 0;
	for (i = 0; i < 44; i++) {
		c = i >= 36 && i < 40 ? 0 : p[i];
		s = (s << 8) ^ (s + c);
	}
	return (~s);
}

/* Both journal byte orders require matching identity and a valid checksum. */
static void
test_journals(void)
{
	unsigned char p[44], bad[44];
	hfsplus_journal_t journal;
	void (*put)(unsigned char *, uint64_t, size_t);
	size_t i, order;
	static const struct {
		const char *name;
		size_t offset;
	} fields[] = {
		{ .name = "journal flipped magic", .offset = 0 },
		{ .name = "journal flipped endian", .offset = 4 },
		{ .name = "journal flipped start", .offset = 8 },
		{ .name = "journal flipped end", .offset = 16 },
		{ .name = "journal flipped size", .offset = 24 },
		{ .name = "journal flipped blhdr_size", .offset = 32 },
		{ .name = "journal flipped checksum", .offset = 36 },
		{ .name = "journal flipped jhdr_size", .offset = 40 }
	};

	for (order = 0; order < 2; order++) {
		put = order == 0 ? put_be : put_le;
		bzero(p, sizeof(p));
		put(p, 0x4a4e4c78, 4);
		put(p + 4, 0x12345678, 4);
		put(p + 8, UINT64_C(0x0123456789abcdef), 8);
		put(p + 16, UINT64_C(0x123456789abcdef0), 8);
		put(p + 24, UINT64_C(0x23456789abcdef01), 8);
		put(p + 32, 0x3456789a, 4);
		put(p + 40, 0x456789ab, 4);
		/* Word-boundary sums from the byte recurrence, big-endian:
		 * 4a98375c 78665070 79821f40 cfb59e30 bdf35744 202c2a68
		 * c3972dc0 6a6d9a28 1ef677c4 1ef677c4 dc4d3fa4.
		 * Little-endian ends at 3b23d7a4. Complement the final sum.
		 */
		case_check(journal_checksum(p) ==
		    (order == 0 ? 0x23b2c05bu : 0xc4dc285bu),
		    "journal checksum known answer", order);
		put(p + 36, journal_checksum(p), 4);
		check_span(decode_journal, p, 44, sizeof(journal), false,
		    order == 0 ? "journal BE" : "journal LE");
		CHECK(hfsplus_journal_decode(p, 44, &journal));
		CHECK(journal.little_endian == (order != 0));
		CHECK(journal.start == UINT64_C(0x0123456789abcdef));
		CHECK(journal.end == UINT64_C(0x123456789abcdef0));
		CHECK(journal.size == UINT64_C(0x23456789abcdef01));
		CHECK(journal.blhdr_size == 0x3456789a);
		CHECK(journal.jhdr_size == 0x456789ab);
		for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
			bcopy(p, bad, sizeof(p));
			bad[fields[i].offset] ^= 1;
			reject(decode_journal, bad, 44, sizeof(journal),
			    fields[i].name);
		}
		bcopy(p, bad, sizeof(p));
		(order == 0 ? put_le : put_be)(bad + 4, 0x12345678, 4);
		put(bad + 36, journal_checksum(bad), 4);
		reject(decode_journal, bad, 44, sizeof(journal),
		    "journal mixed byte order");
		bcopy(p, bad, sizeof(p));
		(order == 0 ? put_le : put_be)(bad, 0x4a4e4c78, 4);
		put(bad + 36, journal_checksum(bad), 4);
		reject(decode_journal, bad, 44, sizeof(journal),
		    "journal reversed magic only");
		/* Valid checksums isolate recognition from checksum rejection. */
		bcopy(p, bad, sizeof(p));
		put(bad, 0x4a4e4c79, 4);
		put(bad + 36, journal_checksum(bad), 4);
		reject(decode_journal, bad, 44, sizeof(journal),
		    "journal wrong magic with valid checksum");
		bcopy(p, bad, sizeof(p));
		put(bad + 4, 0x12345679, 4);
		put(bad + 36, journal_checksum(bad), 4);
		reject(decode_journal, bad, 44, sizeof(journal),
		    "journal wrong endian with valid checksum");
	}
}

/* Compression framing mixes little-endian metadata with resource BE words. */
static void
test_compression(void)
{
	unsigned char p[24] = {0}, bad[16];
	hfsplus_cmpf_t cmpf;
	hfsplus_rsrc_t rsrc;
	hfsplus_rsrc_table_t table;
	hfsplus_chunk_t chunk, before;
	uint32_t word, old;
	size_t i;
	static const uint32_t indices[] = {0, 1, 2, UINT32_MAX};

	bcopy("fpmc", p, 4);
	put_le(p + 4, 0x12345678, 4);
	put_le(p + 8, UINT64_C(0x7fffffffffffffff), 8);
	check_span(decode_cmpf, p, 16, sizeof(cmpf), false, "cmpf");
	CHECK(hfsplus_cmpf_decode(p, 16, &cmpf));
	CHECK(cmpf.type == 0x12345678);
	CHECK(cmpf.uncompressed_size == UINT64_C(0x7fffffffffffffff));
	for (i = 0; i < 4; i++) {
		bcopy(p, bad, 16);
		bad[i] ^= 1;
		reject(decode_cmpf, bad, 16, sizeof(cmpf), "cmpf magic byte");
	}
	put_le(p + 8, UINT64_C(0x8000000000000000), 8);
	reject(decode_cmpf, p, 16, sizeof(cmpf), "cmpf signed size bound");
	put_be(p, 0x12345678, 4);
	put_be(p + 4, 0x23456789, 4);
	put_be(p + 8, 0x3456789a, 4);
	put_be(p + 12, 0x456789ab, 4);
	check_span(decode_rsrc, p, 16, sizeof(rsrc), false, "resource");
	CHECK(hfsplus_rsrc_decode(p, 16, &rsrc));
	CHECK(rsrc.data_offset == 0x12345678 && rsrc.map_offset == 0x23456789);
	CHECK(rsrc.data_length == 0x3456789a && rsrc.map_length == 0x456789ab);
	put_le(p + 4, 0x89abcdef, 4);
	check_span(decode_rsrc_table, p, 8, sizeof(table), false, "resource table");
	CHECK(hfsplus_rsrc_table_decode(p, 8, &table));
	CHECK(table.resource_length == 0x12345678 && table.chunk_count == 0x89abcdef);
	put_le(p + 8, 0x12345678, 4);
	put_le(p + 12, 0x23456789, 4);
	put_le(p + 16, 0x3456789a, 4);
	put_le(p + 20, 0x456789ab, 4);
	/* The entry array starts after the separately decoded table header. */
	check_span(decode_pair, p + 8, 8, sizeof(chunk), false, "chunk pair");
	check_span(decode_word, p + 8, 4, sizeof(word), false, "chunk word");
	for (i = 0; i < sizeof(indices) / sizeof(indices[0]); i++) {
		memset(&chunk, 0xa5, sizeof(chunk));
		bcopy(&chunk, &before, sizeof(chunk));
		word = old = 0xa5a5a5a5;
		if (indices[i] < 2) {
			case_check(hfsplus_chunk_pair_decode(p + 8, 16,
			    indices[i], &chunk), "pair decode", indices[i]);
			case_check(chunk.offset ==
			    (indices[i] == 0 ? 0x12345678u : 0x3456789au),
			    "pair offset LE", indices[i]);
			case_check(chunk.length ==
			    (indices[i] == 0 ? 0x23456789u : 0x456789abu),
			    "pair length LE", indices[i]);
			case_check(hfsplus_chunk_word_decode(p + 8, 8,
			    indices[i], &word), "word decode", indices[i]);
			case_check(word ==
			    (indices[i] == 0 ? 0x12345678u : 0x23456789u),
			    "word value LE", indices[i]);
		} else {
			case_check(!hfsplus_chunk_pair_decode(p + 8, 16, indices[i], &chunk),
			    "pair index", indices[i]);
			case_check(bcmp(&chunk, &before, sizeof(chunk)) == 0,
			    "pair failure atomicity", indices[i]);
			case_check(!hfsplus_chunk_word_decode(p + 8, 8, indices[i], &word),
			    "word index", indices[i]);
			case_check(word == old, "word failure atomicity", indices[i]);
		}
	}
}

int
main(void)
{
	test_header_and_forks();
	test_tree_headers();
	test_keys();
	test_catalog_records();
	test_attributes();
	test_locations();
	test_journals();
	test_compression();
	return (0);
}
