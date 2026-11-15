/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <strings.h>

#include "apfs.h"
#include "fsimg-endian.h"
#include "reader.h"

/*
 * The space manager (specs/design-apfs.md, "Ephemeral objects and the space
 * manager"): the geometry its counts follow, the size its arrays give it,
 * the fixed regions of a container, the geometry the native formatter
 * derives from N, and the pager over its chunk structures and bitmaps.
 */

#define ROLE_SM		"space manager"
#define ROLE_CAB	"chunk-information address block"
#define ROLE_CIB	"chunk-information block"
#define ROLE_CHUNK	"chunk"
#define ROLE_BITMAP	"chunk bitmap"
#define ROLE_POOL	"internal-pool bitmap"

/*
 * Writer values (specs/design-apfs.md, "Writer values"): fits to the E1
 * and E12 captures, not rules of [A1].
 */
#define NATIVE_RING_MULTIPLIER	16u	/* ring slots per pool bitmap block */
#define NATIVE_POOL_FACTOR	3u	/* pool blocks per chunk structure */
#define NATIVE_VOLUME_BYTES	(512ull << 20)	/* one volume per 512 MiB */
#define NATIVE_VOLUMES_MAX	100u
#define NATIVE_MAPPINGS_PER_BLOCK 101u
#define NATIVE_QUEUE_LIMIT_MAX	512u
#define NATIVE_EPHEMERAL_WORD0	0x40001ull	/* below the M field */
#define NATIVE_SMALL_BLOCKS	32768u	/* M = L up to 128 MiB */

/* The counts a container's B and N give its main device. */
typedef struct sm_geometry {
	uint64_t	chunks;
	uint64_t	cibs;
	uint64_t	cabs;
	uint32_t	blocks_per_chunk;
	uint32_t	chunks_per_cib;
	uint32_t	cibs_per_cab;
} sm_geometry_t;

/* The arrays after the fixed structure, in bytes of the object. */
enum { ARRAY_XID, ARRAY_INDEX, ARRAY_FREE_NEXT, ARRAY_ADDRESS, ARRAYS };

typedef struct sm_array {
	uint64_t	offset;
	uint64_t	bytes;
	uint32_t	width;		/* of one entry, and its alignment */
} sm_array_t;

struct apfs_sm_pager {
	apfs_container_t *container;
	const apfs_spaceman_t *sm;	/* the container's checked copy */
	sm_geometry_t	 geometry;
	apfs_sm_chunk_t	 chunk;		/* the chunk last visited */
	apfs_cib_t	 cab_header;
	apfs_cib_t	 cib_header;
	uint64_t	 array_block;	/* its block in array, or UINT64_MAX */
	uint64_t	 next_chunk;
	uint64_t	 free_sum;	/* of the chunks visited */
	unsigned char	*array;
	unsigned char	*cab;
	unsigned char	*cib;
	unsigned char	*bitmap;
	bool		 bitmap_counted; /* bitmap holds chunk's, recounted */
};

static const char *const region_roles[APFS_FIXED_REGIONS] = {
	[APFS_REGION_BLOCK0] = "block 0",
	[APFS_REGION_DESCRIPTORS] = "checkpoint descriptor area",
	[APFS_REGION_DATA] = "checkpoint data area",
	[APFS_REGION_POOL_RING] = "internal-pool bitmap ring",
	[APFS_REGION_POOL] = "internal pool"
};

static uint64_t	ceil_div(uint64_t, uint64_t);
static uint64_t	native_quotient(uint64_t, uint64_t);
static void	sm_geometry(uint32_t, uint64_t, sm_geometry_t *);
static void	sm_arrays(const apfs_spaceman_t *, sm_array_t[ARRAYS]);
static uint64_t	sm_pack(apfs_spaceman_t *);
static uint64_t	sm_implied_blocks(const apfs_spaceman_t *, uint32_t);
static uint64_t	bits_set(const unsigned char *, uint64_t);
static fsimg_status_t	array_entry(apfs_sm_pager_t *, uint64_t, uint32_t,
			    uint64_t *, fsimg_error_t *);
static fsimg_status_t	structure_read(apfs_sm_pager_t *, uint16_t, uint64_t,
			    uint64_t, uint64_t, fsimg_error_t *);
static fsimg_status_t	cib_load(apfs_sm_pager_t *, uint64_t,
			    fsimg_error_t *);
static fsimg_status_t	chunk_check(const apfs_sm_pager_t *,
			    const apfs_chunk_info_t *, uint64_t,
			    fsimg_error_t *);

