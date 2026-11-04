/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdio.h>

#include "hfsplus-gen.h"

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
		    .type = 2, .mode = 0100644, .data_size = 5 }
	};
	static const gen_attribute_t attribute = {
		.file_id = 18, .name = 'a'
	};
	gen_plan_t plan;
	gen_image_t image;
	FILE *output;
	int status;

	if (argc != 2) {
		fprintf(stderr, "usage: hfsplus-marks OUTPUT\n");
		return (1);
	}
	gen_plan_init(&plan);
	plan.objects = objects;
	plan.object_count = sizeof(objects) / sizeof(objects[0]);
	plan.attributes = true;
	plan.attribute_records = &attribute;
	plan.attribute_count = 1;
	gen_build(&plan, &image);
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
