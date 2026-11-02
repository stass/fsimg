/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <strings.h>

#include "internal.h"
#include "test.h"
#include "iso9660-image.h"

#include "meta.h"

typedef enum stage {
	STAGE_OPEN,
	STAGE_WALK,
	STAGE_CONTENT,
	STAGE_VERIFY
} stage_t;

typedef enum fault {
	FAULT_ALLOC,
	FAULT_SCRATCH_OPEN,
	FAULT_SCRATCH_RESIZE,
	FAULT_SCRATCH_READ,
	FAULT_SCRATCH_WRITE,
	FAULT_SHORT,
	FAULT_EOF,
	FAULT_CANCEL
} fault_t;

typedef struct short_source {
	image_t *image;
	uint64_t point;
	bool fired;
} short_source_t;

static fsimg_status_t short_read(void *, uint64_t, void *, size_t, size_t *,
    fsimg_error_t *);
static fsimg_status_t short_size(void *, uint64_t *, fsimg_error_t *);
static fsimg_status_t perform(fixture_t *, stage_t, fsimg_iso_view_t);
static void prepare(fixture_t *, image_t *, stage_t, fsimg_iso_view_t,
    unsigned *, short_source_t *);
static void sweep(image_t *, fsimg_iso_view_t, stage_t, fault_t);
static image_t fault_image(fsimg_iso_view_t);

/* A positive short transfer is legal: exact I/O must resume at its end.
 * EOF is injected separately using the original callback's zero transfer.
 */
static fsimg_status_t
short_size(void *argument, uint64_t *size, fsimg_error_t *error)
{
	short_source_t *source = argument;

	return (image_size(source->image, size, error));
}

static fsimg_status_t
short_read(void *argument, uint64_t offset, void *buffer, size_t length,
    size_t *count, fsimg_error_t *error)
{
	short_source_t *source = argument;
	fsimg_status_t status;

	status = image_read(source->image, offset, buffer, length, count, error);
	if (status == FSIMG_OK && source->image->reads == source->point &&
	    length > 1) {
		*count = length / 2;
		source->fired = true;
	}
	return (status);
}

static fsimg_status_t
perform(fixture_t *f, stage_t stage, fsimg_iso_view_t view)
{
	fsimg_verify_report_t report;
	unsigned char bytes[5];
	size_t count;
	fsimg_status_t status;

	switch (stage) {
	case STAGE_OPEN:
		return (open_image(f, view));
	case STAGE_WALK:
		return (walk(f));
	case STAGE_CONTENT:
		status = fsimg_reader_read_at(f->reader, 0, bytes, sizeof(bytes),
		    &count, NULL);
		if (status == FSIMG_OK) {
			CHECK(count == sizeof(bytes));
			CHECK(bcmp(bytes, "hello", sizeof(bytes)) == 0);
		}
		return (status);
	case STAGE_VERIFY:
		status = fsimg_verify(f->reader, FSIMG_VERIFY_PAYLOAD, &report, NULL);
		CHECK(report.complete == (status == FSIMG_OK));
		return (status);
	}
	CHECK(false);
	return (FSIMG_EINVAL);
}

/* Setup runs with faults disabled so a sweep belongs to one named stage.
 * A two-page cache makes the verifier's scratch I/O reachable on small trees.
 */
static void
prepare(fixture_t *f, image_t *image, stage_t stage, fsimg_iso_view_t view,
    unsigned *budget, short_source_t *source)
{
	fsimg_options_t options;
	fsimg_io_ops_t ops;
	const fsimg_entry_t *entry;

	image->reads = 0;
	image->fail_read = 0;
	image->eof = false;
	fsimg_options_init(&options);
	options.cancelled = cancelled;
	options.cancel_argument = budget;
	*budget = UINT_MAX;
	fixture_new(f, image, &options);
	CHECK(fsimg_test_meta_cache(f->ctx, 2, NULL) == FSIMG_OK);
	fsimg_io_free(f->io);
	fsimg_io_ops_init(&ops);
	ops.read_at = short_read;
	ops.get_size = short_size;
	CHECK(fsimg_io_from_callbacks(f->ctx, &ops, source, &f->io, NULL) == FSIMG_OK);
	if (stage != STAGE_OPEN)
		CHECK(open_image(f, view) == FSIMG_OK);
	if (stage == STAGE_CONTENT) {
		do {
			CHECK(fsimg_reader_next(f->reader, &entry, NULL) == FSIMG_OK);
		} while (entry->kind != FSIMG_ENTRY_FILE);
		CHECK(entry->size == 5);
	}
	image->reads = 0;
}

