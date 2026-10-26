/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "format.h"
#include "io.h"
#include "layer.h"
#include "unicode.h"

/*
 * An image is the input peeled down to its disk, with the disk's partition
 * table enumerated and nothing of the filesystem opened. Reading peels
 * from the outside in. The compression step decides, once, whether the
 * input is compressed, by running the compression probes on its prefix
 * together with every other probe that can run there; a compressed input
 * is decoded into the spool, and a stream, which cannot be read in place,
 * is copied there unchanged. The spool is then the file, and nothing later
 * sees how it was filled. The container step runs once, at open: the
 * container probes run on the file's prefix and its last 512 bytes, and a
 * claim's tables are validated and become the disk, a positional object of
 * the length they declare; without a claim the file is the disk. A
 * container whose payload is one unframed stream, such as hbcifs, decodes
 * it into the spool instead, which is why the open keeps an unused spool
 * through the container step; a file the spool already holds leaves no
 * spool for it. A container that carries a filesystem then reads as if the
 * stack named that filesystem with an empty scheme slot. Then one
 * step decides between a partition scheme and a bare volume by running the
 * scheme probes and the filesystem probes on the disk prefix together. In both
 * steps exactly one claim decides, so a boot sector that also resembles a
 * partition table is an ambiguity the caller resolves by forcing a slot,
 * never a guess.
 *
 * The scheme step runs at most once, on the first operation that needs
 * the table. A range or disk selection never needs it, so a damaged table
 * cannot block raw access to the disk's bytes.
 */
/* A format name with its terminator, as the public records hold it. */
#define FORMAT_NAME_BYTES	16
_Static_assert(sizeof(((fsimg_partition_info_t *)NULL)->format) ==
    FORMAT_NAME_BYTES, "a partition's format holds one format name");

struct fsimg_image {
	fsimg_ctx_t	*ctx;
	fsimg_io_t	*input;		/* The file: borrowed, or the spool's view */
	fsimg_io_t	*disk;		/* The file, or the container's disk */
	fsimg_io_t	*disk_view;	/* Owned final view, freed before backing */
	fsimg_session_t	 session;
	fsimg_stack_t	 forced;
	fsimg_layer_options_t layers;
	/* The compression the options force, NULL to detect one. */
	const fsimg_layer_t *forced_compression;
	/* The adopted spool, NULL once the container step proves it unused;
	 * spooled is its read-only view when it holds the file.
	 */
	fsimg_spool_t	*spool;
	fsimg_io_t	*spooled;
	const fsimg_layer_t *compression;	/* NULL for none */
	uint64_t	 file_bytes;
	uint64_t	 disk_bytes;
	/* The container the disk was peeled from, NULL for none; opened owns
	 * its disk object.
	 */
	const fsimg_layer_t *container;
	fsimg_container_t opened;
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

/* What one step's probes see: a prefix of the step's input, the input's
 * last bytes for a container probe, and the input's length.
 */
typedef struct probe_input {
	const unsigned char *prefix;
	size_t		 length;
	const unsigned char *tail;
	size_t		 tail_length;
	uint64_t	 bytes;
} probe_input_t;

/* The kinds of probe one survey runs, as a set of fsimg_layer_kind_t bits;
 * a survey holds one count per kind, indexed by kind.
 */
#define SURVEY(kind)	(1u << (kind))
#define SURVEY_KINDS	((unsigned)FSIMG_LAYER_COMPRESSION + 1)
_Static_assert(FSIMG_LAYER_COMPRESSION == FSIMG_STACK_SLOTS,
    "compression, the one kind that fills no slot, is the last kind");

static fsimg_status_t image_ready(fsimg_image_t *, fsimg_error_t *);
static fsimg_status_t open_image(fsimg_ctx_t *, fsimg_io_t *, int,
    const fsimg_image_options_t *, fsimg_image_t **, fsimg_error_t *);
static fsimg_status_t check_open(fsimg_ctx_t *, fsimg_io_t *,
    const fsimg_image_options_t *, const fsimg_layer_t **, fsimg_error_t *);
static bool explicit_stack(const fsimg_image_t *);
static fsimg_status_t read_first_block(fsimg_image_t *, int, size_t,
    unsigned char **, probe_input_t *, unsigned char *, fsimg_error_t *);
static fsimg_status_t decide_compression(fsimg_image_t *,
    const probe_input_t *, bool, fsimg_error_t *);
static fsimg_status_t peel_compression(fsimg_image_t *, int,
    fsimg_error_t *);
static void read_in_place(fsimg_image_t *);
static fsimg_status_t fill_spool(fsimg_image_t *, int, const unsigned char *,
    size_t, fsimg_error_t *);
static bool probing(const fsimg_layer_t *);
static size_t probe_bytes(const fsimg_layer_t *);
static size_t survey_bytes(fsimg_layer_kind_t, const fsimg_layer_t *, size_t);
static fsimg_status_t probe_layer(const fsimg_layer_t *, const probe_input_t *,
    fsimg_error_t *);
static fsimg_status_t survey(unsigned, const probe_input_t *, const char *,
    fsimg_survey_t *, fsimg_error_t *);
static const char *forced_filesystem(const fsimg_image_t *);
static fsimg_status_t validate_forced_stack(const fsimg_image_options_t *,
    const fsimg_layer_t *, fsimg_error_t *);
static fsimg_status_t read_prefix(fsimg_ctx_t *, fsimg_io_t *,
    fsimg_session_t *, uint64_t, uint64_t, size_t, unsigned char **, size_t *,
    fsimg_error_t *);
static fsimg_status_t claim_volume(fsimg_image_t *, uint64_t, uint64_t,
    fsimg_claim_t *, char [static FORMAT_NAME_BYTES], fsimg_error_t *);
static void container_input(fsimg_image_t *, fsimg_spool_t *,
    fsimg_container_input_t *);
static fsimg_status_t layer_failed(const fsimg_layer_t *, fsimg_status_t,
    fsimg_error_t *);
static fsimg_status_t identify_container(fsimg_image_t *,
    const fsimg_layer_t *, const fsimg_layer_t **, fsimg_error_t *);
static fsimg_status_t second_spool(const fsimg_image_t *,
    const fsimg_layer_t *, fsimg_error_t *);
static fsimg_status_t claim_container(fsimg_image_t *,
    const fsimg_layer_t **, fsimg_error_t *);
static void adopt_carried_stack(fsimg_image_t *, const fsimg_layer_t *);
static fsimg_status_t peel_container(fsimg_image_t *, fsimg_error_t *);
static fsimg_status_t identify_disk(fsimg_image_t *, const fsimg_layer_t *,
    const fsimg_layer_t **, fsimg_error_t *);
static fsimg_status_t detect_scheme(fsimg_image_t *, fsimg_error_t *);
static void scheme_input(fsimg_image_t *, fsimg_scheme_input_t *);
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
 * scheme and an implemented container. Of the layer settings reading uses
 * only gpt:sector, which forces the GPT LBA size and so requires gpt in
 * the scheme slot.
 */
static fsimg_status_t
validate_forced_stack(const fsimg_image_options_t *options,
    const fsimg_layer_t *compression, fsimg_error_t *error)
{
	const fsimg_stack_slot_t *slots = options->stack.slots;
	const fsimg_format_variant_t *variant;
	const fsimg_format_t *format;
	const fsimg_layer_t *scheme, *container;
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
	if (slots[FSIMG_LAYER_CONTAINER].state == FSIMG_SLOT_NAMED) {
		container = fsimg_layer_find(slots[FSIMG_LAYER_CONTAINER].name);
		if (container == NULL || container->container == NULL)
			return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
			    FSIMG_OP_IMAGE, "the named container is not supported"));
	}

	status = fsimg_layer_options_check(&options->stack, compression,
	    &options->layers, NULL, false, error);
	if (status != FSIMG_OK)
		return (status);
	sector = options->layers.gpt_sector_bytes;
	if (sector != 0 && sector != 512 && sector != 4096) {
		fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "gpt:sector must be 512 or 4096");
		fsimg_error_path(error, "gpt:sector");
		return (FSIMG_EINVAL);
	}
	return (FSIMG_OK);
}

