/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <strings.h>
#include <unistd.h>

#include "meta.h"

#define STORE_HEADER_SIZE 16u
#define DEFAULT_CACHE_PAGES 256u

typedef struct fsimg_meta_page {
	fsimg_store_t	*owner;
	uint64_t	 number;
	uint32_t	 newer;
	uint32_t	 older;
	bool		 dirty;
	unsigned char	 data[FSIMG_META_PAGE_SIZE];
} fsimg_meta_page_t;

/* Indices link pages from newest to oldest without moving their buffers.
 * UINT32_MAX terminates each end. No pointer escapes a transfer operation.
 */
struct fsimg_meta_cache {
	uint64_t	 objects;
	uint64_t	 resident_pages;
	uint32_t	 count;
	uint32_t	 newest;
	uint32_t	 oldest;
	fsimg_meta_page_t pages[];
};

struct fsimg_store {
	fsimg_ctx_t	*ctx;
	fsimg_session_t	*session;
	fsimg_io_t	*io;
	uint64_t	 length;
	uint64_t	 reserved;
	uint64_t	 physical;
	int		 fd;
	bool		 failed;
};

static fsimg_status_t store_ready(fsimg_store_t *, fsimg_error_t *);
static uint64_t page_count(uint64_t);
static fsimg_status_t scratch_fault(fsimg_ctx_t *, fsimg_meta_fault_t,
    fsimg_error_t *);
static fsimg_status_t scratch_physical(fsimg_store_t *, fsimg_error_t *);
static fsimg_status_t scratch_resize(fsimg_store_t *, uint64_t,
    fsimg_error_t *);
static fsimg_status_t scratch_write(fsimg_store_t *, uint64_t, const void *,
    size_t, fsimg_error_t *);
static fsimg_status_t store_spill(fsimg_store_t *, uint64_t, fsimg_error_t *);
static fsimg_status_t page_flush(fsimg_meta_page_t *, fsimg_error_t *);
static fsimg_status_t page_get(fsimg_store_t *, uint64_t,
    fsimg_meta_page_t **, fsimg_error_t *);
static fsimg_status_t store_transfer(fsimg_store_t *, uint64_t, void *,
    size_t, bool, fsimg_error_t *);

static uint64_t
page_count(uint64_t length)
{
	return (length / FSIMG_META_PAGE_SIZE +
	    (length % FSIMG_META_PAGE_SIZE != 0));
}

fsimg_status_t
fsimg_test_meta_cache(fsimg_ctx_t *ctx, uint32_t pages, fsimg_error_t *error)
{
	fsimg_status_t status;

	status = fsimg_ctx_ready(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (ctx->meta_cache != NULL || pages == 0 || pages > 1024)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_METADATA, "invalid or active metadata cache"));
	ctx->meta_cache_pages = pages;
	return (FSIMG_OK);
}

void
fsimg_test_meta_fail_after(fsimg_ctx_t *ctx, fsimg_meta_fault_t point,
    uint64_t after, int os_error)
{
	ctx->meta_fault_point = point;
	ctx->meta_fault_after = after;
	ctx->meta_fault_errno = os_error;
}

static fsimg_status_t
scratch_fault(fsimg_ctx_t *ctx, fsimg_meta_fault_t point, fsimg_error_t *error)
{
	if (ctx->meta_fault_point == (unsigned)point &&
	    ctx->meta_fault_after != 0 && --ctx->meta_fault_after == 0)
		return (fsimg_error_set(error, FSIMG_EIO,
		    ctx->meta_fault_errno, FSIMG_OP_METADATA,
		    "injected scratch failure"));
	return (FSIMG_OK);
}

