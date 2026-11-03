/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include "format.h"
#include "write-iso9660.h"

static fsimg_status_t iso_probe(const unsigned char *, size_t, uint64_t, bool,
    const fsimg_format_variant_t **, fsimg_error_t *);

/* One public name for every view and level; the view is a read setting.
 * The reader presents the Rock Ridge view, with symbolic links, FIFOs,
 * devices and sockets, owner, mode, device numbers and four times under
 * names of up to 1024 bytes, and the Joliet and primary hierarchies: files
 * and directories with their recording dates and hidden attribute.
 * Multi-section files reach the volume's size. Devices and sockets are
 * read-only kinds (specs/design-device-nodes.md): the catalog never holds
 * one, so the writer never meets them, and no writer stores a device
 * number, so metadata_fields does not list it.
 */
static const fsimg_format_variant_t iso_variants[] = {
	{
		.info = {
			.name = "iso9660",
			.profile = "rockridge/joliet/primary",
			.entry_kinds = FSIMG_ENTRY_FILE |
			    FSIMG_ENTRY_DIRECTORY | FSIMG_ENTRY_SYMLINK |
			    FSIMG_ENTRY_FIFO | FSIMG_ENTRY_CHAR_DEVICE |
			    FSIMG_ENTRY_BLOCK_DEVICE | FSIMG_ENTRY_SOCKET,
			.metadata_fields = FSIMG_META_CREATED |
			    FSIMG_META_MODIFIED | FSIMG_META_ACCESSED |
			    FSIMG_META_CHANGED | FSIMG_META_ATTRIBUTES |
			    FSIMG_META_OWNER | FSIMG_META_MODE,
			.sector_size_mask = ISO_BLOCK_BYTES,
			.allocation_unit_size_mask = ISO_BLOCK_BYTES,
			.write_allocation_unit_size_mask = ISO_BLOCK_BYTES,
			.max_file_bytes = ISO_VOLUME_BLOCKS_MAX *
			    ISO_BLOCK_BYTES,
			.max_volume_bytes = ISO_VOLUME_BLOCKS_MAX *
			    ISO_BLOCK_BYTES,
			.max_name_utf16_units = ISO_NAME_MAX,
			.max_link_target_bytes = ISO_NAME_MAX,
			.max_label_bytes = ISO_LABEL_BYTES,
			.write_settings = FSIMG_SETTING_ISO_LEVEL |
			    FSIMG_SETTING_ELTORITO |
			    FSIMG_SETTING_ISO_DESCRIPTOR,
			.read_settings = FSIMG_SETTING_ISO_VIEW
		},
		.id = 0
	}
};

_Static_assert(ISO_BLOCK_BYTES == 1u << 11,
    "the size masks name 2048-byte sectors and allocation units");

const fsimg_format_t fsimg_iso9660_format = {
	.variants = iso_variants,
	.variant_count = sizeof(iso_variants) / sizeof(iso_variants[0]),
	.probe_bytes = ISO_PROBE_BYTES,
	.probe = iso_probe,
	.reader = &fsimg_iso9660_reader_ops,
	.writer = &fsimg_iso9660_writer_ops,
	.reserved_system_area = true
};

/* Detection's signature is a volume descriptor at block 16: CD001 after a
 * type the profile lists (specs/design-iso9660-driver.md#probe-and-open).
 * No other probe's structures hold it there, and no probe scans for it
 * elsewhere, so CD001 inside another format's data draws a second claim
 * only at block 16. Only CD001 is identity: a caller assuming the identity
 * skips it, and an unknown type is then refused as open refuses it. With
 * the signature the probe claims the image. A
 * primary descriptor at block 16 is checked further for its block size
 * and volume size; any other first descriptor is left to open, which
 * finds the primary descriptor.
 */
static fsimg_status_t
iso_probe(const unsigned char *prefix, size_t length,
    uint64_t volume_bytes FSIMG_UNUSED, bool assume_identity,
    const fsimg_format_variant_t **variant, fsimg_error_t *error)
{
	const unsigned char *descriptor;
	uint32_t space_size;
	bool known;

	*variant = &iso_variants[0];
	if (length < ISO_PROBE_BYTES)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0,
		    FSIMG_OP_READER, "no ISO 9660 volume descriptor"));
	descriptor = prefix + ISO_SYSTEM_BLOCKS * ISO_BLOCK_BYTES;
	known = iso_descriptor_type_known(descriptor[0]);
	if (!assume_identity && (!iso_identity(descriptor) || !known))
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0,
		    FSIMG_OP_READER, "no ISO 9660 volume descriptor"));
	if (!known)
		return (iso_unsupported(error,
		    "unsupported ISO 9660 volume descriptor type or version"));
	if (descriptor[0] != ISO_VD_PRIMARY)
		return (FSIMG_OK);
	return (iso_geometry_decode(descriptor, &space_size, error));
}
