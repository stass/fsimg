/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "test.h"
#include "hfsplus-write-check.h"

/* Offsets are from the volume, fork, node and catalog tables in the design.
 * Only fields explicitly checked below may contain nonzero bytes. The final
 * scan therefore covers free blocks, boot areas, node slack, unused nodes,
 * fork tails and reserved fields with a single rule.
 */
typedef struct raw_check {
	const unsigned char *bytes;
	unsigned char *owned;
	uint64_t length;
	uint32_t block;
	uint32_t blocks;
	uint32_t prefix;
	uint32_t tail;
	uint32_t next;
	unsigned variant;
	const hwf_fixture_t *fixture;
} raw_check_t;

typedef struct primary {
	uint32_t id;
	uint32_t parent;
	const unsigned char *name;
	unsigned units;
	const unsigned char *data;
} primary_t;

static uint16_t
u16(const unsigned char *p)
{
	return ((uint16_t)((uint16_t)p[0] << 8 | p[1]));
}

static uint32_t
u32(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	    (uint32_t)p[2] << 8 | p[3]);
}

static uint64_t
u64(const unsigned char *p)
{
	return ((uint64_t)u32(p) << 32 | u32(p + 4));
}

static void
own(raw_check_t *c, const unsigned char *p, uint64_t n)
{
	uint64_t offset = (uint64_t)(p - c->bytes);

	CHECK(offset <= c->length && n <= c->length - offset);
	memset(c->owned + offset, 1, (size_t)n);
}

static void
zero(const unsigned char *p, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++)
		CHECK(p[i] == 0);
}

static uint64_t
round_blocks(uint64_t n, uint32_t block)
{
	return (n / block + (n % block != 0));
}

/* Claim forks in layout order; this proves both contiguity and disjointness.
 * Empty data forks consume no space and have an entirely zero descriptor.
 */
static const unsigned char *
fork_at(raw_check_t *c, const unsigned char *p, bool metadata)
{
	uint64_t length = u64(p);
	uint32_t blocks = u32(p + 12), first = u32(p + 16);

	own(c, p, 24);
	zero(p + 24, 56);
	CHECK(blocks == u32(p + 20));
	CHECK(blocks == round_blocks(length, c->block));
	if (blocks == 0) {
		zero(p, 80);
		return (NULL);
	}
	CHECK(first == c->next);
	CHECK(first >= round_blocks(1536, c->block));
	CHECK(first <= c->prefix && blocks <= c->prefix - first);
	c->next += blocks;
	if (metadata)
		CHECK(u32(p + 8) == length);
	else
		CHECK(u32(p + 8) == 0);
	return (c->bytes + (uint64_t)first * c->block);
}

static unsigned
offset_at(const unsigned char *node, unsigned size, unsigned index)
{
	return (u16(node + size - 2 * (index + 1)));
}

/* Fixture indices in catalog order, derived from the design's UTF-16 rules.
 * Folded alpha < Zeta < long z; binary Zeta precedes lowercase alpha.
 * Greek alpha folds before capital omega; binary capital omega comes first.
 * The colon is stored as slash. U+2400 is stored as zero: folded zero ranks
 * last, binary zero first. The emoji's D83D DE00 precedes fullwidth A (FF21,
 * folded FF41) in both orders: comparison is by units, not Unicode scalars.
 * U+200C is ignored when folding, so a<U+200C>x precedes ay; binary reverses
 * that pair. The other names retain their lowercase ASCII order.
 */
static const unsigned root_order[2][HWF_COUNT - 3] = {
	{ 18, 12, 5, 15, 22, 23, 8, 0, 2, 4, 7, 9, 10, 6,
	    14, 13, 17, 16, 20, 21, 19 },
	{ 19, 18, 14, 12, 5, 15, 23, 22, 8, 0, 2, 4, 7, 9,
	    10, 6, 13, 16, 17, 20, 21 }
};

static void name_text(const unsigned char *, unsigned, char *, size_t);

