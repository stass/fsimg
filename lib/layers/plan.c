/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <stdio.h>
#include <strings.h>

#include "format.h"
#include "io.h"
#include "layer.h"
#include "source.h"

/*
 * The disk plan: what a writer, wrap or compose puts around its volumes.
 * The three reach the same scheme operations, which resolve the scheme's
 * settings, identities and partition metadata
 * (specs/design-scheme-planning.md), so they share one rule for each
 * default, identifier and limit. Placement knows only the scheme's sector
 * size and table footprint, and lays partitions out as sgdisk and fdisk
 * do: each at the first 1 MiB boundary after the
 * scheme's leading tables or the previous partition, whole sectors long,
 * with the trailing tables at the end of the disk. A container rounds the
 * disk up to its granularity first, so those tables end the disk it
 * declares, and states the bound of the output it will encode; a
 * compression states the bound of its encoding of that. A filesystem that
 * must fill its slot exactly refuses a scheme or a padding container.
 */
#define PARTITION_ALIGNMENT	(UINT64_C(1) << 20)

static bool round_up(uint64_t, uint64_t, uint64_t *);
static void partition_key(const fsimg_scheme_partition_args_t *,
    const char *, char *, size_t);
static fsimg_status_t footprint_bytes(const fsimg_disk_plan_t *, uint64_t *,
    uint64_t *, fsimg_error_t *);
static fsimg_status_t resolve_container(fsimg_ctx_t *, fsimg_disk_plan_t *,
    const fsimg_disk_request_t *, fsimg_error_t *);
static fsimg_status_t record_inputs(fsimg_ctx_t *, fsimg_disk_plan_t *,
    const fsimg_disk_request_t *, fsimg_error_t *);
static void name_input(fsimg_error_t *, fsimg_file_setting_t);
static fsimg_status_t resolve_compression(fsimg_disk_plan_t *,
    const fsimg_layer_options_t *, fsimg_error_t *);
static fsimg_status_t check_exact_volume(const fsimg_stack_t *,
    const fsimg_layer_t *, fsimg_error_t *);
static fsimg_status_t size_disk(fsimg_disk_plan_t *, uint64_t,
    fsimg_error_t *);

/*
 * The identifier derivation: the epoch as a little-endian 64-bit value,
 * the role byte, the index byte and six zero bytes, in RFC 4122 text
 * order; then version 4 in the high nibble of byte 6 and the variant bits
 * 10 at the top of byte 8, so tools accept the GUID as well formed.
 */
void
fsimg_identifier_derive(int64_t epoch, fsimg_identifier_role_t role,
    uint8_t index, unsigned char guid[static 16])
{
	uint64_t value = (uint64_t)epoch;

	for (unsigned i = 0; i < 8; i++)
		guid[i] = (unsigned char)(value >> (8 * i));
	guid[8] = (unsigned char)role;
	guid[9] = index;
	bzero(guid + 10, 6);
	guid[6] = (unsigned char)((guid[6] & 0x0f) | 0x40);
	guid[8] = (unsigned char)((guid[8] & 0x3f) | 0x80);
}

fsimg_status_t
fsimg_container_identifier(const unsigned char *given, const char *key,
    fsimg_identifier_role_t role, uint32_t flags, int64_t epoch,
    unsigned char guid[static 16], fsimg_error_t *error)
{
	if (!fsimg_guid_unset(given)) {
		bcopy(given, guid, 16);
		return (FSIMG_OK);
	}
	if ((flags & FSIMG_WRITE_REPRODUCIBLE) != 0) {
		fsimg_identifier_derive(epoch, role, 0, guid);
		return (FSIMG_OK);
	}
	return (fsimg_setting_error(error, key,
	    "the identifier must be set outside reproducible mode"));
}

static bool
round_up(uint64_t value, uint64_t unit, uint64_t *out)
{
	uint64_t remainder = value % unit;

	if (remainder == 0) {
		*out = value;
		return (true);
	}
	return (fsimg_add_u64(value, unit - remainder, out));
}

bool
fsimg_guid_unset(const unsigned char *guid)
{
	static const unsigned char unset[16];

	return (bcmp(guid, unset, sizeof(unset)) == 0);
}