/* ceil(x / k) without the overflow of x + k - 1. */
static uint64_t
ceil_div(uint64_t x, uint64_t k)
{
	return (x / k + (x % k != 0));
}

/* q(x, k) of the writer values: ceil(x / k), except that 2 becomes 3. */
static uint64_t
native_quotient(uint64_t x, uint64_t k)
{
	uint64_t q = ceil_div(x, k);

	return (q == 2 ? 3 : q);
}

/*
 * The [A1] relations, with E12's address-block threshold: no address block
 * while one level of chunk-information blocks fits in one address block's
 * worth, and otherwise enough address blocks to name them all.
 */
static void
sm_geometry(uint32_t block_size, uint64_t block_count, sm_geometry_t *g)
{
	g->blocks_per_chunk = 8 * block_size;
	g->chunks_per_cib = (block_size - APFS_CIB_HEADER_BYTES) /
	    APFS_CHUNK_INFO_BYTES;
	g->cibs_per_cab = (block_size - APFS_CAB_HEADER_BYTES) /
	    APFS_CAB_ENTRY_BYTES;
	g->chunks = ceil_div(block_count, g->blocks_per_chunk);
	g->cibs = ceil_div(g->chunks, g->chunks_per_cib);
	g->cabs = g->cibs <= g->cibs_per_cab ? 0 :
	    ceil_div(g->cibs, g->cibs_per_cab);
}

/*
 * The pool ring's XID array (one u64 per bitmap block), its bitmap-index
 * array (one u16 per bitmap block), its free-next array (one u16 per ring
 * slot) and the main device's address array, which names address blocks
 * when the device has any and chunk-information blocks otherwise. Counts
 * are u32 fields, so no length overflows.
 */
static void
sm_arrays(const apfs_spaceman_t *sm, sm_array_t arrays[ARRAYS])
{
	const apfs_sm_device_t *main = &sm->devices[APFS_SM_DEVICE_MAIN];
	uint64_t addresses;

	addresses = main->cab_count != 0 ? main->cab_count : main->cib_count;
	arrays[ARRAY_XID] = (sm_array_t){ .offset = sm->ip_bm_xid_offset,
	    .bytes = 8ull * sm->ip_bm_size_in_blocks, .width = 8 };
	arrays[ARRAY_INDEX] = (sm_array_t){ .offset = sm->ip_bitmap_offset,
	    .bytes = 2ull * sm->ip_bm_size_in_blocks, .width = 2 };
	arrays[ARRAY_FREE_NEXT] = (sm_array_t){
	    .offset = sm->ip_bm_free_next_offset,
	    .bytes = 2ull * sm->ip_bm_block_count, .width = 2 };
	arrays[ARRAY_ADDRESS] = (sm_array_t){ .offset = main->addr_offset,
	    .bytes = 8ull * addresses, .width = 8 };
}

/*
 * The smallest whole number of blocks holding the fixed structure and the
 * arrays at the offsets stated, whether or not those offsets are legal.
 */
static uint64_t
sm_implied_blocks(const apfs_spaceman_t *sm, uint32_t block_size)
{
	sm_array_t arrays[ARRAYS];
	uint64_t end = sm->structure_end;

	sm_arrays(sm, arrays);
	for (unsigned i = 0; i < ARRAYS; i++)
		if (end < arrays[i].offset + arrays[i].bytes)
			end = arrays[i].offset + arrays[i].bytes;
	return (ceil_div(end, block_size));
}

/*
 * The native packing, which every E1 and E12 dump and every older-layout
 * specimen shows: the arrays in sm_arrays() order from the end of the
 * fixed structure in either layout, the free-next array at the next
 * multiple of 8, then the tier-2 address array, empty. Returns the end of
 * the arrays.
 */
static uint64_t
sm_pack(apfs_spaceman_t *sm)
{
	sm_array_t arrays[ARRAYS];
	uint64_t end;

	sm->ip_bm_xid_offset = sm->structure_end;
	sm_arrays(sm, arrays);
	sm->ip_bitmap_offset = (uint32_t)(sm->ip_bm_xid_offset +
	    arrays[ARRAY_XID].bytes);
	end = sm->ip_bitmap_offset + arrays[ARRAY_INDEX].bytes;
	sm->ip_bm_free_next_offset = (uint32_t)(ceil_div(end, 8) * 8);
	sm->devices[APFS_SM_DEVICE_MAIN].addr_offset =
	    (uint32_t)(sm->ip_bm_free_next_offset +
	    arrays[ARRAY_FREE_NEXT].bytes);
	end = sm->devices[APFS_SM_DEVICE_MAIN].addr_offset +
	    arrays[ARRAY_ADDRESS].bytes;
	sm->devices[APFS_SM_DEVICE_TIER2].addr_offset = (uint32_t)end;
	return (end);
}

