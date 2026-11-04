/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <strings.h>

#include "hfsplus.h"

#define DECMPFS_UNITS	17u

/* "com.apple.decmpfs" as stored units, big-endian. */
static const unsigned char decmpfs_name[2 * DECMPFS_UNITS] = {
	0, 'c', 0, 'o', 0, 'm', 0, '.', 0, 'a', 0, 'p', 0, 'p', 0, 'l', 0, 'e',
	0, '.', 0, 'd', 0, 'e', 0, 'c', 0, 'm', 0, 'p', 0, 'f', 0, 's'
};

static fsimg_status_t scan_record(const hfsplus_akey_t *,
    const hfsplus_record_t *, bool, unsigned char *, hfsplus_attr_scan_t *,
    fsimg_error_t *);

/* Classify one record of the node ID. A compressed file's com.apple.decmpfs is
 * its content, not stored data; on any other object that name is an ordinary
 * attribute.
 */
static fsimg_status_t
scan_record(const hfsplus_akey_t *key, const hfsplus_record_t *record,
    bool compressed, unsigned char *payload, hfsplus_attr_scan_t *scan,
    fsimg_error_t *error)
{
	hfsplus_attr_t attr;
	size_t bytes;

	if (!hfsplus_attr_decode(record->data, record->data_bytes, &attr))
		return (hfsplus_corrupt(error,
		    "HFS Plus attribute record is malformed"));
	if (attr.type != HFSPLUS_ATTR_INLINE && attr.type != HFSPLUS_ATTR_FORK &&
	    attr.type != HFSPLUS_ATTR_EXTENTS)
		return (hfsplus_unsupported(error,
		    "HFS Plus attribute record has an unknown type"));
	if (!compressed || key->units != DECMPFS_UNITS ||
	    bcmp(key->name, decmpfs_name, sizeof(decmpfs_name)) != 0) {
		scan->stored = true;
		return (FSIMG_OK);
	}
	if (attr.type != HFSPLUS_ATTR_INLINE)
		return (hfsplus_unsupported(error,
		    "HFS Plus compression attribute is not stored inline"));
	if (!hfsplus_cmpf_decode(record->data + attr.data_offset, attr.data_size,
	    &scan->cmpf))
		return (hfsplus_corrupt(error,
		    "HFS Plus compression attribute's header is malformed"));
	scan->compression = true;
	bytes = attr.data_size - HFSPLUS_CMPF_BYTES;
	scan->payload_bytes = bytes;
	if (payload == NULL)
		return (FSIMG_OK);
	if (bytes > HFSPLUS_CHUNK_STORED_MAX)
		return (hfsplus_corrupt(error,
		    "HFS Plus compression attribute's payload is too long"));
	bcopy(record->data + attr.data_offset + HFSPLUS_CMPF_BYTES, payload,
	    bytes);
	return (FSIMG_OK);
}

/* One lower-bound search for (id, "", 0), which no attribute key equals
 * because a name has at least one unit, then a walk while fileID matches. An
 * absent tree holds nothing. The walk checks key order, which refuses a looping
 * sibling chain at its first repeated key; the cursor's cap on leaves, which the
 * slice bounds, ends one whatever its keys say.
 */
fsimg_status_t
hfsplus_attr_scan(fsimg_reader_t *reader, uint32_t id, bool compressed,
    unsigned char *payload, hfsplus_attr_scan_t *scan, fsimg_error_t *error)
{
	hfsplus_tree_t *tree = &hfsplus_state(reader)->attributes;
	unsigned char previous_name[2 * HFSPLUS_ATTR_NAME_UNITS_MAX];
	hfsplus_akey_t key, previous;
	hfsplus_cursor_t cursor;
	hfsplus_record_t record;
	hfsplus_key_t search;
	bool started;
	fsimg_status_t status;

	bzero(scan, sizeof(*scan));
	if (!tree->present)
		return (FSIMG_OK);
	hfsplus_cursor_init(&cursor, tree, tree->search_node);
	status = hfsplus_attribute_key_init(&search, id, 0, NULL, 0, error);
	if (status == FSIMG_OK)
		status = hfsplus_btree_search(&cursor, &search, error);
	started = false;
	while (status == FSIMG_OK && cursor.valid) {
		status = hfsplus_cursor_record(&cursor, &record, error);
		if (status != FSIMG_OK)
			break;
		if (!hfsplus_akey_decode(record.key, record.key_bytes, &key))
			return (hfsplus_corrupt(error,
			    "HFS Plus attribute key is malformed"));
		if (started && hfsplus_akey_compare(&previous, &key) >= 0)
			return (hfsplus_corrupt(error,
			    "HFS Plus attribute keys are out of order"));
		if (key.file_id != id)
			break;
		status = scan_record(&key, &record, compressed, payload, scan,
		    error);
		if (status != FSIMG_OK)
			break;
		bcopy(key.name, previous_name, 2 * (size_t)key.units);
		previous = key;
		previous.name = previous_name;
		started = true;
		status = hfsplus_cursor_next(&cursor, error);
	}
	return (status);
}