/* Every layer default comes from its catalogue row. */
void
fsimg_layer_options_init(fsimg_layer_options_t *options)
{
	const fsimg_setting_targets_t targets = { .layers = options };
	const fsimg_layer_t *layer;

	bzero(options, sizeof(*options));
	for (size_t i = 0; (layer = fsimg_layer_at(i)) != NULL; i++) {
		if (layer->settings != NULL)
			fsimg_settings_init(layer->settings, &targets,
			    FSIMG_RECORD_LAYERS);
	}
}

void
fsimg_image_options_init(fsimg_image_options_t *options)
{
	bzero(options, sizeof(*options));
	options->size = sizeof(*options);
	options->version = FSIMG_OPTIONS_VERSION;
	fsimg_layer_options_init(&options->layers);
	options->spool_fd = -1;
}

fsimg_status_t
fsimg_image_open(fsimg_ctx_t *ctx, fsimg_io_t *input,
    const fsimg_image_options_t *options, fsimg_image_t **out,
    fsimg_error_t *error)
{
	return (open_image(ctx, input, -1, options, out, error));
}

fsimg_status_t
fsimg_image_open_stream(fsimg_ctx_t *ctx, int stream_fd,
    const fsimg_image_options_t *options, fsimg_image_t **out,
    fsimg_error_t *error)
{
	return (open_image(ctx, NULL, stream_fd, options, out, error));
}

/* What an open checks before it adopts the spool or reads a byte. */
static fsimg_status_t
check_open(fsimg_ctx_t *ctx, fsimg_io_t *file,
    const fsimg_image_options_t *options, const fsimg_layer_t **compression,
    fsimg_error_t *error)
{
	fsimg_status_t status;

	*compression = NULL;
	status = fsimg_ctx_ready(ctx, error);
	if (status != FSIMG_OK)
		return (status);
	if (file != NULL && fsimg_io_context(file) != ctx)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "image input belongs to another context"));
	if (options->flags != 0)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "unknown image option flags"));
	if (options->spool_fd < -1)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "invalid spool descriptor"));
	for (unsigned i = 0; i < 4; i++) {
		if (options->reserved[i] != 0)
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_IMAGE, "nonzero reserved image option"));
	}
	status = fsimg_compression_named(options->compression,
	    sizeof(options->compression), false, compression, error);
	if (status != FSIMG_OK)
		return (status);
	return (validate_forced_stack(options, *compression, error));
}

/*
 * Both opens: file is a seekable input, or NULL for the stream on
 * stream_fd. Once the options' size and version are valid, the spool
 * descriptor belongs to the open on every outcome: it is checked before
 * any input byte is read, closed when the open fails, and closed once the
 * container step leaves it unused.
 */
