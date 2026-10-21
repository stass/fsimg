/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "format.h"
#include "layer.h"
#include "unicode.h"

/*
 * The disk plan: what a writer, wrap or compose puts around its volumes.
 * Every layer setting is resolved here once, so the three share one rule
 * for each default, identifier and limit, and placement lays partitions
 * out as sgdisk and fdisk do: each at the first 1 MiB boundary after the
 * scheme's leading tables or the previous partition, whole sectors long,
 * with the trailing tables at the end of the disk. A container rounds the
 * disk up to its granularity first, so those tables end the disk it
 * declares, and states the bound of the output it will encode; a
 * compression states the bound of its encoding of that. A filesystem that
 * must fill its slot exactly refuses a scheme or a padding container.
 */
#define PARTITION_ALIGNMENT	(UINT64_C(1) << 20)

static bool round_up(uint64_t, uint64_t, uint64_t *);
static bool guid_unset(const unsigned char *);
static const char *disk_key(const fsimg_disk_plan_t *);
static void request_key(const fsimg_disk_plan_t *,
    const fsimg_partition_request_t *, uint32_t, const char *, char *, size_t);
static fsimg_status_t resolve_type(fsimg_disk_plan_t *,
    fsimg_partition_record_t *, const fsimg_partition_request_t *,
    const char *, const char **, fsimg_error_t *);
static fsimg_status_t resolve_name(fsimg_partition_record_t *, const char *,
    const char *, fsimg_error_t *);
static fsimg_status_t check_identities(const fsimg_disk_plan_t *,
    fsimg_error_t *);
