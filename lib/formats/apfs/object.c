/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdio.h>

#include "apfs.h"
#include "fsimg-endian.h"
#include "reader.h"

/*
 * Objects (specs/design-apfs-driver.md, "Objects"): the one function that
 * turns a physical, virtual or ephemeral reference into a validated object
 * in a caller's window, and the one read of a block by number.
 */

/* How an object's XID must relate to the XID it is read at. */
typedef enum {
	XID_AT_MOST,		/* not above it */
	XID_EQUAL		/* equal to it: an object-map key's XID */
} xid_rule_t;

static fsimg_status_t	object_check(const apfs_ref_t *, const unsigned char *,
			    size_t, xid_rule_t, uint64_t, apfs_obj_t *,
			    fsimg_error_t *);
static fsimg_status_t	object_load(apfs_container_t *, const apfs_ref_t *,
			    uint64_t, uint64_t, unsigned char *,
			    apfs_fletcher_t *, fsimg_error_t *);
static fsimg_status_t	object_message(fsimg_error_t *, fsimg_status_t,
			    const char *, const char *);

static fsimg_status_t
object_message(fsimg_error_t *error, fsimg_status_t status, const char *role,
    const char *what)
{
	char message[160];

	(void)snprintf(message, sizeof(message), "APFS %s %s", role, what);
	return (fsimg_error_set(error, status, 0, FSIMG_OP_READER, message));
}

fsimg_status_t
apfs_corrupt(fsimg_error_t *error, const char *role, const char *what)
{
	return (object_message(error, FSIMG_ECORRUPT, role, what));
}

fsimg_status_t
apfs_unsupported(fsimg_error_t *error, const char *role, const char *what)
{
	return (object_message(error, FSIMG_EUNSUPPORTED, role, what));
}

/*
 * Block 0 is read only by the block-0 validation, so every other block lies
 * in [1, N). N * B was computed checked when the geometry was accepted, so
 * the offset cannot overflow. A block below N and past the slice fails in
 * the read funnel as truncation.
 */
fsimg_status_t
apfs_block_read(apfs_container_t *container, uint64_t block,
    unsigned char *window, const char *role, fsimg_error_t *error)
{
	size_t transferred;

	if (block == 0 || block >= container->block_count)
		return (apfs_corrupt(error, role, "lies outside the container"));
	return (fsimg_reader_bytes(container->reader,
	    block * container->block_size, window, container->block_size,
	    &transferred, error));
}

/*
 * Read blocks [address, address + blocks) through window, feeding bytes 8
 * onward of the first block and every byte of the others to *sum, and
 * leave the first block in window. An object of several blocks needs its
 * first block twice, at the start of the checksum and in the window at the
 * end, so the first block is read again last; its partial sums must come
 * out as they did the first time, or the object changed while it was read.
 */
static fsimg_status_t
object_load(apfs_container_t *container, const apfs_ref_t *ref,
    uint64_t address, uint64_t blocks, unsigned char *window,
    apfs_fletcher_t *sum, fsimg_error_t *error)
{
	apfs_fletcher_t first, again;
	size_t block_size = container->block_size;
	fsimg_status_t status;

	if (blocks == 0 || address >= container->block_count ||
	    blocks > container->block_count - address)
		return (apfs_corrupt(error, ref->role,
		    "lies outside the container"));
	status = apfs_block_read(container, address, window, ref->role, error);
	if (status != FSIMG_OK)
		return (status);
	apfs_fletcher_init(sum);
	apfs_fletcher_update(sum, window + APFS_OBJ_CHECKSUM_BYTES,
	    block_size - APFS_OBJ_CHECKSUM_BYTES);
	if (blocks == 1)
		return (FSIMG_OK);

	first = *sum;
	for (uint64_t i = 1; i < blocks; i++) {
		status = fsimg_session_charge(&container->reader->session, 0, 0,
		    1, error);
		if (status == FSIMG_OK)
			status = apfs_block_read(container, address + i, window,
			    ref->role, error);
		if (status != FSIMG_OK)
			return (status);
		apfs_fletcher_update(sum, window, block_size);
	}
	status = apfs_block_read(container, address, window, ref->role, error);
	if (status != FSIMG_OK)
		return (status);
	apfs_fletcher_init(&again);
	apfs_fletcher_update(&again, window + APFS_OBJ_CHECKSUM_BYTES,
	    block_size - APFS_OBJ_CHECKSUM_BYTES);
	if (again.s1 != first.s1 || again.s2 != first.s2)
		return (apfs_corrupt(error, ref->role, "changed while read"));
	return (FSIMG_OK);
}