static fsimg_status_t
open_image(fsimg_ctx_t *ctx, fsimg_io_t *file, int stream_fd,
    const fsimg_image_options_t *options, fsimg_image_t **out,
    fsimg_error_t *error)
{
	fsimg_image_options_t defaults;
	const fsimg_layer_t *compression;
	fsimg_image_t *image;
	fsimg_spool_t *spool;
	fsimg_error_t local;
	fsimg_status_t status;

	if (error == NULL)
		error = &local;
	if (out != NULL)
		*out = NULL;
	if (options == NULL) {
		fsimg_image_options_init(&defaults);
		options = &defaults;
	}
	if (options->size != sizeof(*options) ||
	    options->version != FSIMG_OPTIONS_VERSION)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "invalid image options size or version"));
	status = check_open(ctx, file, options, &compression, error);
	if (status == FSIMG_OK &&
	    (out == NULL || (file == NULL && stream_fd < 0)))
		status = fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "image needs an input and an output");
	if (status != FSIMG_OK) {
		if (options->spool_fd > -1)
			close(options->spool_fd);
		return (status);
	}
	spool = NULL;
	if (options->spool_fd != -1) {
		status = fsimg_spool_adopt(ctx, options->spool_fd, &spool, error);
		if (status != FSIMG_OK)
			return (status);
	}

	image = fsimg_alloc(ctx, sizeof(*image), error);
	if (image == NULL) {
		fsimg_spool_free(spool);
		return (error->status);
	}
	bzero(image, sizeof(*image));
	image->ctx = ctx;
	image->input = file;
	image->forced = options->stack;
	image->layers = options->layers;
	image->forced_compression = compression;
	image->spool = spool;
	fsimg_session_init(&image->session, ctx, "image layers", 0);
	status = peel_compression(image, stream_fd, error);
	if (status == FSIMG_OK)
		status = peel_container(image, error);
	if (status == FSIMG_OK) {
		/* Publish one view of the interpreted disk. Decoding changes its
		 * coordinate system even when a container returns a zero-copy slice.
		 */
		if (image->compression != NULL || image->container != NULL)
			status = fsimg_io_decoded_view(image->disk,
			    &image->disk_view, error);
		else
			status = fsimg_io_read_slice(image->disk, 0,
			    image->disk_bytes, &image->disk_view, error);
		if (status == FSIMG_OK)
			image->disk = image->disk_view;
	}
	if (status != FSIMG_OK) {
		fsimg_image_free(image);
		return (status);
	}
	*out = image;
	return (FSIMG_OK);
}

void
fsimg_image_free(fsimg_image_t *image)
{
	if (image == NULL || image->ctx->in_callback)
		return;
	fsimg_io_free(image->disk_view);
	fsimg_io_free(image->opened.disk);
	fsimg_io_free(image->spooled);
	fsimg_spool_free(image->spool);
	fsimg_dealloc(image->table.records);
	fsimg_dealloc(image);
}

/* Whether the caller named or emptied any slot, which resolves a
 * compression-step ambiguity as uncompressed.
 */
static bool
explicit_stack(const fsimg_image_t *image)
{
	for (unsigned i = 0; i < FSIMG_STACK_SLOTS; i++) {
		if (image->forced.slots[i].state != FSIMG_SLOT_UNSPECIFIED)
			return (true);
	}
	return (false);
}

/*
 * The first block of the input, which every compression-step probe sees:
 * from a seekable file its prefix, its last bytes and its length; from a
 * stream as much as it gives up to want bytes, with no tail and an
 * unknown length (UINT64_MAX), which lets a length check pass rather than
 * hide a claim. *block holds the prefix for the caller to free.
 */
static fsimg_status_t
read_first_block(fsimg_image_t *image, int stream_fd, size_t want,
    unsigned char **block, probe_input_t *input, unsigned char *tail,
    fsimg_error_t *error)
{
	fsimg_spool_input_t stream = { .ctx = image->ctx, .fd = stream_fd };
	size_t count, transferred;
	fsimg_status_t status;

	bzero(input, sizeof(*input));
	*block = NULL;
	if (image->input != NULL) {
		input->tail = tail;
		input->tail_length = image->file_bytes < FSIMG_CONTAINER_TAIL_BYTES ?
		    (size_t)image->file_bytes : FSIMG_CONTAINER_TAIL_BYTES;
		input->bytes = image->file_bytes;
		status = fsimg_io_read_session(image->input, &image->session,
		    image->file_bytes - input->tail_length, tail,
		    input->tail_length, &transferred, error);
		if (status == FSIMG_OK)
			status = read_prefix(image->ctx, image->input,
			    &image->session, 0, image->file_bytes, want, block,
			    &input->length, error);
		input->prefix = *block;
		return (status);
	}

	input->bytes = UINT64_MAX;
	*block = fsimg_alloc(image->ctx, want, error);
	if (*block == NULL)
		return (error->status);
	input->prefix = *block;
	do {
		status = fsimg_spool_input_read(&stream, *block + input->length,
		    want - input->length, &count, error);
		input->length += count;
	} while (status == FSIMG_OK && count != 0 && input->length < want);
	return (status);
}

/*
 * Step 1's decision (specs/design-layers.md#peeling-order), made here and
 * nowhere else. A forced compression runs only its probe, which must
 * claim.
 * Otherwise the compression probes run with the scheme and filesystem
 * probes and, on a seekable file, the container probes: a compression
 * claiming alone decides compressed; with any other claim the input is
 * ambiguous unless the caller gave a stack, which resolves it as
 * uncompressed; no compression claim decides uncompressed. Other claims
 * decide nothing further: their own steps run later on the file.
 */
