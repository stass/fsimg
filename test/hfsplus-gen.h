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

typedef struct gen_attribute {
	uint32_t	 file_id;
	uint32_t	 start_block;
	uint16_t	 name;
} gen_attribute_t;

typedef struct gen_plan {
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
	bool		 journal_little_endian;
	uint32_t	 journal_flags;
	uint64_t	 journal_size;
	uint64_t	 journal_start;
	uint64_t	 journal_end;
} gen_plan_t;

typedef struct gen_record {
	unsigned char	 bytes[800];
	size_t		 length;
	size_t		 key_length;
	uint64_t	 offset;
} gen_record_t;

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

#endif
