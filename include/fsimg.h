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
typedef struct fsimg_image fsimg_image_t;
typedef struct fsimg_layer_options fsimg_layer_options_t;

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
	FSIMG_OP_EXTRACT,
	FSIMG_OP_IMAGE
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
	FSIMG_RESOURCE_PATH,
	FSIMG_RESOURCE_SPOOL
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
	/* The logical bytes of every live spool, holding a stream or a
	 * decompressed input; default 64 GiB.
	 */
	uint64_t	 spool_bytes;
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
	/* Logical spool bytes reserved, and the allocated blocks sampled once
	 * a spool is filled, which holes for zero input keep below them.
	 */
	uint64_t	 spool_bytes;
	uint64_t	 spool_peak;
	uint64_t	 spool_physical_bytes;
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
/* The next range at or after offset, below the object's declared length,
 * that reads as zero: offset <= *start <= *end <= length, and an empty
 * answer (*start == *end) says no zero range begins before *start. Each
 * answer makes progress: *start < *end, *start > offset, or
 * *start == *end == length. The library checks the shape and relies on
 * the contents, encoding a reported range as zeros without reading it.
 */
typedef fsimg_status_t (*fsimg_next_zero_fn_t)(void *, uint64_t, uint64_t,
    uint64_t *, uint64_t *, fsimg_error_t *);

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
	fsimg_next_zero_fn_t next_zero_range;
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
 * next_zero_range is optional, requires read_at and spares reads only; a
 * read-only descriptor object instead answers from the host's hole report
 * after syncing the file once, never for the chunk holding its last byte,
 * and reads in full when the sync fails. Its queries seek the file offset
 * the descriptor shares with the caller's and restore it before
 * returning, so nothing else may use that offset while an operation reads
 * the object. A writable descriptor object answers nothing. Zeroing never
 * consults either answer.
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

typedef enum fsimg_validation_scope {
	FSIMG_VALIDATION_HEADER = 1,
	FSIMG_VALIDATION_STRUCTURAL,
	FSIMG_VALIDATION_PAYLOAD
} fsimg_validation_scope_t;

/* The layers around a volume. A stack has three slots in fixed order,
 * indexed by the layer kind that fills them; compression wraps the whole
 * file and is not a slot.
 */
typedef enum fsimg_layer_kind {
	FSIMG_LAYER_FILESYSTEM = 0,
	FSIMG_LAYER_SCHEME = 1,
	FSIMG_LAYER_CONTAINER = 2,
	FSIMG_LAYER_COMPRESSION = 3
} fsimg_layer_kind_t;

#define FSIMG_STACK_SLOTS 3

/* An unspecified slot is detected on read and empty on create; an empty
 * slot is forced empty; a named slot is forced to its layer.
 */
typedef enum fsimg_slot_state {
	FSIMG_SLOT_UNSPECIFIED = 0,
	FSIMG_SLOT_EMPTY = 1,
	FSIMG_SLOT_NAMED = 2
} fsimg_slot_state_t;

typedef struct fsimg_stack_slot {
	fsimg_slot_state_t state;
	char		 name[16];	/* NUL-terminated; empty unless NAMED */
} fsimg_stack_slot_t;

/* A zeroed stack leaves every slot to detection. */
typedef struct fsimg_stack {
	fsimg_stack_slot_t slots[FSIMG_STACK_SLOTS];
} fsimg_stack_t;

/* One registered scheme, container or compression. capabilities holds the
 * FSIMG_CAP_* operations this build implements; suffix is the file suffix
 * naming a container or compression, empty for a scheme. settings lists
 * the keys the layer stores, comma-separated and unqualified ("type,name"),
 * empty for none. codecs lists the chunk encodings a container reads
 * ("raw,zero,zlib"), and codec_version names the library a layer's codecs
 * depend on with its version ("zlib 1.3.1"), both empty for none:
 * compressed bytes are reproducible only for one zlib.
 */
