/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "fsimg-endian.h"
#include "write-erofs.h"

/*
 * Directory packing, shared by planning, which needs each directory's size,
 * and emission, which needs its bytes. Both run the same walk, so the size
 * planning reserves is the size emission fills.
 *
 * A directory's entries are its children plus "." and "..", in bytewise
 * name order with a shorter equal prefix first: the order the reader
 * requires across the whole directory. The catalog's child stream already
 * holds each directory's children in that order, grouped by increasing
 * parent ID, so one cursor serves every directory when they are walked in
 * increasing ID order. The dot entries are merged into their places: a
 * name such as "-a" sorts before ".", and ".-" between "." and "..".
 *
 * Blocks fill in order: an entry joins the open block while the block's
 * 12-byte entries and its names fit block_bytes, else it opens the next
 * block. The reader takes a block's entry count from its first name
 * offset and each name's end from the next entry's offset, the last name
 * ending at the first NUL or the block's valid end, so the zero bytes
 * after the last name end it. The directory's size covers its full blocks
 * and the used bytes of its last one.
 */

/* An entry's fixed part: NID, name offset, file type and a reserved byte. */
_Static_assert((1u << EROFS_BLKSZBITS_MAX) <= UINT16_MAX,
    "a name offset inside a block fits the entry's 16-bit field");
#define DIRENT_NID		0
#define DIRENT_NAME_OFFSET	8
#define DIRENT_FILE_TYPE	10

static int name_order(const char *, size_t, const char *, size_t);
static fsimg_status_t refuse_name(fsimg_writer_t *, uint64_t,
    fsimg_error_t *);
static fsimg_status_t place_entry(erofs_dir_pack_t *,
    const erofs_dir_builder_t *, uint64_t, const char *, size_t,
    fsimg_error_t *);
static fsimg_status_t close_block(erofs_dir_pack_t *,
    const erofs_dir_builder_t *, fsimg_error_t *);

void
erofs_dir_pack_init(erofs_dir_pack_t *pack, uint32_t block_bytes,
    unsigned char *buffer)
{
	bzero(pack, sizeof(*pack));
	pack->block_bytes = block_bytes;
	if (buffer != NULL) {
		pack->block = buffer;
		pack->names = buffer + block_bytes;
	}
}

/* Whether the open block can take one more entry with this name. */
bool
erofs_dir_pack_fits(const erofs_dir_pack_t *pack, size_t name_length)
{
	uint64_t used;

	used = (uint64_t)EROFS_DIRENT_BYTES * (pack->entries + 1) +
	    pack->name_bytes + name_length;
	return (used <= pack->block_bytes);
}

/*
 * Append an entry to the open block, which erofs_dir_pack_fits() said can
 * take it. While building bytes, the entry's name offset is relative to
 * the first name until the block closes.
 */
void
erofs_dir_pack_add(erofs_dir_pack_t *pack, const erofs_dir_entry_t *entry)
{
	unsigned char *dirent;

	if (pack->block != NULL) {
		dirent = pack->block + (size_t)EROFS_DIRENT_BYTES * pack->entries;
		bzero(dirent, EROFS_DIRENT_BYTES);
		fsimg_store_le64(dirent + DIRENT_NID, entry->nid);
		fsimg_store_le16(dirent + DIRENT_NAME_OFFSET,
		    (uint16_t)pack->name_bytes);
		dirent[DIRENT_FILE_TYPE] = entry->file_type;
		bcopy(entry->name, pack->names + pack->name_bytes,
		    entry->name_length);
	}
	pack->entries++;
	pack->name_bytes += (uint32_t)entry->name_length;
}

/*
 * Finish the open block and return its valid length, the bytes its entries
 * and names take. While building bytes, the names move behind the entry
 * array, each name offset gains the array's length, and the rest of the
 * block is zeroed. The next entry opens a new block.
 */
size_t
erofs_dir_pack_close(erofs_dir_pack_t *pack)
{
	unsigned char *dirent;
	uint32_t names_start, used;

	names_start = EROFS_DIRENT_BYTES * pack->entries;
	used = names_start + pack->name_bytes;
	if (pack->block != NULL) {
		for (uint32_t i = 0; i < pack->entries; i++) {
			dirent = pack->block + (size_t)EROFS_DIRENT_BYTES * i;
			fsimg_store_le16(dirent + DIRENT_NAME_OFFSET,
			    (uint16_t)(names_start +
			    fsimg_load_le16(dirent + DIRENT_NAME_OFFSET)));
		}
		bcopy(pack->names, pack->block + names_start, pack->name_bytes);
		bzero(pack->block + used, pack->block_bytes - used);
	}
	pack->closed++;
	pack->entries = 0;
	pack->name_bytes = 0;
	return (used);
}

/* The directory's size so far: its closed blocks and the open block's use. */
uint64_t
erofs_dir_pack_size(const erofs_dir_pack_t *pack)
{
	return (pack->closed * pack->block_bytes +
	    (uint64_t)EROFS_DIRENT_BYTES * pack->entries + pack->name_bytes);
}

/*
 * Pack directory_id, whose parent is parent_id (the root is its own
 * parent), reading its children from the catalog's child stream at
 * *cursor, which is left at the next directory's first child. The
 * directory's size is stored in *size. With builder NULL this only
 * measures; otherwise every entry is resolved and every block delivered
 * through builder. The child stream shares the writer's record buffer, so
 * a catalog snapshot taken before the walk does not survive it.
 */
