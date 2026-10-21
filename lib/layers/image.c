/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "format.h"
#include "io.h"
#include "layer.h"
#include "unicode.h"

/*
 * An image is the input peeled down to its disk, with the disk's partition
 * table enumerated and nothing of the filesystem opened. Reading peels
 * from the outside in: compression and containers are not implemented, so
 * the input is the disk; then one step decides between a partition scheme
 * and a bare volume by running the scheme probes and the filesystem probes
 * on the disk prefix together. Exactly one claim decides, so a boot sector
 * that also resembles a partition table is an ambiguity the caller resolves
 * by forcing a slot, never a guess.
 *
 * That scheme step runs at most once, on the first operation that needs
 * the table. A range or disk selection never needs it, so a damaged table
 * cannot block raw access to the disk's bytes.
 */
/* A format name with its terminator, as the public records hold it. */
#define FORMAT_NAME_BYTES	16
_Static_assert(sizeof(((fsimg_partition_info_t *)NULL)->format) ==
    FORMAT_NAME_BYTES, "a partition's format holds one format name");

struct fsimg_image {
	fsimg_ctx_t	*ctx;
	fsimg_io_t	*disk;		/* Borrowed input */
	fsimg_session_t	 session;
	fsimg_stack_t	 forced;
	fsimg_layer_options_t layers;
	uint64_t	 disk_bytes;
	/* The scheme step's result once detected is set; scheme stays NULL
	 * for a bare volume, and table is then empty.
	 */
	const fsimg_layer_t *scheme;
	fsimg_scheme_table_t table;
	bool		 detected;
	uint32_t	 cursor;	/* Slots next_partition has returned */
	/* The last selection: its slot (zero for none) and the filesystem
	 * its prefix claims, empty when none matched.
	 */
	uint32_t	 selected;
	char		 selected_format[FORMAT_NAME_BYTES];
};

static fsimg_status_t image_ready(fsimg_image_t *, fsimg_error_t *);
static const char *forced_filesystem(const fsimg_image_t *);
static fsimg_status_t validate_forced_stack(const fsimg_image_options_t *,
    fsimg_error_t *);
static fsimg_status_t read_prefix(fsimg_image_t *, uint64_t, uint64_t,
    size_t, unsigned char **, size_t *, fsimg_error_t *);
static fsimg_status_t claim_volume(fsimg_image_t *, uint64_t, uint64_t,
    fsimg_claim_t *, char [static FORMAT_NAME_BYTES], fsimg_error_t *);
static fsimg_status_t identify_disk(fsimg_image_t *, const fsimg_layer_t *,
    const fsimg_layer_t **, fsimg_error_t *);
static fsimg_status_t detect_scheme(fsimg_image_t *, fsimg_error_t *);
static void scheme_input(fsimg_image_t *, fsimg_scheme_input_t *);
static void name_to_utf8(const uint16_t *, char *, size_t);
static fsimg_status_t select_slice(fsimg_image_t *, uint64_t, uint64_t,
    uint32_t, fsimg_io_t **, fsimg_error_t *);
static fsimg_status_t select_record(fsimg_image_t *,
    const fsimg_partition_record_t *, fsimg_io_t **, fsimg_error_t *);
static fsimg_status_t select_range(fsimg_image_t *, const fsimg_selector_t *,
    fsimg_io_t **, fsimg_error_t *);
static fsimg_status_t select_automatic(fsimg_image_t *, fsimg_io_t **,
    fsimg_error_t *);
static fsimg_status_t find_by_index(fsimg_image_t *, uint32_t,
    const fsimg_partition_record_t **, fsimg_error_t *);
static fsimg_status_t find_by_name(fsimg_image_t *, const char *,
    const fsimg_partition_record_t **, fsimg_error_t *);
static fsimg_status_t find_by_type(fsimg_image_t *, const char *,
    const fsimg_partition_record_t **, fsimg_error_t *);

