/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "contract.h"
#include "host.h"
#include "internal.h"
#include "meta.h"

/*
 * Common filesystem driver contracts.
 *
 * Each registered filesystem variant has one coverage profile: test facts
 * written here, independently of the production registry. The registry
 * decides which scenarios a profile owes (a write bit schedules the writer
 * scenarios); the profile and the scenario contracts decide every expected
 * answer. Each scenario is a plain function reading as setup, operation,
 * assertion and cleanup; nothing here knows a format beyond its profile.
 *
 * Every registered format both reads and writes, so reader fixtures are
 * produced by the common writer from the profile's source tree. They are
 * round-trip integration evidence, not independent format evidence: the
 * format-local tests, the Ruby models and the integration scripts with
 * other implementations keep that role.
 */

/* What a source entry's bytes are, and how the provider delivers them. */
typedef enum payload {
	PAYLOAD_NONE,		/* a directory, FIFO or format-owned entry */
	PAYLOAD_EMPTY,		/* provider source with no bytes */
	PAYLOAD_ZERO_SOURCE,	/* FSIMG_SOURCE_ZERO */
	PAYLOAD_PATTERN,	/* provider source, one DATA range */
	PAYLOAD_MIXED,		/* provider source: DATA, ZERO, DATA */
	PAYLOAD_ZERO_RANGE,	/* provider source that is one ZERO range */
	PAYLOAD_TARGET		/* a symbolic link's target text */
} payload_t;

#define MIXED_ZERO_START	70000u
#define MIXED_ZERO_END		140000u
#define LINK_TARGET		"target"

typedef struct contract_entry {
	const char	*path;
	fsimg_entry_kind_t kind;
	uint64_t	 size;
	payload_t	 payload;
} contract_entry_t;

/*
 * The common source tree every writer collects: an empty file, a zero
 * source, a provider file that is one ZERO range, an empty directory, a
 * mixed DATA/ZERO/DATA file and a nested file spanning several copy
 * batches. Each entry's identity is its provider ID, and a parent is
 * listed before its children.
 */
enum {
	ID_EMPTY = 1,
	ID_ZEROS,
	ID_SPARSE,
	ID_DIR,
	ID_EDIR,
	ID_MIXED,
	ID_SUB,
	ID_LARGE,
	ID_COUNT = ID_LARGE
};

static const contract_entry_t source_tree[] = {
	[ID_EMPTY - 1] = { .path = "/empty", .kind = FSIMG_ENTRY_FILE,
	    .payload = PAYLOAD_EMPTY },
	[ID_ZEROS - 1] = { .path = "/zeros", .kind = FSIMG_ENTRY_FILE,
	    .size = 70001, .payload = PAYLOAD_ZERO_SOURCE },
	[ID_SPARSE - 1] = { .path = "/sparse", .kind = FSIMG_ENTRY_FILE,
	    .size = 50000, .payload = PAYLOAD_ZERO_RANGE },
	[ID_DIR - 1] = { .path = "/dir", .kind = FSIMG_ENTRY_DIRECTORY },
	[ID_EDIR - 1] = { .path = "/dir/edir", .kind = FSIMG_ENTRY_DIRECTORY },
	[ID_MIXED - 1] = { .path = "/dir/mixed", .kind = FSIMG_ENTRY_FILE,
	    .size = 200000, .payload = PAYLOAD_MIXED },
	[ID_SUB - 1] = { .path = "/dir/sub", .kind = FSIMG_ENTRY_DIRECTORY },
	[ID_LARGE - 1] = { .path = "/dir/sub/large", .kind = FSIMG_ENTRY_FILE,
	    .size = 300000, .payload = PAYLOAD_PATTERN },
};

#define SOURCE_COUNT	(sizeof(source_tree) / sizeof(source_tree[0]))
_Static_assert(SOURCE_COUNT == ID_COUNT, "every source identity has an entry");
/* The provider ID of the entry a metadata case adds. */
#define META_ID		(SOURCE_COUNT + 1)
#define META_PATH	"/meta"
#define META_FILE_BYTES	1000u
#define PROVIDER_IDS	(META_ID + 1)

/*
 * A metadata case adds one entry at META_PATH to the common tree and
 * states, from the format and metadata contracts, what planning does with
 * it and what a reader decodes. losses counts fields; loss_mask is the
 * union of the fields the loss callbacks name. checked selects the
 * timestamp, owner and mode fields compared after reading back.
 */
typedef struct contract_metadata_case {
	const char	*id;
	fsimg_entry_kind_t kind;
	fsimg_metadata_t source;
	bool		 forbid_loss;	/* clear FSIMG_WRITE_ALLOW_LOSS */
	fsimg_status_t	 plan_status;
	uint64_t	 losses;
	uint32_t	 loss_mask;
	uint32_t	 checked;
	fsimg_metadata_t stored;
} contract_metadata_case_t;

/* The explicit creation settings a profile tests, applied after
 * fsimg_write_options_init() selects every other default.
 */
typedef struct contract_write_setup {
	uint32_t	 flags;
	fsimg_byte_order_t byte_order;
} contract_write_setup_t;

typedef struct contract_fs_profile {
	const char	*id;
	const char	*format;
	bool		 coverage_profile;
	unsigned	 expected_capabilities;
	contract_write_setup_t write;
	/* Entries the format itself adds to every image. */
	const contract_entry_t *owned;
	size_t		 owned_count;
	const contract_metadata_case_t *metadata_cases;
	size_t		 metadata_case_count;
	uint64_t	 max_fixture_bytes;
} contract_fs_profile_t;

#define ALL_CAPABILITIES	(FSIMG_CAP_READ | FSIMG_CAP_WRITE | FSIMG_CAP_VERIFY)
#define COUNT(array)		(sizeof(array) / sizeof((array)[0]))

/* 2000-01-01 00:00:00, an instant every format stores. */
#define Y2K			INT64_C(946684800)
#define TIME(s, ns, b, off)	{ .seconds = (s), .nanoseconds = (ns), \
	.precision_ns = 1, .basis = (b), .offset_minutes = (off) }

/*
 * FAT stores wall-clock calendar fields: modification at two seconds,
 * creation at 10 ms, access by date; owner, mode and the change time are
 * not stored, and links and FIFOs are not entry kinds
 * (design-fat-writer.md, design-fat-reader.md).
 */
static const contract_metadata_case_t fat_metadata[] = {
	{ .id = "wall-time-exact", .kind = FSIMG_ENTRY_FILE,
	    .source = { .valid = FSIMG_META_MODIFIED | FSIMG_META_CREATED,
	    .modified = TIME(Y2K + 2, 0, FSIMG_TIME_WALL, 0),
	    .created = TIME(Y2K + 1, 250000000, FSIMG_TIME_WALL, 0) },
	    .plan_status = FSIMG_OK,
	    .checked = FSIMG_META_MODIFIED | FSIMG_META_CREATED,
	    .stored = { .modified = TIME(Y2K + 2, 0, FSIMG_TIME_WALL, 0),
	    .created = TIME(Y2K + 1, 250000000, FSIMG_TIME_WALL, 0) } },
	{ .id = "odd-second-rounds", .kind = FSIMG_ENTRY_FILE,
	    .source = { .valid = FSIMG_META_MODIFIED,
	    .modified = TIME(Y2K + 3, 0, FSIMG_TIME_WALL, 0) },
	    .plan_status = FSIMG_OK, .losses = 1,
	    .loss_mask = FSIMG_META_MODIFIED, .checked = FSIMG_META_MODIFIED,
	    .stored = { .modified = TIME(Y2K + 2, 0, FSIMG_TIME_WALL, 0) } },
	{ .id = "utc-basis-lost", .kind = FSIMG_ENTRY_FILE,
	    .source = { .valid = FSIMG_META_MODIFIED,
	    .modified = TIME(Y2K, 0, FSIMG_TIME_UTC, 0) },
	    .plan_status = FSIMG_OK, .losses = 1,
	    .loss_mask = FSIMG_META_MODIFIED, .checked = FSIMG_META_MODIFIED,
	    .stored = { .modified = TIME(Y2K, 0, FSIMG_TIME_WALL, 0) } },
	{ .id = "owner-omitted", .kind = FSIMG_ENTRY_FILE,
	    .source = { .valid = FSIMG_META_OWNER | FSIMG_META_MODE,
	    .uid = 1000, .gid = 100, .mode = 0640 },
	    .plan_status = FSIMG_OK, .losses = 2,
	    .loss_mask = FSIMG_META_OWNER | FSIMG_META_MODE },
	{ .id = "loss-forbidden", .kind = FSIMG_ENTRY_FILE,
	    .source = { .valid = FSIMG_META_OWNER, .uid = 1000 },
	    .forbid_loss = true, .plan_status = FSIMG_EUNSUPPORTED },
	{ .id = "loss-required", .kind = FSIMG_ENTRY_FILE,
	    .source = { .valid = FSIMG_META_MODIFIED,
	    .required = FSIMG_META_MODIFIED,
	    .modified = TIME(Y2K + 3, 0, FSIMG_TIME_WALL, 0) },
	    .plan_status = FSIMG_EUNSUPPORTED },
	{ .id = "symlink-kind", .kind = FSIMG_ENTRY_SYMLINK,
	    .plan_status = FSIMG_EUNSUPPORTED },
	{ .id = "fifo-kind", .kind = FSIMG_ENTRY_FIFO,
	    .plan_status = FSIMG_EUNSUPPORTED },
};

/*
 * exFAT stores UTC instants with quarter-hour offsets; a known zero offset
 * reads back as UTC. Creation and modification have 10 ms resolution and
 * access two seconds (design-exfat.md, design-metadata-loss.md).
 */
static const contract_metadata_case_t exfat_metadata[] = {
	{ .id = "offset-time-exact", .kind = FSIMG_ENTRY_FILE,
	    .source = { .valid = FSIMG_META_MODIFIED | FSIMG_META_CREATED,
	    .modified = TIME(Y2K, 120000000, FSIMG_TIME_OFFSET, 60),
	    .created = TIME(Y2K + 1, 0, FSIMG_TIME_UTC, 0) },
	    .plan_status = FSIMG_OK,
	    .checked = FSIMG_META_MODIFIED | FSIMG_META_CREATED,
	    .stored = {
	    .modified = TIME(Y2K, 120000000, FSIMG_TIME_OFFSET, 60),
	    .created = TIME(Y2K + 1, 0, FSIMG_TIME_UTC, 0) } },
	{ .id = "access-odd-second-rounds", .kind = FSIMG_ENTRY_FILE,
	    .source = { .valid = FSIMG_META_ACCESSED,
	    .accessed = TIME(Y2K + 3, 0, FSIMG_TIME_UTC, 0) },
	    .plan_status = FSIMG_OK, .losses = 1,
	    .loss_mask = FSIMG_META_ACCESSED, .checked = FSIMG_META_ACCESSED,
	    .stored = { .accessed = TIME(Y2K + 2, 0, FSIMG_TIME_UTC, 0) } },
	{ .id = "owner-omitted", .kind = FSIMG_ENTRY_FILE,
	    .source = { .valid = FSIMG_META_OWNER | FSIMG_META_CHANGED,
	    .uid = 1000, .gid = 100,
	    .changed = TIME(Y2K, 0, FSIMG_TIME_UTC, 0) },
	    .plan_status = FSIMG_OK, .losses = 2,
	    .loss_mask = FSIMG_META_OWNER | FSIMG_META_CHANGED },
	{ .id = "loss-forbidden", .kind = FSIMG_ENTRY_FILE,
	    .source = { .valid = FSIMG_META_MODE, .mode = 0640 },
	    .forbid_loss = true, .plan_status = FSIMG_EUNSUPPORTED },
	{ .id = "loss-required", .kind = FSIMG_ENTRY_FILE,
	    .source = { .valid = FSIMG_META_OWNER,
	    .required = FSIMG_META_OWNER, .uid = 1000 },
	    .plan_status = FSIMG_EUNSUPPORTED },
	{ .id = "symlink-kind", .kind = FSIMG_ENTRY_SYMLINK,
	    .plan_status = FSIMG_EUNSUPPORTED },
	{ .id = "fifo-kind", .kind = FSIMG_ENTRY_FIFO,
	    .plan_status = FSIMG_EUNSUPPORTED },
};

/*
 * QNX4 and QNX6 store the POSIX owner, permission bits and whole-second UTC
 * times of every kind; a fraction is dropped, not rounded, and is one loss.
 * QNX4 identifiers above 65534 are one owner loss. Neither stores generic
 * attributes (design-qnx4.md, design-qnx6.md, design-metadata-loss.md).
 */
#define QNX_EXACT_CASE { .id = "posix-exact", .kind = FSIMG_ENTRY_FILE, \
	.source = { .valid = FSIMG_META_OWNER | FSIMG_META_MODE | \
	FSIMG_META_MODIFIED, .uid = 1000, .gid = 100, .mode = 0640, \
	.modified = TIME(Y2K, 0, FSIMG_TIME_UTC, 0) }, \
	.plan_status = FSIMG_OK, .checked = FSIMG_META_OWNER | \
	FSIMG_META_MODE | FSIMG_META_MODIFIED, \
	.stored = { .uid = 1000, .gid = 100, .mode = 0640, \
	.modified = TIME(Y2K, 0, FSIMG_TIME_UTC, 0) } }
#define QNX_FRACTION_CASE { .id = "fraction-dropped", \
	.kind = FSIMG_ENTRY_FILE, .source = { .valid = FSIMG_META_MODIFIED, \
	.modified = TIME(Y2K, 999000000, FSIMG_TIME_UTC, 0) }, \
	.plan_status = FSIMG_OK, .losses = 1, .loss_mask = FSIMG_META_MODIFIED, \
	.checked = FSIMG_META_MODIFIED, \
	.stored = { .modified = TIME(Y2K, 0, FSIMG_TIME_UTC, 0) } }
#define QNX_KIND_CASES \
	{ .id = "symlink-kind", .kind = FSIMG_ENTRY_SYMLINK, \
	    .plan_status = FSIMG_OK }, \
	{ .id = "fifo-kind", .kind = FSIMG_ENTRY_FIFO, \
	    .plan_status = FSIMG_OK }
