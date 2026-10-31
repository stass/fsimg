/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <strings.h>

#include "fsimg-endian.h"
#include "qnx6.h"

/* Every disk integer is decoded in the volume's byte order, which the header
 * magic establishes; nothing here depends on the host's order.
 */
uint16_t
qnx6_u16(const unsigned char *p, bool big)
{
	if (big)
		return (fsimg_load_be16(p));
	return (fsimg_load_le16(p));
}

uint32_t
qnx6_u32(const unsigned char *p, bool big)
{
	if (big)
		return (fsimg_load_be32(p));
	return (fsimg_load_le32(p));
}

uint64_t
qnx6_u64(const unsigned char *p, bool big)
{
	if (big)
		return (fsimg_load_be64(p));
	return (fsimg_load_le64(p));
}

/* The header CRC: MSB first, polynomial 0x04c11db7, a zero initial register,
 * no reflection and no final XOR. "123456789" gives 0x89a1897f.
 */
uint32_t
qnx6_crc32(const unsigned char *bytes, size_t length)
{
	uint32_t crc;

	crc = 0;
	for (size_t i = 0; i < length; i++) {
		crc ^= (uint32_t)bytes[i] << 24;
		for (unsigned bit = 0; bit < 8; bit++)
			crc = (crc & UINT32_C(0x80000000)) != 0 ?
			    crc << 1 ^ UINT32_C(0x04c11db7) : crc << 1;
	}
	return (crc);
}

/* The long-name checksum: halve the previous value, add the next unsigned
 * byte, and toggle the high bit when the previous value was odd. A result
 * of zero is stored as 0xffffffff.
 */
uint32_t
qnx6_name_checksum(const unsigned char *name, size_t length)
{
	uint32_t previous, sum;

	sum = 0;
	for (size_t i = 0; i < length; i++) {
		previous = sum;
		sum = (previous >> 1) + name[i];
		if ((previous & 1) != 0)
			sum ^= UINT32_C(0x80000000);
	}
	return (sum == 0 ? QNX6_SENTINEL : sum);
}

/* Whether four bytes hold the magic, and in which byte order. */
bool
qnx6_magic_order(const unsigned char *p, bool *big)
{
	if (qnx6_u32(p, false) == QNX6_MAGIC) {
		*big = false;
		return (true);
	}
	if (qnx6_u32(p, true) == QNX6_MAGIC) {
		*big = true;
		return (true);
	}
	return (false);
}

/* An 80-byte header tree descriptor: length, sixteen references, depth, a
 * mode byte that is 1 in the classic profile and six zero extension bytes.
 * Returns whether the mode and extension bytes are classic.
 */
static bool
tree_decode(const unsigned char *p, bool big, qnx6_tree_t *tree)
{
	static const unsigned char zero[6];

	tree->length = qnx6_u64(p, big);
	for (unsigned i = 0; i < QNX6_ROOT_SLOTS; i++)
		tree->ref[i] = qnx6_u32(p + 8 + i * 4, big);
	tree->depth = p[72];
	return (p[73] == 1 && bcmp(p + 74, zero, sizeof(zero)) == 0);
}

