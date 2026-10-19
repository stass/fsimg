/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */

#ifndef FSIMG_H
#define FSIMG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FSIMG_OPTIONS_VERSION 1u
#define FSIMG_IO_READ 0x01u
#define FSIMG_IO_WRITE 0x02u
#define FSIMG_IO_DURABLE 0x04u

typedef struct fsimg_ctx fsimg_ctx_t;
typedef struct fsimg_io fsimg_io_t;
typedef struct fsimg_writer fsimg_writer_t;
typedef struct fsimg_plan fsimg_plan_t;
typedef struct fsimg_reader fsimg_reader_t;

typedef enum fsimg_status {
	FSIMG_OK = 0,
	FSIMG_DONE = 1,
	FSIMG_EINVAL = -1,
	FSIMG_ESTATE = -2,
	FSIMG_EIO = -3,
	FSIMG_EFORMAT = -4,
	FSIMG_ECORRUPT = -5,
	FSIMG_EUNSUPPORTED = -6,
	FSIMG_ERANGE = -7,
	FSIMG_ELIMIT = -8,
	FSIMG_ENOMEM = -9,
	FSIMG_ECANCELLED = -10,
	FSIMG_ESOURCE_CHANGED = -11,
	FSIMG_EEXISTS = -12,
	FSIMG_EDURABILITY = -13
} fsimg_status_t;

typedef enum fsimg_operation {
	FSIMG_OP_NONE,
	FSIMG_OP_CONTEXT,
	FSIMG_OP_ALLOCATE,
	FSIMG_OP_IO,
	FSIMG_OP_PUBLISH,
	FSIMG_OP_METADATA,
	FSIMG_OP_SOURCE,
	FSIMG_OP_CATALOG,
	FSIMG_OP_WRITER,
	FSIMG_OP_READER,
	FSIMG_OP_VERIFY,
	FSIMG_OP_EXTRACT
} fsimg_operation_t;

typedef enum fsimg_resource {
	FSIMG_RESOURCE_NONE,
	FSIMG_RESOURCE_MEMORY,
	FSIMG_RESOURCE_SCRATCH,
	FSIMG_RESOURCE_DESCRIPTORS,
	FSIMG_RESOURCE_ENTRIES,
	FSIMG_RESOURCE_OUTPUT,
	FSIMG_RESOURCE_WORK,
	FSIMG_RESOURCE_DEPTH,
	FSIMG_RESOURCE_PATH
} fsimg_resource_t;

typedef enum fsimg_output_state {
	FSIMG_OUTPUT_ABSENT,
	FSIMG_OUTPUT_INCOMPLETE,
	FSIMG_OUTPUT_PUBLISHED,
	FSIMG_OUTPUT_UNCERTAIN
} fsimg_output_state_t;

typedef struct fsimg_error {
	fsimg_status_t	 status;
	int		 os_error;
	/* Nonzero when disposable output could not be removed; path identifies it. */
	int		 cleanup_error;
	fsimg_operation_t operation;
	fsimg_resource_t resource;
	fsimg_output_state_t output_state;
	uint64_t	 offset;
	uint64_t	 limit;
	uint64_t	 observed;
	bool		 offset_valid;
	bool		 path_truncated;
	bool		 message_truncated;
	char		 path[256];
	char		 message[256];
} fsimg_error_t;

typedef struct fsimg_progress {
	uint64_t	 entries;
	uint64_t	 bytes;
	uint64_t	 work;
	const char	*phase;
} fsimg_progress_t;

typedef void (*fsimg_progress_fn_t)(void *, const fsimg_progress_t *);
typedef bool (*fsimg_cancel_fn_t)(void *);
/* path is borrowed for this callback; fields is a FSIMG_META_* mask. */
typedef void (*fsimg_loss_fn_t)(void *, const char *, uint32_t);

typedef struct fsimg_options {
	size_t		 size;
	uint32_t	 version;
	uint32_t	 flags;
	uint64_t	 memory_bytes;
	uint64_t	 scratch_bytes;
	uint64_t	 max_entries;
	uint64_t	 max_output_bytes;
	uint64_t	 max_work;
	uint32_t	 max_depth;
	uint32_t	 max_path_bytes;
	uint32_t	 max_open_files;
	int		 scratch_directory_fd;
	fsimg_progress_fn_t progress;
	void		*progress_argument;
	fsimg_cancel_fn_t cancelled;
	void		*cancel_argument;
	fsimg_loss_fn_t	 loss;
	void		*loss_argument;
	uint64_t	 reserved[4];
} fsimg_options_t;

