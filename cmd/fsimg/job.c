/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stddef.h>
#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "internal.h"
#include "layer.h"
#include "worker.h"

/* Each numeric field occupies eight little-endian bytes. Fixed strings
 * follow at byte 200: the format name in 16 bytes and the label in 49, each
 * NUL-terminated and zero after the terminator, then one byte saying
 * whether a label is present. Then come the write settings: the 16-byte
 * UUID, the byte order as one byte, and the inode capacity and reserve
 * percentage as eight little-endian bytes each, and the number of the
 * job's paths as eight bytes: a create job's root descriptors, or a read
 * job's members. The read-side selection follows: the
 * selector kind, partition index and GPT sector size as eight bytes each,
 * the three stack slot states as one byte each, the scheme and container
 * names in 16 bytes each (the format name is the filesystem slot's), and
 * the partition selector text in 112. The layer settings follow:
 * the GPT and MBR disk lengths and the MBR identifier as eight bytes each,
 * one byte of MBR flags (identifier set, partition active) and seven zero
 * bytes, the disk and partition GUIDs in 16 bytes each, and the GPT type in
 * 40, the GPT name in 112 and the MBR type in 16, NUL-terminated; then the
 * container settings: the DMG segment ID and VHD unique identifier in 16
 * bytes each, the DMG chunk, VHD alignment, simg block and DMG level as
 * eight bytes each, and one byte of container flags (DMG raw chunks, simg
 * CRC32) with seven zero bytes; then the spool limit and the gzip level in
 * eight bytes each, and the compression's name in 16 bytes,
 * NUL-terminated. The GPT sector size travels with the selection, since
 * reading uses it too. The mountpoint follows in a NUL-terminated field of
 * FSIMG_JOB_MOUNTPOINT_BYTES zero after its terminator, then one byte
 * saying whether it is present and seven zero bytes. The QNX boot image
 * settings follow: the machine and the five target addresses
 * (startup_vaddr, paddr_bias, image_paddr, ram_paddr, imagefs_paddr) as
 * eight bytes each, one byte whose bits say which of those six and the
 * virtual setting are set, one byte of values (the virtual bit, raw
 * storage), and six zero bytes. The compression settings follow, eight
 * bytes each: the xz preset and the xz check (fsimg_xz_check_t). The file
 * settings travel as descriptors,
 * one capability bit each, never in the record. Three eight-byte fields
 * close the record: image input kind, byte length and transfer quantum.
 * These enums name wire positions and sizes only and are independent of
 * native struct layout or padding.
 *
 * The job's image paths follow the record in SELECTION frames, as one
 * sequence of NUL-terminated paths whose total length the record states:
 * a read job sends its members, a create job one path per root
 * descriptor: for a manifest root's descriptors FSIMG_JOB_MANIFEST, then
 * FSIMG_JOB_CONTENTS when it has a content directory.
 */
typedef enum job_field {
	JOB_KIND, JOB_SANDBOX, JOB_FLAGS, JOB_MEMORY, JOB_SCRATCH, JOB_ENTRIES,
	JOB_OUTPUT, JOB_WORK, JOB_DEPTH, JOB_PATH, JOB_FDS, JOB_OFFSET, JOB_LENGTH,
	JOB_WRITE_FLAGS, JOB_VOLUME, JOB_FREE, JOB_EPOCH, JOB_SECTOR, JOB_CLUSTER,
	JOB_ROOT, JOB_SERIAL, JOB_CODEPAGE, JOB_SECONDS, JOB_CAPABILITIES,
	JOB_SELECTION
} job_field_t;

typedef enum job_string_field {
	JOB_FORMAT_OFFSET = 200,
	JOB_FORMAT_BYTES = 16,
	JOB_LABEL_OFFSET = 216,
	JOB_LABEL_BYTES = 49,
	JOB_LABEL_PRESENT = JOB_LABEL_OFFSET + JOB_LABEL_BYTES,
	JOB_UUID_OFFSET = 272,
	JOB_BYTE_ORDER = 288,
	JOB_INODES_OFFSET = 296,
	JOB_RESERVE_OFFSET = 304,
	JOB_PATHS_OFFSET = 312,
	JOB_SELECTOR_OFFSET = 320,
	JOB_PARTITION_INDEX_OFFSET = 328,
	JOB_GPT_SECTOR_OFFSET = 336,
	JOB_SLOT_STATES_OFFSET = 344,
	JOB_SCHEME_OFFSET = 352,
	JOB_CONTAINER_OFFSET = 368,
	JOB_PARTITION_TEXT_OFFSET = 384,
	JOB_PARTITION_TEXT_BYTES = 112,
	JOB_GPT_DISK_OFFSET = 496,
	JOB_MBR_DISK_OFFSET = 504,
	JOB_MBR_ID_OFFSET = 512,
	JOB_MBR_FLAGS = 520,
	JOB_DISK_GUID_OFFSET = 528,
	JOB_GUID_OFFSET = 544,
	JOB_GPT_TYPE_OFFSET = 560,
	JOB_GPT_TYPE_BYTES = 40,
	JOB_GPT_NAME_OFFSET = 600,
	JOB_GPT_NAME_BYTES = 112,
	JOB_MBR_TYPE_OFFSET = 712,
	JOB_MBR_TYPE_BYTES = 16,
	JOB_DMG_UUID_OFFSET = 728,
	JOB_VHD_UUID_OFFSET = 744,
	JOB_DMG_CHUNK_OFFSET = 760,
	JOB_VHD_ALIGN_OFFSET = 768,
	JOB_SIMG_BLOCK_OFFSET = 776,
	JOB_DMG_LEVEL_OFFSET = 784,
	JOB_CONTAINER_FLAGS = 792,
	JOB_SPOOL_OFFSET = 800,
	JOB_GZIP_LEVEL_OFFSET = 808,
	JOB_COMPRESSION_OFFSET = 816,
	JOB_COMPRESSION_BYTES = 16,
	JOB_MOUNTPOINT_OFFSET = 832,
	JOB_MOUNTPOINT_PRESENT = JOB_MOUNTPOINT_OFFSET +
	    FSIMG_JOB_MOUNTPOINT_BYTES,
	JOB_RESERVED_OFFSET = JOB_MOUNTPOINT_PRESENT + 1,
	JOB_IFSBOOT_OFFSET = JOB_MOUNTPOINT_PRESENT + 8,
	JOB_IFSBOOT_FIELDS = 6,
	JOB_IFSBOOT_SET = JOB_IFSBOOT_OFFSET + JOB_IFSBOOT_FIELDS * 8,
	JOB_IFSBOOT_VALUES = JOB_IFSBOOT_SET + 1,
	JOB_IFSBOOT_RESERVED = JOB_IFSBOOT_VALUES + 1,
	JOB_XZ_LEVEL_OFFSET = JOB_IFSBOOT_RESERVED + 6,
	JOB_XZ_CHECK_OFFSET = JOB_XZ_LEVEL_OFFSET + 8,
	JOB_INPUT_KIND = JOB_XZ_CHECK_OFFSET + 8,
	JOB_INPUT_LENGTH = JOB_INPUT_KIND + 8,
	JOB_INPUT_QUANTUM = JOB_INPUT_LENGTH + 8
} job_string_field_t;