static unsigned
root_position(raw_check_t *c, const unsigned char *key)
{
	char name[768];

	if (u16(key + 6) == 0)
		return (0); /* The root thread precedes all its children. */
	name_text(key + 8, u16(key + 6), name, sizeof(name));
	for (unsigned i = 0; i < HWF_COUNT - 3; i++)
		if (strcmp(name, c->fixture->entries[root_order[c->variant][i]].listed + 1) == 0)
			return (i + 1);
	CHECK(false);
	return (0);
}

static int
key_compare(raw_check_t *c, const unsigned char *a, const unsigned char *b)
{
	uint32_t ap = u32(a + 2), bp = u32(b + 2);
	unsigned an = u16(a + 6), bn = u16(b + 6);

	if (ap != bp)
		return (ap < bp ? -1 : 1);
	if (ap == 2 && c->variant == 0) {
		unsigned ar = root_position(c, a), br = root_position(c, b);

		return (ar < br ? -1 : ar != br);
	}
	for (unsigned i = 0; i < an && i < bn; i++) {
		unsigned ac = u16(a + 8 + 2 * i);
		unsigned bc = u16(b + 8 + 2 * i);

		if (ac != bc)
			return (ac < bc ? -1 : 1);
	}
	return (an < bn ? -1 : an != bn);
}

