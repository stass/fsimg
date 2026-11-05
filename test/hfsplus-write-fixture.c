/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "format.h"
#include "internal.h"
#include "writer.h"
#include "test.h"
#include "hfsplus-write-fixture.h"

static const char long_first[] =
	"/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char long_last[] =
	"/zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"
	"zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"
	"zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"
	"zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz";

static bool
cancelled(void *arg)
{
	hwf_fixture_t *f = arg;

	return (++f->polls == f->cancel_at);
}

unsigned char
hwf_byte(uint64_t id, uint64_t offset)
{
	static const char target[] = "dir/sub/below";

	if (id == 10)
		return ((unsigned char)target[offset]);
	return ((unsigned char)(1 + (id * 17 + offset * 13) % 251));
}

static fsimg_status_t
source_read(void *arg, uint64_t id, uint64_t offset, void *buffer,
    size_t length, size_t *done, fsimg_error_t *error FSIMG_UNUSED)
{
	hwf_fixture_t *f = arg;
	unsigned char *p = buffer;
	size_t low = 0, high = f->target_count, mid;

	*done = 0;
	if (++f->reads == f->fail_read)
		return (FSIMG_OK); /* Premature EOF. */
	if (f->reads == f->short_read && length > 1)
		length /= 2;
	while (low < high) {
		mid = low + (high - low) / 2;
		if (f->targets[mid].id < id)
			low = mid + 1;
		else
			high = mid;
	}
	if (low < f->target_count && f->targets[low].id == id) {
		CHECK(offset <= f->targets[low].length);
		CHECK(length <= f->targets[low].length - offset);
		bcopy(f->targets[low].bytes + offset, buffer, length);
	} else {
		for (size_t i = 0; i < length; i++)
			p[i] = hwf_byte(id, offset + i);
	}
	*done = length;
	return (FSIMG_OK);
}

static fsimg_status_t
source_validate(void *arg, uint64_t id FSIMG_UNUSED,
    fsimg_error_t *error FSIMG_UNUSED)
{
	hwf_fixture_t *f = arg;

	return (++f->validates == f->fail_validate ?
	    FSIMG_ESOURCE_CHANGED : FSIMG_OK);
}

fsimg_metadata_t
hwf_metadata(void)
{
	fsimg_metadata_t m = { .valid = FSIMG_META_CREATED |
	    FSIMG_META_MODIFIED | FSIMG_META_CHANGED | FSIMG_META_ACCESSED |
	    FSIMG_META_OWNER | FSIMG_META_MODE, .uid = 123, .gid = 456,
	    .mode = 0751 };

	m.created = (fsimg_timestamp_t){ .seconds = HWF_EPOCH - 4,
	    .basis = FSIMG_TIME_UTC, .precision_ns = 1000000000 };
	m.modified = m.created;
	m.modified.seconds++;
	m.changed = m.modified;
	m.changed.seconds++;
	m.accessed = m.changed;
	m.accessed.seconds++;
	return (m);
}

void
hwf_options(fsimg_write_options_t *o, uint32_t block)
{
	fsimg_write_options_init(o);
	o->flags = FSIMG_WRITE_REPRODUCIBLE | FSIMG_WRITE_ALLOW_LOSS;
	o->epoch = HWF_EPOCH;
	o->allocation_unit_bytes = block;
	o->label = HWF_LABEL;
	for (size_t i = 0; i < sizeof(o->volume_uuid); i++)
		o->volume_uuid[i] = (unsigned char)(i + 1);
}

void
hwf_open(hwf_fixture_t *f, unsigned variant, const fsimg_write_options_t *o,
    uint64_t memory, int scratch, bool poison)
{
	fsimg_options_t options;
	fsimg_source_ops_t source;

	bzero(f, sizeof(*f));
	fsimg_options_init(&options);
	options.memory_bytes = memory;
	options.scratch_directory_fd = scratch;
	options.cancelled = cancelled;
	options.cancel_argument = f;
	CHECK(fsimg_ctx_new(&options, &f->ctx, NULL) == FSIMG_OK);
	fsimg_test_poison_allocations(f->ctx, poison);
	if (!poison && memory == 16 * 1024 * 1024 && scratch != -1)
		CHECK(fsimg_test_meta_cache(f->ctx, 2, NULL) == FSIMG_OK);
	CHECK(fsimg_ctx_get_stats(f->ctx, &f->baseline, NULL) == FSIMG_OK);
	CHECK(fsimg_writer_new_variant(f->ctx, &fsimg_hfsplus_format,
	    &fsimg_hfsplus_format.variants[variant], o, &f->writer,
	    NULL) == FSIMG_OK);
	fsimg_source_ops_init(&source);
	source.read_at = source_read;
	source.validate = source_validate;
	CHECK(fsimg_writer_set_sources(f->writer, &source, f, NULL) == FSIMG_OK);
}

