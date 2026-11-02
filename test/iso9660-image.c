/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "host.h"
#include "test.h"
#include "iso9660-image.h"

static void verify_directory_limit_block(unsigned char *p, unsigned block);
static void verify_scale_block(unsigned char *p, unsigned block);

void
put16(unsigned char *p, uint16_t value)
{
	p[0] = value;
	p[1] = value >> 8;
	p[2] = value >> 8;
	p[3] = value;
}

void
put32(unsigned char *p, uint32_t value)
{
	for (unsigned i = 0; i < 4; i++) {
		p[i] = value >> (8 * i);
		p[7 - i] = value >> (8 * i);
	}
}

size_t
record(unsigned char *p, const void *name, size_t name_length,
    uint32_t extent, uint32_t length, unsigned flags)
{
	size_t bytes = 33 + name_length + (name_length % 2 == 0);

	CHECK(bytes <= 254);
	bzero(p, bytes);
	p[0] = bytes;
	put32(p + DR_EXTENT, extent);
	put32(p + DR_SIZE, length);
	p[DR_FLAGS] = flags;
	put16(p + DR_SEQUENCE, 1);
	p[DR_NAME_SIZE] = name_length;
	bcopy(name, p + DR_NAME, name_length);
	return (bytes);
}

void
directory(unsigned char *p, uint32_t block, uint32_t parent, uint32_t length)
{
	unsigned char self = 0, up = 1;

	record(p, &self, 1, block, length, 2);
	record(p + 34, &up, 1, parent, BLOCK, 2);
	if (parent == block)
		put32(p + 34 + DR_SIZE, length);
}

void
descriptor(unsigned char *p, unsigned type, unsigned version)
{
	bzero(p, BLOCK);
	p[0] = type;
	bcopy("CD001", p + 1, 5);
	p[6] = version;
}

image_t
image_new(void)
{
	image_t image = { .length = BLOCKS * BLOCK, .stored = BLOCKS * BLOCK };
	unsigned char *p, zero = 0;

	image.bytes = calloc(BLOCKS, BLOCK);
	CHECK(image.bytes != NULL);
	p = image.bytes + 16 * BLOCK;
	descriptor(p, 1, 1);
	memset(p + 40, ' ', 32);
	bcopy("TEST ISO", p + 40, 8);
	put32(p + VD_SPACE, BLOCKS);
	put16(p + VD_SET, 1);
	put16(p + VD_SEQUENCE, 1);
	put16(p + VD_BLOCK, BLOCK);
	put32(p + VD_PATH_SIZE, 10);
	p[VD_PATH_L] = 82;
	p[VD_PATH_M + 3] = 83;
	record(p + VD_ROOT, &zero, 1, ROOT, BLOCK, 2);
	p[VD_STRUCTURE] = 1;
	descriptor(image.bytes + 17 * BLOCK, 255, 1);
	/* Type-L and type-M root path-table records. */
	p = image.bytes + 82 * BLOCK;
	p[0] = 1;
	p[2] = ROOT;
	p[6] = 1;
	p = image.bytes + 83 * BLOCK;
	p[0] = 1;
	p[5] = ROOT;
	p[7] = 1;
	p = image.bytes + ROOT * BLOCK;
	directory(p, ROOT, ROOT, BLOCK);
	record(p + DOT_PAIR_BYTES, "A.TXT;1", 7, DATA, 5, 0);
	memset(image.bytes + DATA * BLOCK, 0xa5, BLOCK);
	bcopy("hello", image.bytes + DATA * BLOCK, 5);
	return (image);
}

fsimg_status_t
image_size(void *arg, uint64_t *size, fsimg_error_t *error FSIMG_UNUSED)
{
	*size = ((image_t *)arg)->length;
	return (FSIMG_OK);
}

