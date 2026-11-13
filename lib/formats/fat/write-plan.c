/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "write.h"

/* A candidate explains its own rejection. Only agreement across every
 * candidate establishes one cause for a failed automatic cluster search.
 */
typedef enum fat_geometry_result {
	FAT_GEOMETRY_NONE,
	FAT_GEOMETRY_FITS,
	FAT_GEOMETRY_TOO_SMALL,
	FAT_GEOMETRY_TOO_LARGE,
	FAT_GEOMETRY_CAPACITY,
	FAT_GEOMETRY_SPACE,
	FAT_GEOMETRY_FIELD_LIMIT,
	FAT_GEOMETRY_NO_ROOM,
	FAT_GEOMETRY_OVERLAP,
	FAT_GEOMETRY_CONFLICT
} fat_geometry_result_t;

/* The cluster sizes a search can try: the powers of two from the sector to
 * 32 KiB, at most seven with 512-byte sectors.
 */
#define FAT_UNITS_MAX 7u
#define FAT_CLUSTER_MAX 32768u

/* The writer's reserved area without placements: the boot sector, and on
 * FAT32 also FSInfo and the backups at sectors 1, 6 and 7.
 */
#define FAT_RESERVED_SECTORS(variant) ((variant) == 32 ? 32u : 1u)

/* What one cluster size's allocation pass uses: the clusters the runs
 * occupy, and the cluster after the highest of them, which placement gaps
 * can move past allocated + 2.
 */
typedef struct fat_usage {
	uint64_t	 allocated;
	uint64_t	 end;
} fat_usage_t;

/* The next-fit allocation cursor of one cluster size: the next free
 * cluster, and the first placed run, in plan->placed order, that it has not
 * yet passed. A run that would overlap that placed run starts after it
 * instead, and the gap before the placed run stays free.
 */
typedef struct fat_cursor {
	uint64_t	 next;
	uint32_t	 placed;
	uint32_t	 cluster_bytes;
} fat_cursor_t;

/* One candidate cluster size's geometry; the data area follows the
 * reserved sectors, both FAT copies and the fixed root.
 */
typedef struct fat_candidate {
	uint64_t	 volume_bytes;
	uint64_t	 fat_bytes;
	uint32_t	 cluster_bytes;
	uint32_t	 clusters;
	uint32_t	 allocated;
	uint32_t	 reserved_sectors;
} fat_candidate_t;

static uint64_t divide_up(uint64_t, uint64_t);
static uint64_t root_sectors_entries(uint64_t, uint32_t);
static uint64_t table_sectors(unsigned, uint64_t, uint32_t);
static void sort_placements(fat_plan_t *);
static fsimg_status_t check_placements_matched(const fat_plan_t *,
    fsimg_error_t *);
static void placed_run(const fat_plan_t *, uint32_t, uint32_t, uint64_t *,
    uint64_t *);
static uint32_t placed_overlap(const fat_plan_t *, uint32_t);
static bool cursor_take(const fat_plan_t *, fat_cursor_t *, uint64_t,
    uint64_t *);
static uint64_t cursor_end(const fat_plan_t *, const fat_cursor_t *);
static fsimg_status_t count_clusters(fat_plan_t *, uint32_t, unsigned,
    fat_usage_t *, fsimg_error_t *);
static fsimg_status_t choose_root(fat_plan_t *, fsimg_error_t *);
static fsimg_status_t choose_geometry(fat_plan_t *, fsimg_error_t *);
static void placed_shape(const fat_plan_t *, uint32_t, uint64_t,
    uint64_t *, uint64_t *, uint64_t *);
static fat_geometry_result_t try_geometry(const fat_plan_t *, uint32_t,
    const fat_usage_t *, fat_candidate_t *);
static void apply_geometry(fat_plan_t *, const fat_candidate_t *);
static fsimg_status_t geometry_error(const fat_plan_t *, fat_geometry_result_t,
    uint32_t, fsimg_error_t *);
static fsimg_status_t allocate_layout(fat_plan_t *, fsimg_error_t *);

static uint64_t
divide_up(uint64_t value, uint64_t unit)
{
	return (value / unit + (value % unit != 0 ? 1 : 0));
}

/* The sectors one FAT copy needs for clusters data clusters and the two
 * reserved entries.
 */
static uint64_t
table_sectors(unsigned variant, uint64_t clusters, uint32_t sector_bytes)
{
	return (divide_up(divide_up((clusters + 2) * variant, 8), sector_bytes));
}

/* entries rounded up to whole sectors of 32-byte directory entries: the
 * only capacities a FAT12/16 fixed root region can have.
 */