fsimg_status_t
apfs_spaceman_check(const apfs_spaceman_t *sm, uint32_t block_size,
    uint64_t block_count, uint64_t *blocks, fsimg_error_t *error)
{
	const apfs_sm_device_t *main = &sm->devices[APFS_SM_DEVICE_MAIN];
	const apfs_sm_device_t *tier2 = &sm->devices[APFS_SM_DEVICE_TIER2];
	apfs_spaceman_t packed;
	sm_geometry_t g;
	sm_array_t arrays[ARRAYS];
	uint64_t packed_end;

	if (block_count == 0)
		return (apfs_corrupt(error, ROLE_SM, "has an empty device"));
	sm_geometry(block_size, block_count, &g);
	if (sm->block_size != block_size ||
	    sm->blocks_per_chunk != g.blocks_per_chunk ||
	    sm->chunks_per_cib != g.chunks_per_cib ||
	    sm->cibs_per_cab != g.cibs_per_cab)
		return (apfs_corrupt(error, ROLE_SM,
		    "has the wrong chunk geometry"));
	if (main->block_count != block_count || main->chunk_count != g.chunks ||
	    main->cib_count != g.cibs || main->free_count > block_count)
		return (apfs_corrupt(error, ROLE_SM,
		    "has device counts the container does not give it"));
	if (main->cab_count != g.cabs) {
		/* E12 bisected the threshold at these block sizes only. */
		if (block_size == 4096 || block_size == 16384)
			return (apfs_corrupt(error, ROLE_SM,
			    "has the wrong address-block count"));
		return (apfs_unsupported(error, ROLE_SM,
		    "has an address-block count unproven at its block size"));
	}
	if (tier2->block_count != 0 || tier2->chunk_count != 0 ||
	    tier2->cib_count != 0 || tier2->cab_count != 0 ||
	    tier2->free_count != 0)
		return (apfs_corrupt(error, ROLE_SM, "has a tier-2 device"));
	/*
	 * The pool's bitmap covers the pool, and the ring holds at least the
	 * current bitmap; its u16 slot links end at APFS_SM_RING_FREE_END.
	 */
	if (sm->ip_block_count == 0 ||
	    sm->ip_bm_size_in_blocks !=
	    ceil_div(sm->ip_block_count, g.blocks_per_chunk) ||
	    sm->ip_bm_block_count < sm->ip_bm_size_in_blocks ||
	    sm->ip_bm_block_count >= APFS_SM_RING_FREE_END)
		return (apfs_corrupt(error, ROLE_SM,
		    "has a pool bitmap that does not fit its pool"));

	/*
	 * The decoder recognizes the older layout by its arrays, so what it
	 * reads as the current layout may hold any structure size. Only 2520
	 * is known. A larger size with every array past it, in a space manager
	 * the rules above tie to the container, may be a later layout. Any
	 * other size describes no layout, as a damaged size or a damaged older
	 * layout's array offset gives.
	 */
	if (sm->structure_end == APFS_SM_BYTES &&
	    sm->struct_size != APFS_SM_BYTES) {
		if (sm->struct_size > APFS_SM_BYTES &&
		    apfs_spaceman_lowest_array(sm) >= sm->struct_size)
			return (apfs_unsupported(error, ROLE_SM,
			    "has the structure size of an unknown layout"));
		return (apfs_corrupt(error, ROLE_SM,
		    "has an invalid structure size"));
	}

	/*
	 * The arrays may lie anywhere past the structure, but end no later
	 * than the native packing of the same counts does, so the counts
	 * checked above bound the object's size. Only fresh containers show
	 * the packing, so a layout past it is refused as unsupported, not
	 * corrupt (specs/design-apfs.md: provisional, E18).
	 */
	packed = *sm;
	packed_end = sm_pack(&packed);
	sm_arrays(sm, arrays);
	for (unsigned i = 0; i < ARRAYS; i++) {
		if (arrays[i].offset + arrays[i].bytes > packed_end)
			return (apfs_unsupported(error, ROLE_SM,
			    "has an array past its packed extent"));
		if (arrays[i].offset < sm->structure_end ||
		    arrays[i].offset % arrays[i].width != 0)
			return (apfs_corrupt(error, ROLE_SM,
			    "has an array inside its structure or unaligned"));
		for (unsigned j = 0; j < i; j++) {
			const sm_array_t *a = &arrays[i], *b = &arrays[j];

			if (a->offset < b->offset + b->bytes &&
			    b->offset < a->offset + a->bytes)
				return (apfs_corrupt(error, ROLE_SM,
				    "has overlapping arrays"));
		}
	}
	*blocks = sm_implied_blocks(sm, block_size);
	return (FSIMG_OK);
}

