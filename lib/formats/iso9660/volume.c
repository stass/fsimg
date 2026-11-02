/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <strings.h>

#include "iso9660.h"

/*
 * Opening an ISO 9660 volume (specs/design-iso9660-driver.md#probe-and-open):
 * the descriptor scan, the geometry of each hierarchy, the system area, the
 * El Torito boot catalog and the view, each step failing before the next
 * reads. Open reads at most the 65 descriptor blocks, the 16 system-area
 * blocks, the boot catalog block and, when the view may be Rock Ridge,
 * the primary root's first block with its at most 16 continuation
 * blocks, one at a time through the reader's block windows.
 */

/* Read volume block number into window unless the window holds it,
 * charging one work unit per read, so every walk over blocks observes the
 * session's limits and cancellation. The block lies within the readable
 * volume, which before the geometry is known is the slice's whole blocks.
 */
fsimg_status_t
iso_window_read(fsimg_reader_t *reader, iso_window_t *window, uint64_t block,
    fsimg_error_t *error)
{
	fsimg_iso9660_reader_t *iso = iso_state(reader);
	size_t transferred;
	fsimg_status_t status;

	if (window->valid && window->block == block)
		return (FSIMG_OK);
	if (block >= iso->readable_blocks)
		return (iso_corrupt(error,
		    "ISO 9660 structure lies past the end of the image"));
	window->valid = false;
	status = fsimg_session_charge(&reader->session, 0, 0, 1, error);
	if (status == FSIMG_OK)
		status = fsimg_reader_bytes(reader, block * ISO_BLOCK_BYTES,
		    window->bytes, ISO_BLOCK_BYTES, &transferred, error);
	if (status != FSIMG_OK)
		return (status);
	window->block = block;
	window->valid = true;
	return (FSIMG_OK);
}

/* Read a descriptor, system-area, boot catalog or directory block into
 * the reader's window: every block but a SUSP continuation block.
 */
fsimg_status_t
iso_block_read(fsimg_reader_t *reader, uint64_t block, fsimg_error_t *error)
{
	return (iso_window_read(reader, &iso_state(reader)->window, block,
	    error));
}

/* Whether blocks blocks at start lie within the volume, without a sum
 * that could wrap.
 */
bool
iso_in_volume(const fsimg_iso9660_reader_t *iso, uint64_t start,
    uint64_t blocks)
{
	uint64_t size = iso->primary.volume_space_size;

	return (blocks <= size && start <= size - blocks);
}

/* Check the extent of blocks blocks at start before it is read: within the
 * volume, and within the slice, whose missing tail is corruption once a
 * structure the reader uses lies in it.
 */
fsimg_status_t
iso_extent_check(const fsimg_iso9660_reader_t *iso, uint64_t start,
    uint64_t blocks, fsimg_error_t *error)
{
	if (!iso_in_volume(iso, start, blocks))
		return (iso_corrupt(error,
		    "ISO 9660 extent lies outside the volume"));
	if (start + blocks > iso->readable_blocks)	/* both below 2^32 */
		return (iso_corrupt(error,
		    "ISO 9660 extent lies past the end of the image"));
	return (FSIMG_OK);
}

/* Read descriptors from block 16 until the terminator, at most 64 before
 * it, keeping the first primary descriptor, the Joliet descriptor and the
 * El Torito boot record. A later primary descriptor must repeat the first
 * byte for byte.
 */