static uint64_t
root_sectors_entries(uint64_t entries, uint32_t sector_bytes)
{
	uint64_t per_sector = sector_bytes / 32;

	return (divide_up(entries, per_sector) * per_sector);
}

fsimg_status_t
fsimg_fat_layout_get(fat_plan_t *plan, uint64_t id, fat_layout_t *row,
    fsimg_error_t *error)
{
	unsigned char raw[FAT_LAYOUT_BYTES];
	fsimg_status_t status;

	bzero(row, sizeof(*row));
	status = fsimg_array_get(plan->layout, id, raw, error);
	if (status != FSIMG_OK)
		return (status);
	row->length = fsimg_load_le64(raw);
	row->slots = fsimg_load_le64(raw + 8);
	row->parent_slot = fsimg_load_le64(raw + 16);
	row->first = (uint32_t)fsimg_load_le64(raw + 24);
	row->clusters = (uint32_t)(fsimg_load_le64(raw + 24) >> 32);
	bcopy(raw + 32, row->entry, 32);
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_fat_layout_set(fat_plan_t *plan, uint64_t id, const fat_layout_t *row,
    fsimg_error_t *error)
{
	unsigned char raw[FAT_LAYOUT_BYTES];

	fsimg_store_le64(raw, row->length);
	fsimg_store_le64(raw + 8, row->slots);
	fsimg_store_le64(raw + 16, row->parent_slot);
	fsimg_store_le64(raw + 24, row->first | (uint64_t)row->clusters << 32);
	bcopy(row->entry, raw + 32, 32);
	return (fsimg_array_set(plan->layout, id, raw, error));
}

/* Copy the writer's placements into the plan sorted by offset, by
 * insertion over at most FSIMG_PLACEMENT_MAX entries. Common validation
 * has made the offsets distinct, so the order is total.
 */
static void
sort_placements(fat_plan_t *plan)
{
	const fsimg_write_options_t *options = &plan->common.writer->options;
	fat_placed_t item;
	uint32_t j;

	plan->placed_count = options->placement_count;
	for (uint32_t i = 0; i < plan->placed_count; i++) {
		item = (fat_placed_t){ .path = options->placements[i].path,
		    .offset = options->placements[i].offset };
		for (j = i; j > 0 && plan->placed[j - 1].offset > item.offset; j--)
			plan->placed[j] = plan->placed[j - 1];
		plan->placed[j] = item;
	}
}

/* Match one catalog entry of the names pass against the placements by
 * exact path, as the catalog spells it (specs/design-placement.md). The
 * paths are distinct, so at most one matches.
 */
fsimg_status_t
fsimg_fat_place_match(fat_plan_t *plan, const fsimg_catalog_entry_t *catalog,
    fsimg_error_t *error)
{
	for (uint32_t i = 0; i < plan->placed_count; i++) {
		if (strcmp(catalog->entry.path, plan->placed[i].path) != 0)
			continue;
		if (catalog->entry.kind != FSIMG_ENTRY_FILE ||
		    catalog->entry.size == 0)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_WRITER,
			    "place must name a nonempty regular file"));
		plan->placed[i].id = catalog->id;
		plan->placed[i].length = catalog->entry.size;
		break;
	}
	return (FSIMG_OK);
}

/* After the names pass every placement has met its entry. */
static fsimg_status_t
check_placements_matched(const fat_plan_t *plan, fsimg_error_t *error)
{
	for (uint32_t i = 0; i < plan->placed_count; i++) {
		if (plan->placed[i].id == 0)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_WRITER, "place names no file in the image"));
	}
	return (FSIMG_OK);
}

/* The index in plan->placed of the placement matched to catalog entry id,
 * or placed_count when the entry is not placed. The root, id 0, never is.
 */
uint32_t
fsimg_fat_placed_index(const fat_plan_t *plan, uint64_t id)
{
	uint32_t i;

	for (i = 0; i < plan->placed_count; i++) {
		if (plan->placed[i].id == id && id != 0)
			break;
	}
	return (i);
}

/* Placement i's run with clusters of cluster_bytes, from *first to *end
 * exclusive: the data area begins at the lowest offset as cluster 2.
 * Candidate sizes divide every offset, and sorting puts the lowest first, so
 * the difference cannot wrap. The quotient is below 2^56, and the names pass
 * refuses a file above UINT32_MAX bytes before matching it, so the run is
 * below 2^24 clusters: these bounds, not checks, keep both sums in range.
 */