static fsimg_status_t
decide_compression(fsimg_image_t *image, const probe_input_t *input,
    bool seekable, fsimg_error_t *error)
{
	const fsimg_layer_t *forced = image->forced_compression;
	fsimg_survey_t counts[SURVEY_KINDS], others[SURVEY_KINDS];
	const fsimg_survey_t *compressions = &counts[FSIMG_LAYER_COMPRESSION];
	unsigned claims, kinds;
	fsimg_status_t status;

	if (forced != NULL) {
		status = probe_layer(forced, input, error);
		if (status == FSIMG_OK)
			image->compression = forced;
		return (layer_failed(forced, status, error));
	}
	status = survey(SURVEY(FSIMG_LAYER_COMPRESSION), input, NULL, counts,
	    error);
	if (status != FSIMG_OK || compressions->claims == 0)
		return (status);
	/* A compression claimed, so every other claim here makes it
	 * ambiguous.
	 */
	kinds = SURVEY(FSIMG_LAYER_SCHEME) | SURVEY(FSIMG_LAYER_FILESYSTEM) |
	    (seekable ? SURVEY(FSIMG_LAYER_CONTAINER) : 0);
	status = survey(kinds, input, NULL, others, error);
	if (status != FSIMG_OK)
		return (status);
	claims = compressions->claims;
	for (unsigned kind = 0; kind < SURVEY_KINDS; kind++)
		claims += others[kind].claims;
	if (claims > 1) {
		if (explicit_stack(image))
			return (FSIMG_OK);
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0, FSIMG_OP_IMAGE,
		    "the input passes for both a compressed and an uncompressed "
		    "image; force the compression or a stack"));
	}
	if (compressions->status != FSIMG_OK) {
		*error = compressions->reason;
		return (layer_failed(compressions->layer, compressions->status,
		    error));
	}
	image->compression = compressions->layer;
	return (FSIMG_OK);
}

/*
 * The compression step. A seekable file that is not compressed is read in
 * place; anything else becomes the spool. Without a spool, a stream fails
 * as a spool limit of zero before a byte is read, and a compressed file
 * once the probes have named it.
 */
static fsimg_status_t
peel_compression(fsimg_image_t *image, int stream_fd, fsimg_error_t *error)
{
	unsigned char tail[FSIMG_CONTAINER_TAIL_BYTES], *block;
	probe_input_t input;
	size_t want;
	bool seekable;
	fsimg_status_t status;

	seekable = image->input != NULL;
	if (!seekable && image->spool == NULL)
		return (fsimg_limit_error(error, FSIMG_RESOURCE_SPOOL, 0, 0));
	if (seekable) {
		status = fsimg_io_get_size(image->input, &image->file_bytes,
		    error);
		if (status != FSIMG_OK)
			return (status);
	}
	/* The first block serves every probe that runs on it. */
	want = fsimg_format_probe_bytes(NULL);
	want = survey_bytes(FSIMG_LAYER_CONTAINER, NULL, want);
	want = survey_bytes(FSIMG_LAYER_SCHEME, NULL, want);
	want = survey_bytes(FSIMG_LAYER_COMPRESSION, NULL, want);
	status = read_first_block(image, stream_fd, want, &block, &input, tail,
	    error);
	if (status == FSIMG_OK)
		status = decide_compression(image, &input, seekable, error);
	if (status == FSIMG_OK) {
		if (seekable && image->compression == NULL)
			read_in_place(image);
		else
			status = fill_spool(image, stream_fd, block, input.length,
			    error);
	}
	fsimg_dealloc(block);
	return (status);
}

/* The file is the input as it is; the spool waits for the container step. */
static void
read_in_place(fsimg_image_t *image)
{
	image->disk = image->input;
	image->disk_bytes = image->file_bytes;
}

/*
 * Fill the spool, starting with the block the probes read: decoded when
 * the input is compressed, else (a stream) copied unchanged. The sealed
 * spool then replaces the input as the file the later steps read.
 */
static fsimg_status_t
fill_spool(fsimg_image_t *image, int stream_fd, const unsigned char *block,
    size_t block_length, fsimg_error_t *error)
{
	fsimg_spool_input_t source = { .ctx = image->ctx, .prefix = block,
	    .prefix_length = block_length, .file = image->input,
	    .session = &image->session, .offset = block_length,
	    .file_bytes = image->file_bytes, .fd = stream_fd };
	const fsimg_layer_t *compression = image->compression;
	fsimg_status_t status;

	if (image->spool == NULL)
		return (fsimg_limit_error(error, FSIMG_RESOURCE_SPOOL, 0, 0));
	if (compression != NULL)
		status = layer_failed(compression,
		    compression->compression->decode(&source, image->spool,
		    error), error);
	else
		status = fsimg_spool_fill(image->spool, &source, error);
	if (status == FSIMG_OK)
		status = fsimg_spool_seal(image->spool, &image->spooled, error);
	if (status == FSIMG_OK)
		status = fsimg_io_get_size(image->spooled, &image->file_bytes,
		    error);
	if (status != FSIMG_OK)
		return (status);
	image->input = image->spooled;
	image->disk = image->spooled;
	image->disk_bytes = image->file_bytes;
	return (FSIMG_OK);
}

/* Read up to want bytes at an offset, cut to the length available there,
 * into a buffer charged to the context. Zero bytes need no buffer.
 */
static fsimg_status_t
read_prefix(fsimg_ctx_t *ctx, fsimg_io_t *io, fsimg_session_t *session,
    uint64_t offset, uint64_t available, size_t want, unsigned char **prefix,
    size_t *length, fsimg_error_t *error)
{
	size_t transferred;
	fsimg_status_t status;

	*prefix = NULL;
	*length = available < want ? (size_t)available : want;
	if (*length == 0)
		return (FSIMG_OK);
	*prefix = fsimg_alloc(ctx, *length, error);
	if (*prefix == NULL)
		return (error->status);
	status = fsimg_io_read_session(io, session, offset, *prefix, *length,
	    &transferred, error);
	if (status != FSIMG_OK) {
		fsimg_dealloc(*prefix);
		*prefix = NULL;
	}
	return (status);
}

static fsimg_status_t
claim_volume(fsimg_image_t *image, uint64_t offset, uint64_t length,
    fsimg_claim_t *claim, char format[static FORMAT_NAME_BYTES],
    fsimg_error_t *error)
{
	return (fsimg_volume_claim(image->ctx, image->disk, &image->session,
	    offset, length, forced_filesystem(image), claim, format, error));
}