/* Bits of JOB_IFSBOOT_SET: field n of the ifsboot settings is set, and the
 * virtual setting. Bits of JOB_IFSBOOT_VALUES.
 */
#define JOB_IFSBOOT_VIRTUAL_SET	(1u << JOB_IFSBOOT_FIELDS)
#define JOB_IFSBOOT_VIRTUAL	0x01u
#define JOB_IFSBOOT_RAW		0x02u

/* MBR flag bits at JOB_MBR_FLAGS. */
#define JOB_MBR_ID_SET	0x01u
#define JOB_MBR_ACTIVE	0x02u
/* Container flag bits at JOB_CONTAINER_FLAGS. */
#define JOB_DMG_RAW	0x01u
#define JOB_SIMG_CRC	0x02u
_Static_assert(JOB_FORMAT_OFFSET + JOB_FORMAT_BYTES == JOB_LABEL_OFFSET &&
    JOB_LABEL_PRESENT < JOB_UUID_OFFSET, "the string fields do not overlap");
_Static_assert(sizeof(((fsimg_stack_slot_t *)0)->name) == JOB_FORMAT_BYTES &&
    sizeof(((fsimg_worker_request_t *)0)->label_storage) == JOB_LABEL_BYTES &&
    sizeof(((fsimg_worker_request_t *)0)->partition_text) ==
    JOB_PARTITION_TEXT_BYTES &&
    sizeof(((fsimg_worker_request_t *)0)->compression) ==
    JOB_COMPRESSION_BYTES &&
    sizeof(((fsimg_write_options_t *)0)->compression) ==
    JOB_COMPRESSION_BYTES &&
    sizeof(((fsimg_image_options_t *)0)->compression) ==
    JOB_COMPRESSION_BYTES,
    "the request holds each string field");
_Static_assert(JOB_SCHEME_OFFSET + JOB_FORMAT_BYTES == JOB_CONTAINER_OFFSET &&
    JOB_CONTAINER_OFFSET + JOB_FORMAT_BYTES == JOB_PARTITION_TEXT_OFFSET &&
    JOB_PARTITION_TEXT_OFFSET + JOB_PARTITION_TEXT_BYTES ==
    JOB_GPT_DISK_OFFSET, "the selection fields do not overlap");
_Static_assert(JOB_DISK_GUID_OFFSET + 16 == JOB_GUID_OFFSET &&
    JOB_GUID_OFFSET + 16 == JOB_GPT_TYPE_OFFSET &&
    JOB_GPT_TYPE_OFFSET + JOB_GPT_TYPE_BYTES == JOB_GPT_NAME_OFFSET &&
    JOB_GPT_NAME_OFFSET + JOB_GPT_NAME_BYTES == JOB_MBR_TYPE_OFFSET &&
    JOB_MBR_TYPE_OFFSET + JOB_MBR_TYPE_BYTES == JOB_DMG_UUID_OFFSET &&
    JOB_DMG_UUID_OFFSET + 16 == JOB_VHD_UUID_OFFSET &&
    JOB_VHD_UUID_OFFSET + 16 == JOB_DMG_CHUNK_OFFSET &&
    JOB_DMG_CHUNK_OFFSET + 8 == JOB_VHD_ALIGN_OFFSET &&
    JOB_VHD_ALIGN_OFFSET + 8 == JOB_SIMG_BLOCK_OFFSET &&
    JOB_SIMG_BLOCK_OFFSET + 8 == JOB_DMG_LEVEL_OFFSET &&
    JOB_DMG_LEVEL_OFFSET + 8 == JOB_CONTAINER_FLAGS &&
    JOB_CONTAINER_FLAGS + 8 == JOB_SPOOL_OFFSET &&
    JOB_SPOOL_OFFSET + 8 == JOB_GZIP_LEVEL_OFFSET &&
    JOB_GZIP_LEVEL_OFFSET + 8 == JOB_COMPRESSION_OFFSET &&
    JOB_COMPRESSION_OFFSET + JOB_COMPRESSION_BYTES == JOB_MOUNTPOINT_OFFSET,
    "the layer fields do not overlap");
_Static_assert(JOB_RESERVED_OFFSET + 7 == JOB_IFSBOOT_OFFSET &&
    JOB_INPUT_QUANTUM + 8 == FSIMG_JOB_BYTES,
    "the prepared image description closes the record");
_Static_assert(sizeof(((fsimg_worker_request_t *)0)->mountpoint_storage) ==
    FSIMG_JOB_MOUNTPOINT_BYTES && FSIMG_JOB_MOUNTPOINT_BYTES >
    sizeof(((fsimg_volume_info_t *)0)->mountpoint),
    "the request holds the mountpoint, and the writer refuses a long one");
_Static_assert(sizeof(((fsimg_layer_options_t *)0)->gpt_type) ==
    JOB_GPT_TYPE_BYTES &&
    sizeof(((fsimg_layer_options_t *)0)->gpt_name) == JOB_GPT_NAME_BYTES &&
    sizeof(((fsimg_layer_options_t *)0)->mbr_type) == JOB_MBR_TYPE_BYTES &&
    sizeof(((fsimg_layer_options_t *)0)->gpt_guid) == 16 &&
    sizeof(((fsimg_layer_options_t *)0)->gpt_disk_guid) == 16 &&
    sizeof(((fsimg_layer_options_t *)0)->dmg_uuid) == 16 &&
    sizeof(((fsimg_layer_options_t *)0)->vhd_uuid) == 16,
    "the request holds each layer field");
/* encode_layers and decode_layers carry every member of the layout below,
 * which ends with gzip_level at 288, the ifsboot settings from 292, the
 * xz settings from 328 and reserved at 336. A new layer setting moves
 * reserved or grows the record and stops the build until they carry it
 * and this layout is updated.
 */
_Static_assert(offsetof(fsimg_layer_options_t, gzip_level) == 288 &&
    offsetof(fsimg_layer_options_t, ifsboot_compress) == 292 &&
    offsetof(fsimg_layer_options_t, ifsboot_imagefs_paddr_set) == 327 &&
    offsetof(fsimg_layer_options_t, xz_level) == 328 &&
    offsetof(fsimg_layer_options_t, xz_check) == 332 &&
    offsetof(fsimg_layer_options_t, reserved) == 336 &&
    sizeof(fsimg_layer_options_t) == 360,
    "every layer setting travels in the job record");