static void
placed_run(const fat_plan_t *plan, uint32_t i, uint32_t cluster_bytes,
    uint64_t *first, uint64_t *end)
{
	*first = 2 + (plan->placed[i].offset - plan->placed[0].offset) /
	    cluster_bytes;
	*end = *first + divide_up(plan->placed[i].length, cluster_bytes);
}

/* The first placement whose run overlaps the next one's with clusters of
 * cluster_bytes, or placed_count when none does. Sorted by offset, the runs
 * are pairwise disjoint exactly when each ends where or before the next
 * begins.
 */
static uint32_t
placed_overlap(const fat_plan_t *plan, uint32_t cluster_bytes)
{
	uint64_t first, end, next_first, next_end;

	for (uint32_t i = 0; i + 1 < plan->placed_count; i++) {
		placed_run(plan, i, cluster_bytes, &first, &end);
		placed_run(plan, i + 1, cluster_bytes, &next_first, &next_end);
		if (end > next_first)
			return (i);
	}
	return (plan->placed_count);
}

/* Give the next unplaced run of clusters its first cluster by next fit,
 * passing over every placed run it would overlap; false if the cursor
 * would wrap. The cursor never stops inside a placed run, so no run it
 * gives can overlap one.
 */
static bool
cursor_take(const fat_plan_t *plan, fat_cursor_t *cursor, uint64_t clusters,
    uint64_t *first)
{
	uint64_t stop, start, end;

	for (;;) {
		if (!fsimg_add_u64(cursor->next, clusters, &stop))
			return (false);
		if (cursor->placed == plan->placed_count)
			break;
		placed_run(plan, cursor->placed, cursor->cluster_bytes, &start,
		    &end);
		if (stop <= start)
			break;
		if (end > cursor->next)
			cursor->next = end;
		cursor->placed++;
	}
	*first = cursor->next;
	cursor->next = stop;
	return (true);
}

/* The cluster after the highest run in use once the pass is complete: the
 * cursor's, or the highest placed run's if the cursor stopped before it.
 */
static uint64_t
cursor_end(const fat_plan_t *plan, const fat_cursor_t *cursor)
{
	uint64_t first, end;

	if (plan->placed_count == 0)
		return (cursor->next);
	placed_run(plan, plan->placed_count - 1, cursor->cluster_bytes, &first,
	    &end);
	return (end > cursor->next ? end : cursor->next);
}

/* What every candidate size's allocation uses, in one pass over the layout
 * rows: usage[i] with clusters of first << i bytes. The pass simulates
 * allocate_layout's next fit with one cursor per candidate, so placed runs
 * shape each candidate's usage as they will its layout. The FAT12/16 root
 * lives outside the data area and needs none.
 */
static fsimg_status_t
count_clusters(fat_plan_t *plan, uint32_t first, unsigned count,
    fat_usage_t *usage, fsimg_error_t *error)
{
	fat_cursor_t cursors[FAT_UNITS_MAX];
	fat_layout_t row;
	uint64_t length, clusters, start;
	fsimg_status_t status;
	bool is_placed;

	for (unsigned i = 0; i < count; i++) {
		usage[i].allocated = 0;
		cursors[i] = (fat_cursor_t){ .next = 2,
		    .cluster_bytes = first << i };
	}
	for (uint64_t id = 0; id <= plan->common.writer->count; id++) {
		status = fsimg_fat_layout_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		if (id == 0 && plan->variant != 32)
			continue;
		length = row.slots == 0 ? row.length : row.slots * 32;
		is_placed = fsimg_fat_placed_index(plan, id) != plan->placed_count;
		for (unsigned i = 0; i < count; i++) {
			clusters = divide_up(length, cursors[i].cluster_bytes);
			if (!fsimg_add_u64(usage[i].allocated, clusters,
			    &usage[i].allocated))
				return (fsimg_error_set(error, FSIMG_ERANGE, 0,
				    FSIMG_OP_WRITER, "FAT allocation total overflows"));
			/* Placed runs are fixed; every other run takes the next
			 * fit, whose first cluster sizing does not need.
			 */
			if (is_placed || clusters == 0)
				continue;
			if (!cursor_take(plan, &cursors[i], clusters, &start))
				return (fsimg_error_set(error, FSIMG_ERANGE, 0,
				    FSIMG_OP_WRITER, "FAT allocation end overflows"));
		}
	}
	for (unsigned i = 0; i < count; i++)
		usage[i].end = cursor_end(plan, &cursors[i]);
	return (FSIMG_OK);
}