fsimg_status_t
image_read(void *arg, uint64_t offset, void *buffer, size_t length,
    size_t *count, fsimg_error_t *error FSIMG_UNUSED)
{
	image_t *image = arg;

	CHECK(offset <= image->length && length <= image->length - offset);
	image->reads++;
	if (image->verify_report != NULL &&
	    image->verify_report->files == 2 && offset == RR_AREA * BLOCK) {
		*count = 0;
		return (FSIMG_EIO);
	}
	if (offset <= RR_AREA * BLOCK && offset + length > RR_AREA * BLOCK) {
		image->continuation_reads++;
		image->ce_read_index = image->reads;
	}
	*count = 0;
	if (image->reads == image->fail_read) {
		if (image->cancel_read)
			return (FSIMG_ECANCELLED);
		return (image->eof ? FSIMG_OK : FSIMG_EIO);
	}
	if (offset + length > image->high_read)
		image->high_read = offset + length;
	bzero(buffer, length);
	if (offset < image->stored) {
		size_t stored = image->stored - offset;

		if (stored > length)
			stored = length;
		bcopy(image->bytes + offset, buffer, stored);
	}
	if (image->scale || image->directory_scale) {
		unsigned char block[BLOCK];
		uint64_t begin = ROOT * BLOCK;
		uint64_t end = image->directory_scale ? image->length :
		    begin + (uint64_t)VERIFY_SCALE_BLOCKS * BLOCK;
		uint64_t cursor = offset > begin ? offset : begin;

		while (cursor < end && cursor < offset + length) {
			uint64_t index = (cursor - begin) / BLOCK;
			size_t within = (cursor - begin) % BLOCK;
			size_t bytes = BLOCK - within;

			if (bytes > offset + length - cursor)
				bytes = offset + length - cursor;
			if (image->directory_scale)
				verify_directory_limit_block(block, ROOT + index);
			else
				verify_scale_block(block, index);
			bcopy(block + within, (unsigned char *)buffer + cursor - offset,
			    bytes);
			cursor += bytes;
		}
	}
	*count = length;
	return (FSIMG_OK);
}

