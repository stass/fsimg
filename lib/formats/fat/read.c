/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 * Copyright (c) 2026, fsimg contributors
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fat.h"
#include "format.h"
#include "fsimg-endian.h"

static fsimg_status_t table_byte(fsimg_reader_t *, unsigned, uint64_t,
    unsigned char *, fsimg_error_t *);
static fsimg_status_t table_value(fsimg_reader_t *, unsigned, uint32_t,
    uint32_t *, fsimg_error_t *);
static fsimg_status_t chain_take(fsimg_reader_t *, fat_chain_t *, uint32_t *,
    fsimg_error_t *);
static fsimg_status_t build_index(fsimg_reader_t *, fsimg_error_t *);

static fsimg_status_t
table_byte(fsimg_reader_t *reader, unsigned table, uint64_t offset,
    unsigned char *out, fsimg_error_t *error)
{
	fsimg_fat_reader_t *fat = fsimg_fat_state(reader);
	const fat_geometry_t *g = &fat->geometry;
	uint64_t window;
	size_t length, transferred;
	unsigned copies;
	fsimg_status_t status;

	if (offset >= g->fat_bytes)
		return (fsimg_fat_corrupt(error, "FAT entry exceeds table extent"));
	if (fat->table_window_length == 0 || offset < fat->table_window_offset ||
	    offset - fat->table_window_offset >= fat->table_window_length) {
		window = offset - offset % sizeof(fat->table_window[0]);
		length = sizeof(fat->table_window[0]);
		if (length > g->fat_bytes - window)
			length = (size_t)(g->fat_bytes - window);
		fat->table_window_length = 0;
		copies = g->mirrored ? g->tables : 1;
		/* One window covers the same coordinates in every relevant copy.
		 * Comparison must not evict the first copy for every FAT entry.
		 */
		for (unsigned i = 0; i < copies; i++) {
			status = fsimg_reader_bytes(reader, g->fat_offset +
			    (g->mirrored ? i : g->active_table) * g->fat_bytes + window,
			    fat->table_window[i], length, &transferred, error);
			if (status != FSIMG_OK)
				return (status);
		}
		fat->table_window_offset = window;
		fat->table_window_length = length;
	}
	*out = fat->table_window[g->mirrored ? table : 0]
	    [offset - fat->table_window_offset];
	return (FSIMG_OK);
}

