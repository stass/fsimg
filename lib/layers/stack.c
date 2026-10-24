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
 * Schemes, containers and gzip are read and written, except hbcifs, which
 * is read only, and ifsboot, which writes only the machines and modes its
 * machine table lists as writable; zstd and xz are compression names reserved
 * for later profiles, recognized by name and suffix with no operations.
 */
static const fsimg_layer_t layers[] = {
	{ .name = "gpt", .kind = FSIMG_LAYER_SCHEME, .scheme = &fsimg_gpt_scheme,
	    .settings = &fsimg_gpt_settings },
	{ .name = "mbr", .kind = FSIMG_LAYER_SCHEME, .scheme = &fsimg_mbr_scheme,
	    .settings = &fsimg_mbr_settings },
	{ .name = "dmg", .suffix = ".dmg", .kind = FSIMG_LAYER_CONTAINER,
	    .container = &fsimg_dmg_container, .settings = &fsimg_dmg_settings },
	{ .name = "vhd", .suffix = ".vhd", .kind = FSIMG_LAYER_CONTAINER,
	    .container = &fsimg_vhd_container, .settings = &fsimg_vhd_settings },
	{ .name = "simg", .suffix = ".simg", .kind = FSIMG_LAYER_CONTAINER,
	    .container = &fsimg_simg_container, .settings = &fsimg_simg_settings },
	{ .name = "ifsboot", .kind = FSIMG_LAYER_CONTAINER,
	    .container = &fsimg_ifsboot_container,
	    .settings = &fsimg_ifsboot_settings },
	{ .name = "hbcifs", .kind = FSIMG_LAYER_CONTAINER,
	    .container = &fsimg_hbcifs_container },
	{ .name = "gzip", .suffix = ".gz", .kind = FSIMG_LAYER_COMPRESSION,
	    .compression = &fsimg_gzip_compression,
	    .settings = &fsimg_gzip_settings },
	{ .name = "zstd", .suffix = ".zst", .kind = FSIMG_LAYER_COMPRESSION },
	{ .name = "xz", .suffix = ".xz", .kind = FSIMG_LAYER_COMPRESSION }
};

#define LAYER_COUNT	(sizeof(layers) / sizeof(layers[0]))

/* Filesystem names the grammar reserves without a driver. */
static const char *const reserved_filesystems[] = { "apfs", "hfs" };

#define RESERVED_COUNT	(sizeof(reserved_filesystems) / sizeof(reserved_filesystems[0]))

static void put_text(char *, size_t, size_t *, char);
static void put_key(fsimg_layer_info_t *, size_t *, const char *);
static bool layer_selected(const fsimg_stack_t *, const fsimg_layer_t *,
    const fsimg_layer_t *);
static const fsimg_setting_t *file_row(fsimg_file_setting_t);
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
 * in the stack text. The token is rendered like every path an error
 * carries; one too long for the field keeps its tail, as the rendering
 * itself does.
 */
static fsimg_status_t
stack_syntax(fsimg_error_t *error, const char *message, const char *token,
    size_t length)
{
	char text[sizeof(error->path)];
	fsimg_status_t status;
	size_t tail_start;

	status = fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE, message);
	if (error != NULL) {
		tail_start = length < sizeof(text) ? 0 :
		    length - (sizeof(text) - 1);
		bcopy(token + tail_start, text, length - tail_start);
		text[length - tail_start] = '\0';
		fsimg_error_path(error, text);
		error->path_truncated = error->path_truncated || tail_start != 0;
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
	return (fsimg_stack_carried(stack,
	    fsimg_stack_layer(stack, FSIMG_LAYER_CONTAINER), error));
}

fsimg_status_t
fsimg_stack_carried(const fsimg_stack_t *stack, const fsimg_layer_t *container,
    fsimg_error_t *error)
{
	const fsimg_stack_slot_t *filesystem, *scheme;
	const char *carries;

	if (container == NULL || container->container == NULL ||
	    container->container->carries == NULL)
		return (FSIMG_OK);
	carries = container->container->carries;
	filesystem = &stack->slots[FSIMG_LAYER_FILESYSTEM];
	scheme = &stack->slots[FSIMG_LAYER_SCHEME];
	if (scheme->state != FSIMG_SLOT_NAMED &&
	    (filesystem->state != FSIMG_SLOT_NAMED ||
	    strcmp(filesystem->name, carries) == 0))
		return (FSIMG_OK);
	fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
	    scheme->state == FSIMG_SLOT_NAMED ?
	    "the container holds a volume, not a partition table" :
	    "the container holds a different filesystem");
	fsimg_error_path(error, container->name);
	return (FSIMG_EINVAL);
}

