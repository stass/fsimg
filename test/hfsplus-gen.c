/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdlib.h>
#include <strings.h>

#include "hfsplus-gen.h"
#include "test.h"

/* A level is a sequence of nodes, each paired with its first key. Building
 * the next level needs only those pairs. Leaves are packed in input order;
 * the caller supplies sorted catalog children, with IDs at least 16.
 */
typedef struct node_ref {
	const gen_record_t *first;
	uint32_t number;
} node_ref_t;

typedef struct packed_tree {
	unsigned char	*bytes;
	uint32_t	 capacity;
	uint32_t	 used;
	gen_tree_t	*tree;
} packed_tree_t;

static void encode_fork(unsigned char *, uint64_t, uint32_t, uint32_t);
static gen_record_t catalog_key(uint32_t, const uint16_t *, size_t);
static gen_record_t folder_record(const gen_plan_t *);
static gen_record_t thread_record(const gen_plan_t *);
static gen_record_t file_record(const gen_child_t *, uint32_t, uint32_t,
    uint32_t);
static void overflow_records(gen_tree_t *, uint32_t, uint8_t, uint32_t,
    uint32_t);
static void add_record(gen_tree_t *, const gen_record_t *);
static unsigned char *new_node(packed_tree_t *);
static void pack_tree(const gen_plan_t *, gen_tree_id_t, gen_tree_t *,
    unsigned char **);
static uint32_t allocate_blocks(uint32_t *, uint64_t, uint32_t);
static uint64_t tree_position(const gen_image_t *, gen_tree_id_t, uint64_t);

void
gen_store(unsigned char *p, uint64_t value, size_t width)
{
	CHECK(width <= 8);
	for (size_t i = 0; i < width; i++)
		p[width - 1 - i] = (unsigned char)(value >> (8 * i));
}

uint64_t
gen_load(const unsigned char *p, size_t width)
{
	uint64_t value = 0;

	CHECK(width <= 8);
	for (size_t i = 0; i < width; i++)
		value = (value << 8) | p[i];
	return (value);
}

void
gen_plan_init(gen_plan_t *plan)
{
	static const uint16_t label[] = { 'T', 'e', 's', 't' };

	bzero(plan, sizeof(*plan));
	plan->block_size = 4096;
	plan->total_blocks = 512;
	plan->node_size[GEN_EXTENTS] = 512;
	plan->node_size[GEN_CATALOG] = 4096;
	plan->node_size[GEN_ATTRIBUTES] = 4096;
	plan->compare_type = 0xcf;
	plan->label = label;
	plan->label_units = 4;
	plan->journal_file = true;
	plan->journal_flags = 1;
	plan->journal_size = 262144;
	plan->journal_start = 512;
	plan->journal_end = 512;
}

static void
encode_fork(unsigned char *p, uint64_t size, uint32_t first, uint32_t blocks)
{
	gen_store(p, size, 8);
	gen_store(p + 12, blocks, 4);
	gen_store(p + 16, first, 4);
	gen_store(p + 20, blocks, 4);
}

static gen_record_t
catalog_key(uint32_t parent, const uint16_t *name, size_t units)
{
	gen_record_t r = { .key_length = 8 + 2 * units };

	CHECK(units <= 255);
	gen_store(r.bytes, r.key_length - 2, 2);
	gen_store(r.bytes + 2, parent, 4);
	gen_store(r.bytes + 6, units, 2);
	for (size_t i = 0; i < units; i++)
		gen_store(r.bytes + 8 + 2 * i, name[i], 2);
	r.length = r.key_length;
	return (r);
}

static gen_record_t
folder_record(const gen_plan_t *plan)
{
	gen_record_t r = catalog_key(1, plan->label, plan->label_units);
	unsigned char *data = r.bytes + r.key_length;

	gen_store(data, 1, 2);
	gen_store(data + 4, plan->child_count +
	    (plan->journal && plan->journal_file ? 1 : 0), 4);
	gen_store(data + 8, 2, 4);
	r.length += 88;
	return (r);
}