void
hwf_add_tree(hwf_fixture_t *f, uint32_t block, bool reverse)
{
	hwf_entry_t entries[HWF_COUNT] = {
		{ "/dir", "/dir", FSIMG_ENTRY_DIRECTORY, 0 },
		{ "/dir/sub", "/dir/sub", FSIMG_ENTRY_DIRECTORY, 0 },
		{ "/empty", "/empty", FSIMG_ENTRY_FILE, 0 },
		{ "/dir/sub/below", "/dir/sub/below", FSIMG_ENTRY_FILE, block - 1 },
		{ "/equal", "/equal", FSIMG_ENTRY_FILE, block },
		{ "/above", "/above", FSIMG_ENTRY_FILE, block + 1 },
		{ "/two", "/two", FSIMG_ENTRY_FILE, 2 * block + 1 },
		{ "/hello", "/hello", FSIMG_ENTRY_FILE, 5 },
		{ "/caf\303\251", "/cafe\314\201", FSIMG_ENTRY_FILE, 19 },
		{ "/link", "/link", FSIMG_ENTRY_SYMLINK, 13 },
		{ "/pipe", "/pipe", FSIMG_ENTRY_FIFO, 0 },
		{ "/dir/vacant", "/dir/vacant", FSIMG_ENTRY_DIRECTORY, 0 },
		{ long_first, long_first, FSIMG_ENTRY_FILE, 0 },
		{ long_last, long_last, FSIMG_ENTRY_FILE, 0 },
		{ "/Zeta", "/Zeta", FSIMG_ENTRY_FILE, 0 },
		{ "/alpha", "/alpha", FSIMG_ENTRY_FILE, 0 },
		{ "/\316\251", "/\316\251", FSIMG_ENTRY_FILE, 0 },
		{ "/\316\261", "/\316\261", FSIMG_ENTRY_FILE, 0 },
		{ "/:", "/:", FSIMG_ENTRY_FILE, 0 },
		{ "/\342\220\200", "/\342\220\200", FSIMG_ENTRY_FILE, 0 },
		{ "/\360\237\230\200", "/\360\237\230\200", FSIMG_ENTRY_FILE, 0 },
		{ "/\357\274\241", "/\357\274\241", FSIMG_ENTRY_FILE, 0 },
		{ "/a\342\200\214x", "/a\342\200\214x", FSIMG_ENTRY_FILE, 0 },
		{ "/ay", "/ay", FSIMG_ENTRY_FILE, 0 }
	};
	fsimg_entry_t e;
	fsimg_source_t source = { .kind = FSIMG_SOURCE_PROVIDER };

	bcopy(entries, f->entries, sizeof(entries));
	for (unsigned n = 0; n < HWF_COUNT; n++) {
		unsigned i = reverse ? HWF_COUNT - n - 1 : n;

		e = (fsimg_entry_t){ .path = entries[i].path,
		    .kind = entries[i].kind, .size = entries[i].length,
		    .metadata = hwf_metadata() };
		source.id = i + 1;
		CHECK(fsimg_writer_add(f->writer, &e,
		    e.kind == FSIMG_ENTRY_DIRECTORY || e.kind == FSIMG_ENTRY_FIFO ?
		    NULL : &source, NULL) == FSIMG_OK);
	}
}

/* Long names leave fewer than six primary/thread pairs per leaf, so this
 * exceeds the header map's 30720-node capacity without any host sources.
 */