/* The ifsboot settings in JOB_IFSBOOT order: each value and its flag. */
#define JOB_IFSBOOT_FIELD(value, set)	\
	{ offsetof(fsimg_layer_options_t, value), \
	  offsetof(fsimg_layer_options_t, set) }
static const struct ifsboot_field {
	size_t		 value;
	size_t		 set;
} ifsboot_fields[JOB_IFSBOOT_FIELDS] = {
	JOB_IFSBOOT_FIELD(ifsboot_machine, ifsboot_machine_set),
	JOB_IFSBOOT_FIELD(ifsboot_startup_vaddr, ifsboot_startup_vaddr_set),
	JOB_IFSBOOT_FIELD(ifsboot_paddr_bias, ifsboot_paddr_bias_set),
	JOB_IFSBOOT_FIELD(ifsboot_image_paddr, ifsboot_image_paddr_set),
	JOB_IFSBOOT_FIELD(ifsboot_ram_paddr, ifsboot_ram_paddr_set),
	JOB_IFSBOOT_FIELD(ifsboot_imagefs_paddr, ifsboot_imagefs_paddr_set)
};
#undef JOB_IFSBOOT_FIELD

/* Where each stack slot's name travels: the filesystem's in the format
 * field, the others in their own.
 */
static const unsigned slot_name_offsets[FSIMG_STACK_SLOTS] = {
	[FSIMG_LAYER_FILESYSTEM] = JOB_FORMAT_OFFSET,
	[FSIMG_LAYER_SCHEME] = JOB_SCHEME_OFFSET,
	[FSIMG_LAYER_CONTAINER] = JOB_CONTAINER_OFFSET
};

static void put_field(unsigned char *, job_field_t, uint64_t);
static uint64_t get_field(const unsigned char *, job_field_t);
static int64_t get_signed_field(const unsigned char *, job_field_t);
static const int *capability_field(const fsimg_worker_request_t *,
    fsimg_capability_t);
static fsimg_status_t invalid_job(fsimg_error_t *);
static bool string_field_valid(const unsigned char *, size_t);
static void encode_layers(const fsimg_layer_options_t *, unsigned char *);
static bool layers_valid(const unsigned char *);
static void decode_layers(const unsigned char *, fsimg_layer_options_t *);

static void
put_field(unsigned char *raw, job_field_t field, uint64_t value)
{
	fsimg_store_le64(raw + (unsigned)field * 8, value);
}

static uint64_t
get_field(const unsigned char *raw, job_field_t field)
{
	return (fsimg_load_le64(raw + (unsigned)field * 8));
}

static int64_t
get_signed_field(const unsigned char *raw, job_field_t field)
{
	return (fsimg_wire_get_signed64(raw + (unsigned)field * 8));
}

/* A string field holds a terminator, and only zero bytes follow it, so a
 * record has one encoding.
 */
static bool
string_field_valid(const unsigned char *field, size_t bytes)
{
	const unsigned char *end;

	end = memchr(field, '\0', bytes);
	if (end == NULL)
		return (false);
	for (; end < field + bytes; end++) {
		if (*end != 0)
			return (false);
	}
	return (true);
}

/* The layer settings after gpt:sector, which travels with the selection. */
static void
encode_layers(const fsimg_layer_options_t *layers, unsigned char *raw)
{
	fsimg_store_le64(raw + JOB_GPT_DISK_OFFSET, layers->gpt_disk_bytes);
	fsimg_store_le64(raw + JOB_MBR_DISK_OFFSET, layers->mbr_disk_bytes);
	fsimg_store_le64(raw + JOB_MBR_ID_OFFSET, layers->mbr_id);
	raw[JOB_MBR_FLAGS] = (unsigned char)((layers->mbr_id_set ?
	    JOB_MBR_ID_SET : 0) | (layers->mbr_active ? JOB_MBR_ACTIVE : 0));
	bcopy(layers->gpt_disk_guid, raw + JOB_DISK_GUID_OFFSET, 16);
	bcopy(layers->gpt_guid, raw + JOB_GUID_OFFSET, 16);
	bcopy(layers->gpt_type, raw + JOB_GPT_TYPE_OFFSET,
	    strlen(layers->gpt_type));
	bcopy(layers->gpt_name, raw + JOB_GPT_NAME_OFFSET,
	    strlen(layers->gpt_name));
	bcopy(layers->mbr_type, raw + JOB_MBR_TYPE_OFFSET,
	    strlen(layers->mbr_type));
	bcopy(layers->dmg_uuid, raw + JOB_DMG_UUID_OFFSET, 16);
	bcopy(layers->vhd_uuid, raw + JOB_VHD_UUID_OFFSET, 16);
	fsimg_store_le64(raw + JOB_DMG_CHUNK_OFFSET, layers->dmg_chunk_bytes);
	fsimg_store_le64(raw + JOB_VHD_ALIGN_OFFSET, layers->vhd_align_bytes);
	fsimg_store_le64(raw + JOB_SIMG_BLOCK_OFFSET, layers->simg_block_bytes);
	fsimg_store_le64(raw + JOB_DMG_LEVEL_OFFSET, layers->dmg_level);
	raw[JOB_CONTAINER_FLAGS] = (unsigned char)((layers->dmg_compress ==
	    FSIMG_DMG_NONE ? JOB_DMG_RAW : 0) | (layers->simg_crc ?
	    JOB_SIMG_CRC : 0));
	fsimg_store_le64(raw + JOB_GZIP_LEVEL_OFFSET, layers->gzip_level);
	for (unsigned i = 0; i < JOB_IFSBOOT_FIELDS; i++) {
		const unsigned char *base = (const unsigned char *)layers;
		uint32_t value;
		bool set;

		bcopy(base + ifsboot_fields[i].value, &value, sizeof(value));
		bcopy(base + ifsboot_fields[i].set, &set, sizeof(set));
		fsimg_store_le64(raw + JOB_IFSBOOT_OFFSET + i * 8, value);
		if (set)
			raw[JOB_IFSBOOT_SET] |= (unsigned char)(1u << i);
	}
	if (layers->ifsboot_virtual_set)
		raw[JOB_IFSBOOT_SET] |= JOB_IFSBOOT_VIRTUAL_SET;
	raw[JOB_IFSBOOT_VALUES] = (unsigned char)((layers->ifsboot_virtual ?
	    JOB_IFSBOOT_VIRTUAL : 0) |
	    (layers->ifsboot_compress == FSIMG_IFSBOOT_NONE ? JOB_IFSBOOT_RAW :
	    0));
	fsimg_store_le64(raw + JOB_XZ_LEVEL_OFFSET, layers->xz_level);
	fsimg_store_le64(raw + JOB_XZ_CHECK_OFFSET, (uint64_t)layers->xz_check);
}

