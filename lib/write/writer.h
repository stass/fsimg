/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_WRITER_H
#define FSIMG_WRITER_H

#include "generated.h"
#include "host.h"
#include "layer.h"
#include "meta.h"
#include "sha256.h"

#define FSIMG_CATALOG_HEADER 192u
#define FSIMG_CATALOG_INDEX_SIZE 24u
#define FSIMG_CATALOG_TIME_BYTES 32u

/* A catalog entry record is a fixed little-endian header followed by the
 * NUL-terminated UTF-8 path at FSIMG_CATALOG_HEADER. Size and source ID are
 * u64; storage, kind, the metadata masks, uid, gid and mode are u32. Each
 * timestamp occupies FSIMG_CATALOG_TIME_BYTES: seconds and precision as u64,
 * then nanoseconds, basis and offset minutes as u32, then four zero bytes.
 * The record's rank follows: its origin as u32, four zero bytes, then its
 * order as u64.
 * Absent fields and the padding are zero, so equal entries encode equally.
 * Child records contain parent ID, child ID, and NUL-terminated component.
 * Index records contain stream frame offset, parent ID, and child count.
 */
typedef enum fsimg_catalog_field {
	FSIMG_CATALOG_SIZE = 0,
	FSIMG_CATALOG_SOURCE_ID = 8,
	FSIMG_CATALOG_STORAGE = 16,
	FSIMG_CATALOG_KIND = 20,
	FSIMG_CATALOG_VALID = 24,
	FSIMG_CATALOG_REQUIRED = 28,
	FSIMG_CATALOG_ATTRIBUTES = 32,
	FSIMG_CATALOG_UID = 36,
	FSIMG_CATALOG_GID = 40,
	FSIMG_CATALOG_MODE = 44,
	FSIMG_CATALOG_CREATED = 48,
	FSIMG_CATALOG_MODIFIED = 80,
	FSIMG_CATALOG_ACCESSED = 112,
	FSIMG_CATALOG_CHANGED = 144,
	FSIMG_CATALOG_ORIGIN = 176,
	FSIMG_CATALOG_ORDER = 184
} fsimg_catalog_field_t;
_Static_assert(FSIMG_CATALOG_CHANGED + FSIMG_CATALOG_TIME_BYTES ==
    FSIMG_CATALOG_ORIGIN, "the rank follows the last timestamp");
_Static_assert(FSIMG_CATALOG_ORDER + 8 == FSIMG_CATALOG_HEADER,
    "the catalog header ends with the rank's order");
_Static_assert(FSIMG_CATALOG_GID == FSIMG_CATALOG_UID + 4,
    "uid and gid form one eight-byte owner slot");

/* Every collected record carries a rank, the pair (origin, order), that
 * decides among records of one image path; the lowest rank wins. A record
 * scanned beneath a source root has origin SCANNED and that root's
 * registration order, so the first-registered root wins among scanned
 * records. A programmatic record, such as a parent directory the CLI
 * synthesizes, has origin PROGRAMMATIC and order zero, so any scanned record
 * outranks it whenever either was added.
 */
typedef enum fsimg_catalog_origin {
	FSIMG_ORIGIN_SCANNED = 0,
	FSIMG_ORIGIN_PROGRAMMATIC = 1
} fsimg_catalog_origin_t;

typedef struct fsimg_source_root fsimg_source_root_t;
typedef struct fsimg_source_cursor fsimg_source_cursor_t;
typedef struct fsimg_format fsimg_format_t;
typedef struct fsimg_format_variant fsimg_format_variant_t;
typedef struct fsimg_emit fsimg_emit_t;

/* Each driver embeds this base first in one owned plan allocation. Common
 * code sees only the summary; the selected driver frees its entire extension.
 */
struct fsimg_plan {
	fsimg_writer_t	*writer;
	fsimg_plan_info_t info;
};

/*
 * validate checks a variant's options and resolves their defaults in
 * place, the sector size always: it keeps a nonzero sector_bytes and
 * replaces zero with its default. It accepts volume_bytes zero (automatic
 * sizing), and validating its own result again changes nothing, since a
 * fixed disk's fit (writer.c fit_volume) rounds the volume to that sector
 * and validates once more. emit places the plan's bytes through the
 * emission object (emit.h); the driver runs only after the writer has
 * checked and resized the sink and fsimg_emit_image() has zeroed it.
 */
typedef struct fsimg_writer_ops {
	fsimg_status_t (*validate)(fsimg_write_options_t *, unsigned,
	    fsimg_error_t *);
	fsimg_status_t (*plan)(fsimg_writer_t *, fsimg_error_t *);
	fsimg_status_t (*emit)(fsimg_writer_t *, fsimg_emit_t *, fsimg_error_t *);
	void (*free)(fsimg_plan_t *);
} fsimg_writer_ops_t;

typedef enum fsimg_writer_state {
	FSIMG_WRITER_COLLECTING,
	FSIMG_WRITER_PREPARING,
	FSIMG_WRITER_SEALED,
	FSIMG_WRITER_EMITTING,
	FSIMG_WRITER_FINISHED,
	FSIMG_WRITER_FAILED
} fsimg_writer_state_t;