/* Decode a 512-byte header without validating it; header.c validates. */
void
qnx6_header_decode(const unsigned char *p, bool big, qnx6_header_t *header)
{
	static const unsigned char zero[QNX6_HEADER_BYTES - 392];
	qnx6_tree_t spare;
	bool classic, spare_classic;

	bzero(header, sizeof(*header));
	header->serial = qnx6_u64(p + 8, big);
	header->format_time = qnx6_u32(p + 16, big);
	header->sync_time = qnx6_u32(p + 20, big);
	header->flags = qnx6_u32(p + 24, big);
	header->version = qnx6_u16(p + 28, big);
	header->reserve = qnx6_u16(p + 30, big);
	bcopy(p + 32, header->uuid, sizeof(header->uuid));
	header->block_size = qnx6_u32(p + 48, big);
	header->inode_count = qnx6_u32(p + 52, big);
	header->free_inodes = qnx6_u32(p + 56, big);
	header->block_count = qnx6_u32(p + 60, big);
	header->free_blocks = qnx6_u32(p + 64, big);
	header->groups = qnx6_u32(p + 68, big);
	classic = tree_decode(p + 72, big, &header->inodes);
	classic &= tree_decode(p + 152, big, &header->bitmap);
	classic &= tree_decode(p + 232, big, &header->names);
	header->descriptors_classic = classic;
	spare_classic = tree_decode(p + 312, big, &spare);
	header->spare_tree_empty = spare_classic && spare.length == 0 &&
	    spare.depth == 0;
	for (unsigned i = 0; i < QNX6_ROOT_SLOTS; i++)
		header->spare_tree_empty &= spare.ref[i] == QNX6_SENTINEL;
	header->extension_zero = bcmp(p + 392, zero, sizeof(zero)) == 0;
}

/* Decode a 128-byte inode without validating it; inode.c validates. */
void
qnx6_inode_decode(const unsigned char *p, bool big, qnx6_inode_t *inode)
{
	static const unsigned char zero[QNX6_INODE_BYTES - 102];

	bzero(inode, sizeof(*inode));
	inode->size = qnx6_u64(p, big);
	inode->uid = qnx6_u32(p + 8, big);
	inode->gid = qnx6_u32(p + 12, big);
	inode->created = qnx6_u32(p + 16, big);
	inode->modified = qnx6_u32(p + 20, big);
	inode->accessed = qnx6_u32(p + 24, big);
	inode->changed = qnx6_u32(p + 28, big);
	inode->mode = qnx6_u16(p + 32, big);
	inode->links = qnx6_u16(p + 34, big);
	inode->tree.length = inode->size;
	for (unsigned i = 0; i < QNX6_ROOT_SLOTS; i++)
		inode->tree.ref[i] = qnx6_u32(p + 36 + i * 4, big);
	inode->tree.depth = p[100];
	inode->status = p[101];
	inode->tail_zero = bcmp(p + 102, zero, sizeof(zero)) == 0;
}

/* Decode a 32-byte directory slot without validating it. */
void
qnx6_slot_decode(const unsigned char *p, bool big, qnx6_slot_t *slot)
{
	static const unsigned char zero[16];

	bzero(slot, sizeof(*slot));
	slot->inode = qnx6_u32(p, big);
	slot->length = p[4];
	if (slot->length == QNX6_LONG_NAME_MARKER) {
		slot->long_name = true;
		slot->long_index = qnx6_u32(p + 8, big);
		slot->checksum = qnx6_u32(p + 12, big);
		slot->long_padding_zero = p[5] == 0 && p[6] == 0 && p[7] == 0 &&
		    bcmp(p + 16, zero, sizeof(zero)) == 0;
		return;
	}
	if (slot->length <= QNX6_INLINE_NAME_MAX)
		bcopy(p + 5, slot->name, slot->length);
}

/* The encoders are the exact inverses of the decoders above. Each fills the
 * whole record, so an encoded record has no unspecified byte.
 */
void
qnx6_put16(unsigned char *p, uint16_t value, bool big)
{
	if (big)
		fsimg_store_be16(p, value);
	else
		fsimg_store_le16(p, value);
}

void
qnx6_put32(unsigned char *p, uint32_t value, bool big)
{
	if (big)
		fsimg_store_be32(p, value);
	else
		fsimg_store_le32(p, value);
}

void
qnx6_put64(unsigned char *p, uint64_t value, bool big)
{
	if (big)
		fsimg_store_be64(p, value);
	else
		fsimg_store_le64(p, value);
}