fsimg_status_t
erofs_dir_walk(erofs_plan_t *plan, uint64_t *cursor, uint64_t directory_id,
    uint64_t parent_id, const erofs_dir_builder_t *builder, uint64_t *size,
    fsimg_error_t *error)
{
	const struct {
		const char	*name;
		size_t		 length;
		uint64_t	 id;
	} dots[] = {
		{ ".", 1, directory_id },
		{ "..", 2, parent_id }
	};
	fsimg_writer_t *writer = plan->common.writer;
	fsimg_catalog_child_t child;
	erofs_dir_pack_t pack;
	char name[EROFS_NAME_MAX];
	uint64_t before;
	size_t length, next_dot;
	fsimg_status_t status;

	erofs_dir_pack_init(&pack, plan->block_bytes,
	    builder != NULL ? builder->pack_buffer : NULL);
	next_dot = 0;
	for (;;) {
		before = *cursor;
		status = fsimg_stream_next(writer->children, cursor,
		    writer->record, writer->record_capacity, &length, error);
		if (status == FSIMG_DONE)
			break;
		if (status != FSIMG_OK)
			return (status);
		status = fsimg_catalog_decode_child(writer, length, &child,
		    error);
		if (status != FSIMG_OK)
			return (status);
		/* The next directory's first child: leave it for that walk. */
		if (child.parent_id > directory_id) {
			*cursor = before;
			break;
		}
		if (child.parent_id < directory_id)
			return (fsimg_error_set(error, FSIMG_ESTATE, 0,
			    FSIMG_OP_WRITER,
			    "EROFS directory walk lost the catalog's child order"));
		if (child.name_length > EROFS_NAME_MAX)
			return (refuse_name(writer, child.id, error));
		/*
		 * The child's name lives in the writer's record buffer, which
		 * a builder's resolve could reuse; the walk keeps its own copy.
		 */
		bcopy(child.name, name, child.name_length);

		/* Dot entries that sort before this child come first. */
		while (next_dot < 2 && name_order(dots[next_dot].name,
		    dots[next_dot].length, name, child.name_length) < 0) {
			status = place_entry(&pack, builder, dots[next_dot].id,
			    dots[next_dot].name, dots[next_dot].length, error);
			if (status != FSIMG_OK)
				return (status);
			next_dot++;
		}
		status = place_entry(&pack, builder, child.id, name,
		    child.name_length, error);
		if (status != FSIMG_OK)
			return (status);
	}
	for (; next_dot < 2; next_dot++) {
		status = place_entry(&pack, builder, dots[next_dot].id,
		    dots[next_dot].name, dots[next_dot].length, error);
		if (status != FSIMG_OK)
			return (status);
	}

	/*
	 * The last entry placed is always in the open block, so the open block
	 * is the directory's last. Its size is taken before closing it, which
	 * resets the open block's counts.
	 */
	*size = erofs_dir_pack_size(&pack);
	return (close_block(&pack, builder, error));
}

/*
 * Compare two names by unsigned bytes, a shorter equal prefix first: the
 * catalog's sibling order and the reader's directory order.
 */
static int
name_order(const char *name, size_t length, const char *other,
    size_t other_length)
{
	int order;

	order = memcmp(name, other, length < other_length ? length :
	    other_length);
	if (order != 0)
		return (order);
	return ((length > other_length) - (length < other_length));
}

/*
 * Refuse catalog entry id's name, longer than EROFS_NAME_MAX bytes: the
 * catalog bounds a name in UTF-16 units, EROFS in bytes. Every name is
 * checked here, in its parent's walk, which planning runs before it
 * reaches the entry itself. The source lookup keeps the walk's record.
 */
static fsimg_status_t
refuse_name(fsimg_writer_t *writer, uint64_t id, fsimg_error_t *error)
{
	fsimg_catalog_entry_t entry;
	fsimg_status_t status;

	status = fsimg_catalog_read_source(writer, id, &entry, error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_WRITER,
	    "name exceeds the EROFS limit of 255 bytes");
	fsimg_error_path(error, entry.entry.path);
	return (FSIMG_EINVAL);
}

/*
 * Add the entry name, naming catalog ID id, to the open block, resolving
 * its NID and file type first when building bytes, and closing the open
 * block first when the entry does not fit. A name of at most
 * EROFS_NAME_MAX bytes always fits an empty block of 512 bytes or more,
 * so a refusal there is an internal error.
 */
static fsimg_status_t
place_entry(erofs_dir_pack_t *pack, const erofs_dir_builder_t *builder,
    uint64_t id, const char *name, size_t name_length, fsimg_error_t *error)
{
	erofs_dir_entry_t entry;
	fsimg_status_t status;

	if (name_length == 0 || name_length > EROFS_NAME_MAX)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_WRITER, "EROFS directory entry name length"));
	entry = (erofs_dir_entry_t){ .name = name, .name_length = name_length };
	if (builder != NULL) {
		status = builder->resolve(builder->argument, id, &entry, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (!erofs_dir_pack_fits(pack, name_length)) {
		status = close_block(pack, builder, error);
		if (status != FSIMG_OK)
			return (status);
	}
	if (!erofs_dir_pack_fits(pack, name_length))
		return (fsimg_error_set(error, FSIMG_ESTATE, 0,
		    FSIMG_OP_WRITER, "EROFS directory entry exceeds a block"));
	erofs_dir_pack_add(pack, &entry);
	return (FSIMG_OK);
}

/* Close the open block and, when building bytes, deliver it. */
static fsimg_status_t
close_block(erofs_dir_pack_t *pack, const erofs_dir_builder_t *builder,
    fsimg_error_t *error)
{
	size_t used;

	used = erofs_dir_pack_close(pack);
	if (builder == NULL)
		return (FSIMG_OK);
	return (builder->block(builder->argument, pack->closed - 1, pack->block,
	    used, error));
}