fsimg_status_t
fsimg_setting_error(fsimg_error_t *error, const char *key,
    const char *message)
{
	fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE, message);
	fsimg_error_path(error, key);
	return (FSIMG_EINVAL);
}

/* A partition's failure names its setting ("gpt:type") when the layer
 * options supplied it, and the partition ("partition 2") in a compose.
 */
static void
partition_key(const fsimg_scheme_partition_args_t *args,
    const char *setting, char *key, size_t capacity)
{
	if (args->part != NULL)
		snprintf(key, capacity, "partition %u", (unsigned)args->index);
	else
		snprintf(key, capacity, "%s", setting);
}

fsimg_status_t
fsimg_partition_error(const fsimg_scheme_partition_args_t *args,
    const char *setting, fsimg_status_t status, fsimg_error_t *error)
{
	char key[32];

	partition_key(args, setting, key, sizeof(key));
	fsimg_error_path(error, key);
	return (status);
}

fsimg_status_t
fsimg_partition_setting_error(const fsimg_scheme_partition_args_t *args,
    const char *setting, const char *message, fsimg_error_t *error)
{
	fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE, message);
	return (fsimg_partition_error(args, setting, FSIMG_EINVAL, error));
}

/* The write set is all present or all absent; plan_disk stands for it. */
bool
fsimg_scheme_writable(const fsimg_scheme_ops_t *ops)
{
	return (ops->plan_disk != NULL);
}

bool
fsimg_scheme_write_set_valid(const fsimg_scheme_ops_t *ops)
{
	unsigned present;

	present = (ops->plan_disk != NULL) + (ops->plan_partition != NULL) +
	    (ops->validate_plan != NULL) + (ops->report_plan != NULL) +
	    (ops->footprint != NULL) + (ops->encode != NULL) +
	    (ops->slots != 0) + (ops->max_disk_sectors != 0);
	return (present == 0 || present == 8);
}

/* The facts of a disk without a container, and a new plan's defaults. */
static const fsimg_container_plan_t no_container = {
	.granularity = 1,
	.chunk_bytes = FSIMG_TRANSCODE_CHUNK_BYTES,
	.passes = 1
};

void *
fsimg_container_plan_alloc(fsimg_ctx_t *ctx, size_t size,
    fsimg_container_plan_t **out, fsimg_error_t *error)
{
	fsimg_container_plan_t *base;

	base = fsimg_alloc(ctx, size, error);
	if (base == NULL)
		return (NULL);
	bzero(base, size);
	*base = no_container;
	*out = base;
	return (base);
}

void
fsimg_container_plan_release(fsimg_container_plan_t *plan)
{
	fsimg_dealloc(plan);
}

const fsimg_container_plan_t *
fsimg_disk_plan_container(const fsimg_disk_plan_t *plan)
{
	return (plan->container_plan != NULL ? plan->container_plan :
	    &no_container);
}

/* The stack's container and its private plan; none leaves the disk
 * unencoded with the defaults. A container that carries a filesystem holds
 * nothing else, and one without a writer, such as hbcifs, is refused
 * naming it. A failed constructor may leave a partial plan, which only
 * fsimg_disk_plan_free reaches.
 */
static fsimg_status_t
resolve_container(fsimg_ctx_t *ctx, fsimg_disk_plan_t *plan,
    const fsimg_disk_request_t *request, fsimg_error_t *error)
{
	fsimg_container_plan_args_t args;
	fsimg_status_t status;

	plan->container = fsimg_stack_layer(request->stack,
	    FSIMG_LAYER_CONTAINER);
	if (plan->container == NULL)
		return (FSIMG_OK);
	status = fsimg_stack_carried(request->stack, plan->container, error);
	if (status != FSIMG_OK)
		return (status);
	if (plan->container->container->writer == NULL) {
		fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0, FSIMG_OP_IMAGE,
		    "the container is read only");
		fsimg_error_path(error, plan->container->name);
		return (FSIMG_EUNSUPPORTED);
	}
	args = (fsimg_container_plan_args_t){ .ctx = ctx,
	    .session = request->session, .layers = request->layers,
	    .files = plan->files, .epoch = plan->epoch, .flags = plan->flags,
	    .volume_order = request->volume_order };
	return (plan->container->container->writer->plan(&args,
	    &plan->container_plan, error));
}

