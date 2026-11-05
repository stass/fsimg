/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hfsplus-write-fixture.h"
#include "test.h"

static uint64_t
number(const char *text)
{
	char *end;
	uintmax_t value;

	errno = 0;
	value = strtoumax(text, &end, 10);
	CHECK(errno == 0 && *text != '\0' && *text != '-' && *end == '\0');
	CHECK(value <= UINT64_MAX);
	return ((uint64_t)value);
}

int
main(int argc, char **argv)
{
	hwf_fixture_t f;
	fsimg_write_options_t options;
	fsimg_error_t error;
	fsimg_status_t status;
	char *parent, *name, *path, *scratch_path;
	const char *tmp;
	unsigned variant;
	uint64_t block;
	int directory, scratch;
	bool minimize;

	if (argc != 5 && argc != 6) {
		fprintf(stderr, "usage: %s OUTPUT VARIANT BLOCK SHAPE [VOLUME_BYTES]\n", argv[0]);
		return (1);
	}
	CHECK(strcmp(argv[2], "hfsplus") == 0 || strcmp(argv[2], "hfsx") == 0);
	variant = strcmp(argv[2], "hfsx") == 0;
	block = number(argv[3]);
	minimize = strcmp(argv[4], "empty-min") == 0 ||
	    strcmp(argv[4], "tree-min") == 0;
	if (minimize)
		CHECK(block == 0 && argc == 5);
	else
		CHECK(block >= 512 && block <= 65536 &&
		    (block & (block - 1)) == 0);
	CHECK(strcmp(argv[4], "empty") == 0 || strcmp(argv[4], "tree") == 0 ||
	    strcmp(argv[4], "mapnodes") == 0 || strcmp(argv[4], "nearlyfull") == 0 ||
	    minimize);
	hwf_options(&options, (uint32_t)block);
	if (minimize) {
		options.flags |= FSIMG_WRITE_MINIMIZE;
		options.volume_bytes = 0;
	}
	if (argc == 6)
		options.volume_bytes = number(argv[5]);
	if (strcmp(argv[4], "nearlyfull") == 0)
		CHECK(options.volume_bytes != 0);
	path = strdup(argv[1]);
	CHECK(path != NULL);
	name = strrchr(path, '/');
	parent = ".";
	if (name != NULL) {
		*name++ = '\0';
		parent = path[0] == '\0' ? "/" : path;
	} else
		name = path;
	CHECK(*name != '\0');
	directory = open(parent, O_RDONLY | O_DIRECTORY);
	CHECK(directory >= 0);
	tmp = getenv("TMPDIR");
	if (tmp == NULL)
		tmp = "/tmp";
	scratch_path = malloc(strlen(tmp) + 32);
	CHECK(scratch_path != NULL);
	sprintf(scratch_path, "%s/fsimg-hfs-gen.XXXXXX", tmp);
	CHECK(mkdtemp(scratch_path) != NULL);
	scratch = open(scratch_path, O_RDONLY | O_DIRECTORY);
	CHECK(scratch >= 0);
	hwf_open(&f, variant, &options, 256 * 1024 * 1024, scratch, false);
	if (strcmp(argv[4], "mapnodes") == 0)
		hwf_add_mapnodes(&f);
	else if (strcmp(argv[4], "tree-min") == 0)
		hwf_add_tree(&f, 4096, false);
	else if (strcmp(argv[4], "empty") != 0 &&
	    strcmp(argv[4], "empty-min") != 0)
		hwf_add_tree(&f, (uint32_t)block, false);
	status = fsimg_writer_write_at(f.writer, directory, name, &error);
	if (status != FSIMG_OK)
		fprintf(stderr, "generate %s: %d %s (offset=%" PRIu64 ")\n",
		    argv[1], status, error.message, error.offset);
	hwf_close(&f);
	CHECK(close(scratch) == 0);
	CHECK(rmdir(scratch_path) == 0);
	CHECK(close(directory) == 0);
	free(scratch_path);
	free(path);
	return (status == FSIMG_OK ? 0 : 1);
}