#define OMITTED_CASES(field) \
	{ .id = "unstored-field-omitted", .kind = FSIMG_ENTRY_FILE, \
	    .source = { .valid = (field), .attributes = FSIMG_ATTR_READONLY, \
	    .created = TIME(Y2K, 0, FSIMG_TIME_UTC, 0) }, \
	    .plan_status = FSIMG_OK, .losses = 1, .loss_mask = (field) }, \
	{ .id = "loss-forbidden", .kind = FSIMG_ENTRY_FILE, \
	    .source = { .valid = (field), .attributes = FSIMG_ATTR_READONLY, \
	    .created = TIME(Y2K, 0, FSIMG_TIME_UTC, 0) }, \
	    .forbid_loss = true, .plan_status = FSIMG_EUNSUPPORTED }, \
	{ .id = "loss-required", .kind = FSIMG_ENTRY_FILE, \
	    .source = { .valid = (field), .required = (field), \
	    .attributes = FSIMG_ATTR_READONLY, \
	    .created = TIME(Y2K, 0, FSIMG_TIME_UTC, 0) }, \
	    .plan_status = FSIMG_EUNSUPPORTED }

static const contract_metadata_case_t qnx6_metadata[] = {
	QNX_EXACT_CASE,
	QNX_FRACTION_CASE,
	QNX_KIND_CASES,
	OMITTED_CASES(FSIMG_META_ATTRIBUTES),
};

static const contract_metadata_case_t qnx4_metadata[] = {
	QNX_EXACT_CASE,
	QNX_FRACTION_CASE,
	{ .id = "wide-owner-once", .kind = FSIMG_ENTRY_FILE,
	    .source = { .valid = FSIMG_META_OWNER, .uid = 70000,
	    .gid = 70001 },
	    .plan_status = FSIMG_OK, .losses = 1,
	    .loss_mask = FSIMG_META_OWNER },
	QNX_KIND_CASES,
	OMITTED_CASES(FSIMG_META_ATTRIBUTES),
};

/*
 * ImageFS stores owner, permission bits and a whole-second UTC
 * modification time; creation is not stored (design-ifs.md).
 */
static const contract_metadata_case_t ifs_metadata[] = {
	QNX_EXACT_CASE,
	QNX_FRACTION_CASE,
	QNX_KIND_CASES,
	OMITTED_CASES(FSIMG_META_CREATED),
};

static const contract_entry_t qnx6_owned[] = {
	{ .path = "/.boot", .kind = FSIMG_ENTRY_DIRECTORY },
};

static const contract_entry_t qnx4_owned[] = {
	{ .path = "/.boot", .kind = FSIMG_ENTRY_FILE, .payload = PAYLOAD_EMPTY },
	{ .path = "/.altboot", .kind = FSIMG_ENTRY_FILE,
	    .payload = PAYLOAD_EMPTY },
};

#define WRITE_FLAGS	(FSIMG_WRITE_ALLOW_LOSS | FSIMG_WRITE_REPRODUCIBLE)
#define MIB		(UINT64_C(1) * 1024 * 1024)

/* FAT12, FAT16 and FAT32 are separate coverage keys although one
 * descriptor serves them; QNX6's big-endian byte order is an additional
 * profile beside its little-endian coverage profile.
 */
static const contract_fs_profile_t fs_profiles[] = {
	{ .id = "fat12", .format = "fat12", .coverage_profile = true,
	    .expected_capabilities = ALL_CAPABILITIES,
	    .write = { .flags = WRITE_FLAGS },
	    .metadata_cases = fat_metadata,
	    .metadata_case_count = COUNT(fat_metadata),
	    .max_fixture_bytes = 4 * MIB },
	{ .id = "fat16", .format = "fat16", .coverage_profile = true,
	    .expected_capabilities = ALL_CAPABILITIES,
	    .write = { .flags = WRITE_FLAGS },
	    .metadata_cases = fat_metadata,
	    .metadata_case_count = COUNT(fat_metadata),
	    .max_fixture_bytes = 8 * MIB },
	{ .id = "fat32", .format = "fat32", .coverage_profile = true,
	    .expected_capabilities = ALL_CAPABILITIES,
	    .write = { .flags = WRITE_FLAGS },
	    .metadata_cases = fat_metadata,
	    .metadata_case_count = COUNT(fat_metadata),
	    .max_fixture_bytes = 64 * MIB },
	{ .id = "exfat", .format = "exfat", .coverage_profile = true,
	    .expected_capabilities = ALL_CAPABILITIES,
	    .write = { .flags = WRITE_FLAGS },
	    .metadata_cases = exfat_metadata,
	    .metadata_case_count = COUNT(exfat_metadata),
	    .max_fixture_bytes = 8 * MIB },
	{ .id = "qnx6-le", .format = "qnx6", .coverage_profile = true,
	    .expected_capabilities = ALL_CAPABILITIES,
	    .write = { .flags = WRITE_FLAGS,
	    .byte_order = FSIMG_BYTE_ORDER_LITTLE },
	    .owned = qnx6_owned, .owned_count = COUNT(qnx6_owned),
	    .metadata_cases = qnx6_metadata,
	    .metadata_case_count = COUNT(qnx6_metadata),
	    .max_fixture_bytes = 8 * MIB },
	{ .id = "qnx6-be", .format = "qnx6", .coverage_profile = false,
	    .expected_capabilities = ALL_CAPABILITIES,
	    .write = { .flags = WRITE_FLAGS,
	    .byte_order = FSIMG_BYTE_ORDER_BIG },
	    .owned = qnx6_owned, .owned_count = COUNT(qnx6_owned),
	    .metadata_cases = qnx6_metadata,
	    .metadata_case_count = COUNT(qnx6_metadata),
	    .max_fixture_bytes = 8 * MIB },
	{ .id = "qnx4", .format = "qnx4", .coverage_profile = true,
	    .expected_capabilities = ALL_CAPABILITIES,
	    .write = { .flags = WRITE_FLAGS },
	    .owned = qnx4_owned, .owned_count = COUNT(qnx4_owned),
	    .metadata_cases = qnx4_metadata,
	    .metadata_case_count = COUNT(qnx4_metadata),
	    .max_fixture_bytes = 8 * MIB },
	{ .id = "ifs", .format = "ifs", .coverage_profile = true,
	    .expected_capabilities = ALL_CAPABILITIES,
	    .write = { .flags = WRITE_FLAGS },
	    .metadata_cases = ifs_metadata,
	    .metadata_case_count = COUNT(ifs_metadata),
	    .max_fixture_bytes = 8 * MIB },
};

/*
 * The context settings a scenario needs; zero keeps the generous fixture
 * default. cache_pages bounds the metadata cache so scratch operations are
 * reached by a small workload.
 */
typedef struct ctx_setup {
	uint64_t	 memory_bytes;
	uint64_t	 scratch_bytes;
	uint64_t	 max_entries;
	uint64_t	 max_work;
	uint64_t	 max_output_bytes;
	uint32_t	 max_depth;
	uint32_t	 cache_pages;
} ctx_setup_t;

#define FIXTURE_MEMORY	(UINT64_C(64) * 1024 * 1024)

/*
 * One case's apparatus: a private directory that is also the scratch
 * directory, a context with the fixture's cancellation and loss hooks, and
 * the provider's observations and faults. Handles are NULL until acquired.
 */
typedef struct fixture {
	const contract_fs_profile_t *profile;
	contract_dir_t	 dir;
	fsimg_ctx_t	*ctx;
	fsimg_stats_t	 baseline;
	fsimg_writer_t	*writer;
	const contract_metadata_case_t *metadata;
	/* Provider observations. */
	uint64_t	 zero_range_reads;
	bool		 read_seen[PROVIDER_IDS];
	/* Provider faults; zero disables each. */
	uint64_t	 changed_id;
	bool		 changed_after_read;
	uint64_t	 eof_id;
	uint64_t	 fail_id;
	bool		 provider_fault_fired;
	/* Cancellation: polls counts calls, and cancel_at_poll stops one. */
	uint64_t	 polls;
	uint64_t	 cancel_at_poll;
	/* Loss callback observations. */
	uint64_t	 loss_calls;
	uint32_t	 loss_mask;
} fixture_t;

static const contract_entry_t *
source_entry(uint64_t id)
{
	CONTRACT_CHECK(id >= 1 && id <= SOURCE_COUNT);
	return (&source_tree[id - 1]);
}

/* Whether a provider entry's byte at offset lies in a ZERO range. */
static bool
zero_at(const contract_entry_t *entry, uint64_t offset)
{
	return (entry->payload == PAYLOAD_ZERO_RANGE ||
	    (entry->payload == PAYLOAD_MIXED && offset >= MIXED_ZERO_START &&
	    offset < MIXED_ZERO_END));
}

/* The independent oracle of a file's bytes: zero for zero sources and ZERO
 * ranges, the recipe pattern otherwise.
 */
static unsigned char
expected_byte(const contract_entry_t *entry, uint64_t identity,
    uint64_t offset)
{
	if (entry->payload == PAYLOAD_TARGET)
		return ((unsigned char)LINK_TARGET[offset]);
	if (entry->payload == PAYLOAD_ZERO_SOURCE || zero_at(entry, offset))
		return (0);
	return (contract_pattern(identity, offset));
}

static fsimg_status_t
provider_validate(void *argument, uint64_t id, fsimg_error_t *error)
{
	fixture_t *fixture = argument;

	if (id == 0 || id != fixture->changed_id)
		return (FSIMG_OK);
	if (fixture->changed_after_read && !fixture->read_seen[id])
		return (FSIMG_OK);
	fixture->provider_fault_fired = true;
	return (fsimg_error_set(error, FSIMG_ESOURCE_CHANGED, 0,
	    FSIMG_OP_SOURCE, "contract source changed"));
}

static fsimg_status_t
provider_read(void *argument, uint64_t id, uint64_t offset, void *buffer,
    size_t length, size_t *transferred, fsimg_error_t *error FSIMG_UNUSED)
{
	fixture_t *fixture = argument;
	const contract_entry_t *entry;
	contract_entry_t meta;
	unsigned char *bytes = buffer;

	*transferred = 0;
	CONTRACT_CHECK(id >= 1 && id < PROVIDER_IDS);
	fixture->read_seen[id] = true;
	if (id == META_ID) {
		meta = (contract_entry_t){ .path = META_PATH,
		    .payload = fixture->metadata->kind == FSIMG_ENTRY_SYMLINK ?
		    PAYLOAD_TARGET : PAYLOAD_PATTERN };
		entry = &meta;
	} else
		entry = source_entry(id);
	/* Faults strike once the copy is under way. */
	if (offset >= 65536 && id == fixture->eof_id) {
		fixture->provider_fault_fired = true;
		return (FSIMG_OK);
	}
	if (offset >= 65536 && id == fixture->fail_id) {
		fixture->provider_fault_fired = true;
		return (FSIMG_EIO);
	}
	for (size_t i = 0; i < length; i++) {
		if (zero_at(entry, offset + i))
			fixture->zero_range_reads++;
		bytes[i] = expected_byte(entry, id, offset + i);
	}
	*transferred = length;
	return (FSIMG_OK);
}

static fsimg_status_t
provider_range(void *argument, uint64_t id, uint64_t offset,
    fsimg_range_t *range, fsimg_error_t *error FSIMG_UNUSED)
{
	const contract_entry_t *entry;
	uint64_t end;

	if (id == META_ID) {
		end = ((fixture_t *)argument)->metadata->kind ==
		    FSIMG_ENTRY_SYMLINK ? strlen(LINK_TARGET) : META_FILE_BYTES;
		CONTRACT_CHECK(offset < end);
		range->offset = offset;
		range->kind = FSIMG_RANGE_DATA;
		range->length = end - offset;
		return (FSIMG_OK);
	}
	entry = source_entry(id);
	end = entry->size;
	CONTRACT_CHECK(offset < end);
	if (entry->payload == PAYLOAD_MIXED && offset < MIXED_ZERO_START)
		end = MIXED_ZERO_START;
	else if (entry->payload == PAYLOAD_MIXED && offset < MIXED_ZERO_END)
		end = MIXED_ZERO_END;
	range->offset = offset;
	range->length = end - offset;
	range->kind = zero_at(entry, offset) ? FSIMG_RANGE_ZERO :
	    FSIMG_RANGE_DATA;
	return (FSIMG_OK);
}

static bool
cancelled(void *argument)
{
	fixture_t *fixture = argument;

	fixture->polls++;
	return (fixture->cancel_at_poll != 0 &&
	    fixture->polls >= fixture->cancel_at_poll);
}

static void
record_loss(void *argument, const char *path, uint32_t fields)
{
	fixture_t *fixture = argument;

	CONTRACT_CHECK(path != NULL && path[0] == '/');
	CONTRACT_CHECK(fields != 0);
	fixture->loss_calls++;
	fixture->loss_mask |= fields;
}

static void
fixture_open(fixture_t *fixture, const contract_fs_profile_t *profile,
    const ctx_setup_t *setup)
{
	static const ctx_setup_t defaults;
	fsimg_options_t options;
	fsimg_error_t error;

	if (setup == NULL)
		setup = &defaults;
	bzero(fixture, sizeof(*fixture));
	fixture->profile = profile;
	contract_dir_open(&fixture->dir);
	fsimg_options_init(&options);
	options.memory_bytes = setup->memory_bytes != 0 ? setup->memory_bytes :
	    FIXTURE_MEMORY;
	if (setup->scratch_bytes != 0)
		options.scratch_bytes = setup->scratch_bytes;
	if (setup->max_entries != 0)
		options.max_entries = setup->max_entries;
	options.max_work = setup->max_work;
	options.max_output_bytes = setup->max_output_bytes;
	if (setup->max_depth != 0)
		options.max_depth = setup->max_depth;
	options.max_open_files = 16;
	options.scratch_directory_fd = fixture->dir.fd;
	options.cancelled = cancelled;
	options.cancel_argument = fixture;
	options.loss = record_loss;
	options.loss_argument = fixture;
	CONTRACT_STATUS(fsimg_ctx_new(&options, &fixture->ctx, &error), FSIMG_OK,
	    &error);
	if (setup->cache_pages != 0)
		CONTRACT_STATUS(fsimg_test_meta_cache(fixture->ctx,
		    setup->cache_pages, &error), FSIMG_OK, &error);
	contract_baseline(fixture->ctx, &fixture->baseline);
}