static fsimg_status_t resolve_container(fsimg_disk_plan_t *,
    const fsimg_stack_t *, const fsimg_layer_options_t *, fsimg_error_t *);
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
	if (!guid_unset(given)) {
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

static bool
guid_unset(const unsigned char *guid)
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

static const char *
disk_key(const fsimg_disk_plan_t *plan)
{
	return (plan->table.kind == FSIMG_SCHEME_GPT ? "gpt:disk" : "mbr:disk");
}

/* A partition's failure names its setting ("gpt:type") when the layer
 * options supplied it, and the partition ("partition 2") in a compose.
 */
static void
request_key(const fsimg_disk_plan_t *plan,
    const fsimg_partition_request_t *request, uint32_t index,
    const char *setting, char *key, size_t capacity)
{
	if (request->composed)
		snprintf(key, capacity, "partition %u", (unsigned)index + 1);
	else
		snprintf(key, capacity, "%s:%s", plan->scheme->name, setting);
}

/* The stack's container and its resolved settings; none leaves the disk
 * unencoded, with a granularity of one byte, copied in 1 MiB chunks. A
 * container that carries a filesystem holds nothing else, and one without
 * a writer, such as hbcifs, is refused naming it.
 */
static fsimg_status_t
resolve_container(fsimg_disk_plan_t *plan, const fsimg_stack_t *stack,
    const fsimg_layer_options_t *layers, fsimg_error_t *error)
{
	fsimg_status_t status;

	plan->container_settings.granularity = 1;
	plan->container_settings.chunk_bytes = FSIMG_TRANSCODE_CHUNK_BYTES;
	plan->container = fsimg_stack_layer(stack, FSIMG_LAYER_CONTAINER);
	if (plan->container == NULL)
		return (FSIMG_OK);
	status = fsimg_stack_carried(stack, plan->container, error);
	if (status != FSIMG_OK)
		return (status);
	if (plan->container->container->plan == NULL) {
		fsimg_error_set(error, FSIMG_EUNSUPPORTED, 0, FSIMG_OP_IMAGE,
		    "the container is read only");
		fsimg_error_path(error, plan->container->name);
		return (FSIMG_EUNSUPPORTED);
	}
	return (plan->container->container->plan(layers, plan->flags,
	    plan->epoch, &plan->container_settings, error));
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
 * Resolve the stack's scheme and container and the disk-level settings:
 * the sector size, a fixed disk length, and the disk identifier, which is
 * taken as given, derived from the epoch in reproducible mode, or refused.
 * A stack that breaks the exact-volume rule is refused with
 * FSIMG_EUNSUPPORTED: a scheme before any container setting is resolved,
 * a padding container once its settings give its granularity. The disk
 * holds count partitions, each made an empty record in slot order.
 */
fsimg_status_t
fsimg_disk_plan_init(fsimg_ctx_t *ctx, const fsimg_stack_t *stack,
    const fsimg_layer_options_t *layers, const char *compression,
    uint32_t count, int64_t epoch, uint32_t flags, fsimg_disk_plan_t *plan,
    fsimg_error_t *error)
{
	fsimg_scheme_table_t *table = &plan->table;
	const char *key;
	char message[64];
	fsimg_status_t status;
	bool reproducible;

	bzero(plan, sizeof(*plan));
	plan->epoch = epoch;
	plan->flags = flags;
	/* compression is a write options' field, whose size bounds the name. */
	status = fsimg_compression_named(compression,
	    sizeof(((fsimg_write_options_t *)NULL)->compression), true,
	    &plan->compression, error);
	if (status == FSIMG_OK)
		status = fsimg_layer_options_check(stack, plan->compression,
		    layers, true, error);
	if (status == FSIMG_OK)
		status = check_exact_volume(stack,
		    fsimg_stack_layer(stack, FSIMG_LAYER_SCHEME), error);
	if (status == FSIMG_OK)
		status = resolve_container(plan, stack, layers, error);
	if (status == FSIMG_OK && plan->container_settings.granularity > 1)
		status = check_exact_volume(stack, plan->container, error);
	if (status == FSIMG_OK)
		status = resolve_compression(plan, layers, error);
	if (status != FSIMG_OK)
		return (status);
	plan->scheme = fsimg_stack_layer(stack, FSIMG_LAYER_SCHEME);
	if (plan->scheme == NULL)
		return (FSIMG_OK);
	if (count == 0 || count > plan->scheme->scheme->slots) {
		snprintf(message, sizeof(message), "a %s disk holds 1 to %u partitions",
		    plan->scheme->name, (unsigned)plan->scheme->scheme->slots);
		fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE, message);
		if (error != NULL) {
			error->limit = plan->scheme->scheme->slots;
			error->observed = count;
		}
		return (FSIMG_EINVAL);
	}

	reproducible = (flags & FSIMG_WRITE_REPRODUCIBLE) != 0;
	if (strcmp(plan->scheme->name, "gpt") == 0) {
		table->kind = FSIMG_SCHEME_GPT;
		table->sector_bytes = layers->gpt_sector_bytes == 0 ? 512 :
		    layers->gpt_sector_bytes;
		if (table->sector_bytes != 512 && table->sector_bytes != 4096)
			return (fsimg_setting_error(error, "gpt:sector",
			    "gpt:sector must be 512 or 4096"));
		plan->disk_bytes = layers->gpt_disk_bytes;
		if (!guid_unset(layers->gpt_disk_guid))
			bcopy(layers->gpt_disk_guid, table->disk_guid, 16);
		else if (reproducible)
			fsimg_identifier_derive(epoch, FSIMG_ROLE_DISK, 0,
			    table->disk_guid);
		else
			return (fsimg_setting_error(error, "gpt:disk-guid",
			    "the disk GUID must be set outside reproducible mode"));
	} else {
		table->kind = FSIMG_SCHEME_MBR;
		table->sector_bytes = 512;
		plan->disk_bytes = layers->mbr_disk_bytes;
		if (plan->disk_bytes / 512 > plan->scheme->scheme->max_disk_sectors)
			return (fsimg_setting_error(error, "mbr:disk",
			    "an MBR disk holds at most 2^32 sectors"));
		if (layers->mbr_id_set)
			table->mbr_disk_id = layers->mbr_id;
		else if (reproducible)
			table->mbr_disk_id = (uint32_t)(uint64_t)epoch;
		else
			return (fsimg_setting_error(error, "mbr:id",
			    "the disk identifier must be set outside reproducible mode"));
	}
	key = disk_key(plan);
	if (plan->disk_bytes % table->sector_bytes != 0)
		return (fsimg_setting_error(error, key,
		    "the disk length is not a multiple of the sector size"));
	if (plan->disk_bytes % plan->container_settings.granularity != 0)
		return (fsimg_setting_error(error, key, "the disk length is not a "
		    "multiple of the container's granularity"));
	plan->fixed = plan->disk_bytes != 0;

	table->records = fsimg_alloc(ctx, (size_t)count * sizeof(*table->records),
	    error);
	if (table->records == NULL)
		return (error->status);
	table->count = count;
	for (uint32_t i = 0; i < count; i++)
		table->records[i].index = i + 1;
	return (FSIMG_OK);
}

/* The record's type: the requested text, or the type-table entry named
 * after the filesystem. default_name receives the type's GPT name.
 */
static fsimg_status_t
resolve_type(fsimg_disk_plan_t *plan, fsimg_partition_record_t *record,
    const fsimg_partition_request_t *request, const char *key,
    const char **default_name, fsimg_error_t *error)
{
	fsimg_type_selector_t selector;
	fsimg_status_t status;

	bzero(&selector, sizeof(selector));
	if (request->type != NULL && request->type[0] != '\0') {
		status = fsimg_partition_type_parse(request->type, &selector, error);
		if (status != FSIMG_OK) {
			fsimg_error_path(error, key);
			return (status);
		}
	} else {
		if (request->filesystem != NULL)
			selector.type = fsimg_partition_type_find(request->filesystem);
		if (selector.type == NULL)
			return (fsimg_setting_error(error, key,
			    "no filesystem gives the partition a type; set one"));
	}
	status = fsimg_partition_type_assign(&selector, plan->table.kind, record,
	    default_name, error);
	if (status != FSIMG_OK)
		fsimg_error_path(error, key);
	return (status);
}

/* A GPT name: validated UTF-8 of at most 36 UTF-16 units, stored as units
 * with zero padding.
 */
static fsimg_status_t
resolve_name(fsimg_partition_record_t *record, const char *name,
    const char *key, fsimg_error_t *error)
{
	size_t units;
	fsimg_status_t status;

	status = fsimg_utf8_to_utf16(name, strlen(name), NULL, 0, &units, error);
	if (status == FSIMG_OK && units > FSIMG_GPT_NAME_UNITS)
		status = fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "the GPT name exceeds 36 UTF-16 units");
	if (status == FSIMG_OK)
		status = fsimg_utf8_to_utf16(name, strlen(name), record->name,
		    FSIMG_GPT_NAME_UNITS, &units, error);
	if (status != FSIMG_OK) {
		if (error != NULL)
			error->status = FSIMG_EINVAL;
		fsimg_error_path(error, key);
		return (FSIMG_EINVAL);
	}
	return (FSIMG_OK);
}