static fsimg_status_t
store_ready(fsimg_store_t *store, fsimg_error_t *error)
{
	fsimg_status_t status;

	status = fsimg_ctx_ready(store == NULL ? NULL : store->ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (store->failed)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_METADATA, "failed metadata object must be discarded"));
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_store_new(fsimg_session_t *session, fsimg_store_t **out,
    fsimg_error_t *error)
{
	fsimg_error_t detail;
	fsimg_store_t *store;
	fsimg_ctx_t *ctx;
	fsimg_meta_cache_t *cache;
	fsimg_status_t status;
	uint32_t pages;

	if (out != NULL)
		*out = NULL;
	ctx = session == NULL ? NULL : session->ctx;
	status = fsimg_ctx_ready(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_METADATA, "missing metadata output"));
	store = fsimg_alloc(ctx, sizeof(*store), &detail);
	if (store == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	cache = ctx->meta_cache;
	if (cache == NULL) {
		pages = ctx->meta_cache_pages == 0 ? DEFAULT_CACHE_PAGES :
		    ctx->meta_cache_pages;
		cache = fsimg_alloc(ctx, sizeof(*cache) +
		    pages * sizeof(cache->pages[0]), &detail);
		if (cache == NULL) {
			fsimg_dealloc(store);
			if (error != NULL)
				*error = detail;
			return (detail.status);
		}
		cache->count = pages;
		cache->newest = 0;
		cache->oldest = pages - 1;
		for (uint32_t i = 0; i < pages; i++) {
			cache->pages[i].newer = i == 0 ? UINT32_MAX : i - 1;
			cache->pages[i].older = i + 1 == pages ? UINT32_MAX : i + 1;
		}
		ctx->meta_cache = cache;
	}
	cache->objects++;
	store->ctx = ctx;
	store->session = session;
	store->fd = -1;
	*out = store;
	return (FSIMG_OK);
}

void
fsimg_store_free(fsimg_store_t *store)
{
	fsimg_meta_cache_t *cache;
	fsimg_ctx_t *ctx;

	if (store == NULL || store->ctx->in_callback)
		return;
	ctx = store->ctx;
	cache = ctx->meta_cache;
	for (uint32_t i = 0; i < cache->count; i++) {
		if (cache->pages[i].owner == store) {
			cache->pages[i].owner = NULL;
			cache->pages[i].dirty = false;
			bzero(cache->pages[i].data, sizeof(cache->pages[i].data));
		}
	}
	if (store->io == NULL)
		cache->resident_pages -= page_count(store->length);
	fsimg_io_free(store->io);
	ctx->stats.scratch_bytes -= store->reserved;
	ctx->stats.scratch_physical_bytes -= store->physical;
	fsimg_dealloc(store);
	if (--cache->objects == 0) {
		fsimg_dealloc(cache);
		ctx->meta_cache = NULL;
	}
}

uint64_t
fsimg_store_size(const fsimg_store_t *store)
{
	return (store->length);
}

fsimg_ctx_t *
fsimg_store_context(const fsimg_store_t *store)
{
	return (store->ctx);
}

fsimg_session_t *
fsimg_store_session(const fsimg_store_t *store)
{
	return (store->session);
}

static fsimg_status_t
scratch_physical(fsimg_store_t *store, fsimg_error_t *error)
{
	struct stat st;
	uint64_t bytes, observed;
	fsimg_ctx_t *ctx;

	if (fstat(store->fd, &st) == -1)
		return (fsimg_error_set(error, FSIMG_EIO, errno,
		    FSIMG_OP_METADATA, "scratch allocation stat failed"));
	if (st.st_blocks < 0 ||
	    !fsimg_mul_u64((uint64_t)st.st_blocks, 512, &bytes))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_METADATA, "scratch allocation size overflow"));
	ctx = store->ctx;
	if (!fsimg_add_u64(ctx->stats.scratch_physical_bytes - store->physical,
	    bytes, &observed))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_METADATA, "aggregate scratch allocation overflow"));
	ctx->stats.scratch_physical_bytes = observed;
	store->physical = bytes;
	if (ctx->stats.scratch_physical_bytes > ctx->stats.scratch_physical_peak)
		ctx->stats.scratch_physical_peak = ctx->stats.scratch_physical_bytes;
	return (FSIMG_OK);
}

