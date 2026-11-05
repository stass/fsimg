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
#include <strings.h>
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

/* Spec numbers have no whitespace or sign, except for signed Unix times. */
static uint64_t
spec_number(const char *text, unsigned base, uint64_t limit)
{
	uint64_t value = 0;
	unsigned digit;

	CHECK(*text != '\0');
	for (; *text != '\0'; text++) {
		CHECK(*text >= '0' && *text < '0' + (int)base);
		digit = (unsigned)(*text - '0');
		CHECK(value <= (limit - digit) / base);
		value = value * base + digit;
	}
	return (value);
}

static fsimg_timestamp_t
spec_time(const char *text)
{
	bool negative = *text == '-';
	uint64_t magnitude;
	fsimg_timestamp_t time = { .basis = FSIMG_TIME_UTC,
	    .precision_ns = 1000000000 };

	magnitude = spec_number(text + negative, 10,
	    (uint64_t)INT64_MAX + negative);
	if (negative && magnitude == (uint64_t)INT64_MAX + 1)
		time.seconds = INT64_MIN;
	else if (negative)
		time.seconds = -(int64_t)magnitude;
	else
		time.seconds = (int64_t)magnitude;
	return (time);
}

static unsigned char *
spec_hex(const char *text, size_t *length)
{
	const char *digits = "0123456789abcdef";
	const char *high, *low;
	unsigned char *bytes;

	*length = strlen(text) / 2;
	CHECK(strlen(text) % 2 == 0);
	bytes = malloc(*length + 1);
	CHECK(bytes != NULL);
	for (size_t i = 0; i < *length; i++) {
		CHECK(text[2 * i] != '\0' && text[2 * i + 1] != '\0');
		high = strchr(digits, text[2 * i]);
		low = strchr(digits, text[2 * i + 1]);
		CHECK(high != NULL && low != NULL);
		bytes[i] = (unsigned char)((high - digits) * 16 + low - digits);
	}
	bytes[*length] = '\0';
	return (bytes);
}

