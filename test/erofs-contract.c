/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdlib.h>
#include <string.h>

#include "erofs-contract.h"
#include "erofs-gen.h"
#include "test.h"

enum {
	BLKSZBITS = 12,
	MAX_CHILDREN = 8
};

/* The index of the directory holding source i, or -1 for the root. */
static ptrdiff_t
parent_of(const erofs_source_t *sources, size_t i)
{
	const char *slash = strrchr(sources[i].path, '/');
	size_t length = (size_t)(slash - sources[i].path);

	for (size_t j = 0; j < i && length != 0; j++) {
		if (sources[j].directory && strlen(sources[j].path) == length &&
		    strncmp(sources[j].path, sources[i].path, length) == 0)
			return ((ptrdiff_t)j);
	}
	CHECK(length == 0);
	return (-1);
}

/* Place every entry in the tree: a directory's slot, a file with its bytes. */
static void
place_entries(gen_tree_t *tree, const erofs_source_t *sources, size_t count,
    erofs_byte_fn_t byte_at, void *argument, uint64_t *nids)
{
	gen_inode_t file = { .mode = 0100644, .nlink = 1, .layout = 2,
	    .uid = 1000, .gid = 100 };

	for (size_t i = 0; i < count; i++) {
		unsigned char *bytes;

		if (sources[i].directory) {
			nids[i] = gen_tree_slot(tree, GEN_COMPACT_BYTES, 0);
			continue;
		}
		bytes = calloc(sources[i].size + 1, 1);
		CHECK(bytes != NULL);
		for (uint64_t k = 0; k < sources[i].size; k++)
			bytes[k] = byte_at(argument, i, k);
		nids[i] = gen_tree_add(tree, &file, bytes,
		    (size_t)sources[i].size);
		free(bytes);
	}
}

/* Write directory `dir` (-1 is the root) naming the sources inside it. */
static void
place_directory(gen_tree_t *tree, const erofs_source_t *sources, size_t count,
    const uint64_t *nids, ptrdiff_t dir)
{
	gen_dirent_t children[MAX_CHILDREN];
	ptrdiff_t up = dir < 0 ? -1 : parent_of(sources, (size_t)dir);
	size_t n = 0;

	for (size_t i = 0; i < count; i++) {
		if (parent_of(sources, i) != dir)
			continue;
		CHECK(n < MAX_CHILDREN);
		children[n++] = (gen_dirent_t){
		    strrchr(sources[i].path, '/') + 1, nids[i],
		    sources[i].directory ? 2 : 1 };
	}
	/* The root is NID 0 and is its own parent. */
	gen_tree_dir(tree, dir < 0 ? 0 : nids[dir], up < 0 ? 0 : nids[up],
	    children, n);
}

unsigned char *
erofs_contract_image(const erofs_source_t *sources, size_t count,
    erofs_byte_fn_t byte_at, void *argument, size_t *length)
{
	uint64_t nids[16];
	uint64_t data_bytes = 0;
	gen_tree_t tree;
	unsigned char *image;

	CHECK(count <= sizeof(nids) / sizeof(nids[0]));

	/* A block for each directory, whole blocks for each file, one spare. */
	for (size_t i = 0; i < count; i++)
		data_bytes += sources[i].directory ? 4096 : sources[i].size;
	gen_tree_init(&tree, BLKSZBITS, gen_tree_first_block(BLKSZBITS) +
	    (uint32_t)(data_bytes / 4096) + (uint32_t)count + 1);
	place_entries(&tree, sources, count, byte_at, argument, nids);
	place_directory(&tree, sources, count, nids, -1);
	for (size_t i = 0; i < count; i++) {
		if (sources[i].directory)
			place_directory(&tree, sources, count, nids,
			    (ptrdiff_t)i);
	}
	gen_tree_seal(&tree);
	image = tree.image.bytes;
	*length = tree.image.length;
	return (image);
}