typedef struct fsimg_layer_info {
	char		 name[16];
	char		 suffix[8];
	char		 settings[64];
	char		 codecs[32];
	char		 codec_version[32];
	fsimg_layer_kind_t kind;
	unsigned	 capabilities;
} fsimg_layer_info_t;

/* Typed layer settings, one field per key of a layer, zero (empty) for its
 * default. A field of a layer the stack does not name must hold its
 * default, and so must every field reading does not use: on read only
 * gpt_sector_bytes, which requires gpt in the forced stack.
 *
 * gpt_sector_bytes is the GPT LBA size, 512 or 4096: zero detects it on
 * read and selects 512 on write. gpt_disk_bytes and mbr_disk_bytes fix the
 * disk length, a multiple of the scheme's sector; zero derives it from the
 * volume. gpt_type is a type-table name or GUID text and mbr_type a name or
 * "0x" and two hex digits; empty selects the type named after the
 * filesystem. gpt_name is UTF-8 of at most 36 UTF-16 units; empty selects
 * the type's conventional name. The GUIDs are in textual (RFC 4122) byte
 * order; all zero leaves the identifier unset, and so does a clear
 * mbr_id_set. An unset identifier is derived from the epoch in reproducible
 * mode and is otherwise FSIMG_EINVAL naming its key. mbr_active marks the
 * partition active (status 0x80).
 *
 * The container settings: dmg_compress stores data chunks as zlib (UDZO)
 * or raw; dmg_level is the zlib level, 1 to 9, zero selecting 6;
 * dmg_chunk_bytes is the chunk size, a multiple of 512 up to 16 MiB, zero
 * selecting 1 MiB. vhd_align_bytes is the disk-length granularity, a power
 * of two from 512 to 1 GiB, zero selecting 512. simg_block_bytes is the
 * block size, a power of two from 1 KiB to 1 MiB, zero selecting 4096, and
 * simg_crc appends a CRC32 chunk. dmg_uuid (the segment ID) and vhd_uuid
 * are identifiers like the GUIDs above.
 *
 * The compression setting: gzip_level is the DEFLATE level of an output
 * the write options compress with gzip, 1 to 9, zero selecting 6.
 */
typedef enum fsimg_dmg_compression {
	FSIMG_DMG_ZLIB = 0,
	FSIMG_DMG_NONE = 1
} fsimg_dmg_compression_t;

struct fsimg_layer_options {
	uint32_t	 gpt_sector_bytes;
	uint64_t	 gpt_disk_bytes;
	uint64_t	 mbr_disk_bytes;
	char		 gpt_type[40];
	char		 gpt_name[112];
	char		 mbr_type[16];
	unsigned char	 gpt_disk_guid[16];
	unsigned char	 gpt_guid[16];
	uint32_t	 mbr_id;
	bool		 mbr_id_set;
	bool		 mbr_active;
	fsimg_dmg_compression_t dmg_compress;
	uint32_t	 dmg_level;
	uint32_t	 dmg_chunk_bytes;
	unsigned char	 dmg_uuid[16];
	uint32_t	 vhd_align_bytes;
	unsigned char	 vhd_uuid[16];
	uint32_t	 simg_block_bytes;
	bool		 simg_crc;
	uint32_t	 gzip_level;
	uint64_t	 reserved[4];
};

/* No image option flags are defined; flags must be zero.
 *
 * compression forces the input's compression, as -z does: a registered
 * compression's name, NUL-terminated, whose probe alone runs at the
 * compression step and must claim the input; empty detects it.
 *
 * spool_fd is the spool, an exclusively created, empty, read/write regular
 * file without O_APPEND that the caller has unlinked, or -1 for none, the
 * caller keeping no other descriptor or mapping of it and never writing
 * it, since fsimg reads back as zero what it never wrote there; the
 * value fsimg_image_options_init() sets. Options whose size
 * fsimg_image_options_init() did not set are refused before spool_fd is
 * examined, so zero-filled options never hand over descriptor 0. Once the
 * options' size and version are valid spool_fd is adopted on every
 * outcome; open() keeps it only when the input is compressed and
 * otherwise closes it, and open_stream() requires it.
 */
