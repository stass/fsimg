/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "ifs.h"

static fsimg_status_t ifs_open(fsimg_reader_t *,
    const fsimg_format_variant_t *, fsimg_error_t *);
static void ifs_close(fsimg_reader_t *);
static fsimg_status_t ifs_resolve(fsimg_reader_t *, uint64_t,
    fsimg_extent_t *, fsimg_error_t *);
static uint64_t shadowed_references(const fsimg_ifs_reader_t *);
static void report_details(const fsimg_ifs_reader_t *, fsimg_volume_info_t *);
static void report_info(fsimg_reader_t *);

const fsimg_reader_ops_t fsimg_ifs_reader_ops = {
	.open = ifs_open,
	.close = ifs_close,
	.prepare = ifs_prepare,
	.replay = ifs_replay,
	.keys = ifs_keys,
	.key = ifs_key,
	.ambiguous = "duplicate ImageFS path",
	.name_profile = FSIMG_NAME_PROFILE_QNX,
	.resolve = ifs_resolve,
	.verify = ifs_verify
};

fsimg_ifs_reader_t *
ifs_state(const fsimg_reader_t *reader)
{
	return (reader->driver);
}

fsimg_status_t
ifs_corrupt(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_READER,
	    message));
}

fsimg_status_t
ifs_unsupported(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0, FSIMG_OP_READER,
	    message));
}

/* Open validates the header, the mountpoint and every record, then derives
 * the hierarchy; every value info reports is known when it returns.
 */
static fsimg_status_t
ifs_open(fsimg_reader_t *reader,
    const fsimg_format_variant_t *forced FSIMG_UNUSED, fsimg_error_t *error)
{
	fsimg_ifs_reader_t *ifs;
	fsimg_stream_t *paths;
	fsimg_error_t detail;
	fsimg_status_t status;

	ifs = fsimg_alloc(reader->ctx, sizeof(*ifs), &detail);
	if (ifs == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	bzero(ifs, sizeof(*ifs));
	reader->driver = ifs;
	status = ifs_volume_open(reader, &paths, error);
	if (status != FSIMG_OK)
		return (status);
	status = ifs_tree_build(reader, paths, error);
	if (status != FSIMG_OK)
		return (status);
	report_info(reader);
	return (FSIMG_OK);
}

/* The nonzero references whose inode word a shadowed record carries. */
static uint64_t
shadowed_references(const fsimg_ifs_reader_t *ifs)
{
	uint64_t count = 0;

	for (unsigned i = 0; i < IFS_REFERENCES; i++)
		if (ifs->counts.reference_shadowed[i])
			count++;
	return (count);
}

/* The header's boot state and the open passes' counts as named details,
 * in the documented order. The table is sized by its initializer, so a
 * detail that would not fit the volume information fails the build.
 */
static void
report_details(const fsimg_ifs_reader_t *ifs, fsimg_volume_info_t *info)
{
	const ifs_header_t *header = &ifs->header;
	const fsimg_volume_detail_t details[] = {
		{ "mount_flags", header->mount_flags },
		{ "boot_ino_0", header->boot_ino[0] },
		{ "boot_ino_1", header->boot_ino[1] },
		{ "boot_ino_2", header->boot_ino[2] },
		{ "boot_ino_3", header->boot_ino[3] },
		{ "script_ino", header->script_ino },
		{ "chain_paddr", header->chain_paddr },
		/* Records by inode flag nibble 0x0, 0x8, 0xc and 0xe. */
		{ "flags_0", ifs->counts.records_by_flag[0] },
		{ "flags_8", ifs->counts.records_by_flag[1] },
		{ "flags_c", ifs->counts.records_by_flag[2] },
		{ "flags_e", ifs->counts.records_by_flag[3] },
		{ "implicit_directories", ifs->counts.implicit_directories },
		{ "shadowed", ifs->counts.shadowed },
		{ "omitted", ifs->counts.omitted },
		{ "omitted_references", ifs->counts.omitted_references },
		{ "shadowed_references", shadowed_references(ifs) }
	};

	_Static_assert(IFS_FLAG_NIBBLES == 4, "four flag details");
	_Static_assert(sizeof(details) <= sizeof(info->details),
	    "the ImageFS details fit the volume information's table");
	bcopy(details, info->details, sizeof(details));
	info->detail_count = sizeof(details) / sizeof(details[0]);
}

/* The volume information: byte geometry over image_size, the mountpoint
 * when its bytes are valid, and the details.
 */
static void
report_info(fsimg_reader_t *reader)
{
	fsimg_ifs_reader_t *ifs = ifs_state(reader);
	const ifs_header_t *header = &ifs->header;
	fsimg_volume_info_t *info = &reader->info;
	bool bootable;

	bootable = false;
	for (unsigned i = 0; i < IFS_REFERENCES; i++)
		if (ifs_reference(header, i) != 0)
			bootable = true;
	strcpy(info->format, "ifs");
	strcpy(info->profile, header->order == IFS_BIG_ENDIAN ?
	    "big-endian" : "little-endian");
	if (bootable)
		strcat(info->profile, ", bootable");
	info->valid |= FSIMG_VOLUME_GEOMETRY;
	info->volume_bytes = header->image_size;
	info->sector_bytes = 1;
	info->allocation_unit_bytes = 1;
	info->allocation_units = header->image_size;
	info->time_basis = FSIMG_TIME_UTC;
	info->scope = FSIMG_VALIDATION_HEADER;
	info->clean = true;
	info->io_error = false;
	if (ifs->mountpoint_valid) {
		info->valid |= FSIMG_VOLUME_MOUNTPOINT;
		bcopy(ifs->mountpoint, info->mountpoint, sizeof(ifs->mountpoint));
	}
	report_details(ifs, info);
}

static void
ifs_close(fsimg_reader_t *reader)
{
	fsimg_ifs_reader_t *ifs = ifs_state(reader);

	if (ifs == NULL)
		return;
	fsimg_stream_free(ifs->children);
	fsimg_array_free(ifs->groups);
	fsimg_extent_index_free(ifs->file);
	fsimg_dealloc(ifs);
	reader->driver = NULL;
}

/* The current file's or link's one extent, which replay built. */
static fsimg_status_t
ifs_resolve(fsimg_reader_t *reader, uint64_t offset, fsimg_extent_t *extent,
    fsimg_error_t *error)
{
	return (fsimg_extent_index_lookup(ifs_state(reader)->file, offset,
	    extent, error));
}