typedef struct fsimg_stats {
	uint64_t	 memory_bytes;
	uint64_t	 memory_peak;
	uint64_t	 scratch_bytes;
	uint64_t	 scratch_peak;
	uint64_t	 descriptors;
	uint64_t	 descriptors_peak;
	uint64_t	 read_bytes;
	uint64_t	 written_bytes;
	uint64_t	 io_calls;
	/* Sampled allocated scratch blocks, distinct from logical scratch bytes. */
	uint64_t	 scratch_physical_bytes;
	uint64_t	 scratch_physical_peak;
	uint64_t	 metadata_losses;
	/* Entries safe extraction omitted under FSIMG_EXTRACT_SKIP_UNSUPPORTED. */
	uint64_t	 skipped_entries;
} fsimg_stats_t;

/* Options and callback tables must have exactly this size and version.
 * Zero limits select defaults. Contexts are serial; callbacks cannot re-enter.
 * The context duplicates scratch_directory_fd; -1 disables scratch.
 * Supply a scratch directory protected from untrusted writers. Scratch holds
 * private disposable metadata and does not promise secure erasure.
 * Child handles must be freed before their context. Destructors accept NULL.
 */
void fsimg_options_init(fsimg_options_t *);
fsimg_status_t fsimg_ctx_new(const fsimg_options_t *, fsimg_ctx_t **,
    fsimg_error_t *);
void fsimg_ctx_free(fsimg_ctx_t *);
fsimg_status_t fsimg_ctx_get_stats(fsimg_ctx_t *, fsimg_stats_t *,
    fsimg_error_t *);
const char *fsimg_status_string(fsimg_status_t);

typedef fsimg_status_t (*fsimg_read_at_fn_t)(void *, uint64_t, void *,
    size_t, size_t *, fsimg_error_t *);
typedef fsimg_status_t (*fsimg_write_at_fn_t)(void *, uint64_t, const void *,
    size_t, size_t *, fsimg_error_t *);
typedef fsimg_status_t (*fsimg_get_size_fn_t)(void *, uint64_t *,
    fsimg_error_t *);
typedef fsimg_status_t (*fsimg_resize_fn_t)(void *, uint64_t, fsimg_error_t *);
typedef fsimg_status_t (*fsimg_flush_fn_t)(void *, fsimg_error_t *);
typedef fsimg_status_t (*fsimg_zero_fn_t)(void *, uint64_t, uint64_t,
    fsimg_error_t *);

typedef struct fsimg_io_ops {
	size_t		 size;
	uint32_t	 version;
	unsigned	 flags;
	size_t		 max_transfer;
	fsimg_read_at_fn_t read_at;
	fsimg_write_at_fn_t write_at;
	fsimg_get_size_fn_t get_size;
	fsimg_resize_fn_t resize;
	fsimg_flush_fn_t	 flush;
	fsimg_zero_fn_t	 zero_range;
	uint64_t	 reserved[4];
} fsimg_io_ops_t;

/* Tables are copied; their argument and buffers remain caller-owned.
 * Callback transfers may be short, including progress on failure, but must
 * never exceed the request. EINTR with confirmed progress is retried only
 * after advancing. Other errors are terminal. Zero-length requests do no I/O.
 * At most 1024 consecutive interruptions without progress are retried;
 * cancellation is checked between retries, including flush retries.
 * FD adapters duplicate regular-file FDs and preserve shared flags/position.
 * Read-only authority requires O_RDONLY. Keep shared flags and storage stable.
 * A slice borrows its parent, has fixed length, and cannot resize its parent.
 * Raw writes can leave partial output. No-flush sinks give logical completion;
 * durable completion requires FSIMG_IO_DURABLE plus a flush callback.
 */
void fsimg_io_ops_init(fsimg_io_ops_t *);
fsimg_status_t fsimg_io_from_fd(fsimg_ctx_t *, int, unsigned, fsimg_io_t **,
    fsimg_error_t *);
fsimg_status_t fsimg_io_from_callbacks(fsimg_ctx_t *, const fsimg_io_ops_t *,
    void *, fsimg_io_t **, fsimg_error_t *);
fsimg_status_t fsimg_io_slice(fsimg_io_t *, uint64_t, uint64_t, fsimg_io_t **,
    fsimg_error_t *);