/* The shape of a placed volume with clusters of cluster_bytes, needing
 * needed clusters: its cluster count, FAT sectors and total sectors. The
 * data area begins at the lowest placed offset, so the cluster count does
 * not depend on the FAT length: it is needed under automatic sizing, or the
 * whole clusters an exact size leaves after the data start, and the FAT
 * follows from it. Whether the area before the FATs is large enough is the
 * caller's check.
 */
static void
placed_shape(const fat_plan_t *plan, uint32_t cluster_bytes, uint64_t needed,
    uint64_t *clusters, uint64_t *fat_sectors, uint64_t *sectors)
{
	const fsimg_write_options_t *options = &plan->common.writer->options;
	uint32_t sector = options->sector_bytes;
	uint32_t spc = cluster_bytes / sector;
	uint64_t data_start = plan->placed[0].offset / sector;

	if (options->volume_bytes == 0) {
		*clusters = needed;
		*sectors = data_start + needed * spc;
	} else {
		*sectors = options->volume_bytes / sector;
		*clusters = *sectors > data_start ?
		    (*sectors - data_start) / spc : 0;
	}
	*fat_sectors = table_sectors(plan->variant, *clusters, sector);
}

/* The geometry with clusters of cluster_bytes holding the usage and the
 * free reserve, or why there is none; the plan is unchanged.
 *
 * Without placements the data area follows the normal reserved area. For
 * exact size, increasing FAT length decreases the number of data clusters
 * and increases table capacity. Binary search finds the first sufficient FAT
 * in at most 32 steps. Oversized metadata cannot be a valid candidate.
 *
 * With placements placed_shape fixes the clusters and the FAT, and the
 * reserved count takes the sectors between the boot sector and the FATs:
 * at least the normal count and within its 16-bit field.
 */
static fat_geometry_result_t
try_geometry(const fat_plan_t *plan, uint32_t cluster_bytes,
    const fat_usage_t *usage, fat_candidate_t *out)
{
	const fsimg_write_options_t *options;
	uint64_t minimum, maximum, needed, clusters, sectors, fat_sectors;
	uint64_t root_sectors, overhead, low, high, middle, capacity, data_sectors;
	uint64_t min_reserved, reserved, data_start;
	uint32_t sector, spc;

	options = &plan->common.writer->options;
	sector = options->sector_bytes;
	spc = cluster_bytes / sector;
	if (placed_overlap(plan, cluster_bytes) != plan->placed_count)
		return (FAT_GEOMETRY_OVERLAP);
	minimum = plan->variant == 12 ? 1 : plan->variant == 16 ? 4085 : 65525;
	maximum = plan->variant == 12 ? 4084 :
	    plan->variant == 16 ? 65524 : 0x0ffffff5;
	/* Microsoft's formatting recommendation keeps automatic volumes away
	 * from type cutovers that other implementations can misclassify. Exact
	 * requests retain every legal count; upper margins can advance the
	 * automatic search to its next permitted cluster size.
	 */
	if (options->volume_bytes == 0) {
		if (plan->variant != 12)
			minimum += FAT_AUTO_TYPE_MARGIN;
		if (plan->variant != 32)
			maximum -= FAT_AUTO_TYPE_MARGIN - 1;
	}
	/* The volume holds every run through the highest one and leaves the
	 * free reserve unallocated; placement gaps count as free, as the
	 * FSInfo free count says. Without placements the end is allocated + 2.
	 */
	needed = divide_up(options->min_free_bytes, cluster_bytes);
	if (!fsimg_add_u64(usage->allocated, needed, &needed))
		return (FAT_GEOMETRY_CAPACITY);
	if (needed < usage->end - 2)
		needed = usage->end - 2;
	if (needed < minimum)
		needed = minimum;
	if (needed > maximum)
		return (FAT_GEOMETRY_CAPACITY);
	min_reserved = FAT_RESERVED_SECTORS(plan->variant);
	reserved = min_reserved;
	root_sectors = (uint64_t)plan->root_entries * 32 / sector;
	overhead = min_reserved + root_sectors;
	if (plan->placed_count != 0)
		placed_shape(plan, cluster_bytes, needed, &clusters, &fat_sectors,
		    &sectors);
	else if (options->volume_bytes == 0) {
		clusters = needed;
		fat_sectors = table_sectors(plan->variant, clusters, sector);
		sectors = overhead + 2 * fat_sectors + clusters * spc;
	} else {
		sectors = options->volume_bytes / sector;
		if (sectors <= overhead + 2)
			return (FAT_GEOMETRY_TOO_SMALL);
		low = 1;
		high = (sectors - overhead) / 2;
		while (low < high) {
			middle = low + (high - low) / 2;
			data_sectors = sectors - overhead - 2 * middle;
			clusters = data_sectors / spc;
			capacity = middle * sector * 8 / plan->variant;
			if (capacity >= clusters + 2)
				high = middle;
			else
				low = middle + 1;
		}
		fat_sectors = low;
		clusters = (sectors - overhead - 2 * fat_sectors) / spc;
	}
	if (clusters < minimum)
		return (FAT_GEOMETRY_TOO_SMALL);
	if (clusters > maximum)
		return (FAT_GEOMETRY_TOO_LARGE);
	if (clusters < needed)
		return (FAT_GEOMETRY_SPACE);
	if (plan->placed_count != 0) {
		data_start = plan->placed[0].offset / sector;
		if (data_start < overhead + 2 * fat_sectors)
			return (FAT_GEOMETRY_NO_ROOM);
		reserved = data_start - root_sectors - 2 * fat_sectors;
	}
	if (sectors > UINT32_MAX || fat_sectors == 0 ||
	    (plan->variant != 32 && fat_sectors > UINT16_MAX) ||
	    reserved > UINT16_MAX)
		return (FAT_GEOMETRY_FIELD_LIMIT);
	*out = (fat_candidate_t){ .volume_bytes = sectors * sector,
	    .fat_bytes = fat_sectors * sector, .cluster_bytes = cluster_bytes,
	    .clusters = (uint32_t)clusters,
	    .allocated = (uint32_t)usage->allocated,
	    .reserved_sectors = (uint32_t)reserved };
	return (FAT_GEOMETRY_FITS);
}