static fsimg_status_t
scratch_resize(fsimg_store_t *store, uint64_t length, fsimg_error_t *error)
{
	fsimg_ctx_t *ctx;
	uint64_t bytes, observed;
	fsimg_status_t status;

	ctx = store->ctx;
	bytes = length + STORE_HEADER_SIZE;
	if (!fsimg_add_u64(ctx->stats.scratch_bytes, bytes - store->reserved,
	    &observed))
		return (fsimg_limit_error(error, FSIMG_RESOURCE_SCRATCH,
		    ctx->options.scratch_bytes, UINT64_MAX));
	if (observed > ctx->options.scratch_bytes)
		return (fsimg_limit_error(error, FSIMG_RESOURCE_SCRATCH,
		    ctx->options.scratch_bytes, observed));
	/* Keep the reservation until destruction even if the host fails partway. */
	ctx->stats.scratch_bytes = observed;
	store->reserved = bytes;
	if (observed > ctx->stats.scratch_peak)
		ctx->stats.scratch_peak = observed;
	status = scratch_fault(ctx, FSIMG_META_RESIZE, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_io_resize(store->io, bytes, error);
	if (status != FSIMG_OK)
		return (status);
	return (scratch_physical(store, error));
}

static fsimg_status_t
scratch_write(fsimg_store_t *store, uint64_t offset, const void *data,
    size_t length, fsimg_error_t *error)
{
	fsimg_error_t physical_error;
	fsimg_status_t status, sampled;
	size_t count;

	status = scratch_fault(store->ctx, FSIMG_META_WRITE, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_io_write_exact(store->io, offset, data, length,
	    &count, error);
	sampled = scratch_physical(store, &physical_error);
	if (status != FSIMG_OK)
		return (status);
	if (sampled != FSIMG_OK && error != NULL)
		*error = physical_error;
	return (sampled);
}

static fsimg_status_t
store_spill(fsimg_store_t *store, uint64_t length, fsimg_error_t *error)
{
	const unsigned char header[STORE_HEADER_SIZE] =
	    { 'F', 'S', 'M', 'E', 'T', 'A', 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	fsimg_ctx_t *ctx;
	fsimg_status_t status;
	char name[64];
	int fd, saved;

	ctx = store->ctx;
	if (store->io != NULL)
		return (FSIMG_OK);
	if (ctx->options.scratch_directory_fd == -1)
		return (fsimg_limit_error(error, FSIMG_RESOURCE_MEMORY,
		    ctx->options.memory_bytes, ctx->options.memory_bytes + 1));
	status = fsimg_fd_reserve(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	fd = -1;
	for (unsigned attempt = 0; attempt < 128; attempt++) {
		status = fsimg_poll(ctx, NULL, error);
		if (status != FSIMG_OK)
			break;
		status = scratch_fault(ctx, FSIMG_META_OPEN, error);
		if (status != FSIMG_OK)
			break;
		if (ctx->scratch_sequence == UINT64_MAX) {
			status = fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_METADATA, "scratch name sequence exhausted");
			break;
		}
		ctx->scratch_sequence++;
		snprintf(name, sizeof(name), "fsimg-meta-v1-%016" PRIx64,
		    ctx->scratch_sequence);
		fd = openat(ctx->options.scratch_directory_fd, name,
		    O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (fd != -1)
			break;
		saved = errno;
		status = fsimg_error_set(error, FSIMG_EIO, saved,
		    FSIMG_OP_METADATA, "exclusive scratch creation failed");
		if (saved != EEXIST)
			break;
	}
	if (fd == -1) {
		fsimg_fd_release(ctx, -1);
		return (status);
	}
	if (unlinkat(ctx->options.scratch_directory_fd, name, 0) == -1) {
		saved = errno;
		fsimg_fd_release(ctx, fd);
		return (fsimg_error_set(error, FSIMG_EIO, saved,
		    FSIMG_OP_METADATA, "scratch unlink failed"));
	}
	status = fsimg_io_adopt_fd(ctx, fd, FSIMG_IO_READ | FSIMG_IO_WRITE,
	    &store->io, error);
	if (status != FSIMG_OK)
		return (status);
	store->fd = fd;
	status = scratch_resize(store, length, error);
	if (status == FSIMG_OK)
		status = scratch_write(store, 0, header, sizeof(header), error);
	if (status != FSIMG_OK) {
		/* Creating backing storage does not change the logical object.
		 * Retained dirty pages still contain its complete resident data.
		 */
		fsimg_io_free(store->io);
		store->io = NULL;
		store->fd = -1;
		ctx->stats.scratch_bytes -= store->reserved;
		ctx->stats.scratch_physical_bytes -= store->physical;
		store->reserved = 0;
		store->physical = 0;
	} else {
		ctx->meta_cache->resident_pages -= page_count(store->length);
	}
	return (status);
}

fsimg_status_t
fsimg_store_grow(fsimg_store_t *store, uint64_t length, fsimg_error_t *error)
{
	fsimg_meta_cache_t *cache;
	fsimg_status_t status;
	uint64_t pages;

	status = store_ready(store, error);
	if (status != FSIMG_OK)
		return (status);
	if (length < store->length || length > INT64_MAX - STORE_HEADER_SIZE)
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_METADATA, "invalid metadata growth"));
	status = fsimg_session_charge(store->session, 0, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	cache = store->ctx->meta_cache;
	if (store->io == NULL) {
		pages = page_count(length) - page_count(store->length);
		if (pages <= cache->count - cache->resident_pages) {
			cache->resident_pages += pages;
		} else {
			if (store->ctx->options.scratch_directory_fd == -1)
				return (fsimg_limit_error(error, FSIMG_RESOURCE_MEMORY,
				    cache->count * FSIMG_META_PAGE_SIZE,
				    (cache->resident_pages + pages) * FSIMG_META_PAGE_SIZE));
			status = store_spill(store, length, error);
			if (status != FSIMG_OK)
				return (status);
		}
	} else {
		status = scratch_resize(store, length, error);
		if (status != FSIMG_OK) {
			store->failed = true;
			return (status);
		}
	}
	store->length = length;
	return (FSIMG_OK);
}

static fsimg_status_t
page_flush(fsimg_meta_page_t *page, fsimg_error_t *error)
{
	fsimg_store_t *store;
	uint64_t offset;
	size_t length;
	fsimg_status_t status;

	if (page->owner == NULL || !page->dirty)
		return (FSIMG_OK);
	store = page->owner;
	if (store->failed)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_METADATA, "failed metadata owns a dirty cache page"));
	status = store_spill(store, store->length, error);
	if (status != FSIMG_OK)
		return (status);
	offset = page->number * FSIMG_META_PAGE_SIZE;
	length = store->length - offset < FSIMG_META_PAGE_SIZE ?
	    (size_t)(store->length - offset) : FSIMG_META_PAGE_SIZE;
	status = scratch_write(store, offset + STORE_HEADER_SIZE, page->data,
	    length, error);
	if (status != FSIMG_OK)
		return (status);
	page->dirty = false;
	return (FSIMG_OK);
}

static fsimg_status_t
page_get(fsimg_store_t *store, uint64_t number, fsimg_meta_page_t **out,
    fsimg_error_t *error)
{
	unsigned char data[FSIMG_META_PAGE_SIZE];
	fsimg_meta_page_t *page;
	fsimg_meta_cache_t *cache;
	uint64_t offset;
	fsimg_status_t status;
	size_t count, length;
	uint32_t slot;

	status = fsimg_session_charge(store->session, 0, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	cache = store->ctx->meta_cache;
	slot = cache->count;
	for (uint32_t i = 0; i < cache->count; i++) {
		if (cache->pages[i].owner == store &&
		    cache->pages[i].number == number) {
			slot = i;
			break;
		}
	}
	if (slot == cache->count) {
		/* Prefer an empty slot or a reconstructible all-zero page. */
		slot = cache->oldest;
		for (uint32_t i = 0; i < cache->count; i++) {
			if (cache->pages[i].owner == NULL ||
			    (!cache->pages[i].dirty &&
			    cache->pages[i].owner->io == NULL)) {
				slot = i;
				break;
			}
		}
		status = page_flush(&cache->pages[slot], error);
		if (status != FSIMG_OK)
			return (status);
		/* Read into temporary storage before replacing a live cached page. */
		bzero(data, sizeof(data));
		if (store->io != NULL) {
			offset = number * FSIMG_META_PAGE_SIZE;
			length = store->length - offset < FSIMG_META_PAGE_SIZE ?
			    (size_t)(store->length - offset) : FSIMG_META_PAGE_SIZE;
			status = scratch_fault(store->ctx, FSIMG_META_READ, error);
			if (status != FSIMG_OK)
				return (status);
			status = fsimg_io_read_exact(store->io,
			    offset + STORE_HEADER_SIZE, data, length,
			    &count, error);
			if (status != FSIMG_OK)
				return (status);
		}
		page = &cache->pages[slot];
		bcopy(data, page->data, sizeof(data));
		page->owner = store;
		page->number = number;
		page->dirty = false;
	}
	page = &cache->pages[slot];
	if (slot != cache->newest) {
		cache->pages[page->newer].older = page->older;
		if (page->older == UINT32_MAX)
			cache->oldest = page->newer;
		else
			cache->pages[page->older].newer = page->newer;
		page->newer = UINT32_MAX;
		page->older = cache->newest;
		cache->pages[cache->newest].newer = slot;
		cache->newest = slot;
	}
	*out = page;
	return (FSIMG_OK);
}

static fsimg_status_t
store_transfer(fsimg_store_t *store, uint64_t offset, void *buffer,
    size_t length, bool writing, fsimg_error_t *error)
{
	fsimg_meta_page_t *page;
	unsigned char *bytes;
	fsimg_status_t status;
	size_t batch, within;

	status = store_ready(store, error);
	if (status != FSIMG_OK)
		return (status);
	if (!fsimg_range_fits(offset, length, store->length))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0,
		    FSIMG_OP_METADATA, "metadata access outside object"));
	if (length != 0 && buffer == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_METADATA, "missing metadata buffer"));
	bytes = buffer;
	while (length != 0) {
		status = page_get(store, offset / FSIMG_META_PAGE_SIZE,
		    &page, error);
		if (status != FSIMG_OK) {
			if (writing)
				store->failed = true;
			return (status);
		}
		within = offset % FSIMG_META_PAGE_SIZE;
		batch = FSIMG_META_PAGE_SIZE - within;
		if (batch > length)
			batch = length;
		if (writing) {
			bcopy(bytes, page->data + within, batch);
			page->dirty = true;
		} else {
			bcopy(page->data + within, bytes, batch);
		}
		offset += batch;
		bytes += batch;
		length -= batch;
	}
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_store_read(fsimg_store_t *store, uint64_t offset, void *buffer,
    size_t length, fsimg_error_t *error)
{
	return (store_transfer(store, offset, buffer, length, false, error));
}

fsimg_status_t
fsimg_store_write(fsimg_store_t *store, uint64_t offset, const void *buffer,
    size_t length, fsimg_error_t *error)
{
	return (store_transfer(store, offset, (void *)buffer, length, true, error));
}

fsimg_status_t
fsimg_store_flush(fsimg_store_t *store, fsimg_error_t *error)
{
	fsimg_meta_cache_t *cache;
	fsimg_status_t status;

	status = store_ready(store, error);
	if (status != FSIMG_OK)
		return (status);
	if (store->ctx->options.scratch_directory_fd == -1)
		return (FSIMG_OK);
	cache = store->ctx->meta_cache;
	for (uint32_t i = 0; i < cache->count; i++) {
		status = fsimg_poll(store->ctx, NULL, error);
		if (status != FSIMG_OK)
			return (status);
		if (cache->pages[i].owner != store)
			continue;
		status = page_flush(&cache->pages[i], error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}