/* Free the writer, prove every charge returned to the baseline, then
 * destroy the context and the (necessarily empty) directory.
 */
static void
fixture_close(fixture_t *fixture)
{
	contract_phase("cleanup", 0);
	fsimg_writer_free(fixture->writer);
	fixture->writer = NULL;
	contract_check_released(fixture->ctx, &fixture->baseline);
	fsimg_ctx_free(fixture->ctx);
	contract_dir_close(&fixture->dir);
}

static void
write_options(const fixture_t *fixture, fsimg_write_options_t *options)
{
	fsimg_write_options_init(options);
	options->flags = fixture->profile->write.flags;
	options->byte_order = fixture->profile->write.byte_order;
}

static fsimg_status_t
add_entry(fixture_t *fixture, const contract_entry_t *entry, uint64_t id,
    const fsimg_metadata_t *metadata, fsimg_error_t *error)
{
	fsimg_entry_t record;
	fsimg_source_t source;

	bzero(&record, sizeof(record));
	record.path = entry->path;
	record.kind = entry->kind;
	record.size = entry->size;
	if (metadata != NULL)
		record.metadata = *metadata;
	if (entry->kind == FSIMG_ENTRY_DIRECTORY ||
	    entry->kind == FSIMG_ENTRY_FIFO)
		return (fsimg_writer_add(fixture->writer, &record, NULL, error));
	source.kind = entry->payload == PAYLOAD_ZERO_SOURCE ?
	    FSIMG_SOURCE_ZERO : FSIMG_SOURCE_PROVIDER;
	source.id = entry->payload == PAYLOAD_ZERO_SOURCE ? 0 : id;
	return (fsimg_writer_add(fixture->writer, &record, &source, error));
}

/* The entry a metadata case adds at META_PATH. */
static contract_entry_t
metadata_entry(const contract_metadata_case_t *test)
{
	contract_entry_t entry = { .path = META_PATH, .kind = test->kind };

	if (test->kind == FSIMG_ENTRY_FILE) {
		entry.size = META_FILE_BYTES;
		entry.payload = PAYLOAD_PATTERN;
	} else if (test->kind == FSIMG_ENTRY_SYMLINK) {
		entry.size = strlen(LINK_TARGET);
		entry.payload = PAYLOAD_TARGET;
	}
	return (entry);
}

/* Create the profile's writer and collect the common tree, plus a metadata
 * case's entry when one is given; the first failure is returned.
 */