typedef struct fsimg_image_options {
	size_t		 size;
	uint32_t	 version;
	uint32_t	 flags;
	fsimg_stack_t	 stack;		/* The forced slots */
	fsimg_layer_options_t layers;
	char		 compression[16];
	int		 spool_fd;
	uint64_t	 reserved[4];
} fsimg_image_options_t;

/* stack is the canonical text of the layers found: the container and the
 * scheme when detected or forced, and the filesystem when forced or once
 * select() has identified the selected volume. container names the
 * container the disk was peeled from, empty for none; codecs lists the
 * chunk encodings its index holds ("raw,zero,zlib"), chunk_count its
 * chunks (one for a fixed VHD, whose disk is one raw run), and
 * container_guid the VHD unique identifier or DMG segment ID, all zero
 * when it has none. compression names the compression decoded into the
 * spool, empty for none. file_bytes is the length of the file the layers
 * were peeled from: the input, or the spool holding a stream or a
 * decompressed input; a plan reports its planned disk as the file.
 * disk_bytes is the disk the container yields, or file_bytes without one.
 * GUIDs are in textual (RFC 4122) byte order.
 * selected is the slot of the last partition select() returned, zero for
 * none. The scope is the validation the tables received.
 */
typedef struct fsimg_image_info {
	char		 stack[64];
	char		 compression[16];
	char		 container[16];
	char		 codecs[32];
	char		 scheme[16];
	unsigned char	 container_guid[16];
	unsigned char	 disk_guid[16];
	uint64_t	 chunk_count;
	uint64_t	 file_bytes;
	uint64_t	 disk_bytes;
	uint32_t	 sector_bytes;	/* Zero without a scheme */
	uint32_t	 partition_count; /* Table slots, used or not */
	uint32_t	 selected;
	uint32_t	 mbr_disk_id;
	fsimg_validation_scope_t scope;
} fsimg_image_info_t;

/* What the filesystem probes say about a partition's first bytes. */
typedef enum fsimg_claim {
	FSIMG_CLAIM_NONE = 0,
	FSIMG_CLAIM_FORMAT = 1,		/* format names the filesystem */
	FSIMG_CLAIM_UNSUPPORTED = 2,
	FSIMG_CLAIM_CORRUPT = 3,
	FSIMG_CLAIM_AMBIGUOUS = 4
} fsimg_claim_t;

/* One table slot, numbered from 1 as sgdisk and /dev/sda1 count. type is
 * the type table's name, or empty for a type the table lacks. A GPT entry
 * fills type_guid, guid and name (UTF-8; an unpaired surrogate reads as
 * U+FFFD); an MBR entry fills mbr_type and active. An unused slot has only
 * index set; its claim is NONE.
 */
typedef struct fsimg_partition_info {
	char		 type[16];
	char		 format[16];
	char		 name[112];
	unsigned char	 type_guid[16];
	unsigned char	 guid[16];
	uint64_t	 offset;	/* Disk offset in bytes */
	uint64_t	 length;
	uint32_t	 index;
	uint32_t	 mbr_type;
	fsimg_claim_t	 claim;
	bool		 used;
	bool		 active;
} fsimg_partition_info_t;

typedef enum fsimg_selector_kind {
	FSIMG_SELECT_AUTOMATIC = 0,
	FSIMG_SELECT_INDEX,
	FSIMG_SELECT_NAME,
	FSIMG_SELECT_TYPE,
	FSIMG_SELECT_RANGE,
	FSIMG_SELECT_DISK
} fsimg_selector_kind_t;

/* index for INDEX; text for NAME (UTF-8) and TYPE (a type-table name, GUID
 * text or "0x" and two hex digits), borrowed for the call; offset and
 * length for RANGE, where a zero length extends to the end of the disk.
 */
