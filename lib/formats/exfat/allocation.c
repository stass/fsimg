/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <strings.h>

#include "exfat.h"

static fsimg_status_t map_append(fsimg_reader_t *, exfat_map_t *, uint64_t,
    uint32_t, uint32_t, fsimg_error_t *);
static fsimg_status_t bitmap_range(fsimg_reader_t *, uint32_t, uint32_t,
    fsimg_error_t *);
static fsimg_status_t map_select_extent(exfat_map_t *, uint64_t, fsimg_error_t *);

fsimg_status_t
exfat_table_get(fsimg_reader_t *reader, uint32_t cluster, uint32_t *value,
    fsimg_error_t *error)
{
	fsimg_exfat_reader_t *exfat = reader->format.exfat;
	const exfat_geometry_t *g = &exfat->geometry;
	uint64_t offset, window;
	size_t transferred, length;
	fsimg_status_t status;

	status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	offset = (uint64_t)cluster * 4;
	if (!fsimg_range_fits(offset, 4, g->fat_bytes))
		return (exfat_corrupt(error, "exFAT cluster exceeds FAT"));
	window = offset / sizeof(exfat->table_window) * sizeof(exfat->table_window);
	if (exfat->table_window_length == 0 || exfat->table_window_offset != window) {
		length = g->fat_bytes - window > sizeof(exfat->table_window) ?
		    sizeof(exfat->table_window) : (size_t)(g->fat_bytes - window);
		status = fsimg_reader_bytes(reader, g->fat_offset + window,
		    exfat->table_window, length, &transferred, error);
		if (status != FSIMG_OK)
			return (status);
		exfat->table_window_offset = window;
		exfat->table_window_length = length;
	}
	*value = exfat_u32(exfat->table_window + (size_t)(offset - window));
	return (FSIMG_OK);
}

void
exfat_map_free(exfat_map_t *map)
{
	fsimg_store_free(map->index);
	bzero(map, sizeof(*map));
}

static fsimg_status_t
map_append(fsimg_reader_t *reader, exfat_map_t *map, uint64_t logical,
    uint32_t first, uint32_t count, fsimg_error_t *error)
{
	const exfat_geometry_t *g = &reader->format.exfat->geometry;
	unsigned char record[24];
	uint64_t needed, capacity;
	fsimg_status_t status;

	needed = (map->count + 1) * sizeof(record);
	capacity = fsimg_store_size(map->index);
	if (capacity < needed) {
		/* Grow in pages so fragmented files do not resize scratch per run. */
		capacity = (needed + 4095) / 4096 * 4096;
		status = fsimg_store_grow(map->index, capacity, error);
		if (status != FSIMG_OK)
			return (status);
	}
	fsimg_meta_put_u64(record, logical);
	fsimg_meta_put_u64(record + 8, exfat_cluster_offset(g, first));
	fsimg_meta_put_u64(record + 16, (uint64_t)count * g->cluster_bytes);
	status = fsimg_store_write(map->index, map->count * sizeof(record),
	    record, sizeof(record), error);
	if (status == FSIMG_OK)
		map->count++;
	return (status);
}