const fsimg_layer_t *
fsimg_stack_layer(const fsimg_stack_t *stack, fsimg_layer_kind_t kind)
{
	const fsimg_stack_slot_t *slot = &stack->slots[kind];

	return (slot->state == FSIMG_SLOT_NAMED ? fsimg_layer_find(slot->name) :
	    NULL);
}

fsimg_status_t
fsimg_compression_named(const char *name, size_t size, bool writing,
    const fsimg_layer_t **out, fsimg_error_t *error)
{
	const fsimg_layer_t *layer;
	bool implemented;

	*out = NULL;
	if (memchr(name, '\0', size) == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "compression name is not terminated"));
	if (name[0] == '\0')
		return (FSIMG_OK);
	layer = fsimg_layer_find(name);
	if (layer == NULL || layer->kind != FSIMG_LAYER_COMPRESSION) {
		fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "not a registered compression");
		fsimg_error_path(error, name);
		return (FSIMG_EINVAL);
	}
	implemented = layer->compression != NULL && (writing ?
	    layer->compression->encoder != NULL :
	    layer->compression->decode != NULL);
	if (!implemented) {
		fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0, FSIMG_OP_IMAGE,
		    writing ? "writing this compression is not supported" :
		    "reading this compression is not supported");
		fsimg_error_path(error, name);
		return (FSIMG_EUNSUPPORTED);
	}
	*out = layer;
	return (FSIMG_OK);
}

/* Whether an operation selects a layer: the scheme or container its stack
 * names, or the compression it names, which fills no slot. Layers are
 * compared as registry records.
 */
static bool
layer_selected(const fsimg_stack_t *stack, const fsimg_layer_t *compression,
    const fsimg_layer_t *layer)
{
	if (layer->kind == FSIMG_LAYER_COMPRESSION)
		return (layer == compression);
	return (layer == fsimg_stack_layer(stack, layer->kind));
}

/*
 * The applicability of every layer setting. The visit covers every
 * registered layer, the ones the stack lacks included, since a setting
 * of an absent layer that holds anything but its default must be refused,
 * never ignored; rows are checked in catalogue order, so the first
 * failing row is the one reported.
 */