static fsimg_status_t
image_ready(fsimg_image_t *image, fsimg_error_t *error)
{
	if (image == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "missing image"));
	return (fsimg_ctx_ready(image->ctx, error));
}

static const char *
forced_filesystem(const fsimg_image_t *image)
{
	const fsimg_stack_slot_t *slot;

	slot = &image->forced.slots[FSIMG_LAYER_FILESYSTEM];
	return (slot->state == FSIMG_SLOT_NAMED ? slot->name : NULL);
}

/*
 * A forced stack must be well formed, and each layer it forces must be
 * readable in this build: a filesystem with a reader, an implemented
 * scheme, and no container. gpt:sector forces the GPT LBA size, so it
 * requires gpt in the scheme slot.
 */
static fsimg_status_t
validate_forced_stack(const fsimg_image_options_t *options,
    fsimg_error_t *error)
{
	const fsimg_stack_slot_t *slots = options->stack.slots;
	const fsimg_format_variant_t *variant;
	const fsimg_format_t *format;
	const fsimg_layer_t *scheme;
	uint32_t sector;
	fsimg_status_t status;

	status = fsimg_stack_validate(&options->stack, error);
	if (status != FSIMG_OK)
		return (status);
	if (slots[FSIMG_LAYER_FILESYSTEM].state == FSIMG_SLOT_NAMED) {
		format = fsimg_format_find(slots[FSIMG_LAYER_FILESYSTEM].name,
		    &variant);
		if (format == NULL || format->reader == NULL)
			return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
			    FSIMG_OP_IMAGE, "the named filesystem has no reader"));
	}
	if (slots[FSIMG_LAYER_SCHEME].state == FSIMG_SLOT_NAMED) {
		scheme = fsimg_layer_find(slots[FSIMG_LAYER_SCHEME].name);
		if (scheme == NULL || scheme->scheme == NULL)
			return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
			    FSIMG_OP_IMAGE, "the named partition scheme is not supported"));
	}
	if (slots[FSIMG_LAYER_CONTAINER].state == FSIMG_SLOT_NAMED)
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IMAGE, "image containers are not supported"));

	sector = options->layers.gpt_sector_bytes;
	if (sector != 0 && sector != 512 && sector != 4096)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "gpt:sector must be 512 or 4096"));
	if (sector != 0 && (slots[FSIMG_LAYER_SCHEME].state != FSIMG_SLOT_NAMED ||
	    strcmp(slots[FSIMG_LAYER_SCHEME].name, "gpt") != 0))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "gpt:sector requires gpt in the forced stack"));
	for (unsigned i = 0; i < 4; i++) {
		if (options->layers.reserved[i] != 0)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_IMAGE, "nonzero reserved layer option"));
	}
	return (FSIMG_OK);
}

void
fsimg_layer_options_init(fsimg_layer_options_t *options)
{
	bzero(options, sizeof(*options));
}

void
fsimg_image_options_init(fsimg_image_options_t *options)
{
	bzero(options, sizeof(*options));
	options->size = sizeof(*options);
	options->version = FSIMG_OPTIONS_VERSION;
	fsimg_layer_options_init(&options->layers);
}

