/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "iso9660.h"

static fsimg_status_t iso_open(fsimg_reader_t *,
    const fsimg_format_variant_t *, fsimg_error_t *);
static void iso_close(fsimg_reader_t *);
static fsimg_status_t iso_resolve(fsimg_reader_t *, uint64_t,
    fsimg_extent_t *, fsimg_error_t *);

/* The reader has no full verifier: a report could not claim the ownership
 * sweep, path tables, order and every offered view of
 * specs/design-iso9660-driver.md#verification, so the format advertises no
 * verify capability and fsimg_verify refuses before any work.
 */
const fsimg_reader_ops_t fsimg_iso9660_reader_ops = {
	.open = iso_open,
	.close = iso_close,
	.prepare = iso_prepare,
	.replay = iso_replay,
	.keys = iso_keys,
	.key = iso_key,
	.ambiguous = "duplicate ISO 9660 name or directory in one directory",
	.name_profile = FSIMG_NAME_PROFILE_PORTABLE,
	.resolve = iso_resolve
};

fsimg_iso9660_reader_t *
iso_state(const fsimg_reader_t *reader)
{
	return (reader->driver);
}

fsimg_status_t
iso_corrupt(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, FSIMG_ECORRUPT, 0, FSIMG_OP_READER,
	    message));
}

fsimg_status_t
iso_unsupported(fsimg_error_t *error, const char *message)
{
	return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0, FSIMG_OP_READER,
	    message));
}

/* The profile: the selected view's name. Open never leaves the view auto,
 * and the switch names every view so a new one fails -Wswitch.
 */
static const char *
view_name(fsimg_iso_view_t view)
{
	switch (view) {
	case FSIMG_ISO_VIEW_ROCKRIDGE:
		return ("rockridge");
	case FSIMG_ISO_VIEW_JOLIET:
		return ("joliet");
	case FSIMG_ISO_VIEW_PRIMARY:
	case FSIMG_ISO_VIEW_AUTO:
		break;
	}
	return ("primary");
}

/* The boot entries for platform; zero without El Torito. */
static unsigned
boot_platform_count(const iso_boot_t *boot, uint8_t platform)
{
	unsigned count = 0;

	for (unsigned i = 0; i < boot->entry_count; i++)
		count += boot->entries[i].platform == platform;
	return (count);
}

/* The named details, in the order the driver design's volume information
 * documents. The table is sized by its initializer, so a detail that
 * would not fit the volume information fails the build.
 */
static void
report_details(const fsimg_iso9660_reader_t *iso, fsimg_volume_info_t *info)
{
	const fsimg_volume_detail_t details[] = {
		{ "rockridge", iso->rockridge },
		{ "joliet_level", iso->joliet_level },
		{ "enhanced", iso->enhanced ? 1 : 0 },
		{ "descriptors", iso->descriptors },
		{ "terminator_block", iso->terminator_block },
		{ "root_block", iso_hierarchy(iso)->root.extent },
		{ "path_table_bytes", iso_hierarchy(iso)->path_table_bytes },
		{ "system_area_used", iso->system_area_used ? 1 : 0 },
		{ "missing_blocks", iso->primary.volume_space_size -
		    iso->readable_blocks },
		{ "boot_catalog_block", iso->boot.catalog_block },
		{ "boot_entries", iso->boot.entry_count },
		{ "boot_x86", boot_platform_count(&iso->boot, ISO_BOOT_X86) },
		{ "boot_efi", boot_platform_count(&iso->boot, ISO_BOOT_EFI) }
	};

	_Static_assert(sizeof(details) <= sizeof(info->details),
	    "the ISO 9660 details fit the volume information's table");
	bcopy(details, info->details, sizeof(details));
	info->detail_count = sizeof(details) / sizeof(details[0]);
}

static fsimg_status_t
iso_open(fsimg_reader_t *reader,
    const fsimg_format_variant_t *forced FSIMG_UNUSED, fsimg_error_t *error)
{
	fsimg_iso9660_reader_t *iso;
	fsimg_volume_info_t *info = &reader->info;
	fsimg_error_t detail;
	fsimg_status_t status;

	iso = fsimg_alloc(reader->ctx, sizeof(*iso), &detail);
	if (iso == NULL) {
		if (error != NULL)
			*error = detail;
		return (detail.status);
	}
	bzero(iso, sizeof(*iso));
	reader->driver = iso;
	status = iso_volume_open(reader, error);
	if (status != FSIMG_OK)
		return (status);

	strcpy(info->format, "iso9660");
	strcpy(info->profile, view_name(iso->view));
	info->valid |= FSIMG_VOLUME_GEOMETRY;
	_Static_assert(ISO_LABEL_TEXT_MAX < sizeof(info->label),
	    "an ISO 9660 label fits the volume information");
	if (iso_label_decode(iso_hierarchy(iso), info->label) &&
	    info->label[0] != '\0')
		info->valid |= FSIMG_VOLUME_LABEL;
	info->volume_bytes = (uint64_t)iso->primary.volume_space_size *
	    ISO_BLOCK_BYTES;
	info->sector_bytes = ISO_BLOCK_BYTES;
	info->allocation_unit_bytes = ISO_BLOCK_BYTES;
	info->allocation_units = iso->primary.volume_space_size;
	info->time_basis = FSIMG_TIME_OFFSET;
	info->scope = FSIMG_VALIDATION_HEADER;
	/* The format records no clean or error state. */
	info->clean = true;
	info->io_error = false;
	report_details(iso, info);
	return (FSIMG_OK);
}

static void
iso_close(fsimg_reader_t *reader)
{
	fsimg_iso9660_reader_t *iso = iso_state(reader);

	if (iso == NULL)
		return;
	fsimg_extent_index_free(iso->file);
	fsimg_dealloc(iso);
	reader->driver = NULL;
}

/* Replay indexed the current file's sections, each validated against the
 * volume and the content domain, so a lookup is the whole of resolution.
 */
static fsimg_status_t
iso_resolve(fsimg_reader_t *reader, uint64_t offset, fsimg_extent_t *extent,
    fsimg_error_t *error)
{
	return (fsimg_extent_index_lookup(iso_state(reader)->file, offset,
	    extent, error));
}