static fsimg_status_t
descriptors_scan(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_iso9660_reader_t *iso = iso_state(reader);
	unsigned char first_primary[ISO_BLOCK_BYTES];
	iso_descriptor_t decoded;
	uint32_t block;
	bool have_primary;
	fsimg_status_t status;

	have_primary = false;
	for (uint32_t i = 0; ; i++) {
		block = ISO_SYSTEM_BLOCKS + i;
		/* Past 64 descriptors only a terminator is decoded. */
		if (i == ISO_DESCRIPTORS_MAX && block >= iso->readable_blocks)
			return (iso_unsupported(error, "ISO 9660 descriptor set "
			    "has no terminator within 64 descriptors"));
		status = iso_block_read(reader, block, error);
		if (status == FSIMG_OK && i == ISO_DESCRIPTORS_MAX &&
		    iso->window.bytes[0] != ISO_VD_TERMINATOR)
			return (iso_unsupported(error, "ISO 9660 descriptor set "
			    "has no terminator within 64 descriptors"));
		if (status == FSIMG_OK)
			status = iso_descriptor_decode(iso->window.bytes, block,
			    &decoded, error);
		if (status != FSIMG_OK)
			return (status);
		if (decoded.type == ISO_VD_TERMINATOR)
			break;
		iso->descriptors++;
		if (decoded.type == ISO_VD_PRIMARY) {
			if (!have_primary) {
				iso->primary = decoded;
				bcopy(iso->window.bytes, first_primary,
				    sizeof(first_primary));
				have_primary = true;
			} else if (bcmp(iso->window.bytes, first_primary,
			    sizeof(first_primary)) != 0)
				return (iso_unsupported(error,
				    "ISO 9660 primary descriptors differ"));
		} else if (decoded.type == ISO_VD_BOOT) {
			status = iso_boot_record_decode(iso->window.bytes,
			    &iso->boot, error);
			if (status != FSIMG_OK)
				return (status);
		} else if (decoded.type == ISO_VD_SUPPLEMENTARY) {
			if (decoded.version == 2)
				iso->enhanced = true;
			else if (decoded.joliet_level != 0) {
				if (iso->joliet_level != 0)
					return (iso_unsupported(error,
					    "ISO 9660 volume has two Joliet "
					    "descriptors"));
				iso->joliet = decoded;
				iso->joliet_level = decoded.joliet_level;
			}
		}
	}
	iso->terminator_block = block;
	if (!have_primary)
		return (iso_corrupt(error,
		    "ISO 9660 descriptor set has no primary descriptor"));
	return (FSIMG_OK);
}

/* The structures one descriptor names, against the volume: its root
 * directory and its path tables with their optional copies. Path tables
 * are not read until verification, so only their placement is checked.
 */
static fsimg_status_t
hierarchy_check(const fsimg_iso9660_reader_t *iso,
    const iso_descriptor_t *descriptor, fsimg_error_t *error)
{
	uint64_t table_blocks = ISO_BLOCKS(descriptor->path_table_bytes);

	if (!iso_in_volume(iso, descriptor->root.extent,
	    ISO_BLOCKS(descriptor->root.data_length)))
		return (iso_corrupt(error,
		    "ISO 9660 root directory lies outside the volume"));
	if (!iso_in_volume(iso, descriptor->path_table_l, table_blocks) ||
	    !iso_in_volume(iso, descriptor->path_table_m, table_blocks) ||
	    (descriptor->path_table_l_copy != 0 &&
	    !iso_in_volume(iso, descriptor->path_table_l_copy, table_blocks)) ||
	    (descriptor->path_table_m_copy != 0 &&
	    !iso_in_volume(iso, descriptor->path_table_m_copy, table_blocks)))
		return (iso_corrupt(error,
		    "ISO 9660 path table lies outside the volume"));
	return (FSIMG_OK);
}

/* The volume is the primary descriptor's block count; the readable volume
 * also ends at the slice's last whole block, and info reports the blocks
 * that leaves out. The descriptor set lies within the volume. A Joliet
 * descriptor repeats the primary descriptor's geometry, which decoding
 * already held to the profile, so a Joliet value that differs contradicts
 * the primary descriptor.
 */
static fsimg_status_t
geometry_check(fsimg_iso9660_reader_t *iso, fsimg_error_t *error)
{
	uint64_t blocks = iso->primary.volume_space_size;
	fsimg_status_t status;

	if (iso->terminator_block >= blocks)
		return (iso_corrupt(error,
		    "ISO 9660 descriptor set extends past the volume"));
	if (iso->readable_blocks > blocks)
		iso->readable_blocks = blocks;
	status = hierarchy_check(iso, &iso->primary, error);
	if (status != FSIMG_OK || iso->joliet_level == 0)
		return (status);
	if (iso->joliet.volume_space_size != blocks ||
	    iso->joliet.block_size != iso->primary.block_size ||
	    iso->joliet.set_size != iso->primary.set_size ||
	    iso->joliet.sequence != iso->primary.sequence)
		return (iso_corrupt(error, "ISO 9660 Joliet descriptor "
		    "disagrees with the primary geometry"));
	return (hierarchy_check(iso, &iso->joliet, error));
}