fsimg_status_t
fsimg_image_open(fsimg_ctx_t *ctx, fsimg_io_t *input,
    const fsimg_image_options_t *options, fsimg_image_t **out,
    fsimg_error_t *error)
{
	fsimg_image_options_t defaults;
	fsimg_image_t *image;
	fsimg_error_t local;
	uint64_t disk_bytes;
	fsimg_status_t status;

	if (error == NULL)
		error = &local;
	if (out != NULL)
		*out = NULL;
	status = fsimg_ctx_ready(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL || input == NULL || fsimg_io_context(input) != ctx)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "image needs input and output in its context"));
	if (options == NULL) {
		fsimg_image_options_init(&defaults);
		options = &defaults;
	}
	if (options->size != sizeof(*options) ||
	    options->version != FSIMG_OPTIONS_VERSION || options->flags != 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "invalid image options size, version or flags"));
	for (unsigned i = 0; i < 4; i++) {
		if (options->reserved[i] != 0)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_IMAGE, "nonzero reserved image option"));
	}
	status = validate_forced_stack(options, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_io_get_size(input, &disk_bytes, error);
	if (status != FSIMG_OK)
		return (status);

	image = fsimg_alloc(ctx, sizeof(*image), error);
	if (image == NULL)
		return (error->status);
	bzero(image, sizeof(*image));
	image->ctx = ctx;
	image->disk = input;
	image->forced = options->stack;
	image->layers = options->layers;
	image->disk_bytes = disk_bytes;
	fsimg_session_init(&image->session, ctx, "image layers", 0);
	*out = image;
	return (FSIMG_OK);
}

void
fsimg_image_free(fsimg_image_t *image)
{
	if (image == NULL || image->ctx->in_callback)
		return;
	fsimg_dealloc(image->table.records);
	fsimg_dealloc(image);
}

/* Read up to want bytes at a disk offset, cut to the length available
 * there, into a buffer charged to the context. Zero bytes need no buffer.
 */
static fsimg_status_t
read_prefix(fsimg_image_t *image, uint64_t offset, uint64_t available,
    size_t want, unsigned char **prefix, size_t *length, fsimg_error_t *error)
{
	size_t transferred;
	fsimg_status_t status;

	*prefix = NULL;
	*length = available < want ? (size_t)available : want;
	if (*length == 0)
		return (FSIMG_OK);
	*prefix = fsimg_alloc(image->ctx, *length, error);
	if (*prefix == NULL)
		return (error->status);
	status = fsimg_io_read_session(image->disk, &image->session, offset,
	    *prefix, *length, &transferred, error);
	if (status != FSIMG_OK) {
		fsimg_dealloc(*prefix);
		*prefix = NULL;
	}
	return (status);
}

/* What the filesystem probes, the forced one or all of them, say about the
 * volume at a disk range. format receives the identified format's name.
 */