fsimg_status_t
apfs_spaceman_read(apfs_container_t *container, unsigned char *window,
    fsimg_error_t *error)
{
	apfs_spaceman_t *sm = &container->spaceman;
	apfs_cpm_t map;
	apfs_ref_t ref;
	uint64_t blocks, again;
	uint32_t block_size = container->block_size;
	fsimg_status_t status;
	bool found;

	status = apfs_ephemeral_find(container, container->nx.spaceman_oid,
	    &found, &map, error);
	if (status != FSIMG_OK)
		return (status);
	if (!found || map.kind != APFS_KIND_SPACEMAN)
		return (apfs_corrupt(error, ROLE_SM,
		    "is not mapped by the checkpoint"));

	if ((container->nx.incompat_features & APFS_NX_INCOMPAT_FUSION) != 0)
		return (apfs_unsupported(error, ROLE_SM,
		    "manages a Fusion container"));

	/*
	 * The size comes before the read: the first block, unverified, must
	 * pass the rules, which bound the size its arrays imply by the
	 * container's geometry, and the mapped size must equal that size. A
	 * damaged first block is therefore reported by the rule it breaks
	 * rather than by its checksum. The rules run again on the
	 * checksummed copy.
	 */
	status = apfs_block_read(container, map.address, window, ROLE_SM,
	    error);
	if (status != FSIMG_OK)
		return (status);
	if (!apfs_spaceman_decode(window, block_size, sm))
		return (apfs_corrupt(error, ROLE_SM, "is malformed"));
	status = apfs_spaceman_check(sm, block_size, container->block_count,
	    &blocks, error);
	if (status != FSIMG_OK)
		return (status);

	/* Offsets and counts are u32 fields: blocks is far below 2^32. */
	ref = (apfs_ref_t){ .role = ROLE_SM, .oid = container->nx.spaceman_oid,
	    .xid = container->xid, .blocks = (uint32_t)blocks,
	    .storage = APFS_OBJ_EPHEMERAL, .subtype = APFS_SUBTYPE_NONE,
	    .kind = APFS_KIND_SPACEMAN };
	status = apfs_object_read(container, &ref, window, NULL, error);
	if (status != FSIMG_OK)
		return (status);
	if (!apfs_spaceman_decode(window, block_size, sm))
		return (apfs_corrupt(error, ROLE_SM, "is malformed"));
	status = apfs_spaceman_check(sm, block_size, container->block_count,
	    &again, error);
	if (status != FSIMG_OK)
		return (status);
	if (again != blocks)
		return (apfs_corrupt(error, ROLE_SM, "changed while read"));
	container->spaceman_address = map.address;
	return (FSIMG_OK);
}

const char *
apfs_region_role(apfs_region_kind_t kind)
{
	return (kind < APFS_FIXED_REGIONS ? region_roles[kind] : "region");
}

fsimg_status_t
apfs_fixed_regions(const apfs_nx_t *nx, const apfs_spaceman_t *sm,
    apfs_region_t regions[APFS_FIXED_REGIONS], fsimg_error_t *error)
{
	uint64_t n = nx->block_count;
	apfs_region_t region;
	unsigned j;

	regions[APFS_REGION_BLOCK0] = (apfs_region_t){ .first = 0,
	    .count = 1, .kind = APFS_REGION_BLOCK0 };
	regions[APFS_REGION_DESCRIPTORS] = (apfs_region_t){
	    .first = nx->desc_base, .count = nx->desc_blocks,
	    .kind = APFS_REGION_DESCRIPTORS };
	regions[APFS_REGION_DATA] = (apfs_region_t){ .first = nx->data_base,
	    .count = nx->data_blocks, .kind = APFS_REGION_DATA };
	regions[APFS_REGION_POOL_RING] = (apfs_region_t){
	    .first = sm->ip_bm_base, .count = sm->ip_bm_block_count,
	    .kind = APFS_REGION_POOL_RING };
	regions[APFS_REGION_POOL] = (apfs_region_t){ .first = sm->ip_base,
	    .count = sm->ip_block_count, .kind = APFS_REGION_POOL };

	/* Insertion sort by first block; a damaged container may reorder. */
	for (unsigned i = 1; i < APFS_FIXED_REGIONS; i++) {
		region = regions[i];
		for (j = i; j > 0 && regions[j - 1].first > region.first; j--)
			regions[j] = regions[j - 1];
		regions[j] = region;
	}
	for (unsigned i = 0; i < APFS_FIXED_REGIONS; i++) {
		if (regions[i].count == 0 || regions[i].first >= n ||
		    regions[i].count > n - regions[i].first)
			return (apfs_corrupt(error,
			    region_roles[regions[i].kind],
			    "lies outside the container"));
		/* Sorted and inside [0, N): first + count cannot overflow. */
		if (i > 0 && regions[i].first <
		    regions[i - 1].first + regions[i - 1].count)
			return (apfs_corrupt(error,
			    region_roles[regions[i].kind],
			    "overlaps another fixed region"));
	}
	return (FSIMG_OK);
}