static gen_record_t
thread_record(const gen_plan_t *plan)
{
	gen_record_t r = catalog_key(2, NULL, 0);
	unsigned char *data = r.bytes + r.key_length;

	gen_store(data, 3, 2);
	gen_store(data + 4, 1, 4);
	gen_store(data + 8, plan->label_units, 2);
	for (size_t i = 0; i < plan->label_units; i++)
		gen_store(data + 10 + 2 * i, plan->label[i], 2);
	r.length += 10 + 2 * plan->label_units;
	return (r);
}

static gen_record_t
file_record(const gen_child_t *child, uint32_t data_first,
    uint32_t resource_first, uint32_t block_size)
{
	gen_record_t r = catalog_key(2, child->name, child->units);
	unsigned char *data = r.bytes + r.key_length;
	uint32_t counts[] = { child->data_extents, child->resource_extents };
	uint32_t starts[] = { data_first, resource_first };

	gen_store(data, 2, 2);
	gen_store(data + 8, child->id, 4);
	for (unsigned f = 0; f < 2; f++) {
		unsigned char *fork = data + 88 + 80 * f;

		gen_store(fork, (uint64_t)counts[f] * block_size, 8);
		gen_store(fork + 12, counts[f], 4);
		for (uint32_t i = 0; i < counts[f] && i < 8; i++) {
			gen_store(fork + 16 + 8 * i, starts[f] + 2 * i, 4);
			gen_store(fork + 20 + 8 * i, 1, 4);
		}
	}
	r.length += 248;
	return (r);
}

static void
add_record(gen_tree_t *tree, const gen_record_t *record)
{
	gen_record_t *records;

	records = realloc(tree->records,
	    (tree->record_count + 1) * sizeof(*records));
	CHECK(records != NULL);
	tree->records = records;
	tree->records[tree->record_count++] = *record;
}

static void
overflow_records(gen_tree_t *tree, uint32_t id, uint8_t fork_type,
    uint32_t count, uint32_t first)
{
	for (uint32_t i = 8; i < count; i += 8) {
		gen_record_t r = { .key_length = 12, .length = 76 };

		gen_store(r.bytes, 10, 2);
		r.bytes[2] = fork_type;
		gen_store(r.bytes + 4, id, 4);
		gen_store(r.bytes + 8, i, 4);
		for (uint32_t j = 0; j < 8 && i + j < count; j++) {
			gen_store(r.bytes + 12 + 8 * j, first + 2 * (i + j), 4);
			gen_store(r.bytes + 16 + 8 * j, 1, 4);
		}
		add_record(tree, &r);
	}
}

static unsigned char *
new_node(packed_tree_t *packed)
{
	unsigned char *bytes;
	uint32_t size = packed->tree->node_size;

	if (packed->used == packed->capacity) {
		packed->capacity = packed->capacity == 0 ? 8 : packed->capacity
		    * 2;
		bytes = realloc(packed->bytes, (size_t)packed->capacity * size);
		CHECK(bytes != NULL);
		packed->bytes = bytes;
	}
	bytes = packed->bytes + (size_t)packed->used++ * size;
	bzero(bytes, size);
	return (bytes);
}