static fsimg_status_t
writer_collect(fixture_t *fixture, const contract_metadata_case_t *test,
    fsimg_error_t *error)
{
	fsimg_write_options_t options;
	fsimg_source_ops_t source_ops;
	contract_entry_t meta;
	fsimg_status_t status;

	write_options(fixture, &options);
	/* Reproducible mode substitutes its epoch for present timestamps,
	 * itself a loss on FAT (design-fat-writer.md); metadata cases test
	 * the values they supply.
	 */
	if (test != NULL) {
		options.flags &= ~FSIMG_WRITE_REPRODUCIBLE;
		if (test->forbid_loss)
			options.flags &= ~FSIMG_WRITE_ALLOW_LOSS;
	}
	fixture->metadata = test;
	status = fsimg_writer_new(fixture->ctx, fixture->profile->format,
	    &options, &fixture->writer, error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_source_ops_init(&source_ops);
	source_ops.validate = provider_validate;
	source_ops.read_at = provider_read;
	source_ops.next_range = provider_range;
	status = fsimg_writer_set_sources(fixture->writer, &source_ops, fixture,
	    error);
	for (uint64_t id = 1; id <= SOURCE_COUNT && status == FSIMG_OK; id++)
		status = add_entry(fixture, source_entry(id), id, NULL, error);
	if (status == FSIMG_OK && test != NULL) {
		meta = metadata_entry(test);
		status = add_entry(fixture, &meta, META_ID, &test->source, error);
	}
	return (status);
}

static void
writer_ready(fixture_t *fixture)
{
	fsimg_error_t error;

	CONTRACT_STATUS(writer_collect(fixture, NULL, &error), FSIMG_OK, &error);
}

/* Plan, and allocate a volume of the planned length within the profile's
 * fixture ceiling, filled with fill.
 */
static void
plan_volume(fixture_t *fixture, contract_volume_t *volume, unsigned char fill)
{
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_error_t error;

	contract_phase("plan", 0);
	CONTRACT_STATUS(fsimg_writer_plan(fixture->writer, &plan, &error),
	    FSIMG_OK, &error);
	CONTRACT_STATUS(fsimg_plan_get_info(plan, &info, &error), FSIMG_OK,
	    &error);
	CONTRACT_CHECK(info.volume_bytes != 0);
	CONTRACT_CHECK(info.volume_bytes <= fixture->profile->max_fixture_bytes);
	CONTRACT_CHECK(info.image.disk_bytes == info.volume_bytes);
	contract_volume_init(volume, info.volume_bytes, fill);
}

/* Emit into the volume through its slice. */
static fsimg_status_t
emit_into(fixture_t *fixture, contract_volume_t *volume, fsimg_error_t *error)
{
	fsimg_io_t *backing, *slice;
	fsimg_status_t status;

	slice = contract_volume_open(fixture->ctx, volume, true, &backing);
	contract_phase("emit", contract_run.fault);
	status = fsimg_writer_emit(fixture->writer, slice, error);
	contract_volume_close(slice, backing);
	return (status);
}

/*
 * A failed emission: the exact status, a released source handle and a
 * failed writer that refuses another attempt. An output the emission
 * reached is incomplete; one it reports absent was refused before any
 * byte or zero range reached it.
 */
static void
check_emit_failed(fixture_t *fixture, contract_volume_t *volume,
    fsimg_status_t status, fsimg_status_t expected, const fsimg_error_t *error)
{
	fsimg_error_t again;

	CONTRACT_STATUS(status, expected, error);
	if (error->output_state == FSIMG_OUTPUT_ABSENT)
		CONTRACT_CHECK(volume->writes == 0 && volume->zeroes == 0);
	else
		CONTRACT_CHECK(error->output_state == FSIMG_OUTPUT_INCOMPLETE);
	CONTRACT_CHECK(!contract_writer_source_active(fixture->writer));
	CONTRACT_CHECK(contract_writer_failed(fixture->writer));
	contract_volume_reset(volume);
	CONTRACT_STATUS(emit_into(fixture, volume, &again), FSIMG_ESTATE, &again);
	CONTRACT_CHECK(volume->writes == 0 && volume->zeroes == 0);
}

/*
 * Reading back. The expected tree is the common source tree plus the
 * entries the profile says the format adds; entries are matched by path,
 * so no iteration order is prescribed, duplicates fail and nothing may be
 * missing.
 */
typedef struct expected_tree {
	const contract_entry_t *entries[SOURCE_COUNT + 4];
	uint64_t	 identities[SOURCE_COUNT + 4];
	size_t		 count;
} expected_tree_t;

static void
expected_tree(const contract_fs_profile_t *profile, expected_tree_t *tree)
{
	bzero(tree, sizeof(*tree));
	for (size_t i = 0; i < SOURCE_COUNT; i++) {
		tree->entries[tree->count] = &source_tree[i];
		tree->identities[tree->count++] = i + 1;
	}
	CONTRACT_CHECK(profile->owned_count <= 4);
	for (size_t i = 0; i < profile->owned_count; i++) {
		tree->entries[tree->count] = &profile->owned[i];
		tree->identities[tree->count++] = 0;
	}
}

static size_t
expected_find(const expected_tree_t *tree, const char *path)
{
	for (size_t i = 0; i < tree->count; i++) {
		if (strcmp(tree->entries[i]->path, path) == 0)
			return (i);
	}
	fprintf(stderr, "unexpected entry %s\n", path);
	CONTRACT_CHECK(!"entry is expected");
	return (0);
}

/* Read the whole current file in odd-sized pieces and compare it. */
static void
check_content(fsimg_reader_t *reader, const contract_entry_t *entry,
    uint64_t identity)
{
	unsigned char buffer[7919];
	uint64_t offset;
	size_t length;
	fsimg_error_t error;

	for (offset = 0; offset < entry->size; offset += length) {
		CONTRACT_STATUS(fsimg_reader_read_at(reader, offset, buffer,
		    sizeof(buffer), &length, &error), FSIMG_OK, &error);
		CONTRACT_CHECK(length > 0 && length <= entry->size - offset);
		for (size_t i = 0; i < length; i++)
			CONTRACT_CHECK(buffer[i] ==
			    expected_byte(entry, identity, offset + i));
	}
	CONTRACT_STATUS(fsimg_reader_read_at(reader, entry->size, buffer,
	    sizeof(buffer), &length, &error), FSIMG_OK, &error);
	CONTRACT_CHECK(length == 0);
}

/* Traverse the reader to its end, checking every entry and its bytes. */
static void
check_tree(fsimg_reader_t *reader, const contract_fs_profile_t *profile)
{
	expected_tree_t tree;
	bool seen[SOURCE_COUNT + 4];
	const fsimg_entry_t *entry;
	fsimg_error_t error;
	fsimg_status_t status;
	size_t index;

	expected_tree(profile, &tree);
	bzero(seen, sizeof(seen));
	while ((status = fsimg_reader_next(reader, &entry, &error)) == FSIMG_OK) {
		index = expected_find(&tree, entry->path);
		CONTRACT_CHECK(!seen[index]);
		seen[index] = true;
		CONTRACT_CHECK(entry->kind == tree.entries[index]->kind);
		CONTRACT_CHECK(entry->size == tree.entries[index]->size);
		if (entry->kind == FSIMG_ENTRY_FILE)
			check_content(reader, tree.entries[index],
			    tree.identities[index]);
	}
	CONTRACT_STATUS(status, FSIMG_DONE, &error);
	CONTRACT_CHECK(entry == NULL);
	for (size_t i = 0; i < tree.count; i++)
		CONTRACT_CHECK(seen[i]);
}

/*
 * The materialized reader fixture: the common writer's reproducible image
 * of the source tree, made in its own context and directory. It is
 * round-trip evidence (see the file comment).
 */
static void
materialize(const contract_fs_profile_t *profile, contract_volume_t *volume)
{
	fixture_t fixture;
	fsimg_error_t error;

	contract_phase("materialize", 0);
	fixture_open(&fixture, profile, NULL);
	writer_ready(&fixture);
	plan_volume(&fixture, volume, 0);
	CONTRACT_STATUS(emit_into(&fixture, volume, &error), FSIMG_OK, &error);
	fixture_close(&fixture);
	contract_volume_reset(volume);
}

static fsimg_status_t
reader_open(fixture_t *fixture, fsimg_io_t *input, fsimg_reader_t **reader,
    fsimg_error_t *error)
{
	fsimg_read_options_t options;

	fsimg_read_options_init(&options);
	options.format = fixture->profile->format;
	return (fsimg_reader_open(fixture->ctx, input, &options, reader, error));
}

static uint64_t
count_kind(const contract_fs_profile_t *profile, fsimg_entry_kind_t kind,
    uint64_t *bytes)
{
	expected_tree_t tree;
	uint64_t count = 0;

	expected_tree(profile, &tree);
	if (bytes != NULL)
		*bytes = 0;
	for (size_t i = 0; i < tree.count; i++) {
		if (tree.entries[i]->kind != kind)
			continue;
		count++;
		if (bytes != NULL)
			*bytes += tree.entries[i]->size;
	}
	return (count);
}

/*
 * Scenario: empty and small tree, writer side. The common tree is emitted
 * into a clean volume and read back through the public reader: exact
 * names, kinds, lengths and bytes; ZERO ranges are never read.
 */
static void
writer_small_tree(const contract_fs_profile_t *profile)
{
	fixture_t fixture;
	contract_volume_t volume;
	fsimg_io_t *backing, *slice;
	fsimg_reader_t *reader;
	fsimg_error_t error;

	if (!contract_begin(profile->id, "writer-small-tree"))
		return;
	fixture_open(&fixture, profile, NULL);
	writer_ready(&fixture);
	plan_volume(&fixture, &volume, 0);
	CONTRACT_STATUS(emit_into(&fixture, &volume, &error), FSIMG_OK, &error);
	CONTRACT_CHECK(fixture.zero_range_reads == 0);
	CONTRACT_CHECK(!contract_writer_source_active(fixture.writer));
	for (uint64_t id = 1; id <= SOURCE_COUNT; id++) {
		if (source_entry(id)->payload == PAYLOAD_EMPTY)
			CONTRACT_CHECK(!fixture.read_seen[id]);
	}

	contract_phase("read back", 0);
	slice = contract_volume_open(fixture.ctx, &volume, false, &backing);
	CONTRACT_STATUS(reader_open(&fixture, slice, &reader, &error), FSIMG_OK,
	    &error);
	check_tree(reader, profile);
	fsimg_reader_free(reader);
	contract_volume_close(slice, backing);
	fixture_close(&fixture);
	contract_volume_free(&volume);
	contract_end();
}

/*
 * Scenario: writer lifecycle. Planning seals collection, a repeated plan
 * returns the same object and information, emission happens once, and a
 * finished writer refuses further collection and emission.
 */
static void
writer_lifecycle(const contract_fs_profile_t *profile)
{
	static const contract_entry_t late = { .path = "/late",
	    .kind = FSIMG_ENTRY_DIRECTORY };
	fixture_t fixture;
	contract_volume_t volume;
	const fsimg_plan_t *first, *second;
	fsimg_plan_info_t first_info, second_info;
	fsimg_error_t error;

	if (!contract_begin(profile->id, "writer-lifecycle"))
		return;
	fixture_open(&fixture, profile, NULL);
	writer_ready(&fixture);
	plan_volume(&fixture, &volume, 0);
	CONTRACT_STATUS(fsimg_writer_plan(fixture.writer, &first, &error),
	    FSIMG_OK, &error);
	CONTRACT_STATUS(fsimg_writer_plan(fixture.writer, &second, &error),
	    FSIMG_OK, &error);
	CONTRACT_CHECK(first == second);
	CONTRACT_STATUS(fsimg_plan_get_info(first, &first_info, &error),
	    FSIMG_OK, &error);
	CONTRACT_STATUS(fsimg_plan_get_info(second, &second_info, &error),
	    FSIMG_OK, &error);
	CONTRACT_CHECK(bcmp(&first_info, &second_info, sizeof(first_info)) == 0);
	CONTRACT_CHECK(strcmp(first_info.format, profile->format) == 0);
	CONTRACT_CHECK(first_info.metadata_losses == 0);
	CONTRACT_CHECK(fixture.loss_calls == 0);

	contract_phase("collect after plan", 0);
	CONTRACT_STATUS(add_entry(&fixture, &late, 0, NULL, &error),
	    FSIMG_ESTATE, &error);
	CONTRACT_CHECK(!contract_writer_failed(fixture.writer));
	CONTRACT_STATUS(emit_into(&fixture, &volume, &error), FSIMG_OK, &error);

	contract_phase("emit again", 0);
	contract_volume_reset(&volume);
	CONTRACT_STATUS(emit_into(&fixture, &volume, &error), FSIMG_ESTATE,
	    &error);
	CONTRACT_CHECK(volume.writes == 0 && volume.zeroes == 0);
	CONTRACT_STATUS(add_entry(&fixture, &late, 0, NULL, &error),
	    FSIMG_ESTATE, &error);
	CONTRACT_STATUS(fsimg_writer_plan(fixture.writer, &second, &error),
	    FSIMG_OK, &error);
	CONTRACT_CHECK(second == first);
	fixture_close(&fixture);
	contract_volume_free(&volume);
	contract_end();
}

/*
 * Scenario: source consistency. A change is detected where no bytes move
 * (an empty file, a provider file of one ZERO range) and after bytes were
 * read; a provider that ends early or fails is an I/O failure. Each fails
 * the emission once, never yields an image, and leaves no source open.
 */
static void
writer_source_consistency(const contract_fs_profile_t *profile)
{
	static const struct {
		const char	*phase;
		uint64_t	 changed_id;
		bool		 changed_after_read;
		uint64_t	 eof_id;
		uint64_t	 fail_id;
		fsimg_status_t	 expected;
	} cases[] = {
		{ .phase = "changed empty source", .changed_id = ID_EMPTY,
		    .expected = FSIMG_ESOURCE_CHANGED },
		{ .phase = "changed zero-range source", .changed_id = ID_SPARSE,
		    .expected = FSIMG_ESOURCE_CHANGED },
		{ .phase = "changed after read", .changed_id = ID_LARGE,
		    .changed_after_read = true,
		    .expected = FSIMG_ESOURCE_CHANGED },
		{ .phase = "provider premature EOF", .eof_id = ID_LARGE,
		    .expected = FSIMG_EIO },
		{ .phase = "provider I/O failure", .fail_id = ID_MIXED,
		    .expected = FSIMG_EIO },
	};
	fixture_t fixture;
	contract_volume_t volume;
	fsimg_error_t error;
	fsimg_status_t status;

	if (!contract_begin(profile->id, "writer-source-consistency"))
		return;
	for (size_t i = 0; i < COUNT(cases); i++) {
		fixture_open(&fixture, profile, NULL);
		writer_ready(&fixture);
		plan_volume(&fixture, &volume, 0xa5);
		contract_phase(cases[i].phase, 0);
		fixture.changed_id = cases[i].changed_id;
		fixture.changed_after_read = cases[i].changed_after_read;
		fixture.eof_id = cases[i].eof_id;
		fixture.fail_id = cases[i].fail_id;
		status = emit_into(&fixture, &volume, &error);
		CONTRACT_CHECK(fixture.provider_fault_fired);
		check_emit_failed(&fixture, &volume, status, cases[i].expected,
		    &error);
		CONTRACT_CHECK(fixture.zero_range_reads == 0);
		fixture_close(&fixture);
		contract_volume_free(&volume);
	}
	contract_end();
}

/* Emit a fresh writer of the profile into volume; it must succeed. */
static void
emit_reference(const contract_fs_profile_t *profile, const ctx_setup_t *setup,
    contract_volume_t *volume, unsigned char fill)
{
	fixture_t fixture;
	fsimg_error_t error;

	fixture_open(&fixture, profile, setup);
	writer_ready(&fixture);
	plan_volume(&fixture, volume, fill);
	CONTRACT_STATUS(emit_into(&fixture, volume, &error), FSIMG_OK, &error);
	CONTRACT_CHECK(fixture.zero_range_reads == 0);
	fixture_close(&fixture);
}

static bool
same_image(contract_volume_t *left, contract_volume_t *right)
{
	return (left->length == right->length &&
	    bcmp(contract_volume_bytes(left), contract_volume_bytes(right),
	    (size_t)left->length) == 0);
}

/*
 * Scenario: defined output. Stale sink bytes cannot reach a successful
 * image: a clean sink, poisoned sinks with and without a zero-range
 * callback and a sink accepting only short transfers give identical bytes.
 * (Zero padding itself is the format-local checkers' assertion.)
 */
static void
writer_defined_output(const contract_fs_profile_t *profile)
{
	static const struct {
		const char	*phase;
		unsigned char	 fill;
		bool		 no_zero_range;
		size_t		 max_transfer;
	} sinks[] = {
		{ .phase = "poisoned sink", .fill = 0xa5 },
		{ .phase = "poisoned sink without zero_range", .fill = 0x5a,
		    .no_zero_range = true },
		{ .phase = "short transfers", .fill = 0xa5, .max_transfer = 1000 },
	};
	fixture_t fixture;
	contract_volume_t clean, volume;
	fsimg_error_t error;

	if (!contract_begin(profile->id, "writer-defined-output"))
		return;
	contract_phase("clean sink", 0);
	emit_reference(profile, NULL, &clean, 0);
	for (size_t i = 0; i < COUNT(sinks); i++) {
		fixture_open(&fixture, profile, NULL);
		writer_ready(&fixture);
		plan_volume(&fixture, &volume, sinks[i].fill);
		contract_phase(sinks[i].phase, 0);
		volume.no_zero_range = sinks[i].no_zero_range;
		volume.max_transfer = sinks[i].max_transfer;
		CONTRACT_STATUS(emit_into(&fixture, &volume, &error), FSIMG_OK,
		    &error);
		if (sinks[i].no_zero_range)
			CONTRACT_CHECK(volume.zeroes == 0);
		CONTRACT_CHECK(same_image(&clean, &volume));
		CONTRACT_CHECK(contract_volume_guards_intact(&volume));
		fixture_close(&fixture);
		contract_volume_free(&volume);
	}
	contract_volume_free(&clean);
	contract_end();
}

/* Read a published image file back from the fixture directory into a
 * volume of the same length, then remove it.
 */
static void
load_published(fixture_t *fixture, const char *name, contract_volume_t *volume,
    uint64_t length)
{
	struct stat st;
	unsigned char *bytes;
	uint64_t done;
	ssize_t count;
	int fd;

	fd = openat(fixture->dir.fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	CONTRACT_CHECK(fd != -1);
	CONTRACT_CHECK(fstat(fd, &st) == 0 && S_ISREG(st.st_mode));
	CONTRACT_CHECK((uint64_t)st.st_size == length);
	contract_volume_init(volume, length, 0);
	bytes = contract_volume_bytes(volume);
	for (done = 0; done < length; done += (uint64_t)count) {
		count = read(fd, bytes + done, (size_t)(length - done));
		CONTRACT_CHECK(count > 0);
	}
	CONTRACT_CHECK(close(fd) == 0);
	CONTRACT_CHECK(unlinkat(fixture->dir.fd, name, 0) == 0);
}

/*
 * Scenario: reproducibility. Identical inputs and identifiers yield the
 * same bytes from independent contexts, from a metadata cache small
 * enough to page through scratch, and through the published-file sink.
 */
static void
writer_reproducibility(const contract_fs_profile_t *profile)
{
	static const ctx_setup_t small_cache = { .cache_pages = 2 };
	fixture_t fixture;
	contract_volume_t first, second;
	fsimg_error_t error;

	if (!contract_begin(profile->id, "writer-reproducibility"))
		return;
	contract_phase("first context", 0);
	emit_reference(profile, NULL, &first, 0);
	contract_phase("second context, small cache", 0);
	emit_reference(profile, &small_cache, &second, 0x5a);
	CONTRACT_CHECK(same_image(&first, &second));
	contract_volume_free(&second);

	contract_phase("published file sink", 0);
	fixture_open(&fixture, profile, NULL);
	writer_ready(&fixture);
	CONTRACT_STATUS(fsimg_writer_write_at(fixture.writer, fixture.dir.fd,
	    "image", &error), FSIMG_OK, &error);
	CONTRACT_CHECK(fixture.zero_range_reads == 0);
	load_published(&fixture, "image", &second, first.length);
	CONTRACT_CHECK(same_image(&first, &second));
	fixture_close(&fixture);
	contract_volume_free(&first);
	contract_volume_free(&second);
	contract_end();
}

/*
 * Scenario: sink failure. A write failing first, midway and last confirms
 * half its bytes and the error reports exactly that confirmed end; a write
 * making no progress, a zero-range failure and a flush failure each end the
 * emission with FSIMG_EIO. A sink of the wrong length and a read-only sink
 * are refused before any write.
 */
static void
writer_sink_failure(const contract_fs_profile_t *profile)
{
	fixture_t fixture;
	contract_volume_t volume;
	fsimg_io_t *backing, *slice;
	fsimg_error_t error;
	fsimg_status_t status;
	uint64_t length, writes, points[3];

	if (!contract_begin(profile->id, "writer-sink-failure"))
		return;
	contract_phase("measure", 0);
	emit_reference(profile, NULL, &volume, 0);
	contract_volume_free(&volume);
	fixture_open(&fixture, profile, NULL);
	writer_ready(&fixture);
	plan_volume(&fixture, &volume, 0);
	CONTRACT_STATUS(emit_into(&fixture, &volume, &error), FSIMG_OK, &error);
	writes = volume.writes;
	fixture_close(&fixture);
	contract_volume_free(&volume);
	CONTRACT_CHECK(writes > 2);

	points[0] = 1;
	points[1] = writes / 2;
	points[2] = writes;
	for (size_t i = 0; i < 3; i++) {
		fixture_open(&fixture, profile, NULL);
		writer_ready(&fixture);
		plan_volume(&fixture, &volume, 0xa5);
		contract_phase("write failure", points[i]);
		volume.fail_write = points[i];
		status = emit_into(&fixture, &volume, &error);
		CONTRACT_CHECK(volume.fault_fired);
		CONTRACT_CHECK(error.offset_valid &&
		    error.offset == volume.confirmed_end);
		check_emit_failed(&fixture, &volume, status, FSIMG_EIO, &error);
		fixture_close(&fixture);
		contract_volume_free(&volume);
	}

	for (unsigned c = 0; c < 3; c++) {
		fixture_open(&fixture, profile, NULL);
		writer_ready(&fixture);
		plan_volume(&fixture, &volume, 0xa5);
		if (c == 0) {
			contract_phase("write without progress", writes / 2);
			volume.stall_write = writes / 2;
		} else if (c == 1) {
			contract_phase("zero-range failure", 1);
			volume.fail_zero = 1;
		} else {
			contract_phase("flush failure", 0);
			volume.fail_flush = true;
		}
		status = emit_into(&fixture, &volume, &error);
		CONTRACT_CHECK(volume.fault_fired);
		check_emit_failed(&fixture, &volume, status, FSIMG_EIO, &error);
		fixture_close(&fixture);
		contract_volume_free(&volume);
	}

	/* Preflight refusals, one cause each: a writable sink a sector
	 * longer than planned, and a read-only sink of the planned length.
	 * Nothing is written and the output is absent.
	 */
	for (unsigned c = 0; c < 2; c++) {
		fixture_open(&fixture, profile, NULL);
		writer_ready(&fixture);
		plan_volume(&fixture, &volume, 0xa5);
		length = volume.length;
		contract_volume_free(&volume);
		if (c == 0) {
			contract_phase("sink of the wrong length", 0);
			contract_volume_init(&volume, length + 512, 0xa5);
			slice = contract_volume_open(fixture.ctx, &volume, true,
			    &backing);
		} else {
			contract_phase("read-only sink", 0);
			contract_volume_init(&volume, length, 0xa5);
			slice = contract_volume_open(fixture.ctx, &volume, false,
			    &backing);
		}
		status = fsimg_writer_emit(fixture.writer, slice, &error);
		contract_volume_close(slice, backing);
		CONTRACT_STATUS(status, c == 0 ? FSIMG_EINVAL : FSIMG_EUNSUPPORTED,
		    &error);
		CONTRACT_CHECK(error.output_state == FSIMG_OUTPUT_ABSENT);
		CONTRACT_CHECK(volume.writes == 0 && volume.zeroes == 0);
		CONTRACT_CHECK(!contract_writer_source_active(fixture.writer));
		fixture_close(&fixture);
		contract_volume_free(&volume);
	}
	contract_end();
}

/* The phase of a writer operation a fault is armed for. */
typedef enum writer_phase {
	PHASE_COLLECT,
	PHASE_PLAN,
	PHASE_EMIT
} writer_phase_t;

static const char *const writer_phase_names[] = { "collect", "plan", "emit" };

typedef enum fault_kind {
	FAULT_ALLOCATION,
	FAULT_META_OPEN = FSIMG_META_OPEN,
	FAULT_META_RESIZE = FSIMG_META_RESIZE,
	FAULT_META_READ = FSIMG_META_READ,
	FAULT_META_WRITE = FSIMG_META_WRITE
} fault_kind_t;

static void
arm(fixture_t *fixture, fault_kind_t kind, uint64_t position)
{
	if (kind == FAULT_ALLOCATION)
		contract_arm_alloc(fixture->ctx, position);
	else
		contract_arm_meta(fixture->ctx, (int)kind, position);
}

static bool
fired(fixture_t *fixture, fault_kind_t kind)
{
	if (kind == FAULT_ALLOCATION)
		return (contract_alloc_fired(fixture->ctx));
	return (contract_meta_fired(fixture->ctx));
}

/*
 * One writer phase's library call, with the fault armed at position for
 * that call alone (zero arms nothing), not for the test's own adapters.
 * Emission writes into a poisoned volume of the planned length, which
 * this allocates; *hit says whether the armed fault fired.
 */
static fsimg_status_t
writer_phase_call(fixture_t *fixture, writer_phase_t phase, fault_kind_t kind,
    uint64_t position, contract_volume_t *volume, bool *hit,
    fsimg_error_t *error)
{
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_io_t *backing, *slice;
	fsimg_status_t status;

	switch (phase) {
	case PHASE_COLLECT:
		arm(fixture, kind, position);
		status = writer_collect(fixture, NULL, error);
		break;
	case PHASE_PLAN:
		arm(fixture, kind, position);
		status = fsimg_writer_plan(fixture->writer, &plan, error);
		break;
	case PHASE_EMIT:
		CONTRACT_STATUS(fsimg_writer_plan(fixture->writer, &plan, error),
		    FSIMG_OK, error);
		CONTRACT_STATUS(fsimg_plan_get_info(plan, &info, error),
		    FSIMG_OK, error);
		contract_volume_init(volume, info.volume_bytes, 0xa5);
		slice = contract_volume_open(fixture->ctx, volume, true,
		    &backing);
		arm(fixture, kind, position);
		status = fsimg_writer_emit(fixture->writer, slice, error);
		*hit = position != 0 && fired(fixture, kind);
		arm(fixture, kind, 0);
		contract_volume_close(slice, backing);
		return (status);
	default:
		CONTRACT_CHECK(!"writer phase");
		return (FSIMG_EINVAL);
	}
	*hit = position != 0 && fired(fixture, kind);
	arm(fixture, kind, 0);
	return (status);
}

/*
 * One complete writer run: collection, planning, then emission into a
 * poisoned volume, with one fault armed at position in the named phase.
 * A run whose fault fired must fail with expected; one whose fault never
 * fired must succeed with the reference image. A failed plan leaves a
 * failed writer, and a failed emission is checked by check_emit_failed().
 * Resources return to the baseline on every outcome. Returns whether the
 * fault fired.
 */
static bool
writer_faulted_run(const contract_fs_profile_t *profile,
    const ctx_setup_t *setup, fault_kind_t kind, writer_phase_t phase,
    uint64_t position, fsimg_status_t expected, contract_volume_t *reference)
{
	fixture_t fixture;
	contract_volume_t volume;
	const fsimg_plan_t *plan;
	fsimg_error_t error;
	fsimg_status_t status = FSIMG_OK;
	writer_phase_t current;
	bool hit = false, fault_fired = false;

	fixture_open(&fixture, profile, setup);
	volume.bytes = NULL;
	for (current = PHASE_COLLECT; current <= PHASE_EMIT; current++) {
		contract_phase(writer_phase_names[current], position);
		status = writer_phase_call(&fixture, current, kind,
		    current == phase ? position : 0, &volume, &hit, &error);
		fault_fired = fault_fired || hit;
		if (status != FSIMG_OK)
			break;
	}
	contract_phase(writer_phase_names[phase], position);
	if (status == FSIMG_OK) {
		CONTRACT_CHECK(!fault_fired);
		CONTRACT_CHECK(same_image(reference, &volume));
	} else if (current == PHASE_EMIT) {
		check_emit_failed(&fixture, &volume, status,
		    fault_fired ? expected : FSIMG_OK, &error);
	} else {
		CONTRACT_STATUS(status, fault_fired ? expected : FSIMG_OK,
		    &error);
		if (current == PHASE_PLAN) {
			CONTRACT_CHECK(contract_writer_failed(fixture.writer));
			CONTRACT_STATUS(fsimg_writer_plan(fixture.writer, &plan,
			    &error), FSIMG_ESTATE, &error);
		}
	}
	contract_volume_free(&volume);
	fixture_close(&fixture);
	return (fault_fired);
}

/*
 * Sweep one fault kind over every position a phase reaches: each position
 * that fires fails with the fault's own status, and the first position
 * that does not fire is the unfaulted final run, which succeeds exactly.
 * No fired fault is accepted as success: these small fixtures reach no
 * documented bounded fallback (a refused optional metadata-cache chunk),
 * and one that became reachable needs a targeted case identifying it
 * before a sweep may accept its success. Returns the positions that fired.
 */
static uint64_t
writer_sweep(const contract_fs_profile_t *profile, const ctx_setup_t *setup,
    fault_kind_t kind, writer_phase_t phase, fsimg_status_t expected,
    contract_volume_t *reference)
{
	uint64_t position;

	for (position = 1; position <= CONTRACT_SWEEP_MAX; position++) {
		if (writer_faulted_run(profile, setup, kind, phase, position,
		    expected, reference))
			continue;
		if (getenv("CONTRACT_VERBOSE") != NULL)
			printf("%s %s fault %d: %llu positions\n", profile->id,
			    writer_phase_names[phase], kind,
			    (unsigned long long)position - 1);
		return (position - 1);
	}
	fprintf(stderr, "sweep exceeded %u positions\n", CONTRACT_SWEEP_MAX);
	CONTRACT_CHECK(position <= CONTRACT_SWEEP_MAX);
	return (0);
}

/* Scenario: allocation failure in each writer phase. */
static void
writer_allocation(const contract_fs_profile_t *profile)
{
	contract_volume_t reference;

	if (!contract_begin(profile->id, "writer-allocation"))
		return;
	emit_reference(profile, NULL, &reference, 0);
	/* Every phase allocates, so every sweep reaches some position. */
	for (writer_phase_t phase = PHASE_COLLECT; phase <= PHASE_EMIT; phase++)
		CONTRACT_CHECK(writer_sweep(profile, NULL, FAULT_ALLOCATION,
		    phase, FSIMG_ENOMEM, &reference) != 0);
	contract_volume_free(&reference);
	contract_end();
}

/*
 * Scenario: scratch failure. With a two-page metadata cache the small
 * tree pages through scratch, so each scratch operation is reached; every
 * injected failure is FSIMG_EIO with scratch released.
 */
static void
writer_scratch(const contract_fs_profile_t *profile)
{
	static const ctx_setup_t small_cache = { .cache_pages = 2 };
	static const fault_kind_t kinds[] = { FAULT_META_OPEN,
	    FAULT_META_RESIZE, FAULT_META_READ, FAULT_META_WRITE };
	contract_volume_t reference;
	uint64_t reached;

	if (!contract_begin(profile->id, "writer-scratch"))
		return;
	emit_reference(profile, &small_cache, &reference, 0);
	for (size_t k = 0; k < COUNT(kinds); k++) {
		reached = 0;
		for (writer_phase_t phase = PHASE_COLLECT; phase <= PHASE_EMIT;
		    phase++)
			reached += writer_sweep(profile, &small_cache, kinds[k],
			    phase, FSIMG_EIO, &reference);
		/* The small cache makes the workload reach every operation. */
		CONTRACT_CHECK(reached != 0);
	}
	contract_volume_free(&reference);
	contract_end();
}

/*
 * Scenario: writer cancellation, at the first poll of planning, the first
 * poll of emission, and the middle and last polls of emission.
 */
static void
writer_cancellation(const contract_fs_profile_t *profile)
{
	fixture_t fixture;
	contract_volume_t volume;
	const fsimg_plan_t *plan;
	fsimg_error_t error;
	uint64_t polls, points[3];

	if (!contract_begin(profile->id, "writer-cancellation"))
		return;
	contract_phase("measure", 0);
	fixture_open(&fixture, profile, NULL);
	writer_ready(&fixture);
	plan_volume(&fixture, &volume, 0);
	fixture.polls = 0;
	CONTRACT_STATUS(emit_into(&fixture, &volume, &error), FSIMG_OK, &error);
	polls = fixture.polls;
	fixture_close(&fixture);
	contract_volume_free(&volume);
	CONTRACT_CHECK(polls > 2);

	contract_phase("plan, first poll", 1);
	fixture_open(&fixture, profile, NULL);
	writer_ready(&fixture);
	fixture.polls = 0;
	fixture.cancel_at_poll = 1;
	CONTRACT_STATUS(fsimg_writer_plan(fixture.writer, &plan, &error),
	    FSIMG_ECANCELLED, &error);
	CONTRACT_CHECK(contract_writer_failed(fixture.writer));
	fixture_close(&fixture);

	points[0] = 1;
	points[1] = polls / 2;
	points[2] = polls;
	for (size_t i = 0; i < 3; i++) {
		fixture_open(&fixture, profile, NULL);
		writer_ready(&fixture);
		plan_volume(&fixture, &volume, 0xa5);
		contract_phase("emit", points[i]);
		fixture.polls = 0;
		fixture.cancel_at_poll = points[i];
		check_emit_failed(&fixture, &volume,
		    emit_into(&fixture, &volume, &error), FSIMG_ECANCELLED,
		    &error);
		fixture_close(&fixture);
		contract_volume_free(&volume);
	}
	contract_end();
}

/* Collect, plan and emit, returning the first failure. */
static fsimg_status_t
writer_whole_run(fixture_t *fixture, contract_volume_t *volume,
    bool *have_volume, fsimg_error_t *error)
{
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_status_t status;

	*have_volume = false;
	status = writer_collect(fixture, NULL, error);
	if (status == FSIMG_OK)
		status = fsimg_writer_plan(fixture->writer, &plan, error);
	if (status != FSIMG_OK)
		return (status);
	CONTRACT_STATUS(fsimg_plan_get_info(plan, &info, error), FSIMG_OK,
	    error);
	contract_volume_init(volume, info.volume_bytes, 0xa5);
	*have_volume = true;
	return (emit_into(fixture, volume, error));
}

/*
 * Scenario: writer limits. Each constrained resource is reached by the
 * small tree, and the refusal names that resource with an observation
 * beyond its limit. Memory is reached through aggregate accounting:
 * planned writers kept live in one context exhaust the smallest legal
 * budget, so another handle cannot escape the total.
 */
#define LIVE_WRITERS_MAX	512
static void
writer_limits(const contract_fs_profile_t *profile)
{
	static const struct {
		const char	*phase;
		ctx_setup_t	 setup;
		fsimg_resource_t resource;
	} cases[] = {
		{ .phase = "entries", .setup = { .max_entries = 3 },
		    .resource = FSIMG_RESOURCE_ENTRIES },
		{ .phase = "work", .setup = { .max_work = 4 },
		    .resource = FSIMG_RESOURCE_WORK },
		{ .phase = "output", .setup = { .max_output_bytes = 4096 },
		    .resource = FSIMG_RESOURCE_OUTPUT },
		{ .phase = "scratch", .setup = { .scratch_bytes = 4096,
		    .cache_pages = 1 }, .resource = FSIMG_RESOURCE_SCRATCH },
	};
	static const ctx_setup_t smallest = { .memory_bytes = 16 * MIB };
	fsimg_writer_t *writers[LIVE_WRITERS_MAX];
	fixture_t fixture;
	contract_volume_t volume;
	const fsimg_plan_t *plan;
	fsimg_stats_t stats;
	fsimg_error_t error;
	size_t live;
	fsimg_status_t status;
	bool have_volume;

	if (!contract_begin(profile->id, "writer-limits"))
		return;
	for (size_t i = 0; i < COUNT(cases); i++) {
		contract_phase(cases[i].phase, 0);
		fixture_open(&fixture, profile, &cases[i].setup);
		status = writer_whole_run(&fixture, &volume, &have_volume, &error);
		CONTRACT_STATUS(status, FSIMG_ELIMIT, &error);
		CONTRACT_CHECK(error.resource == cases[i].resource);
		CONTRACT_CHECK(error.observed > error.limit);
		if (fixture.writer != NULL)
			CONTRACT_CHECK(contract_writer_failed(fixture.writer));
		if (have_volume)
			contract_volume_free(&volume);
		fixture_close(&fixture);
	}

	/* Memory: planned writers stay live in one context until the
	 * smallest legal budget refuses one; no handle escapes the total.
	 */
	contract_phase("aggregate memory", 0);
	fixture_open(&fixture, profile, &smallest);
	for (live = 0; live < LIVE_WRITERS_MAX; live++) {
		fixture.writer = NULL;
		status = writer_collect(&fixture, NULL, &error);
		if (status == FSIMG_OK)
			status = fsimg_writer_plan(fixture.writer, &plan, &error);
		if (status != FSIMG_OK)
			break;
		writers[live] = fixture.writer;
	}
	CONTRACT_STATUS(status, FSIMG_ELIMIT, &error);
	CONTRACT_CHECK(error.resource == FSIMG_RESOURCE_MEMORY);
	CONTRACT_CHECK(error.observed > error.limit && live > 0);
	contract_baseline(fixture.ctx, &stats);
	CONTRACT_CHECK(stats.memory_peak <= smallest.memory_bytes);
	for (size_t i = 0; i < live; i++)
		fsimg_writer_free(writers[i]);
	fixture_close(&fixture);
	contract_end();
}

/* The timestamp a FSIMG_META_* time field names. */
static const fsimg_timestamp_t *
timestamp_field(const fsimg_metadata_t *metadata, uint32_t field)
{
	switch (field) {
	case FSIMG_META_CREATED:
		return (&metadata->created);
	case FSIMG_META_MODIFIED:
		return (&metadata->modified);
	case FSIMG_META_ACCESSED:
		return (&metadata->accessed);
	case FSIMG_META_CHANGED:
		return (&metadata->changed);
	default:
		CONTRACT_CHECK(!"timestamp field");
		return (NULL);
	}
}

/* The decoded value of each checked field equals the profile's. */
static void
check_metadata(const contract_metadata_case_t *test, const fsimg_entry_t *entry)
{
	static const uint32_t times[] = { FSIMG_META_CREATED,
	    FSIMG_META_MODIFIED, FSIMG_META_ACCESSED, FSIMG_META_CHANGED };
	const fsimg_timestamp_t *actual, *expected;

	CONTRACT_CHECK((entry->metadata.valid & test->checked) == test->checked);
	for (size_t i = 0; i < COUNT(times); i++) {
		if ((test->checked & times[i]) == 0)
			continue;
		actual = timestamp_field(&entry->metadata, times[i]);
		expected = timestamp_field(&test->stored, times[i]);
		if (actual->seconds != expected->seconds ||
		    actual->nanoseconds != expected->nanoseconds ||
		    actual->basis != expected->basis ||
		    actual->offset_minutes != expected->offset_minutes)
			fprintf(stderr, "field 0x%x: %lld.%09u basis %d offset %d\n",
			    times[i], (long long)actual->seconds,
			    actual->nanoseconds, actual->basis,
			    actual->offset_minutes);
		CONTRACT_CHECK(actual->seconds == expected->seconds);
		CONTRACT_CHECK(actual->nanoseconds == expected->nanoseconds);
		CONTRACT_CHECK(actual->basis == expected->basis);
		CONTRACT_CHECK(actual->offset_minutes == expected->offset_minutes);
	}
	if ((test->checked & FSIMG_META_OWNER) != 0)
		CONTRACT_CHECK(entry->metadata.uid == test->stored.uid &&
		    entry->metadata.gid == test->stored.gid);
	if ((test->checked & FSIMG_META_MODE) != 0)
		CONTRACT_CHECK(entry->metadata.mode == test->stored.mode);
}

/* Find the metadata entry in a fresh reader and check it and its bytes. */
static void
read_back_metadata(fixture_t *fixture, contract_volume_t *volume,
    const contract_metadata_case_t *test)
{
	contract_entry_t meta = metadata_entry(test);
	fsimg_io_t *backing, *slice;
	fsimg_reader_t *reader;
	const fsimg_entry_t *entry;
	fsimg_error_t error;
	fsimg_status_t status;
	bool found = false;

	slice = contract_volume_open(fixture->ctx, volume, false, &backing);
	CONTRACT_STATUS(reader_open(fixture, slice, &reader, &error), FSIMG_OK,
	    &error);
	while ((status = fsimg_reader_next(reader, &entry, &error)) == FSIMG_OK) {
		if (strcmp(entry->path, META_PATH) != 0)
			continue;
		CONTRACT_CHECK(!found);
		found = true;
		CONTRACT_CHECK(entry->kind == test->kind);
		CONTRACT_CHECK(entry->size == meta.size);
		check_metadata(test, entry);
		if (meta.size != 0)
			check_content(reader, &meta, META_ID);
	}
	CONTRACT_STATUS(status, FSIMG_DONE, &error);
	CONTRACT_CHECK(found);
	fsimg_reader_free(reader);
	contract_volume_close(slice, backing);
}

/*
 * Scenario: metadata. Each profile case adds one entry to the common tree.
 * An accepted case has exactly its stated field count in the plan and the
 * context, callbacks naming exactly its fields, and decodes to its stated
 * values; a refused case fails planning naming the entry, with no count
 * and no callback.
 */
static void
writer_metadata(const contract_fs_profile_t *profile)
{
	const contract_metadata_case_t *test;
	fixture_t fixture;
	contract_volume_t volume;
	const fsimg_plan_t *plan;
	fsimg_plan_info_t info;
	fsimg_stats_t stats;
	fsimg_error_t error;
	fsimg_status_t status;

	if (!contract_begin(profile->id, "writer-metadata"))
		return;
	for (size_t i = 0; i < profile->metadata_case_count; i++) {
		test = &profile->metadata_cases[i];
		contract_phase(test->id, 0);
		fixture_open(&fixture, profile, NULL);
		CONTRACT_STATUS(writer_collect(&fixture, test, &error), FSIMG_OK,
		    &error);
		status = fsimg_writer_plan(fixture.writer, &plan, &error);
		CONTRACT_STATUS(status, test->plan_status, &error);
		contract_baseline(fixture.ctx, &stats);
		CONTRACT_CHECK(stats.metadata_losses == test->losses);
		CONTRACT_CHECK(fixture.loss_mask == test->loss_mask);
		CONTRACT_CHECK((fixture.loss_calls == 0) == (test->losses == 0));
		if (status != FSIMG_OK) {
			CONTRACT_CHECK(error.operation == FSIMG_OP_WRITER);
			CONTRACT_CHECK(strcmp(error.path, META_PATH) == 0);
			CONTRACT_CHECK(contract_writer_failed(fixture.writer));
			fixture_close(&fixture);
			continue;
		}
		CONTRACT_STATUS(fsimg_plan_get_info(plan, &info, &error),
		    FSIMG_OK, &error);
		CONTRACT_CHECK(info.metadata_losses == test->losses);
		CONTRACT_CHECK(info.volume_bytes <= profile->max_fixture_bytes);
		contract_volume_init(&volume, info.volume_bytes, 0xa5);
		CONTRACT_STATUS(emit_into(&fixture, &volume, &error), FSIMG_OK,
		    &error);
		contract_phase(test->id, 0);
		/* Emission neither counts nor reports the losses again. */
		contract_baseline(fixture.ctx, &stats);
		CONTRACT_CHECK(stats.metadata_losses == test->losses);
		read_back_metadata(&fixture, &volume, test);
		fixture_close(&fixture);
		contract_volume_free(&volume);
	}
	contract_end();
}

/*
 * Reader scenarios share one materialized volume per scenario; each run
 * opens a fresh context and reader on a read-only slice of it.
 */

/*
 * Scenario: reader lifecycle. Opening reports the profile's format and
 * capabilities with header scope; iteration ends with a stable DONE and
 * NULL; state errors before work (content of a directory, an offset past
 * the end) preserve the current entry and the reader.
 */
static void
reader_lifecycle(const contract_fs_profile_t *profile)
{
	fixture_t fixture;
	contract_volume_t volume;
	fsimg_io_t *backing, *slice;
	fsimg_reader_t *reader;
	fsimg_volume_info_t info;
	fsimg_verify_report_t report;
	const fsimg_entry_t *entry;
	unsigned char byte;
	size_t length;
	fsimg_error_t error;
	fsimg_status_t status;
	bool saw_directory = false, saw_file = false;

	if (!contract_begin(profile->id, "reader-lifecycle"))
		return;
	materialize(profile, &volume);
	fixture_open(&fixture, profile, NULL);
	slice = contract_volume_open(fixture.ctx, &volume, false, &backing);
	contract_phase("open", 0);
	CONTRACT_STATUS(reader_open(&fixture, slice, &reader, &error), FSIMG_OK,
	    &error);
	CONTRACT_STATUS(fsimg_reader_get_info(reader, &info, &error), FSIMG_OK,
	    &error);
	CONTRACT_CHECK(strcmp(info.format, profile->format) == 0);
	/* An opened volume offers the reader's operations alone. */
	CONTRACT_CHECK(info.capabilities == (profile->expected_capabilities &
	    (FSIMG_CAP_READ | FSIMG_CAP_VERIFY)));
	CONTRACT_CHECK(info.scope == FSIMG_VALIDATION_HEADER);
	CONTRACT_CHECK((info.valid & FSIMG_VOLUME_GEOMETRY) != 0);
	CONTRACT_CHECK(info.volume_bytes == volume.length);

	contract_phase("content before iteration", 0);
	CONTRACT_STATUS(fsimg_reader_read_at(reader, 0, &byte, 1, &length,
	    &error), FSIMG_ESTATE, &error);

	contract_phase("iterate", 0);
	while ((status = fsimg_reader_next(reader, &entry, &error)) == FSIMG_OK) {
		if (entry->kind == FSIMG_ENTRY_DIRECTORY && !saw_directory) {
			saw_directory = true;
			CONTRACT_STATUS(fsimg_reader_read_at(reader, 0, &byte, 1,
			    &length, &error), FSIMG_ESTATE, &error);
			CONTRACT_CHECK(entry->kind == FSIMG_ENTRY_DIRECTORY);
		}
		if (entry->kind == FSIMG_ENTRY_FILE && entry->size > 1 &&
		    !saw_file) {
			saw_file = true;
			CONTRACT_STATUS(fsimg_reader_read_at(reader,
			    entry->size + 1, &byte, 1, &length, &error),
			    FSIMG_EINVAL, &error);
			CONTRACT_STATUS(fsimg_reader_read_at(reader, 0, &byte, 1,
			    &length, &error), FSIMG_OK, &error);
			CONTRACT_CHECK(length == 1);
		}
	}
	CONTRACT_STATUS(status, FSIMG_DONE, &error);
	CONTRACT_CHECK(entry == NULL && saw_directory && saw_file);
	contract_phase("after end", 0);
	CONTRACT_STATUS(fsimg_reader_next(reader, &entry, &error), FSIMG_DONE,
	    &error);
	CONTRACT_CHECK(entry == NULL);
	CONTRACT_STATUS(fsimg_reader_read_at(reader, 0, &byte, 1, &length,
	    &error), FSIMG_ESTATE, &error);
	CONTRACT_STATUS(fsimg_verify(reader, 0, &report, &error), FSIMG_ESTATE,
	    &error);
	CONTRACT_CHECK(!report.complete);
	fsimg_reader_free(reader);
	contract_volume_close(slice, backing);
	fixture_close(&fixture);
	contract_volume_free(&volume);
	contract_end();
}

/*
 * Scenario: empty and small tree, reader side, through a slice whose
 * backing callbacks record every access: exact entries and bytes, every
 * access inside the volume (checked as it happens), and canaries intact.
 */
static void
reader_small_tree(const contract_fs_profile_t *profile)
{
	fixture_t fixture;
	contract_volume_t volume;
	fsimg_io_t *backing, *slice;
	fsimg_reader_t *reader;
	fsimg_error_t error;

	if (!contract_begin(profile->id, "reader-small-tree"))
		return;
	materialize(profile, &volume);
	fixture_open(&fixture, profile, NULL);
	slice = contract_volume_open(fixture.ctx, &volume, false, &backing);
	contract_phase("traverse", 0);
	CONTRACT_STATUS(reader_open(&fixture, slice, &reader, &error), FSIMG_OK,
	    &error);
	check_tree(reader, profile);
	fsimg_reader_free(reader);
	contract_volume_close(slice, backing);
	CONTRACT_CHECK(volume.reads > 0);
	CONTRACT_CHECK(contract_volume_guards_intact(&volume));
	fixture_close(&fixture);
	contract_volume_free(&volume);
	contract_end();
}

/*
 * Scenario: bounded volume, writer side. Emission into a slice with
 * canaries before and after touches only the volume: every backing call
 * was checked as it happened, and the guards survive.
 */
static void
writer_bounded_volume(const contract_fs_profile_t *profile)
{
	fixture_t fixture;
	contract_volume_t volume;
	fsimg_error_t error;

	if (!contract_begin(profile->id, "writer-bounded-volume"))
		return;
	fixture_open(&fixture, profile, NULL);
	writer_ready(&fixture);
	plan_volume(&fixture, &volume, 0xa5);
	CONTRACT_STATUS(emit_into(&fixture, &volume, &error), FSIMG_OK, &error);
	CONTRACT_CHECK(volume.writes > 0);
	CONTRACT_CHECK(contract_volume_guards_intact(&volume));
	fixture_close(&fixture);
	contract_volume_free(&volume);
	contract_end();
}

/* Position the reader on path, which must exist. */
static const fsimg_entry_t *
reader_find(fsimg_reader_t *reader, const char *path)
{
	const fsimg_entry_t *entry;
	fsimg_error_t error;
	fsimg_status_t status;

	while ((status = fsimg_reader_next(reader, &entry, &error)) == FSIMG_OK) {
		if (strcmp(entry->path, path) == 0)
			return (entry);
	}
	CONTRACT_STATUS(status, FSIMG_OK, &error);
	return (NULL);
}

/*
 * Scenario: content delivery on the mixed file. Random reads, reads
 * clipped at the end, a read at the end and one past it, and ranges that
 * tile the logical file: DATA ranges carry the expected bytes, a ZERO range
 * reads as zeros.
 */
static void
reader_content(const contract_fs_profile_t *profile)
{
	const contract_entry_t *mixed = source_entry(ID_MIXED);
	static const uint64_t offsets[] = { 199999, 0, 70000 - 3, 139998,
	    65535, 131072, 1 };
	fixture_t fixture;
	contract_volume_t volume;
	fsimg_io_t *backing, *slice;
	fsimg_reader_t *reader;
	const fsimg_entry_t *entry;
	fsimg_range_t range;
	unsigned char buffer[4096];
	uint64_t offset;
	size_t length;
	fsimg_error_t error;

	if (!contract_begin(profile->id, "reader-content"))
		return;
	CONTRACT_CHECK(mixed->payload == PAYLOAD_MIXED);
	materialize(profile, &volume);
	fixture_open(&fixture, profile, NULL);
	slice = contract_volume_open(fixture.ctx, &volume, false, &backing);
	CONTRACT_STATUS(reader_open(&fixture, slice, &reader, &error), FSIMG_OK,
	    &error);
	entry = reader_find(reader, mixed->path);
	CONTRACT_CHECK(entry->size == mixed->size);

	contract_phase("random reads", 0);
	for (size_t i = 0; i < COUNT(offsets); i++) {
		CONTRACT_STATUS(fsimg_reader_read_at(reader, offsets[i], buffer,
		    sizeof(buffer), &length, &error), FSIMG_OK, &error);
		CONTRACT_CHECK(length == (mixed->size - offsets[i] <
		    sizeof(buffer) ? mixed->size - offsets[i] : sizeof(buffer)));
		for (size_t k = 0; k < length; k++)
			CONTRACT_CHECK(buffer[k] ==
			    expected_byte(mixed, ID_MIXED, offsets[i] + k));
	}
	contract_phase("end of file", 0);
	CONTRACT_STATUS(fsimg_reader_read_at(reader, mixed->size, buffer,
	    sizeof(buffer), &length, &error), FSIMG_OK, &error);
	CONTRACT_CHECK(length == 0);
	CONTRACT_STATUS(fsimg_reader_read_at(reader, mixed->size + 1, buffer,
	    sizeof(buffer), &length, &error), FSIMG_EINVAL, &error);

	contract_phase("ranges", 0);
	for (offset = 0; offset < mixed->size; offset += range.length) {
		CONTRACT_STATUS(fsimg_reader_next_range(reader, offset, &range,
		    &error), FSIMG_OK, &error);
		CONTRACT_CHECK(range.offset == offset && range.length > 0);
		CONTRACT_CHECK(range.length <= mixed->size - offset);
		CONTRACT_CHECK(range.kind == FSIMG_RANGE_DATA ||
		    range.kind == FSIMG_RANGE_ZERO);
		if (range.kind == FSIMG_RANGE_ZERO) {
			for (uint64_t k = 0; k < range.length; k++)
				CONTRACT_CHECK(expected_byte(mixed, ID_MIXED,
				    offset + k) == 0);
		}
	}
	CONTRACT_STATUS(fsimg_reader_next_range(reader, mixed->size, &range,
	    &error), FSIMG_DONE, &error);
	fsimg_reader_free(reader);
	contract_volume_close(slice, backing);
	fixture_close(&fixture);
	contract_volume_free(&volume);
	contract_end();
}

/*
 * Open, traverse and read everything, returning the first failure. What
 * is delivered is checked as it arrives: every entry is expected and
 * unique, and every byte read is the expected byte. A traversal that ends
 * must have delivered the whole expected tree, so a failure cannot pass
 * as a short success.
 */
static fsimg_status_t
reader_whole_run(fixture_t *fixture, fsimg_io_t *slice, fsimg_error_t *error)
{
	expected_tree_t tree;
	bool seen[SOURCE_COUNT + 4];
	fsimg_reader_t *reader;
	const fsimg_entry_t *entry;
	unsigned char buffer[65536];
	uint64_t offset;
	size_t index, length;
	fsimg_status_t status;

	expected_tree(fixture->profile, &tree);
	bzero(seen, sizeof(seen));
	status = reader_open(fixture, slice, &reader, error);
	if (status != FSIMG_OK)
		return (status);
	while ((status = fsimg_reader_next(reader, &entry, error)) == FSIMG_OK) {
		index = expected_find(&tree, entry->path);
		CONTRACT_CHECK(!seen[index]);
		seen[index] = true;
		CONTRACT_CHECK(entry->kind == tree.entries[index]->kind &&
		    entry->size == tree.entries[index]->size);
		if (entry->kind != FSIMG_ENTRY_FILE)
			continue;
		for (offset = 0; offset < entry->size; offset += length) {
			status = fsimg_reader_read_at(reader, offset, buffer,
			    sizeof(buffer), &length, error);
			if (status != FSIMG_OK)
				break;
			CONTRACT_CHECK(length > 0);
			for (size_t i = 0; i < length; i++)
				CONTRACT_CHECK(buffer[i] == expected_byte(
				    tree.entries[index], tree.identities[index],
				    offset + i));
		}
		if (status != FSIMG_OK)
			break;
	}
	if (status == FSIMG_DONE) {
		for (size_t i = 0; i < tree.count; i++)
			CONTRACT_CHECK(seen[i]);
	} else {
		/* A fatal failure invalidates the reader. */
		CONTRACT_CHECK(status < 0);
		CONTRACT_STATUS(fsimg_reader_next(reader, &entry, NULL),
		    FSIMG_ESTATE, NULL);
	}
	fsimg_reader_free(reader);
	return (status == FSIMG_DONE ? FSIMG_OK : status);
}

/*
 * Scenario: reader transport faults. A backing read that fails, or ends
 * early although the volume advertised its length, is an I/O failure
 * (FSIMG_EIO), never a malformed-image result, at the first read, a
 * middle read and the last read of a complete traversal.
 */
static void
reader_transport(const contract_fs_profile_t *profile)
{
	fixture_t fixture;
	contract_volume_t volume;
	fsimg_io_t *backing, *slice;
	fsimg_error_t error;
	uint64_t reads, points[3];

	if (!contract_begin(profile->id, "reader-transport"))
		return;
	materialize(profile, &volume);
	fixture_open(&fixture, profile, NULL);
	slice = contract_volume_open(fixture.ctx, &volume, false, &backing);
	CONTRACT_STATUS(reader_whole_run(&fixture, slice, &error), FSIMG_OK,
	    &error);
	reads = volume.reads;
	contract_volume_close(slice, backing);
	fixture_close(&fixture);
	CONTRACT_CHECK(reads > 2);

	points[0] = 1;
	points[1] = reads / 2;
	points[2] = reads;
	for (unsigned kind = 0; kind < 2; kind++) {
		for (size_t i = 0; i < 3; i++) {
			contract_volume_reset(&volume);
			if (kind == 0)
				volume.fail_read = points[i];
			else
				volume.eof_read = points[i];
			contract_run.fault = points[i];
			fixture_open(&fixture, profile, NULL);
			slice = contract_volume_open(fixture.ctx, &volume, false,
			    &backing);
			CONTRACT_STATUS(reader_whole_run(&fixture, slice, &error),
			    FSIMG_EIO, &error);
			CONTRACT_CHECK(volume.fault_fired);
			contract_volume_close(slice, backing);
			fixture_close(&fixture);
		}
	}
	contract_volume_free(&volume);
	contract_end();
}

/* The operation a reader fault sweep targets. */
typedef enum reader_operation {
	READ_TRAVERSE,
	READ_VERIFY,
	READ_EXTRACT
} reader_operation_t;

static const char *const reader_operation_names[] = { "traverse", "verify",
    "extract" };

static void remove_extracted(fixture_t *, const char *, bool);
static fsimg_status_t verify_run(fixture_t *, fsimg_io_t *, unsigned,
    fsimg_verify_report_t *, fsimg_error_t *);

static fsimg_status_t
extract_run(fixture_t *fixture, fsimg_io_t *slice, fsimg_error_t *error)
{
	fsimg_reader_t *reader;
	fsimg_extract_options_t options;
	fsimg_status_t status;

	status = reader_open(fixture, slice, &reader, error);
	if (status != FSIMG_OK)
		return (status);
	fsimg_extract_options_init(&options);
	status = fsimg_extract_at(reader, fixture->dir.fd, "out", &options,
	    error);
	fsimg_reader_free(reader);
	return (status);
}

static fsimg_status_t
reader_operation(fixture_t *fixture, fsimg_io_t *slice,
    reader_operation_t operation, fsimg_error_t *error)
{
	fsimg_verify_report_t report;

	if (operation == READ_TRAVERSE)
		return (reader_whole_run(fixture, slice, error));
	if (operation == READ_VERIFY)
		return (verify_run(fixture, slice, FSIMG_VERIFY_PAYLOAD,
		    &report, error));
	return (extract_run(fixture, slice, error));
}

/*
 * Sweep one fault kind over a reader operation, as writer_sweep() does:
 * every position that fires fails with expected, and the first unreached
 * position is the final run, whose result the operation checks in full.
 * Extraction output is checked and removed on every outcome.
 */
static uint64_t
reader_sweep(const contract_fs_profile_t *profile, contract_volume_t *volume,
    reader_operation_t operation, fault_kind_t kind, fsimg_status_t expected,
    const ctx_setup_t *setup)
{
	fixture_t fixture;
	fsimg_io_t *backing, *slice;
	fsimg_error_t error;
	fsimg_status_t status;
	uint64_t position;
	bool hit;

	for (position = 1; position <= CONTRACT_SWEEP_MAX; position++) {
		contract_phase(reader_operation_names[operation], position);
		fixture_open(&fixture, profile, setup);
		slice = contract_volume_open(fixture.ctx, volume, false, &backing);
		arm(&fixture, kind, position);
		status = reader_operation(&fixture, slice, operation, &error);
		hit = fired(&fixture, kind);
		arm(&fixture, kind, 0);
		contract_volume_close(slice, backing);
		if (operation == READ_EXTRACT)
			remove_extracted(&fixture, "out", status == FSIMG_OK);
		fixture_close(&fixture);
		contract_phase(reader_operation_names[operation], position);
		if (!hit) {
			CONTRACT_STATUS(status, FSIMG_OK, &error);
			if (getenv("CONTRACT_VERBOSE") != NULL)
				printf("%s %s fault %d: %llu positions\n",
				    profile->id,
				    reader_operation_names[operation], kind,
				    (unsigned long long)position - 1);
			return (position - 1);
		}
		CONTRACT_STATUS(status, expected, &error);
		if (operation == READ_EXTRACT && status != FSIMG_OK)
			CONTRACT_CHECK(error.output_state != FSIMG_OUTPUT_PUBLISHED);
	}
	fprintf(stderr, "sweep exceeded %u positions\n", CONTRACT_SWEEP_MAX);
	CONTRACT_CHECK(position <= CONTRACT_SWEEP_MAX);
	return (0);
}

/* Scenario: allocation and scratch failures across reader operations. */
static void
reader_faults(const contract_fs_profile_t *profile)
{
	static const ctx_setup_t small_cache = { .cache_pages = 2 };
	static const fault_kind_t meta_kinds[] = { FAULT_META_OPEN,
	    FAULT_META_RESIZE, FAULT_META_READ, FAULT_META_WRITE };
	contract_volume_t volume;
	uint64_t reached[COUNT(meta_kinds)];

	if (!contract_begin(profile->id, "reader-allocation-scratch"))
		return;
	materialize(profile, &volume);
	bzero(reached, sizeof(reached));
	for (reader_operation_t op = READ_TRAVERSE; op <= READ_EXTRACT; op++) {
		CONTRACT_CHECK(reader_sweep(profile, &volume, op,
		    FAULT_ALLOCATION, FSIMG_ENOMEM, NULL) != 0);
		for (size_t k = 0; k < COUNT(meta_kinds); k++)
			reached[k] += reader_sweep(profile, &volume, op,
			    meta_kinds[k], FSIMG_EIO, &small_cache);
	}
	/* The small cache makes some reader operation reach each one. */
	for (size_t k = 0; k < COUNT(meta_kinds); k++)
		CONTRACT_CHECK(reached[k] != 0);
	contract_volume_free(&volume);
	contract_end();
}

/*
 * Scenario: reader cancellation at the first poll and in the middle of
 * each reader operation.
 */
static void
reader_cancellation(const contract_fs_profile_t *profile)
{
	fixture_t fixture;
	contract_volume_t volume;
	fsimg_io_t *backing, *slice;
	fsimg_error_t error;
	uint64_t polls;

	if (!contract_begin(profile->id, "reader-cancellation"))
		return;
	materialize(profile, &volume);
	for (reader_operation_t op = READ_TRAVERSE; op <= READ_EXTRACT; op++) {
		contract_phase(reader_operation_names[op], 0);
		fixture_open(&fixture, profile, NULL);
		slice = contract_volume_open(fixture.ctx, &volume, false,
		    &backing);
		CONTRACT_STATUS(reader_operation(&fixture, slice, op, &error),
		    FSIMG_OK, &error);
		polls = fixture.polls;
		contract_volume_close(slice, backing);
		if (op == READ_EXTRACT)
			remove_extracted(&fixture, "out", true);
		fixture_close(&fixture);
		CONTRACT_CHECK(polls > 1);
		for (uint64_t point = 1; point <= polls; point += polls / 2) {
			contract_phase(reader_operation_names[op], point);
			fixture_open(&fixture, profile, NULL);
			slice = contract_volume_open(fixture.ctx, &volume, false,
			    &backing);
			fixture.cancel_at_poll = point;
			CONTRACT_STATUS(reader_operation(&fixture, slice, op,
			    &error), FSIMG_ECANCELLED, &error);
			contract_volume_close(slice, backing);
			if (op == READ_EXTRACT)
				remove_extracted(&fixture, "out", false);
			fixture_close(&fixture);
		}
	}
	contract_volume_free(&volume);
	contract_end();
}

/*
 * Scenario: reader limits. The tree has more entries than three and a
 * depth of three, so both limits are reached during traversal.
 */
static void
reader_limits(const contract_fs_profile_t *profile)
{
	static const struct {
		const char	*phase;
		ctx_setup_t	 setup;
		fsimg_resource_t resource;
	} cases[] = {
		{ .phase = "entries", .setup = { .max_entries = 3 },
		    .resource = FSIMG_RESOURCE_ENTRIES },
		{ .phase = "depth", .setup = { .max_depth = 1 },
		    .resource = FSIMG_RESOURCE_DEPTH },
		{ .phase = "work", .setup = { .max_work = 4 },
		    .resource = FSIMG_RESOURCE_WORK },
	};
	fixture_t fixture;
	contract_volume_t volume;
	fsimg_io_t *backing, *slice;
	fsimg_error_t error;

	if (!contract_begin(profile->id, "reader-limits"))
		return;
	materialize(profile, &volume);
	for (size_t i = 0; i < COUNT(cases); i++) {
		contract_phase(cases[i].phase, 0);
		fixture_open(&fixture, profile, &cases[i].setup);
		slice = contract_volume_open(fixture.ctx, &volume, false,
		    &backing);
		CONTRACT_STATUS(reader_whole_run(&fixture, slice, &error),
		    FSIMG_ELIMIT, &error);
		CONTRACT_CHECK(error.resource == cases[i].resource);
		CONTRACT_CHECK(error.observed > error.limit);
		contract_volume_close(slice, backing);
		fixture_close(&fixture);
	}
	contract_volume_free(&volume);
	contract_end();
}

/*
 * A successful report is complete for its scope, with the exact file,
 * directory (the implicit root included) and logical byte counts of the
 * expected tree; only the payload scope reads payload bytes.
 */
static void
check_report(const contract_fs_profile_t *profile, unsigned flags,
    const fsimg_verify_report_t *report)
{
	uint64_t bytes, files, directories;
	bool payload = (flags & FSIMG_VERIFY_PAYLOAD) != 0;

	files = count_kind(profile, FSIMG_ENTRY_FILE, &bytes);
	directories = count_kind(profile, FSIMG_ENTRY_DIRECTORY, NULL) + 1;
	CONTRACT_CHECK(report->complete);
	CONTRACT_CHECK(report->scope == (payload ? FSIMG_VALIDATION_PAYLOAD :
	    FSIMG_VALIDATION_STRUCTURAL));
	CONTRACT_CHECK(report->files == files);
	CONTRACT_CHECK(report->directories == directories);
	CONTRACT_CHECK(report->logical_bytes == bytes);
	CONTRACT_CHECK(report->omitted_entries == 0);
	if (payload)
		CONTRACT_CHECK(report->payload_bytes_read > 0 &&
		    report->payload_bytes_read <= bytes);
	else
		CONTRACT_CHECK(report->payload_bytes_read == 0);
}

static fsimg_status_t
verify_run(fixture_t *fixture, fsimg_io_t *slice, unsigned flags,
    fsimg_verify_report_t *report, fsimg_error_t *error)
{
	fsimg_reader_t *reader;
	fsimg_status_t status;

	status = reader_open(fixture, slice, &reader, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_verify(reader, flags, report, error);
	if (status != FSIMG_OK)
		CONTRACT_CHECK(!report->complete);
	else
		check_report(fixture->profile, flags, report);
	fsimg_reader_free(reader);
	return (status);
}

/*
 * Scenario: verification. A fresh reader per scope reports a complete
 * result with the exact file, directory (including the implicit root) and
 * logical byte counts; only the payload scope reads payload bytes. A
 * touched reader is refused, and a transport failure yields an incomplete
 * report.
 */
static void
reader_verify(const contract_fs_profile_t *profile)
{
	fixture_t fixture;
	contract_volume_t volume;
	fsimg_io_t *backing, *slice;
	fsimg_reader_t *reader;
	const fsimg_entry_t *entry;
	fsimg_verify_report_t report;
	fsimg_error_t error;

	if (!contract_begin(profile->id, "reader-verify"))
		return;
	materialize(profile, &volume);
	for (unsigned payload = 0; payload < 2; payload++) {
		contract_phase(payload ? "payload" : "structural", 0);
		fixture_open(&fixture, profile, NULL);
		slice = contract_volume_open(fixture.ctx, &volume, false,
		    &backing);
		CONTRACT_STATUS(verify_run(&fixture, slice,
		    payload ? FSIMG_VERIFY_PAYLOAD : 0, &report, &error),
		    FSIMG_OK, &error);
		contract_volume_close(slice, backing);
		fixture_close(&fixture);
	}

	contract_phase("touched reader", 0);
	fixture_open(&fixture, profile, NULL);
	slice = contract_volume_open(fixture.ctx, &volume, false, &backing);
	CONTRACT_STATUS(reader_open(&fixture, slice, &reader, &error), FSIMG_OK,
	    &error);
	CONTRACT_STATUS(fsimg_reader_next(reader, &entry, &error), FSIMG_OK,
	    &error);
	CONTRACT_STATUS(fsimg_verify(reader, 0, &report, &error), FSIMG_ESTATE,
	    &error);
	CONTRACT_CHECK(!report.complete);
	fsimg_reader_free(reader);
	contract_volume_close(slice, backing);
	fixture_close(&fixture);

	contract_phase("transport failure", 0);
	fixture_open(&fixture, profile, NULL);
	slice = contract_volume_open(fixture.ctx, &volume, false, &backing);
	CONTRACT_STATUS(reader_open(&fixture, slice, &reader, &error), FSIMG_OK,
	    &error);
	volume.fail_read = volume.reads + 2;
	CONTRACT_STATUS(fsimg_verify(reader, FSIMG_VERIFY_PAYLOAD, &report,
	    &error), FSIMG_EIO, &error);
	CONTRACT_CHECK(volume.fault_fired && !report.complete);
	fsimg_reader_free(reader);
	contract_volume_close(slice, backing);
	fixture_close(&fixture);
	contract_volume_free(&volume);
	contract_end();
}

/* An extracted file holds exactly the entry's expected bytes. */
static void
check_extracted_file(int parent, const char *name, const contract_entry_t *entry,
    uint64_t identity)
{
	unsigned char buffer[8192];
	struct stat st;
	uint64_t offset;
	ssize_t count;
	int fd;

	fd = openat(parent, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	CONTRACT_CHECK(fd != -1);
	CONTRACT_CHECK(fstat(fd, &st) == 0 && S_ISREG(st.st_mode));
	CONTRACT_CHECK((uint64_t)st.st_size == entry->size);
	for (offset = 0; offset < entry->size; offset += (uint64_t)count) {
		count = read(fd, buffer, sizeof(buffer));
		CONTRACT_CHECK(count > 0);
		for (ssize_t i = 0; i < count; i++)
			CONTRACT_CHECK(buffer[i] ==
			    expected_byte(entry, identity, offset + (uint64_t)i));
	}
	CONTRACT_CHECK(close(fd) == 0);
}

/* Remove name beneath parent without following anything it holds, and
 * return how many objects, name included, were removed.
 */
static uint64_t
remove_tree_at(int parent, const char *name)
{
	struct stat st;
	DIR *stream;
	struct dirent *child;
	uint64_t removed = 1;
	int fd;

	CONTRACT_CHECK(fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) == 0);
	if (!S_ISDIR(st.st_mode)) {
		CONTRACT_CHECK(unlinkat(parent, name, 0) == 0);
		return (removed);
	}
	fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC |
	    O_NOFOLLOW);
	CONTRACT_CHECK(fd != -1);
	stream = fdopendir(fd);
	CONTRACT_CHECK(stream != NULL);
	while ((child = readdir(stream)) != NULL) {
		if (strcmp(child->d_name, ".") == 0 ||
		    strcmp(child->d_name, "..") == 0)
			continue;
		removed += remove_tree_at(dirfd(stream), child->d_name);
		rewinddir(stream);
	}
	CONTRACT_CHECK(closedir(stream) == 0);
	CONTRACT_CHECK(unlinkat(parent, name, AT_REMOVEDIR) == 0);
	return (removed);
}

/*
 * Remove what an extraction left at name in the fixture directory. A
 * successful extraction must hold exactly the expected tree, checked
 * entry by entry with its bytes as it is removed; an incomplete one is
 * removed without following anything it contains.
 */
static void
remove_extracted(fixture_t *fixture, const char *name, bool complete)
{
	expected_tree_t tree;
	struct stat st;
	char path[256];
	uint64_t removed;
	int count;

	if (fstatat(fixture->dir.fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
		CONTRACT_CHECK(errno == ENOENT && !complete);
		return;
	}
	if (complete) {
		expected_tree(fixture->profile, &tree);
		for (size_t i = 0; i < tree.count; i++) {
			count = snprintf(path, sizeof(path), "%s%s", name,
			    tree.entries[i]->path);
			CONTRACT_CHECK(count > 0 && (size_t)count < sizeof(path));
			/* Parents precede children in the tree, so each
			 * directory on this path was already seen to be a
			 * real directory, not a link to follow.
			 */
			CONTRACT_CHECK(fstatat(fixture->dir.fd, path, &st,
			    AT_SYMLINK_NOFOLLOW) == 0);
			if (tree.entries[i]->kind == FSIMG_ENTRY_DIRECTORY) {
				CONTRACT_CHECK(S_ISDIR(st.st_mode));
				continue;
			}
			check_extracted_file(fixture->dir.fd, path,
			    tree.entries[i], tree.identities[i]);
		}
	}
	removed = remove_tree_at(fixture->dir.fd, name);
	/* Exactly the expected tree and its root directory, nothing more. */
	if (complete)
		CONTRACT_CHECK(removed == tree.count + 1);
}

/*
 * Scenario: extraction through the driver-to-extractor seam. A complete
 * extraction creates the private destination holding exactly the expected
 * tree and bytes; the destination is never merged into; a transport
 * failure partway reports an unpublished, incomplete result.
 */
static void
reader_extraction(const contract_fs_profile_t *profile)
{
	struct stat st;
	fixture_t fixture;
	contract_volume_t volume;
	fsimg_io_t *backing, *slice;
	fsimg_error_t error;
	uint64_t reads;

	if (!contract_begin(profile->id, "reader-extraction"))
		return;
	materialize(profile, &volume);
	contract_phase("complete", 0);
	fixture_open(&fixture, profile, NULL);
	slice = contract_volume_open(fixture.ctx, &volume, false, &backing);
	CONTRACT_STATUS(extract_run(&fixture, slice, &error), FSIMG_OK, &error);
	reads = volume.reads;
	contract_phase("existing destination", 0);
	CONTRACT_STATUS(extract_run(&fixture, slice, &error), FSIMG_EEXISTS,
	    &error);
	contract_volume_close(slice, backing);
	remove_extracted(&fixture, "out", true);
	fixture_close(&fixture);

	/* The last read of the extraction comes after its destination
	 * exists, so the failure leaves private incomplete output.
	 */
	contract_phase("transport failure", reads);
	contract_volume_reset(&volume);
	volume.fail_read = reads;
	fixture_open(&fixture, profile, NULL);
	slice = contract_volume_open(fixture.ctx, &volume, false, &backing);
	CONTRACT_STATUS(extract_run(&fixture, slice, &error), FSIMG_EIO, &error);
	CONTRACT_CHECK(volume.fault_fired);
	CONTRACT_CHECK(error.output_state == FSIMG_OUTPUT_INCOMPLETE);
	contract_volume_close(slice, backing);
	CONTRACT_CHECK(fstatat(fixture.dir.fd, "out", &st,
	    AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode));
	remove_extracted(&fixture, "out", false);
	fixture_close(&fixture);
	contract_volume_free(&volume);
	contract_end();
}

/*
 * Registry reconciliation, in both directions, before any fixture runs:
 * every advertised variant has exactly one coverage profile whose declared
 * capabilities equal the advertised ones; every profile names an advertised
 * variant and has a unique ID; and every profile carries the facts its
 * capabilities oblige.
 */
static void
check_registry(void)
{
	fsimg_format_info_t info;
	fsimg_error_t error;
	size_t cursor = 0, coverage, advertised = 0;
	fsimg_status_t status;
	bool found;

	if (!contract_begin("registry", "reconcile"))
		return;
	while ((status = fsimg_format_next(&cursor, &info, &error)) == FSIMG_OK) {
		advertised++;
		coverage = 0;
		for (size_t i = 0; i < COUNT(fs_profiles); i++) {
			if (strcmp(fs_profiles[i].format, info.name) != 0)
				continue;
			CONTRACT_CHECK(fs_profiles[i].expected_capabilities ==
			    info.capabilities);
			coverage += fs_profiles[i].coverage_profile;
		}
		if (coverage != 1)
			fprintf(stderr, "format %s has %zu coverage profiles\n",
			    info.name, coverage);
		CONTRACT_CHECK(coverage == 1);
	}
	CONTRACT_STATUS(status, FSIMG_DONE, &error);
	CONTRACT_CHECK(advertised != 0);

	/* The source tree lists every parent before its children. */
	for (size_t i = 0; i < SOURCE_COUNT; i++) {
		const char *slash = strrchr(source_tree[i].path, '/');
		size_t parent = (size_t)(slash - source_tree[i].path);
		bool listed = parent == 0;

		for (size_t j = 0; j < i && !listed; j++)
			listed = strlen(source_tree[j].path) == parent &&
			    strncmp(source_tree[j].path, source_tree[i].path,
			    parent) == 0 &&
			    source_tree[j].kind == FSIMG_ENTRY_DIRECTORY;
		CONTRACT_CHECK(listed);
	}

	for (size_t i = 0; i < COUNT(fs_profiles); i++) {
		const contract_fs_profile_t *profile = &fs_profiles[i];

		for (size_t j = 0; j < i; j++)
			CONTRACT_CHECK(strcmp(fs_profiles[j].id, profile->id) != 0);
		found = false;
		cursor = 0;
		while (fsimg_format_next(&cursor, &info, NULL) == FSIMG_OK)
			found = found || strcmp(info.name, profile->format) == 0;
		if (!found)
			fprintf(stderr, "profile %s names unadvertised %s\n",
			    profile->id, profile->format);
		CONTRACT_CHECK(found);
		CONTRACT_CHECK(profile->max_fixture_bytes != 0);
		if ((profile->expected_capabilities & FSIMG_CAP_WRITE) != 0)
			CONTRACT_CHECK(profile->metadata_case_count != 0);
		/* Readers without a writer need an independent materializer,
		 * which no registered format needs yet.
		 */
		if ((profile->expected_capabilities & FSIMG_CAP_READ) != 0)
			CONTRACT_CHECK((profile->expected_capabilities &
			    FSIMG_CAP_WRITE) != 0);
		for (size_t c = 0; c < profile->metadata_case_count; c++) {
			const contract_metadata_case_t *test =
			    &profile->metadata_cases[c];

			CONTRACT_CHECK((test->source.required &
			    ~test->source.valid) == 0);
			if (test->plan_status != FSIMG_OK)
				CONTRACT_CHECK(test->losses == 0 &&
				    test->loss_mask == 0 && test->checked == 0);
			CONTRACT_CHECK((test->losses == 0) ==
			    (test->loss_mask == 0));
		}
	}
	contract_end();
}

/*
 * The schedule: the registry's capabilities decide which scenario
 * families each profile owes. Conditional families that do not apply are
 * counted with their reason; nothing is silently filtered.
 */
static void
run_profile(const contract_fs_profile_t *profile)
{
	unsigned caps = profile->expected_capabilities;

	if ((caps & FSIMG_CAP_WRITE) != 0) {
		writer_small_tree(profile);
		writer_lifecycle(profile);
		writer_bounded_volume(profile);
		writer_source_consistency(profile);
		writer_defined_output(profile);
		writer_reproducibility(profile);
		writer_sink_failure(profile);
		writer_allocation(profile);
		writer_scratch(profile);
		writer_cancellation(profile);
		writer_limits(profile);
		writer_metadata(profile);
	} else
		contract_not_applicable(profile->id, "writer-*",
		    "format does not advertise write");
	if ((caps & FSIMG_CAP_READ) != 0) {
		reader_lifecycle(profile);
		reader_small_tree(profile);
		reader_content(profile);
		reader_transport(profile);
		reader_faults(profile);
		reader_cancellation(profile);
		reader_limits(profile);
		reader_extraction(profile);
	} else
		contract_not_applicable(profile->id, "reader-*",
		    "format does not advertise read");
	if ((caps & FSIMG_CAP_VERIFY) != 0)
		reader_verify(profile);
	else
		contract_not_applicable(profile->id, "reader-verify",
		    "format does not advertise verify");
}

int
main(int argc, char **argv)
{
	contract_init(argc, argv);
	check_registry();
	for (size_t i = 0; i < COUNT(fs_profiles); i++)
		run_profile(&fs_profiles[i]);
	return (contract_finish("driver contracts"));
}