static void
apply_geometry(fat_plan_t *plan, const fat_candidate_t *g)
{
	fsimg_plan_info_t *info = &plan->common.info;
	uint32_t sector = plan->common.writer->options.sector_bytes;

	plan->clusters = g->clusters;
	plan->allocated = g->allocated;
	plan->reserved_sectors = g->reserved_sectors;
	plan->fat_offset = (uint64_t)plan->reserved_sectors * sector;
	plan->fat_bytes = g->fat_bytes;
	plan->root_offset = plan->fat_offset + 2 * plan->fat_bytes;
	plan->data_offset = plan->root_offset + (uint64_t)plan->root_entries * 32;
	info->volume_bytes = g->volume_bytes;
	info->allocation_unit_bytes = g->cluster_bytes;
	info->allocation_units = g->clusters;
	info->root_entries = plan->root_entries;
	info->free_bytes = (uint64_t)(g->clusters - g->allocated) * g->cluster_bytes;
	info->allocated_bytes = (uint64_t)g->allocated * g->cluster_bytes;
}

/* smallest is the smallest candidate cluster size. Runs that overlap with
 * some size also overlap with every larger one, so when every candidate
 * failed on overlap, the smallest names a pair that does; the downgrade to
 * a conflict below only guards the array index.
 */
static fsimg_status_t
geometry_error(const fat_plan_t *plan, fat_geometry_result_t reason,
    uint32_t smallest, fsimg_error_t *error)
{
	const fsimg_write_options_t *options = &plan->common.writer->options;
	const char *placed_context;
	char cluster_context[48], message[256];
	uint32_t overlap;

	/* An explicit cluster size narrows the search. Its capacity failure does
	 * not establish that the source exceeds every supported cluster size.
	 * Placements narrow it too, and fix where the data area begins.
	 */
	cluster_context[0] = '\0';
	if (options->allocation_unit_bytes != 0)
		snprintf(cluster_context, sizeof(cluster_context), " with %u-byte clusters",
		    options->allocation_unit_bytes);
	placed_context = plan->placed_count != 0 ? " at the placed offsets" : "";
	overlap = plan->placed_count;
	if (reason == FAT_GEOMETRY_OVERLAP)
		overlap = placed_overlap(plan, smallest);
	if (reason == FAT_GEOMETRY_OVERLAP && overlap == plan->placed_count)
		reason = FAT_GEOMETRY_CONFLICT;

	switch (reason) {
	case FAT_GEOMETRY_TOO_SMALL:
	case FAT_GEOMETRY_TOO_LARGE:
		/* Automatic sizing derives a legal cluster count or rejects capacity
		 * or field limits; only explicit sizes reach these range failures.
		 */
		snprintf(message, sizeof(message), "image size %llu bytes is too %s for FAT%u%s (%u-byte sectors)",
		    (unsigned long long)options->volume_bytes,
		    reason == FAT_GEOMETRY_TOO_SMALL ? "small" : "large", plan->variant,
		    cluster_context, options->sector_bytes);
		break;
	case FAT_GEOMETRY_CAPACITY:
		snprintf(message, sizeof(message), "%s too large for FAT%u%s%s",
		    options->min_free_bytes == 0 ? "source tree is" :
		    "source tree and requested free space are", plan->variant, cluster_context,
		    placed_context);
		break;
	case FAT_GEOMETRY_SPACE:
		snprintf(message, sizeof(message), "image size %llu bytes cannot fit the source tree%s as FAT%u%s%s",
		    (unsigned long long)options->volume_bytes,
		    options->min_free_bytes == 0 ? "" : " and requested free space",
		    plan->variant, cluster_context, placed_context);
		break;
	case FAT_GEOMETRY_FIELD_LIMIT:
		snprintf(message, sizeof(message), "required image exceeds FAT%u size limits with %u-byte sectors%s",
		    plan->variant, options->sector_bytes, placed_context);
		break;
	case FAT_GEOMETRY_NO_ROOM:
		snprintf(message, sizeof(message), "placement at offset %llu leaves no room before it for the FAT%u reserved sectors%s%s",
		    (unsigned long long)plan->placed[0].offset, plan->variant,
		    plan->variant == 32 ? " and FATs" : ", FATs and root",
		    cluster_context);
		break;
	case FAT_GEOMETRY_OVERLAP:
		snprintf(message, sizeof(message), "placements at offsets %llu and %llu overlap with %u-byte clusters",
		    (unsigned long long)plan->placed[overlap].offset,
		    (unsigned long long)plan->placed[overlap + 1].offset, smallest);
		break;
	default:
		snprintf(message, sizeof(message), "cannot create FAT%u: requested size and allocation settings conflict",
		    plan->variant);
		break;
	}
	return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER, message));
}