/*
 * Resolve one partition: its type; for GPT its name and unique GUID, the
 * GUID taken as given, derived from the epoch and the slot number in
 * reproducible mode, or refused; for MBR its status. A setting the scheme
 * does not store is refused rather than dropped.
 */
fsimg_status_t
fsimg_disk_plan_partition(fsimg_disk_plan_t *plan, uint32_t index,
    const fsimg_partition_request_t *request, fsimg_error_t *error)
{
	fsimg_partition_record_t *record;
	const char *default_name;
	char key[32];
	bool guid_given;
	fsimg_status_t status;

	if (plan->scheme == NULL || index >= plan->table.count)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
		    "the disk plan has no such partition"));
	record = &plan->table.records[index];
	request_key(plan, request, index, "type", key, sizeof(key));
	status = resolve_type(plan, record, request, key, &default_name, error);
	if (status != FSIMG_OK)
		return (status);
	guid_given = request->guid != NULL && !guid_unset(request->guid);
	if (plan->table.kind == FSIMG_SCHEME_MBR) {
		request_key(plan, request, index, "name", key, sizeof(key));
		if ((request->name != NULL && request->name[0] != '\0') ||
		    guid_given)
			return (fsimg_setting_error(error, key,
			    "MBR partitions have no name or GUID"));
		record->active = request->active;
		record->used = true;
		return (FSIMG_OK);
	}
	request_key(plan, request, index, "active", key, sizeof(key));
	if (request->active)
		return (fsimg_setting_error(error, key, "GPT partitions have no active flag"));
	request_key(plan, request, index, "name", key, sizeof(key));
	status = resolve_name(record, request->name != NULL &&
	    request->name[0] != '\0' ? request->name : default_name, key, error);
	if (status != FSIMG_OK)
		return (status);
	request_key(plan, request, index, "guid", key, sizeof(key));
	if (guid_given)
		bcopy(request->guid, record->guid, 16);
	else if ((plan->flags & FSIMG_WRITE_REPRODUCIBLE) != 0)
		fsimg_identifier_derive(plan->epoch, FSIMG_ROLE_PARTITION,
		    (uint8_t)record->index, record->guid);
	else
		return (fsimg_setting_error(error, key,
		    "the partition GUID must be set outside reproducible mode"));
	record->used = true;
	return (FSIMG_OK);
}

