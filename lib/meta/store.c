/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
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

#include "host.h"
#include "meta.h"

#define STORE_HEADER_SIZE 16u
#define NO_PAGE UINT32_MAX
#define CACHE_CHUNK_PAGES 256u
/* At most 16 GiB of cached pages, which bounds the chunk table. */
#define CACHE_SLOT_LIMIT (UINT32_C(1) << 22)

typedef struct fsimg_meta_page {
	fsimg_store_t	*owner;		/* NULL for a free slot */
	uint64_t	 number;
	uint32_t	 newer;
	uint32_t	 older;
	uint32_t	 chain;		/* next slot in the same hash bucket */
	bool		 dirty;
	unsigned char	 data[FSIMG_META_PAGE_SIZE];
} fsimg_meta_page_t;

/* Slots live in chunks allocated as the cache grows, so a context charges
 * only for the slots it has needed. Slot i is entry i % chunk_pages of
 * chunk i / chunk_pages. Indices link slots from newest to oldest; NO_PAGE
 * terminates each end and each hash bucket chain. Free slots sit at the
 * oldest end, so the replacement victim is always the oldest slot. No
 * pointer escapes a transfer operation.
 */
struct fsimg_meta_cache {
	uint64_t	 objects;
	uint64_t	 resident_pages;
	uint32_t	 count;		/* slots allocated */
	uint32_t	 limit;		/* most slots the cache may allocate */
	uint32_t	 chunk_pages;
	uint32_t	 newest;
	uint32_t	 oldest;
	uint32_t	 mask;		/* bucket count less one; the count is at least the slots */
	uint32_t	*buckets;	/* a separate allocation, rebuilt on growth */
	fsimg_meta_page_t *chunks[];
};

/* A walk over one store's cached slots. Its mode is fixed when it starts:
 * a store with fewer pages than the cache has slots looks each page up, a
 * larger one scans the slots, so the walk costs the smaller of the two.
 */
typedef struct page_walk {
	const fsimg_store_t *store;
	bool		 by_page;
	uint64_t	 next;
	uint64_t	 end;
} page_walk_t;