static fsimg_status_t
table_value(fsimg_reader_t *reader, unsigned table, uint32_t cluster,
    uint32_t *out, fsimg_error_t *error)
{
	const fat_geometry_t *g = &fsimg_fat_state(reader)->geometry;
	unsigned char bytes[4];
	uint64_t offset;
	unsigned count;
	fsimg_status_t status;

	offset = g->variant == 12 ? (uint64_t)cluster * 3 / 2 :
	    (uint64_t)cluster * (g->variant / 8);
	count = g->variant == 32 ? 4 : 2;
	for (unsigned i = 0; i < count; i++) {
		status = table_byte(reader, table, offset + i, &bytes[i], error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (g->variant == 12)
		*out = (cluster & 1) != 0 ? fsimg_load_le16(bytes) >> 4 : fsimg_load_le16(bytes) & 0xfff;
	else if (g->variant == 16)
		*out = fsimg_load_le16(bytes);
	else
		*out = fsimg_load_le32(bytes) & 0x0fffffff;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_fat_table_get(fsimg_reader_t *reader, uint32_t cluster, uint32_t *out,
    fsimg_error_t *error)
{
	const fat_geometry_t *g = &fsimg_fat_state(reader)->geometry;
	uint32_t copy;
	fsimg_status_t status;

	status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
	if (status != FSIMG_OK)
		return (status);
	status = table_value(reader, g->active_table, cluster, out, error);
	if (status != FSIMG_OK)
		return (status);
	/* Mirroring makes table 0 active (geometry decoding), so every other
	 * copy must agree with the value just read.
	 */
	for (unsigned table = 1; g->mirrored && table < g->tables; table++) {
		status = table_value(reader, table, cluster, &copy, error);
		if (status != FSIMG_OK)
			return (status);
		if (copy != *out)
			return (fsimg_fat_corrupt(error, "mirrored FAT entries disagree"));
	}
	return (FSIMG_OK);
}

fsimg_fat_reader_t *
fsimg_fat_state(const fsimg_reader_t *reader)
{
	return (reader->driver);
}

fsimg_status_t
fsimg_fat_open(fsimg_reader_t *reader, const fsimg_format_variant_t *forced,
    fsimg_error_t *error)
{
	unsigned char boot[512];
	fat_geometry_t geometry;
	const fsimg_format_variant_t *variant;
	fsimg_fat_reader_t *fat;
	fsimg_error_t detail;
	uint64_t length;
	uint32_t first, second, mask;
	size_t transferred;
	fsimg_status_t status;

	status = fsimg_io_get_size(reader->input, &length, error);
	if (status != FSIMG_OK)
		return (status);
	if (length < sizeof(boot))
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0,
		    FSIMG_OP_READER, "volume is smaller than a boot record"));
	status = fsimg_reader_bytes(reader, 0, boot, sizeof(boot), &transferred, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_fat_geometry_decode(boot, length, &geometry, error);
	if (status != FSIMG_OK)
		return (status);
	variant = fsimg_fat_variant(geometry.variant);
	if (forced != NULL && forced != variant)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0,
		    FSIMG_OP_READER, "forced format differs from FAT cluster-count type"));
	fat = fsimg_alloc(reader->ctx, sizeof(*fat), &detail);
	if (fat == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	reader->driver = fat;
	fat->geometry = geometry;
	reader->content_domain.offset = geometry.data_offset;
	reader->content_domain.length = (uint64_t)geometry.clusters *
	    geometry.cluster_bytes;
	reader->content_domain.unit_bytes = geometry.cluster_bytes;
	status = fsimg_fat_table_get(reader, 0, &first, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_fat_table_get(reader, 1, &second, error);
	if (status != FSIMG_OK)
		return (status);
	mask = geometry.variant == 12 ? 0xfff :
	    geometry.variant == 16 ? 0xffff : 0x0fffffff;
	if (first != ((mask & ~0xffu) | geometry.media) ||
	    (geometry.variant == 12 && second < 0xff8) ||
	    (geometry.variant == 16 && (second & 0x3fff) != 0x3fff) ||
	    (geometry.variant == 32 && (second & 0x03ffffff) != 0x03ffffff))
		return (fsimg_fat_corrupt(error, "invalid FAT reserved entries"));
	strcpy(reader->info.format, variant->info.name);
	strcpy(reader->info.profile, variant->info.profile);
	reader->info.valid = FSIMG_VOLUME_GEOMETRY;
	reader->info.volume_bytes = geometry.volume_bytes;
	reader->info.allocation_units = geometry.clusters;
	reader->info.sector_bytes = geometry.sector_bytes;
	reader->info.allocation_unit_bytes = geometry.cluster_bytes;
	reader->info.time_basis = FSIMG_TIME_WALL;
	reader->info.scope = FSIMG_VALIDATION_HEADER;
	reader->info.clean = geometry.variant == 12 ||
	    (second & (geometry.variant == 16 ? 0x8000u : 0x08000000u)) != 0;
	reader->info.io_error = geometry.variant != 12 &&
	    (second & (geometry.variant == 16 ? 0x4000u : 0x04000000u)) == 0;
	if (boot[geometry.variant == 32 ? 66 : 38] == 0x29) {
		status = fsimg_fat_label(boot + (geometry.variant == 32 ? 71 : 43),
		    reader->info.label, error);
		if (status != FSIMG_OK)
			return (status);
		reader->info.valid |= FSIMG_VOLUME_LABEL;
	}
	return (FSIMG_OK);
}

void
fsimg_fat_close(fsimg_reader_t *reader)
{
	fsimg_fat_reader_t *fat = fsimg_fat_state(reader);

	if (fat == NULL)
		return;
	fsimg_bitset_free(fat->directory_clusters);
	fsimg_owner_runs_free(fat->owners);
	fsimg_extent_index_free(fat->index);
	fsimg_dealloc(fat);
	reader->driver = NULL;
}

void
fsimg_fat_chain_init(fat_chain_t *chain, uint32_t first, uint64_t expected)
{
	bzero(chain, sizeof(*chain));
	chain->cluster = first;
	chain->checkpoint = first;
	chain->power = 1;
	chain->expected = expected;
}

static fsimg_status_t
chain_take(fsimg_reader_t *reader, fat_chain_t *chain, uint32_t *cluster,
    fsimg_error_t *error)
{
	const fat_geometry_t *g = &fsimg_fat_state(reader)->geometry;
	uint32_t next;
	fsimg_status_t status;

	/* Define the output on every path; it is meaningful only on success. */
	*cluster = 0;
	if (!fsimg_fat_cluster_valid(g, chain->cluster) || chain->steps >= g->clusters)
		return (fsimg_fat_corrupt(error, "invalid or excessive FAT chain"));
	if (chain->expected == 0 &&
	    chain->steps >= FAT_DIRECTORY_LIMIT / g->cluster_bytes)
		return (fsimg_fat_corrupt(error, "FAT directory exceeds its byte limit"));
	status = fsimg_fat_table_get(reader, chain->cluster, &next, error);
	if (status != FSIMG_OK)
		return (status);
	if (next >= g->eoc_value)
		next = 0;
	else if (!fsimg_fat_cluster_valid(g, next))
		return (fsimg_fat_corrupt(error, "FAT chain references free, bad or reserved storage"));
	*cluster = chain->cluster;
	chain->steps++;
	if (chain->expected != 0 &&
	    ((chain->steps == chain->expected && next != 0) ||
	    (chain->steps < chain->expected && next == 0) ||
	    chain->steps > chain->expected))
		return (fsimg_fat_corrupt(error, "FAT file chain length differs from its size"));
	if (next != 0) {
		/* Brent's checkpoint advances after runs of 1, 2, 4, ... links. A link
		 * back to it proves a cycle using constant memory. The independent
		 * geometry limit also terminates a chain whose backing bytes change.
		 */
		chain->distance++;
		if (next == chain->checkpoint)
			return (fsimg_fat_corrupt(error, "cycle in FAT allocation chain"));
		if (chain->distance == chain->power) {
			chain->checkpoint = next;
			chain->power *= 2;
			chain->distance = 0;
		}
	}
	chain->cluster = next;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_fat_chain_run(fsimg_reader_t *reader, fat_chain_t *chain,
    fsimg_extent_t *run, fsimg_error_t *error)
{
	const fat_geometry_t *g = &fsimg_fat_state(reader)->geometry;
	uint32_t cluster;
	fsimg_status_t status;

	bzero(run, sizeof(*run));
	if (chain->cluster == 0)
		return (FSIMG_DONE);
	run->logical_offset = chain->steps * g->cluster_bytes;
	run->image_offset = fsimg_fat_cluster_offset(g, chain->cluster);
	/* Consume each link exactly once. A contiguous sequence becomes one run;
	 * the cursor retains the first cluster of the following nonadjacent run.
	 * The whole allocation is represented, including final-cluster slack.
	 */
	for (;;) {
		status = chain_take(reader, chain, &cluster, error);
		if (status != FSIMG_OK)
			return (status);
		run->length += g->cluster_bytes;
		if (chain->cluster == 0 || chain->cluster != cluster + 1)
			return (FSIMG_OK);
	}
}

fsimg_status_t
fsimg_fat_owner_append(fsimg_reader_t *reader, const fsimg_extent_t *run,
    fsimg_error_t *error)
{
	fsimg_fat_reader_t *fat = fsimg_fat_state(reader);
	const fat_geometry_t *g = &fat->geometry;
	uint64_t heap_offset;

	if (fat->owners == NULL)
		return (FSIMG_OK);
	/* A validated chain run is whole clusters of the data region; its
	 * claim is their cluster numbers. The run utility bounds them by the
	 * heap.
	 */
	if (run->image_offset < g->data_offset ||
	    (run->image_offset - g->data_offset) % g->cluster_bytes != 0 ||
	    run->length % g->cluster_bytes != 0)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_VERIFY,
		    "FAT run is not whole data-region clusters"));
	heap_offset = run->image_offset - g->data_offset;
	return (fsimg_owner_runs_append(fat->owners,
	    heap_offset / g->cluster_bytes + 2, run->length / g->cluster_bytes,
	    error));
}

/* One forward pass over the current file's chain records every run in the
 * shared index. The chain walk validates the allocation length; the index
 * confines every record to the data region. Only a complete index becomes
 * the current file's.
 */
static fsimg_status_t
build_index(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_fat_reader_t *fat = fsimg_fat_state(reader);
	const fat_geometry_t *g = &fat->geometry;
	fsimg_extent_index_t *index;
	fat_chain_t chain;
	fsimg_extent_t run;
	fsimg_status_t status;

	status = fsimg_extent_index_new(&reader->session,
	    &reader->content_domain, &index, error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_fat_chain_init(&chain, fat->current_cluster,
	    (reader->entry.size + g->cluster_bytes - 1) / g->cluster_bytes);
	for (;;) {
		status = fsimg_fat_chain_run(reader, &chain, &run, error);
		if (status == FSIMG_DONE)
			break;
		if (status != FSIMG_OK)
			goto fail;
		status = fsimg_extent_index_append(index, &run, error);
		if (status != FSIMG_OK)
			goto fail;
	}
	fat->index = index;
	return (FSIMG_OK);
fail:
	fsimg_extent_index_free(index);
	return (status);
}

/* Resolve offset within the current file. Common delivery owns the read,
 * its bounds and its stability checks; this only finds the containing run.
 */
fsimg_status_t
fsimg_fat_resolve(fsimg_reader_t *reader, uint64_t offset, fsimg_extent_t *run,
    fsimg_error_t *error)
{
	fsimg_fat_reader_t *fat = fsimg_fat_state(reader);
	const fat_geometry_t *g = &fat->geometry;
	uint64_t end;
	fsimg_status_t status;

	if (fat->index != NULL)
		return (fsimg_extent_index_lookup(fat->index, offset, run,
		    error));
	/* Inside the cached extent: no chain work. */
	end = fat->extent.logical_offset + fat->extent.length;
	if (offset >= fat->extent.logical_offset && offset < end) {
		*run = fat->extent;
		return (FSIMG_OK);
	}
	/* Anywhere but the cached extent's end is a seek: one complete forward
	 * pass builds the index, and later lookups search it instead of
	 * restarting the allocation chain.
	 */
	if (offset != end) {
		status = build_index(reader, error);
		if (status != FSIMG_OK)
			return (status);
		return (fsimg_extent_index_lookup(fat->index, offset, run,
		    error));
	}
	/* At the cached extent's end: advance the chain by one run. */
	if (!fat->file_started) {
		fsimg_fat_chain_init(&fat->file_chain, fat->current_cluster,
		    (reader->entry.size + g->cluster_bytes - 1) / g->cluster_bytes);
		fat->file_started = true;
	}
	status = fsimg_fat_chain_run(reader, &fat->file_chain, &fat->extent, error);
	if (status != FSIMG_OK)
		return (status == FSIMG_DONE ?
		    fsimg_fat_corrupt(error, "FAT chain ended before content") : status);
	*run = fat->extent;
	return (FSIMG_OK);
}