/* Where an entry's bytes come from: nowhere (a directory or FIFO), the zero
 * source, the caller's provider, a host object, or a generator a manifest
 * describes. HOST and GENERATED records live in the writer's source-record
 * stream (host_sources) and belong to scanned records only.
 */
typedef enum fsimg_source_storage {
	FSIMG_STORAGE_NONE,
	FSIMG_STORAGE_ZERO,
	FSIMG_STORAGE_PROVIDER,
	FSIMG_STORAGE_HOST,
	FSIMG_STORAGE_GENERATED
} fsimg_source_storage_t;

/* The writer's single current source owns io. fd is only a borrowed alias
 * used for identity checks: the reopened descriptor io owns, which freeing
 * io closes and releases, or a regular-file root's descriptor, which the
 * root registry owns and io borrows.
 * A symbolic link, host, provider or generated, has no descriptor: opening
 * it reads and checks its whole target into link_target, link is set, and
 * io delivers those bytes as one data range. Generated file content has
 * neither descriptor nor io: generated holds its generator, and reads and
 * ranges are computed from it.
 *
 * A host file whose record carries a digest is checked as it is copied:
 * digest_state hashes the bytes the cursor serves in order from offset
 * zero, digest_offset counts them, and finishing requires every byte to
 * have been served and compares the digest. A caller that opens a source
 * only to inspect it closes it instead.
 */
struct fsimg_source_cursor {
	fsimg_writer_t	*writer;
	fsimg_io_t	*io;
	fsimg_host_file_t identity;
	fsimg_generated_t generated;
	fsimg_sha256_t	 digest_state;
	fsimg_source_storage_t storage;
	uint64_t	 id;
	uint64_t	 length;
	uint64_t	 digest_offset;
	int		 fd;
	bool		 active;
	bool		 link;
	bool		 digest_claimed;
	unsigned char	 digest[FSIMG_SHA256_DIGEST_LENGTH];
	unsigned char	 link_target[FSIMG_SYMLINK_TARGET_MAX + 1];
};

typedef struct fsimg_catalog_entry {
	fsimg_entry_t	 entry;
	uint64_t	 id;
	uint64_t	 parent_id;
	uint64_t	 children;
	fsimg_source_storage_t storage;
	/* HOST, GENERATED: host_sources frame offset; PROVIDER: caller ID;
	 * NONE/ZERO: 0.
	 */
	uint64_t	 source_id;
} fsimg_catalog_entry_t;

typedef struct fsimg_catalog_child {
	uint64_t	 parent_id;
	uint64_t	 id;
	const char	*name;
	size_t		 name_length;
} fsimg_catalog_child_t;

struct fsimg_writer {
	fsimg_ctx_t	*ctx;
	fsimg_plan_t	*plan;
	const fsimg_format_t *format;
	/* The selected variant's capability record; NULL for catalog-only use. */
	const fsimg_format_info_t *capabilities;
	fsimg_write_options_t options;
	/* The stack being written, its layer settings (options.layers points
	 * here), and the disk around the volume: no scheme for a bare volume,
	 * otherwise one partition, placed once the volume is planned.
	 */
	fsimg_stack_t	 stack;
	fsimg_layer_options_t layers;
	fsimg_disk_plan_t disk;
	/* Prepared worker output authority cannot be imported as source content. */
	fsimg_host_file_t output_root;
	fsimg_session_t	 session;
	fsimg_writer_state_t state;
	fsimg_stream_t	*entries;
	fsimg_stream_t	*host_sources;
	/* The sha256digest claims manifests make for generated content, NULL
	 * until one is made. Planning checks them once the image has accepted
	 * every size, and releases them (fsimg_manifest_check_digests). That
	 * every claim is checked, and only at a size the plan accepted, rests
	 * on two facts: a file is never dropped by preparation (only
	 * directories merge), so each claim's file is in the plan; and both
	 * emission entry points plan first.
	 */
	fsimg_stream_t	*generated_digests;
	fsimg_stream_t	*ordered;
	fsimg_stream_t	*children;
	fsimg_array_t	*index;
	fsimg_source_root_t *roots;
	fsimg_source_cursor_t source;
	fsimg_source_ops_t provider;
	void		*provider_argument;
	unsigned char	*record;
	unsigned char	*source_record;
	fsimg_catalog_entry_t current;
	uint64_t	 root_count;
	/* Records appended to the entry stream during collection. */
	uint64_t	 collected;
	/* Catalog entries, IDs 1..count: zero until preparation indexes the
	 * catalog, then the collected records less the merged duplicates.
	 */
	uint64_t	 count;
	uint64_t	 payload_bytes;
	/* The planning attempt's metadata losses: every field
	 * fsimg_writer_loss() accepted, the common omissions and the driver's
	 * encoding losses alike. Zero before planning; a successful plan
	 * publishes it as its metadata_losses.
	 */
	uint64_t	 metadata_losses;
	/* Private fault injection: interrupt the nth subsequent directory read. */
	uint64_t	 interrupt_directory_read_after;
	/* Catalog records (record, the entry streams) and source records
	 * (source_record, host_sources), which also hold generated records
	 * and so are never smaller than FSIMG_GENERATED_RECORD_MAX.
	 */
	uint32_t	 record_capacity;
	uint32_t	 source_record_capacity;
	/* Summaries of every catalog entry that planning compares with the
	 * capability record: the longest symbolic-link target (zero without
	 * links) and the unions of kinds, valid and required fields. They are
	 * taken over the kept records when preparation indexes the catalog: a
	 * dropped directory record's metadata never reaches the image.
	 */
	uint64_t	 catalog_link_target_max;
	uint32_t	 catalog_kinds;
	uint32_t	 catalog_fields;
	uint32_t	 catalog_required;
	bool		 provider_set;
	bool		 catalog_ready;
	bool		 output_root_known;
	unsigned	 variant;
	/* A label of at most 48 bytes and its terminator. */
	char		 label[49];
	/* A mountpoint as long as the volume information reports, and its
	 * terminator.
	 */
	char		 mountpoint[sizeof(((fsimg_volume_info_t *)0)->mountpoint)];
};