fsimg_status_t
fsimg_layer_options_check(const fsimg_stack_t *stack,
    const fsimg_layer_t *compression, const fsimg_layer_options_t *options,
    const fsimg_layer_inputs_t *inputs, bool writing, fsimg_error_t *error)
{
	const fsimg_setting_records_t records = { .layers = options,
	    .inputs = inputs };
	const fsimg_setting_t *row;
	const fsimg_layer_t *layer;
	char key[32];
	fsimg_status_t status;

	for (unsigned i = 0; i < 4; i++) {
		if (options->reserved[i] != 0)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_IMAGE, "nonzero reserved layer option"));
	}
	for (size_t i = 0; (layer = fsimg_layer_at(i)) != NULL; i++) {
		for (size_t j = 0; layer->settings != NULL &&
		    j < layer->settings->count; j++) {
			row = &layer->settings->rows[j];
			status = fsimg_setting_check(row, &records, error);
			if (status != FSIMG_OK)
				return (status);
			if (fsimg_setting_is_default(row, &records))
				continue;
			fsimg_setting_key(row, key, sizeof(key));
			if (!layer_selected(stack, compression, layer))
				return (fsimg_setting_error(error, key,
				    "setting names a layer the stack does not contain"));
			if (!writing && (row->scope & FSIMG_SCOPE_READ) == 0)
				return (fsimg_setting_error(error, key,
				    "layer setting applies only to creation"));
		}
	}
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_layer_options_disk_only(const fsimg_layer_options_t *options,
    fsimg_error_t *error)
{
	const fsimg_setting_records_t records = { .layers = options };
	const fsimg_setting_t *row;
	const fsimg_layer_t *layer;
	char key[32];

	for (size_t i = 0; (layer = fsimg_layer_at(i)) != NULL; i++) {
		for (size_t j = 0; layer->settings != NULL &&
		    j < layer->settings->count; j++) {
			row = &layer->settings->rows[j];
			if ((row->scope & FSIMG_SCOPE_PARTITION) == 0 ||
			    fsimg_setting_is_default(row, &records))
				continue;
			fsimg_setting_key(row, key, sizeof(key));
			return (fsimg_setting_error(error, key,
			    "compose takes partition settings from its parts"));
		}
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
		out->capabilities = FSIMG_CAP_READ | FSIMG_CAP_VERIFY |
		    (layer->container->writer != NULL ? FSIMG_CAP_WRITE : 0);
		snprintf(out->codecs, sizeof(out->codecs), "%s",
		    layer->container->codecs);
		if (layer->container->zlib)
			snprintf(out->codec_version, sizeof(out->codec_version),
			    "zlib %s", zlibVersion());
		if (layer->container->carries != NULL)
			snprintf(out->carries, sizeof(out->carries), "%s",
			    layer->container->carries);
	}
	/* The operations fsimg_compression_named accepts, read and write. */
	if (layer->compression != NULL) {
		out->capabilities =
		    (layer->compression->decode != NULL ? FSIMG_CAP_READ |
		    FSIMG_CAP_VERIFY : 0) |
		    (layer->compression->encoder != NULL ? FSIMG_CAP_WRITE : 0);
		if (layer->compression->zlib)
			snprintf(out->codec_version, sizeof(out->codec_version),
			    "zlib %s", zlibVersion());
	}
	/* The layer's keys in its slice's order, file settings first; the
	 * longest list fits.
	 */
	used = 0;
	for (size_t i = 0; layer->settings != NULL &&
	    i < layer->settings->count; i++)
		put_key(out, &used, layer->settings->rows[i].key);
	(*cursor)++;
	return (FSIMG_OK);
}

/* Append one key to a capability record's comma-separated settings. */
static void
put_key(fsimg_layer_info_t *out, size_t *used, const char *key)
{
	if (*used != 0)
		put_text(out->settings, sizeof(out->settings), used, ',');
	for (const char *c = key; *c != '\0'; c++)
		put_text(out->settings, sizeof(out->settings), used, *c);
}

/* The catalogue row of a file setting, found in its layer's slice. */
static const fsimg_setting_t *
file_row(fsimg_file_setting_t setting)
{
	const fsimg_setting_t *row;
	const fsimg_layer_t *layer;

	for (size_t i = 0; (layer = fsimg_layer_at(i)) != NULL; i++) {
		for (size_t j = 0; layer->settings != NULL &&
		    j < layer->settings->count; j++) {
			row = &layer->settings->rows[j];
			if (row->record == FSIMG_RECORD_INPUTS &&
			    row->file == setting)
				return (row);
		}
	}
	return (NULL);
}

void
fsimg_file_setting_name(fsimg_file_setting_t setting, char *buffer,
    size_t capacity)
{
	fsimg_setting_key(file_row(setting), buffer, capacity);
}

fsimg_io_t *
fsimg_file_setting_input(const fsimg_layer_inputs_t *inputs,
    fsimg_file_setting_t setting)
{
	const fsimg_setting_records_t records = { .inputs = inputs };
	fsimg_setting_value_t value;

	fsimg_setting_load(file_row(setting), &records, &value);
	return (value.io);
}

void
fsimg_file_setting_set(fsimg_layer_inputs_t *inputs,
    fsimg_file_setting_t setting, fsimg_io_t *io)
{
	const fsimg_setting_targets_t targets = { .inputs = inputs };
	const fsimg_setting_value_t value = { .io = io };

	fsimg_setting_store(file_row(setting), &targets, &value);
}

bool
fsimg_layer_section(const fsimg_layer_t *layer, const char *name,
    fsimg_file_setting_t *out)
{
	const fsimg_setting_t *row;

	if (layer == NULL || layer->settings == NULL)
		return (false);
	row = fsimg_setting_find(layer->settings, name, strlen(name));
	if (row == NULL || row->record != FSIMG_RECORD_INPUTS)
		return (false);
	*out = row->file;
	return (true);
}