static void
tree(raw_check_t *c, const unsigned char *base, uint64_t length,
    unsigned size, unsigned maxkey, unsigned compare, unsigned flags)
{
	const unsigned char *h = base + 14, *previous[17] = { NULL };
	uint32_t last[17] = { 0 }, first[17] = { 0 }, referenced[17] = { 0 };
	uint32_t total = u32(h + 22), free_nodes = u32(h + 26);
	uint32_t used = total - free_nodes, leaf_records = 0;
	uint32_t first_map = u32(base), maps = 0;
	unsigned highest = 0, last_height = 1;

	CHECK(length == (uint64_t)total * size && free_nodes <= total);
	CHECK(base[8] == 1 && base[9] == 0 && u16(base + 10) == 3);
	CHECK(u32(base + 4) == 0);
	CHECK(offset_at(base, size, 0) == 14);
	CHECK(offset_at(base, size, 1) == 120);
	CHECK(offset_at(base, size, 2) == 248);
	CHECK(offset_at(base, size, 3) == size - 8);
	CHECK(u16(h + 18) == size && u16(h + 20) == maxkey);
	CHECK(u32(h + 32) == length && h[36] == 0 && h[37] == compare);
	CHECK(u32(h + 38) == flags);
	own(c, base, 12);
	own(c, base + size - 8, 8);
	own(c, h, 30);
	own(c, h + 32, 10);
	zero(h + 30, 2);
	zero(h + 42, 64);
	zero(base + 120, 128);
	/* Small fixtures have no map node. Map-chain coverage is also checked
	 * below, so a larger fixture can use the same oracle.
	 */
	{
		const unsigned char *map = base + 248;
		uint32_t map_node = u32(base);
		uint64_t bit = 0;
		unsigned bytes = size - 256;

		for (;;) {
			own(c, map, bytes);
			for (unsigned i = 0; i < bytes * 8; i++, bit++)
				CHECK(((map[i / 8] >> (7 - i % 8)) & 1) ==
				    (bit < used));
			if (map_node == 0)
				break;
			CHECK(map_node < used && map_node < total);
			CHECK(map_node == first_map + maps);
			maps++;
			map = base + (uint64_t)map_node * size;
			CHECK(map[8] == 2 && map[9] == 0);
			CHECK(u32(map + 4) == 0 && u16(map + 10) == 1);
			CHECK(offset_at(map, size, 0) == 14);
			CHECK(offset_at(map, size, 1) == size - 6);
			own(c, map, 12);
			own(c, map + size - 4, 4);
			map_node = u32(map);
			map += 14;
			bytes = size - 20;
			CHECK(bit < total); /* Reject a redundant or looping map. */
		}
		CHECK(bit >= total);
		CHECK(first_map == 0 || first_map + maps == used);
	}
	for (uint32_t n = 1; n < used; n++) {
		const unsigned char *node = base + (uint64_t)n * size;
		unsigned height = node[9], records = u16(node + 10);
		unsigned end = offset_at(node, size, records);

		if (node[8] == 2) {
			CHECK(first_map != 0 && n >= first_map);
			continue;
		}
		CHECK(first_map == 0 || n < first_map);
		CHECK(height >= last_height && height <= last_height + 1);
		CHECK(height <= 16 && records > 0);
		CHECK(node[8] == (height == 1 ? 255 : 0));
		CHECK(offset_at(node, size, 0) == 14);
		CHECK(end <= size - 2 * (records + 1));
		own(c, node, 12);
		own(c, node + size - 2 * (records + 1), 2 * (records + 1));
		if (first[height] == 0)
			first[height] = n;
		CHECK(u32(node + 4) == last[height]);
		if (last[height] != 0)
			CHECK(u32(base + (uint64_t)last[height] * size) == n);
		last[height] = n;
		last_height = height;
		highest = height;
		for (unsigned r = 0; r < records; r++) {
			unsigned start = offset_at(node, size, r);
			unsigned stop = offset_at(node, size, r + 1);
			const unsigned char *key = node + start;
			unsigned keybytes = u16(key) + 2;

			CHECK(start < stop && start % 2 == 0 && stop % 2 == 0);
			CHECK(keybytes <= stop - start && keybytes == 8 + 2 * u16(key + 6));
			if (previous[height] != NULL)
				CHECK(key_compare(c, previous[height], key) < 0);
			previous[height] = key;
			own(c, key, keybytes);
			if (height == 1) {
				leaf_records++;
			} else {
				uint32_t child = u32(key + keybytes);
				const unsigned char *cn;

				CHECK(stop - start == keybytes + 4);
				CHECK(child > 0 && child < n);
				/* Nodes are emitted consecutively within each level. */
				CHECK(child == first[height - 1] + referenced[height - 1]);
				referenced[height - 1]++;
				cn = base + (uint64_t)child * size;
				CHECK(cn[9] + 1 == height);
				CHECK(u16(cn + 14) + 2 == keybytes);
				CHECK(bcmp(key, cn + 14, keybytes) == 0);
				own(c, key + keybytes, 4);
			}
		}
		zero(node + end, size - 2 * (records + 1) - end);
	}
	for (unsigned height = 1; height <= highest; height++) {
		CHECK(u32(base + (uint64_t)last[height] * size) == 0);
		if (height < highest)
			CHECK(referenced[height] == last[height] - first[height] + 1);
	}
	CHECK(leaf_records == u32(h + 6));
	CHECK(u16(h) == highest);
	CHECK(u32(h + 2) == (highest == 0 ? 0 : first[highest]));
	CHECK(first[highest] == last[highest]);
	CHECK(u32(h + 10) == first[1] && u32(h + 14) == last[1]);
}

/* A standalone binary catalog lets mutation tests use the smallest index
 * shape without requiring the volume fixture's complete namespace.
 */
void
hwc_check_tree(const unsigned char *bytes, uint64_t length)
{
	raw_check_t c = { .bytes = bytes, .length = length, .variant = 1 };

	c.owned = calloc((size_t)length, 1);
	CHECK(c.owned != NULL);
	tree(&c, bytes, length, 4096, 516, 0xbc, 6);
	free(c.owned);
}

