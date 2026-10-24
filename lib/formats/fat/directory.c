/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fat.h"
#include "fsimg-endian.h"

/* FAT's part of directory preparation (specs/design-directories.md): decode
 * one directory's slots, validate its dot records and admit each child.
 * The private payload is the child's 32-byte short entry followed by the
 * little-endian cluster its ".." record must name.
 */
#define FAT_PAYLOAD_BYTES 36u

typedef struct directory_pass {
	fsimg_reader_t	*reader;
	fsimg_directory_build_t *build;
	fat_lfn_t	 lfn;
	uint64_t	 slots;
	uint32_t	 level;
	uint32_t	 self_cluster;
	uint32_t	 parent_cluster;
	bool		 ended;
} directory_pass_t;

static fsimg_status_t validate_dot(directory_pass_t *, const unsigned char *,
    fsimg_error_t *);
static fsimg_status_t decode_slot(directory_pass_t *, const unsigned char *,
    fsimg_error_t *);
static fsimg_status_t read_directory_run(directory_pass_t *,
    const fsimg_extent_t *, fsimg_error_t *);
static fsimg_status_t claim_run(directory_pass_t *, const fsimg_extent_t *,
    fsimg_error_t *);

static fsimg_status_t
validate_dot(directory_pass_t *pass, const unsigned char *raw,
    fsimg_error_t *error)
{
	const fat_geometry_t *g = &fsimg_fat_state(pass->reader)->geometry;
	fsimg_entry_t entry;
	unsigned char copy[32];
	uint32_t cluster, expected;
	fsimg_status_t status;

	expected = pass->slots == 0 ? pass->self_cluster : pass->parent_cluster;
	if (bcmp(raw, pass->slots == 0 ? ".          " : "..         ", 11) != 0 ||
	    raw[11] != 0x10 || raw[12] != 0 || fsimg_load_le32(raw + 28) != 0)
		return (fsimg_fat_corrupt(error, "missing or invalid FAT dot directory record"));
	cluster = fsimg_load_le16(raw + 26);
	if (g->variant == 32)
		cluster |= (uint32_t)fsimg_load_le16(raw + 20) << 16;
	else if (fsimg_load_le16(raw + 20) != 0)
		return (fsimg_fat_corrupt(error, "extended attributes in FAT dot record"));
	if (cluster != expected)
		return (fsimg_fat_corrupt(error, "FAT dot record has wrong directory cluster"));
	/* A root parent is encoded as cluster zero, including on FAT32. Substitute
	 * a valid cluster solely for the common attribute/timestamp validator,
	 * never for traversal.
	 */
	bcopy(raw, copy, sizeof(copy));
	copy[20] = copy[21] = copy[27] = 0;
	copy[26] = 2;
	status = fsimg_fat_entry_decode(g, copy, &entry, &cluster, error);
	return (status);
}

