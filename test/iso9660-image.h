/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_TEST_ISO9660_IMAGE_H
#define FSIMG_TEST_ISO9660_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fsimg.h"

/* Independent ECMA-119 encoder. These are wire offsets, not driver types.
 * The prefix reserves room for 65 descriptors, path tables, directories
 * and payload. The callback supplies zeros beyond it for large files.
 */
#define BLOCK 2048u
#define BLOCKS 128u
#define ROOT 84u
#define DATA 104u
#define RR_AREA 110u
#define RR_DIRECTORY 85u
#define VERIFY_SCALE_CLAIMS 1000000u
#define VERIFY_SCALE_SECTIONS 62500u
#define VERIFY_SCALE_FILES (VERIFY_SCALE_CLAIMS / VERIFY_SCALE_SECTIONS)
#define VERIFY_SCALE_BLOCKS (1 + (VERIFY_SCALE_CLAIMS - 40 + 41) / 42)
#define VERIFY_SCALE_DATA 24000u
#define VERIFY_DIRECTORY_CHILDREN 65535u
#define VERIFY_DIRECTORY_FIRST_CHILD 2048u
#define VERIFY_DIRECTORY_ROOT_BLOCKS \
	(1 + (VERIFY_DIRECTORY_CHILDREN - 40 + 41) / 42)

enum {
	DOT_BYTES = 34, DOT_PAIR_BYTES = 2 * DOT_BYTES,
	SP_BYTES = 7, PX_BYTES = 36, CE_BYTES = 28, LINK_BYTES = 12,
	SUSP_LENGTH = 2, SUSP_VERSION = 3, SUSP_PAYLOAD = 4,
	ROOT_SP = DOT_BYTES, ROOT_ER = ROOT_SP + SP_BYTES,
	ROOT_DOT_BYTES = 60, ROOT_CE_DOT_BYTES = 70,
	CHILD_SUSP = 40, RELOCATED_CHILD = DOT_PAIR_BYTES + LINK_BYTES,
	XORRISO_PLACEHOLDER = 43
};
/* Producer placeholder extents: docs/build.md, ISO producer observations. */
#define MKISOFS_PLACEHOLDER 0xffffffeeu

enum {
	VD_LABEL = 40, VD_ESCAPE = 88,
	VD_SPACE = 80, VD_SET = 120, VD_SEQUENCE = 124, VD_BLOCK = 128,
	VD_PATH_SIZE = 132, VD_PATH_L = 140, VD_PATH_M = 148,
	VD_ROOT = 156, VD_DATE = 813, DATE17 = 17, VD_STRUCTURE = 881,
	DR_XATTR = 1, DR_EXTENT = 2, DR_SIZE = 10, DR_DATE = 18,
	DR_FLAGS = 25, DR_UNIT = 26, DR_GAP = 27, DR_SEQUENCE = 28,
	DR_NAME_SIZE = 32, DR_NAME = 33
};

typedef struct image {
	unsigned char	*bytes;
	uint64_t	 length;
	uint64_t	 stored;
	uint64_t	 high_read;
	uint64_t	 reads;
	uint64_t	 continuation_reads;
	uint64_t	 ce_read_index;
	uint64_t	 fail_read;
	/* Borrowed only by the second-walk read-fault fixture. */
	fsimg_verify_report_t *verify_report;
	bool		 scale;
	bool		 directory_scale;
	bool		 eof;
	bool		 cancel_read;
} image_t;

typedef struct fixture {
	fsimg_ctx_t	*ctx;
	fsimg_io_t	*io;
	fsimg_reader_t	*reader;
	fsimg_stats_t	 baseline;
} fixture_t;

#define BOOT_CATALOG 100u

/* SUSP wire encoders are independent of the reader. A child's stream lives
 * in block 110 by default, leaving room for boundary-sized NM and SL data.
 */
#define RR_CHILD 94u
typedef struct susp_area {
	unsigned char bytes[BLOCK];
	size_t length;
} susp_area_t;

/* A second hierarchy uses independent directory and path-table blocks.
 * Identifiers below are explicit UTF-16 code units from the format contract.
 */
#define JOLIET_ROOT 88u
#define JOLIET_CHILD 89u

/* Base ECMA-119 encoders. image_new() allocates zeroed bytes owned by the
 * caller, which frees them after fixture_free().
 */
void put16(unsigned char *p, uint16_t value);
void put32(unsigned char *p, uint32_t value);
size_t record(unsigned char *p, const void *name, size_t name_length,
    uint32_t extent, uint32_t length, unsigned flags);
void directory(unsigned char *p, uint32_t block, uint32_t parent, uint32_t length);
void descriptor(unsigned char *p, unsigned type, unsigned version);
image_t image_new(void);

/* Callback fixtures borrow the image and own the context, I/O and reader.
 * Cleanup checks resources against the context's initial baseline.
 */
fsimg_status_t image_size(void *arg, uint64_t *size, fsimg_error_t *error);
fsimg_status_t image_read(void *arg, uint64_t offset, void *buffer, size_t length,
    size_t *count, fsimg_error_t *error);
void fixture_new(fixture_t *f, image_t *image, const fsimg_options_t *options);
void fixture_free(fixture_t *f);
fsimg_status_t open_image(fixture_t *f, fsimg_iso_view_t view);
fsimg_status_t walk(fixture_t *f);
bool cancelled(void *argument);

/* El Torito boot records and catalogs. */
void boot_le16(unsigned char *p, uint16_t value);
void boot_le32(unsigned char *p, uint32_t value);
void boot_record(unsigned char *p, uint32_t catalog);
void boot_checksum(unsigned char *p);
void boot_validation(unsigned char *p, unsigned platform);
void boot_entry(unsigned char *p, unsigned media, uint16_t sectors,
    uint32_t image_block);
void boot_header(unsigned char *p, unsigned marker, unsigned platform,
    uint16_t count);
image_t boot_image(unsigned platform);
void boot_efi_section(image_t *image);

/* SUSP entries and Rock Ridge directory fixtures. */
unsigned char *susp_entry(susp_area_t *area, const char *signature, size_t length);
void susp_px(susp_area_t *area, unsigned mode, size_t length);
void susp_nm(susp_area_t *area, const void *name, size_t length, unsigned flags);
void susp_ce(susp_area_t *area, unsigned block, unsigned offset, unsigned length);
void susp_sl(susp_area_t *area, unsigned flags, unsigned component,
    const void *text, size_t length);
void susp_er(susp_area_t *area, const char *identifier, unsigned version);
image_t rr_image(const susp_area_t *area, unsigned flags);
unsigned char *rr_child_directory(image_t *image);
image_t rr_dot_image(const susp_area_t *dot, bool root, bool parent);

/* Joliet descriptors and identifiers. */
void joliet_add(image_t *image);
size_t joliet_record(unsigned char *p, const uint16_t *units, size_t count,
    uint32_t extent, uint32_t size, unsigned flags);

/* Independent path-table encoders for verifier fixtures. */
size_t verify_path(unsigned char *p, const void *name, size_t length,
    uint32_t extent, uint16_t parent, bool big);
void verify_table_root(image_t *image, unsigned vd, unsigned root,
    unsigned l, unsigned m);

#endif