static fsimg_status_t
claim_volume(fsimg_image_t *image, uint64_t offset, uint64_t length,
    fsimg_claim_t *claim, char format[static FORMAT_NAME_BYTES],
    fsimg_error_t *error)
{
	fsimg_format_survey_t survey;
	unsigned char *prefix;
	size_t prefix_length;
	fsimg_status_t status;

	*claim = FSIMG_CLAIM_NONE;
	format[0] = '\0';
	status = read_prefix(image, offset, length,
	    fsimg_format_probe_bytes(forced_filesystem(image)), &prefix,
	    &prefix_length, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_format_identify(forced_filesystem(image), prefix,
	    prefix_length, length, &survey, error);
	fsimg_dealloc(prefix);
	if (status != FSIMG_OK)
		return (status);
	if (survey.claims > 1)
		*claim = FSIMG_CLAIM_AMBIGUOUS;
	else if (survey.claims == 0)
		*claim = FSIMG_CLAIM_NONE;
	else if (survey.status == FSIMG_OK) {
		*claim = FSIMG_CLAIM_FORMAT;
		snprintf(format, FORMAT_NAME_BYTES, "%s",
		    survey.variant->info.name);
	} else if (survey.status == FSIMG_EUNSUPPORTED)
		*claim = FSIMG_CLAIM_UNSUPPORTED;
	else
		*claim = FSIMG_CLAIM_CORRUPT;
	return (FSIMG_OK);
}

/*
 * The scheme step's probes, run on one prefix of the disk: the forced
 * scheme's alone, or every scheme's together with the filesystem probes.
 * On success *scheme is the scheme that claimed, or NULL for a bare volume.
 * Unforced, exactly one claim decides; none is an unrecognized disk and
 * several are an ambiguity. A scheme claim that is corrupt or unsupported
 * fails with its probe's reason, never falling back to a bare volume.
 */
static fsimg_status_t
identify_disk(fsimg_image_t *image, const fsimg_layer_t *forced,
    const fsimg_layer_t **scheme, fsimg_error_t *error)
{
	const fsimg_layer_t *layer;
	fsimg_format_survey_t filesystems;
	fsimg_error_t detail, reason;
	unsigned char *prefix;
	size_t want, length;
	unsigned claims;
	fsimg_status_t status, verdict;

	*scheme = NULL;
	fsimg_error_clear(&reason);
	want = forced != NULL ? forced->scheme->probe_bytes :
	    fsimg_format_probe_bytes(forced_filesystem(image));
	for (size_t i = 0; forced == NULL && (layer = fsimg_layer_at(i)) != NULL; i++) {
		if (layer->scheme != NULL && layer->scheme->probe_bytes > want)
			want = layer->scheme->probe_bytes;
	}
	status = read_prefix(image, 0, image->disk_bytes, want, &prefix, &length,
	    error);
	if (status != FSIMG_OK)
		return (status);
	if (forced != NULL) {
		status = forced->scheme->probe(prefix, length, image->disk_bytes, error);
		fsimg_dealloc(prefix);
		if (status == FSIMG_OK)
			*scheme = forced;
		return (status);
	}

	/* Count the scheme claims, keeping the first claimant with its verdict
	 * and reason: a corrupt or unsupported claim is still a claim.
	 */
	claims = 0;
	verdict = FSIMG_OK;
	for (size_t i = 0; (layer = fsimg_layer_at(i)) != NULL; i++) {
		if (layer->scheme == NULL)
			continue;
		fsimg_error_clear(&detail);
		status = layer->scheme->probe(prefix, length < layer->scheme->probe_bytes ?
		    length : layer->scheme->probe_bytes, image->disk_bytes, &detail);
		if (status == FSIMG_EFORMAT)
			continue;
		if (claims++ == 0) {
			*scheme = layer;
			verdict = status;
			reason = detail;
		}
	}
	status = fsimg_format_identify(forced_filesystem(image), prefix, length,
	    image->disk_bytes, &filesystems, error);
	fsimg_dealloc(prefix);
	if (status != FSIMG_OK)
		return (status);

	/* Nothing claimed; both kinds claimed; several schemes claimed; one
	 * scheme claimed but its table is damaged or unsupported.
	 */
	if (claims + filesystems.claims == 0)
		status = fsimg_error_set(error, FSIMG_EFORMAT, 0, FSIMG_OP_IMAGE,
		    "no partition table or filesystem recognized");
	else if (claims != 0 && filesystems.claims != 0)
		status = fsimg_error_set(error, FSIMG_EFORMAT, 0, FSIMG_OP_IMAGE,
		    "the disk matches both a partition table and a filesystem; "
		    "force the scheme slot");
	else if (claims > 1)
		status = fsimg_error_set(error, FSIMG_EFORMAT, 0, FSIMG_OP_IMAGE,
		    "the disk matches several partition schemes; force one");
	else if (verdict != FSIMG_OK) {
		*error = reason;
		status = verdict;
	}
	/* Otherwise one scheme claimed, or the filesystems (one, or several
	 * left for the reader to report) make it a bare volume.
	 */
	if (status != FSIMG_OK)
		*scheme = NULL;
	return (status);
}

static void
scheme_input(fsimg_image_t *image, fsimg_scheme_input_t *input)
{
	input->ctx = image->ctx;
	input->disk = image->disk;
	input->session = &image->session;
	input->options = &image->layers;
	input->disk_bytes = image->disk_bytes;
}

/* Decide between a scheme and a bare volume, once, and validate the
 * scheme's tables completely. A forced empty scheme slot decides without
 * reading: the disk is the volume.
 */
static fsimg_status_t
detect_scheme(fsimg_image_t *image, fsimg_error_t *error)
{
	const fsimg_stack_slot_t *slot;
	const fsimg_layer_t *forced, *scheme;
	fsimg_scheme_input_t input;
	fsimg_status_t status;

	if (image->detected)
		return (FSIMG_OK);
	slot = &image->forced.slots[FSIMG_LAYER_SCHEME];
	if (slot->state == FSIMG_SLOT_EMPTY) {
		image->detected = true;
		return (FSIMG_OK);
	}
	forced = slot->state == FSIMG_SLOT_NAMED ? fsimg_layer_find(slot->name) :
	    NULL;
	status = identify_disk(image, forced, &scheme, error);
	if (status != FSIMG_OK)
		return (status);
	if (scheme != NULL) {
		scheme_input(image, &input);
		status = scheme->scheme->open(&input, &image->table, error);
		if (status != FSIMG_OK)
			return (status);
	}
	image->scheme = scheme;
	image->detected = true;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_image_get_info(fsimg_image_t *image, fsimg_image_info_t *out,
    fsimg_error_t *error)
{
	fsimg_stack_t stack;
	fsimg_error_t local;
	fsimg_status_t status;

	if (error == NULL)
		error = &local;
	status = image_ready(image, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "missing image information output"));
	status = detect_scheme(image, error);
	if (status != FSIMG_OK)
		return (status);

	bzero(out, sizeof(*out));
	bzero(&stack, sizeof(stack));
	stack.slots[FSIMG_LAYER_FILESYSTEM] =
	    image->forced.slots[FSIMG_LAYER_FILESYSTEM];
	if (stack.slots[FSIMG_LAYER_FILESYSTEM].state != FSIMG_SLOT_NAMED &&
	    image->selected_format[0] != '\0') {
		stack.slots[FSIMG_LAYER_FILESYSTEM].state = FSIMG_SLOT_NAMED;
		bcopy(image->selected_format, stack.slots[FSIMG_LAYER_FILESYSTEM].name,
		    sizeof(image->selected_format));
	}
	if (image->scheme != NULL) {
		stack.slots[FSIMG_LAYER_SCHEME].state = FSIMG_SLOT_NAMED;
		snprintf(stack.slots[FSIMG_LAYER_SCHEME].name,
		    sizeof(stack.slots[FSIMG_LAYER_SCHEME].name), "%s",
		    image->scheme->name);
		snprintf(out->scheme, sizeof(out->scheme), "%s", image->scheme->name);
	}
	fsimg_stack_format(&stack, out->stack, sizeof(out->stack));
	bcopy(image->table.disk_guid, out->disk_guid, sizeof(out->disk_guid));
	out->disk_bytes = image->disk_bytes;
	out->sector_bytes = image->table.sector_bytes;
	out->partition_count = image->table.count;
	out->selected = image->selected;
	out->mbr_disk_id = image->table.mbr_disk_id;
	out->scope = FSIMG_VALIDATION_STRUCTURAL;
	return (FSIMG_OK);
}

/* A GPT name as UTF-8 for display. Names are untrusted, so an unpaired
 * surrogate becomes U+FFFD instead of failing the listing; selection by
 * name compares the stored units, not this text.
 */
static void
name_to_utf8(const uint16_t *units, char *text, size_t capacity)
{
	static const uint16_t replacement = 0xfffd;
	const uint16_t *span;
	size_t length, span_length, used, written;

	length = 0;
	while (length < FSIMG_GPT_NAME_UNITS && units[length] != 0)
		length++;
	used = 0;
	for (size_t i = 0; i < length; i += span_length) {
		span = &units[i];
		span_length = 1;
		if (units[i] >= 0xd800 && units[i] <= 0xdbff && i + 1 < length &&
		    units[i + 1] >= 0xdc00 && units[i + 1] <= 0xdfff)
			span_length = 2;
		else if (units[i] >= 0xd800 && units[i] <= 0xdfff)
			span = &replacement;
		/* 36 units need at most 108 bytes, so the text always fits. */
		if (fsimg_utf16_to_utf8(span, span == &replacement ? 1 : span_length,
		    text + used, capacity - 1 - used, &written, NULL) != FSIMG_OK)
			break;
		used += written;
	}
	text[used] = '\0';
}

fsimg_status_t
fsimg_image_next_partition(fsimg_image_t *image, fsimg_partition_info_t *out,
    fsimg_error_t *error)
{
	const fsimg_partition_record_t *record;
	const char *type;
	fsimg_error_t local;
	fsimg_status_t status;

	if (error == NULL)
		error = &local;
	status = image_ready(image, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "missing partition output"));
	status = detect_scheme(image, error);
	if (status != FSIMG_OK)
		return (status);
	bzero(out, sizeof(*out));
	if (image->scheme == NULL || image->cursor >= image->table.count)
		return (FSIMG_DONE);

	record = &image->table.records[image->cursor];
	out->index = record->index;
	if (record->used) {
		out->used = true;
		out->offset = record->offset;
		out->length = record->length;
		type = fsimg_partition_type_name(image->table.kind, record);
		if (type != NULL)
			snprintf(out->type, sizeof(out->type), "%s", type);
		if (image->table.kind == FSIMG_SCHEME_GPT) {
			bcopy(record->type_guid, out->type_guid, sizeof(out->type_guid));
			bcopy(record->guid, out->guid, sizeof(out->guid));
			name_to_utf8(record->name, out->name, sizeof(out->name));
		} else {
			out->mbr_type = record->mbr_type;
			out->active = record->active;
		}
		status = claim_volume(image, record->offset, record->length,
		    &out->claim, out->format, error);
		if (status != FSIMG_OK)
			return (status);
	}
	image->cursor++;
	return (FSIMG_OK);
}

