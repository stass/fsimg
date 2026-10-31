/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fsimg.h"

/*
 * The partition-table oracles' view of compose, which has no command-line
 * form: compose the volumes into a new disk in the current directory,
 * reproducibly at a fixed epoch.
 *
 *	layers-compose OUTPUT SCHEME VOLUME[:TYPE[:NAME]] ...
 *
 * OUTPUT is a basename; a volume without a type takes its filesystem's.
 */

#define MAX_PARTS	8

static int
fail(const char *what, const fsimg_error_t *error)
{
	fprintf(stderr, "layers-compose: %s: %s%s%s\n", what,
	    error != NULL ? error->message : "",
	    error != NULL && error->path[0] != '\0' ? " " : "",
	    error != NULL ? error->path : "");
	return (1);
}

int
main(int argc, char **argv)
{
	fsimg_part_t parts[MAX_PARTS];
	char *specs[MAX_PARTS];
	fsimg_write_options_t options;
	fsimg_sink_t sink;
	fsimg_error_t error;
	fsimg_ctx_t *ctx;
	char *type, *name;
	size_t count;
	int fds[MAX_PARTS], directory, status;

	if (argc < 4 || argc - 3 > MAX_PARTS)
		return (fail("usage: OUTPUT SCHEME VOLUME[:TYPE[:NAME]] ...", NULL));
	if (fsimg_ctx_new(NULL, &ctx, &error) != FSIMG_OK)
		return (fail("context", &error));
	count = (size_t)argc - 3;
	memset(parts, 0, sizeof(parts));
	for (size_t i = 0; i < count; i++) {
		specs[i] = argv[i + 3];
		type = strchr(specs[i], ':');
		name = NULL;
		if (type != NULL) {
			*type++ = '\0';
			name = strchr(type, ':');
			if (name != NULL)
				*name++ = '\0';
		}
		fds[i] = open(specs[i], O_RDONLY | O_CLOEXEC);
		if (fds[i] == -1) {
			perror(specs[i]);
			return (1);
		}
		if (fsimg_io_from_fd(ctx, fds[i], FSIMG_IO_READ, &parts[i].volume,
		    &error) != FSIMG_OK)
			return (fail(specs[i], &error));
		parts[i].type = type;
		parts[i].name = name;
	}
	fsimg_write_options_init(&options);
	options.flags = FSIMG_WRITE_REPRODUCIBLE;
	options.epoch = 1700000000;
	directory = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (directory == -1) {
		perror(".");
		return (1);
	}
	fsimg_sink_init(&sink, FSIMG_SINK_FILE);
	sink.parent_fd = directory;
	sink.name = argv[1];
	status = fsimg_image_compose(ctx, argv[2], parts, count, &options, &sink,
	    &error) == FSIMG_OK ? 0 : fail("compose", &error);
	for (size_t i = 0; i < count; i++) {
		fsimg_io_free(parts[i].volume);
		close(fds[i]);
	}
	close(directory);
	fsimg_ctx_free(ctx);
	return (status);
}
