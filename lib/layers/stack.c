/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "format.h"
#include "layer.h"

/*
 * The layer registry is the one list of layer names and file suffixes.
 * Containers and compression are recognized by name and suffix; this build
 * has no implementation of them, so their records carry no operations.
 */
static const fsimg_layer_t layers[] = {
	{ .name = "gpt", .kind = FSIMG_LAYER_SCHEME, .scheme = &fsimg_gpt_scheme },
	{ .name = "mbr", .kind = FSIMG_LAYER_SCHEME, .scheme = &fsimg_mbr_scheme },
	{ .name = "dmg", .suffix = ".dmg", .kind = FSIMG_LAYER_CONTAINER },
	{ .name = "vhd", .suffix = ".vhd", .kind = FSIMG_LAYER_CONTAINER },
	{ .name = "simg", .suffix = ".simg", .kind = FSIMG_LAYER_CONTAINER },
	{ .name = "gzip", .suffix = ".gz", .kind = FSIMG_LAYER_COMPRESSION }
};

#define LAYER_COUNT	(sizeof(layers) / sizeof(layers[0]))

/* Filesystem names the grammar reserves without a driver. */
static const char *const reserved_filesystems[] = { "apfs", "hfs" };

#define RESERVED_COUNT	(sizeof(reserved_filesystems) / sizeof(reserved_filesystems[0]))

static void put_text(char *, size_t, size_t *, char);
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

fsimg_status_t
fsimg_layer_next(size_t *cursor, fsimg_layer_info_t *out, fsimg_error_t *error)
{
	const fsimg_layer_t *layer;

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
		out->capabilities = FSIMG_CAP_READ | FSIMG_CAP_VERIFY;
	(*cursor)++;
	return (FSIMG_OK);
}
