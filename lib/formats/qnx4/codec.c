/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "qnx4.h"

/* Pure decoders of the four QNX4 records. Every disk integer is
 * little-endian. A decoder copies every field it names and reduces each
 * reserved range to a flag, so a decoded record holds nothing unexamined.
 * Slot classification (free, link) and the root scan's search for
 * format-owned names read the status byte and name field directly.
 */

static uint8_t name_length(const unsigned char *, size_t, bool *);

uint16_t
qnx4_u16(const unsigned char *p)
{
	return ((uint16_t)(p[0] | (unsigned)p[1] << 8));
}

uint32_t
qnx4_u32(const unsigned char *p)
{
	return ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	    (uint32_t)p[3] << 24);
}

bool
qnx4_bytes_zero(const unsigned char *p, size_t length)
{
	for (size_t i = 0; i < length; i++)
		if (p[i] != 0)
			return (false);
	return (true);
}

/* The bytes of a NUL-padded name field before its first NUL; *padding_zero
 * is false when a nonzero byte follows that NUL. A field without a NUL is
 * a name of the field's full width.
 */
static uint8_t
name_length(const unsigned char *field, size_t width, bool *padding_zero)
{
	const unsigned char *nul;
	size_t length;

	nul = memchr(field, '\0', width);
	length = nul == NULL ? width : (size_t)(nul - field);
	*padding_zero = qnx4_bytes_zero(field + length, width - length);
	return ((uint8_t)length);
}

/* A directory slot is free when its status has neither the used nor the
 * link bit; its other bytes are stale and never decoded.
 */
bool
qnx4_slot_free(const unsigned char *slot)
{
	return ((slot[0x3f] & (QNX4_STATUS_USED | QNX4_STATUS_LINK)) == 0);
}

bool
qnx4_slot_link(const unsigned char *slot)
{
	return ((slot[0x3f] & QNX4_STATUS_LINK) != 0);
}

void
qnx4_inode_decode(const unsigned char *raw, qnx4_inode_t *inode)
{
	bzero(inode, sizeof(*inode));
	bcopy(raw, inode->name, QNX4_DIRECT_NAME_MAX);
	inode->name_length = name_length(raw, QNX4_DIRECT_NAME_MAX,
	    &inode->name_padding_zero);
	inode->size = qnx4_u32(raw + 0x10);
	inode->first_extent.first = qnx4_u32(raw + 0x14);
	inode->first_extent.blocks = qnx4_u32(raw + 0x18);
	inode->chain = qnx4_u32(raw + 0x1c);
	inode->created = qnx4_u32(raw + 0x20);
	inode->modified = qnx4_u32(raw + 0x24);
	inode->accessed = qnx4_u32(raw + 0x28);
	inode->changed = qnx4_u32(raw + 0x2c);
	inode->extents = qnx4_u16(raw + 0x30);
	inode->mode = qnx4_u16(raw + 0x32);
	inode->uid = qnx4_u16(raw + 0x34);
	inode->gid = qnx4_u16(raw + 0x36);
	inode->links = qnx4_u16(raw + 0x38);
	inode->reserved_zero = qnx4_bytes_zero(raw + 0x3a, 4);
	inode->type = raw[0x3e];
	inode->status = raw[0x3f];
}

void
qnx4_link_decode(const unsigned char *raw, qnx4_link_t *link)
{
	bzero(link, sizeof(*link));
	bcopy(raw, link->name, QNX4_LINK_NAME_MAX);
	link->name_length = name_length(raw, QNX4_LINK_NAME_MAX,
	    &link->name_padding_zero);
	link->target_block = qnx4_u32(raw + 0x30);
	link->target_slot = raw[0x34];
	link->long_block = qnx4_u32(raw + 0x35);
	link->spare_zero = qnx4_bytes_zero(raw + 0x39, 6);
	link->status = raw[0x3f];
}

void
qnx4_xblock_decode(const unsigned char *raw, qnx4_xblock_t *xblock)
{
	static const unsigned char signature[8] = "IamXblk";

	bzero(xblock, sizeof(*xblock));
	xblock->next = qnx4_u32(raw);
	xblock->previous = qnx4_u32(raw + 4);
	xblock->used = raw[8];
	xblock->spare_zero = qnx4_bytes_zero(raw + 9, 3);
	xblock->covered = qnx4_u32(raw + 12);
	for (unsigned i = 0; i < QNX4_XBLOCK_EXTENTS; i++) {
		xblock->extent[i].first = qnx4_u32(raw + 16 + i * 8);
		xblock->extent[i].blocks = qnx4_u32(raw + 20 + i * 8);
	}
	xblock->signature_ok = bcmp(raw + 0x1f0, signature,
	    sizeof(signature)) == 0;
	xblock->first_copy.first = qnx4_u32(raw + 0x1f8);
	xblock->first_copy.blocks = qnx4_u32(raw + 0x1fc);
}

void
qnx4_longname_decode(const unsigned char *raw, qnx4_longname_t *record)
{
	const unsigned char *nul;

	bzero(record, sizeof(*record));
	record->block = qnx4_u32(raw);
	record->slot = raw[4];
	record->state = raw[5];
	bcopy(raw + 6, record->name, QNX4_NAME_MAX);
	/* 505 bytes do not fit name_length's eight bits. */
	nul = memchr(raw + 6, '\0', QNX4_NAME_MAX);
	record->name_length = nul == NULL ? QNX4_NAME_MAX :
	    (uint16_t)(nul - (raw + 6));
	record->name_padding_zero = qnx4_bytes_zero(raw + 6 +
	    record->name_length, QNX4_NAME_MAX - record->name_length);
	record->tail_zero = raw[0x1ff] == 0;
}