/* The FAT12/16 fixed root: the requested capacity, which validation has
 * rounded up to whole sectors, or the smallest whole number of sectors
 * holding the root's slots, at least 512 entries under the default policy
 * and exactly that under minimization. Either must hold the root's slots
 * and fit the 16-bit entry count. FAT32 has none.
 */
static fsimg_status_t
choose_root(fat_plan_t *plan, fsimg_error_t *error)
{
	const fsimg_write_options_t *options = &plan->common.writer->options;
	fat_layout_t root;
	uint64_t root_entries;
	fsimg_status_t status;
	char message[128];

	if (plan->variant == 32)
		return (FSIMG_OK);
	status = fsimg_fat_layout_get(plan, 0, &root, error);
	if (status != FSIMG_OK)
		return (status);
	root_entries = options->fat_root_entries;
	if (root_entries == 0) {
		root_entries = root_sectors_entries(root.slots, options->sector_bytes);
		if ((options->flags & FSIMG_WRITE_MINIMIZE) == 0 && root_entries < 512)
			root_entries = 512;
	}
	if (root_entries < root.slots || root_entries > UINT16_MAX) {
		if (root_entries < root.slots)
			snprintf(message, sizeof(message), "FAT root directory "
			    "capacity %llu is below the %llu entries the root needs",
			    (unsigned long long)root_entries,
			    (unsigned long long)root.slots);
		else
			snprintf(message, sizeof(message), "FAT root directory "
			    "needs %llu entries, above the 65535 a fixed root holds",
			    (unsigned long long)root_entries);
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_WRITER,
		    message));
	}
	plan->root_entries = (uint32_t)root_entries;
	return (FSIMG_OK);
}

/* The cluster size: the requested one, or every legal size in increasing
 * order (specs/design-sizing.md). The default policy takes the first that
 * fits; minimization the one with the smallest volume, the earliest on a
 * tie, so it agrees with the default whenever that is already minimal.
 * Each candidate evaluated charges one work unit.
 *
 * A placed run must begin on a cluster boundary, so placements keep the
 * candidates to the sizes dividing every offset: the powers of two up to
 * the lowest bit set in any offset. Validation has made every offset a
 * multiple of the first candidate, so at least that one remains.
 */