/* Return a slice and remember it as the selection, with the filesystem
 * its prefix claims.
 */
static fsimg_status_t
select_slice(fsimg_image_t *image, uint64_t offset, uint64_t length,
    uint32_t slot, fsimg_io_t **volume, fsimg_error_t *error)
{
	fsimg_claim_t claim;
	char format[FORMAT_NAME_BYTES];
	fsimg_status_t status;

	status = claim_volume(image, offset, length, &claim, format, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_io_read_slice(image->disk, offset, length, volume, error);
	if (status != FSIMG_OK)
		return (status);
	image->selected = slot;
	bcopy(format, image->selected_format, sizeof(format));
	return (FSIMG_OK);
}

static fsimg_status_t
select_record(fsimg_image_t *image, const fsimg_partition_record_t *record,
    fsimg_io_t **volume, fsimg_error_t *error)
{
	char message[96];

	if (record->extended) {
		snprintf(message, sizeof(message), "partition %u is an extended "
		    "partition; logical partitions are not supported",
		    (unsigned)record->index);
		return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
		    FSIMG_OP_IMAGE, message));
	}
	return (select_slice(image, record->offset, record->length,
	    record->index, volume, error));
}

/* A checked slice of the disk, never of the input file's own layout. */
static fsimg_status_t
select_range(fsimg_image_t *image, const fsimg_selector_t *selector,
    fsimg_io_t **volume, fsimg_error_t *error)
{
	uint64_t length;

	if (selector->offset > image->disk_bytes)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "the range starts past the end of the disk"));
	length = selector->length;
	if (length == 0)
		length = image->disk_bytes - selector->offset;
	else if (length > image->disk_bytes - selector->offset)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "the range ends past the end of the disk"));
	return (select_slice(image, selector->offset, length, 0, volume, error));
}