/*
 * Borrow the request's file settings, whose layers the options check has
 * already found in the stack, each refused naming its key unless its
 * object is of the plan's context, and record each host file's identity
 * and the object's length before anything reads it, so that after the
 * copy fsimg_disk_plan_inputs_unchanged can tell whether it changed. A
 * file whose length moved since its object was made is refused as changed
 * here, since the object would read only the bytes its old length covers.
 */
static fsimg_status_t
record_inputs(fsimg_ctx_t *ctx, fsimg_disk_plan_t *plan,
    const fsimg_disk_request_t *request, fsimg_error_t *error)
{
	fsimg_planned_file_t *input;
	fsimg_status_t status;

	status = FSIMG_OK;
	for (unsigned i = 0; status == FSIMG_OK && i < FSIMG_FILE_SETTINGS;
	    i++) {
		input = &plan->files[i];
		input->io = fsimg_file_setting_input(request->inputs, i);
		if (input->io == NULL)
			continue;
		if (fsimg_io_context(input->io) != ctx)
			status = fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_IMAGE, "file setting is not in this context");
		if (status == FSIMG_OK)
			status = fsimg_io_identity(input->io,
			    &input->identity, &input->known, error);
		if (status == FSIMG_OK && input->known &&
		    !fsimg_io_length_current(input->io, &input->identity))
			status = fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
			    FSIMG_OP_SOURCE,
			    "file setting length differs from its object's");
		if (status == FSIMG_OK)
			status = fsimg_io_get_size(input->io, &input->length, error);
		if (status != FSIMG_OK)
			name_input(error, i);
	}
	return (status);
}

/* Name the file setting a failure concerns in error.path. */
static void
name_input(fsimg_error_t *error, fsimg_file_setting_t setting)
{
	char key[32];

	fsimg_file_setting_name(setting, key, sizeof(key));
	fsimg_error_path(error, key);
}

fsimg_status_t
fsimg_disk_plan_inputs_unchanged(const fsimg_disk_plan_t *plan,
    fsimg_error_t *error)
{
	const fsimg_planned_file_t *input;
	fsimg_host_file_t now;
	bool known;
	fsimg_status_t status;

	for (unsigned i = 0; i < FSIMG_FILE_SETTINGS; i++) {
		input = &plan->files[i];
		if (input->io == NULL || !input->known)
			continue;
		status = fsimg_io_identity(input->io, &now, &known, error);
		if (status == FSIMG_OK && (!known ||
		    !fsimg_source_identity_equal(&input->identity, &now)))
			status = fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
			    FSIMG_OP_SOURCE,
			    "file setting identity, size, or change metadata differs");
		if (status != FSIMG_OK) {
			name_input(error, i);
			return (status);
		}
	}
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_disk_plan_input_aliases(const fsimg_disk_plan_t *plan,
    const fsimg_host_file_t *output, fsimg_error_t *error)
{
	const fsimg_planned_file_t *input;

	for (unsigned i = 0; i < FSIMG_FILE_SETTINGS; i++) {
		input = &plan->files[i];
		if (input->io == NULL || !input->known ||
		    input->identity.device != output->device ||
		    input->identity.inode != output->inode)
			continue;
		fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "output aliases a file setting");
		name_input(error, i);
		return (FSIMG_EINVAL);
	}
	return (FSIMG_OK);
}

/* The plan's compression's resolved settings. */
static fsimg_status_t
resolve_compression(fsimg_disk_plan_t *plan,
    const fsimg_layer_options_t *layers, fsimg_error_t *error)
{
	if (plan->compression == NULL)
		return (FSIMG_OK);
	return (plan->compression->compression->plan(layers, plan->flags,
	    plan->epoch, &plan->compression_settings, error));
}

/*
 * The exact-volume stack rule (specs/design-layers.md#exact-volumes): no
 * layer below a filesystem whose descriptor declares an exact volume may
 * make its slot longer than the volume. The caller passes each layer that
 * can lengthen the slot: the scheme, since a partition is whole
 * sectors, and the container only when it rounds the disk up to a
 * granularity above one byte, the one way a container pads. Compression
 * encodes the disk byte for byte and is never a candidate. NULL passes.
 * The refusal names the filesystem and the layer ("ifs/vhd").
 */