/* One encoding per settings value: known flags, a 32-bit identifier that
 * is zero unless set, 32-bit container quantities, zero padding and
 * terminated text. The library validates the values themselves.
 */
static bool
layers_valid(const unsigned char *raw)
{
	static const unsigned quantities[] = { JOB_DMG_CHUNK_OFFSET,
	    JOB_VHD_ALIGN_OFFSET, JOB_SIMG_BLOCK_OFFSET, JOB_DMG_LEVEL_OFFSET,
	    JOB_GZIP_LEVEL_OFFSET, JOB_XZ_LEVEL_OFFSET };
	uint64_t id;

	id = fsimg_load_le64(raw + JOB_MBR_ID_OFFSET);
	if (raw[JOB_MBR_FLAGS] > (JOB_MBR_ID_SET | JOB_MBR_ACTIVE) ||
	    id > UINT32_MAX ||
	    (id != 0 && (raw[JOB_MBR_FLAGS] & JOB_MBR_ID_SET) == 0))
		return (false);
	for (unsigned i = JOB_MBR_FLAGS + 1; i < JOB_DISK_GUID_OFFSET; i++) {
		if (raw[i] != 0)
			return (false);
	}
	for (unsigned i = 0; i < sizeof(quantities) / sizeof(quantities[0]); i++) {
		if (fsimg_load_le64(raw + quantities[i]) > UINT32_MAX)
			return (false);
	}
	if (raw[JOB_CONTAINER_FLAGS] > (JOB_DMG_RAW | JOB_SIMG_CRC))
		return (false);
	for (unsigned i = JOB_CONTAINER_FLAGS + 1; i < JOB_SPOOL_OFFSET; i++) {
		if (raw[i] != 0)
			return (false);
	}
	/* An ifsboot value is zero unless its setting is set. */
	if (raw[JOB_IFSBOOT_SET] >= JOB_IFSBOOT_VIRTUAL_SET << 1 ||
	    raw[JOB_IFSBOOT_VALUES] > (JOB_IFSBOOT_VIRTUAL | JOB_IFSBOOT_RAW) ||
	    ((raw[JOB_IFSBOOT_VALUES] & JOB_IFSBOOT_VIRTUAL) != 0 &&
	    (raw[JOB_IFSBOOT_SET] & JOB_IFSBOOT_VIRTUAL_SET) == 0))
		return (false);
	for (unsigned i = 0; i < JOB_IFSBOOT_FIELDS; i++) {
		id = fsimg_load_le64(raw + JOB_IFSBOOT_OFFSET + i * 8);
		if (id > UINT32_MAX ||
		    (id != 0 && (raw[JOB_IFSBOOT_SET] & (1u << i)) == 0))
			return (false);
	}
	for (unsigned i = JOB_IFSBOOT_RESERVED; i < JOB_XZ_LEVEL_OFFSET; i++) {
		if (raw[i] != 0)
			return (false);
	}
	if (fsimg_load_le64(raw + JOB_XZ_CHECK_OFFSET) > FSIMG_XZ_SHA256)
		return (false);
	return (string_field_valid(raw + JOB_GPT_TYPE_OFFSET, JOB_GPT_TYPE_BYTES) &&
	    string_field_valid(raw + JOB_GPT_NAME_OFFSET, JOB_GPT_NAME_BYTES) &&
	    string_field_valid(raw + JOB_MBR_TYPE_OFFSET, JOB_MBR_TYPE_BYTES));
}

static void
decode_layers(const unsigned char *raw, fsimg_layer_options_t *layers)
{
	layers->gpt_disk_bytes = fsimg_load_le64(raw + JOB_GPT_DISK_OFFSET);
	layers->mbr_disk_bytes = fsimg_load_le64(raw + JOB_MBR_DISK_OFFSET);
	layers->mbr_id = (uint32_t)fsimg_load_le64(raw + JOB_MBR_ID_OFFSET);
	layers->mbr_id_set = (raw[JOB_MBR_FLAGS] & JOB_MBR_ID_SET) != 0;
	layers->mbr_active = (raw[JOB_MBR_FLAGS] & JOB_MBR_ACTIVE) != 0;
	bcopy(raw + JOB_DISK_GUID_OFFSET, layers->gpt_disk_guid, 16);
	bcopy(raw + JOB_GUID_OFFSET, layers->gpt_guid, 16);
	bcopy(raw + JOB_GPT_TYPE_OFFSET, layers->gpt_type, JOB_GPT_TYPE_BYTES);
	bcopy(raw + JOB_GPT_NAME_OFFSET, layers->gpt_name, JOB_GPT_NAME_BYTES);
	bcopy(raw + JOB_MBR_TYPE_OFFSET, layers->mbr_type, JOB_MBR_TYPE_BYTES);
	bcopy(raw + JOB_DMG_UUID_OFFSET, layers->dmg_uuid, 16);
	bcopy(raw + JOB_VHD_UUID_OFFSET, layers->vhd_uuid, 16);
	layers->dmg_chunk_bytes =
	    (uint32_t)fsimg_load_le64(raw + JOB_DMG_CHUNK_OFFSET);
	layers->vhd_align_bytes =
	    (uint32_t)fsimg_load_le64(raw + JOB_VHD_ALIGN_OFFSET);
	layers->simg_block_bytes =
	    (uint32_t)fsimg_load_le64(raw + JOB_SIMG_BLOCK_OFFSET);
	layers->dmg_level = (uint32_t)fsimg_load_le64(raw + JOB_DMG_LEVEL_OFFSET);
	layers->dmg_compress = (raw[JOB_CONTAINER_FLAGS] & JOB_DMG_RAW) != 0 ?
	    FSIMG_DMG_NONE : FSIMG_DMG_ZLIB;
	layers->simg_crc = (raw[JOB_CONTAINER_FLAGS] & JOB_SIMG_CRC) != 0;
	layers->gzip_level =
	    (uint32_t)fsimg_load_le64(raw + JOB_GZIP_LEVEL_OFFSET);
	for (unsigned i = 0; i < JOB_IFSBOOT_FIELDS; i++) {
		unsigned char *base = (unsigned char *)layers;
		uint32_t value;
		bool set;

		value = (uint32_t)fsimg_load_le64(raw + JOB_IFSBOOT_OFFSET + i * 8);
		set = (raw[JOB_IFSBOOT_SET] & (1u << i)) != 0;
		bcopy(&value, base + ifsboot_fields[i].value, sizeof(value));
		bcopy(&set, base + ifsboot_fields[i].set, sizeof(set));
	}
	layers->ifsboot_virtual_set =
	    (raw[JOB_IFSBOOT_SET] & JOB_IFSBOOT_VIRTUAL_SET) != 0;
	layers->ifsboot_virtual =
	    (raw[JOB_IFSBOOT_VALUES] & JOB_IFSBOOT_VIRTUAL) != 0;
	layers->ifsboot_compress =
	    (raw[JOB_IFSBOOT_VALUES] & JOB_IFSBOOT_RAW) != 0 ?
	    FSIMG_IFSBOOT_NONE : FSIMG_IFSBOOT_LZO;
	layers->xz_level = (uint32_t)fsimg_load_le64(raw + JOB_XZ_LEVEL_OFFSET);
	layers->xz_check =
	    (fsimg_xz_check_t)fsimg_load_le64(raw + JOB_XZ_CHECK_OFFSET);
}

