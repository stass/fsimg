/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <zlib.h>

#include "format.h"
#include "layer.h"

/*
 * The layer registry is the one list of layer names and file suffixes.
 * Schemes, containers and gzip are read and written; zstd and xz are
 * compression names reserved for later profiles, recognized by name and
 * suffix with no operations.
 */
static const fsimg_layer_t layers[] = {
	{ .name = "gpt", .kind = FSIMG_LAYER_SCHEME, .scheme = &fsimg_gpt_scheme },
	{ .name = "mbr", .kind = FSIMG_LAYER_SCHEME, .scheme = &fsimg_mbr_scheme },
	{ .name = "dmg", .suffix = ".dmg", .kind = FSIMG_LAYER_CONTAINER,
	    .container = &fsimg_dmg_container },
	{ .name = "vhd", .suffix = ".vhd", .kind = FSIMG_LAYER_CONTAINER,
	    .container = &fsimg_vhd_container },
	{ .name = "simg", .suffix = ".simg", .kind = FSIMG_LAYER_CONTAINER,
	    .container = &fsimg_simg_container },
	{ .name = "gzip", .suffix = ".gz", .kind = FSIMG_LAYER_COMPRESSION,
	    .compression = &fsimg_gzip_compression },
	{ .name = "zstd", .suffix = ".zst", .kind = FSIMG_LAYER_COMPRESSION },
	{ .name = "xz", .suffix = ".xz", .kind = FSIMG_LAYER_COMPRESSION }
};

#define LAYER_COUNT	(sizeof(layers) / sizeof(layers[0]))

/*
 * Every setting fsimg_layer_options_t holds: the layer and key naming it,
 * its field, whether reading uses it, and whether it describes the one
 * partition a writer or wrap creates rather than the disk. A field holds
 * its default when all its bytes are zero, or for text when it is empty.
 * This table is the one list of layer settings: checking options, compose's
 * refusal of partition settings and the capability record all read it, so
 * a setting added here is covered by each.
 */
typedef struct layer_setting {
	const char	*layer;
	const char	*key;
	size_t		 offset;
	size_t		 size;
	bool		 text;
	bool		 read;
	bool		 partition;
} layer_setting_t;

#define SETTING_FIELD(member)	offsetof(fsimg_layer_options_t, member), \
	sizeof(((fsimg_layer_options_t *)NULL)->member)

static const layer_setting_t layer_settings[] = {
	{ "gpt", "sector", SETTING_FIELD(gpt_sector_bytes), false, true, false },
	{ "gpt", "disk", SETTING_FIELD(gpt_disk_bytes), false, false, false },
	{ "gpt", "type", SETTING_FIELD(gpt_type), true, false, true },
	{ "gpt", "name", SETTING_FIELD(gpt_name), true, false, true },
	{ "gpt", "disk-guid", SETTING_FIELD(gpt_disk_guid), false, false, false },
	{ "gpt", "guid", SETTING_FIELD(gpt_guid), false, false, true },
	{ "mbr", "disk", SETTING_FIELD(mbr_disk_bytes), false, false, false },
	{ "mbr", "type", SETTING_FIELD(mbr_type), true, false, true },
	{ "mbr", "active", SETTING_FIELD(mbr_active), false, false, true },
	{ "mbr", "id", SETTING_FIELD(mbr_id_set), false, false, false },
	{ "dmg", "compress", SETTING_FIELD(dmg_compress), false, false, false },
	{ "dmg", "level", SETTING_FIELD(dmg_level), false, false, false },
	{ "dmg", "chunk", SETTING_FIELD(dmg_chunk_bytes), false, false, false },
	{ "dmg", "uuid", SETTING_FIELD(dmg_uuid), false, false, false },
	{ "vhd", "align", SETTING_FIELD(vhd_align_bytes), false, false, false },
	{ "vhd", "uuid", SETTING_FIELD(vhd_uuid), false, false, false },
	{ "simg", "block", SETTING_FIELD(simg_block_bytes), false, false, false },
	{ "simg", "crc", SETTING_FIELD(simg_crc), false, false, false },
	{ "gzip", "level", SETTING_FIELD(gzip_level), false, false, false }
};