bool
apfs_native_geometry(uint64_t block_count, apfs_nx_t *nx, apfs_spaceman_t *sm)
{
	apfs_sm_device_t *main = &sm->devices[APFS_SM_DEVICE_MAIN];
	sm_geometry_t g;
	uint64_t pool, bitmap, ring, sm_blocks, volumes, main_limit, pool_limit;
	uint64_t ephemeral_factor, estimate, desc_blocks;

	if (block_count == 0 || block_count > APFS_WRITER_BLOCKS_MAX)
		return (false);
	bzero(nx, sizeof(*nx));
	bzero(sm, sizeof(*sm));

	/*
	 * The space manager, in the current layout, the only one the writer
	 * emits: N bounds every count below u32.
	 */
	sm_geometry(APFS_WRITER_BLOCK_SIZE, block_count, &g);
	sm->structure_end = APFS_SM_BYTES;
	sm->struct_size = APFS_SM_BYTES;
	pool = NATIVE_POOL_FACTOR * (g.chunks + g.cibs + g.cabs);
	bitmap = ceil_div(pool, g.blocks_per_chunk);
	ring = NATIVE_RING_MULTIPLIER * bitmap;
	sm->block_size = APFS_WRITER_BLOCK_SIZE;
	sm->blocks_per_chunk = g.blocks_per_chunk;
	sm->chunks_per_cib = g.chunks_per_cib;
	sm->cibs_per_cab = g.cibs_per_cab;
	main->block_count = block_count;
	main->chunk_count = g.chunks;
	main->cib_count = (uint32_t)g.cibs;
	main->cab_count = (uint32_t)g.cabs;
	sm->ip_block_count = pool;
	sm->ip_bm_tx_multiplier = NATIVE_RING_MULTIPLIER;
	sm->ip_bm_size_in_blocks = (uint32_t)bitmap;
	sm->ip_bm_block_count = (uint32_t)ring;
	sm_blocks = ceil_div(sm_pack(sm), APFS_WRITER_BLOCK_SIZE);

	/*
	 * The free-queue node limits L (main) and I (pool), the volume count
	 * F, and M of the ephemeral-information word, which sizes the
	 * checkpoint areas through E.
	 */
	volumes = ceil_div(block_count, NATIVE_VOLUME_BYTES /
	    APFS_WRITER_BLOCK_SIZE);
	if (volumes > NATIVE_VOLUMES_MAX)
		volumes = NATIVE_VOLUMES_MAX;
	if (block_count < (1ull << 18))
		main_limit = native_quotient(block_count, 4544);
	else if (block_count < (1ull << 20))
		main_limit = native_quotient(block_count, 2272);
	else
		main_limit = NATIVE_QUEUE_LIMIT_MAX;
	pool_limit = native_quotient(pool, 1136);
	if (pool_limit > NATIVE_QUEUE_LIMIT_MAX)
		pool_limit = NATIVE_QUEUE_LIMIT_MAX;
	ephemeral_factor = block_count <= NATIVE_SMALL_BLOCKS ? main_limit : 8;
	estimate = main_limit + pool_limit + 2 * sm_blocks + 1 +
	    8 * ephemeral_factor * volumes;
	sm->free_queues[APFS_SM_FQ_INTERNAL_POOL].node_limit =
	    (uint16_t)pool_limit;
	sm->free_queues[APFS_SM_FQ_MAIN].node_limit = (uint16_t)main_limit;

	/* The superblock: the areas from block 1, then the ring and pool. */
	desc_blocks = 4 * (ceil_div(estimate, NATIVE_MAPPINGS_PER_BLOCK) + 1);
	nx->block_size = APFS_WRITER_BLOCK_SIZE;
	nx->block_count = block_count;
	nx->max_volumes = (uint32_t)volumes;
	nx->ephemeral_info[0] = (ephemeral_factor << 32) |
	    NATIVE_EPHEMERAL_WORD0;
	nx->desc_base = 1;
	nx->desc_blocks = (uint32_t)desc_blocks;
	nx->data_base = nx->desc_base + desc_blocks;
	nx->data_blocks = (uint32_t)(4 * estimate);
	sm->ip_bm_base = nx->data_base + nx->data_blocks;
	sm->ip_base = sm->ip_bm_base + ring;
	return (true);
}