static fsimg_status_t
invalid_job(fsimg_error_t *error)
{
	return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_CONTEXT,
	    "invalid worker job description"));
}

void
fsimg_job_select_volume(fsimg_worker_request_t *request)
{
	static const char *const whole_volume[] = { "/" };

	request->members = whole_volume;
	request->member_count = 1;
}

void
fsimg_worker_request_init(fsimg_worker_request_t *request)
{
	bzero(request, sizeof(*request));
	fsimg_options_init(&request->limits);
	fsimg_write_options_init(&request->write);
	fsimg_layer_options_init(&request->layers);
	fsimg_job_select_volume(request);
	request->codepage = 437;
	for (fsimg_capability_t c = 0; c < FSIMG_CAPABILITY_COUNT; c++)
		*fsimg_job_capability_field(request, c) = -1;
}

const char *
fsimg_job_format(const fsimg_worker_request_t *request)
{
	return (request->stack.slots[FSIMG_LAYER_FILESYSTEM].name);
}

bool
fsimg_job_has_roots(fsimg_job_kind_t kind)
{
	return (kind == FSIMG_JOB_CREATE || kind == FSIMG_JOB_PLAN);
}

bool
fsimg_job_names_entries(fsimg_job_kind_t kind)
{
	return (fsimg_job_has_roots(kind) || kind == FSIMG_JOB_EXTRACT);
}

const char *
fsimg_job_compression(const fsimg_worker_request_t *request)
{
	return (fsimg_job_has_roots(request->kind) ? request->compression : "");
}

bool
fsimg_job_encoded(const fsimg_worker_request_t *request)
{
	/* The worker writes a staged file through an IO sink, which needs the
	 * raw intermediate exactly when the output is an encoding of the disk.
	 */
	return (fsimg_job_has_roots(request->kind) &&
	    fsimg_sink_needs_raw(FSIMG_SINK_IO, &request->stack,
	    request->compression));
}

bool
fsimg_job_needs_staging(const fsimg_worker_request_t *request)
{
	return (request->kind == FSIMG_JOB_CREATE &&
	    (request->flags & FSIMG_JOB_STDOUT) == 0);
}

bool
fsimg_job_needs_raw(const fsimg_worker_request_t *request)
{
	return (request->kind == FSIMG_JOB_CREATE &&
	    fsimg_sink_needs_raw((request->flags & FSIMG_JOB_STDOUT) != 0 ?
	    FSIMG_SINK_STREAM : FSIMG_SINK_IO, &request->stack,
	    request->compression));
}

/*
 * Where each capability's descriptor lives in a request. The switch has no
 * default, so -Wswitch refuses a named capability without its field. The
 * file settings are a run from SETTING, so settings past the first, which
 * have no name of their own, are found by their offset; COUNT is no
 * capability and has none.
 */
static const int *
capability_field(const fsimg_worker_request_t *request, fsimg_capability_t c)
{
	switch (c) {
	case FSIMG_CAPABILITY_INPUT:
		return (&request->input_fd);
	case FSIMG_CAPABILITY_OUTPUT:
		return (&request->output_fd);
	case FSIMG_CAPABILITY_SCRATCH:
		return (&request->scratch_fd);
	case FSIMG_CAPABILITY_RAW:
		return (&request->raw_fd);
	case FSIMG_CAPABILITY_SPOOL:
		return (&request->spool_fd);
	case FSIMG_CAPABILITY_MANIFEST:
		return (&request->manifest_fd);
	case FSIMG_CAPABILITY_SETTING:
		return (&request->setting_fds[0]);
	case FSIMG_CAPABILITY_COUNT:
		break;
	}
	if (c >= FSIMG_CAPABILITY_SETTING && c < FSIMG_CAPABILITY_COUNT)
		return (&request->setting_fds[c - FSIMG_CAPABILITY_SETTING]);
	return (NULL);
}

int
fsimg_job_capability_fd(const fsimg_worker_request_t *request,
    fsimg_capability_t c)
{
	return (*capability_field(request, c));
}

int *
fsimg_job_capability_field(fsimg_worker_request_t *request,
    fsimg_capability_t c)
{
	return ((int *)capability_field(request, c));
}

uint32_t
fsimg_job_path_count(const fsimg_worker_request_t *request)
{
	return (fsimg_job_has_roots(request->kind) ? request->root_count :
	    request->member_count);
}

const char *
fsimg_job_path(const fsimg_worker_request_t *request, uint32_t index)
{
	if (fsimg_job_has_roots(request->kind))
		return (request->root_paths[index]);
	/* A decoded read job has no members until its paths arrive. */
	return (request->members == NULL ? NULL : request->members[index]);
}

/* Whether path is a create job's manifest tag: protocol naming a root
 * descriptor's role, not an image path, so the path limit never applies.
 */
static bool
manifest_tag(const fsimg_worker_request_t *request, const char *path)
{
	return (fsimg_job_has_roots(request->kind) &&
	    (strcmp(path, FSIMG_JOB_MANIFEST) == 0 ||
	    strcmp(path, FSIMG_JOB_CONTENTS) == 0));
}

/* The bytes of the job's path sequence, each path with its terminator.
 * Every image path must be shorter than the path limit, and a manifest
 * tag is at most FSIMG_JOB_TAG_BYTES; since there are at most UINT32_MAX
 * paths of at most UINT32_MAX bytes, the total fits a uint64_t.
 */