typedef struct fsimg_selector {
	fsimg_selector_kind_t kind;
	uint32_t	 index;
	const char	*text;
	uint64_t	 offset;
	uint64_t	 length;
} fsimg_selector_t;

/* container names the verified container and scheme the verified table,
 * each empty without one; partitions counts the used slots whose bounds
 * were checked. The scope is FSIMG_VALIDATION_PAYLOAD when a container's
 * chunks were decoded and checked under FSIMG_VERIFY_PAYLOAD. The
 * filesystem of a selected partition is verified separately, and no other
 * partition's contents.
 */
typedef struct fsimg_layer_report {
	char		 compression[16];	/* Members checked at spool time */
	char		 container[16];
	char		 scheme[16];
	uint32_t	 partitions;
	fsimg_validation_scope_t scope;
	bool		 complete;
} fsimg_layer_report_t;

#define FSIMG_WRITE_ALLOW_LOSS 0x01u
#define FSIMG_WRITE_REPRODUCIBLE 0x02u
#define FSIMG_WRITE_DURABLE 0x04u
/* Host trees also record uid, gid and permission bits as optional fields. */
#define FSIMG_WRITE_IMPORT_POSIX 0x08u

/* The on-disk byte order of a format that offers both, such as QNX6. */
typedef enum fsimg_byte_order {
	FSIMG_BYTE_ORDER_LITTLE = 0,
	FSIMG_BYTE_ORDER_BIG = 1
} fsimg_byte_order_t;

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
	/* Format-specific settings: a format whose record lacks the matching
	 * FSIMG_SETTING_* bit requires the default.
	 */
	fsimg_byte_order_t byte_order; /* Default little. */
	uint32_t	 inode_capacity; /* Zero selects the format's default. */
	uint32_t	 reserve_percent; /* Default 3; at most 100. */
	/* Default zero; used verbatim in all modes, in textual byte order. */
	unsigned char	 volume_uuid[16];
	/* Borrowed for construction and copied; NULL selects every default. */
	const fsimg_layer_options_t *layers;
	/* The compression wrapping the whole output, as -z says: a registered
	 * compression's name, NUL-terminated, such as "gzip", whose layer
	 * settings then apply; empty for none, the default. A name the
	 * registry lacks is FSIMG_EINVAL and one this build cannot write
	 * FSIMG_EUNSUPPORTED, each naming it in error.path.
	 */
	char		 compression[16];
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
	/* The image around the volume, reported as an opened image reports
	 * it, with every identifier the plan derived. image.disk_bytes is the
	 * disk, which equals volume_bytes without a scheme or container;
	 * image.scope is zero. With a scheme, partition is the volume's slot
	 * (image.selected): partition.offset is the volume offset and
	 * partition.length the partition's whole sectors, at least
	 * volume_bytes. Without one, partition is zero. With a container,
	 * image.container and image.container_guid name it and its identifier.
	 */
	fsimg_image_info_t image;
	fsimg_partition_info_t partition;
	/* output_bytes is the most bytes the output can take: exactly the disk
	 * without a container or compression, else the largest encoding of
	 * it, the container's and then gzip's, which always exceeds the disk
	 * since each adds its own metadata. A raw intermediate holds the disk
	 * while an encoded output or a stream is written, so the output
	 * budget is then image.disk_bytes + output_bytes. image.compression
	 * names the compression the write options name, empty for none.
	 * padding_bytes is the zero padding the container's disk-length
	 * granularity added to the disk. codec_version names the compression
	 * library the output depends on ("zlib 1.3.1"), empty for none.
	 */
	uint64_t	 output_bytes;
	uint64_t	 padding_bytes;
	char		 codec_version[32];
} fsimg_plan_info_t;