#define SETTING_COUNT	(sizeof(layer_settings) / sizeof(layer_settings[0]))

/* Filesystem names the grammar reserves without a driver. */
static const char *const reserved_filesystems[] = { "apfs", "hfs" };

#define RESERVED_COUNT	(sizeof(reserved_filesystems) / sizeof(reserved_filesystems[0]))

static void put_text(char *, size_t, size_t *, char);
static bool setting_is_default(const fsimg_layer_options_t *,
    const layer_setting_t *);
static fsimg_status_t setting_error(fsimg_error_t *, const layer_setting_t *,
    const char *);
static bool slot_of_name(const char *, fsimg_layer_kind_t *);
static fsimg_status_t stack_syntax(fsimg_error_t *, const char *, const char *,
    size_t);

const fsimg_layer_t *
fsimg_layer_at(size_t index)
{
	return (index < LAYER_COUNT ? &layers[index] : NULL);
}

const fsimg_layer_t *
fsimg_layer_find(const char *name)
{
	for (size_t i = 0; i < LAYER_COUNT; i++) {
		if (strcmp(layers[i].name, name) == 0)
			return (&layers[i]);
	}
	return (NULL);
}

/* The slot a name fills: a registered or reserved filesystem, or a scheme
 * or container. A compression is not a slot, so its name fills none.
 */
static bool
slot_of_name(const char *name, fsimg_layer_kind_t *slot)
{
	const fsimg_format_variant_t *variant;
	const fsimg_layer_t *layer;

	*slot = FSIMG_LAYER_FILESYSTEM;
	if (fsimg_format_find(name, &variant) != NULL)
		return (true);
	for (size_t i = 0; i < RESERVED_COUNT; i++) {
		if (strcmp(name, reserved_filesystems[i]) == 0)
			return (true);
	}
	layer = fsimg_layer_find(name);
	if (layer == NULL || layer->kind == FSIMG_LAYER_COMPRESSION)
		return (false);
	*slot = layer->kind;
	return (true);
}

/* A syntax error naming the offending token, which need not be terminated
 * in the stack text.
 */
static fsimg_status_t
stack_syntax(fsimg_error_t *error, const char *message, const char *token,
    size_t length)
{
	fsimg_status_t status;
	int count;

	status = fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE, message);
	if (error != NULL) {
		count = snprintf(error->path, sizeof(error->path), "%.*s",
		    (int)(length < sizeof(error->path) ? length :
		    sizeof(error->path)), token);
		error->path_truncated = count < 0 ||
		    length >= sizeof(error->path);
	}
	return (status);
}

/*
 * The stack grammar: tokens separated by "/", each a layer name or "-". A
 * name fills the slot of its kind; "-" fills the next slot and marks it
 * empty. Every token must fill a slot later than every slot filled before
 * it, so a repeated kind or a misordered name is rejected as it is read and
 * the result is three slots with nothing left to check.
 */
fsimg_status_t
fsimg_stack_parse(const char *text, fsimg_stack_t *out, fsimg_error_t *error)
{
	fsimg_stack_t stack;
	fsimg_layer_kind_t slot, next;
	const char *token, *end;
	char name[sizeof(stack.slots[0].name)];
	size_t length;

	fsimg_error_clear(error);
	if (text == NULL || out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "missing stack text or output"));
	bzero(&stack, sizeof(stack));
	if (text[0] == '\0')
		return (stack_syntax(error, "the stack is empty", text, 0));
	next = FSIMG_LAYER_FILESYSTEM;
	for (token = text;; token = end + 1) {
		end = strchr(token, '/');
		length = end == NULL ? strlen(token) : (size_t)(end - token);
		if (length == 0)
			return (stack_syntax(error, "the stack has an empty token",
			    text, strlen(text)));
		if (length == 1 && token[0] == '-') {
			if (next == FSIMG_LAYER_FILESYSTEM)
				return (stack_syntax(error,
				    "- cannot fill the filesystem slot", token, 1));
			if (next >= FSIMG_STACK_SLOTS)
				return (stack_syntax(error,
				    "the stack has too many tokens", text,
				    strlen(text)));
			stack.slots[next].state = FSIMG_SLOT_EMPTY;
			next++;
		} else {
			if (length >= sizeof(name))
				return (stack_syntax(error, "unknown stack name",
				    token, length));
			bcopy(token, name, length);
			name[length] = '\0';
			if (!slot_of_name(name, &slot))
				return (stack_syntax(error, "unknown stack name",
				    token, length));
			if (slot < next)
				return (stack_syntax(error,
				    "stack name repeats or misorders a slot", token,
				    length));
			stack.slots[slot].state = FSIMG_SLOT_NAMED;
			bcopy(name, stack.slots[slot].name, length + 1);
			next = slot + 1;
		}
		if (end == NULL)
			break;
	}
	*out = stack;
	return (FSIMG_OK);
}