void fsimg_io_free(fsimg_io_t *);
fsimg_status_t fsimg_io_get_size(fsimg_io_t *, uint64_t *, fsimg_error_t *);
fsimg_status_t fsimg_io_read_at(fsimg_io_t *, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);
fsimg_status_t fsimg_io_write_at(fsimg_io_t *, uint64_t, const void *, size_t,
    size_t *, fsimg_error_t *);
fsimg_status_t fsimg_io_read_exact(fsimg_io_t *, uint64_t, void *, size_t,
    size_t *, fsimg_error_t *);
fsimg_status_t fsimg_io_write_exact(fsimg_io_t *, uint64_t, const void *,
    size_t, size_t *, fsimg_error_t *);
fsimg_status_t fsimg_io_resize(fsimg_io_t *, uint64_t, fsimg_error_t *);
fsimg_status_t fsimg_io_zero_range(fsimg_io_t *, uint64_t, uint64_t,
    fsimg_error_t *);
fsimg_status_t fsimg_io_flush(fsimg_io_t *, bool, fsimg_error_t *);

#define FSIMG_META_CREATED 0x01u
#define FSIMG_META_MODIFIED 0x02u
#define FSIMG_META_ACCESSED 0x04u
#define FSIMG_META_ATTRIBUTES 0x08u
#define FSIMG_META_CHANGED 0x10u
#define FSIMG_META_OWNER 0x20u
#define FSIMG_META_MODE 0x40u
#define FSIMG_ATTR_READONLY 0x01u
#define FSIMG_ATTR_HIDDEN 0x02u
#define FSIMG_ATTR_SYSTEM 0x04u
#define FSIMG_ATTR_ARCHIVE 0x08u

/* Kinds are distinct bits so a capability record lists a set as a mask.
 * A symbolic link's size is its target length, 1 through 1023 bytes, and its
 * content is the target without a terminator; fsimg never resolves it.
 * Directory and FIFO sizes are zero and they have no content.
 */
typedef enum fsimg_entry_kind {
	FSIMG_ENTRY_FILE = 1,
	FSIMG_ENTRY_DIRECTORY = 2,
	FSIMG_ENTRY_SYMLINK = 4,
	FSIMG_ENTRY_FIFO = 8
} fsimg_entry_kind_t;

typedef enum fsimg_time_basis {
	FSIMG_TIME_UTC = 1,
	FSIMG_TIME_OFFSET = 2,
	FSIMG_TIME_WALL = 3,
	/* Volume information only; inspect each individual timestamp. */
	FSIMG_TIME_PER_TIMESTAMP = 4
} fsimg_time_basis_t;

/* UTC and OFFSET seconds denote Unix-epoch UTC instants. OFFSET also retains
 * the original minutes east of UTC, strictly within a day. WALL encodes
 * calendar fields as if UTC without asserting a timezone. UTC/WALL require
 * offset zero.
 * Nanoseconds are below 1e9. Positive precision_ns states representation
 * precision, not clock accuracy. An absent timestamp has no meaning.
 */
typedef struct fsimg_timestamp {
	int64_t		 seconds;
	uint64_t	 precision_ns;
	uint32_t	 nanoseconds;
	fsimg_time_basis_t basis;
	int32_t		 offset_minutes;
} fsimg_timestamp_t;

/* Required is a subset of valid. Unknown bits are rejected. The common
 * catalog preserves every valid field; format planning checks fidelity.
 * OWNER governs uid and gid; MODE governs the permission bits 07777, never
 * file-type bits; CHANGED is the inode change time.
 */
typedef struct fsimg_metadata {
	uint32_t	 valid;
	uint32_t	 required;
	uint32_t	 attributes;
	uint32_t	 uid;
	uint32_t	 gid;
	uint32_t	 mode;
	fsimg_timestamp_t created;
	fsimg_timestamp_t modified;
	fsimg_timestamp_t accessed;
	fsimg_timestamp_t changed;
} fsimg_metadata_t;

typedef struct fsimg_entry {
	const char	*path;
	fsimg_entry_kind_t kind;
	uint64_t	 size;
	fsimg_metadata_t metadata;
} fsimg_entry_t;

typedef enum fsimg_source_kind {
	FSIMG_SOURCE_ZERO = 1,
	FSIMG_SOURCE_PROVIDER = 2
} fsimg_source_kind_t;

typedef struct fsimg_source {
	fsimg_source_kind_t kind;
	uint64_t	 id;
} fsimg_source_t;