/* Where an output operation writes. FILE stages a new file beneath the
 * trusted directory parent_fd and publishes it as name without replacing
 * anything, durably, as write_at does. IO writes the positional object io
 * from offset zero, as emit does. STREAM writes stream_fd sequentially,
 * borrowed for the call. raw_fd is the raw intermediate a container,
 * compressed or stream output of a writer needs: an exclusively created,
 * empty, unlinked read/write regular file without O_APPEND, of which the
 * caller keeps no other descriptor or mapping and which nothing else
 * writes, or -1; every other output
 * takes -1. fsimg_sink_init makes an empty sink of a kind, with size set
 * and every descriptor -1, so a caller fills in only what its kind uses. A
 * sink whose size fsimg_sink_init did not set is refused untouched, so a
 * zeroed value never hands over descriptor 0. The value and io are
 * borrowed for the call; once its size is valid, raw_fd is adopted, and
 * closed, on every outcome, a refused sink included. A stream has no publication: a failure after
 * its first byte reports FSIMG_OUTPUT_INCOMPLETE, and the caller must
 * discard what it received. A container or compressed output to an IO
 * sink needs a resizable whole object, since the encoded length is known
 * only at the end; the sink ends at that length.
 */
typedef enum fsimg_sink_kind {
	FSIMG_SINK_FILE = 1,
	FSIMG_SINK_IO,
	FSIMG_SINK_STREAM
} fsimg_sink_kind_t;

typedef struct fsimg_sink {
	size_t		 size;
	fsimg_sink_kind_t kind;
	int		 parent_fd;
	const char	*name;
	fsimg_io_t	*io;
	int		 stream_fd;
	int		 raw_fd;
} fsimg_sink_t;

void fsimg_sink_init(fsimg_sink_t *, fsimg_sink_kind_t);

/* Writers copy options/labels and own their borrowed plan until destruction.
 * plan seals collection; subsequent plan queries return the same object.
 * emit/write_at plan automatically and attempt emission only once. A failed
 * writer retains a plan that plan returned for inspection; a plan a later
 * planning step refused is not kept. Raw sinks may be incomplete on failure;
 * known source aliases are rejected before mutation. Callback source/sink
 * storage must be disjoint. Planning refuses an output whose bound, plus the
 * raw disk an encoded output is written from, exceeds max_output_bytes; output
 * charges a stream's raw intermediate before emitting. Fixed sinks must match
 * the planned size; whole resizable sinks are resized. Every byte is defined.
 * ALLOW_LOSS permits optional timestamp rounding/omission, time-basis loss and
 * omission of a field the format does not store, counted once per field;
 * required losses and entry kinds the format lacks always fail planning.
 * Counts remain available in plan information.
 * add_tree takes a readable directory root, whose tree populates image_path,
 * or a regular-file root, the one file at a non-root image_path. Planning
 * merges a directory that several sources supply into one directory holding
 * all their children, with the metadata of a scanned record over a
 * programmatic one and of the first-added root among scanned records;
 * programmatic directories of one path with no scanned record of that path
 * merge with each other only when their metadata are identical. Any other
 * repeated path, a dropped directory that requires metadata, or such
 * programmatic directories whose metadata differ fail with FSIMG_EINVAL
 * naming the path in error.path.
 * add_manifest imports an mtree(5) manifest as one more root, read from
 * manifest_fd, an O_RDONLY regular file borrowed for the call, from offset
 * zero to its length when the call begins. contents_fd is an O_RDONLY
 * directory, duplicated, beneath which host files supply the bytes of
 * entries that need them, or -1 when none does; the directory is taken as
 * a root only once an entry needs host bytes, and an entry that needs them
 * without one fails. Its records rank as that root's scanned records. A
 * failure while reading the manifest names the line by byte offset in
 * error.offset and by number in error.message, and the entry's image path
 * in error.path. Planning checks the sha256digest of generated content
 * once the image has accepted every size, and a mismatch fails it with
 * FSIMG_EINVAL naming the line and path the same way. A host file that
 * changed, or whose bytes differ from the manifest's sha256digest, fails
 * emission with FSIMG_ESOURCE_CHANGED.
 * write_at requires a single basename beneath a trusted directory descriptor,
 * publishes without replacement, and always performs durable publication.
 * On unsupported publication error.path names the retained image relative to
 * parent_fd; when cleanup_error is nonzero it names the retained directory.
 * The format is a stack naming the filesystem: "fat32" writes a bare
 * volume, "fat32/gpt" a disk holding it in one partition, whose layout
 * the plan reports and whose size every sink must accept, and
 * "fat32/gpt/dmg" that disk in a container, which only output writes.
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
fsimg_status_t fsimg_writer_add_manifest(fsimg_writer_t *, int, int,
    fsimg_error_t *);
fsimg_status_t fsimg_writer_plan(fsimg_writer_t *, const fsimg_plan_t **,
    fsimg_error_t *);
fsimg_status_t fsimg_plan_get_info(const fsimg_plan_t *, fsimg_plan_info_t *,
    fsimg_error_t *);
fsimg_status_t fsimg_writer_emit(fsimg_writer_t *, fsimg_io_t *, fsimg_error_t *);
fsimg_status_t fsimg_writer_write_at(fsimg_writer_t *, int, const char *,
    fsimg_error_t *);
/* output generalises emit (an IO sink) and write_at (a FILE sink), which
 * write only stacks without a container and refuse others. A container or
 * a STREAM sink needs the sink's raw intermediate: the writer emits the
 * raw disk into it, then one sequential transcode writes the container,
 * or copies the disk, to the sink.
 */
