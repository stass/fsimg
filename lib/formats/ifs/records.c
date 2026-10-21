/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "ifs.h"

/* Pure codecs of the header and of every record kind. They read and write
 * only the bytes they are given, perform no I/O and hold no state. Each
 * decoder check names its class, so a value ImageFS forbids is corruption
 * and a value no native image carries is unsupported. The encoders are
 * their inverses for the values the writer plans, and they fill every
 * padding byte with zero.
 */

static fsimg_status_t record_path(const unsigned char *, size_t, uint32_t,
    ifs_record_t *, fsimg_error_t *);
static fsimg_status_t record_prefix(const unsigned char *, ifs_byte_order_t,
    ifs_record_t *, fsimg_error_t *);
static fsimg_status_t record_link(const unsigned char *, uint32_t,
    ifs_byte_order_t, ifs_record_t *, fsimg_error_t *);

uint16_t
ifs_u16(const unsigned char *bytes, ifs_byte_order_t order)
{
	if (order == IFS_BIG_ENDIAN)
		return ((uint16_t)(bytes[0] << 8 | bytes[1]));
	return ((uint16_t)(bytes[0] | bytes[1] << 8));
}

uint32_t
ifs_u32(const unsigned char *bytes, ifs_byte_order_t order)
{
	if (order == IFS_BIG_ENDIAN)
		return ((uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
		    (uint32_t)bytes[2] << 8 | bytes[3]);
	return ((uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
	    (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24);
}

void
ifs_put16(unsigned char *bytes, uint16_t value, ifs_byte_order_t order)
{
	if (order == IFS_BIG_ENDIAN) {
		bytes[0] = (unsigned char)(value >> 8);
		bytes[1] = (unsigned char)value;
	} else {
		bytes[0] = (unsigned char)value;
		bytes[1] = (unsigned char)(value >> 8);
	}
}

void
ifs_put32(unsigned char *bytes, uint32_t value, ifs_byte_order_t order)
{
	for (unsigned i = 0; i < 4; i++)
		bytes[order == IFS_BIG_ENDIAN ? 3 - i : i] =
		    (unsigned char)(value >> (8 * i));
}

/* Where a record of each kind stores its path. */
uint32_t
ifs_path_offset(ifs_kind_t kind)
{
	switch (kind) {
	case IFS_KIND_DIRECTORY:
		return (IFS_REC_DIR_PATH);
	case IFS_KIND_FILE:
		return (IFS_REC_FILE_PATH);
	case IFS_KIND_SYMLINK:
		return (IFS_REC_LINK_PATH);
	case IFS_KIND_FIFO:
		return (IFS_REC_FIFO_PATH);
	}
	return (IFS_REC_PREFIX);
}

/* Place a record's path and, for a symbolic link, its target within the
 * record from its kind and path_length: the path at the kind's path
 * offset, the target right after the path's NUL. Encoding and the content
 * copy both take the positions from here.
 */
void
ifs_record_place(ifs_record_t *record)
{
	record->path_offset = ifs_path_offset(record->kind);
	record->target_offset = record->kind == IFS_KIND_SYMLINK ?
	    record->path_offset + record->path_length + 1 : 0;
}

/* A kind's minimum record length for a path of path_length bytes and, for
 * a symbolic link, a target of target_length bytes: the payload, the path
 * and its NUL, and the target and its NUL.
 */
uint64_t
ifs_record_minimum(ifs_kind_t kind, uint32_t path_length,
    uint32_t target_length)
{
	uint64_t minimum;

	minimum = (uint64_t)ifs_path_offset(kind) + path_length + 1;
	if (kind == IFS_KIND_SYMLINK)
		minimum += (uint64_t)target_length + 1;
	return (minimum);
}

/* Encode the fixed header: the signature, the flags byte of the byte order,
 * the three sizes, the references, chain_paddr, the mount flags and ten
 * zero spare words.
 */
void
ifs_header_encode(const ifs_header_t *header, unsigned char *bytes)
{
	ifs_byte_order_t order = header->order;

	bzero(bytes, IFS_HEADER_BYTES);
	bcopy(IFS_SIGNATURE, bytes, IFS_SIGNATURE_BYTES);
	bytes[IFS_HDR_FLAGS] = order == IFS_BIG_ENDIAN ? IFS_FLAGS_BIG :
	    IFS_FLAGS_LITTLE;
	ifs_put32(bytes + IFS_HDR_IMAGE_SIZE, header->image_size, order);
	ifs_put32(bytes + IFS_HDR_DIR_SIZE, header->hdr_dir_size, order);
	ifs_put32(bytes + IFS_HDR_DIR_OFFSET, header->dir_offset, order);
	for (unsigned i = 0; i < IFS_BOOT_SLOTS; i++)
		ifs_put32(bytes + IFS_HDR_BOOT_INO + 4 * i, header->boot_ino[i],
		    order);
	ifs_put32(bytes + IFS_HDR_SCRIPT_INO, header->script_ino, order);
	ifs_put32(bytes + IFS_HDR_CHAIN_PADDR, header->chain_paddr, order);
	ifs_put32(bytes + IFS_HDR_MOUNT_FLAGS, header->mount_flags, order);
}

/* Encode one record of record->length bytes, at least its kind's minimum,
 * whose positions ifs_record_place() set: the prefix with a zero
 * extended-attribute offset, the kind's payload, the path_length bytes of
 * path at path_offset and their NUL, and zero everywhere else. A symbolic
 * link's sym_offset is target_offset relative to the path and its sym_size
 * target_length; the target itself is content, which the caller copies to
 * target_offset, so its bytes and the NUL after them stay zero here.
 */
void
ifs_record_encode(const ifs_record_t *record, const unsigned char *path,
    ifs_byte_order_t order, unsigned char *bytes)
{
	static const uint32_t types[] = {
		[IFS_KIND_DIRECTORY] = IFS_MODE_DIRECTORY,
		[IFS_KIND_FILE] = IFS_MODE_FILE,
		[IFS_KIND_SYMLINK] = IFS_MODE_SYMLINK,
		[IFS_KIND_FIFO] = IFS_MODE_FIFO
	};

	bzero(bytes, record->length);
	ifs_put16(bytes + IFS_REC_LENGTH, (uint16_t)record->length, order);
	ifs_put32(bytes + IFS_REC_INODE, record->inode, order);
	ifs_put32(bytes + IFS_REC_MODE, types[record->kind] |
	    record->permissions, order);
	ifs_put32(bytes + IFS_REC_GID, record->gid, order);
	ifs_put32(bytes + IFS_REC_UID, record->uid, order);
	ifs_put32(bytes + IFS_REC_MTIME, record->mtime, order);
	switch (record->kind) {
	case IFS_KIND_FILE:
		ifs_put32(bytes + IFS_REC_FILE_OFFSET, record->data_offset,
		    order);
		ifs_put32(bytes + IFS_REC_FILE_SIZE, record->data_length, order);
		break;
	case IFS_KIND_SYMLINK:
		ifs_put16(bytes + IFS_REC_LINK_OFFSET,
		    (uint16_t)(record->target_offset - record->path_offset),
		    order);
		ifs_put16(bytes + IFS_REC_LINK_SIZE,
		    (uint16_t)record->target_length, order);
		break;
	case IFS_KIND_DIRECTORY:
	case IFS_KIND_FIFO:
		break;
	}
	bcopy(path, bytes + record->path_offset, record->path_length);
}

/* Decode the fixed 0x58-byte header of an image whose prefix holds the
 * identity signature. The flags byte comes first, since it selects the
 * byte order of every other field; a value other than the two native ones
 * is unsupported.
 */
fsimg_status_t
ifs_header_decode(const unsigned char *bytes, ifs_header_t *header,
    fsimg_error_t *error)
{
	ifs_byte_order_t order;

	bzero(header, sizeof(*header));
	header->flags = bytes[IFS_HDR_FLAGS];
	if (header->flags == IFS_FLAGS_LITTLE)
		order = IFS_LITTLE_ENDIAN;
	else if (header->flags == IFS_FLAGS_BIG)
		order = IFS_BIG_ENDIAN;
	else
		return (ifs_unsupported(error,
		    "ImageFS header flags are not supported"));
	header->order = order;
	header->image_size = ifs_u32(bytes + IFS_HDR_IMAGE_SIZE, order);
	header->hdr_dir_size = ifs_u32(bytes + IFS_HDR_DIR_SIZE, order);
	header->dir_offset = ifs_u32(bytes + IFS_HDR_DIR_OFFSET, order);
	for (unsigned i = 0; i < IFS_BOOT_SLOTS; i++)
		header->boot_ino[i] = ifs_u32(bytes + IFS_HDR_BOOT_INO + 4 * i,
		    order);
	header->script_ino = ifs_u32(bytes + IFS_HDR_SCRIPT_INO, order);
	header->chain_paddr = ifs_u32(bytes + IFS_HDR_CHAIN_PADDR, order);
	header->mount_flags = ifs_u32(bytes + IFS_HDR_MOUNT_FLAGS, order);
	header->spare_zero = true;
	for (unsigned i = 0; i < 4 * IFS_SPARE_WORDS; i++)
		if (bytes[IFS_HDR_SPARE + i] != 0)
			header->spare_zero = false;
	return (FSIMG_OK);
}

/* The relations every image's header satisfies against the volume that
 * holds it, computed in 64 bits so that no bound underflows:
 * 0x58 <= dir_offset, dir_offset + 4 <= hdr_dir_size (the table holds at
 * least its terminator), hdr_dir_size + 4 <= image_size,
 * image_size a multiple of 4, and image_size within the volume. A violation
 * contradicts the image's own layout.
 */
fsimg_status_t
ifs_header_bounds(const ifs_header_t *header, uint64_t volume_bytes,
    fsimg_error_t *error)
{
	if (header->dir_offset < IFS_HEADER_BYTES ||
	    (uint64_t)header->dir_offset + 4 > header->hdr_dir_size ||
	    (uint64_t)header->hdr_dir_size + 4 > header->image_size)
		return (ifs_corrupt(error,
		    "ImageFS header sizes are out of order"));
	if (header->image_size % 4 != 0)
		return (ifs_corrupt(error,
		    "ImageFS image size is not a multiple of 4"));
	if (header->image_size > volume_bytes)
		return (ifs_corrupt(error,
		    "ImageFS image size exceeds the volume"));
	return (FSIMG_OK);
}

/* The record length word at the start of a record. */
uint32_t
ifs_record_length(const unsigned char *bytes, ifs_byte_order_t order)
{
	return (ifs_u16(bytes + IFS_REC_LENGTH, order));
}

/* The index of an inode word's flag nibble among the four native ones, or
 * IFS_FLAG_NIBBLES for any other nibble.
 */
unsigned
ifs_flag_index(uint32_t inode)
{
	switch (inode >> IFS_INODE_FLAG_SHIFT) {
	case 0x0: return (0);
	case 0x8: return (1);
	case 0xc: return (2);
	case 0xe: return (3);
	}
	return (IFS_FLAG_NIBBLES);
}

/* Reference slot i: boot_ino[0] to boot_ino[3], then script_ino. Zero
 * means unused.
 */
uint32_t
ifs_reference(const ifs_header_t *header, unsigned i)
{
	return (i < IFS_BOOT_SLOTS ? header->boot_ino[i] : header->script_ino);
}

/* The prefix fields common to every kind, and the kind its mode's type
 * selects. Mode bits above the type and permissions, an unknown type, a
 * nonzero extended-attribute offset, a zero inode word and an unknown flag
 * nibble are all unobserved.
 */
static fsimg_status_t
record_prefix(const unsigned char *bytes, ifs_byte_order_t order,
    ifs_record_t *record, fsimg_error_t *error)
{
	uint32_t mode;

	mode = ifs_u32(bytes + IFS_REC_MODE, order);
	if (mode > IFS_MODE_MAX)
		return (ifs_unsupported(error,
		    "ImageFS record mode has unknown bits"));
	switch (mode & IFS_MODE_TYPE) {
	case IFS_MODE_DIRECTORY:
		record->kind = IFS_KIND_DIRECTORY;
		record->path_offset = IFS_REC_DIR_PATH;
		break;
	case IFS_MODE_FILE:
		record->kind = IFS_KIND_FILE;
		record->path_offset = IFS_REC_FILE_PATH;
		break;
	case IFS_MODE_SYMLINK:
		record->kind = IFS_KIND_SYMLINK;
		record->path_offset = IFS_REC_LINK_PATH;
		break;
	case IFS_MODE_FIFO:
		record->kind = IFS_KIND_FIFO;
		record->path_offset = IFS_REC_FIFO_PATH;
		break;
	default:
		return (ifs_unsupported(error,
		    "ImageFS record type is not supported"));
	}
	record->permissions = mode & FSIMG_MODE_BITS;
	record->inode = ifs_u32(bytes + IFS_REC_INODE, order);
	record->gid = ifs_u32(bytes + IFS_REC_GID, order);
	record->uid = ifs_u32(bytes + IFS_REC_UID, order);
	record->mtime = ifs_u32(bytes + IFS_REC_MTIME, order);
	if (ifs_u16(bytes + IFS_REC_EXTATTR, order) != 0)
		return (ifs_unsupported(error,
		    "ImageFS record has extended attributes"));
	if (record->inode == 0)
		return (ifs_unsupported(error,
		    "ImageFS record has a zero inode word"));
	if (ifs_flag_index(record->inode) == IFS_FLAG_NIBBLES)
		return (ifs_unsupported(error,
		    "ImageFS inode flags are not supported"));
	return (FSIMG_OK);
}

/* Find the path's NUL. available bytes of a length-byte record are in
 * bytes; a path is at most 1024 bytes, so at most 1025 bytes after the
 * path offset decide: a NUL there ends the path, no NUL before the
 * record's end is corruption, and a longer path is unsupported.
 */
static fsimg_status_t
record_path(const unsigned char *bytes, size_t available, uint32_t length,
    ifs_record_t *record, fsimg_error_t *error)
{
	const unsigned char *nul;
	size_t span, search;

	if (record->path_offset >= length)
		return (ifs_corrupt(error,
		    "ImageFS record is shorter than its kind"));
	span = length - record->path_offset;
	search = span < IFS_PATH_MAX + 1 ? span : IFS_PATH_MAX + 1;
	if (record->path_offset + search > available)
		return (ifs_corrupt(error, "ImageFS record bytes are missing"));
	nul = memchr(bytes + record->path_offset, '\0', search);
	if (nul == NULL && span <= IFS_PATH_MAX + 1)
		return (ifs_corrupt(error,
		    "ImageFS record path has no terminator"));
	if (nul == NULL)
		return (ifs_unsupported(error,
		    "ImageFS path is longer than 1024 bytes"));
	record->path_length = (uint32_t)(nul - (bytes + record->path_offset));
	return (FSIMG_OK);
}

/* A symbolic link's target follows its path's NUL: sym_offset is the path
 * length plus one, the target is sym_size bytes without a NUL, and a NUL
 * follows it. A record too short for the target it declares contradicts
 * itself; any other layout, and a target of 0 or over 1024 bytes, is
 * unobserved.
 */
static fsimg_status_t
record_link(const unsigned char *bytes, uint32_t length,
    ifs_byte_order_t order, ifs_record_t *record, fsimg_error_t *error)
{
	uint32_t sym_offset, sym_size;
	uint64_t minimum;

	sym_offset = ifs_u16(bytes + IFS_REC_LINK_OFFSET, order);
	sym_size = ifs_u16(bytes + IFS_REC_LINK_SIZE, order);
	minimum = (uint64_t)IFS_REC_LINK_PATH + record->path_length + 1 +
	    sym_size + 1;
	if (length < minimum)
		return (ifs_corrupt(error,
		    "ImageFS link record is shorter than its target"));
	if (sym_offset != record->path_length + 1)
		return (ifs_unsupported(error,
		    "ImageFS link target layout is not supported"));
	if (sym_size == 0 || sym_size > IFS_LINK_TARGET_MAX)
		return (ifs_unsupported(error,
		    "ImageFS link target length is not supported"));
	record->target_offset = record->path_offset + sym_offset;
	record->target_length = sym_size;
	/* The target and its NUL lie within the decoded bytes: the path is at
	 * most 1024 bytes and the target at most 1024.
	 */
	if (bytes[record->target_offset + sym_size] != '\0')
		return (ifs_unsupported(error,
		    "ImageFS link target is not terminated after its length"));
	if (memchr(bytes + record->target_offset, '\0', sym_size) != NULL)
		return (ifs_unsupported(error, "ImageFS link target holds a NUL"));
	return (FSIMG_OK);
}

/* Decode one record of length bytes, of which the first available are in
 * bytes: all of them, or IFS_RECORD_DECODE_BYTES when the record is longer.
 * Checks apply in the order the table rule, the prefix, the kinds and the
 * link layout give them; the bytes after the payload's final NUL are
 * padding and are never read.
 */
fsimg_status_t
ifs_record_decode(const unsigned char *bytes, size_t available,
    uint32_t length, ifs_byte_order_t order, ifs_record_t *record,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	bzero(record, sizeof(*record));
	if (length < IFS_REC_PREFIX)
		return (ifs_corrupt(error, "ImageFS record is too short"));
	if (available < IFS_REC_PREFIX || available > length ||
	    (available < length && available < IFS_RECORD_DECODE_BYTES))
		return (ifs_corrupt(error, "ImageFS record bytes are missing"));
	record->length = length;
	status = record_prefix(bytes, order, record, error);
	if (status != FSIMG_OK)
		return (status);
	status = record_path(bytes, available, length, record, error);
	if (status != FSIMG_OK)
		return (status);

	switch (record->kind) {
	case IFS_KIND_FILE:
		record->data_offset = ifs_u32(bytes + IFS_REC_FILE_OFFSET, order);
		record->data_length = ifs_u32(bytes + IFS_REC_FILE_SIZE, order);
		break;
	case IFS_KIND_SYMLINK:
		status = record_link(bytes, length, order, record, error);
		break;
	case IFS_KIND_FIFO:
		if (ifs_u32(bytes + IFS_REC_FIFO_WORDS, order) != 0 ||
		    ifs_u32(bytes + IFS_REC_FIFO_WORDS + 4, order) != 0)
			status = ifs_unsupported(error,
			    "ImageFS FIFO record has nonzero words");
		break;
	case IFS_KIND_DIRECTORY:
		break;
	}
	return (status);
}
