/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef HFSPLUS_WRITE_FIXTURE_H
#define HFSPLUS_WRITE_FIXTURE_H
#include "fsimg.h"
#define HWF_COUNT 24u
#define HWF_LABEL "Writer volume"
#define HWF_EPOCH 1700000000
typedef struct hwf_entry {
	const char *path;
	const char *listed;
	fsimg_entry_kind_t kind;
	uint64_t length;
} hwf_entry_t;
typedef struct hwf_target {
	uint64_t id;
	unsigned char *bytes;
	size_t length;
} hwf_target_t;
typedef struct hwf_fixture {
	fsimg_ctx_t *ctx;
	fsimg_writer_t *writer;
	fsimg_stats_t baseline;
	hwf_entry_t entries[HWF_COUNT];
	/* Sorted by source id; only spec symlinks need stored content. */
	hwf_target_t *targets;
	size_t target_count;
	uint64_t reads;
	uint64_t validates;
	uint64_t fail_read;
	uint64_t short_read;
	uint64_t fail_validate;
	uint64_t polls;
	uint64_t cancel_at;
} hwf_fixture_t;
typedef struct hwf_image {
	fsimg_ctx_t *poison_context; /* End transfer-buffer poison on zero. */
	unsigned char *bytes;
	uint64_t length;
	uint64_t writes;
	uint64_t fail_write;
	uint64_t short_write;
	bool fail_flush;
} hwf_image_t;
void hwf_options(fsimg_write_options_t *, uint32_t);
void hwf_open(hwf_fixture_t *, unsigned, const fsimg_write_options_t *,
    uint64_t, int, bool);
void hwf_add_tree(hwf_fixture_t *, uint32_t, bool);
void hwf_add_mapnodes(hwf_fixture_t *, unsigned);
void hwf_close(hwf_fixture_t *);
unsigned char hwf_byte(uint64_t, uint64_t);
fsimg_metadata_t hwf_metadata(void);
fsimg_status_t hwf_emit(hwf_fixture_t *, hwf_image_t *, fsimg_error_t *);
fsimg_status_t hwf_io(hwf_fixture_t *, hwf_image_t *, fsimg_io_t **);
#endif