void
fixture_new(fixture_t *f, image_t *image, const fsimg_options_t *options)
{
	fsimg_options_t local;
	fsimg_io_ops_t ops;
	int scratch;

	bzero(f, sizeof(*f));
	fsimg_options_init(&local);
	if (options != NULL)
		local = *options;
	scratch = open(".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(scratch >= 0);
	local.scratch_directory_fd = scratch;
	CHECK(fsimg_ctx_new(&local, &f->ctx, NULL) == FSIMG_OK);
	CHECK(close(scratch) == 0);
	CHECK(fsimg_ctx_get_stats(f->ctx, &f->baseline, NULL) == FSIMG_OK);
	fsimg_io_ops_init(&ops);
	ops.read_at = image_read;
	ops.get_size = image_size;
	CHECK(fsimg_io_from_callbacks(f->ctx, &ops, image, &f->io, NULL) ==
	    FSIMG_OK);
}

void
fixture_free(fixture_t *f)
{
	fsimg_stats_t stats;

	fsimg_reader_free(f->reader);
	fsimg_io_free(f->io);
	CHECK(fsimg_ctx_get_stats(f->ctx, &stats, NULL) == FSIMG_OK);
	CHECK(stats.memory_bytes == f->baseline.memory_bytes);
	CHECK(stats.scratch_bytes == 0);
	CHECK(stats.descriptors == f->baseline.descriptors);
	fsimg_ctx_free(f->ctx);
}

fsimg_status_t
open_image(fixture_t *f, fsimg_iso_view_t view)
{
	fsimg_read_options_t options;

	fsimg_read_options_init(&options);
	options.iso_view = view;
	return (fsimg_reader_open(f->ctx, f->io, &options, &f->reader, NULL));
}

fsimg_status_t
walk(fixture_t *f)
{
	const fsimg_entry_t *entry;
	fsimg_status_t status;

	while ((status = fsimg_reader_next(f->reader, &entry, NULL)) ==
	    FSIMG_OK)
		;
	return (status == FSIMG_DONE ? FSIMG_OK : status);
}

bool
cancelled(void *argument)
{
	unsigned *remaining = argument;

	if (*remaining == 0)
		return (true);
	(*remaining)--;
	return (false);
}

void
boot_le16(unsigned char *p, uint16_t value)
{
	p[0] = value;
	p[1] = value >> 8;
}

void
boot_le32(unsigned char *p, uint32_t value)
{
	for (unsigned i = 0; i < 4; i++)
		p[i] = value >> (8 * i);
}

void
boot_record(unsigned char *p, uint32_t catalog)
{
	descriptor(p, 0, 1);
	bcopy("EL TORITO SPECIFICATION", p + 7, 23);
	boot_le32(p + 71, catalog);
}

void
boot_checksum(unsigned char *p)
{
	uint32_t sum = 0;

	boot_le16(p + 28, 0);
	for (unsigned i = 0; i < 32; i += 2)
		sum += p[i] + 256u * p[i + 1];
	boot_le16(p + 28, (uint16_t)(0u - sum));
}

void
boot_validation(unsigned char *p, unsigned platform)
{
	bzero(p, 32);
	p[0] = 1;
	p[1] = platform;
	memset(p + 4, 'V', 24);
	p[30] = 0x55;
	p[31] = 0xaa;
	boot_checksum(p);
}

void
boot_entry(unsigned char *p, unsigned media, uint16_t sectors,
    uint32_t image_block)
{
	bzero(p, 32);
	p[0] = 0x88;
	p[1] = media;
	boot_le16(p + 2, 0x7c0);
	p[4] = 0x83;
	boot_le16(p + 6, sectors);
	boot_le32(p + 8, image_block);
}

void
boot_header(unsigned char *p, unsigned marker, unsigned platform,
    uint16_t count)
{
	bzero(p, 32);
	p[0] = marker;
	p[1] = platform;
	boot_le16(p + 2, count);
	memset(p + 4, 'S', 28);
}

image_t
boot_image(unsigned platform)
{
	image_t image = image_new();
	unsigned char *catalog = image.bytes + BOOT_CATALOG * BLOCK;

	boot_record(image.bytes + 17 * BLOCK, BOOT_CATALOG);
	descriptor(image.bytes + 18 * BLOCK, 255, 1);
	boot_validation(catalog, platform);
	boot_entry(catalog + 32, 0, 4, DATA);
	return (image);
}

void
boot_efi_section(image_t *image)
{
	unsigned char *catalog = image->bytes + BOOT_CATALOG * BLOCK;

	boot_header(catalog + 64, 0x91, 0xef, 1);
	boot_entry(catalog + 96, 0, 4, DATA + 1);
}

unsigned char *
susp_entry(susp_area_t *area, const char *signature, size_t length)
{
	unsigned char *p;

	CHECK(length >= 4 && length <= 255);
	CHECK(area->length + length <= sizeof(area->bytes));
	p = area->bytes + area->length;
	bzero(p, length);
	bcopy(signature, p, 2);
	p[2] = length;
	p[3] = 1;
	area->length += length;
	return (p);
}

void
susp_px(susp_area_t *area, unsigned mode, size_t length)
{
	unsigned char *p = susp_entry(area, "PX", length);

	put32(p + 4, mode);
	put32(p + 12, 1);
	put32(p + 20, 123);
	put32(p + 28, 456);
}

void
susp_nm(susp_area_t *area, const void *name, size_t length, unsigned flags)
{
	unsigned char *p = susp_entry(area, "NM", 5 + length);

	p[4] = flags;
	bcopy(name, p + 5, length);
}

void
susp_ce(susp_area_t *area, unsigned block, unsigned offset, unsigned length)
{
	unsigned char *p = susp_entry(area, "CE", 28);

	put32(p + 4, block);
	put32(p + 12, offset);
	put32(p + 20, length);
}

void
susp_sl(susp_area_t *area, unsigned flags, unsigned component,
    const void *text, size_t length)
{
	unsigned char *p = susp_entry(area, "SL", 7 + length);

	p[4] = flags;
	p[5] = component;
	p[6] = length;
	bcopy(text, p + 7, length);
}

void
susp_er(susp_area_t *area, const char *identifier, unsigned version)
{
	size_t length = strlen(identifier);
	unsigned char *p = susp_entry(area, "ER", 8 + length);

	p[4] = length;
	p[7] = version;
	bcopy(identifier, p + 8, length);
}

image_t
rr_image(const susp_area_t *area, unsigned flags)
{
	image_t image = image_new();
	susp_area_t handshake = {0}, continuation = {0};
	unsigned char *root = image.bytes + ROOT * BLOCK;
	unsigned char *p, self = 0, up = 1;
	size_t length;

	p = susp_entry(&handshake, "SP", 7);
	p[4] = 0xbe;
	p[5] = 0xef;
	susp_er(&handshake, "RRIP_1991A", 1);
	CHECK(handshake.length == 25);
	bzero(root, BLOCK);
	length = record(root, &self, 1, ROOT, BLOCK, 2);
	bcopy(handshake.bytes, root + length, handshake.length);
	root[0] += handshake.length + (handshake.length % 2);
	record(root + root[0], &up, 1, ROOT, BLOCK, 2);
	susp_ce(&continuation, RR_AREA, 0, area->length);
	p = root + RR_CHILD;
	length = record(p, "A.TXT;1", 7, DATA, 5, flags);
	bcopy(continuation.bytes, p + length, continuation.length);
	p[0] += continuation.length;
	bcopy(area->bytes, image.bytes + RR_AREA * BLOCK, area->length);
	return (image);
}

unsigned char *
rr_child_directory(image_t *image)
{
	unsigned char *child = image->bytes + ROOT * BLOCK + RR_CHILD;
	unsigned char *contents = image->bytes + RR_DIRECTORY * BLOCK;

	put32(child + DR_EXTENT, RR_DIRECTORY);
	put32(child + DR_SIZE, BLOCK);
	directory(contents, RR_DIRECTORY, ROOT, BLOCK);
	return (contents);
}

/* Build a root with one valid child directory, placing the supplied SUSP
 * area on exactly one of the four dot records. The root handshake precedes
 * any supplied entries on its first dot record.
 */
image_t
rr_dot_image(const susp_area_t *dot, bool root, bool parent)
{
	image_t image = image_new();
	susp_area_t area;
	unsigned char *p;
	size_t length;

	for (unsigned level = 0; level < 2; level++) {
		unsigned extent = level == 0 ? ROOT : RR_DIRECTORY;

		p = image.bytes + extent * BLOCK;
		bzero(p, BLOCK);
		for (unsigned char identifier = 0; identifier < 2;
		    identifier++) {
			bzero(&area, sizeof(area));
			if (level == 0 && identifier == 0) {
				unsigned char *sp;

				sp = susp_entry(&area, "SP", SP_BYTES);
				sp[4] = 0xbe;
				sp[5] = 0xef;
				susp_er(&area, "RRIP_1991A", 1);
			}
			if (root == (level == 0) &&
			    parent == (identifier == 1)) {
				bcopy(dot->bytes, area.bytes + area.length,
				    dot->length);
				area.length += dot->length;
			}
			length = record(p, &identifier, 1,
			    identifier == 0 ? extent : ROOT, BLOCK, 2);
			bcopy(area.bytes, p + length, area.length);
			length += area.length;
			p[0] = length + (length % 2);
			p += p[0];
		}
		if (level == 0)
			record(p, "CHILD", 5, RR_DIRECTORY, BLOCK, 2);
	}
	return (image);
}

/* Add a Joliet descriptor with its own root and both path tables. */
void
joliet_add(image_t *image)
{
	unsigned char *p = image->bytes + 17 * BLOCK;
	unsigned char zero = 0;

	bcopy(image->bytes + 16 * BLOCK, p, BLOCK);
	p[0] = 2;
	bzero(p + VD_ESCAPE, 32);
	bcopy("%/E", p + VD_ESCAPE, 3);
	bzero(p + VD_LABEL, 32);
	put32(p + VD_PATH_SIZE, 22);
	bzero(p + VD_PATH_L, 16);
	p[VD_PATH_L] = 90;
	p[VD_PATH_M + 3] = 91;
	record(p + VD_ROOT, &zero, 1, JOLIET_ROOT, BLOCK, 2);
	descriptor(image->bytes + 18 * BLOCK, 255, 1);
	directory(image->bytes + JOLIET_ROOT * BLOCK,
	    JOLIET_ROOT, JOLIET_ROOT, BLOCK);
	/* Path tables include an otherwise unused child to distinguish size. */
	p = image->bytes + 90 * BLOCK;
	p[0] = 1;
	p[2] = JOLIET_ROOT;
	p[6] = 1;
	p[10] = 2;
	p[12] = JOLIET_CHILD;
	p[16] = 1;
	p[19] = 'D';
	p = image->bytes + 91 * BLOCK;
	p[0] = 1;
	p[5] = JOLIET_ROOT;
	p[7] = 1;
	p[10] = 2;
	p[15] = JOLIET_CHILD;
	p[17] = 1;
	p[19] = 'D';
}

/* Write a directory record whose identifier is big-endian UTF-16. */
size_t
joliet_record(unsigned char *p, const uint16_t *units, size_t count,
    uint32_t extent, uint32_t size, unsigned flags)
{
	unsigned char name[220];

	CHECK(count <= sizeof(name) / 2);
	for (size_t i = 0; i < count; i++) {
		name[2 * i] = units[i] >> 8;
		name[2 * i + 1] = units[i];
	}
	return (record(p, name, count * 2, extent, size, flags));
}

/* Verifier fixtures encode the redundant index independently of traversal.
 * Both byte orders are written explicitly; no library codec is an oracle.
 */
size_t
verify_path(unsigned char *p, const void *name, size_t length,
    uint32_t extent, uint16_t parent, bool big)
{
	size_t bytes = 8 + length + (length % 2);

	bzero(p, bytes);
	p[0] = length;
	for (unsigned i = 0; i < 4; i++)
		p[2 + i] = extent >> (8 * (big ? 3 - i : i));
	p[6] = big ? parent >> 8 : parent;
	p[7] = big ? parent : parent >> 8;
	bcopy(name, p + 8, length);
	return (bytes);
}

void
verify_table_root(image_t *image, unsigned vd, unsigned root,
    unsigned l, unsigned m)
{
	unsigned char zero = 0, *p = image->bytes + vd * BLOCK;

	put32(p + VD_PATH_SIZE, 10);
	bzero(p + VD_PATH_L, 16);
	boot_le32(p + VD_PATH_L, l);
	p[VD_PATH_M] = m >> 24;
	p[VD_PATH_M + 1] = m >> 16;
	p[VD_PATH_M + 2] = m >> 8;
	p[VD_PATH_M + 3] = m;
	verify_path(image->bytes + l * BLOCK, &zero, 1, root, 1, false);
	verify_path(image->bytes + m * BLOCK, &zero, 1, root, 1, true);
}

/* The wide root and its empty children are synthesized on demand. Each
 * root block holds 42 records (40 after the dots); names sort numerically.
 */
static void
verify_directory_limit_block(unsigned char *p, unsigned block)
{
	unsigned index = block - ROOT;
	unsigned first = index == 0 ? 0 : 40 + (index - 1) * 42;
	unsigned slots = index == 0 ? 40 : 42;
	size_t offset = index == 0 ? DOT_PAIR_BYTES : 0;
	char name[15];

	bzero(p, BLOCK);
	if (block >= VERIFY_DIRECTORY_FIRST_CHILD) {
		directory(p, block, ROOT, BLOCK);
		put32(p + DOT_BYTES + DR_SIZE, VERIFY_DIRECTORY_ROOT_BLOCKS * BLOCK);
		return;
	}
	if (index >= VERIFY_DIRECTORY_ROOT_BLOCKS)
		return;
	if (index == 0)
		directory(p, ROOT, ROOT, VERIFY_DIRECTORY_ROOT_BLOCKS * BLOCK);
	for (unsigned i = 0; i < slots && first + i < VERIFY_DIRECTORY_CHILDREN; i++) {
		CHECK(snprintf(name, sizeof(name), "D%013u", first + i) == 14);
		offset += record(p + offset, name, 14,
		    VERIFY_DIRECTORY_FIRST_CHILD + first + i, BLOCK, 2);
	}
}

/* The primary-only image has 16 files of 62500 one-block sections each:
 * exactly a million data claims, but only 16 prepared entries. Each run's
 * sections ascend without overlap; files share the same payload range.
 * Complete 48-byte records are synthesized on demand, never stored on disk.
 */
static void
verify_scale_block(unsigned char *p, unsigned block)
{
	unsigned first = block == 0 ? 0 : 40 + (block - 1) * 42;
	unsigned slots = block == 0 ? 40 : 42;
	size_t offset = block == 0 ? DOT_PAIR_BYTES : 0;
	char name[15];

	bzero(p, BLOCK);
	if (block == 0)
		directory(p, ROOT, ROOT, VERIFY_SCALE_BLOCKS * BLOCK);
	for (unsigned i = 0; i < slots && first + i < VERIFY_SCALE_CLAIMS; i++) {
		unsigned section = (first + i) % VERIFY_SCALE_SECTIONS;
		unsigned file = (first + i) / VERIFY_SCALE_SECTIONS;

		CHECK(snprintf(name, sizeof(name), "%08u.TXT;1", file) == 14);
		offset += record(p + offset, name, 14, VERIFY_SCALE_DATA + section,
		    BLOCK, section + 1 < VERIFY_SCALE_SECTIONS ? 0x80 : 0);
	}
}