/* The header rules of specs/design-apfs.md, "Object header and checksum". */
static fsimg_status_t
object_check(const apfs_ref_t *ref, const unsigned char *window,
    size_t length, xid_rule_t rule, uint64_t xid, apfs_obj_t *header,
    fsimg_error_t *error)
{
	apfs_obj_t obj;

	if (!apfs_obj_decode(window, length, &obj))
		return (apfs_corrupt(error, ref->role, "is malformed"));
	if ((obj.flags & (APFS_OBJ_ENCRYPTED | APFS_OBJ_NONPERSISTENT)) != 0)
		return (apfs_unsupported(error, ref->role,
		    "is encrypted or nonpersistent"));
	if ((obj.flags & APFS_OBJ_NOHEADER) != 0 ||
	    (obj.flags & APFS_OBJ_STORAGE_MASK) != ref->storage ||
	    obj.kind != ref->kind || obj.subtype != ref->subtype)
		return (apfs_corrupt(error, ref->role,
		    "has the wrong kind, subtype or storage class"));
	if (obj.oid != ref->oid)
		return (apfs_corrupt(error, ref->role, "has the wrong OID"));
	if (obj.xid == 0 ||
	    (rule == XID_EQUAL ? obj.xid != xid : obj.xid > xid))
		return (apfs_corrupt(error, ref->role, "has the wrong XID"));
	if (header != NULL)
		*header = obj;
	return (FSIMG_OK);
}

fsimg_status_t
apfs_object_read(apfs_container_t *container, const apfs_ref_t *ref,
    unsigned char *window, apfs_obj_t *header, fsimg_error_t *error)
{
	apfs_fletcher_t sum;
	apfs_omap_key_t key;
	apfs_omap_val_t value;
	apfs_cpm_t mapping;
	uint64_t address, blocks, expected, xid;
	xid_rule_t rule = XID_AT_MOST;
	fsimg_status_t status;
	bool found;

	expected = ref->blocks == 0 ? 1 : ref->blocks;
	xid = ref->xid;
	switch (ref->storage) {
	case APFS_OBJ_PHYSICAL:
		address = ref->oid;
		blocks = expected;
		break;
	case APFS_OBJ_EPHEMERAL:
		status = apfs_ephemeral_find(container, ref->oid, &found,
		    &mapping, error);
		if (status != FSIMG_OK)
			return (status);
		if (!found)
			return (apfs_corrupt(error, ref->role,
			    "is not mapped by the checkpoint"));
		if ((mapping.flags & APFS_OBJ_STORAGE_MASK) !=
		    APFS_OBJ_EPHEMERAL || mapping.kind != ref->kind ||
		    mapping.subtype != ref->subtype)
			return (apfs_corrupt(error, ref->role,
			    "is mapped with another kind or subtype"));
		if ((mapping.flags & (APFS_OBJ_ENCRYPTED |
		    APFS_OBJ_NONPERSISTENT)) != 0)
			return (apfs_unsupported(error, ref->role,
			    "is mapped as encrypted or nonpersistent"));
		address = mapping.address;
		blocks = mapping.size / container->block_size;
		xid = container->xid;
		break;
	case APFS_OBJ_VIRTUAL:
		if (ref->map == NULL)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_READER, "virtual reference without a map"));
		status = apfs_map_lookup(ref->map, ref->oid, ref->xid, &found,
		    &key, &value, error);
		if (status != FSIMG_OK)
			return (status);
		if (!found)
			return (apfs_corrupt(error, ref->role,
			    "is missing from its object map"));
		address = value.address;
		blocks = value.size / container->block_size;
		rule = XID_EQUAL;
		xid = key.xid;
		break;
	default:
		return (fsimg_error_set(error, FSIMG_EINVAL, 0,
		    FSIMG_OP_READER, "invalid object storage class"));
	}

	if (blocks != expected)
		return (apfs_corrupt(error, ref->role, "has the wrong size"));
	status = fsimg_session_charge(&container->reader->session, 0, 0, 1,
	    error);
	if (status == FSIMG_OK)
		status = object_load(container, ref, address, blocks, window,
		    &sum, error);
	if (status != FSIMG_OK)
		return (status);
	if (apfs_fletcher_value(&sum) != fsimg_load_le64(window))
		return (apfs_corrupt(error, ref->role, "has a bad checksum"));
	return (object_check(ref, window, container->block_size, rule, xid,
	    header, error));
}