/*
 * The set bits among the first count bits, least significant first: whole
 * 64-bit words by the classic SWAR population count (pairs, nibbles, then
 * bytes summed by the multiply), the remainder bit by bit.
 */
static uint64_t
bits_set(const unsigned char *bits, uint64_t count)
{
	uint64_t total = 0, i, x;

	for (i = 0; i + 64 <= count; i += 64) {
		x = fsimg_load_le64(bits + i / 8);
		x -= (x >> 1) & 0x5555555555555555ull;
		x = (x & 0x3333333333333333ull) +
		    ((x >> 2) & 0x3333333333333333ull);
		x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0full;
		total += (x * 0x0101010101010101ull) >> 56;
	}
	for (; i < count; i++)
		total += (bits[i / 8] >> (i % 8)) & 1u;
	return (total);
}

fsimg_status_t
apfs_sm_pager_open(apfs_container_t *container, apfs_sm_pager_t **pagerp,
    fsimg_error_t *error)
{
	apfs_sm_pager_t *pager;
	fsimg_ctx_t *ctx = container->reader->ctx;
	size_t block_size = container->block_size;
	fsimg_error_t detail;

	*pagerp = NULL;
	pager = fsimg_alloc(ctx, sizeof(*pager), &detail);
	if (pager == NULL)
		goto fail;
	*pager = (apfs_sm_pager_t){ .container = container,
	    .sm = &container->spaceman, .array_block = UINT64_MAX };
	pager->array = fsimg_alloc(ctx, block_size, &detail);
	if (pager->array == NULL)
		goto fail;
	pager->cab = fsimg_alloc(ctx, block_size, &detail);
	if (pager->cab == NULL)
		goto fail;
	pager->cib = fsimg_alloc(ctx, block_size, &detail);
	if (pager->cib == NULL)
		goto fail;
	pager->bitmap = fsimg_alloc(ctx, block_size, &detail);
	if (pager->bitmap == NULL)
		goto fail;
	sm_geometry(container->block_size, container->block_count,
	    &pager->geometry);
	*pagerp = pager;
	return (FSIMG_OK);

fail:
	apfs_sm_pager_free(pager);
	if (error != NULL)
		*error = detail;
	return (detail.status);
}

void
apfs_sm_pager_free(apfs_sm_pager_t *pager)
{
	if (pager == NULL)
		return;
	fsimg_dealloc(pager->array);
	fsimg_dealloc(pager->cab);
	fsimg_dealloc(pager->cib);
	fsimg_dealloc(pager->bitmap);
	fsimg_dealloc(pager);
}

/*
 * One entry of an array of the space manager, at an offset inside it that
 * apfs_spaceman_check() proved aligned and inside the object, so an entry
 * never straddles two blocks. The block is read again, without the
 * checksum, which covered it when the checkpoint was loaded; every value
 * read is judged by its caller.
 */
static fsimg_status_t
array_entry(apfs_sm_pager_t *pager, uint64_t offset, uint32_t width,
    uint64_t *value, fsimg_error_t *error)
{
	apfs_container_t *container = pager->container;
	uint64_t block = offset / container->block_size;
	const unsigned char *p;
	fsimg_status_t status;

	if (block != pager->array_block) {
		pager->array_block = UINT64_MAX;
		status = fsimg_session_charge(&container->reader->session, 0, 0,
		    1, error);
		if (status == FSIMG_OK)
			status = apfs_block_read(container,
			    container->spaceman_address + block, pager->array,
			    ROLE_SM, error);
		if (status != FSIMG_OK)
			return (status);
		pager->array_block = block;
	}
	p = pager->array + offset % container->block_size;
	*value = width == 8 ? fsimg_load_le64(p) : fsimg_load_le16(p);
	return (FSIMG_OK);
}

/*
 * Read the index-th address block (kind APFS_KIND_SPACEMAN_CAB) or
 * chunk-information block of the device into its window. It must say it
 * is the index-th and hold count entries; the index rule keeps a block
 * named twice from being visited twice.
 */