struct fsimg_store {
	fsimg_ctx_t	*ctx;
	fsimg_session_t	*session;
	fsimg_io_t	*io;
	uint64_t	 length;
	uint64_t	 reserved;
	uint64_t	 written;	/* host file extent written, header included */
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
static fsimg_status_t cache_new(fsimg_ctx_t *, fsimg_error_t *);
static fsimg_status_t cache_grow(fsimg_ctx_t *, fsimg_error_t *);
static bool cache_try_grow(fsimg_ctx_t *);
static void cache_free(fsimg_meta_cache_t *);
static fsimg_meta_page_t *page_at(const fsimg_meta_cache_t *, uint32_t);
static uint32_t page_bucket(const fsimg_meta_cache_t *, const fsimg_store_t *,
    uint64_t);
static uint32_t page_find(const fsimg_store_t *, uint64_t);
static void page_hash(fsimg_meta_cache_t *, uint32_t);
static void page_unhash(fsimg_meta_cache_t *, uint32_t);
static void page_unlink(fsimg_meta_cache_t *, uint32_t);
static void page_newest(fsimg_meta_cache_t *, uint32_t);
static void page_oldest(fsimg_meta_cache_t *, uint32_t);
static void page_walk_start(page_walk_t *, const fsimg_store_t *);
static uint32_t page_walk_next(page_walk_t *);
static fsimg_status_t page_load(fsimg_store_t *, uint64_t, unsigned char *,
    fsimg_error_t *);
static fsimg_status_t page_get(fsimg_store_t *, uint64_t, bool,
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

/* The most slots a cache may allocate under a memory budget: whole chunks
 * whose slots and buckets fit a quarter of it, and at least the first one.
 */
static uint32_t
cache_slot_limit(uint64_t memory_bytes)
{
	uint64_t limit, slot_bytes;

	/* The bucket array holds at most two entries per slot. */
	slot_bytes = sizeof(fsimg_meta_page_t) + 2 * sizeof(uint32_t);
	limit = memory_bytes / 4 / slot_bytes;
	if (limit > CACHE_SLOT_LIMIT)
		limit = CACHE_SLOT_LIMIT;
	if (limit < CACHE_CHUNK_PAGES)
		return (CACHE_CHUNK_PAGES);
	return ((uint32_t)(limit / CACHE_CHUNK_PAGES * CACHE_CHUNK_PAGES));
}

uint64_t
fsimg_meta_cache_max_bytes(uint64_t memory_bytes)
{
	uint64_t buckets, slots;

	slots = cache_slot_limit(memory_bytes);
	for (buckets = 1; buckets < slots; buckets *= 2)
		continue;
	/* The header, its chunk table, the slots, and the buckets at their
	 * last rebuild, when the old array is still held beside the new one.
	 */
	return (sizeof(fsimg_meta_cache_t) +
	    slots / CACHE_CHUNK_PAGES * sizeof(fsimg_meta_page_t *) +
	    slots * sizeof(fsimg_meta_page_t) +
	    (buckets + buckets / 2) * sizeof(uint32_t));
}

/* Create the context's cache with one chunk of slots. A validation
 * control fixes the capacity in a single chunk; otherwise the cache may
 * grow to cache_slot_limit.
 */
static fsimg_status_t
cache_new(fsimg_ctx_t *ctx, fsimg_error_t *error)
{
	fsimg_error_t detail;
	fsimg_meta_cache_t *cache;
	fsimg_status_t status;
	uint64_t limit;
	uint32_t chunk;

	if (ctx->meta_cache_pages != 0) {
		limit = chunk = ctx->meta_cache_pages;
	} else {
		chunk = CACHE_CHUNK_PAGES;
		limit = cache_slot_limit(ctx->options.memory_bytes);
	}
	/* At most CACHE_SLOT_LIMIT / CACHE_CHUNK_PAGES or 1024 chunk pointers. */
	cache = fsimg_alloc(ctx, sizeof(*cache) +
	    (size_t)(limit / chunk) * sizeof(cache->chunks[0]), &detail);
	if (cache == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	cache->limit = (uint32_t)limit;
	cache->chunk_pages = chunk;
	cache->newest = cache->oldest = NO_PAGE;
	ctx->meta_cache = cache;
	status = cache_grow(ctx, error);
	if (status != FSIMG_OK) {
		ctx->meta_cache = NULL;
		fsimg_dealloc(cache);
	}
	return (status);
}

/* Add a chunk of free slots at the oldest end, rebuilding the bucket array
 * when the slots outgrow it. The caller checks the limit.
 */
static fsimg_status_t
cache_grow(fsimg_ctx_t *ctx, fsimg_error_t *error)
{
	fsimg_meta_cache_t *cache;
	fsimg_meta_page_t *chunk;
	fsimg_error_t detail;
	uint32_t buckets, first, *table;

	cache = ctx->meta_cache;
	/* The allocator zeroes the chunk, so every new slot is free. */
	chunk = fsimg_alloc(ctx, (size_t)cache->chunk_pages * sizeof(*chunk),
	    &detail);
	if (chunk == NULL)
		goto fail;
	first = cache->count;
	if (cache->buckets == NULL || first + cache->chunk_pages > cache->mask + 1) {
		/* Limit is at most 2^22, so the bucket count stays below 2^23. */
		for (buckets = 1; buckets < first + cache->chunk_pages;
		    buckets *= 2)
			continue;
		table = fsimg_alloc(ctx, (size_t)buckets * sizeof(*table),
		    &detail);
		if (table == NULL) {
			fsimg_dealloc(chunk);
			goto fail;
		}
		fsimg_dealloc(cache->buckets);
		cache->buckets = table;
		cache->mask = buckets - 1;
		for (uint32_t i = 0; i < buckets; i++)
			table[i] = NO_PAGE;
		for (uint32_t slot = 0; slot < first; slot++)
			if (page_at(cache, slot)->owner != NULL)
				page_hash(cache, slot);
	}
	cache->chunks[first / cache->chunk_pages] = chunk;
	cache->count += cache->chunk_pages;
	for (uint32_t slot = first; slot < cache->count; slot++)
		page_oldest(cache, slot);
	return (FSIMG_OK);
fail:
	if (error != NULL)
		*error = detail;
	return (detail.status);
}

/* Grow past the first chunk only while the cache is under its limit and the
 * context's charge leaves a quarter of the budget for sort runs and callers.
 * Growth is opportunistic: on refusal the caller replaces a page instead.
 */
static bool
cache_try_grow(fsimg_ctx_t *ctx)
{
	fsimg_meta_cache_t *cache;
	uint64_t need;

	cache = ctx->meta_cache;
	if (cache->count == cache->limit)
		return (false);
	/* A chunk and, if the buckets are rebuilt, a doubled array while the
	 * old one is still held.
	 */
	need = (uint64_t)cache->chunk_pages * sizeof(fsimg_meta_page_t) +
	    3 * ((uint64_t)cache->mask + 1) * sizeof(uint32_t);
	if (ctx->stats.memory_bytes + need >
	    ctx->options.memory_bytes - ctx->options.memory_bytes / 4)
		return (false);
	return (cache_grow(ctx, NULL) == FSIMG_OK);
}

static void
cache_free(fsimg_meta_cache_t *cache)
{
	for (uint32_t i = 0; i < cache->count / cache->chunk_pages; i++)
		fsimg_dealloc(cache->chunks[i]);
	fsimg_dealloc(cache->buckets);
	fsimg_dealloc(cache);
}

static fsimg_meta_page_t *
page_at(const fsimg_meta_cache_t *cache, uint32_t slot)
{
	return (&cache->chunks[slot / cache->chunk_pages]
	    [slot % cache->chunk_pages]);
}

static uint32_t
page_bucket(const fsimg_meta_cache_t *cache, const fsimg_store_t *store,
    uint64_t number)
{
	uint64_t h;

	h = ((uint64_t)(uintptr_t)store * UINT64_C(0x9e3779b97f4a7c15)) ^ number;
	h *= UINT64_C(0xbf58476d1ce4e5b9);
	return ((uint32_t)(h >> 32) & cache->mask);
}

/* The slot caching a store's page, or NO_PAGE. */
static uint32_t
page_find(const fsimg_store_t *store, uint64_t number)
{
	const fsimg_meta_cache_t *cache;
	uint32_t slot;

	cache = store->ctx->meta_cache;
	slot = cache->buckets[page_bucket(cache, store, number)];
	while (slot != NO_PAGE && (page_at(cache, slot)->owner != store ||
	    page_at(cache, slot)->number != number))
		slot = page_at(cache, slot)->chain;
	return (slot);
}

/* Index an owned slot under its owner and page number. */
static void
page_hash(fsimg_meta_cache_t *cache, uint32_t slot)
{
	fsimg_meta_page_t *page;
	uint32_t *bucket;

	page = page_at(cache, slot);
	bucket = &cache->buckets[page_bucket(cache, page->owner, page->number)];
	page->chain = *bucket;
	*bucket = slot;
}

/* Remove an owned slot from the index; its owner must still be set. */
static void
page_unhash(fsimg_meta_cache_t *cache, uint32_t slot)
{
	fsimg_meta_page_t *page;
	uint32_t *link;

	page = page_at(cache, slot);
	link = &cache->buckets[page_bucket(cache, page->owner, page->number)];
	while (*link != slot)
		link = &page_at(cache, *link)->chain;
	*link = page->chain;
	page->chain = NO_PAGE;
}

/* Remove a slot from the replacement order; page_newest or page_oldest
 * relinks it.
 */
static void
page_unlink(fsimg_meta_cache_t *cache, uint32_t slot)
{
	fsimg_meta_page_t *page;

	page = page_at(cache, slot);
	if (page->newer == NO_PAGE)
		cache->newest = page->older;
	else
		page_at(cache, page->newer)->older = page->older;
	if (page->older == NO_PAGE)
		cache->oldest = page->newer;
	else
		page_at(cache, page->older)->newer = page->newer;
}

/* Make a linked slot the most recently used. */
static void
page_newest(fsimg_meta_cache_t *cache, uint32_t slot)
{
	fsimg_meta_page_t *page;

	if (slot == cache->newest)
		return;
	page_unlink(cache, slot);
	page = page_at(cache, slot);
	page->newer = NO_PAGE;
	page->older = cache->newest;
	page_at(cache, cache->newest)->newer = slot;
	cache->newest = slot;
}

/* Link an unlinked free slot at the oldest end, the next victim. Only new
 * and released slots come here, which keeps free slots at that end.
 */
static void
page_oldest(fsimg_meta_cache_t *cache, uint32_t slot)
{
	fsimg_meta_page_t *page;

	page = page_at(cache, slot);
	page->newer = cache->oldest;
	page->older = NO_PAGE;
	page->chain = NO_PAGE;
	if (cache->oldest == NO_PAGE)
		cache->newest = slot;
	else
		page_at(cache, cache->oldest)->older = slot;
	cache->oldest = slot;
}

static void
page_walk_start(page_walk_t *walk, const fsimg_store_t *store)
{
	uint64_t pages, slots;

	pages = page_count(store->length);
	slots = store->ctx->meta_cache->count;
	walk->store = store;
	walk->by_page = pages < slots;
	walk->next = 0;
	walk->end = walk->by_page ? pages : slots;
}

/* The walk's next slot owned by its store, or NO_PAGE when done. */
static uint32_t
page_walk_next(page_walk_t *walk)
{
	const fsimg_meta_cache_t *cache;
	uint32_t slot;

	cache = walk->store->ctx->meta_cache;
	while (walk->next < walk->end) {
		if (walk->by_page) {
			slot = page_find(walk->store, walk->next++);
		} else {
			slot = (uint32_t)walk->next++;
			if (page_at(cache, slot)->owner != walk->store)
				slot = NO_PAGE;
		}
		if (slot != NO_PAGE)
			return (slot);
	}
	return (NO_PAGE);
}

fsimg_status_t
fsimg_store_new(fsimg_session_t *session, fsimg_store_t **out,
    fsimg_error_t *error)
{
	fsimg_error_t detail;
	fsimg_store_t *store;
	fsimg_ctx_t *ctx;
	fsimg_status_t status;

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
	if (ctx->meta_cache == NULL) {
		status = cache_new(ctx, error);
		if (status != FSIMG_OK) {
			fsimg_dealloc(store);
			return (status);
		}
	}
	ctx->meta_cache->objects++;
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
	fsimg_meta_page_t *page;
	fsimg_ctx_t *ctx;
	page_walk_t walk;
	uint32_t slot;

	if (store == NULL || store->ctx->in_callback)
		return;
	ctx = store->ctx;
	cache = ctx->meta_cache;
	page_walk_start(&walk, store);
	while ((slot = page_walk_next(&walk)) != NO_PAGE) {
		page = page_at(cache, slot);
		page_unhash(cache, slot);
		page_unlink(cache, slot);
		page_oldest(cache, slot);
		page->owner = NULL;
		page->dirty = false;
		bzero(page->data, FSIMG_META_PAGE_SIZE);
	}
	if (store->io == NULL)
		cache->resident_pages -= page_count(store->length);
	fsimg_io_free(store->io);
	ctx->stats.scratch_bytes -= store->reserved;
	ctx->stats.scratch_physical_bytes -= store->physical;
	fsimg_dealloc(store);
	if (--cache->objects == 0) {
		cache_free(cache);
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

	if (fsimg_host_fstat(store->fd, &st) == -1)
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
	/* Bytes past the written extent read as zero, so growth makes no host
	 * call; the file grows as page flushes reach it.
	 */
	return (fsimg_poll(ctx, NULL, error));
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
	if (status == FSIMG_OK && offset + length > store->written)
		store->written = offset + length;
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
	status = fsimg_io_adopt_scratch(ctx, fd, &store->io, error);
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
		store->written = 0;
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
		/* Grow the cache what it can to keep the pages resident, then
		 * spill if they still do not fit; resident_pages <= count.
		 */
		while (pages > cache->count - cache->resident_pages &&
		    cache_try_grow(store->ctx))
			continue;
		if (pages <= cache->count - cache->resident_pages) {
			cache->resident_pages += pages;
		} else {
			if (store->ctx->options.scratch_directory_fd == -1)
				return (fsimg_limit_error(error, FSIMG_RESOURCE_MEMORY,
				    (uint64_t)cache->count * FSIMG_META_PAGE_SIZE,
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

/* Read a spilled store's page from its written extent into data, which
 * the caller has zeroed: bytes past the extent were never written.
 */
static fsimg_status_t
page_load(fsimg_store_t *store, uint64_t number, unsigned char *data,
    fsimg_error_t *error)
{
	uint64_t offset;
	fsimg_status_t status;
	size_t count, length;

	offset = number * FSIMG_META_PAGE_SIZE + STORE_HEADER_SIZE;
	if (store->io == NULL || offset >= store->written)
		return (FSIMG_OK);
	length = store->written - offset < FSIMG_META_PAGE_SIZE ?
	    (size_t)(store->written - offset) : FSIMG_META_PAGE_SIZE;
	status = scratch_fault(store->ctx, FSIMG_META_READ, error);
	if (status != FSIMG_OK)
		return (status);
	return (fsimg_io_read_exact(store->io, offset, data, length, &count,
	    error));
}

/* Find or load a page and make it the newest, dirty when writing. A read
 * of a resident page that is not cached yields *out == NULL, meaning zeros:
 * only writes give resident pages slots, so every cached resident page is
 * dirty.
 */
static fsimg_status_t
page_get(fsimg_store_t *store, uint64_t number, bool writing,
    fsimg_meta_page_t **out, fsimg_error_t *error)
{
	unsigned char data[FSIMG_META_PAGE_SIZE];
	fsimg_meta_page_t *page;
	fsimg_meta_cache_t *cache;
	fsimg_status_t status;
	uint32_t slot;

	*out = NULL;
	status = fsimg_session_charge(store->session, 0, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	cache = store->ctx->meta_cache;
	slot = page_find(store, number);
	if (slot == NO_PAGE) {
		if (!writing && store->io == NULL)
			return (FSIMG_OK);
		if (page_at(cache, cache->oldest)->owner != NULL)
			(void)cache_try_grow(store->ctx);
		slot = cache->oldest;
		status = page_flush(page_at(cache, slot), error);
		if (status != FSIMG_OK)
			return (status);
		/* Load into temporary storage before replacing a live page. */
		bzero(data, sizeof(data));
		status = page_load(store, number, data, error);
		if (status != FSIMG_OK)
			return (status);
		page = page_at(cache, slot);
		if (page->owner != NULL)
			page_unhash(cache, slot);
		bcopy(data, page->data, sizeof(data));
		page->owner = store;
		page->number = number;
		page->dirty = false;
		page_hash(cache, slot);
	}
	page_newest(cache, slot);
	page = page_at(cache, slot);
	if (writing)
		page->dirty = true;
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
		    writing, &page, error);
		if (status != FSIMG_OK) {
			if (writing)
				store->failed = true;
			return (status);
		}
		within = offset % FSIMG_META_PAGE_SIZE;
		batch = FSIMG_META_PAGE_SIZE - within;
		if (batch > length)
			batch = length;
		if (writing)
			bcopy(bytes, page->data + within, batch);
		else if (page == NULL)
			bzero(bytes, batch);
		else
			bcopy(page->data + within, bytes, batch);
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
	page_walk_t walk;
	uint32_t slot;

	status = store_ready(store, error);
	if (status != FSIMG_OK)
		return (status);
	if (store->ctx->options.scratch_directory_fd == -1)
		return (FSIMG_OK);
	cache = store->ctx->meta_cache;
	page_walk_start(&walk, store);
	while ((slot = page_walk_next(&walk)) != NO_PAGE) {
		status = fsimg_poll(store->ctx, NULL, error);
		if (status != FSIMG_OK)
			return (status);
		status = page_flush(page_at(cache, slot), error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}