static fsimg_status_t
check_exact_volume(const fsimg_stack_t *stack,
    const fsimg_layer_t *lengthening, fsimg_error_t *error)
{
	const fsimg_stack_slot_t *slot = &stack->slots[FSIMG_LAYER_FILESYSTEM];
	const fsimg_format_variant_t *variant;
	const fsimg_format_t *format;
	char layers[2 * sizeof(slot->name)];

	if (lengthening == NULL || slot->state != FSIMG_SLOT_NAMED)
		return (FSIMG_OK);
	format = fsimg_format_find(slot->name, &variant);
	if (format == NULL || !format->exact_volume)
		return (FSIMG_OK);
	snprintf(layers, sizeof(layers), "%s/%s", slot->name,
	    lengthening->name);
	fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0, FSIMG_OP_IMAGE,
	    "the filesystem cannot sit in a scheme or a padding container");
	fsimg_error_path(error, layers);
	return (FSIMG_EUNSUPPORTED);
}

bool
fsimg_disk_plan_encoded(const fsimg_disk_plan_t *plan)
{
	return (plan->container != NULL || plan->compression != NULL);
}

/*
 * Resolve the stack's container and compression, then its scheme. A stack
 * that breaks the exact-volume rule is refused with FSIMG_EUNSUPPORTED: a
 * scheme before any container setting is resolved, a padding container
 * once its settings give its granularity.
 */
fsimg_status_t
fsimg_disk_plan_init(fsimg_ctx_t *ctx, const fsimg_disk_request_t *request,
    fsimg_disk_plan_t *plan, fsimg_error_t *error)
{
	const fsimg_stack_t *stack = request->stack;
	const fsimg_layer_options_t *layers = request->layers;
	fsimg_status_t status;

	bzero(plan, sizeof(*plan));
	if (request->session == NULL || request->session->ctx != ctx)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
		    "a disk plan needs its operation's session"));
	plan->epoch = request->epoch;
	plan->flags = request->flags;
	plan->verified_disk = request->verified_disk;
	/* compression is a write options' field, whose size bounds the name. */
	status = fsimg_compression_named(request->compression,
	    sizeof(((fsimg_write_options_t *)NULL)->compression), true,
	    &plan->compression, error);
	if (status == FSIMG_OK)
		status = fsimg_layer_options_check(stack, plan->compression,
		    layers, request->inputs, true, error);
	if (status == FSIMG_OK)
		status = record_inputs(ctx, plan, request, error);
	if (status == FSIMG_OK)
		status = check_exact_volume(stack,
		    fsimg_stack_layer(stack, FSIMG_LAYER_SCHEME), error);
	if (status == FSIMG_OK)
		status = resolve_container(ctx, plan, request, error);
	if (status == FSIMG_OK &&
	    fsimg_disk_plan_container(plan)->granularity > 1)
		status = check_exact_volume(stack, plan->container, error);
	if (status == FSIMG_OK)
		status = resolve_compression(plan, layers, error);
	if (status != FSIMG_OK)
		return (status);
	return (fsimg_disk_plan_scheme(ctx, plan,
	    fsimg_stack_layer(stack, FSIMG_LAYER_SCHEME), request->layers,
	    request->partitions, error));
}

/*
 * The scheme's disk: admission by its write set and slot count, then its
 * plan_disk, whose facts this consumes at once. The sector must divide the
 * partition alignment, so aligned starts are whole sectors, and a fixed
 * length must be whole sectors and a whole number of the container's
 * granules. Each record starts zeroed with its slot number.
 */