static void
name_text(const unsigned char *p, unsigned units, char *out, size_t capacity)
{
	size_t n = 0;

	for (unsigned i = 0; i < units; i++) {
		unsigned ch = u16(p + 2 * i);

		CHECK(n + 4 < capacity);
		if (ch == 0)
			ch = 0x2400;
		else if (ch == '/')
			ch = ':';
		else if (ch >= 0xd800 && ch <= 0xdbff) {
			unsigned low;

			CHECK(i + 1 < units);
			low = u16(p + 2 * ++i);
			CHECK(low >= 0xdc00 && low <= 0xdfff);
			ch = 0x10000 + ((ch - 0xd800) << 10) + low - 0xdc00;
		}
		if (ch < 128)
			out[n++] = (char)ch;
		else {
			if (ch >= 0x10000) {
				out[n++] = (char)(0xf0 | ch >> 18);
				out[n++] = (char)(0x80 | ((ch >> 12) & 63));
			} else if (ch >= 0x800)
				out[n++] = (char)(0xe0 | ch >> 12);
			else {
				out[n++] = (char)(0xc0 | ch >> 6);
				out[n++] = (char)(0x80 | (ch & 63));
				continue;
			}
			out[n++] = (char)(0x80 | ((ch >> 6) & 63));
			out[n++] = (char)(0x80 | (ch & 63));
		}
	}
	out[n] = '\0';
}