/* Common collection is private; public construction selects a format driver.
 * Preparation seals before work and is single-use.
 * Returned entries are writer-owned current snapshots, valid until the next
 * catalog operation or destruction. Source operations preserve the snapshot.
 * Invalid entry/provider values are terminal collection failures; calls in
 * the wrong lifecycle state return ESTATE without changing it.
 */
fsimg_status_t fsimg_writer_collect_new(fsimg_ctx_t *, fsimg_writer_t **,
    fsimg_error_t *);
/* fsimg_writer_new() for one variant of a given descriptor, registered or
 * not. A descriptor outside the registry is reachable only by code that
 * names it, which is how tests and native qualification exercise a writer
 * before its capability is registered.
 */
fsimg_status_t fsimg_writer_new_variant(fsimg_ctx_t *, const fsimg_format_t *,
    const fsimg_format_variant_t *, const fsimg_write_options_t *,
    fsimg_writer_t **, fsimg_error_t *);
fsimg_status_t fsimg_writer_set_sources(fsimg_writer_t *,
    const fsimg_source_ops_t *, void *, fsimg_error_t *);
fsimg_status_t fsimg_writer_add(fsimg_writer_t *, const fsimg_entry_t *,
    const fsimg_source_t *, fsimg_error_t *);
fsimg_status_t fsimg_writer_add_tree(fsimg_writer_t *, int, const char *,
    fsimg_error_t *);
fsimg_status_t fsimg_writer_add_manifest(fsimg_writer_t *, int, int,
    fsimg_error_t *);
void fsimg_writer_free(fsimg_writer_t *);
fsimg_status_t fsimg_writer_collect_ready(fsimg_writer_t *, fsimg_error_t *);
fsimg_status_t fsimg_writer_fail(fsimg_writer_t *, fsimg_status_t);
fsimg_status_t fsimg_writer_prepare(fsimg_writer_t *,
    const fsimg_sort_options_t *, fsimg_error_t *);
fsimg_status_t fsimg_catalog_get(fsimg_writer_t *, uint64_t,
    const fsimg_catalog_entry_t **, fsimg_error_t *);
/* Append one record. root is the source root whose scan produced it, which
 * gives the record its rank, or NULL for a programmatic record.
 */
fsimg_status_t fsimg_writer_collect(fsimg_writer_t *, const fsimg_entry_t *,
    fsimg_source_storage_t, uint64_t, const fsimg_source_root_t *,
    fsimg_error_t *);
void fsimg_catalog_discard(fsimg_writer_t *);
fsimg_status_t fsimg_catalog_prepare(fsimg_writer_t *,
    const fsimg_sort_options_t *, fsimg_error_t *);
/* The writer's one metadata-loss policy, for entry's metadata fields named
 * by fields that planning cannot preserve: the common check's omissions
 * and a driver's inexact encodings alike. Each field is a valid one of the
 * entry's, submitted once per planning attempt. A required field, or any
 * field when the caller clears FSIMG_WRITE_ALLOW_LOSS, refuses with
 * EUNSUPPORTED, message and the entry's path, counting and reporting
 * nothing. Otherwise every field is counted in metadata_losses and the
 * mask goes to the context's loss report once. A zero mask does nothing;
 * a field the entry lacks is an internal ESTATE. entry and message are
 * borrowed for the call.
 */
fsimg_status_t fsimg_writer_loss(fsimg_writer_t *, const fsimg_entry_t *,
    uint32_t, const char *, fsimg_error_t *);
/* Source lookup uses its own record buffer and preserves current snapshot. */
fsimg_status_t fsimg_catalog_read_source(fsimg_writer_t *, uint64_t,
    fsimg_catalog_entry_t *, fsimg_error_t *);
fsimg_status_t fsimg_catalog_decode_child(fsimg_writer_t *, size_t,
    fsimg_catalog_child_t *, fsimg_error_t *);
/* Catalog build phase: validate the sorted child stream and derive counts. */
fsimg_status_t fsimg_catalog_count_children(fsimg_writer_t *, fsimg_error_t *);

#endif