static void
pack_tree(const gen_plan_t *plan, gen_tree_id_t kind, gen_tree_t *tree,
    unsigned char **result)
{
	packed_tree_t packed = { .tree = tree };
	node_ref_t *level, *next;
	const gen_record_t *first;
	unsigned char *node, *header;
	size_t count, position, used, records, next_count, key_length;
	uint32_t size = plan->node_size[kind], number, previous;
	uint16_t height = 1;

	tree->node_size = (uint16_t)size;
	CHECK(size >= 256 && size <= 32768);
	new_node(&packed);
	count = tree->record_count;
	level = calloc(count + 1, sizeof(*level));
	CHECK(level != NULL);
	position = 0;
	next_count = 0;
	previous = 0;
	while (position < count) {
		number = packed.used;
		node = new_node(&packed);
		node[8] = 255;
		node[9] = 1;
		gen_store(node + 4, previous, 4);
		if (previous != 0)
			gen_store(packed.bytes + (size_t)previous * size,
			    number, 4);
		level[next_count++] = (node_ref_t){ &tree->records[position],
		    number };
		used = 14;
		records = 0;
		while (position < count) {
			gen_record_t *r = &tree->records[position];

			if (used + r->length + 2 * (records + 2) > size ||
			    (plan->records_per_node != 0 &&
			    records >= plan->records_per_node))
				break;
			gen_store(node + size - 2 * (records + 1), used, 2);
			bcopy(r->bytes, node + used, r->length);
			r->offset = (uint64_t)number * size + used;
			used += r->length;
			records++;
			position++;
		}
		CHECK(records != 0);
		gen_store(node + 10, records, 2);
		gen_store(node + size - 2 * (records + 1), used, 2);
		previous = number;
	}
	count = next_count;
	tree->first_leaf = count == 0 ? 0 : level[0].number;
	tree->last_leaf = previous;
	while (count > 1) {
		height++;
		next = calloc(count, sizeof(*next));
		CHECK(next != NULL);
		position = 0;
		next_count = 0;
		previous = 0;
		while (position < count) {
			number = packed.used;
			node = new_node(&packed);
			node[9] = (unsigned char)height;
			gen_store(node + 4, previous, 4);
			if (previous != 0)
				gen_store(packed.bytes + (size_t)previous *
				    size, number, 4);
			next[next_count++] = (node_ref_t){
			    level[position].first, number };
			used = 14;
			records = 0;
			while (position < count) {
				first = level[position].first;
				key_length = first->key_length;
				if (used + key_length + 4 + 2 * (records + 2) >
				    size ||
				    (plan->records_per_node != 0 &&
				    records >= plan->records_per_node))
					break;
				gen_store(node + size - 2 * (records + 1), used,
				    2);
				bcopy(first->bytes, node + used, key_length);
				gen_store(node + used + key_length,
				    level[position].number, 4);
				used += key_length + 4;
				records++;
				position++;
			}
			CHECK(records != 0);
			gen_store(node + 10, records, 2);
			gen_store(node + size - 2 * (records + 1), used, 2);
			previous = number;
		}
		CHECK(next_count < count);
		free(level);
		level = next;
		count = next_count;
	}
	tree->root = count == 0 ? 0 : level[0].number;
	tree->depth = count == 0 ? 0 : height;
	tree->nodes = packed.used;
	free(level);

	header = packed.bytes;
	header[8] = 1;
	gen_store(header + 10, 3, 2);
	gen_store(header + size - 2, 14, 2);
	gen_store(header + size - 4, 120, 2);
	gen_store(header + size - 6, 248, 2);
	gen_store(header + size - 8, size - 8, 2);
	gen_store(header + 14, tree->depth, 2);
	gen_store(header + 16, tree->root, 4);
	gen_store(header + 20, tree->record_count, 4);
	gen_store(header + 24, tree->first_leaf, 4);
	gen_store(header + 28, tree->last_leaf, 4);
	gen_store(header + 32, size, 2);
	gen_store(header + 34, kind == GEN_EXTENTS ? 10 :
	    (kind == GEN_CATALOG ? 516 : 266), 2);
	gen_store(header + 36, tree->nodes, 4);
	header[51] = kind == GEN_CATALOG ? plan->compare_type : 0;
	gen_store(header + 52, kind == GEN_EXTENTS ? 2 : 6, 4);
	CHECK(tree->nodes <= 8 * (size - 256));
	for (uint32_t i = 0; i < tree->nodes; i++)
		header[248 + i / 8] |= (unsigned char)(0x80u >> (i % 8));
	*result = packed.bytes;
}

static uint32_t
allocate_blocks(uint32_t *next, uint64_t bytes, uint32_t block_size)
{
	uint32_t first = *next;

	CHECK(bytes / block_size < UINT32_MAX - *next);
	*next += (uint32_t)((bytes + block_size - 1) / block_size);
	return (first);
}