static fsimg_status_t
structure_read(apfs_sm_pager_t *pager, uint16_t kind, uint64_t address,
    uint64_t index, uint64_t count, fsimg_error_t *error)
{
	apfs_container_t *container = pager->container;
	bool cab = kind == APFS_KIND_SPACEMAN_CAB;
	const char *role = cab ? ROLE_CAB : ROLE_CIB;
	unsigned char *window = cab ? pager->cab : pager->cib;
	apfs_cib_t *header = cab ? &pager->cab_header : &pager->cib_header;
	apfs_ref_t ref;
	fsimg_status_t status;
	bool decoded;

	ref = (apfs_ref_t){ .role = role, .oid = address,
	    .xid = container->xid, .storage = APFS_OBJ_PHYSICAL,
	    .subtype = APFS_SUBTYPE_NONE, .kind = kind };
	status = apfs_object_read(container, &ref, window, NULL, error);
	if (status != FSIMG_OK)
		return (status);
	decoded = cab ? apfs_cab_decode(window, container->block_size, header) :
	    apfs_cib_decode(window, container->block_size, header);
	if (!decoded)
		return (apfs_corrupt(error, role, "is malformed"));
	if (header->index != index)
		return (apfs_corrupt(error, role, "is out of order"));
	if (header->count != count)
		return (apfs_corrupt(error, role, "has the wrong entry count"));
	return (FSIMG_OK);
}

/*
 * Load chunk-information block cib_index, reading first the address block
 * that lists it when it is the first that block lists.
 */
static fsimg_status_t
cib_load(apfs_sm_pager_t *pager, uint64_t cib_index, fsimg_error_t *error)
{
	const sm_geometry_t *g = &pager->geometry;
	uint64_t array = pager->sm->devices[APFS_SM_DEVICE_MAIN].addr_offset;
	uint64_t address, cab_index, cibs_listed, chunks_listed;
	uint32_t slot;
	fsimg_status_t status = FSIMG_OK;

	if (g->cabs == 0) {
		status = array_entry(pager, array + 8 * cib_index, 8, &address,
		    error);
	} else {
		cab_index = cib_index / g->cibs_per_cab;
		slot = (uint32_t)(cib_index % g->cibs_per_cab);
		if (slot == 0) {
			cibs_listed = g->cibs - cab_index * g->cibs_per_cab;
			if (cibs_listed > g->cibs_per_cab)
				cibs_listed = g->cibs_per_cab;
			status = array_entry(pager, array + 8 * cab_index, 8,
			    &pager->chunk.cab_address, error);
			if (status == FSIMG_OK)
				status = structure_read(pager,
				    APFS_KIND_SPACEMAN_CAB,
				    pager->chunk.cab_address, cab_index,
				    cibs_listed, error);
		}
		/* Defensive: the address block's count covers the slot. */
		if (status == FSIMG_OK && !apfs_cab_address_decode(pager->cab,
		    pager->container->block_size, &pager->cab_header, slot,
		    &address))
			status = apfs_corrupt(error, ROLE_CAB, "is malformed");
	}
	if (status != FSIMG_OK)
		return (status);
	pager->chunk.cib_address = address;
	chunks_listed = g->chunks - cib_index * g->chunks_per_cib;
	if (chunks_listed > g->chunks_per_cib)
		chunks_listed = g->chunks_per_cib;
	return (structure_read(pager, APFS_KIND_SPACEMAN_CIB, address,
	    cib_index, chunks_listed, error));
}

/* The rules of the record of chunk number index. */
static fsimg_status_t
chunk_check(const apfs_sm_pager_t *pager, const apfs_chunk_info_t *info,
    uint64_t index, fsimg_error_t *error)
{
	const apfs_container_t *container = pager->container;
	uint64_t first = index * pager->geometry.blocks_per_chunk;
	uint64_t blocks = container->block_count - first;

	if (blocks > pager->geometry.blocks_per_chunk)
		blocks = pager->geometry.blocks_per_chunk;
	if (info->address != first || info->block_count != blocks)
		return (apfs_corrupt(error, ROLE_CHUNK,
		    "does not cover the blocks its position gives it"));
	if (info->free_count > info->block_count)
		return (apfs_corrupt(error, ROLE_CHUNK,
		    "has more free blocks than blocks"));
	if (info->xid > container->xid)
		return (apfs_corrupt(error, ROLE_CHUNK,
		    "is newer than the checkpoint"));
	if (info->bitmap_address == 0 ?
	    info->free_count != info->block_count :
	    info->bitmap_address >= container->block_count)
		return (apfs_corrupt(error, ROLE_CHUNK,
		    "has a bitmap address its counts do not allow"));
	return (FSIMG_OK);
}