static fsimg_status_t
add_spec(hwf_fixture_t *f, const char *path, fsimg_error_t *error)
{
	fsimg_source_t source = { .kind = FSIMG_SOURCE_PROVIDER, .id = 1000 };
	fsimg_entry_t entry;
	fsimg_status_t status = FSIMG_OK;
	FILE *input;
	char *line = NULL, *fields[11], *tab;
	unsigned char *name, *target;
	size_t capacity = 0, target_capacity = 0, length, target_length;
	ssize_t count;

	input = fopen(path, "r");
	CHECK(input != NULL);
	while ((count = getline(&line, &capacity, input)) != -1) {
		CHECK((size_t)count == strlen(line));
		if (count > 0 && line[count - 1] == '\n')
			line[count - 1] = '\0';
		fields[0] = line;
		for (size_t i = 1; i < 11; i++) {
			tab = strchr(fields[i - 1], '\t');
			CHECK(tab != NULL);
			*tab = '\0';
			fields[i] = tab + 1;
		}
		CHECK(strchr(fields[10], '\t') == NULL);
		bzero(&entry, sizeof(entry));
		if (strcmp(fields[0], "file") == 0)
			entry.kind = FSIMG_ENTRY_FILE;
		else if (strcmp(fields[0], "directory") == 0)
			entry.kind = FSIMG_ENTRY_DIRECTORY;
		else if (strcmp(fields[0], "symlink") == 0)
			entry.kind = FSIMG_ENTRY_SYMLINK;
		else {
			CHECK(strcmp(fields[0], "fifo") == 0);
			entry.kind = FSIMG_ENTRY_FIFO;
		}
		entry.size = spec_number(fields[1], 10, UINT64_MAX);
		CHECK(entry.kind == FSIMG_ENTRY_FILE || entry.size == 0);
		entry.metadata = hwf_metadata();
		entry.metadata.mode = (uint32_t)spec_number(fields[2], 8, 07777);
		entry.metadata.uid = (uint32_t)spec_number(fields[3], 10, UINT32_MAX);
		entry.metadata.gid = (uint32_t)spec_number(fields[4], 10, UINT32_MAX);
		entry.metadata.created = spec_time(fields[5]);
		entry.metadata.modified = spec_time(fields[6]);
		entry.metadata.changed = spec_time(fields[7]);
		entry.metadata.accessed = spec_time(fields[8]);
		if (entry.kind == FSIMG_ENTRY_SYMLINK) {
			target = spec_hex(fields[9], &target_length);
			if (f->target_count == target_capacity) {
				target_capacity = target_capacity == 0 ? 16 :
				    target_capacity * 2;
				CHECK(target_capacity <= SIZE_MAX / sizeof(*f->targets));
				f->targets = realloc(f->targets,
				    target_capacity * sizeof(*f->targets));
				CHECK(f->targets != NULL);
			}
			f->targets[f->target_count++] = (hwf_target_t){
			    .id = source.id, .bytes = target, .length = target_length };
			entry.size = target_length;
		} else
			CHECK(strcmp(fields[9], "-") == 0);
		name = spec_hex(fields[10], &length);
		CHECK(length > 0 && name[0] == '/' &&
		    memchr(name, '\0', length) == NULL);
		entry.path = (char *)name;
		status = fsimg_writer_add(f->writer, &entry,
		    entry.kind == FSIMG_ENTRY_FILE || entry.kind == FSIMG_ENTRY_SYMLINK ?
		    &source : NULL, error);
		free(name);
		if (status != FSIMG_OK)
			break;
		CHECK(source.id != UINT64_MAX);
		source.id++;
	}
	CHECK(!ferror(input));
	CHECK(fclose(input) == 0);
	free(line);
	return (status);
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
	bool minimize, spec;

	if (argc != 5 && argc != 6) {
		fprintf(stderr, "usage: %s OUTPUT VARIANT BLOCK SHAPE [VOLUME_BYTES]\n", argv[0]);
		return (1);
	}
	CHECK(strcmp(argv[2], "hfsplus") == 0 || strcmp(argv[2], "hfsx") == 0);
	variant = strcmp(argv[2], "hfsx") == 0;
	block = number(argv[3]);
	spec = strncmp(argv[4], "spec:", 5) == 0;
	minimize = (spec && block == 0) || strcmp(argv[4], "empty-min") == 0 ||
	    strcmp(argv[4], "tree-min") == 0;
	if (minimize)
		CHECK(block == 0 && argc == 5);
	else
		CHECK(block >= 512 && block <= 65536 &&
		    (block & (block - 1)) == 0);
	CHECK(strcmp(argv[4], "empty") == 0 || strcmp(argv[4], "tree") == 0 ||
	    strcmp(argv[4], "mapnodes") == 0 ||
	    strcmp(argv[4], "mapnodes-small") == 0 ||
	    strcmp(argv[4], "nearlyfull-small") == 0 ||
	    strcmp(argv[4], "nearlyfull") == 0 ||
	    minimize || spec);
	hwf_options(&options, (uint32_t)block);
	/* Spec metadata must survive the fixture's reproducible defaults. */
	if (spec)
		options.flags &= ~FSIMG_WRITE_REPRODUCIBLE;
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
	snprintf(scratch_path, strlen(tmp) + 32, "%s/fsimg-hfs-gen.XXXXXX", tmp);
	CHECK(mkdtemp(scratch_path) != NULL);
	scratch = open(scratch_path, O_RDONLY | O_DIRECTORY);
	CHECK(scratch >= 0);
	hwf_open(&f, variant, &options, 256 * 1024 * 1024, scratch, false);
	status = FSIMG_OK;
	if (spec)
		status = add_spec(&f, argv[4] + 5, &error);
	else if (strcmp(argv[4], "mapnodes") == 0)
		hwf_add_mapnodes(&f, 200000);
	else if (strcmp(argv[4], "mapnodes-small") == 0)
		/* 86769 entries use exactly the header map; one more needs
		 * a map node, including the index levels above the leaves.
		 */
		hwf_add_mapnodes(&f, 86770);
	else if (strcmp(argv[4], "tree-min") == 0)
		hwf_add_tree(&f, 4096, false);
	else if (strcmp(argv[4], "empty") != 0 &&
	    strcmp(argv[4], "empty-min") != 0)
		hwf_add_tree(&f, (uint32_t)block, false);
	if (strcmp(argv[4], "nearlyfull-small") == 0) {
		fsimg_entry_t entry = { .path = "/filler", .kind = FSIMG_ENTRY_FILE };
		fsimg_source_t source = { .kind = FSIMG_SOURCE_ZERO };
		const fsimg_plan_t *plan;
		fsimg_plan_info_t info;

		CHECK(block == 4096 && options.volume_bytes == 4 * 1024 * 1024);
		/* Include the filler's catalog pair when sizing the minimum, then
		 * recreate the writer with a payload that leaves two free blocks.
		 */
		hwf_close(&f);
		options.volume_bytes = 0;
		hwf_open(&f, variant, &options, 256 * 1024 * 1024, scratch, false);
		hwf_add_tree(&f, (uint32_t)block, false);
		CHECK(fsimg_writer_add(f.writer, &entry, &source, NULL) == FSIMG_OK);
		CHECK(fsimg_writer_plan(f.writer, &plan, NULL) == FSIMG_OK);
		CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
		CHECK(info.volume_bytes + 2 * block < 4 * 1024 * 1024);
		entry.size = 4 * 1024 * 1024 - info.volume_bytes - 2 * block;
		hwf_close(&f);
		options.volume_bytes = 4 * 1024 * 1024;
		hwf_open(&f, variant, &options, 256 * 1024 * 1024, scratch, false);
		hwf_add_tree(&f, (uint32_t)block, false);
		CHECK(fsimg_writer_add(f.writer, &entry, &source, NULL) == FSIMG_OK);
	}
	if (status == FSIMG_OK)
		status = fsimg_writer_write_at(f.writer, directory, name, &error);
	if (status != FSIMG_OK && spec)
		fprintf(stderr, "generate %s: %d %s\n",
		    argv[1], status, error.message);
	else if (status != FSIMG_OK)
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