fsimg_status_t fsimg_writer_output(fsimg_writer_t *, const fsimg_sink_t *,
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
	/* A label of at most 48 bytes and its terminator. */
	char		 label[49];
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

/* Format-specific fields of fsimg_write_options_t. */
#define FSIMG_SETTING_BYTE_ORDER 0x01u
#define FSIMG_SETTING_VOLUME_UUID 0x02u
#define FSIMG_SETTING_INODE_CAPACITY 0x04u
#define FSIMG_SETTING_RESERVE 0x08u

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
	/* FSIMG_SETTING_* write options the format stores. */
	uint32_t	 write_settings;
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

/* parse applies the one stack grammar and fails with FSIMG_EINVAL naming
 * the offending token in error.path. format writes the canonical text,
 * omitting empty and unspecified slots, NUL-terminated within capacity, and
 * returns the length it needs without the terminator.
 *
 * An image borrows its input until destruction; the input must be
 * seekable. Opening validates the options and the forced stack, then
 * decides compression: the compression probes run on the input's prefix
 * with the container, scheme and filesystem probes. A compression claiming
 * alone means compressed; a compression and another probe claiming is an
 * ambiguity that a stack with any slot named or empty resolves as not
 * compressed. A forced compression runs its probe alone, and an input it
 * does not claim is FSIMG_EFORMAT naming it. A compressed input is decoded into
 * the spool, member by member, each member's CRC32 and length checked and
 * bytes after the last member refused, with the spool's logical length
 * reserved against spool_bytes before each step; the spool is then the
 * file. open_stream reads stream_fd, a blocking descriptor borrowed for the
 * call, to its end into the spool, decoding it when compressed and copying
 * it unchanged otherwise; its probes run on the first block without the
 * container probes, which need the end of the file. Without a spool,
 * compressed and stream input fail as a FSIMG_RESOURCE_SPOOL limit of
 * zero. Then opening peels
 * the container: the DMG, fixed VHD and sparse-image probes run on the
 * input's prefix and last 512 bytes, or only the forced one, and a claim's
 * tables are validated completely, so the disk has a fixed length before
 * any other byte is read; a damaged or unsupported container fails the
 * open and is never read as a raw disk. The scheme is detected, and its
 * tables validated completely, at most once, on the first get_info,
 * next_partition, verify or partition-based select. A range or disk
 * selection never consults the scheme, so a damaged table cannot block it.
 * next_partition yields each slot from 1, then DONE. select returns a
 * read-only slice for fsimg_reader_open; the slice borrows the image and
 * is released with fsimg_io_free before it. verify checks every table and
 * the container's checksums, decoding every chunk under
 * FSIMG_VERIFY_PAYLOAD, and consumes nothing.
 */
fsimg_status_t fsimg_stack_parse(const char *, fsimg_stack_t *,
    fsimg_error_t *);
size_t fsimg_stack_format(const fsimg_stack_t *, char *, size_t);
fsimg_status_t fsimg_layer_next(size_t *, fsimg_layer_info_t *,
    fsimg_error_t *);
void fsimg_layer_options_init(fsimg_layer_options_t *);
void fsimg_image_options_init(fsimg_image_options_t *);
fsimg_status_t fsimg_image_open(fsimg_ctx_t *, fsimg_io_t *,
    const fsimg_image_options_t *, fsimg_image_t **, fsimg_error_t *);
fsimg_status_t fsimg_image_open_stream(fsimg_ctx_t *, int,
    const fsimg_image_options_t *, fsimg_image_t **, fsimg_error_t *);
fsimg_status_t fsimg_image_get_info(fsimg_image_t *, fsimg_image_info_t *,
    fsimg_error_t *);
fsimg_status_t fsimg_image_next_partition(fsimg_image_t *,
    fsimg_partition_info_t *, fsimg_error_t *);
fsimg_status_t fsimg_image_select(fsimg_image_t *, const fsimg_selector_t *,
    fsimg_io_t **, fsimg_error_t *);
fsimg_status_t fsimg_image_verify(fsimg_image_t *, unsigned,
    fsimg_layer_report_t *, fsimg_error_t *);
void fsimg_image_free(fsimg_image_t *);

/* One partition of a composed disk: a borrowed volume, its GPT name (UTF-8,
 * NULL for the type's conventional name), its type as gpt_type or mbr_type
 * spell it (NULL for the type of the filesystem the volume's prefix
 * identifies), and its unique GPT GUID in textual byte order (all zero is
 * unset, as for gpt_guid).
 */
typedef struct fsimg_part {
	fsimg_io_t	*volume;
	const char	*name;
	const char	*type;
	unsigned char	 guid[16];
} fsimg_part_t;

/* Image operations without a filesystem writer. Each reads its inputs once,
 * in disk order, and writes the sink sequentially; inputs are borrowed for
 * the call and refused when they share storage with the sink. Write options
 * supply only the mode flags, epoch, layers and compression; every
 * filesystem setting must hold its default.
 *
 * wrap puts a raw volume into the stack's scheme as its one partition,
 * typed by gpt_type or mbr_type, else by the filesystem the stack names or
 * the volume's prefix identifies; without a scheme the volume is the
 * disk. unwrap writes the bytes a selector names. compose places count
 * volumes in order, each 1 MiB aligned, under a stack naming a scheme and
 * no filesystem; the layer options' partition fields must hold their
 * defaults, since each part carries its own. A container in the stack of
 * wrap or compose encodes the disk on its way to the sink, in the one
 * transcode a writer uses, and the options' compression compresses the
 * output of any of the three. None of the three stages a raw disk, so each
 * refuses a sink's raw intermediate, and a STREAM sink receives the bytes
 * as they are produced.
 */
fsimg_status_t fsimg_image_wrap(fsimg_ctx_t *, fsimg_io_t *, const char *,
    const fsimg_write_options_t *, const fsimg_sink_t *, fsimg_error_t *);
fsimg_status_t fsimg_image_unwrap(fsimg_image_t *, const fsimg_selector_t *,
    const fsimg_write_options_t *, const fsimg_sink_t *, fsimg_error_t *);
fsimg_status_t fsimg_image_compose(fsimg_ctx_t *, const char *,
    const fsimg_part_t *, size_t, const fsimg_write_options_t *,
    const fsimg_sink_t *, fsimg_error_t *);

#ifdef __cplusplus
}
#endif
#endif