fsimg_status_t
fsimg_disk_plan_scheme(fsimg_ctx_t *ctx, fsimg_disk_plan_t *plan,
    const fsimg_layer_t *scheme, const fsimg_layer_options_t *layers,
    uint32_t count, fsimg_error_t *error)
{
	fsimg_scheme_table_t *table = &plan->table;
	const fsimg_scheme_ops_t *ops;
	fsimg_scheme_plan_args_t args;
	fsimg_scheme_plan_facts_t facts;
	uint64_t granularity;
	char message[64];
	fsimg_status_t status;

	if (plan->scheme != NULL || table->records != NULL)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
		    "the disk plan already has a scheme"));
	if (scheme == NULL)
		return (FSIMG_OK);
	ops = scheme->scheme;
	if (ops == NULL || !fsimg_scheme_writable(ops)) {
		fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0, FSIMG_OP_IMAGE,
		    "the scheme is read only");
		fsimg_error_path(error, scheme->name);
		return (FSIMG_EUNSUPPORTED);
	}
	plan->scheme = scheme;
	if (count == 0 || count > ops->slots) {
		snprintf(message, sizeof(message), "a %s disk holds 1 to %u partitions",
		    scheme->name, (unsigned)ops->slots);
		fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE, message);
		if (error != NULL) {
			error->limit = ops->slots;
			error->observed = count;
		}
		return (FSIMG_EINVAL);
	}

	args = (fsimg_scheme_plan_args_t){ .layers = layers,
	    .epoch = plan->epoch, .flags = plan->flags };
	bzero(&facts, sizeof(facts));
	status = ops->plan_disk(&args, table, &facts, error);
	if (status != FSIMG_OK)
		return (status);
	if (table->sector_bytes == 0 ||
	    PARTITION_ALIGNMENT % table->sector_bytes != 0 ||
	    facts.disk_key == NULL)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
		    "the scheme resolved no usable sector size"));
	plan->disk_key = facts.disk_key;
	plan->disk_bytes = facts.requested_disk_bytes;
	plan->fixed = plan->disk_bytes != 0;
	if (plan->disk_bytes % table->sector_bytes != 0)
		return (fsimg_setting_error(error, plan->disk_key,
		    "the disk length is not a multiple of the sector size"));
	granularity = fsimg_disk_plan_container(plan)->granularity;
	if (plan->disk_bytes % granularity != 0)
		return (fsimg_setting_error(error, plan->disk_key, "the disk length "
		    "is not a multiple of the container's granularity"));

	table->records = fsimg_alloc(ctx, (size_t)count * sizeof(*table->records),
	    error);
	if (table->records == NULL)
		return (error->status);
	bzero(table->records, (size_t)count * sizeof(*table->records));
	table->count = count;
	for (uint32_t i = 0; i < count; i++)
		table->records[i].index = i + 1;
	return (FSIMG_OK);
}

/*
 * One partition's metadata, by the scheme, from exactly one source. The
 * record starts zeroed with its slot number; the scheme writes no
 * placement and marks it used once every field has resolved.
 */
fsimg_status_t
fsimg_disk_plan_partition(fsimg_disk_plan_t *plan, uint32_t slot,
    const fsimg_layer_options_t *layers, const fsimg_partition_request_t *part,
    const char *filesystem, fsimg_error_t *error)
{
	fsimg_partition_record_t *record;
	fsimg_scheme_partition_args_t args;

	if (plan->scheme == NULL || slot == 0 || slot > plan->table.count ||
	    (layers == NULL) == (part == NULL))
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
		    "the disk plan has no such partition"));
	record = &plan->table.records[slot - 1];
	bzero(record, sizeof(*record));
	record->index = slot;
	args = (fsimg_scheme_partition_args_t){ .layers = layers, .part = part,
	    .filesystem = filesystem, .epoch = plan->epoch, .flags = plan->flags,
	    .index = slot };
	return (plan->scheme->scheme->plan_partition(&args, record, error));
}