/* The one used partition whose prefix draws a filesystem claim, corrupt
 * and unsupported claims included so the reader reports the real reason.
 */
static fsimg_status_t
select_automatic(fsimg_image_t *image, fsimg_io_t **volume,
    fsimg_error_t *error)
{
	const fsimg_partition_record_t *record, *candidate;
	fsimg_claim_t claim;
	char format[FORMAT_NAME_BYTES];
	char message[sizeof(error->message)];
	size_t used;
	unsigned candidates;
	fsimg_status_t status;

	if (image->scheme == NULL)
		return (select_slice(image, 0, image->disk_bytes, 0, volume, error));
	candidate = NULL;
	candidates = 0;
	used = (size_t)snprintf(message, sizeof(message),
	    "several partitions hold a filesystem:");
	for (uint32_t i = 0; i < image->table.count; i++) {
		record = &image->table.records[i];
		if (!record->used)
			continue;
		status = claim_volume(image, record->offset, record->length,
		    &claim, format, error);
		if (status != FSIMG_OK)
			return (status);
		if (claim == FSIMG_CLAIM_NONE)
			continue;
		candidate = record;
		candidates++;
		if (used < sizeof(message))
			used += (size_t)snprintf(message + used, sizeof(message) - used,
			    "%s %u", candidates == 1 ? "" : ",",
			    (unsigned)record->index);
	}
	if (candidates == 0)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0, FSIMG_OP_IMAGE,
		    "no recognized filesystem in any partition"));
	if (candidates > 1) {
		fsimg_error_set(error, FSIMG_EFORMAT, 0, FSIMG_OP_IMAGE, message);
		error->message_truncated = used >= sizeof(message);
		return (FSIMG_EFORMAT);
	}
	return (select_record(image, candidate, volume, error));
}

