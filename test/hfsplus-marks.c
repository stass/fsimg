/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "hfsplus-gen.h"

/* Put the final leaf in the unused tail so truncation fails during walking. */
static void
split_catalog(gen_image_t *image, const gen_plan_t *plan)
{
	gen_tree_t *tree = &image->trees[GEN_CATALOG];
	unsigned char *fork = image->bytes + 1024 + GEN_CATALOG_FORK;
	uint32_t first = (uint32_t)gen_load(fork + 16, 4);
	uint32_t leaf = tree->last_leaf;
	uint32_t block = plan->total_blocks - 4;

	bcopy(image->bytes + gen_node_offset(image, GEN_CATALOG, leaf),
	    image->bytes + (uint64_t)block * plan->block_size, tree->node_size);
	bzero(fork + 16, 64);
	gen_store(fork + 16, first, 4);
	gen_store(fork + 20, leaf, 4);
	gen_store(fork + 24, block, 4);
	gen_store(fork + 28, 1, 4);
	if (leaf + 1 < tree->nodes) {
		gen_store(fork + 32, first + leaf + 1, 4);
		gen_store(fork + 36, tree->nodes - leaf - 1, 4);
	}
}

/* A fixed catalog for the CLI's extra-data listing checks. */
int
main(int argc, char **argv)
{
	static const uint16_t plain[] = { 'p', 'l', 'a', 'i', 'n' };
	static const uint16_t forked[] = { 'f', 'o', 'r', 'k', 'e', 'd' };
	static const uint16_t tagged[] = { 't', 'a', 'g', 'g', 'e', 'd' };
	static const uint16_t dir[] = { 'd', 'i', 'r' };
	static const uint16_t inner[] = { 'i', 'n', 'n', 'e', 'r' };
	static const uint16_t clean[] = { 'c', 'l', 'e', 'a', 'n' };
	static const uint16_t link[] = { 'l' };
	static const unsigned char target[] = "plain";
	static const unsigned char resource[] = "resource";
	static const gen_object_t objects[] = {
		{ .name = plain, .units = 5, .id = 16, .type = 2,
		    .mode = 0100644, .data_size = 5 },
		{ .name = forked, .units = 6, .id = 17, .type = 2,
		    .mode = 0100644, .data_size = 5,
		    .resource = resource, .resource_size = sizeof(resource) - 1 },
		{ .name = tagged, .units = 6, .id = 18, .type = 2,
		    .mode = 0100644, .data_size = 5, .flags = 4 },
		{ .name = dir, .units = 3, .id = 19, .type = 1,
		    .mode = 0040755 },
		{ .name = inner, .units = 5, .id = 20, .parent = 19,
		    .type = 2, .mode = 0100644, .data_size = 5,
		    .resource = resource, .resource_size = sizeof(resource) - 1 },
		{ .name = clean, .units = 5, .id = 21, .parent = 19,
		    .type = 2, .mode = 0100644, .data_size = 5 },
		{ .name = link, .units = 1, .id = 22, .type = 2,
		    .mode = 0120777, .data = target,
		    .data_size = sizeof(target) - 1, .flags = 4 }
	};
	static const gen_attribute_t attributes[] = {
		{ .file_id = 18, .name = 'a' },
		{ .file_id = 22, .name = 'a' }
	};
	gen_plan_t plan;
	gen_image_t image;
	FILE *output;
	int status;

	if (argc != 2 && (argc != 3 ||
	    (strcmp(argv[2], "--split-catalog") != 0 &&
	    strcmp(argv[2], "--symlink") != 0))) {
		fprintf(stderr,
		    "usage: hfsplus-marks OUTPUT [--split-catalog|--symlink]\n");
		return (1);
	}
	gen_plan_init(&plan);
	plan.objects = objects;
	/* Keep the symlink separate from the ordinary three-mark fixture. */
	plan.object_count = sizeof(objects) / sizeof(objects[0]) - 1;
	plan.attributes = true;
	plan.attribute_records = attributes;
	plan.attribute_count = 1;
	if (argc == 3 && strcmp(argv[2], "--symlink") == 0) {
		plan.object_count++;
		plan.attribute_count++;
	}
	if (argc == 3 && strcmp(argv[2], "--split-catalog") == 0)
		plan.records_per_node = 2;
	gen_build(&plan, &image);
	if (argc == 3 && strcmp(argv[2], "--split-catalog") == 0)
		split_catalog(&image, &plan);
	output = fopen(argv[1], "wb");
	if (output == NULL) {
		perror(argv[1]);
		gen_free(&image);
		return (1);
	}
	status = fwrite(image.bytes, 1, image.length, output) == image.length ?
	    0 : 1;
	if (fclose(output) != 0)
		status = 1;
	if (status != 0)
		fprintf(stderr, "hfsplus-marks: cannot write image\n");
	gen_free(&image);
	return (status);
}