/* Store one byte of formatted text while it fits before the terminator,
 * counting it either way.
 */
static void
put_text(char *buffer, size_t capacity, size_t *used, char byte)
{
	if (*used + 1 < capacity)
		buffer[*used] = byte;
	(*used)++;
}

size_t
fsimg_stack_format(const fsimg_stack_t *stack, char *buffer, size_t capacity)
{
	const fsimg_stack_slot_t *slot;
	size_t used;

	used = 0;
	for (unsigned i = 0; i < FSIMG_STACK_SLOTS; i++) {
		slot = &stack->slots[i];
		if (slot->state != FSIMG_SLOT_NAMED)
			continue;
		if (used != 0)
			put_text(buffer, capacity, &used, '/');
		for (size_t j = 0; j < sizeof(slot->name) && slot->name[j] != '\0'; j++)
			put_text(buffer, capacity, &used, slot->name[j]);
	}
	if (capacity != 0)
		buffer[used < capacity ? used : capacity - 1] = '\0';
	return (used);
}

fsimg_status_t
fsimg_stack_validate(const fsimg_stack_t *stack, fsimg_error_t *error)
{
	const fsimg_stack_slot_t *slot;
	fsimg_layer_kind_t kind;

	for (unsigned i = 0; i < FSIMG_STACK_SLOTS; i++) {
		slot = &stack->slots[i];
		if (slot->state > FSIMG_SLOT_NAMED ||
		    memchr(slot->name, '\0', sizeof(slot->name)) == NULL ||
		    (slot->state == FSIMG_SLOT_NAMED) != (slot->name[0] != '\0'))
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_IMAGE, "malformed stack slot"));
		if (i == FSIMG_LAYER_FILESYSTEM && slot->state == FSIMG_SLOT_EMPTY)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_IMAGE, "the filesystem slot cannot be empty"));
		if (slot->state != FSIMG_SLOT_NAMED)
			continue;
		if (!slot_of_name(slot->name, &kind) || kind != i) {
			fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
			    "stack slot names a layer of another kind");
			fsimg_error_path(error, slot->name);
			return (FSIMG_EINVAL);
		}
	}
	return (FSIMG_OK);
}

const fsimg_layer_t *
fsimg_stack_layer(const fsimg_stack_t *stack, fsimg_layer_kind_t kind)
{
	const fsimg_stack_slot_t *slot = &stack->slots[kind];

	return (slot->state == FSIMG_SLOT_NAMED ? fsimg_layer_find(slot->name) :
	    NULL);
}

const fsimg_layer_t *
fsimg_write_compression(uint32_t flags)
{
	return ((flags & FSIMG_WRITE_GZIP) != 0 ? fsimg_layer_find("gzip") :
	    NULL);
}

static bool
setting_is_default(const fsimg_layer_options_t *options,
    const layer_setting_t *setting)
{
	const unsigned char *field;

	field = (const unsigned char *)options + setting->offset;
	if (setting->text)
		return (field[0] == '\0');
	for (size_t i = 0; i < setting->size; i++) {
		if (field[i] != 0)
			return (false);
	}
	return (true);
}

static fsimg_status_t
setting_error(fsimg_error_t *error, const layer_setting_t *setting,
    const char *message)
{
	char key[32];

	fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE, message);
	snprintf(key, sizeof(key), "%s:%s", setting->layer, setting->key);
	fsimg_error_path(error, key);
	return (FSIMG_EINVAL);
}