static fsimg_status_t
find_by_index(fsimg_image_t *image, uint32_t index,
    const fsimg_partition_record_t **out, fsimg_error_t *error)
{
	if (index == 0 || index > image->table.count)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "the partition table has no slot of that number"));
	*out = &image->table.records[index - 1];
	if (!(*out)->used)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "the selected partition slot is unused"));
	return (FSIMG_OK);
}

/* GPT names compare as UTF-16 units, the stored name ending at its first
 * zero unit, without case folding or normalization. Exactly one used
 * entry must match.
 */
static fsimg_status_t
find_by_name(fsimg_image_t *image, const char *text,
    const fsimg_partition_record_t **out, fsimg_error_t *error)
{
	const fsimg_partition_record_t *record;
	uint16_t units[FSIMG_GPT_NAME_UNITS];
	size_t count, stored;
	unsigned matches;
	fsimg_status_t status;

	if (image->table.kind != FSIMG_SCHEME_GPT)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "partition names exist only in GPT"));
	status = fsimg_utf8_to_utf16(text, strlen(text), NULL, 0, &count, error);
	if (status != FSIMG_OK)
		return (status);
	/* A name longer than an entry holds matches nothing. */
	matches = 0;
	if (count <= FSIMG_GPT_NAME_UNITS) {
		status = fsimg_utf8_to_utf16(text, strlen(text), units,
		    FSIMG_GPT_NAME_UNITS, &count, error);
		if (status != FSIMG_OK)
			return (status);
		for (uint32_t i = 0; i < image->table.count; i++) {
			record = &image->table.records[i];
			if (!record->used)
				continue;
			stored = 0;
			while (stored < FSIMG_GPT_NAME_UNITS && record->name[stored] != 0)
				stored++;
			if (stored != count || bcmp(units, record->name,
			    count * sizeof(units[0])) != 0)
				continue;
			*out = record;
			matches++;
		}
	}
	if (matches == 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "no partition has that name"));
	if (matches > 1)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "several partitions have that name"));
	return (FSIMG_OK);
}