typedef enum fsimg_range_kind {
	FSIMG_RANGE_DATA = 1,
	FSIMG_RANGE_ZERO = 2
} fsimg_range_kind_t;

typedef struct fsimg_range {
	uint64_t	 offset;
	uint64_t	 length;
	fsimg_range_kind_t kind;
} fsimg_range_t;

typedef fsimg_status_t (*fsimg_source_validate_fn_t)(void *, uint64_t,
    fsimg_error_t *);
typedef fsimg_status_t (*fsimg_source_read_fn_t)(void *, uint64_t, uint64_t,
    void *, size_t, size_t *, fsimg_error_t *);
typedef fsimg_status_t (*fsimg_source_range_fn_t)(void *, uint64_t, uint64_t,
    fsimg_range_t *, fsimg_error_t *);

typedef struct fsimg_source_ops {
	size_t		 size;
	uint32_t	 version;
	uint32_t	 flags;
	size_t		 max_transfer;
	fsimg_source_validate_fn_t validate;
	fsimg_source_read_fn_t read_at;
	fsimg_source_range_fn_t next_range;
	uint64_t	 reserved[4];
} fsimg_source_ops_t;

/* One provider is registered once, before its entries. Its table is copied;
 * its argument/content remain borrowed until writer destruction. No callback
 * retains buffers or re-enters the active context. Reads follow the byte-I/O
 * partial-progress/EINTR rules. Opaque source/sink storage must be disjoint.
 * add copies path/metadata. Directories and FIFOs require size zero and a
 * NULL source; files require an explicit zero or provider source, and a
 * symbolic link requires a provider source whose bytes are its target. Host
 * trees record symbolic links without following them, reject
 * special files, and check identity/size/change metadata on reopening.
 * These checks detect ordinary mutation; hostile writers require a snapshot.
 * Provider ranges start at the requested offset, have positive in-bounds
 * length, and use DATA or ZERO. EOF is handled without invoking the callback.
 * Common collection is private; public writer construction and planning
 * select an accepted format driver.
 */
void fsimg_source_ops_init(fsimg_source_ops_t *);

#define FSIMG_WRITE_ALLOW_LOSS 0x01u
#define FSIMG_WRITE_REPRODUCIBLE 0x02u
#define FSIMG_WRITE_DURABLE 0x04u
/* Host trees also record uid, gid and permission bits as optional fields. */
#define FSIMG_WRITE_IMPORT_POSIX 0x08u

typedef struct fsimg_write_options {
	size_t		 size;
	uint32_t	 version;
	uint32_t	 flags;
	uint64_t	 volume_bytes; /* Zero selects automatic sizing. */
	uint64_t	 min_free_bytes;
	int64_t		 epoch; /* Unix seconds; default 1980-01-01 UTC. */
	/* Copied; NULL selects the format default, such as NO NAME. A format
	 * whose capability record has no label capacity requires NULL.
	 */
	const char	*label;
	uint32_t	 sector_bytes; /* Zero selects 512. */
	uint32_t	 allocation_unit_bytes; /* Zero selects automatically. */
	uint32_t	 fat_root_entries; /* FAT12/16 only; zero selects an adequate root. */
	uint32_t	 volume_serial; /* Default zero; used verbatim in all modes. */
	uint64_t	 reserved[4];
} fsimg_write_options_t;

typedef struct fsimg_plan_info {
	char		 format[16];
	uint64_t	 volume_bytes;
	uint64_t	 payload_bytes;
	uint64_t	 initialized_bytes; /* Format-initialized payload extent; excludes metadata. */
	uint64_t	 allocated_bytes;
	uint64_t	 metadata_bytes;
	uint64_t	 free_bytes;
	uint64_t	 entries;
	uint64_t	 metadata_losses; /* Number of affected fields. */
	uint64_t	 scratch_bytes; /* Observed context use at plan completion. */
	uint32_t	 sector_bytes;
	uint32_t	 allocation_unit_bytes;
	unsigned	 required_io_flags;
} fsimg_plan_info_t;

