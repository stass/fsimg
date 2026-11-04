/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_TEST_HFSPLUS_GEN_H
#define FSIMG_TEST_HFSPLUS_GEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Independent disk positions from the format design, not driver headers. */
enum {
	GEN_BITMAP_FORK = 112,
	GEN_EXTENTS_FORK = 192,
	GEN_CATALOG_FORK = 272,
	GEN_ATTRIBUTES_FORK = 352,
	GEN_STARTUP_FORK = 432
};

/* Independent prepared-record positions used by scratch corruption tests. */
enum {
	PREPARED_ID = 0,
	PREPARED_LEAF = 4,
	PREPARED_RECORD = 8,
	PREPARED_KIND = 10,
	PREPARED_EXTRA = 11,
	PREPARED_COMPRESSION = 12,
	PREPARED_SIZE = 13,
	PREPARED_OWNER = 21,
	PREPARED_GROUP = 25,
	PREPARED_MODE = 29,
	PREPARED_FLAGS = 31,
	PREPARED_CREATED = 32,
	PREPARED_MODIFIED = 36,
	PREPARED_CHANGED = 40,
	PREPARED_ACCESSED = 44,
	PREPARED_SPECIAL = 48,
	PREPARED_VALENCE = 52,
	PREPARED_FOLDER_COUNT = 56,
	PREPARED_BYTES = 60
};

typedef enum gen_tree_id {
	GEN_EXTENTS,
	GEN_CATALOG,
	GEN_ATTRIBUTES,
	GEN_TREE_COUNT
} gen_tree_id_t;

typedef struct gen_child {
	const uint16_t	*name;
	size_t		 units;
	uint32_t	 id;
	uint32_t	 data_extents;
	uint32_t	 resource_extents;
} gen_child_t;

/* Explicit catalog objects. Zero parent means root; type is 1 or 2.
 * Names are supplied in stored UTF-16 form. Content is really allocated;
 * fragmented content uses one block per run with unused blocks between.
 */
typedef struct gen_object {
	const uint16_t	*name;
	size_t		 units;
	uint32_t	 parent;
	uint32_t	 id;
	uint16_t	 type;
	uint16_t	 flags;
	uint16_t	 mode;
	uint32_t	 dates[5];
	uint32_t	 owner;
	uint32_t	 group;
	uint32_t	 special;
	uint32_t	 reserved;
	uint8_t		 admin_flags;
	uint8_t		 owner_flags;
	unsigned char	 finder[32];
	const unsigned char *data;
	const unsigned char *resource;
	size_t		 data_size;
	size_t		 resource_size;
	bool		 fragmented;
	bool		 omit_thread;
} gen_object_t;

typedef struct gen_attr_value {
	uint32_t	 id;
	uint32_t	 start;
	uint32_t	 type;
	const uint16_t	*name;
	size_t		 units;
	const unsigned char *value;
	size_t		 size;
	/* Optional raw fork/extents body, excluding type and reserved word. */
	const unsigned char *body;
	size_t		 body_size;
} gen_attr_value_t;

typedef struct gen_attribute {
	uint32_t	 file_id;
	uint32_t	 start_block;
	uint16_t	 name;
} gen_attribute_t;

typedef struct gen_record gen_record_t;

typedef struct gen_plan {
	const gen_object_t *objects;
	size_t		 object_count;
	const gen_attr_value_t *values;
	size_t		 value_count;
	/* Called after sorting, before packing, for deliberately invalid records. */
	void (*mutate_catalog)(gen_record_t *, size_t);
	const gen_attribute_t *attribute_records;
	size_t		 attribute_count;

	const gen_child_t *children;
	size_t		 child_count;
	const uint16_t	*label;
	size_t		 label_units;
	uint32_t	 block_size;
	uint32_t	 total_blocks;
	uint16_t	 node_size[GEN_TREE_COUNT];
	/* Maximum records per leaf and per index, at least two. Zero packs
	 * full. */
	uint16_t	 records_per_node;
	uint8_t		 compare_type;
	bool		 hfsx;
	bool		 attributes;
	bool		 catalog_overflow;
	bool		 journal;
	bool		 journal_file;
	bool		 journal_info_file;
	bool		 journal_little_endian;
	uint32_t	 journal_flags;
	uint64_t	 journal_size;
	uint64_t	 journal_start;
	uint64_t	 journal_end;
} gen_plan_t;

struct gen_record {
	unsigned char	 bytes[4096];
	size_t		 length;
	size_t		 key_length;
	uint64_t	 offset;
};

typedef struct gen_tree {
	gen_record_t	*records;
	size_t		 record_count;
	uint64_t	 offset;
	uint32_t	 nodes;
	uint32_t	 root;
	uint32_t	 first_leaf;
	uint32_t	 last_leaf;
	uint16_t	 node_size;
	uint16_t	 depth;
} gen_tree_t;

typedef struct gen_image {
	unsigned char	*bytes;
	size_t		 length;
	gen_tree_t	 trees[GEN_TREE_COUNT];
	uint64_t	 header;
	uint64_t	 journal_info;
	uint64_t	 journal_header;
	uint64_t	 journal_record;
	uint64_t	 root_record;
	uint64_t	 root_thread;
	uint32_t	 payload_block;
	uint32_t	 block_size;
	bool		 catalog_overflow;
} gen_image_t;

/* The caller owns the returned image and frees it with gen_free(). No I/O. */
void gen_plan_init(gen_plan_t *);
void gen_build(const gen_plan_t *, gen_image_t *);
void gen_free(gen_image_t *);
uint64_t gen_node_offset(const gen_image_t *, gen_tree_id_t, uint32_t);
void gen_store(unsigned char *, uint64_t, size_t);
uint64_t gen_load(const unsigned char *, size_t);
void gen_journal_header(unsigned char *, bool, uint64_t, uint64_t, uint64_t);

/* Stored streams for independently framed compressed resource chunks. */
typedef struct gen_compressed_chunk {
	const unsigned char *bytes;
	size_t length;
} gen_compressed_chunk_t;

unsigned char *gen_compressed_resource(const gen_compressed_chunk_t *, size_t,
    size_t *);

#endif