static void
catalog(raw_check_t *c, const unsigned char *base, const hwf_fixture_t *f,
    bool empty, bool reproducible)
{
	primary_t prim[HWF_COUNT + 1] = { 0 };
	primary_t threads[HWF_COUNT + 1] = { 0 };
	unsigned count = 0, tcount = 0, files = 0, folders = 0, roots = 0;
	uint32_t node = u32(base + 14 + 10), maxid = 2;
	bool seen[HWF_COUNT] = { false };

	while (node != 0) {
		const unsigned char *p = base + (uint64_t)node * 4096;

		for (unsigned r = 0; r < u16(p + 10); r++) {
			const unsigned char *k = p + offset_at(p, 4096, r);
			const unsigned char *d = k + u16(k) + 2;
			unsigned type = u16(d), end = offset_at(p, 4096, r + 1);

			CHECK(type >= 1 && type <= 4);
			if (type <= 2) {
				CHECK(count < HWF_COUNT + 1);
				CHECK(p + end - d == (type == 1 ? 88 : 248));
				if (u32(k + 2) == 2) {
					roots++;
					CHECK(root_position(c, k) == roots);
				}
				prim[count++] = (primary_t){ u32(d + 8),
				    u32(k + 2), k + 8, u16(k + 6), d };
			} else {
				CHECK(tcount < HWF_COUNT + 1 && u16(k + 6) == 0);
				CHECK(p + end - d == 10 + 2 * u16(d + 8));
				threads[tcount++] = (primary_t){ u32(k + 2),
				    u32(d + 4), d + 10, u16(d + 8), d };
				own(c, d, 2);
				own(c, d + 4, 6 + 2 * u16(d + 8));
			}
		}
		node = u32(p);
	}
	CHECK(count == (empty ? 1 : HWF_COUNT + 1) && tcount == count);
	/* ID order is independent of the tree's parent/name order. */
	for (unsigned i = 0; i < count; i++) {
		unsigned at = i;
		for (unsigned j = i + 1; j < count; j++)
			if (prim[j].id < prim[at].id)
				at = j;
		primary_t tmp = prim[i];
		prim[i] = prim[at];
		prim[at] = tmp;
	}
	for (unsigned i = 0; i < count; i++) {
		const unsigned char *d = prim[i].data;
		unsigned type = u16(d), mode = u16(d + 42);
		unsigned children = 0, childfolders = 0, matches = 0;
		char name[768];
		bool root = prim[i].id == 2;
		unsigned source = HWF_COUNT;

		CHECK(prim[i].id == (i == 0 ? 2 : 15 + i));
		maxid = prim[i].id;
		name_text(prim[i].name, prim[i].units, name, sizeof(name));
		if (root) {
			CHECK(prim[i].parent == 1 && type == 1);
			CHECK(strcmp(name, HWF_LABEL) == 0);
			CHECK(u32(d + 32) == 0 && u32(d + 36) == 0);
			CHECK(mode == 0040755);
		} else {
			for (unsigned j = 0; j < HWF_COUNT; j++) {
				const char *leaf = strrchr(f->entries[j].listed, '/') + 1;
				if (strcmp(leaf, name) == 0) {
					CHECK(!seen[j]);
					seen[j] = true;
					source = j;
				}
			}
			CHECK(source < HWF_COUNT);
			{
				const char *path = f->entries[source].listed;
				const char *slash = strrchr(path, '/');
				bool parent_found = false;

				for (unsigned j = 0; j < count; j++) {
					char parent_name[768];
					const char *start = slash;

					if (prim[j].id != prim[i].parent)
						continue;
					CHECK(u16(prim[j].data) == 1);
					if (slash == path)
						CHECK(prim[j].id == 2);
					else {
						while (start > path && start[-1] != '/')
							start--;
						name_text(prim[j].name, prim[j].units,
						    parent_name, sizeof(parent_name));
						CHECK(strlen(parent_name) == (size_t)(slash - start));
						CHECK(strncmp(start, parent_name, (size_t)(slash - start)) == 0);
					}
					parent_found = true;
				}
				CHECK(parent_found);
			}
			CHECK(u32(d + 32) == 123 && u32(d + 36) == 456);
			CHECK((mode & 07777) == 0751);
			CHECK((mode & 0170000) ==
			    (f->entries[source].kind == FSIMG_ENTRY_DIRECTORY ? 0040000 :
			    f->entries[source].kind == FSIMG_ENTRY_SYMLINK ? 0120000 :
			    f->entries[source].kind == FSIMG_ENTRY_FIFO ? 0010000 : 0100000));
		}
		for (unsigned j = 0; j < count; j++) {
			if (threads[j].id == prim[i].id) {
				CHECK(u16(threads[j].data) == type + 2);
				CHECK(threads[j].parent == prim[i].parent);
				CHECK(threads[j].units == prim[i].units);
				CHECK(bcmp(threads[j].name, prim[i].name, 2 * prim[i].units) == 0);
				matches++;
			}
			if (prim[j].parent == prim[i].id) {
				children++;
				childfolders += u16(prim[j].data) == 1;
			}
		}
		CHECK(matches == 1);
		CHECK(u16(d + 2) == (type == 2 ? 2 : c->variant ? 16 : 0));
		own(c, d, 4);
		own(c, d + 8, 20);
		own(c, d + 32, 8);
		own(c, d + 42, 6);
		CHECK(u32(d + 44) == 1);
		for (unsigned date = 0; date < 4; date++) {
			uint32_t expected = reproducible || root ?
			    u32(c->bytes + 1024 + 16) :
			    (uint32_t)(2082844800u + HWF_EPOCH - (4 - date));

			CHECK(u32(d + 12 + 4 * date) == expected);
		}
		if (type == 1) {
			folders++;
			own(c, d + 4, 4);
			CHECK(u32(d + 4) == children);
			CHECK(u32(d + 84) == (c->variant ? childfolders : 0));
			own(c, d + 84, 4);
		} else {
			const unsigned char *payload;
			files++;
			if ((mode & 0170000) == 0120000) {
				CHECK(bcmp(d + 48, "slnkrhap", 8) == 0);
				own(c, d + 48, 8);
			}
			CHECK(u64(d + 88) == f->entries[source].length);
			payload = fork_at(c, d + 88, false);
			zero(d + 168, 80);
			if (payload != NULL) {
				own(c, payload, f->entries[source].length);
				for (uint64_t b = 0; b < f->entries[source].length; b++)
					CHECK(payload[b] == hwf_byte(source + 1, b));
			}
		}
	}
	CHECK(u32(c->bytes + 1024 + 32) == files);
	CHECK(u32(c->bytes + 1024 + 36) == folders - 1);
	CHECK(u32(c->bytes + 1024 + 64) == (maxid == 2 ? 16 : maxid + 1));
}