static void
sweep(image_t *image, fsimg_iso_view_t view, stage_t stage, fault_t fault)
{
	static const char *stages[] = { "open", "walk", "content", "W" };
	static const char *faults[] = { "allocation", "scratch-open",
	    "scratch-resize", "scratch-read", "scratch-write", "short", "EOF",
	    "cancellation" };
	static const fsimg_meta_fault_t scratch[] = { FSIMG_META_OPEN,
	    FSIMG_META_RESIZE, FSIMG_META_READ, FSIMG_META_WRITE };
	fixture_t f;
	short_source_t source;
	fsimg_status_t status;
	unsigned budget;
	bool hit;

	for (unsigned point = 1;; point++) {
		source = (short_source_t){ .image = image };
		prepare(&f, image, stage, view, &budget, &source);
		if (fault == FAULT_ALLOC)
			fsimg_test_fail_alloc_after(f.ctx, point);
		else if (fault >= FAULT_SCRATCH_OPEN && fault <= FAULT_SCRATCH_WRITE)
			fsimg_test_meta_fail_after(f.ctx,
			    scratch[fault - FAULT_SCRATCH_OPEN], point, EIO);
		else if (fault == FAULT_SHORT)
			source.point = point;
		else if (fault == FAULT_EOF) {
			image->fail_read = point;
			image->eof = true;
		} else
			budget = point - 1;

		status = perform(&f, stage, view);
		if (fault == FAULT_ALLOC)
			hit = f.ctx->allocation_attempts >= point;
		else if (fault >= FAULT_SCRATCH_OPEN && fault <= FAULT_SCRATCH_WRITE)
			hit = f.ctx->meta_fault_after == 0;
		else if (fault == FAULT_SHORT || fault == FAULT_EOF)
			hit = image->reads >= point;
		else
			hit = status == FSIMG_ECANCELLED;

		if (!hit || fault == FAULT_SHORT) {
			if (status != FSIMG_OK)
				fprintf(stderr, "view=%u stage=%s fault=%s point=%u status=%d\n",
				    view, stages[stage], faults[fault], point, status);
			CHECK(status == FSIMG_OK);
			if (hit && fault == FAULT_SHORT)
				CHECK(source.fired);
		} else if (fault == FAULT_ALLOC)
			CHECK(status == FSIMG_ENOMEM || status == FSIMG_ELIMIT);
		else if (fault == FAULT_CANCEL)
			CHECK(status == FSIMG_ECANCELLED);
		else
			CHECK(status == FSIMG_EIO);

		fsimg_test_fail_alloc_after(f.ctx, 0);
		fsimg_test_meta_fail_after(f.ctx, FSIMG_META_OPEN, 0, 0);
		fixture_free(&f);
		if (!hit) {
			printf("ISO faults view=%u stage=%s fault=%s checkpoints=%u\n",
			    view, stages[stage], faults[fault], point - 1);
			break;
		}
		CHECK(point < 10000);
	}
}

static image_t
fault_image(fsimg_iso_view_t view)
{
	susp_area_t area = {0};
	image_t image;
	static const uint16_t name[] = { 'F', 'I', 'L', 'E', ';', '1' };

	if (view == FSIMG_ISO_VIEW_ROCKRIDGE) {
		susp_px(&area, 0100644, PX_BYTES);
		susp_nm(&area, "file", 4, 0);
		return (rr_image(&area, 0));
	}
	if (view == FSIMG_ISO_VIEW_PRIMARY)
		return (boot_image(0));
	image = image_new();
	joliet_add(&image);
	verify_table_root(&image, 17, JOLIET_ROOT, 90, 91);
	joliet_record(image.bytes + JOLIET_ROOT * BLOCK + DOT_PAIR_BYTES,
	    name, sizeof(name) / sizeof(name[0]), DATA, 5, 0);
	return (image);
}

int
main(void)
{
	static const fsimg_iso_view_t views[] = { FSIMG_ISO_VIEW_ROCKRIDGE,
	    FSIMG_ISO_VIEW_JOLIET, FSIMG_ISO_VIEW_PRIMARY };
	image_t image;

	for (size_t i = 0; i < sizeof(views) / sizeof(views[0]); i++) {
		image = fault_image(views[i]);
		for (stage_t stage = STAGE_OPEN; stage <= STAGE_VERIFY; stage++) {
			for (fault_t fault = FAULT_ALLOC; fault <= FAULT_CANCEL; fault++) {
				/* The acceptance suite already sweeps bootable open,
				 * traversal and content allocations and EOF faults.
				 */
				if (views[i] == FSIMG_ISO_VIEW_PRIMARY && stage != STAGE_VERIFY &&
				    (fault == FAULT_ALLOC || fault == FAULT_EOF ||
				    (fault == FAULT_CANCEL && stage != STAGE_CONTENT)))
					continue;
				sweep(&image, views[i], stage, fault);
			}
		}
		free(image.bytes);
	}
	return (0);
}