fsimg_status_t
fsimg_job_paths_length(const fsimg_worker_request_t *request, uint64_t *total,
    fsimg_error_t *error)
{
	const char *path;
	uint32_t path_limit, count;
	size_t length;

	path_limit = request->limits.max_path_bytes == 0 ?
	    65536 : request->limits.max_path_bytes;
	count = fsimg_job_path_count(request);
	*total = 0;
	for (uint32_t i = 0; i < count; i++) {
		path = fsimg_job_path(request, i);
		if (path == NULL)
			return (invalid_job(error));
		if (manifest_tag(request, path)) {
			*total += strlen(path) + 1;
			continue;
		}
		length = strnlen(path, path_limit);
		if (length >= path_limit)
			return (fsimg_limit_error(error, FSIMG_RESOURCE_PATH,
			    path_limit, (uint64_t)length + 1));
		*total += (uint64_t)length + 1;
	}
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_job_encode(const fsimg_worker_request_t *request, unsigned char *raw,
    fsimg_error_t *error)
{
	uint64_t capabilities, paths_length;
	size_t label_length;
	const char *label, *mountpoint;
	fsimg_status_t status;

	if (fsimg_stack_validate(&request->stack, error) != FSIMG_OK ||
	    fsimg_input_validate(&request->input, error) != FSIMG_OK ||
	    memchr(request->partition_text, '\0', JOB_PARTITION_TEXT_BYTES) == NULL ||
	    memchr(request->compression, '\0', JOB_COMPRESSION_BYTES) == NULL ||
	    memchr(request->layers.gpt_type, '\0', JOB_GPT_TYPE_BYTES) == NULL ||
	    memchr(request->layers.gpt_name, '\0', JOB_GPT_NAME_BYTES) == NULL ||
	    memchr(request->layers.mbr_type, '\0', JOB_MBR_TYPE_BYTES) == NULL ||
	    fsimg_job_has_roots(request->kind) != (request->root_count != 0) ||
	    fsimg_job_path_count(request) == 0)
		return (invalid_job(error));
	label = request->write.label == NULL ? "" : request->write.label;
	label_length = strnlen(label, JOB_LABEL_BYTES);
	if (label_length == JOB_LABEL_BYTES)
		return (invalid_job(error));
	mountpoint = request->write.mountpoint;
	if (mountpoint != NULL && strnlen(mountpoint,
	    FSIMG_JOB_MOUNTPOINT_BYTES) == FSIMG_JOB_MOUNTPOINT_BYTES)
		return (invalid_job(error));
	status = fsimg_job_paths_length(request, &paths_length, error);
	if (status != FSIMG_OK)
		return (status);
	bzero(raw, FSIMG_JOB_BYTES);
	put_field(raw, JOB_KIND, request->kind);
	put_field(raw, JOB_SANDBOX, request->sandbox);
	put_field(raw, JOB_FLAGS, request->flags);
	put_field(raw, JOB_MEMORY, request->limits.memory_bytes);
	put_field(raw, JOB_SCRATCH, request->limits.scratch_bytes);
	fsimg_store_le64(raw + JOB_SPOOL_OFFSET, request->limits.spool_bytes);
	put_field(raw, JOB_ENTRIES, request->limits.max_entries);
	put_field(raw, JOB_OUTPUT, request->limits.max_output_bytes);
	put_field(raw, JOB_WORK, request->limits.max_work);
	put_field(raw, JOB_DEPTH, request->limits.max_depth);
	put_field(raw, JOB_PATH, request->limits.max_path_bytes);
	put_field(raw, JOB_FDS, request->limits.max_open_files);
	put_field(raw, JOB_OFFSET, request->offset);
	put_field(raw, JOB_LENGTH, request->length);
	put_field(raw, JOB_WRITE_FLAGS, request->write.flags);
	put_field(raw, JOB_VOLUME, request->write.volume_bytes);
	put_field(raw, JOB_FREE, request->write.min_free_bytes);
	put_field(raw, JOB_EPOCH, (uint64_t)request->write.epoch);
	put_field(raw, JOB_SECTOR, request->write.sector_bytes);
	put_field(raw, JOB_CLUSTER, request->write.allocation_unit_bytes);
	put_field(raw, JOB_ROOT, request->write.fat_root_entries);
	put_field(raw, JOB_SERIAL, request->write.volume_serial);
	put_field(raw, JOB_CODEPAGE, request->codepage);
	put_field(raw, JOB_SECONDS, request->seconds);
	capabilities = request->root_count != 0 ? FSIMG_JOB_CAP_ROOTS : 0;
	for (fsimg_capability_t c = 0; c < FSIMG_CAPABILITY_COUNT; c++) {
		if (fsimg_job_capability_fd(request, c) != -1)
			capabilities |= FSIMG_JOB_CAP(c);
	}
	/* A read job's image is a file or, for a stream, the input pipe. */
	if (request->input.kind == FSIMG_INPUT_STREAM)
		capabilities |= FSIMG_JOB_CAP(FSIMG_CAPABILITY_INPUT);
	if ((capabilities & FSIMG_JOB_CAP_SETTINGS) != 0 &&
	    !fsimg_job_has_roots(request->kind))
		return (invalid_job(error));
	put_field(raw, JOB_CAPABILITIES, capabilities);
	put_field(raw, JOB_SELECTION, paths_length);
	for (unsigned slot = 0; slot < FSIMG_STACK_SLOTS; slot++) {
		raw[JOB_SLOT_STATES_OFFSET + slot] =
		    (unsigned char)request->stack.slots[slot].state;
		bcopy(request->stack.slots[slot].name, raw + slot_name_offsets[slot],
		    strlen(request->stack.slots[slot].name));
	}
	fsimg_store_le64(raw + JOB_SELECTOR_OFFSET, request->selector);
	fsimg_store_le64(raw + JOB_PARTITION_INDEX_OFFSET, request->partition_index);
	fsimg_store_le64(raw + JOB_GPT_SECTOR_OFFSET,
	    request->layers.gpt_sector_bytes);
	encode_layers(&request->layers, raw);
	bcopy(request->partition_text, raw + JOB_PARTITION_TEXT_OFFSET,
	    strlen(request->partition_text));
	bcopy(request->compression, raw + JOB_COMPRESSION_OFFSET,
	    strnlen(request->compression, sizeof(request->compression) - 1));
	bcopy(label, raw + JOB_LABEL_OFFSET, label_length);
	raw[JOB_LABEL_PRESENT] = request->write.label != NULL ? 1 : 0;
	bcopy(request->write.volume_uuid, raw + JOB_UUID_OFFSET,
	    sizeof(request->write.volume_uuid));
	raw[JOB_BYTE_ORDER] = (unsigned char)request->write.byte_order;
	fsimg_store_le64(raw + JOB_INODES_OFFSET, request->write.inode_capacity);
	fsimg_store_le64(raw + JOB_RESERVE_OFFSET, request->write.reserve_percent);
	fsimg_store_le64(raw + JOB_PATHS_OFFSET, fsimg_job_path_count(request));
	if (mountpoint != NULL) {
		bcopy(mountpoint, raw + JOB_MOUNTPOINT_OFFSET, strlen(mountpoint));
		raw[JOB_MOUNTPOINT_PRESENT] = 1;
	}
	fsimg_store_le64(raw + JOB_INPUT_KIND, request->input.kind);
	fsimg_store_le64(raw + JOB_INPUT_LENGTH, request->input.length);
	fsimg_store_le64(raw + JOB_INPUT_QUANTUM, request->input.quantum);
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_job_decode(const unsigned char *raw, uint32_t length,
    fsimg_worker_request_t *request, uint64_t *selection_length,
    unsigned *capabilities, fsimg_error_t *error)
{
	const fsimg_layer_t *compression;
	uint64_t roots, count;
	uint32_t per_path_bytes;

	fsimg_worker_request_init(request);
	if (length != FSIMG_JOB_BYTES)
		return (invalid_job(error));
	if (get_field(raw, JOB_KIND) < FSIMG_JOB_INFO ||
	    get_field(raw, JOB_KIND) > FSIMG_JOB_PLAN ||
	    get_field(raw, JOB_SANDBOX) > FSIMG_SANDBOX_OFF ||
	    (get_field(raw, JOB_FLAGS) & ~(FSIMG_JOB_RECURSIVE | FSIMG_JOB_PAYLOAD |
	    FSIMG_JOB_VERBOSE | FSIMG_JOB_SKIP_UNSUPPORTED | FSIMG_JOB_STDOUT |
	    FSIMG_JOB_NAMES)) != 0 ||
	    ((get_field(raw, JOB_FLAGS) & FSIMG_JOB_STDOUT) != 0 &&
	    get_field(raw, JOB_KIND) != FSIMG_JOB_CREATE) ||
	    ((get_field(raw, JOB_FLAGS) & FSIMG_JOB_NAMES) != 0 &&
	    !fsimg_job_names_entries((fsimg_job_kind_t)get_field(raw, JOB_KIND))) ||
	    (get_field(raw, JOB_CAPABILITIES) & ~(uint64_t)FSIMG_JOB_CAP_ALL) != 0 ||
	    ((get_field(raw, JOB_CAPABILITIES) & FSIMG_JOB_CAP_SETTINGS) != 0 &&
	    !fsimg_job_has_roots((fsimg_job_kind_t)get_field(raw, JOB_KIND))) ||
	    !string_field_valid(raw + JOB_FORMAT_OFFSET, JOB_FORMAT_BYTES) ||
	    !string_field_valid(raw + JOB_LABEL_OFFSET, JOB_LABEL_BYTES) ||
	    raw[JOB_LABEL_PRESENT] > 1 || raw[JOB_BYTE_ORDER] > FSIMG_BYTE_ORDER_BIG ||
	    (raw[JOB_LABEL_PRESENT] == 0 && raw[JOB_LABEL_OFFSET] != 0))
		return (invalid_job(error));
	for (unsigned i = JOB_LABEL_PRESENT + 1; i < JOB_UUID_OFFSET; i++) {
		if (raw[i] != 0)
			return (invalid_job(error));
	}
	for (unsigned i = JOB_BYTE_ORDER + 1; i < JOB_INODES_OFFSET; i++) {
		if (raw[i] != 0)
			return (invalid_job(error));
	}
	for (unsigned i = JOB_SLOT_STATES_OFFSET + FSIMG_STACK_SLOTS;
	    i < JOB_SCHEME_OFFSET; i++) {
		if (raw[i] != 0)
			return (invalid_job(error));
	}
	if (!layers_valid(raw))
		return (invalid_job(error));
	if (fsimg_load_le64(raw + JOB_INPUT_KIND) > FSIMG_INPUT_STREAM)
		return (invalid_job(error));
	request->input.kind = (fsimg_input_kind_t)fsimg_load_le64(raw + JOB_INPUT_KIND);
	request->input.length = fsimg_load_le64(raw + JOB_INPUT_LENGTH);
	request->input.quantum = fsimg_load_le64(raw + JOB_INPUT_QUANTUM);
	if (fsimg_input_validate(&request->input, NULL) != FSIMG_OK ||
	    ((get_field(raw, JOB_CAPABILITIES) &
	    FSIMG_JOB_CAP(FSIMG_CAPABILITY_INPUT)) != 0) !=
	    (request->input.kind != FSIMG_INPUT_ABSENT) ||
	    (request->input.kind != FSIMG_INPUT_ABSENT &&
	    fsimg_job_has_roots((fsimg_job_kind_t)get_field(raw, JOB_KIND))))
		return (invalid_job(error));
	/* An absent mountpoint is an all-zero field. */
	if (raw[JOB_MOUNTPOINT_PRESENT] > 1 ||
	    !string_field_valid(raw + JOB_MOUNTPOINT_OFFSET,
	    FSIMG_JOB_MOUNTPOINT_BYTES) ||
	    (raw[JOB_MOUNTPOINT_PRESENT] == 0 &&
	    raw[JOB_MOUNTPOINT_OFFSET] != 0))
		return (invalid_job(error));
	for (unsigned i = JOB_RESERVED_OFFSET; i < JOB_IFSBOOT_OFFSET; i++) {
		if (raw[i] != 0)
			return (invalid_job(error));
	}
	if (!string_field_valid(raw + JOB_SCHEME_OFFSET, JOB_FORMAT_BYTES) ||
	    !string_field_valid(raw + JOB_CONTAINER_OFFSET, JOB_FORMAT_BYTES) ||
	    !string_field_valid(raw + JOB_PARTITION_TEXT_OFFSET,
	    JOB_PARTITION_TEXT_BYTES) ||
	    !string_field_valid(raw + JOB_COMPRESSION_OFFSET,
	    JOB_COMPRESSION_BYTES) ||
	    fsimg_load_le64(raw + JOB_SELECTOR_OFFSET) > FSIMG_SELECT_DISK ||
	    fsimg_load_le64(raw + JOB_PARTITION_INDEX_OFFSET) > UINT32_MAX ||
	    fsimg_load_le64(raw + JOB_GPT_SECTOR_OFFSET) > UINT32_MAX)
		return (invalid_job(error));
	for (unsigned slot = 0; slot < FSIMG_STACK_SLOTS; slot++) {
		if (raw[JOB_SLOT_STATES_OFFSET + slot] > FSIMG_SLOT_NAMED)
			return (invalid_job(error));
		request->stack.slots[slot].state =
		    (fsimg_slot_state_t)raw[JOB_SLOT_STATES_OFFSET + slot];
		bcopy(raw + slot_name_offsets[slot], request->stack.slots[slot].name,
		    JOB_FORMAT_BYTES);
	}
	if (fsimg_stack_validate(&request->stack, error) != FSIMG_OK)
		return (invalid_job(error));
	for (job_field_t field = JOB_DEPTH; field <= JOB_FDS; field++) {
		if (get_field(raw, field) > UINT32_MAX)
			return (invalid_job(error));
	}
	for (job_field_t field = JOB_SECTOR; field <= JOB_CODEPAGE; field++) {
		if (get_field(raw, field) > UINT32_MAX)
			return (invalid_job(error));
	}
	if (get_field(raw, JOB_WRITE_FLAGS) > UINT32_MAX ||
	    fsimg_load_le64(raw + JOB_INODES_OFFSET) > UINT32_MAX ||
	    fsimg_load_le64(raw + JOB_RESERVE_OFFSET) > UINT32_MAX)
		return (invalid_job(error));
	/* Every job has a path. Root descriptors exist exactly for create
	 * jobs, one per path and at most one per permitted descriptor, and
	 * arrive with the roots capability; a read job's paths are members.
	 */
	count = fsimg_load_le64(raw + JOB_PATHS_OFFSET);
	roots = fsimg_job_has_roots((fsimg_job_kind_t)get_field(raw, JOB_KIND)) ?
	    count : 0;
	if (count == 0 || count > UINT32_MAX ||
	    roots > get_field(raw, JOB_FDS) ||
	    ((get_field(raw, JOB_CAPABILITIES) & FSIMG_JOB_CAP_ROOTS) != 0) !=
	    (roots != 0))
		return (invalid_job(error));
	request->kind = (fsimg_job_kind_t)get_field(raw, JOB_KIND);
	request->sandbox = (fsimg_sandbox_policy_t)get_field(raw, JOB_SANDBOX);
	request->flags = (unsigned)get_field(raw, JOB_FLAGS);
	request->limits.memory_bytes = get_field(raw, JOB_MEMORY);
	request->limits.scratch_bytes = get_field(raw, JOB_SCRATCH);
	request->limits.max_entries = get_field(raw, JOB_ENTRIES);
	request->limits.max_output_bytes = get_field(raw, JOB_OUTPUT);
	request->limits.max_work = get_field(raw, JOB_WORK);
	request->limits.max_depth = (uint32_t)get_field(raw, JOB_DEPTH);
	request->limits.max_path_bytes = (uint32_t)get_field(raw, JOB_PATH);
	request->limits.max_open_files = (uint32_t)get_field(raw, JOB_FDS);
	request->limits.spool_bytes = fsimg_load_le64(raw + JOB_SPOOL_OFFSET);
	request->offset = get_field(raw, JOB_OFFSET);
	request->length = get_field(raw, JOB_LENGTH);
	request->seconds = get_field(raw, JOB_SECONDS);
	request->write.flags = (uint32_t)get_field(raw, JOB_WRITE_FLAGS);
	request->write.volume_bytes = get_field(raw, JOB_VOLUME);
	request->write.min_free_bytes = get_field(raw, JOB_FREE);
	request->write.epoch = get_signed_field(raw, JOB_EPOCH);
	request->write.sector_bytes = (uint32_t)get_field(raw, JOB_SECTOR);
	request->write.allocation_unit_bytes = (uint32_t)get_field(raw, JOB_CLUSTER);
	request->write.fat_root_entries = (uint32_t)get_field(raw, JOB_ROOT);
	request->write.volume_serial = (uint32_t)get_field(raw, JOB_SERIAL);
	request->codepage = (uint32_t)get_field(raw, JOB_CODEPAGE);
	request->selector = (fsimg_selector_kind_t)fsimg_load_le64(raw +
	    JOB_SELECTOR_OFFSET);
	request->partition_index = (uint32_t)fsimg_load_le64(raw +
	    JOB_PARTITION_INDEX_OFFSET);
	request->layers.gpt_sector_bytes =
	    (uint32_t)fsimg_load_le64(raw + JOB_GPT_SECTOR_OFFSET);
	decode_layers(raw, &request->layers);
	bcopy(raw + JOB_PARTITION_TEXT_OFFSET, request->partition_text,
	    JOB_PARTITION_TEXT_BYTES);
	bcopy(raw + JOB_COMPRESSION_OFFSET, request->compression,
	    JOB_COMPRESSION_BYTES);
	/* A create writes its compression and a read decodes it. */
	if (fsimg_compression_named(request->compression,
	    sizeof(request->compression), fsimg_job_has_roots(request->kind),
	    &compression, error) != FSIMG_OK)
		return (invalid_job(error));
	bcopy(raw + JOB_LABEL_OFFSET, request->label_storage, JOB_LABEL_BYTES);
	request->write.label = raw[JOB_LABEL_PRESENT] == 0 ? NULL :
	    request->label_storage;
	bcopy(raw + JOB_UUID_OFFSET, request->write.volume_uuid,
	    sizeof(request->write.volume_uuid));
	request->write.byte_order = (fsimg_byte_order_t)raw[JOB_BYTE_ORDER];
	request->write.inode_capacity =
	    (uint32_t)fsimg_load_le64(raw + JOB_INODES_OFFSET);
	request->write.reserve_percent =
	    (uint32_t)fsimg_load_le64(raw + JOB_RESERVE_OFFSET);
	if (raw[JOB_MOUNTPOINT_PRESENT] != 0) {
		bcopy(raw + JOB_MOUNTPOINT_OFFSET, request->mountpoint_storage,
		    FSIMG_JOB_MOUNTPOINT_BYTES);
		request->write.mountpoint = request->mountpoint_storage;
	}
	*selection_length = get_field(raw, JOB_SELECTION);
	*capabilities = (unsigned)get_field(raw, JOB_CAPABILITIES);
	/* Each path is at least "/" and its terminator. An image path is
	 * shorter than the path limit, and a create job's manifest tag has its
	 * own fixed length, whatever the limit.
	 */
	per_path_bytes = request->limits.max_path_bytes == 0 ? 65536 :
	    request->limits.max_path_bytes;
	if (roots != 0 && per_path_bytes < FSIMG_JOB_TAG_BYTES)
		per_path_bytes = FSIMG_JOB_TAG_BYTES;
	if (*selection_length < 2 * count ||
	    *selection_length > count * per_path_bytes)
		return (invalid_job(error));
	/* A read job's members arrive in the path sequence, and the receiver
	 * points members at them. Last, so a refused record leaves no roots
	 * for cleanup to close.
	 */
	if (roots == 0) {
		request->members = NULL;
		request->member_count = (uint32_t)count;
	} else {
		request->root_count = (uint32_t)roots;
	}
	return (FSIMG_OK);
}