void
hwc_check(const hwf_image_t *image, const hwf_fixture_t *fixture,
    const fsimg_write_options_t *options, unsigned variant, bool empty)
{
	raw_check_t c = { .bytes = image->bytes, .length = image->length,
	    .variant = variant, .fixture = fixture };
	const unsigned char *h = c.bytes + 1024, *bitmap, *extents, *attrs, *cat;
	uint64_t bitmap_length;
	uint32_t clump;

	fprintf(stderr, "raw check variant=%u B=%u V=%" PRIu64 "\n",
	    variant, u32(h + 40), c.length);
	CHECK(c.length >= 2560 && c.length % 512 == 0);
	CHECK(u16(h) == (variant ? 0x4858 : 0x482b));
	CHECK(u16(h + 2) == (variant ? 5 : 4));
	CHECK(u32(h + 4) == 0x80000100 && u32(h + 8) == 0x6673696d);
	CHECK(u32(h + 12) == 0);
	CHECK(bcmp(h, c.bytes + c.length - 1024, 512) == 0);
	c.block = u32(h + 40);
	c.blocks = u32(h + 44);
	CHECK(c.block >= 512 && c.block <= 65536 &&
	    (c.block & (c.block - 1)) == 0);
	CHECK(c.blocks == c.length / c.block);
	/* Writers reserve the last ceil(1024 / B) blocks below N at every size. */
	c.tail = (uint32_t)round_blocks(1024, c.block);
	c.prefix = u32(h + 52);
	CHECK(c.prefix + c.tail <= c.blocks);
	CHECK(u32(h + 48) == c.blocks - c.prefix - c.tail);
	clump = c.block < 4096 ? 16 * c.block : 65536;
	CHECK(u32(h + 56) == clump && u32(h + 60) == clump);
	CHECK(u64(h + 72) == 1);
	CHECK(bcmp(h + 104, options->volume_uuid, 8) == 0);
	if ((options->flags & FSIMG_WRITE_REPRODUCIBLE) != 0)
		CHECK(u32(h + 16) == 2082844800u + HWF_EPOCH);
	CHECK(u32(h + 20) == u32(h + 16));
	CHECK(u32(h + 28) == u32(h + 16));
	c.owned = calloc((size_t)c.length, 1);
	CHECK(c.owned != NULL);
	own(&c, h, 24);
	own(&c, h + 28, 40);
	own(&c, h + 72, 8);
	own(&c, h + 104, 8);
	c.next = (uint32_t)round_blocks(1536, c.block);
	bitmap_length = u64(h + 112);
	bitmap = fork_at(&c, h + 112, true);
	CHECK(bitmap_length == round_blocks(round_blocks(c.blocks, 8), c.block) * c.block);
	extents = fork_at(&c, h + 192, true);
	attrs = fork_at(&c, h + 352, true);
	cat = fork_at(&c, h + 272, true);
	zero(h + 432, 80);
	tree(&c, extents, u64(h + 192), 4096, 10, 0, 2);
	tree(&c, attrs, u64(h + 352), 8192, 266, 0, 6);
	tree(&c, cat, u64(h + 272), 4096, 516, variant ? 0xbc : 0xcf, 6);
	catalog(&c, cat, fixture, empty,
	    (options->flags & FSIMG_WRITE_REPRODUCIBLE) != 0);
	CHECK(c.next == c.prefix);
	own(&c, bitmap, bitmap_length);
	for (uint64_t bit = 0; bit < bitmap_length * 8; bit++)
		CHECK(((bitmap[bit / 8] >> (7 - bit % 8)) & 1) ==
		    (bit < c.prefix || (bit < c.blocks && bit >= c.blocks - c.tail)));
	/* The alternate has exactly the same owned fields as the primary. */
	bcopy(c.owned + 1024, c.owned + c.length - 1024, 512);
	for (uint64_t i = 0; i < c.length; i++) {
		if (c.owned[i] == 0 && c.bytes[i] != 0) {
			fprintf(stderr, "nonzero unowned byte at %" PRIu64 ": %02x\n",
			    i, c.bytes[i]);
			CHECK(c.bytes[i] == 0);
		}
	}
	free(c.owned);
}