/* Writers copy options/labels and own their borrowed plan until destruction.
 * plan seals collection; subsequent plan queries return the same object.
 * emit/write_at plan automatically and attempt emission only once. A failed
 * writer retains an already completed plan for inspection. Raw sinks may be
 * incomplete on failure; known source aliases are rejected before mutation.
 * Callback source/sink storage must be disjoint. Fixed sinks must match the
 * planned size; whole resizable sinks are resized. Every byte is defined.
 * ALLOW_LOSS permits optional timestamp rounding/omission, time-basis loss and
 * omission of a field the format does not store, counted once per field;
 * required losses and entry kinds the format lacks always fail planning.
 * Counts remain available in plan information.
 * write_at requires a single basename beneath a trusted directory descriptor,
 * publishes without replacement, and always performs durable publication.
 * On unsupported publication error.path names the retained image relative to
 * parent_fd; when cleanup_error is nonzero it names the retained directory.
 */
void fsimg_write_options_init(fsimg_write_options_t *);
fsimg_status_t fsimg_writer_new(fsimg_ctx_t *, const char *,
    const fsimg_write_options_t *, fsimg_writer_t **, fsimg_error_t *);
fsimg_status_t fsimg_writer_set_sources(fsimg_writer_t *,
    const fsimg_source_ops_t *, void *, fsimg_error_t *);
fsimg_status_t fsimg_writer_add(fsimg_writer_t *, const fsimg_entry_t *,
    const fsimg_source_t *, fsimg_error_t *);
fsimg_status_t fsimg_writer_add_tree(fsimg_writer_t *, int, const char *,
    fsimg_error_t *);
fsimg_status_t fsimg_writer_plan(fsimg_writer_t *, const fsimg_plan_t **,
    fsimg_error_t *);
fsimg_status_t fsimg_plan_get_info(const fsimg_plan_t *, fsimg_plan_info_t *,
    fsimg_error_t *);
fsimg_status_t fsimg_writer_emit(fsimg_writer_t *, fsimg_io_t *, fsimg_error_t *);
fsimg_status_t fsimg_writer_write_at(fsimg_writer_t *, int, const char *,
    fsimg_error_t *);
void fsimg_writer_free(fsimg_writer_t *);

#define FSIMG_CAP_READ 0x01u
#define FSIMG_CAP_WRITE 0x02u
#define FSIMG_CAP_VERIFY 0x04u
#define FSIMG_VOLUME_GEOMETRY 0x01u
#define FSIMG_VOLUME_LABEL 0x02u
#define FSIMG_VOLUME_FREE 0x04u
#define FSIMG_VOLUME_UUID 0x08u
#define FSIMG_VERIFY_PAYLOAD 0x01u

typedef enum fsimg_validation_scope {
	FSIMG_VALIDATION_HEADER = 1,
	FSIMG_VALIDATION_STRUCTURAL,
	FSIMG_VALIDATION_PAYLOAD
} fsimg_validation_scope_t;

typedef struct fsimg_read_options {
	size_t		 size;
	uint32_t	 version;
	uint32_t	 flags;
	const char	*format;
	/* Zero or 437 selects CP437; other codepages are unsupported. */
	uint32_t	 fat_codepage;
	uint64_t	 reserved[4];
} fsimg_read_options_t;

typedef struct fsimg_volume_info {
	uint32_t	 valid;
	unsigned	 capabilities;
	char		 format[16];
	char		 profile[40];
	char		 label[48];
	/* A 16-byte volume identity in textual (RFC 4122) byte order. */
	unsigned char	 uuid[16];
	uint64_t	 volume_bytes;
	uint64_t	 allocation_units;
	uint64_t	 free_bytes;
	uint32_t	 sector_bytes;
	uint32_t	 allocation_unit_bytes;
	fsimg_time_basis_t time_basis;
	fsimg_validation_scope_t scope;
	/* Recorded clean/error flags; they do not certify structural consistency.
	 * FAT12 has no such flags and reports clean=true, io_error=false.
	 */
	bool		 clean;
	bool		 io_error;
} fsimg_volume_info_t;

typedef struct fsimg_format_info {
	char		 name[16];
	char		 profile[40];
	unsigned	 capabilities;
	/* Masks of the FSIMG_ENTRY_* kinds and FSIMG_META_* fields stored. */
	uint32_t	 entry_kinds;
	uint32_t	 metadata_fields;
	/* Bit n denotes a legal size of 2^n bytes. */
	uint32_t	 sector_size_mask;
	uint32_t	 allocation_unit_size_mask;
	/* Writer profile can be narrower than the reader's legal geometry. */
	uint32_t	 write_allocation_unit_size_mask;
	/* Independent upper bounds; not every combination fits a legal volume. */
	uint64_t	 max_file_bytes;
	uint64_t	 max_volume_bytes;
	uint32_t	 max_name_utf16_units;
	/* Encoded on-disk label capacity; zero when the format has no label. */
	uint32_t	 max_label_bytes;
} fsimg_format_info_t;