fsimg_status_t
fsimg_layer_options_check(const fsimg_stack_t *stack,
    const fsimg_layer_t *compression, const fsimg_layer_options_t *options,
    bool writing, fsimg_error_t *error)
{
	const layer_setting_t *setting;
	const fsimg_layer_t *layer;
	fsimg_layer_kind_t kind;
	const char *field;

	for (unsigned i = 0; i < 4; i++) {
		if (options->reserved[i] != 0)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_IMAGE, "nonzero reserved layer option"));
	}
	for (size_t i = 0; i < SETTING_COUNT; i++) {
		setting = &layer_settings[i];
		field = (const char *)options + setting->offset;
		if (setting->text && memchr(field, '\0', setting->size) == NULL)
			return (setting_error(error, setting,
			    "layer setting text is not terminated"));
		if (setting_is_default(options, setting))
			continue;
		/* A compression fills no slot; the operation names it. */
		kind = fsimg_layer_find(setting->layer)->kind;
		layer = kind == FSIMG_LAYER_COMPRESSION ? compression :
		    fsimg_stack_layer(stack, kind);
		if (layer == NULL || strcmp(layer->name, setting->layer) != 0)
			return (setting_error(error, setting,
			    "setting names a layer the stack does not contain"));
		if (!writing && !setting->read)
			return (setting_error(error, setting,
			    "layer setting applies only to creation"));
	}
	/* The MBR identifier's value means something only once it is set. */
	if (!options->mbr_id_set && options->mbr_id != 0) {
		fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "the MBR identifier has a value but is not set");
		fsimg_error_path(error, "mbr:id");
		return (FSIMG_EINVAL);
	}
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_layer_options_disk_only(const fsimg_layer_options_t *options,
    fsimg_error_t *error)
{
	for (size_t i = 0; i < SETTING_COUNT; i++) {
		if (layer_settings[i].partition &&
		    !setting_is_default(options, &layer_settings[i]))
			return (setting_error(error, &layer_settings[i],
			    "compose takes partition settings from its parts"));
	}
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_layer_next(size_t *cursor, fsimg_layer_info_t *out, fsimg_error_t *error)
{
	const fsimg_layer_t *layer;
	size_t used;

	fsimg_error_clear(error);
	if (cursor == NULL || out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "missing layer cursor or output"));
	bzero(out, sizeof(*out));
	layer = fsimg_layer_at(*cursor);
	if (layer == NULL)
		return (FSIMG_DONE);
	/* Registry names and suffixes are short constants that fit. */
	snprintf(out->name, sizeof(out->name), "%s", layer->name);
	if (layer->suffix != NULL)
		snprintf(out->suffix, sizeof(out->suffix), "%s", layer->suffix);
	out->kind = layer->kind;
	if (layer->scheme != NULL)
		out->capabilities = FSIMG_CAP_READ | FSIMG_CAP_WRITE |
		    FSIMG_CAP_VERIFY;
	if (layer->container != NULL) {
		out->capabilities = FSIMG_CAP_READ | FSIMG_CAP_WRITE |
		    FSIMG_CAP_VERIFY;
		snprintf(out->codecs, sizeof(out->codecs), "%s",
		    layer->container->codecs);
		if (layer->container->zlib)
			snprintf(out->codec_version, sizeof(out->codec_version),
			    "zlib %s", zlibVersion());
	}
	if (layer->compression != NULL) {
		out->capabilities = FSIMG_CAP_READ | FSIMG_CAP_VERIFY |
		    (layer->compression->encoder != NULL ? FSIMG_CAP_WRITE : 0);
		snprintf(out->codec_version, sizeof(out->codec_version),
		    "zlib %s", zlibVersion());
	}
	/* The layer's keys in table order; the longest list fits. */
	used = 0;
	for (size_t i = 0; i < SETTING_COUNT; i++) {
		if (strcmp(layer_settings[i].layer, layer->name) != 0)
			continue;
		if (used != 0)
			put_text(out->settings, sizeof(out->settings), &used, ',');
		for (const char *c = layer_settings[i].key; *c != '\0'; c++)
			put_text(out->settings, sizeof(out->settings), &used, *c);
	}
	(*cursor)++;
	return (FSIMG_OK);
}