static fsimg_status_t
decode_slot(directory_pass_t *pass, const unsigned char *raw,
    fsimg_error_t *error)
{
	fsimg_reader_t *reader = pass->reader;
	fsimg_fat_reader_t *fat = fsimg_fat_state(reader);
	fsimg_entry_t entry;
	unsigned char payload[FAT_PAYLOAD_BYTES];
	char name[FAT_NAME_BYTES], alias[FAT_NAME_BYTES];
	uint32_t cluster;
	fsimg_status_t status;

	if (pass->level != 0 && pass->slots < 2) {
		status = validate_dot(pass, raw, error);
		if (status == FSIMG_OK)
			pass->slots++;
		return (status);
	}
	pass->slots++;
	/* End/deletion markers and structural records never become children.
	 * Deleted slots discard a preceding long name; a live orphan is corruption.
	 */
	if (raw[0] == 0) {
		if (pass->lfn.active)
			return (fsimg_fat_corrupt(error, "orphaned FAT long-name sequence"));
		pass->ended = true;
		return (FSIMG_OK);
	}
	if (raw[0] == 0xe5) {
		bzero(&pass->lfn, sizeof(pass->lfn));
		return (FSIMG_OK);
	}
	if (raw[11] == 0x0f)
		return (fsimg_fat_lfn_slot(&pass->lfn, raw, error));
	if ((raw[11] & 0x08) != 0) {
		/* Native formatters can retain the archive bit on a volume label.
		 * It still has no data allocation and never becomes a child entry.
		 */
		if (pass->level != 0 || (raw[11] & ~0x20u) != 0x08 || pass->lfn.active ||
		    fat->root_label_seen || fsimg_load_le16(raw + 20) != 0 ||
		    fsimg_load_le16(raw + 26) != 0 || fsimg_load_le32(raw + 28) != 0)
			return (fsimg_fat_corrupt(error, "invalid or duplicate FAT volume-label entry"));
		status = fsimg_fat_label(raw, fat->root_label, error);
		if (status != FSIMG_OK)
			return (status);
		fat->root_label_seen = true;
		return (FSIMG_OK);
	}
	/* Validate both spellings and the metadata before admitting the entry;
	 * the alias takes part in lookup even when a long name hides it.
	 */
	status = fsimg_fat_short_name(raw, alias, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_fat_entry_name(&pass->lfn, raw, name, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_fat_entry_decode(&fat->geometry, raw, &entry, &cluster, error);
	if (status != FSIMG_OK)
		return (status);
	/* A child's ".." names this directory, or cluster zero for the root. */
	bcopy(raw, payload, 32);
	fsimg_store_le32(payload + 32, pass->level == 0 ? 0 : pass->self_cluster);
	return (fsimg_directory_admit(pass->build, name, payload, sizeof(payload),
	    error));
}

static fsimg_status_t
read_directory_run(directory_pass_t *pass, const fsimg_extent_t *run,
    fsimg_error_t *error)
{
	fsimg_reader_t *reader = pass->reader;
	fsimg_fat_reader_t *fat = fsimg_fat_state(reader);
	uint64_t offset;
	size_t batch, transferred;
	fsimg_status_t status;

	offset = 0;
	while (offset < run->length && !pass->ended) {
		batch = sizeof(fat->directory_window);
		if (batch > run->length - offset)
			batch = (size_t)(run->length - offset);
		status = fsimg_reader_bytes(reader, run->image_offset + offset,
		    fat->directory_window, batch, &transferred, error);
		if (status != FSIMG_OK)
			return (status);
		for (size_t i = 0; i < batch && !pass->ended; i += 32) {
			status = decode_slot(pass, fat->directory_window + i, error);
			if (status != FSIMG_OK)
				return (status);
		}
		offset += batch;
	}
	return (FSIMG_OK);
}

/* Claim every cluster of a directory chain, including those after its end
 * marker: unused tail clusters still belong to the chain and cannot hide
 * sharing. Re-entering claimed storage is a cycle or a shared chain.
 */
static fsimg_status_t
claim_run(directory_pass_t *pass, const fsimg_extent_t *run,
    fsimg_error_t *error)
{
	fsimg_fat_reader_t *fat = fsimg_fat_state(pass->reader);
	const fat_geometry_t *g = &fat->geometry;
	uint64_t first, count;
	bool claimed;
	fsimg_status_t status;

	first = (run->image_offset - g->data_offset) / g->cluster_bytes + 2;
	count = run->length / g->cluster_bytes;
	for (uint64_t i = 0; i < count; i++) {
		status = fsimg_bitset_test_and_set(fat->directory_clusters, first + i,
		    &claimed, error);
		if (status != FSIMG_OK)
			return (status);
		if (claimed)
			return (fsimg_fat_corrupt(error, "shared or cyclic FAT directory chain"));
	}
	return (fsimg_fat_owner_append(pass->reader, run, error));
}

/* Prepare the root at level 0, otherwise the directory of the current
 * entry. The fixed FAT12/16 root region has no chain to claim.
 */
fsimg_status_t
fsimg_fat_prepare(fsimg_reader_t *reader, fsimg_directory_build_t *build,
    fsimg_error_t *error)
{
	fsimg_fat_reader_t *fat = fsimg_fat_state(reader);
	const fat_geometry_t *g = &fat->geometry;
	directory_pass_t pass;
	fat_chain_t chain;
	fsimg_extent_t run;
	fsimg_status_t status;

	bzero(&pass, sizeof(pass));
	pass.reader = reader;
	pass.build = build;
	pass.level = fsimg_directory_level(build);
	if (pass.level == 0) {
		status = fsimg_bitset_new(&reader->session,
		    (uint64_t)g->clusters + 2, &fat->directory_clusters, error);
		if (status != FSIMG_OK)
			return (status);
		pass.self_cluster = g->root_cluster;
	} else {
		pass.self_cluster = fat->current_cluster;
		pass.parent_cluster = fat->current_parent;
	}
	/* Only the FAT12/16 root lives in the fixed region; a child always has
	 * a cluster, since entry decoding rejects a directory at cluster zero.
	 */
	if (pass.level == 0 && pass.self_cluster == 0) {
		run.logical_offset = 0;
		run.image_offset = g->root_offset;
		run.length = (uint64_t)g->root_entries * 32;
		status = read_directory_run(&pass, &run, error);
		if (status != FSIMG_OK)
			return (status);
	} else {
		if (pass.self_cluster == 0)
			return (fsimg_fat_corrupt(error, "FAT subdirectory without a cluster"));
		fsimg_fat_chain_init(&chain, pass.self_cluster, 0);
		for (;;) {
			status = fsimg_fat_chain_run(reader, &chain, &run, error);
			if (status == FSIMG_DONE)
				break;
			if (status != FSIMG_OK)
				return (status);
			status = claim_run(&pass, &run, error);
			if (status != FSIMG_OK)
				return (status);
			status = read_directory_run(&pass, &run, error);
			if (status != FSIMG_OK)
				return (status);
		}
	}
	if (pass.lfn.active)
		return (fsimg_fat_corrupt(error, "orphaned FAT long name at directory end"));
	return (FSIMG_OK);
}

/* Make a replayed child current. Its payload came from scratch storage, so
 * the name and short entry are validated again before anything uses them.
 */
fsimg_status_t
fsimg_fat_replay(fsimg_reader_t *reader, const char *name,
    const unsigned char *payload, size_t length, fsimg_error_t *error)
{
	fsimg_fat_reader_t *fat = fsimg_fat_state(reader);
	fsimg_status_t status;

	fsimg_extent_index_free(fat->index);
	fat->index = NULL;
	fat->file_started = false;
	bzero(&fat->extent, sizeof(fat->extent));
	if (length != FAT_PAYLOAD_BYTES)
		return (fsimg_fat_corrupt(error, "invalid prepared FAT entry payload"));
	status = fsimg_fat_name_validate(name, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_fat_entry_decode(&fat->geometry, payload, &reader->entry,
	    &fat->current_cluster, error);
	if (status != FSIMG_OK)
		return (status);
	fat->current_parent = fsimg_load_le32(payload + 32);
	reader->initialized = reader->entry.size;
	return (FSIMG_OK);
}

_Static_assert(255 * 4 <= FSIMG_DIRECTORY_KEY_BYTES,
    "a FAT name key fits a directory key buffer");

/* An entry answers to its displayed name and its short-name alias. */
fsimg_status_t
fsimg_fat_keys(fsimg_reader_t *reader FSIMG_UNUSED, const char *name,
    const unsigned char *payload, size_t length, fsimg_directory_keys_t *keys,
    fsimg_error_t *error)
{
	char alias[FAT_NAME_BYTES];
	fsimg_status_t status;

	keys->count = 0;
	if (length != FAT_PAYLOAD_BYTES)
		return (fsimg_fat_corrupt(error, "invalid prepared FAT entry payload"));
	status = fsimg_fat_name_key(name, keys->key[0], &keys->length[0], error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_fat_short_name(payload, alias, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_fat_name_key(alias, keys->key[1], &keys->length[1], error);
	if (status != FSIMG_OK)
		return (status);
	keys->count = 2;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_fat_key(fsimg_reader_t *reader FSIMG_UNUSED, const char *component,
    unsigned char *key, size_t *length, fsimg_error_t *error)
{
	if (fsimg_fat_name_key(component, key, length, error) != FSIMG_OK)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_READER, "selection component is not representable in FAT"));
	return (FSIMG_OK);
}