static fsimg_status_t
choose_geometry(fat_plan_t *plan, fsimg_error_t *error)
{
	const fsimg_write_options_t *options = &plan->common.writer->options;
	fat_usage_t usage[FAT_UNITS_MAX];
	fat_candidate_t candidate, best;
	uint64_t offsets, alignment;
	uint32_t first, last;
	unsigned count;
	fsimg_status_t status;
	fat_geometry_result_t reason, rejected;
	bool minimize, found;

	status = choose_root(plan, error);
	if (status != FSIMG_OK)
		return (status);
	minimize = (options->flags & FSIMG_WRITE_MINIMIZE) != 0;
	first = options->allocation_unit_bytes;
	last = first == 0 ? FAT_CLUSTER_MAX : first;
	if (first == 0)
		first = options->sector_bytes;
	offsets = 0;
	for (uint32_t i = 0; i < plan->placed_count; i++)
		offsets |= plan->placed[i].offset;
	alignment = offsets & (~offsets + 1);
	if (alignment != 0 && alignment < last)
		last = (uint32_t)alignment;
	count = 0;
	while (count < FAT_UNITS_MAX && (first << count) <= last)
		count++;
	status = count_clusters(plan, first, count, usage, error);
	if (status != FSIMG_OK)
		return (status);
	bzero(&best, sizeof(best));
	found = false;
	rejected = FAT_GEOMETRY_NONE;
	for (unsigned i = 0; i < count; i++) {
		status = fsimg_session_charge(&plan->common.writer->session, 0, 0, 1,
		    error);
		if (status != FSIMG_OK)
			return (status);
		reason = try_geometry(plan, first << i, &usage[i], &candidate);
		if (reason != FAT_GEOMETRY_FITS) {
			rejected = rejected == FAT_GEOMETRY_NONE || rejected == reason ?
			    reason : FAT_GEOMETRY_CONFLICT;
			continue;
		}
		if (!found || candidate.volume_bytes < best.volume_bytes)
			best = candidate;
		found = true;
		if (!minimize)
			break;
	}
	if (!found)
		return (geometry_error(plan, rejected, first, error));
	apply_geometry(plan, &best);
	return (FSIMG_OK);
}

/* Give every row its run by the next fit count_clusters simulated for the
 * chosen cluster size: placed files take the run their offset names, every
 * other row in ID order the cursor's next fit, starting with the FAT32
 * root, whose cluster the boot sector records. Directory clusters and the
 * reserved area are the plan's metadata.
 */