fsimg_status_t
fsimg_volume_claim(fsimg_ctx_t *ctx, fsimg_io_t *io, fsimg_session_t *session,
    uint64_t offset, uint64_t length, const char *forced, fsimg_claim_t *claim,
    char format[static FORMAT_NAME_BYTES], fsimg_error_t *error)
{
	fsimg_survey_t counts[SURVEY_KINDS];
	const fsimg_survey_t *filesystems = &counts[FSIMG_LAYER_FILESYSTEM];
	unsigned char *prefix;
	probe_input_t input;
	fsimg_status_t status;

	*claim = FSIMG_CLAIM_NONE;
	format[0] = '\0';
	bzero(&input, sizeof(input));
	input.bytes = length;
	status = read_prefix(ctx, io, session, offset, length,
	    fsimg_format_probe_bytes(forced), &prefix, &input.length, error);
	if (status != FSIMG_OK)
		return (status);
	input.prefix = prefix;
	status = survey(SURVEY(FSIMG_LAYER_FILESYSTEM), &input, forced, counts,
	    error);
	fsimg_dealloc(prefix);
	if (status != FSIMG_OK)
		return (status);
	*claim = fsimg_survey_claim(filesystems);
	if (*claim == FSIMG_CLAIM_FORMAT)
		snprintf(format, FORMAT_NAME_BYTES, "%s",
		    filesystems->variant->info.name);
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_volume_byte_order(fsimg_ctx_t *ctx, fsimg_io_t *io,
    fsimg_session_t *session, uint64_t length, const char *name,
    fsimg_byte_order_t *order, fsimg_error_t *error)
{
	const fsimg_format_variant_t *variant;
	const fsimg_format_t *format;
	unsigned char *prefix;
	size_t prefix_length;
	fsimg_status_t status;

	*order = FSIMG_BYTE_ORDER_LITTLE;
	format = fsimg_format_find(name, &variant);
	if (format == NULL || format->byte_order == NULL)
		return (FSIMG_OK);
	status = read_prefix(ctx, io, session, 0, length, format->probe_bytes,
	    &prefix, &prefix_length, error);
	if (status != FSIMG_OK)
		return (status);
	status = format->byte_order(prefix, prefix_length, order, error);
	fsimg_dealloc(prefix);
	return (status);
}

/* What a container reads; spool is NULL except for a spooled open. */
static void
container_input(fsimg_image_t *image, fsimg_spool_t *spool,
    fsimg_container_input_t *input)
{
	input->ctx = image->ctx;
	input->file = image->input;
	input->session = &image->session;
	input->spool = spool;
	input->file_bytes = image->file_bytes;
}

/* Whether a layer runs a probe at the step of its kind: a scheme, a
 * container or a compression this build implements.
 */
static bool
probing(const fsimg_layer_t *layer)
{
	switch (layer->kind) {
	case FSIMG_LAYER_SCHEME:
		return (layer->scheme != NULL);
	case FSIMG_LAYER_CONTAINER:
		return (layer->container != NULL);
	case FSIMG_LAYER_COMPRESSION:
		return (layer->compression != NULL);
	default:
		return (false);
	}
}

/* A probing layer's probe length. */
static size_t
probe_bytes(const fsimg_layer_t *layer)
{
	switch (layer->kind) {
	case FSIMG_LAYER_SCHEME:
		return (layer->scheme->probe_bytes);
	case FSIMG_LAYER_CONTAINER:
		return (layer->container->probe_bytes);
	case FSIMG_LAYER_COMPRESSION:
		return (layer->compression->probe_bytes);
	default:
		return (0);
	}
}

/* The prefix one step reads: the forced layer's probe length, or the
 * longest of the kind's, but at least least bytes.
 */
static size_t
survey_bytes(fsimg_layer_kind_t kind, const fsimg_layer_t *forced,
    size_t least)
{
	const fsimg_layer_t *layer;
	size_t want = least;

	if (forced != NULL)
		return (probe_bytes(forced));
	for (size_t i = 0; (layer = fsimg_layer_at(i)) != NULL; i++) {
		if (layer->kind == kind && probing(layer) &&
		    probe_bytes(layer) > want)
			want = probe_bytes(layer);
	}
	return (want);
}

/* One layer's probe on its share of the prefix. */
static fsimg_status_t
probe_layer(const fsimg_layer_t *layer, const probe_input_t *input,
    fsimg_error_t *error)
{
	size_t length;

	length = input->length < probe_bytes(layer) ? input->length :
	    probe_bytes(layer);
	if (layer->kind == FSIMG_LAYER_SCHEME)
		return (layer->scheme->probe(input->prefix, length, input->bytes,
		    error));
	if (layer->kind == FSIMG_LAYER_COMPRESSION)
		return (layer->compression->probe(input->prefix, length, error));
	return (layer->container->probe(input->prefix, length, input->tail,
	    input->tail_length, input->bytes, error));
}

/*
 * One survey over both registries: every probe of the kinds in the set,
 * the layer registry's schemes, containers and compressions and the
 * format registry's filesystems (only forced_filesystem's when one is
 * named), each kind counted in counts[kind] under the one claim rule
 * (fsimg_survey_count). Each step reads its outcome from the counts.
 */
static fsimg_status_t
survey(unsigned kinds, const probe_input_t *input,
    const char *forced_filesystem, fsimg_survey_t *counts,
    fsimg_error_t *error)
{
	const fsimg_layer_t *layer;
	fsimg_error_t detail;
	fsimg_status_t status;

	for (unsigned kind = 0; kind < SURVEY_KINDS; kind++)
		fsimg_survey_init(&counts[kind]);
	for (size_t i = 0; (layer = fsimg_layer_at(i)) != NULL; i++) {
		if ((kinds & SURVEY(layer->kind)) == 0 || !probing(layer))
			continue;
		fsimg_error_clear(&detail);
		status = probe_layer(layer, input, &detail);
		if (fsimg_survey_count(&counts[layer->kind], status, &detail))
			counts[layer->kind].layer = layer;
	}
	if ((kinds & SURVEY(FSIMG_LAYER_FILESYSTEM)) == 0)
		return (FSIMG_OK);
	return (fsimg_format_identify(forced_filesystem, input->prefix,
	    input->length, input->bytes, &counts[FSIMG_LAYER_FILESYSTEM],
	    error));
}

/*
 * The container step's probes, run on one prefix and the last 512 bytes
 * of the file: the forced container's alone, or every container's. On
 * success *container is the container that claimed, or NULL when none did
 * and the file is the disk. Several claims are an ambiguity; a claim that
 * is corrupt or unsupported fails with its probe's reason, naming the
 * claimant, never falling back to a raw disk. The scheme and filesystem
 * probes do not run here: they read the disk this step yields.
 */
static fsimg_status_t
identify_container(fsimg_image_t *image, const fsimg_layer_t *forced,
    const fsimg_layer_t **container, fsimg_error_t *error)
{
	unsigned char tail[FSIMG_CONTAINER_TAIL_BYTES], *prefix;
	fsimg_survey_t counts[SURVEY_KINDS];
	const fsimg_survey_t *containers = &counts[FSIMG_LAYER_CONTAINER];
	probe_input_t input;
	size_t transferred;
	fsimg_status_t status;

	*container = NULL;
	input.tail = tail;
	input.tail_length = image->file_bytes < sizeof(tail) ?
	    (size_t)image->file_bytes : sizeof(tail);
	input.bytes = image->file_bytes;
	status = fsimg_io_read_session(image->input, &image->session,
	    image->file_bytes - input.tail_length, tail, input.tail_length,
	    &transferred, error);
	if (status != FSIMG_OK)
		return (status);
	status = read_prefix(image->ctx, image->input, &image->session, 0,
	    image->file_bytes, survey_bytes(FSIMG_LAYER_CONTAINER, forced, 0),
	    &prefix, &input.length, error);
	if (status != FSIMG_OK)
		return (status);
	input.prefix = prefix;
	if (forced != NULL) {
		status = probe_layer(forced, &input, error);
		fsimg_dealloc(prefix);
		if (status == FSIMG_OK)
			*container = forced;
		return (status);
	}
	status = survey(SURVEY(FSIMG_LAYER_CONTAINER), &input, NULL, counts,
	    error);
	fsimg_dealloc(prefix);
	if (status != FSIMG_OK)
		return (status);
	switch (fsimg_survey_claim(containers)) {
	case FSIMG_CLAIM_NONE:
		return (FSIMG_OK);
	case FSIMG_CLAIM_AMBIGUOUS:
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0, FSIMG_OP_IMAGE,
		    "the file matches several containers; force one"));
	case FSIMG_CLAIM_FORMAT:
		*container = containers->layer;
		return (FSIMG_OK);
	default:
		*error = containers->reason;
		return (layer_failed(containers->layer, containers->status,
		    error));
	}
}