/* The scheme's leading and trailing tables in bytes. */
static fsimg_status_t
footprint_bytes(const fsimg_disk_plan_t *plan, uint64_t *leading,
    uint64_t *trailing, fsimg_error_t *error)
{
	uint32_t sector = plan->table.sector_bytes;
	uint64_t leading_sectors, trailing_sectors;

	plan->scheme->scheme->footprint(sector, &leading_sectors,
	    &trailing_sectors);
	if (!fsimg_mul_u64(leading_sectors, sector, leading) ||
	    !fsimg_mul_u64(trailing_sectors, sector, trailing))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_IMAGE,
		    "the scheme's tables exceed the disk range"));
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_disk_plan_fit(const fsimg_disk_plan_t *plan, uint32_t granule,
    uint64_t *volume_bytes, fsimg_error_t *error)
{
	uint64_t leading, trailing, start, end, longest;
	fsimg_status_t status;

	if (plan->scheme == NULL || !plan->fixed || granule == 0)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
		    "only a fixed disk has a volume length to fit"));
	status = footprint_bytes(plan, &leading, &trailing, error);
	if (status != FSIMG_OK)
		return (status);
	if (!round_up(leading, PARTITION_ALIGNMENT, &start) ||
	    plan->disk_bytes < trailing)
		return (fsimg_setting_error(error, plan->disk_key,
		    "the disk is too small for a partition"));
	end = plan->disk_bytes - trailing;
	if (end <= start || (end - start) / granule == 0)
		return (fsimg_setting_error(error, plan->disk_key,
		    "the disk is too small for a partition"));
	longest = (end - start) / granule * granule;
	if (*volume_bytes == 0)
		*volume_bytes = longest;
	else if (*volume_bytes > longest)
		return (fsimg_setting_error(error, plan->disk_key,
		    "the disk is too small for its partitions and tables"));
	return (FSIMG_OK);
}

/*
 * The disk's length once its contents need `used` bytes: a fixed disk's
 * own, else used rounded up to the container's granularity, the padding
 * zeros that become unallocated space. Then the output's bound: the disk
 * itself, or the most its container encoding can take, which also refuses
 * a disk the container cannot describe, and then the most the compression
 * of that can take.
 */
static fsimg_status_t
size_disk(fsimg_disk_plan_t *plan, uint64_t used, fsimg_error_t *error)
{
	const fsimg_container_plan_t *facts = fsimg_disk_plan_container(plan);
	fsimg_status_t status;

	if (!plan->fixed) {
		if (!round_up(used, facts->granularity, &plan->disk_bytes))
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_IMAGE, "the disk exceeds the disk range"));
		plan->padding_bytes = plan->disk_bytes - used;
	}
	plan->container_bytes = plan->disk_bytes;
	if (plan->container != NULL) {
		status = plan->container->container->writer->bound(
		    plan->container_plan, plan->disk_bytes,
		    &plan->container_bytes, error);
		if (status != FSIMG_OK)
			return (status);
	}
	plan->output_bytes = plan->container_bytes;
	if (plan->compression == NULL)
		return (FSIMG_OK);
	return (plan->compression->compression->bound(plan->container_bytes,
	    &plan->output_bytes, error));
}

/*
 * Lay the records out in slot order: each at the first 1 MiB boundary at
 * or after the end of the leading tables or the previous partition, its
 * volume rounded up to whole sectors, the trailing tables after the last.
 * A fixed disk must hold the result; otherwise it is the disk, rounded up
 * to the container's granularity, and the trailing tables end the rounded
 * disk because the scheme encodes them at the disk's end. The final disk
 * is whole sectors, since the scheme cannot encode a partial one, and once
 * the container and compression have bounded it the scheme validates it.
 * Every value is computed with checked arithmetic.
 */
fsimg_status_t
fsimg_disk_plan_place(fsimg_disk_plan_t *plan, const uint64_t *volume_bytes,
    fsimg_error_t *error)
{
	fsimg_partition_record_t *record;
	uint64_t leading, trailing, cursor, start, length, end;
	uint32_t sector;
	fsimg_status_t status;
	char key[32];

	if (plan->scheme == NULL)
		return (size_disk(plan, volume_bytes[0], error));
	sector = plan->table.sector_bytes;
	status = footprint_bytes(plan, &leading, &trailing, error);
	if (status != FSIMG_OK)
		return (status);
	cursor = leading;
	for (uint32_t i = 0; i < plan->table.count; i++) {
		record = &plan->table.records[i];
		if (volume_bytes[i] == 0) {
			snprintf(key, sizeof(key), "partition %u", (unsigned)i + 1);
			return (fsimg_setting_error(error, key, "a partition cannot be empty"));
		}
		if (!round_up(cursor, PARTITION_ALIGNMENT, &start) ||
		    !round_up(volume_bytes[i], sector, &length) ||
		    !fsimg_add_u64(start, length, &cursor))
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_IMAGE, "the partitions exceed the disk range"));
		record->offset = start;
		record->length = length;
	}
	if (!fsimg_add_u64(cursor, trailing, &end))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_IMAGE,
		    "the partitions exceed the disk range"));
	if (plan->fixed && end > plan->disk_bytes)
		return (fsimg_setting_error(error, plan->disk_key,
		    "the disk is too small for its partitions and tables"));
	status = size_disk(plan, end, error);
	if (status != FSIMG_OK)
		return (status);
	if (plan->disk_bytes % sector != 0)
		return (fsimg_setting_error(error, plan->disk_key, "the container's "
		    "granularity leaves the disk a partial sector"));
	return (plan->scheme->scheme->validate_plan(&plan->table,
	    plan->disk_bytes, error));
}