void
gen_journal_header(unsigned char *p, bool little, uint64_t start,
    uint64_t end, uint64_t size)
{
	unsigned char header[44] = { 0 };
	uint32_t checksum = 0;

	gen_store(header, 0x4a4e4c78, 4);
	gen_store(header + 4, 0x12345678, 4);
	gen_store(header + 8, start, 8);
	gen_store(header + 16, end, 8);
	gen_store(header + 24, size, 8);
	gen_store(header + 32, 8192, 4);
	gen_store(header + 40, 512, 4);
	/* Reverse each field independently, including the two 64-bit positions.
	 */
	for (size_t offset = 0; offset < 44;) {
		size_t width = offset >= 8 && offset < 32 ? 8 : 4;

		for (size_t i = 0; i < width; i++)
			p[offset + i] = header[offset + (little ? width - 1 - i
			    : i)];
		offset += width;
	}
	for (size_t i = 0; i < 44; i++)
		checksum = (checksum << 8) ^ (checksum + p[i]);
	gen_store(header, ~checksum, 4);
	for (size_t i = 0; i < 4; i++)
		p[36 + i] = header[little ? 3 - i : i];
}

void
gen_build(const gen_plan_t *plan, gen_image_t *image)
{
	static const uint16_t journal_name[] = { '.', 'j', 'o', 'u', 'r', 'n',
	    'a', 'l' };
	unsigned char *packed[GEN_TREE_COUNT] = { NULL }, *header, *fork;
	gen_record_t record;
	uint32_t next, bitmap, info_block = 0, journal_block = 0;
	uint32_t data, resource, catalog_continuation = 0;
	uint32_t b = plan->block_size;
	uint64_t bitmap_bytes, tree_bytes;

	CHECK(b >= 512 && (b & (b - 1)) == 0);
	CHECK(plan->records_per_node != 1);
	CHECK(plan->total_blocks >= 16 && plan->label_units <= 255);
	CHECK(plan->total_blocks <= SIZE_MAX / b);
	bzero(image, sizeof(*image));
	image->header = 1024;
	image->block_size = b;
	image->catalog_overflow = plan->catalog_overflow;
	image->length = (size_t)plan->total_blocks * b;
	/* calloc leaves untouched payload pages sparse in memory. Scale
	 * fixtures
	 * use only metadata and never materialize their unused payload blocks.
	 */
	image->bytes = calloc(1, image->length);
	CHECK(image->bytes != NULL);
	next = (1536 + b - 1) / b;
	bitmap_bytes = ((uint64_t)plan->total_blocks + 7) / 8;
	bitmap = allocate_blocks(&next, bitmap_bytes, b);
	if (plan->journal) {
		info_block = allocate_blocks(&next, b, b);
		CHECK(plan->journal_size % b == 0);
		journal_block = allocate_blocks(&next, plan->journal_size, b);
	}
	/* Payload comes before trees, so overflow records can be encoded before
	 * the tree sizes are known. Each block is separated by one unused
	 * block.
	 */
	image->payload_block = next;
	record = folder_record(plan);
	add_record(&image->trees[GEN_CATALOG], &record);
	record = thread_record(plan);
	add_record(&image->trees[GEN_CATALOG], &record);
	if (plan->journal && plan->journal_file) {
		gen_child_t child = { .name = journal_name, .units = 8,
		    .id = 16 };

		record = file_record(&child, 0, 0, b);
		encode_fork(record.bytes + record.key_length + 88,
		    plan->journal_size, journal_block,
		    (uint32_t)(plan->journal_size / b));
		add_record(&image->trees[GEN_CATALOG], &record);
	}
	for (size_t i = 0; i < plan->child_count; i++) {
		const gen_child_t *child = &plan->children[i];

		data = allocate_blocks(&next,
		    (uint64_t)child->data_extents * 2 * b, b);
		resource = allocate_blocks(&next,
		    (uint64_t)child->resource_extents * 2 * b, b);
		CHECK((uint64_t)next * b <= image->length - 1024);
		record = file_record(child, data, resource, b);
		add_record(&image->trees[GEN_CATALOG], &record);
		overflow_records(&image->trees[GEN_EXTENTS], child->id, 0,
		    child->data_extents, data);
		overflow_records(&image->trees[GEN_EXTENTS], child->id, 255,
		    child->resource_extents, resource);
		if (child->data_extents < 1000) {
			for (uint32_t j = 0; j < child->data_extents; j++)
				for (uint32_t k = 0; k < b; k++)
					image->bytes[(size_t)(data + 2 * j) * b
					    + k] =
					    (unsigned char)(j + k);
		}
	}
	pack_tree(plan, GEN_CATALOG, &image->trees[GEN_CATALOG],
	    &packed[GEN_CATALOG]);
	if (plan->catalog_overflow) {
		/* Eight one-block inline runs, then one overflow run. The
		 * tree's
		 * bytes are scattered below after its logical stream is packed.
		 */
		CHECK((uint64_t)image->trees[GEN_CATALOG].nodes *
		    plan->node_size[GEN_CATALOG] > 8u * b);
		catalog_continuation = next;
		record = (gen_record_t){ .key_length = 12, .length = 76 };
		gen_store(record.bytes, 10, 2);
		gen_store(record.bytes + 4, 4, 4);
		gen_store(record.bytes + 8, 8, 4);
		gen_store(record.bytes + 12, next + 16, 4);
		tree_bytes = (uint64_t)image->trees[GEN_CATALOG].nodes *
		    plan->node_size[GEN_CATALOG];
		gen_store(record.bytes + 16, (tree_bytes + b - 1) / b - 8, 4);
		/* Catalog ID 4 precedes ordinary file IDs. */
		gen_tree_t *extents = &image->trees[GEN_EXTENTS];
		add_record(extents, &record);
		for (size_t i = extents->record_count - 1; i > 0; i--)
			extents->records[i] = extents->records[i - 1];
		extents->records[0] = record;
		allocate_blocks(&next, tree_bytes + 8u * b, b);
	}
	pack_tree(plan, GEN_EXTENTS, &image->trees[GEN_EXTENTS],
	    &packed[GEN_EXTENTS]);
	for (size_t i = 0; i < plan->attribute_count; i++) {
		const gen_attribute_t *attribute = &plan->attribute_records[i];

		bzero(&record, sizeof(record));
		gen_store(record.bytes, 14, 2);
		gen_store(record.bytes + 4, attribute->file_id, 4);
		gen_store(record.bytes + 8, attribute->start_block, 4);
		gen_store(record.bytes + 12, 1, 2);
		gen_store(record.bytes + 14, attribute->name, 2);
		record.key_length = 16;
		gen_store(record.bytes + record.key_length, 0x10, 4);
		record.length = record.key_length + 16;
		add_record(&image->trees[GEN_ATTRIBUTES], &record);
	}
	if (plan->attributes)
		pack_tree(plan, GEN_ATTRIBUTES, &image->trees[GEN_ATTRIBUTES],
		    &packed[GEN_ATTRIBUTES]);

	header = image->bytes + image->header;
	gen_store(header, plan->hfsx ? 0x4858 : 0x482b, 2);
	gen_store(header + 2, plan->hfsx ? 5 : 4, 2);
	gen_store(header + 4, 0x100u | (plan->journal ? 0x2000u : 0), 4);
	gen_store(header + 8, 0x31302e30, 4);
	gen_store(header + 12, info_block, 4);
	gen_store(header + 16, 123456, 4);
	gen_store(header + 32, plan->child_count + (plan->journal ? 1 : 0), 4);
	gen_store(header + 40, b, 4);
	gen_store(header + 44, plan->total_blocks, 4);
	gen_store(header + 64, 1000000, 4);
	gen_store(header + 104, 0x12345678, 4);
	gen_store(header + 108, 0x9abcdef0, 4);
	encode_fork(header + GEN_BITMAP_FORK, bitmap_bytes, bitmap,
	    (uint32_t)((bitmap_bytes + b - 1) / b));
	for (unsigned t = 0; t < GEN_TREE_COUNT; t++) {
		gen_tree_t *tree = &image->trees[t];
		unsigned offset = t == GEN_EXTENTS ? GEN_EXTENTS_FORK :
		    (t == GEN_CATALOG ? GEN_CATALOG_FORK : GEN_ATTRIBUTES_FORK);
		uint32_t first;

		if (packed[t] == NULL)
			continue;
		tree_bytes = (uint64_t)tree->nodes * tree->node_size;
		first = t == GEN_CATALOG && plan->catalog_overflow ?
		    catalog_continuation : allocate_blocks(&next, tree_bytes,
		    b);
		CHECK((uint64_t)next * b < image->length - 1024);
		tree->offset = (uint64_t)first * b;
		fork = header + offset;
		encode_fork(fork, tree_bytes, first,
		    (uint32_t)((tree_bytes + b - 1) / b));
		if (t == GEN_CATALOG && plan->catalog_overflow) {
			for (unsigned j = 0; j < 8; j++) {
				gen_store(fork + 16 + 8 * j, first + 2 * j, 4);
				gen_store(fork + 20 + 8 * j, 1, 4);
			}
			for (uint64_t j = 0; j < tree_bytes; j++) {
				image->bytes[tree_position(image,
				    (gen_tree_id_t)t, j)] = packed[t][j];
			}
		} else
			bcopy(packed[t], image->bytes + tree->offset,
			    (size_t)tree_bytes);
		for (size_t j = 0; j < tree->record_count; j++) {
			tree->records[j].offset = tree_position(image,
			    (gen_tree_id_t)t, tree->records[j].offset);
		}
		free(packed[t]);
	}
	image->root_record = image->trees[GEN_CATALOG].records[0].offset;
	image->root_thread = image->trees[GEN_CATALOG].records[1].offset;
	if (plan->journal && plan->journal_file)
		image->journal_record =
		    image->trees[GEN_CATALOG].records[2].offset;
	if (plan->journal) {
		image->journal_info = (uint64_t)info_block * b;
		image->journal_header = (uint64_t)journal_block * b;
		gen_store(image->bytes + image->journal_info,
		    plan->journal_flags, 4);
		gen_store(image->bytes + image->journal_info + 36,
		    image->journal_header, 8);
		gen_store(image->bytes + image->journal_info + 44,
		    plan->journal_size, 8);
		if ((plan->journal_flags & 4) == 0)
			gen_journal_header(image->bytes + image->journal_header,
			    plan->journal_little_endian, plan->journal_start,
			    plan->journal_end, plan->journal_size);
	}
	/* All allocated metadata/payload prefixes and the reserved tail are
	 * set.
	 * The reader tests do not claim to verify exclusive allocation
	 * ownership.
	 */
	for (uint32_t i = 0; i < next; i++)
		image->bytes[(size_t)bitmap * b + i / 8] |= (unsigned
		    char)(0x80u >> (i % 8));
	for (uint32_t i = (uint32_t)((image->length - 1024) / b);
	    i < plan->total_blocks; i++)
		image->bytes[(size_t)bitmap * b + i / 8] |= (unsigned
		    char)(0x80u >> (i % 8));
	bcopy(header, image->bytes + image->length - 1024, 512);
}

static uint64_t
tree_position(const gen_image_t *image, gen_tree_id_t tree, uint64_t logical)
{
	uint32_t b = image->block_size;

	if (tree == GEN_CATALOG && image->catalog_overflow)
		logical = (logical / b < 8 ? 2 * (logical / b) :
		    logical / b + 8) * b + logical % b;
	return (image->trees[tree].offset + logical);
}

uint64_t
gen_node_offset(const gen_image_t *image, gen_tree_id_t tree, uint32_t number)
{
	CHECK(number < image->trees[tree].nodes);
	return (tree_position(image, tree,
	    (uint64_t)number * image->trees[tree].node_size));
}

void
gen_free(gen_image_t *image)
{
	for (unsigned i = 0; i < GEN_TREE_COUNT; i++)
		free(image->trees[i].records);
	free(image->bytes);
	bzero(image, sizeof(*image));
}