/*
 * A layer's own failure names the layer in error.path, so a caller can
 * attribute it, as cW does, without parsing the message. Failures of the
 * image object itself, a limit or an I/O error keep their own path.
 */
static fsimg_status_t
layer_failed(const fsimg_layer_t *layer, fsimg_status_t status,
    fsimg_error_t *error)
{
	if (status != FSIMG_OK && layer != NULL &&
	    error->operation == FSIMG_OP_IMAGE && error->path[0] == '\0')
		snprintf(error->path, sizeof(error->path), "%s", layer->name);
	return (status);
}

/*
 * An image has one spool. When it already holds the file, decoded from
 * gzip or copied from a stream, a container that decodes into the spool
 * would need a second one: FSIMG_EUNSUPPORTED naming both layers
 * (specs/design-layers.md#spool-backed-peel).
 */
static fsimg_status_t
second_spool(const fsimg_image_t *image, const fsimg_layer_t *container,
    fsimg_error_t *error)
{
	char message[sizeof(error->message)];

	if (!container->container->spooled || image->spooled == NULL)
		return (FSIMG_OK);
	if (image->compression != NULL)
		snprintf(message, sizeof(message), "the %s container inside %s "
		    "needs a second spool; decompress the image first",
		    container->name, image->compression->name);
	else
		snprintf(message, sizeof(message), "the %s container on a stream "
		    "needs a second spool; read the image from a file",
		    container->name);
	return (fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0, FSIMG_OP_IMAGE,
	    message));
}

/*
 * The container step's claimant in *container: NULL when the container
 * slot is forced empty, nothing claims, or the claim fails, the failure
 * then naming the layer it blames. A claimant that carries a
 * filesystem must agree with the forced slots, and one that decodes into
 * the spool must find it free; both are refused before its payload is
 * read.
 */
static fsimg_status_t
claim_container(fsimg_image_t *image, const fsimg_layer_t **container,
    fsimg_error_t *error)
{
	const fsimg_stack_slot_t *slot;
	const fsimg_layer_t *forced;
	fsimg_status_t status;

	*container = NULL;
	slot = &image->forced.slots[FSIMG_LAYER_CONTAINER];
	if (slot->state == FSIMG_SLOT_EMPTY)
		return (FSIMG_OK);
	forced = slot->state == FSIMG_SLOT_NAMED ? fsimg_layer_find(slot->name) :
	    NULL;
	status = identify_container(image, forced, container, error);
	if (status != FSIMG_OK) {
		*container = NULL;
		return (layer_failed(forced, status, error));
	}
	if (*container == NULL)
		return (FSIMG_OK);
	status = fsimg_stack_carried(&image->forced, *container, error);
	if (status == FSIMG_OK)
		status = second_spool(image, *container, error);
	if (status != FSIMG_OK) {
		layer_failed(*container, status, error);
		*container = NULL;
	}
	return (status);
}

