/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_TEST_ENTRY_LOG_H
#define FSIMG_TEST_ENTRY_LOG_H

#include <string.h>
#include <strings.h>

#include "fsimg.h"
#include "test.h"

/* Callback paths are borrowed. Keep independent copies for assertions after
 * planning or extraction has released its traversal buffers.
 */
typedef struct entry_log {
	size_t		 count;
	struct {
		char	 path[512];
		fsimg_entry_kind_t kind;
	} entries[16];
} entry_log_t;

static void
record_entry(void *argument, const fsimg_entry_t *entry)
{
	entry_log_t *log = argument;
	size_t length;

	CHECK(log->count < sizeof(log->entries) / sizeof(log->entries[0]));
	length = strlen(entry->path);
	CHECK(length < sizeof(log->entries[0].path));
	bcopy(entry->path, log->entries[log->count].path, length + 1);
	log->entries[log->count].kind = entry->kind;
	log->count++;
}

static void
expect_entry(const entry_log_t *log, size_t index, const char *path,
    fsimg_entry_kind_t kind)
{
	CHECK(index < log->count);
	if (strcmp(log->entries[index].path, path) != 0 ||
	    log->entries[index].kind != kind)
		fprintf(stderr, "entry %zu: expected %s (kind %d), got %s (kind %d)\n",
		    index, path, kind, log->entries[index].path,
		    log->entries[index].kind);
	CHECK(strcmp(log->entries[index].path, path) == 0);
	CHECK(log->entries[index].kind == kind);
}

#endif