typedef struct fsimg_verify_report {
	fsimg_validation_scope_t scope;
	bool		 complete;
	uint64_t	 files;
	uint64_t	 directories; /* Includes the implicit root. */
	uint64_t	 logical_bytes;
	uint64_t	 payload_bytes_read;
	uint64_t	 allocated_clusters;
	uint64_t	 free_clusters;
	uint64_t	 bad_clusters;
} fsimg_verify_report_t;

/* Input and its storage remain borrowed and stable until reader destruction.
 * Opening checks headers only. next validates each directory's lookup namespace
 * before exposing its children, then visits depth first in on-disk order.
 * The snapshot is borrowed until the next next call begins, fatal failure,
 * or destruction. Content functions address only the current regular file.
 * read_at clips to EOF; offsets past EOF are invalid. Confirmed bytes can
 * accompany an error. EOF iteration is stable and returns DONE with NULL.
 * Fatal errors invalidate the snapshot and fail the reader; argument/state
 * errors detected before work preserve it. No operation enters a sandbox.
 * verify requires an untouched reader, consumes traversal, and reports complete
 * only after the entire named scope succeeds. A failed report is incomplete.
 * Initialize a format cursor to zero; enumeration allocates no catalog.
 */
void fsimg_read_options_init(fsimg_read_options_t *);
fsimg_status_t fsimg_reader_open(fsimg_ctx_t *, fsimg_io_t *,
    const fsimg_read_options_t *, fsimg_reader_t **, fsimg_error_t *);
fsimg_status_t fsimg_reader_next(fsimg_reader_t *, const fsimg_entry_t **,
    fsimg_error_t *);
fsimg_status_t fsimg_reader_get_info(const fsimg_reader_t *,
    fsimg_volume_info_t *, fsimg_error_t *);
fsimg_status_t fsimg_reader_read_at(fsimg_reader_t *, uint64_t, void *,
    size_t, size_t *, fsimg_error_t *);
fsimg_status_t fsimg_reader_next_range(fsimg_reader_t *, uint64_t,
    fsimg_range_t *, fsimg_error_t *);
fsimg_status_t fsimg_verify(fsimg_reader_t *, unsigned,
    fsimg_verify_report_t *, fsimg_error_t *);
void fsimg_reader_free(fsimg_reader_t *);
fsimg_status_t fsimg_format_next(size_t *, fsimg_format_info_t *,
    fsimg_error_t *);

#define FSIMG_EXTRACT_ALLOW_LOSS 0x01u
#define FSIMG_EXTRACT_TIMES 0x02u
#define FSIMG_EXTRACT_WALL_OFFSET 0x04u
#define FSIMG_EXTRACT_SKIP_UNSUPPORTED 0x08u

typedef struct fsimg_extract_options {
	size_t		 size;
	uint32_t	 version;
	uint32_t	 flags;
	/* Absolute UTF-8 lookup path; format case/alias rules apply. NULL means /. */
	const char	*selection;
	uint64_t	 max_output_bytes; /* Zero uses the reader's ceiling. */
	/* Explicit minutes east of UTC for unknown-zone calendar timestamps. */
	int32_t		 wall_offset_minutes;
	uint64_t	 reserved[4];
} fsimg_extract_options_t;

/* Requires an untouched reader. Creates one exclusive private directory and
 * consumes traversal, preserving hierarchy and selected ancestors. It never
 * merges, overwrites, restores privileged metadata, or sandboxes its host.
 * Failures after directory creation leave private incomplete output at name.
 * TIMES restores access/modification through opened descriptors; wall times
 * require WALL_OFFSET. Optional losses require ALLOW_LOSS (initialized default)
 * and are reported through the context callback/statistics. Required losses fail.
 * Owners, modes and change times are never restored. A symbolic link, FIFO
 * or other kind fails with EUNSUPPORTED unless SKIP_UNSUPPORTED omits it and
 * counts it in the statistics' skipped_entries.
 */
void fsimg_extract_options_init(fsimg_extract_options_t *);
fsimg_status_t fsimg_extract_at(fsimg_reader_t *, int, const char *,
    const fsimg_extract_options_t *, fsimg_error_t *);

#ifdef __cplusplus
}
#endif
#endif