/*
 * After a container that carries a filesystem opens, the forced slots
 * name that filesystem and an empty scheme, so the next step runs that
 * filesystem's probe alone. claim_container checked they agree.
 */
static void
adopt_carried_stack(fsimg_image_t *image, const fsimg_layer_t *container)
{
	fsimg_stack_slot_t *slots = image->forced.slots;

	slots[FSIMG_LAYER_FILESYSTEM].state = FSIMG_SLOT_NAMED;
	snprintf(slots[FSIMG_LAYER_FILESYSTEM].name,
	    sizeof(slots[FSIMG_LAYER_FILESYSTEM].name), "%s",
	    container->container->carries);
	bzero(&slots[FSIMG_LAYER_SCHEME], sizeof(slots[FSIMG_LAYER_SCHEME]));
	slots[FSIMG_LAYER_SCHEME].state = FSIMG_SLOT_EMPTY;
}

/*
 * Peel the container, once, at open. With no claimant the file is the
 * disk; otherwise the claimant validates its tables and yields the disk
 * every later step reads. Only a spooled claimant's open is given the
 * spool. Afterwards, whatever the outcome, the spool is closed unless it
 * holds the file or the opened container's disk.
 */
static fsimg_status_t
peel_container(fsimg_image_t *image, fsimg_error_t *error)
{
	const fsimg_layer_t *container;
	fsimg_container_input_t input;
	fsimg_status_t status;
	bool spooled, keep;

	status = claim_container(image, &container, error);
	spooled = container != NULL && container->container->spooled;
	if (container != NULL) {
		container_input(image, spooled ? image->spool : NULL, &input);
		status = layer_failed(container,
		    container->container->open(&input, &image->opened, error),
		    error);
	}
	keep = image->spooled != NULL || (status == FSIMG_OK && spooled);
	if (!keep) {
		fsimg_spool_free(image->spool);
		image->spool = NULL;
	}
	if (status != FSIMG_OK || container == NULL)
		return (status);
	image->container = container;
	image->disk = image->opened.disk;
	image->disk_bytes = image->opened.disk_bytes;
	if (container->container->carries != NULL)
		adopt_carried_stack(image, container);
	return (FSIMG_OK);
}

/*
 * The scheme step's probes, run on one prefix of the disk: the forced
 * scheme's alone, or every scheme's together with the filesystem probes.
 * On success *scheme is the scheme that claimed, or NULL for a bare volume.
 * Unforced, exactly one claim decides; none is an unrecognized disk and
 * several are an ambiguity. A scheme claim that is corrupt or unsupported
 * fails with its probe's reason, naming the scheme, never falling back
 * to a bare volume.
 */
static fsimg_status_t
identify_disk(fsimg_image_t *image, const fsimg_layer_t *forced,
    const fsimg_layer_t **scheme, fsimg_error_t *error)
{
	fsimg_survey_t counts[SURVEY_KINDS];
	const fsimg_survey_t *schemes = &counts[FSIMG_LAYER_SCHEME];
	const fsimg_survey_t *filesystems = &counts[FSIMG_LAYER_FILESYSTEM];
	probe_input_t input;
	unsigned char *prefix;
	fsimg_status_t status;

	*scheme = NULL;
	input.tail = NULL;
	input.tail_length = 0;
	input.bytes = image->disk_bytes;
	status = read_prefix(image->ctx, image->disk, &image->session, 0,
	    image->disk_bytes, survey_bytes(FSIMG_LAYER_SCHEME, forced,
	    fsimg_format_probe_bytes(forced_filesystem(image))), &prefix,
	    &input.length, error);
	if (status != FSIMG_OK)
		return (status);
	input.prefix = prefix;
	if (forced != NULL) {
		status = probe_layer(forced, &input, error);
		fsimg_dealloc(prefix);
		if (status == FSIMG_OK)
			*scheme = forced;
		return (status);
	}
	status = survey(SURVEY(FSIMG_LAYER_SCHEME) |
	    SURVEY(FSIMG_LAYER_FILESYSTEM), &input, forced_filesystem(image),
	    counts, error);
	fsimg_dealloc(prefix);
	if (status != FSIMG_OK)
		return (status);

	/* Nothing claimed; both kinds claimed; several schemes claimed; one
	 * scheme claimed but its table is damaged or unsupported. Otherwise
	 * one scheme claimed, or the filesystems (one, or several left for the
	 * reader to report) make it a bare volume.
	 */
	if (schemes->claims + filesystems->claims == 0)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0, FSIMG_OP_IMAGE,
		    "no partition table or filesystem recognized"));
	if (schemes->claims != 0 && filesystems->claims != 0)
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0, FSIMG_OP_IMAGE,
		    "the disk matches both a partition table and a filesystem; "
		    "force the scheme slot"));
	switch (fsimg_survey_claim(schemes)) {
	case FSIMG_CLAIM_NONE:
		return (FSIMG_OK);
	case FSIMG_CLAIM_AMBIGUOUS:
		return (fsimg_error_set(error, FSIMG_EFORMAT, 0, FSIMG_OP_IMAGE,
		    "the disk matches several partition schemes; force one"));
	case FSIMG_CLAIM_FORMAT:
		*scheme = schemes->layer;
		return (FSIMG_OK);
	default:
		*error = schemes->reason;
		return (layer_failed(schemes->layer, schemes->status, error));
	}
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
	uint64_t logical_sector;
	char message[128];

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
		return (layer_failed(forced, status, error));
	if (scheme != NULL) {
		scheme_input(image, &input);
		status = scheme->scheme->open(&input, &image->table, error);
		if (status != FSIMG_OK)
			return (layer_failed(scheme, status, error));
		logical_sector = fsimg_io_logical_sector(image->disk);
		if (logical_sector != 0 &&
		    logical_sector != image->table.sector_bytes) {
			snprintf(message, sizeof(message), "partition sector size %u "
			    "differs from device logical sector size %llu",
			    image->table.sector_bytes,
			    (unsigned long long)logical_sector);
			fsimg_dealloc(image->table.records);
			bzero(&image->table, sizeof(image->table));
			status = fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0,
			    FSIMG_OP_IMAGE, message);
			return (layer_failed(scheme, status, error));
		}
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
	if (image->compression != NULL)
		snprintf(out->compression, sizeof(out->compression), "%s",
		    image->compression->name);
	out->file_bytes = image->file_bytes;
	stack.slots[FSIMG_LAYER_FILESYSTEM] =
	    image->forced.slots[FSIMG_LAYER_FILESYSTEM];
	if (image->container != NULL) {
		stack.slots[FSIMG_LAYER_CONTAINER].state = FSIMG_SLOT_NAMED;
		snprintf(stack.slots[FSIMG_LAYER_CONTAINER].name,
		    sizeof(stack.slots[FSIMG_LAYER_CONTAINER].name), "%s",
		    image->container->name);
		snprintf(out->container, sizeof(out->container), "%s",
		    image->container->name);
		/* A spooled container's disk is one stream in its codec. */
		if (image->container->container->spooled)
			snprintf(out->codecs, sizeof(out->codecs), "%s",
			    image->container->container->codecs);
		else
			fsimg_chunk_kinds_text(image->opened.kinds,
			    image->opened.codecs, out->codecs, sizeof(out->codecs));
		bcopy(image->opened.guid, out->container_guid,
		    sizeof(out->container_guid));
		out->chunk_count = image->opened.chunks;
		out->detail_count = image->opened.detail_count;
		bcopy(image->opened.details, out->details,
		    sizeof(out->details));
	}
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