static fsimg_status_t
find_by_type(fsimg_image_t *image, const char *text,
    const fsimg_partition_record_t **out, fsimg_error_t *error)
{
	const fsimg_partition_record_t *record;
	fsimg_type_selector_t selector;
	unsigned matches;
	bool match;
	fsimg_status_t status;

	status = fsimg_partition_type_parse(text, &selector, error);
	if (status != FSIMG_OK)
		return (status);
	matches = 0;
	for (uint32_t i = 0; i < image->table.count; i++) {
		record = &image->table.records[i];
		if (!record->used)
			continue;
		status = fsimg_partition_type_match(&selector, image->table.kind,
		    record, &match, error);
		if (status != FSIMG_OK)
			return (status);
		if (!match)
			continue;
		*out = record;
		matches++;
	}
	if (matches == 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "no partition has that type"));
	if (matches > 1)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "several partitions have that type"));
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_image_select(fsimg_image_t *image, const fsimg_selector_t *selector,
    fsimg_io_t **volume, fsimg_error_t *error)
{
	const fsimg_partition_record_t *record;
	fsimg_error_t local;
	fsimg_status_t status;

	if (error == NULL)
		error = &local;
	if (volume != NULL)
		*volume = NULL;
	status = image_ready(image, error);
	if (status != FSIMG_OK)
		return (status);
	if (selector == NULL || volume == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "missing selector or volume output"));
	if (selector->kind == FSIMG_SELECT_RANGE)
		return (select_range(image, selector, volume, error));
	if (selector->kind == FSIMG_SELECT_DISK)
		return (select_slice(image, 0, image->disk_bytes, 0, volume, error));
	if (selector->kind > FSIMG_SELECT_DISK ||
	    ((selector->kind == FSIMG_SELECT_NAME ||
	    selector->kind == FSIMG_SELECT_TYPE) && selector->text == NULL))
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "invalid partition selector"));

	status = detect_scheme(image, error);
	if (status != FSIMG_OK)
		return (status);
	if (selector->kind == FSIMG_SELECT_AUTOMATIC)
		return (select_automatic(image, volume, error));
	if (image->scheme == NULL)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "the disk has no partition table"));
	switch (selector->kind) {
	case FSIMG_SELECT_INDEX:
		status = find_by_index(image, selector->index, &record, error);
		break;
	case FSIMG_SELECT_NAME:
		status = find_by_name(image, selector->text, &record, error);
		break;
	case FSIMG_SELECT_TYPE:
		status = find_by_type(image, selector->text, &record, error);
		break;
	default:
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "invalid partition selector"));
	}
	if (status != FSIMG_OK)
		return (status);
	return (select_record(image, record, volume, error));
}

/* Opening the table already performed every check reading needs; verify
 * adds the scheme's remaining checks and reports the scope reached.
 */
fsimg_status_t
fsimg_image_verify(fsimg_image_t *image, unsigned flags,
    fsimg_layer_report_t *out, fsimg_error_t *error)
{
	fsimg_scheme_input_t input;
	fsimg_error_t local;
	fsimg_status_t status;

	if (error == NULL)
		error = &local;
	status = image_ready(image, error);
	if (status != FSIMG_OK)
		return (status);
	if (out == NULL || (flags & ~FSIMG_VERIFY_PAYLOAD) != 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "missing layer report or unknown verification flags"));
	bzero(out, sizeof(*out));
	status = detect_scheme(image, error);
	if (status != FSIMG_OK)
		return (status);
	if (image->scheme != NULL) {
		scheme_input(image, &input);
		status = image->scheme->scheme->verify(&input, &image->table, error);
		if (status != FSIMG_OK)
			return (status);
		snprintf(out->scheme, sizeof(out->scheme), "%s", image->scheme->name);
		for (uint32_t i = 0; i < image->table.count; i++) {
			if (image->table.records[i].used)
				out->partitions++;
		}
	}
	/* No layer of this build has bytes beyond its tables to read through. */
	out->scope = FSIMG_VALIDATION_STRUCTURAL;
	out->complete = true;
	return (FSIMG_OK);
}