fsimg_status_t
apfs_sm_pager_next(apfs_sm_pager_t *pager, apfs_sm_chunk_t *chunk,
    bool *found, fsimg_error_t *error)
{
	const sm_geometry_t *g = &pager->geometry;
	uint64_t index = pager->next_chunk;
	uint64_t cib_index = index / g->chunks_per_cib;
	uint32_t slot = (uint32_t)(index % g->chunks_per_cib);
	apfs_chunk_info_t info;
	fsimg_status_t status;

	*found = false;
	if (index == g->chunks) {
		if (pager->free_sum !=
		    pager->sm->devices[APFS_SM_DEVICE_MAIN].free_count)
			return (apfs_corrupt(error, ROLE_SM,
			    "free count differs from its chunks'"));
		return (FSIMG_OK);
	}
	status = fsimg_session_charge(&pager->container->reader->session, 0,
	    0, 1, error);
	if (status == FSIMG_OK && slot == 0)
		status = cib_load(pager, cib_index, error);
	if (status != FSIMG_OK)
		return (status);
	/* Defensive: the CIB's count was checked against the geometry. */
	if (!apfs_chunk_info_decode(pager->cib, pager->container->block_size,
	    &pager->cib_header, slot, &info))
		return (apfs_corrupt(error, ROLE_CIB, "is malformed"));
	status = chunk_check(pager, &info, index, error);
	if (status != FSIMG_OK)
		return (status);

	pager->chunk.info = info;
	pager->chunk.index = index;
	pager->chunk.first_of_cib = slot == 0;
	pager->chunk.first_of_cab = g->cabs != 0 && slot == 0 &&
	    cib_index % g->cibs_per_cab == 0;
	pager->bitmap_counted = false;
	pager->free_sum += info.free_count;	/* each at most its chunk's */
	pager->next_chunk = index + 1;
	*chunk = pager->chunk;
	*found = true;
	return (FSIMG_OK);
}

fsimg_status_t
apfs_sm_pager_bitmap(apfs_sm_pager_t *pager, const unsigned char **bits,
    fsimg_error_t *error)
{
	const apfs_chunk_info_t *info = &pager->chunk.info;
	apfs_container_t *container = pager->container;
	fsimg_status_t status;

	*bits = NULL;
	if (pager->next_chunk == 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_READER,
		    "no space-manager chunk visited"));
	if (info->bitmap_address == 0)
		return (FSIMG_OK);
	if (!pager->bitmap_counted) {
		status = fsimg_session_charge(&container->reader->session, 0, 0,
		    1, error);
		if (status == FSIMG_OK)
			status = apfs_block_read(container,
			    info->bitmap_address, pager->bitmap, ROLE_BITMAP,
			    error);
		if (status != FSIMG_OK)
			return (status);
		if (info->block_count - bits_set(pager->bitmap,
		    info->block_count) != info->free_count)
			return (apfs_corrupt(error, ROLE_CHUNK,
			    "free count differs from its bitmap"));
		pager->bitmap_counted = true;
	}
	*bits = pager->bitmap;
	return (FSIMG_OK);
}

fsimg_status_t
apfs_sm_pager_pool_bitmap(apfs_sm_pager_t *pager, uint32_t k,
    uint64_t *block, const unsigned char **bits, fsimg_error_t *error)
{
	const apfs_spaceman_t *sm = pager->sm;
	apfs_container_t *container = pager->container;
	uint64_t slot, xid;
	fsimg_status_t status;

	*bits = NULL;
	if (k >= sm->ip_bm_size_in_blocks)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_READER,
		    "internal-pool bitmap block out of range"));
	status = array_entry(pager, sm->ip_bitmap_offset + 2ull * k, 2, &slot,
	    error);
	if (status == FSIMG_OK)
		status = array_entry(pager, sm->ip_bm_xid_offset + 8ull * k, 8,
		    &xid, error);
	if (status != FSIMG_OK)
		return (status);
	if (slot >= sm->ip_bm_block_count || sm->ip_bm_base >=
	    container->block_count || slot >= container->block_count -
	    sm->ip_bm_base)
		return (apfs_corrupt(error, ROLE_POOL,
		    "names a slot outside its ring"));
	if (xid > container->xid)
		return (apfs_corrupt(error, ROLE_POOL,
		    "is newer than the checkpoint"));

	/* The window is the chunks' too: their bitmap is read again. */
	pager->bitmap_counted = false;
	status = fsimg_session_charge(&container->reader->session, 0, 0, 1,
	    error);
	if (status == FSIMG_OK)
		status = apfs_block_read(container, sm->ip_bm_base + slot,
		    pager->bitmap, ROLE_POOL, error);
	if (status != FSIMG_OK)
		return (status);
	*block = sm->ip_bm_base + slot;
	*bits = pager->bitmap;
	return (FSIMG_OK);
}