fsimg_status_t
fsimg_image_next_partition(fsimg_image_t *image, fsimg_partition_info_t *out,
    fsimg_error_t *error)
{
	const fsimg_partition_record_t *record;
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
	fsimg_partition_report(image->table.kind, record, out);
	if (record->used) {
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
	const fsimg_partition_record_t *record = NULL;
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
	/* A section is no volume; only unwrap writes one. */
	if (selector->kind == FSIMG_SELECT_SECTION)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "a section selector is accepted only by unwrap"));
	if (selector->kind > FSIMG_SELECT_SECTION ||
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

/*
 * A read-only slice of the file over one of the opened container's
 * sections, which the container locates and checks against the file. A
 * name its file settings do not declare, including any name for an image
 * without a container, is FSIMG_EINVAL naming it.
 */
fsimg_status_t
fsimg_image_section(fsimg_image_t *image, const char *name,
    fsimg_io_t **section, fsimg_error_t *error)
{
	fsimg_container_input_t input;
	fsimg_file_setting_t setting;
	uint64_t offset, length;
	fsimg_status_t status;

	*section = NULL;
	status = image_ready(image, error);
	if (status != FSIMG_OK)
		return (status);
	if (name == NULL || !fsimg_layer_section(image->container, name,
	    &setting) || image->container->container->section == NULL) {
		fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "the container has no section of that name");
		if (name != NULL)
			fsimg_error_path(error, name);
		return (FSIMG_EINVAL);
	}
	container_input(image, NULL, &input);
	status = image->container->container->section(&input, setting, &offset,
	    &length, error);
	if (status != FSIMG_OK)
		return (status);
	return (fsimg_io_read_slice(image->input, offset, length, section,
	    error));
}

/* Opening the container and the table already performed every check
 * reading needs; verify adds the passes that read beyond the tables, the
 * container's first, and reports the scope reached. A container's fields
 * that no evidence gives a rule are named, and the report is then
 * incomplete although every check passed.
 */
fsimg_status_t
fsimg_image_verify(fsimg_image_t *image, unsigned flags,
    fsimg_layer_report_t *out, fsimg_error_t *error)
{
	fsimg_container_input_t container;
	fsimg_scheme_input_t input;
	fsimg_error_t local;
	bool payload;
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
	payload = (flags & FSIMG_VERIFY_PAYLOAD) != 0;
	/* Every member was checked as the spool filled. */
	if (image->compression != NULL)
		snprintf(out->compression, sizeof(out->compression), "%s",
		    image->compression->name);
	if (image->container != NULL) {
		container_input(image, NULL, &container);
		status = image->container->container->verify(&container,
		    &image->opened, payload, error);
		if (status != FSIMG_OK)
			return (layer_failed(image->container, status, error));
		snprintf(out->container, sizeof(out->container), "%s",
		    image->container->name);
		if (image->container->container->unverified != NULL)
			snprintf(out->unverified, sizeof(out->unverified), "%s",
			    image->container->container->unverified);
	}
	status = detect_scheme(image, error);
	if (status != FSIMG_OK)
		return (status);
	if (image->scheme != NULL) {
		scheme_input(image, &input);
		status = image->scheme->scheme->verify(&input, &image->table, error);
		if (status != FSIMG_OK)
			return (layer_failed(image->scheme, status, error));
		snprintf(out->scheme, sizeof(out->scheme), "%s", image->scheme->name);
		for (uint32_t i = 0; i < image->table.count; i++) {
			if (image->table.records[i].used)
				out->partitions++;
		}
	}
	/* Only a container whose checksums cover its decoded bytes has a
	 * payload to read through.
	 */
	out->scope = FSIMG_VALIDATION_STRUCTURAL;
	if (payload && image->container != NULL &&
	    image->container->container->payload_checks)
		out->scope = FSIMG_VALIDATION_PAYLOAD;
	out->complete = out->unverified[0] == '\0';
	return (FSIMG_OK);
}
