/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_WRITER_H
#define FSIMG_WRITER_H

#include "host.h"
#include "meta.h"

#define FSIMG_CATALOG_HEADER 176u
#define FSIMG_CATALOG_INDEX_SIZE 24u

/* Catalog payload: seven little-endian u64 fields (size, storage, source ID,
 * kind, valid, required, attributes), then three 40-byte timestamps (seconds,
 * precision, nanoseconds, basis, offset), then the NUL-terminated UTF-8 path.
 * Child records contain parent ID, child ID, and NUL-terminated component.
 * Index records contain stream frame offset, parent ID, and child count.
 */

typedef struct fsimg_source_root fsimg_source_root_t;
typedef struct fsimg_source_cursor fsimg_source_cursor_t;

typedef enum fsimg_writer_state {
	FSIMG_WRITER_COLLECTING,
	FSIMG_WRITER_PREPARING,
	FSIMG_WRITER_SEALED,
	FSIMG_WRITER_EMITTING,
	FSIMG_WRITER_FINISHED,
	FSIMG_WRITER_FAILED
} fsimg_writer_state_t;

typedef enum fsimg_source_storage {
	FSIMG_STORAGE_NONE,
	FSIMG_STORAGE_ZERO,
	FSIMG_STORAGE_PROVIDER,
	FSIMG_STORAGE_HOST
} fsimg_source_storage_t;

/* The writer's single current source owns io. fd is only a borrowed alias
 * used for identity checks; freeing io closes and releases the descriptor.
 */
struct fsimg_source_cursor {
	fsimg_writer_t	*writer;
	fsimg_io_t	*io;
	fsimg_host_file_t identity;
	fsimg_source_storage_t storage;
	uint64_t	 id;
	uint64_t	 length;
	int		 fd;
	bool		 active;
};

typedef struct fsimg_catalog_entry {
	fsimg_entry_t	 entry;
	uint64_t	 id;
	uint64_t	 parent_id;
	uint64_t	 children;
	fsimg_source_storage_t storage;
	/* HOST: host_sources frame offset; PROVIDER: caller ID; NONE/ZERO: 0. */
	uint64_t	 source_id;
} fsimg_catalog_entry_t;

typedef struct fsimg_catalog_child {
	uint64_t	 parent_id;
	uint64_t	 id;
	const char	*name;
	size_t		 name_length;
} fsimg_catalog_child_t;

struct fsimg_writer {
	fsimg_ctx_t	*ctx;
	fsimg_plan_t	*plan;
	fsimg_write_options_t options;
	fsimg_session_t	 session;
	fsimg_writer_state_t state;
	fsimg_stream_t	*entries;
	fsimg_stream_t	*host_sources;
	fsimg_stream_t	*ordered;
	fsimg_stream_t	*children;
	fsimg_array_t	*index;
	fsimg_source_root_t *roots;
	fsimg_source_cursor_t source;
	fsimg_source_ops_t provider;
	void		*provider_argument;
	unsigned char	*record;
	unsigned char	*source_record;
	fsimg_catalog_entry_t current;
	uint64_t	 root_count;
	uint64_t	 count;
	uint64_t	 payload_bytes;
	/* Private fault injection: interrupt the nth subsequent directory read. */
	uint64_t	 interrupt_directory_read_after;
	uint32_t	 record_capacity;
	bool		 provider_set;
	bool		 catalog_ready;
	unsigned	 variant;
	char		 label[12];
};

/* Common collection is private; public construction selects a format driver.
 * Preparation seals before work and is single-use.
 * Returned entries are writer-owned current snapshots, valid until the next
 * catalog operation or destruction. Source operations preserve the snapshot.
 * Invalid entry/provider values are terminal collection failures; calls in
 * the wrong lifecycle state return ESTATE without changing it.
 */
fsimg_status_t fsimg_writer_collect_new(fsimg_ctx_t *, fsimg_writer_t **,
    fsimg_error_t *);
fsimg_status_t fsimg_writer_set_sources(fsimg_writer_t *,
    const fsimg_source_ops_t *, void *, fsimg_error_t *);
fsimg_status_t fsimg_writer_add(fsimg_writer_t *, const fsimg_entry_t *,
    const fsimg_source_t *, fsimg_error_t *);
fsimg_status_t fsimg_writer_add_tree(fsimg_writer_t *, int, const char *,
    fsimg_error_t *);
void fsimg_writer_free(fsimg_writer_t *);
fsimg_status_t fsimg_writer_collect_ready(fsimg_writer_t *, fsimg_error_t *);
fsimg_status_t fsimg_writer_fail(fsimg_writer_t *, fsimg_status_t);
fsimg_status_t fsimg_writer_prepare(fsimg_writer_t *,
    const fsimg_sort_options_t *, fsimg_error_t *);
fsimg_status_t fsimg_catalog_get(fsimg_writer_t *, uint64_t,
    const fsimg_catalog_entry_t **, fsimg_error_t *);
fsimg_status_t fsimg_writer_collect(fsimg_writer_t *, const fsimg_entry_t *,
    fsimg_source_storage_t, uint64_t, fsimg_error_t *);
void fsimg_catalog_discard(fsimg_writer_t *);
fsimg_status_t fsimg_catalog_prepare(fsimg_writer_t *,
    const fsimg_sort_options_t *, fsimg_error_t *);
/* Source lookup uses its own record buffer and preserves current snapshot. */
fsimg_status_t fsimg_catalog_read_source(fsimg_writer_t *, uint64_t,
    fsimg_catalog_entry_t *, fsimg_error_t *);
fsimg_status_t fsimg_catalog_decode_child(fsimg_writer_t *, size_t,
    fsimg_catalog_child_t *, fsimg_error_t *);
/* Catalog build phase: validate the sorted child stream and derive counts. */
fsimg_status_t fsimg_catalog_count_children(fsimg_writer_t *, fsimg_error_t *);

#endif