/* An 80-byte header descriptor with the classic mode byte 1. */
static void
tree_encode(const qnx6_tree_t *tree, bool big, unsigned char *p)
{
	bzero(p, 80);
	qnx6_put64(p, tree->length, big);
	for (unsigned i = 0; i < QNX6_ROOT_SLOTS; i++)
		qnx6_put32(p + 8 + i * 4, tree->ref[i], big);
	p[72] = tree->depth;
	p[73] = 1;
}

/* Encode a classic header: the three descriptors, the empty fourth tree, a
 * zero extension area and the CRC over bytes [8, 512).
 */
void
qnx6_header_encode(const qnx6_header_t *header, bool big, unsigned char *p)
{
	qnx6_tree_t spare;

	bzero(p, QNX6_HEADER_BYTES);
	qnx6_put32(p, QNX6_MAGIC, big);
	qnx6_put64(p + 8, header->serial, big);
	qnx6_put32(p + 16, header->format_time, big);
	qnx6_put32(p + 20, header->sync_time, big);
	qnx6_put32(p + 24, header->flags, big);
	qnx6_put16(p + 28, header->version, big);
	qnx6_put16(p + 30, header->reserve, big);
	bcopy(header->uuid, p + 32, sizeof(header->uuid));
	qnx6_put32(p + 48, header->block_size, big);
	qnx6_put32(p + 52, header->inode_count, big);
	qnx6_put32(p + 56, header->free_inodes, big);
	qnx6_put32(p + 60, header->block_count, big);
	qnx6_put32(p + 64, header->free_blocks, big);
	qnx6_put32(p + 68, header->groups, big);
	tree_encode(&header->inodes, big, p + 72);
	tree_encode(&header->bitmap, big, p + 152);
	tree_encode(&header->names, big, p + 232);
	spare = (qnx6_tree_t){ .length = 0 };
	for (unsigned i = 0; i < QNX6_ROOT_SLOTS; i++)
		spare.ref[i] = QNX6_SENTINEL;
	tree_encode(&spare, big, p + 312);
	qnx6_put32(p + 4, qnx6_crc32(p + 8, QNX6_HEADER_BYTES - 8), big);
}

/* Encode a 128-byte inode; its tree length is the inode size. */
void
qnx6_inode_encode(const qnx6_inode_t *inode, bool big, unsigned char *p)
{
	bzero(p, QNX6_INODE_BYTES);
	qnx6_put64(p, inode->size, big);
	qnx6_put32(p + 8, inode->uid, big);
	qnx6_put32(p + 12, inode->gid, big);
	qnx6_put32(p + 16, inode->created, big);
	qnx6_put32(p + 20, inode->modified, big);
	qnx6_put32(p + 24, inode->accessed, big);
	qnx6_put32(p + 28, inode->changed, big);
	qnx6_put16(p + 32, inode->mode, big);
	qnx6_put16(p + 34, inode->links, big);
	for (unsigned i = 0; i < QNX6_ROOT_SLOTS; i++)
		qnx6_put32(p + 36 + i * 4, inode->tree.ref[i], big);
	p[100] = inode->tree.depth;
	p[101] = inode->status;
}

/* Encode a live 32-byte slot: an inline name, zero padded, or a long-name
 * marker with its index and checksum and zero padding. The caller keeps an
 * inline length within 27 bytes; a longer one is clamped rather than
 * copied past the slot.
 */
void
qnx6_slot_encode(const qnx6_slot_t *slot, bool big, unsigned char *p)
{
	size_t length;

	bzero(p, QNX6_SLOT_BYTES);
	qnx6_put32(p, slot->inode, big);
	if (slot->long_name) {
		p[4] = QNX6_LONG_NAME_MARKER;
		qnx6_put32(p + 8, slot->long_index, big);
		qnx6_put32(p + 12, slot->checksum, big);
		return;
	}
	length = slot->length < QNX6_INLINE_NAME_MAX ? slot->length :
	    QNX6_INLINE_NAME_MAX;
	p[4] = (unsigned char)length;
	bcopy(slot->name, p + 5, length);
}