static fsimg_status_t
allocate_layout(fat_plan_t *plan, fsimg_error_t *error)
{
	fat_layout_t row;
	fat_cursor_t cursor;
	uint64_t directory_bytes, first, end;
	uint32_t cluster_bytes, placed;
	fsimg_status_t status;

	directory_bytes = 0;
	cluster_bytes = plan->common.info.allocation_unit_bytes;
	cursor = (fat_cursor_t){ .next = 2, .cluster_bytes = cluster_bytes };
	for (uint64_t id = 0; id <= plan->common.writer->count; id++) {
		status = fsimg_fat_layout_get(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
		if (row.slots != 0)
			row.length = row.slots * 32;
		if (id != 0 || plan->variant == 32) {
			row.clusters = (uint32_t)divide_up(row.length, cluster_bytes);
			placed = fsimg_fat_placed_index(plan, id);
			if (placed != plan->placed_count) {
				placed_run(plan, placed, cluster_bytes, &first, &end);
				row.first = (uint32_t)first;
				plan->placed[placed].first = row.first;
				plan->placed[placed].clusters = row.clusters;
			} else if (row.clusters != 0) {
				if (!cursor_take(plan, &cursor, row.clusters, &first))
					return (fsimg_error_set(error, FSIMG_ESTATE, 0,
					    FSIMG_OP_WRITER,
					    "FAT allocation diverged from its sizing"));
				row.first = (uint32_t)first;
			}
			if (row.slots != 0)
				directory_bytes += (uint64_t)row.clusters * cluster_bytes;
		}
		if (id == 0)
			plan->root_cluster = row.first;
		fsimg_store_le16(row.entry + 26, (uint16_t)row.first);
		if (plan->variant == 32)
			fsimg_store_le16(row.entry + 20, (uint16_t)(row.first >> 16));
		status = fsimg_fat_layout_set(plan, id, &row, error);
		if (status != FSIMG_OK)
			return (status);
	}
	plan->used_end = (uint32_t)cursor_end(plan, &cursor);
	plan->common.info.metadata_bytes = plan->data_offset + directory_bytes;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_fat_write_plan(fsimg_writer_t *writer, fsimg_error_t *error)
{
	fat_plan_t *plan;
	fsimg_error_t detail;
	fsimg_status_t status;

	plan = fsimg_alloc(writer->ctx, sizeof(*plan), &detail);
	if (plan == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	plan->common.writer = writer;
	plan->variant = writer->variant;
	snprintf(plan->common.info.format, sizeof(plan->common.info.format), "fat%u", plan->variant);
	plan->common.info.payload_bytes = writer->payload_bytes;
	plan->common.info.initialized_bytes = writer->payload_bytes;
	plan->common.info.entries = writer->count;
	plan->common.info.sector_bytes = writer->options.sector_bytes;
	plan->common.info.required_io_flags = FSIMG_IO_WRITE;
	if ((writer->options.flags & FSIMG_WRITE_DURABLE) != 0)
		plan->common.info.required_io_flags |= FSIMG_IO_DURABLE;
	status = fsimg_array_new(&writer->session, FAT_LAYOUT_BYTES,
	    writer->count + 1, &plan->layout, error);
	if (status != FSIMG_OK)
		goto fail;
	sort_placements(plan);
	status = fsimg_fat_write_names(plan, error);
	if (status != FSIMG_OK)
		goto fail;
	status = check_placements_matched(plan, error);
	if (status != FSIMG_OK)
		goto fail;
	status = choose_geometry(plan, error);
	if (status != FSIMG_OK)
		goto fail;
	status = allocate_layout(plan, error);
	if (status != FSIMG_OK)
		goto fail;
	plan->common.info.scratch_bytes = writer->ctx->stats.scratch_bytes;
	writer->plan = &plan->common;
	return (FSIMG_OK);
fail:
	fsimg_fat_write_free(&plan->common);
	return (status);
}

void
fsimg_fat_write_free(fsimg_plan_t *base)
{
	fat_plan_t *plan = (fat_plan_t *)base;

	if (plan == NULL)
		return;
	fsimg_array_free(plan->layout);
	fsimg_dealloc(plan);
}

fsimg_status_t
fsimg_fat_write_validate(fsimg_write_options_t *selected,
    unsigned variant FSIMG_UNUSED, fsimg_error_t *error)
{
	uint64_t root_entries;
	uint32_t sector, cluster;
	size_t length;

	sector = selected->sector_bytes == 0 ? 512 : selected->sector_bytes;
	cluster = selected->allocation_unit_bytes;
	if ((sector != 512 && sector != 1024 && sector != 2048 && sector != 4096) ||
	    (cluster != 0 && (cluster < sector || cluster > FAT_CLUSTER_MAX ||
	    (cluster & (cluster - 1)) != 0)) ||
	    selected->volume_bytes % sector != 0 ||
	    selected->volume_bytes / sector > UINT32_MAX ||
	    ((selected->flags & FSIMG_WRITE_REPRODUCIBLE) != 0 &&
	    (selected->epoch < 315532800 || selected->epoch > 4354819199LL)))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "invalid FAT geometry or reproducible epoch"));
	selected->sector_bytes = sector;
	/* A requested root capacity rounds up to whole sectors of 32-byte
	 * entries; the common settings check has already refused one on FAT32,
	 * which has no fixed root. Rounding is idempotent, so a second
	 * validation of the selected options keeps the same capacity.
	 */
	if (selected->fat_root_entries != 0) {
		root_entries = root_sectors_entries(selected->fat_root_entries,
		    sector);
		if (root_entries > UINT16_MAX)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_WRITER, "FAT root directory capacity exceeds "
			    "65535 entries once rounded up to whole sectors"));
		selected->fat_root_entries = (uint32_t)root_entries;
	}
	/* A placed run begins on a cluster boundary, so its offset must be a
	 * multiple of the sector, which every candidate cluster is, and of the
	 * cluster when cluster= fixes it (specs/design-fat-writer.md).
	 */
	for (uint32_t i = 0; i < selected->placement_count; i++) {
		if (selected->placements[i].offset % (cluster != 0 ? cluster :
		    sector) != 0) {
			fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER,
			    cluster != 0 ? "placement offset is not a multiple of "
			    "the FAT cluster size" : "placement offset is not a "
			    "multiple of the FAT sector size");
			fsimg_error_path(error, "place");
			return (FSIMG_EINVAL);
		}
	}
	if (selected->label == NULL)
		selected->label = "NO NAME";
	length = strnlen(selected->label, 12);
	if (length == 0 || length > 11 || selected->label[0] == ' ' ||
	    selected->label[length - 1] == ' ')
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_WRITER, "invalid FAT volume label length or spacing"));
	for (size_t i = 0; i < length; i++) {
		if (!((selected->label[i] >= 'A' && selected->label[i] <= 'Z') ||
		    (selected->label[i] >= '0' && selected->label[i] <= '9') ||
		    strchr(" $%'-_@~`!(){}^#&", selected->label[i]) != NULL))
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_WRITER, "FAT label requires uppercase ASCII label characters"));
	}
	return (FSIMG_OK);
}