fsimg_status_t
exfat_map_build(fsimg_reader_t *reader, const exfat_stream_t *stream,
    exfat_length_policy_t policy, exfat_map_t *map, fsimg_error_t *error)
{
	const exfat_geometry_t *g = &reader->format.exfat->geometry;
	uint64_t steps, power, distance, logical;
	uint32_t cluster, next, checkpoint, first, count;
	fsimg_status_t status;

	bzero(map, sizeof(*map));
	status = exfat_stream_validate(g, stream, false, error);
	if (status != FSIMG_OK)
		return (status);
	if (stream->first == 0)
		return (FSIMG_OK);
	status = fsimg_store_new(&reader->session, &map->index, error);
	if (status != FSIMG_OK)
		return (status);
	if (stream->contiguous) {
		count = (uint32_t)(stream->length / g->cluster_bytes +
		    (stream->length % g->cluster_bytes != 0));
		status = map_append(reader, map, 0, stream->first, count, error);
		if (status != FSIMG_OK)
			goto fail;
		map->allocated_bytes = (uint64_t)count * g->cluster_bytes;
		if (reader->format.exfat->bitmap_ready) {
			status = bitmap_range(reader, stream->first, count, error);
			if (status != FSIMG_OK)
				goto fail;
		}
		return (FSIMG_OK);
	}

	/* Coalesce adjacent physical clusters while Brent's checkpoint detects
	 * cycles. The heap count independently caps all walks, even malformed ones.
	 * The index owns the entire chain, including preallocation beyond payload.
	 */
	cluster = stream->first;
	checkpoint = cluster;
	first = cluster;
	count = 0;
	steps = 0;
	distance = 0;
	power = 1;
	logical = 0;
	for (;;) {
		if (!exfat_cluster_valid(g, cluster) || ++steps > g->clusters) {
			status = exfat_corrupt(error, "invalid or cyclic exFAT allocation chain");
			goto fail;
		}
		count++;
		status = exfat_table_get(reader, cluster, &next, error);
		if (status != FSIMG_OK)
			goto fail;
		if (next != cluster + 1 || next == UINT32_MAX) {
			status = map_append(reader, map, logical, first, count, error);
			if (status != FSIMG_OK)
				goto fail;
			if (reader->format.exfat->bitmap_ready) {
				status = bitmap_range(reader, first, count, error);
				if (status != FSIMG_OK)
					goto fail;
			}
			logical += (uint64_t)count * g->cluster_bytes;
			first = next;
			count = 0;
		}
		if (next == UINT32_MAX)
			break;
		distance++;
		if (next == checkpoint) {
			status = exfat_corrupt(error, "cyclic exFAT allocation chain");
			goto fail;
		}
		if (distance == power) {
			checkpoint = next;
			power *= 2;
			distance = 0;
		}
		cluster = next;
	}
	map->allocated_bytes = logical;
	if (logical < stream->length || (policy == EXFAT_LENGTH_EXACT_ALLOCATION && logical != stream->length)) {
		status = exfat_corrupt(error, "exFAT chain does not cover its declared allocation");
		goto fail;
	}
	return (FSIMG_OK);
fail:
	exfat_map_free(map);
	return (status);
}

static fsimg_status_t
map_select_extent(exfat_map_t *map, uint64_t index, fsimg_error_t *error)
{
	unsigned char record[24];
	uint64_t logical, length;
	fsimg_status_t status;

	if (index >= map->count)
		return (exfat_corrupt(error, "exFAT extent index is outside its record range"));
	status = fsimg_store_read(map->index, index * sizeof(record), record,
	    sizeof(record), error);
	if (status != FSIMG_OK)
		return (status);
	logical = fsimg_meta_get_u64(record);
	length = fsimg_meta_get_u64(record + 16);
	if (logical >= map->allocated_bytes || length == 0 ||
	    length > map->allocated_bytes - logical)
		return (exfat_corrupt(error, "invalid cached exFAT allocation extent"));
	map->cached = index;
	map->logical_offset = logical;
	map->image_offset = fsimg_meta_get_u64(record + 8);
	map->length = length;
	return (FSIMG_OK);
}

fsimg_status_t
exfat_map_extent(exfat_map_t *map, uint64_t offset, uint64_t *image_offset,
    uint64_t *length, fsimg_error_t *error)
{
	unsigned char record[24];
	uint64_t low, high, middle, logical;
	fsimg_status_t status;

	if (offset >= map->allocated_bytes)
		return (exfat_corrupt(error, "exFAT stream offset exceeds allocation"));
	if (map->length != 0 && offset >= map->logical_offset &&
	    offset - map->logical_offset < map->length)
		goto found;
	/* A sequential read crosses each index record once. Random access still
	 * uses binary search, without restarting its file's allocation chain.
	 */
	if (map->length != 0 && offset >= map->logical_offset &&
	    offset - map->logical_offset == map->length &&
	    map->cached + 1 < map->count) {
		status = map_select_extent(map, map->cached + 1, error);
		if (status != FSIMG_OK)
			return (status);
		if (offset != map->logical_offset)
			return (exfat_corrupt(error, "discontinuous exFAT extent index"));
		goto found;
	}
	low = 0;
	high = map->count;
	while (low < high) {
		middle = low + (high - low) / 2;
		status = fsimg_store_read(map->index, middle * sizeof(record),
		    record, sizeof(record), error);
		if (status != FSIMG_OK)
			return (status);
		logical = fsimg_meta_get_u64(record);
		if (logical <= offset)
			low = middle + 1;
		else
			high = middle;
	}
	if (low == 0)
		return (exfat_corrupt(error, "exFAT extent index has no covering run"));
	status = map_select_extent(map, low - 1, error);
	if (status != FSIMG_OK)
		return (status);
found:
	if (offset < map->logical_offset || offset - map->logical_offset >= map->length ||
	    !fsimg_add_u64(map->image_offset, offset - map->logical_offset, image_offset))
		return (exfat_corrupt(error, "exFAT extent does not cover requested content"));
	*length = map->length - (offset - map->logical_offset);
	return (FSIMG_OK);
}