void
hwf_add_mapnodes(hwf_fixture_t *f, unsigned count)
{
	char path[242];
	fsimg_entry_t e = { .kind = FSIMG_ENTRY_FILE };
	fsimg_source_t source = { .kind = FSIMG_SOURCE_ZERO };

	memset(path, 'n', sizeof(path) - 1);
	path[sizeof(path) - 1] = '\0';
	for (unsigned i = 0; i < count; i++) {
		snprintf(path, 10, "/%08u", i);
		path[9] = 'n';
		e.path = path;
		CHECK(fsimg_writer_add(f->writer, &e, &source, NULL) == FSIMG_OK);
	}
}

void
hwf_close(hwf_fixture_t *f)
{
	fsimg_stats_t stats;

	fsimg_writer_free(f->writer);
	for (size_t i = 0; i < f->target_count; i++)
		free(f->targets[i].bytes);
	free(f->targets);
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == f->baseline.memory_bytes);
	CHECK(stats.scratch_bytes == f->baseline.scratch_bytes);
	CHECK(stats.descriptors == f->baseline.descriptors);
	CHECK(stats.spool_bytes == f->baseline.spool_bytes);
	fsimg_ctx_free(f->ctx);
}

static fsimg_status_t
image_size(void *arg, uint64_t *size, fsimg_error_t *error FSIMG_UNUSED)
{
	*size = ((hwf_image_t *)arg)->length;
	return (FSIMG_OK);
}

static fsimg_status_t
image_read(void *arg, uint64_t offset, void *buffer, size_t length,
    size_t *done, fsimg_error_t *error FSIMG_UNUSED)
{
	hwf_image_t *image = arg;

	CHECK(offset <= image->length && length <= image->length - offset);
	bcopy(image->bytes + offset, buffer, length);
	*done = length;
	return (FSIMG_OK);
}

static fsimg_status_t
image_write(void *arg, uint64_t offset, const void *buffer, size_t length,
    size_t *done, fsimg_error_t *error FSIMG_UNUSED)
{
	hwf_image_t *image = arg;

	*done = 0;
	if (++image->writes == image->fail_write)
		return (FSIMG_EIO);
	if (image->writes == image->short_write && length > 1)
		length /= 2;
	CHECK(offset <= image->length && length <= image->length - offset);
	bcopy(buffer, image->bytes + offset, length);
	*done = length;
	return (FSIMG_OK);
}

/* fsimg_emit_image allocates its transfer buffer before zeroing the sink.
 * End the poison scope here, before source callback I/O objects are created:
 * those objects rely on the allocator's zero-initialization contract.
 */
static fsimg_status_t
image_zero_after_poison(void *arg, uint64_t offset, uint64_t length,
    fsimg_error_t *error FSIMG_UNUSED)
{
	hwf_image_t *image = arg;

	fsimg_test_poison_allocations(image->poison_context, false);
	CHECK(offset <= image->length && length <= image->length - offset);
	bzero(image->bytes + offset, (size_t)length);
	return (FSIMG_OK);
}

static fsimg_status_t
image_flush(void *arg, fsimg_error_t *error FSIMG_UNUSED)
{
	return (((hwf_image_t *)arg)->fail_flush ? FSIMG_EIO : FSIMG_OK);
}

fsimg_status_t
hwf_io(hwf_fixture_t *f, hwf_image_t *image, fsimg_io_t **io)
{
	fsimg_io_ops_t ops;

	fsimg_io_ops_init(&ops);
	ops.get_size = image_size;
	ops.read_at = image_read;
	ops.write_at = image_write;
	ops.flush = image_flush;
	if (image->poison_context != NULL)
		ops.zero_range = image_zero_after_poison;
	return (fsimg_io_from_callbacks(f->ctx, &ops, image, io, NULL));
}

fsimg_status_t
hwf_emit(hwf_fixture_t *f, hwf_image_t *image, fsimg_error_t *error)
{
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_io_t *io = NULL;
	fsimg_status_t status;

	status = fsimg_writer_plan(f->writer, &plan, error);
	if (status != FSIMG_OK)
		return (status);
	CHECK(fsimg_plan_get_info(plan, &info, NULL) == FSIMG_OK);
	image->length = info.volume_bytes;
	image->bytes = malloc((size_t)image->length);
	CHECK(image->bytes != NULL);
	memset(image->bytes, 0xa5, (size_t)image->length);
	status = hwf_io(f, image, &io);
	if (status == FSIMG_OK)
		status = fsimg_writer_emit(f->writer, io, error);
	fsimg_io_free(io);
	return (status);
}