void
fsimg_disk_plan_report(const fsimg_disk_plan_t *plan, uint32_t slot,
    fsimg_image_info_t *image, fsimg_partition_info_t *partition)
{
	const fsimg_scheme_table_t *table = &plan->table;

	bzero(image, sizeof(*image));
	bzero(partition, sizeof(*partition));
	/* The planned disk is its own file, before any container wraps it,
	 * as an opened image reports the file it peeled once decompressed.
	 */
	image->disk_bytes = image->file_bytes = plan->disk_bytes;
	if (plan->compression != NULL)
		snprintf(image->compression, sizeof(image->compression), "%s",
		    plan->compression->name);
	if (plan->container != NULL) {
		snprintf(image->container, sizeof(image->container), "%s",
		    plan->container->name);
		bcopy(fsimg_disk_plan_container(plan)->identifier,
		    image->container_guid, sizeof(image->container_guid));
	}
	if (plan->scheme == NULL || slot == 0 || slot > table->count)
		return;
	snprintf(image->scheme, sizeof(image->scheme), "%s", plan->scheme->name);
	plan->scheme->scheme->report_plan(table, slot, image, partition);
}

/*
 * The output budget of a placed plan: the output's bound, plus the disk
 * itself when raw says a raw intermediate holds it while the output is
 * written from it. An explicit output limit below that fails before
 * anything is written, naming both parts.
 */
fsimg_status_t
fsimg_disk_plan_budget(fsimg_ctx_t *ctx, const fsimg_disk_plan_t *plan,
    bool raw, fsimg_error_t *error)
{
	uint64_t limit = ctx->options.max_output_bytes;
	uint64_t needed;
	char encoding[40];

	needed = plan->output_bytes;
	if (raw)
		fsimg_stat_add(&needed, plan->disk_bytes);
	if (limit == 0 || needed <= limit)
		return (FSIMG_OK);
	fsimg_limit_error(error, FSIMG_RESOURCE_OUTPUT, limit, needed);
	if (!raw || error == NULL)
		return (FSIMG_ELIMIT);
	/* The encoding's layers, "dmg", "gzip" or "dmg/gzip", or a copy. */
	snprintf(encoding, sizeof(encoding), "%s%s%s",
	    plan->container != NULL ? plan->container->name : "",
	    plan->container != NULL && plan->compression != NULL ? "/" : "",
	    plan->compression != NULL ? plan->compression->name : "");
	snprintf(error->message, sizeof(error->message),
	    "the raw disk of %llu bytes and its %s%s of up to %llu bytes exceed "
	    "the output limit", (unsigned long long)plan->disk_bytes,
	    encoding[0] != '\0' ? encoding : "copy",
	    encoding[0] != '\0' ? " encoding" : "",
	    (unsigned long long)plan->output_bytes);
	return (FSIMG_ELIMIT);
}

void
fsimg_disk_plan_free(fsimg_disk_plan_t *plan)
{
	fsimg_dealloc(plan->table.records);
	plan->table.records = NULL;
	plan->table.count = 0;
	if (plan->container_plan != NULL)
		plan->container->container->writer->plan_free(
		    plan->container_plan);
	plan->container_plan = NULL;
}

void
fsimg_scheme_tables_free(fsimg_scheme_tables_t *tables)
{
	for (unsigned i = 0; i < tables->count; i++)
		fsimg_dealloc(tables->regions[i].bytes);
	bzero(tables, sizeof(*tables));
}