fsimg_status_t
exfat_map_read(fsimg_reader_t *reader, exfat_map_t *map, uint64_t offset,
    void *buffer, size_t length, fsimg_error_t *error)
{
	uint64_t image_offset, extent_length;
	size_t batch, transferred, done;
	fsimg_status_t status;

	done = 0;
	while (done < length) {
		status = exfat_map_extent(map, offset + done, &image_offset,
		    &extent_length, error);
		if (status != FSIMG_OK)
			return (status);
		batch = length - done;
		if (batch > EXFAT_BUFFER_BYTES)
			batch = EXFAT_BUFFER_BYTES;
		if (batch > extent_length)
			batch = (size_t)extent_length;
		status = fsimg_reader_bytes(reader, image_offset,
		    (unsigned char *)buffer + done, batch, &transferred, error);
		if (status != FSIMG_OK)
			return (status);
		done += batch;
	}
	return (FSIMG_OK);
}

fsimg_status_t
exfat_bitmap_bit(fsimg_reader_t *reader, uint32_t cluster, bool *allocated,
    fsimg_error_t *error)
{
	unsigned char byte;
	fsimg_status_t status;

	status = exfat_map_read(reader, &reader->format.exfat->bitmap,
	    (cluster - 2) / 8, &byte, 1, error);
	if (status == FSIMG_OK)
		*allocated = (byte & (1u << ((cluster - 2) % 8))) != 0;
	return (status);
}

static fsimg_status_t
bitmap_range(fsimg_reader_t *reader, uint32_t first, uint32_t count,
    fsimg_error_t *error)
{
	unsigned char bytes[4096];
	uint64_t bit, end, byte_offset;
	size_t batch;
	unsigned begin_bit, end_bit, mask;
	fsimg_status_t status;

	bit = (uint64_t)first - 2;
	end = bit + count;
	while (bit < end) {
		byte_offset = bit / 8;
		batch = (size_t)((end + 7) / 8 - byte_offset);
		if (batch > sizeof(bytes))
			batch = sizeof(bytes);
		status = exfat_map_read(reader, &reader->format.exfat->bitmap,
		    byte_offset, bytes, batch, error);
		if (status != FSIMG_OK)
			return (status);
		for (size_t i = 0; i < batch; i++) {
			begin_bit = i == 0 ? (unsigned)(bit % 8) : 0;
			end_bit = (byte_offset + i + 1) * 8 > end ? (unsigned)(end % 8) : 8;
			mask = ((1u << end_bit) - 1) & ~((1u << begin_bit) - 1);
			if ((bytes[i] & mask) != mask)
				return (exfat_corrupt(error, "owned exFAT cluster is marked free"));
		}
		status = fsimg_session_charge(&reader->session, 0, 0, batch, error);
		if (status != FSIMG_OK)
			return (status);
		bit = (byte_offset + batch) * 8;
	}
	return (FSIMG_OK);
}

fsimg_status_t
exfat_map_check(fsimg_reader_t *reader, exfat_map_t *map, fsimg_error_t *error)
{
	const exfat_geometry_t *g = &reader->format.exfat->geometry;
	unsigned char record[24];
	uint32_t first, count;
	fsimg_status_t status;

	for (uint64_t i = 0; i < map->count; i++) {
		status = fsimg_store_read(map->index, i * sizeof(record), record,
		    sizeof(record), error);
		if (status != FSIMG_OK)
			return (status);
		first = (uint32_t)((fsimg_meta_get_u64(record + 8) -
		    g->heap_offset) / g->cluster_bytes + 2);
		count = (uint32_t)(fsimg_meta_get_u64(record + 16) / g->cluster_bytes);
		status = bitmap_range(reader, first, count, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}

fsimg_status_t
exfat_map_own(fsimg_reader_t *reader, exfat_map_t *map,
    fsimg_error_t *error)
{
	const exfat_geometry_t *g = &reader->format.exfat->geometry;
	unsigned char extent[24], owner[16];
	fsimg_status_t status;

	if (reader->format.exfat->owners == NULL)
		return (FSIMG_OK);
	for (uint64_t i = 0; i < map->count; i++) {
		status = fsimg_store_read(map->index, i * sizeof(extent), extent,
		    sizeof(extent), error);
		if (status != FSIMG_OK)
			return (status);
		fsimg_meta_put_u64(owner,
		    (fsimg_meta_get_u64(extent + 8) - g->heap_offset) / g->cluster_bytes + 2);
		fsimg_meta_put_u64(owner + 8,
		    fsimg_meta_get_u64(extent + 16) / g->cluster_bytes);
		status = fsimg_stream_append(reader->format.exfat->owners, owner,
		    sizeof(owner), NULL, error);
		if (status != FSIMG_OK)
			return (status);
	}
	return (FSIMG_OK);
}