fsimg_status_t
fsimg_disk_plan_fit(const fsimg_disk_plan_t *plan, uint32_t granule,
    uint64_t *volume_bytes, fsimg_error_t *error)
{
	uint64_t leading, trailing, start, end, longest;
	uint32_t sector;

	if (plan->scheme == NULL || !plan->fixed || granule == 0)
		return (fsimg_error_set(error, FSIMG_ESTATE, 0, FSIMG_OP_IMAGE,
		    "only a fixed disk has a volume length to fit"));
	sector = plan->table.sector_bytes;
	plan->scheme->scheme->footprint(sector, &leading, &trailing);
	if (!round_up(leading * sector, PARTITION_ALIGNMENT, &start) ||
	    plan->disk_bytes < trailing * sector)
		return (fsimg_setting_error(error, disk_key(plan),
		    "the disk is too small for a partition"));
	end = plan->disk_bytes - trailing * sector;
	if (end <= start || (end - start) / granule == 0)
		return (fsimg_setting_error(error, disk_key(plan),
		    "the disk is too small for a partition"));
	longest = (end - start) / granule * granule;
	if (*volume_bytes == 0)
		*volume_bytes = longest;
	else if (*volume_bytes > longest)
		return (fsimg_setting_error(error, disk_key(plan),
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
	const fsimg_container_ops_t *container;
	fsimg_status_t status;

	if (!plan->fixed) {
		if (!round_up(used, plan->container_settings.granularity,
		    &plan->disk_bytes))
			return (fsimg_error_set(error, FSIMG_ERANGE, 0,
			    FSIMG_OP_IMAGE, "the disk exceeds the disk range"));
		plan->padding_bytes = plan->disk_bytes - used;
	}
	plan->container_bytes = plan->disk_bytes;
	if (plan->container != NULL) {
		container = plan->container->container;
		status = container->bound(&plan->container_settings,
		    plan->disk_bytes, &plan->container_bytes, error);
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
 * disk because the scheme encodes them at the disk's end. Every value is
 * computed with checked arithmetic.
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
	plan->scheme->scheme->footprint(sector, &leading, &trailing);
	cursor = leading * sector;
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
	if (!fsimg_add_u64(cursor, trailing * sector, &end))
		return (fsimg_error_set(error, FSIMG_ERANGE, 0, FSIMG_OP_IMAGE,
		    "the partitions exceed the disk range"));
	if (plan->fixed && end > plan->disk_bytes)
		return (fsimg_setting_error(error, disk_key(plan),
		    "the disk is too small for its partitions and tables"));
	status = size_disk(plan, end, error);
	if (status != FSIMG_OK)
		return (status);
	if (plan->disk_bytes / sector > plan->scheme->scheme->max_disk_sectors)
		return (fsimg_error_set(error, FSIMG_EINVAL, 0, FSIMG_OP_IMAGE,
		    "the disk exceeds the 2^32 sectors an MBR addresses"));
	return (check_identities(plan, error));
}

/* Unique GUIDs differ from each other and from the disk GUID, so no two
 * GPT objects of one disk share an identity.
 */
static fsimg_status_t
check_identities(const fsimg_disk_plan_t *plan, fsimg_error_t *error)
{
	const fsimg_partition_record_t *records = plan->table.records;
	char message[80];

	if (plan->table.kind != FSIMG_SCHEME_GPT)
		return (FSIMG_OK);
	for (uint32_t i = 0; i < plan->table.count; i++) {
		if (bcmp(records[i].guid, plan->table.disk_guid, 16) == 0) {
			snprintf(message, sizeof(message),
			    "partition %u has the disk GUID", (unsigned)i + 1);
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_IMAGE, message));
		}
		for (uint32_t j = 0; j < i; j++) {
			if (bcmp(records[i].guid, records[j].guid, 16) != 0)
				continue;
			snprintf(message, sizeof(message),
			    "partitions %u and %u have the same GUID",
			    (unsigned)j + 1, (unsigned)i + 1);
			return (fsimg_error_set(error, FSIMG_EINVAL, 0,
			    FSIMG_OP_IMAGE, message));
		}
	}
	return (FSIMG_OK);
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
		bcopy(plan->container_settings.guid, image->container_guid,
		    sizeof(image->container_guid));
	}
	if (plan->scheme == NULL || slot == 0 || slot > table->count)
		return;
	snprintf(image->scheme, sizeof(image->scheme), "%s", plan->scheme->name);
	image->sector_bytes = table->sector_bytes;
	image->partition_count = plan->scheme->scheme->slots;
	image->selected = slot;
	if (table->kind == FSIMG_SCHEME_GPT)
		bcopy(table->disk_guid, image->disk_guid, sizeof(image->disk_guid));
	else
		image->mbr_disk_id = table->mbr_disk_id;
	fsimg_partition_report(table->kind, &table->records[slot - 1],
	    partition);
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
}

void
fsimg_scheme_tables_free(fsimg_scheme_tables_t *tables)
{
	for (unsigned i = 0; i < tables->count; i++)
		fsimg_dealloc(tables->regions[i].bytes);
	bzero(tables, sizeof(*tables));
}