/* Whether any byte of blocks 0 to 15 is nonzero, for info only: the system
 * area has no structure and its contents never fail an image.
 */
static fsimg_status_t
system_area_scan(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_iso9660_reader_t *iso = iso_state(reader);
	fsimg_status_t status;

	for (uint32_t block = 0; block < ISO_SYSTEM_BLOCKS &&
	    !iso->system_area_used; block++) {
		status = iso_block_read(reader, block, error);
		if (status != FSIMG_OK)
			return (status);
		iso->system_area_used = !iso_bytes_zero(iso->window.bytes,
		    ISO_BLOCK_BYTES);
	}
	return (FSIMG_OK);
}

/* Select the view (specs/design-iso9660.md#view-selection). auto and a
 * forced rockridge run the handshake at the primary root; auto takes
 * Rock Ridge when it is present with a skip count of zero and otherwise
 * the primary view. A forced view never falls back: rockridge without a
 * readable Rock Ridge, and joliet, which this reader does not present,
 * are unsupported. A forced primary view never decodes the root's system
 * use area, so a damaged handshake cannot keep a caller from it.
 */
static fsimg_status_t
view_select(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_iso9660_reader_t *iso = iso_state(reader);
	fsimg_iso_view_t view = reader->options.iso_view;
	fsimg_status_t status;

	iso->rockridge = ISO_ROCKRIDGE_UNKNOWN;
	if (view == FSIMG_ISO_VIEW_AUTO || view == FSIMG_ISO_VIEW_ROCKRIDGE) {
		status = iso_handshake(reader, error);
		if (status != FSIMG_OK)
			return (status);
		if (iso->rockridge == ISO_ROCKRIDGE_PRESENT)
			view = FSIMG_ISO_VIEW_ROCKRIDGE;
		else if (view == FSIMG_ISO_VIEW_AUTO)
			view = FSIMG_ISO_VIEW_PRIMARY;
		else
			return (iso_unsupported(error, iso->rockridge ==
			    ISO_ROCKRIDGE_SKIP ? "ISO 9660 Rock Ridge with a "
			    "nonzero SUSP skip count is unsupported" :
			    "requested ISO 9660 view is not available"));
	}
	if (view == FSIMG_ISO_VIEW_JOLIET)
		return (iso_unsupported(error,
		    "requested ISO 9660 view is not available"));
	iso->view = view;
	iso->view_root = iso->primary.root;
	return (FSIMG_OK);
}

fsimg_status_t
iso_volume_open(fsimg_reader_t *reader, fsimg_error_t *error)
{
	fsimg_iso9660_reader_t *iso = iso_state(reader);
	uint64_t slice_bytes, start, end;
	fsimg_status_t status;

	status = fsimg_io_get_size(reader->input, &slice_bytes, error);
	if (status != FSIMG_OK)
		return (status);
	iso->readable_blocks = slice_bytes / ISO_BLOCK_BYTES;
	status = descriptors_scan(reader, error);
	if (status == FSIMG_OK)
		status = geometry_check(iso, error);
	if (status == FSIMG_OK)
		status = system_area_scan(reader, error);
	if (status == FSIMG_OK)
		status = iso_boot_open(reader, error);
	if (status == FSIMG_OK)
		status = view_select(reader, error);
	if (status != FSIMG_OK)
		return (status);

	/* File data lies after the descriptor set and within the readable
	 * volume, so no delivered byte comes from the system area, a
	 * descriptor or the missing tail.
	 */
	start = ((uint64_t)iso->terminator_block + 1) * ISO_BLOCK_BYTES;
	end = iso->readable_blocks * ISO_BLOCK_BYTES;
	reader->content_domain = (fsimg_extent_domain_t){
		.offset = start,
		.length = end > start ? end - start : 0,
		.unit_bytes = ISO_BLOCK_BYTES
	};
	return (FSIMG_OK);
}
