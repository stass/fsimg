# C library interface

## Naming and compatibility

The public header is `fsimg.h`; applications link with `-lfsimg`. Interfaces use
portable C11 declarations and an `extern "C"` guard for C++ consumers. Every
project-defined struct and enum has a matching `_t` typedef. Typedef the object,
never its pointer. The creation API consistently uses **writer**.

The declarations below specify the interface and its semantics. Option/record
field schemas are specified in tables so their final ABI layout can be reviewed
with the public header. They are not permission to invent additional states or
weaken ownership rules. ABI stability begins with an explicitly declared stable
library release; driver and scratch interfaces remain private.

```c
typedef struct fsimg_ctx fsimg_ctx_t;
typedef struct fsimg_io fsimg_io_t;
typedef struct fsimg_reader fsimg_reader_t;
typedef struct fsimg_writer fsimg_writer_t;
typedef struct fsimg_plan fsimg_plan_t;
typedef struct fsimg_error fsimg_error_t;
typedef struct fsimg_entry fsimg_entry_t;
typedef struct fsimg_metadata fsimg_metadata_t;
typedef struct fsimg_range fsimg_range_t;
typedef struct fsimg_options fsimg_options_t;
typedef struct fsimg_read_options fsimg_read_options_t;
typedef struct fsimg_write_options fsimg_write_options_t;
typedef struct fsimg_extract_options fsimg_extract_options_t;
typedef struct fsimg_plan_info fsimg_plan_info_t;
typedef struct fsimg_verify_report fsimg_verify_report_t;
typedef struct fsimg_volume_info fsimg_volume_info_t;
typedef struct fsimg_format_info fsimg_format_info_t;
typedef struct fsimg_io_ops fsimg_io_ops_t;
typedef struct fsimg_source_ops fsimg_source_ops_t;
typedef struct fsimg_source fsimg_source_t;

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
```

`FSIMG_DONE` is an iterator result, not an error. Unknown image format and a
corrupt recognized format are distinct. Unsupported on-disk features are not
silently treated as corrupt or ignored. `FSIMG_ELIMIT` identifies a configured
budget; `FSIMG_ENOMEM` identifies allocator failure despite available budget.
OS ENOSPC and descriptor failures retain their original OS code in `FSIMG_EIO`.

Every fallible operation returns a status and accepts an optional
`fsimg_error_t *`. Errors have fixed-size storage: status, OS error, operation
kind, optional volume-relative offset, optional resource kind/limit/observed
value, bounded path and message buffers, and explicit truncation flags. They
also distinguish output absent, output present but incomplete, output published
with uncertain durability, and uncertain publication outcome. They contain no
borrowed pointers or hidden heap allocation. Formatting for terminals is the
CLI's responsibility; applications can inspect unformatted structured fields.

On success, scalar outputs are initialized and the error is cleared. On failure,
new-object output pointers are NULL and byte-count outputs state only confirmed
progress. An error never implies rollback of bytes already sent to a raw sink.
Passing a NULL error pointer discards details, not the error status.

## Options, context, and callbacks

Each public options struct begins with `size` and `version` fields and has an
initializer function such as `fsimg_options_init()`. The initializer establishes
all defaults. Callers may then change documented fields. Unknown versions,
unknown flags, nonzero reserved fields, and unsupported struct sizes fail with
`FSIMG_EINVAL`; reading beyond the caller-provided size is forbidden. This
strict rule can be extended deliberately when ABI compatibility requires it.

| `fsimg_options_t` field group | Contract |
| --- | --- |
| Limits | Memory, scratch, entries, depth, path bytes, open files, optional work/deadline ceilings; defaults in the architecture |
| Scratch directory descriptor | Authorized parent for private metadata scratch; `-1` disables spill |
| Progress callback and argument | Receives bounded counters and phase, never an accumulated event list |
| Cancellation callback and argument | Polled at each batch and external-sort pass; true requests cancellation |
| Loss-report callback and argument | Receives explicit optional-metadata losses; counts remain available when detailed reporting is capped |

```c
void fsimg_options_init(fsimg_options_t *opts);
fsimg_status_t fsimg_ctx_new(const fsimg_options_t *opts, fsimg_ctx_t **out,
    fsimg_error_t *error);
void fsimg_ctx_free(fsimg_ctx_t *ctx);
```

The context copies options and duplicates the scratch directory descriptor on
success. It owns all library allocations and scratch files. The caller keeps
callback arguments alive until context destruction. Callbacks execute
synchronously on the calling thread and cannot re-enter their active context.
The library does not read environment variables to choose limits or scratch
locations. The CLI translates environment/operands into explicit options.

Memory, scratch, and descriptor quotas aggregate across all live handles in the
context. Entry/output/work quotas and deadlines span a complete writer lifetime
or reader session, including repeated `add()`, `next()`, and content calls;
they never reset per API call. Context statistics
report current and peak allocations, scratch lengths, descriptors, and I/O.
Statistics cannot wrap into a lower value. A context is used serially, without
an internal global lock; independent contexts can run concurrently. All child
handles must be released before `fsimg_ctx_free()`. Destructors accept NULL.

## Positional I/O

```c
fsimg_status_t fsimg_io_from_fd(fsimg_ctx_t *ctx, int fd, unsigned access,
    fsimg_io_t **out, fsimg_error_t *error);
fsimg_status_t fsimg_io_from_callbacks(fsimg_ctx_t *ctx,
    const fsimg_io_ops_t *ops, void *argument, fsimg_io_t **out,
    fsimg_error_t *error);
fsimg_status_t fsimg_io_slice(fsimg_io_t *parent, uint64_t offset,
    uint64_t length, fsimg_io_t **out, fsimg_error_t *error);
void fsimg_io_free(fsimg_io_t *io);
```

The FD constructor duplicates a regular-file descriptor and checks its access
mode. A requested read-only OS capability requires an `O_RDONLY` descriptor;
an access bit does not downgrade an existing `O_RDWR` descriptor. Writable
adapters reject `O_APPEND` and unsupported direct-I/O modes. The constructor
does not clear shared status flags, change the caller's file position, or take
ownership of the original descriptor. The caller must keep shared file-status
flags stable for the adapter's lifetime. Raw devices are not accepted.
A slice borrows its parent, retains backing identity/ranges for alias checks,
and has a fixed length; it never resizes its parent. The parent must outlive it.
A reader/writer borrows its I/O object for the documented operation lifetime.
Objects from different contexts cannot be combined.

`fsimg_io_ops_t` provides `read_at`, `write_at`, `get_size`, and optional
`resize`, `flush`, `zero_range`, and `next_sparse_range` operations. Offset and
logical-size arguments are `uint64_t`; transfer lengths/counts are `size_t`.
Read/write operations receive an explicit byte-count output and error output.
The table and capability declarations are copied; its argument is borrowed
until I/O object destruction. Absent callbacks imply absent capabilities.
Callbacks cannot over-report progress, change the declared coordinate system,
or access a caller buffer after returning.

Resize and flush capabilities are independent. A sink without resize must
already have exactly the planned logical length. Slices never resize their
parents. A sink without flush promises that completed callback writes are
already visible under its logical-storage semantics, as with a fixed memory
buffer; this does not promise persistent durability. A separate durable-flush
capability is required when durable emission is requested. Missing required
capabilities fail before any sink mutation.

A successful read at EOF returns zero bytes. A short successful read is legal
at this layer; exact format reads reject premature EOF. A zero-length request
does not invoke the adapter. Custom zero-range callbacks must actually establish
zeros. An application-supplied callback is trusted application code; the
library cannot sandbox it or account for allocations it makes independently.

## Entries and sources

`fsimg_entry_t` is a public value descriptor with a `const char *path`, entry
kind, `uint64_t size`, and `fsimg_metadata_t metadata`. Its kinds are regular
file, directory, symbolic link and FIFO, as the [POSIX metadata
design](design-metadata.md) defines them; a symbolic link's size and content
are its target. Directory and FIFO sizes are zero.
Root is implicit; all other paths are absolute image paths with validated UTF-8
components and explicit parent directory entries.

Metadata has separate `valid` and `required` masks, timestamp values, and format
attributes with documented meaning. Each timestamp carries signed seconds,
nanoseconds, precision, and a time basis: UTC, known offset, or wall-clock with
unknown offset. UTC and known-offset seconds denote a UTC instant relative to
the Unix epoch; a known offset additionally retains the original minutes east
of UTC. For wall-clock values, the seconds field encodes the calendar
fields as if UTC without claiming that UTC is their actual zone. No implicit
host timezone conversion is allowed. An unset field is absent, not zero.
Volume information can instead report PER_TIMESTAMP, indicating that each
timestamp carries its own basis. This value describes format semantics and is
not a valid basis for an individual timestamp.
Owner, mode and change time are
fields with their own validity bits; a format stores them only when its
capability record says so, FAT and exFAT never do, and extraction never
restores them. Required unsupported metadata fails. Optional
omission follows the explicit writer/extraction policy and loss reporting.

A source reference is either `FSIMG_SOURCE_PROVIDER` with a `uint64_t` ID or
`FSIMG_SOURCE_ZERO`. File length comes from the entry. Directories have no source.
The writer registers at most one application source provider; this keeps
per-entry source state serializable and prevents a hidden closure per file.
Host-tree sources are supplied by the library's scanner independently.

The provider operations are `validate(argument, id, error)`,
`read_at(argument, id, offset, buffer, length, transferred, error)`, and optional
`next_range(argument, id, offset, range, error)`. They use the same status and
partial-I/O rules as byte adapters. Validation checks the caller's immutable
source contract before and after copying. An absent range callback means all
declared bytes are data. A zero source requires no backing allocation.

The provider table is copied; its argument and externally owned content remain
valid until writer destruction. No implicit ownership transfer or per-entry
release callback occurs. An application with millions of sources stores its
own source catalog by ID; its allocations are outside the library budget and
must not be mistaken for fsimg-controlled memory.

## Writer

```c
void fsimg_write_options_init(fsimg_write_options_t *opts);
fsimg_status_t fsimg_writer_new(fsimg_ctx_t *ctx, const char *format,
    const fsimg_write_options_t *opts, fsimg_writer_t **out,
    fsimg_error_t *error);
fsimg_status_t fsimg_writer_set_sources(fsimg_writer_t *writer,
    const fsimg_source_ops_t *ops, void *argument, fsimg_error_t *error);
fsimg_status_t fsimg_writer_add(fsimg_writer_t *writer,
    const fsimg_entry_t *entry, const fsimg_source_t *source,
    fsimg_error_t *error);
fsimg_status_t fsimg_writer_add_tree(fsimg_writer_t *writer, int root_fd,
    const char *image_directory, fsimg_error_t *error);
fsimg_status_t fsimg_writer_plan(fsimg_writer_t *writer,
    const fsimg_plan_t **out, fsimg_error_t *error);
fsimg_status_t fsimg_plan_get_info(const fsimg_plan_t *plan,
    fsimg_plan_info_t *out, fsimg_error_t *error);
fsimg_status_t fsimg_writer_emit(fsimg_writer_t *writer, fsimg_io_t *output,
    fsimg_error_t *error);
fsimg_status_t fsimg_writer_write_at(fsimg_writer_t *writer, int parent_fd,
    const char *name, fsimg_error_t *error);
void fsimg_writer_free(fsimg_writer_t *writer);
```

Write options specify exact or automatic volume size, minimum free bytes,
sector/allocation-unit constraints, label, required/optional metadata policy,
reproducibility policy and epoch, logical-completion versus required-durability
mode, and typed format options. Raw emission defaults to logical completion;
the file-publication helper always performs its durable publication protocol.
Automatic sizing
uses `volume_bytes == 0`. A zero label pointer selects the documented format
default. Invalid geometry is not silently substituted.

`add()` copies the entry strings and metadata before returning. Source IDs are
values, not handles to caller entry structs. `set_sources()` is called once,
before provider-backed entries are added. `add_tree()` scans synchronously,
duplicates a source-root descriptor for subsequent reopening, and records
metadata rather than payload bytes. It rejects symlinks and unsupported host
file kinds. A non-root image destination directory must be explicitly represented
in the catalog by planning time; insertion order is unrestricted.

| State | Allowed operations | Transition |
| --- | --- | --- |
| COLLECTING | Configure provider, add entries/trees, plan, emit/write | Planning seals the catalog |
| PLANNED | Inspect plan, repeat plan query, emit/write | First emission enters EMITTING |
| EMITTING | Internal emission and callbacks only | Success enters FINISHED; failure enters FAILED |
| FINISHED | Inspect plan, free | No further emission |
| FAILED | Inspect a previously successful plan, free | No resume or mutation |

Collection, planning, I/O, resource, and cancellation failures make the writer
FAILED. Invalid-state calls return `FSIMG_ESTATE` without changing the state.
`emit()` and `write_at()` plan automatically if still collecting. A successful
plan pointer remains valid until writer destruction, including after emission
failure; repeated plan queries return the same logical plan. The pointer does
not expose resident arrays and is never freed by the caller.

Plan information contains logical volume size, payload bytes, initialized data
bytes, allocated bytes, metadata bytes, free bytes, geometry, entry counts,
required output capabilities, and observed/estimated scratch use identified as
such. It does not return an O(entries) list. No progress callback can mutate a
sealed plan.

`emit()` borrows output only for the call and requires a compatible writable
sink. Before resizing, zeroing, or writing, it compares known backing identities
against all collected host sources and rejects a shared inode with `FSIMG_EINVAL`,
including through slices or different pathnames. Opaque callback sources/sinks
require the caller to guarantee disjoint storage. Distinct object pointers are
not sufficient evidence. The library cannot undo a truncation performed by the
caller before passing it a descriptor.

It sets the planned logical size only on a resizable whole-object sink; a
fixed sink or slice must already match it exactly. Every byte is defined, and
an available flush is called before success. Required durable mode additionally
requires durable-flush support. Logical completion of a memory sink is not
reported as storage durability. `emit()` does not publish a pathname. A partial
sink remains the caller's responsibility on failure.

`write_at()` takes a trusted parent directory descriptor and one validated
basename, creates private staging in that directory's filesystem, and publishes
without replacing an existing name. It owns and closes its output descriptors.
The caller's descriptor is borrowed for the call. Publication and durability
outcomes follow the [security/output contract](design-security.md#image-publication).
The helper does not enter an OS sandbox on behalf of an embedding application.

## Reader

```c
void fsimg_read_options_init(fsimg_read_options_t *opts);
fsimg_status_t fsimg_reader_open(fsimg_ctx_t *ctx, fsimg_io_t *input,
    const fsimg_read_options_t *opts, fsimg_reader_t **out,
    fsimg_error_t *error);
fsimg_status_t fsimg_reader_next(fsimg_reader_t *reader,
    const fsimg_entry_t **out, fsimg_error_t *error);
fsimg_status_t fsimg_reader_get_info(const fsimg_reader_t *reader,
    fsimg_volume_info_t *out, fsimg_error_t *error);
fsimg_status_t fsimg_reader_read_at(fsimg_reader_t *reader, uint64_t offset,
    void *buffer, size_t length, size_t *transferred, fsimg_error_t *error);
fsimg_status_t fsimg_reader_next_range(fsimg_reader_t *reader, uint64_t offset,
    fsimg_range_t *out, fsimg_error_t *error);
void fsimg_reader_free(fsimg_reader_t *reader);
```

Read options contain an optional format override and typed decoding options,
including the FAT short-name codepage policy. Input region selection is done
by constructing a slice, not by independently applying offsets in the driver.
The input object and its source remain stable and alive until reader destruction.
Host adapters check identity/size changes; concurrent hostile modification has
the same snapshot limitation as writer sources.

Opening performs bounded identification and geometry checks. The fixed-size
volume information record exposes format/profile, geometry, label, capabilities,
timestamp interpretation, and header-level validation scope, with validity bits
for unknown totals. Querying it does not traverse directories or certify the
whole volume; full verification has its own report.

`next()` validates a directory's effective lookup namespace through the bounded
preparation specified in [prepared directories](design-directories.md) before
exposing its children. Duplicate/case-equivalent names and FAT alias collisions
fail rather than selecting an arbitrary entry. It visits entries depth-first
in on-disk directory order and returns a borrowed snapshot
valid until the next `next()` call begins, a fatal reader error, or reader
destruction. It returns `FSIMG_DONE` with NULL at the end. Directory root is
implicit and is not emitted as an ordinary child entry.

Content functions operate only on the current entry: they take no entry pointer
that could belong to an old iteration or another reader. This removes a stale
entry-handle validation protocol. `read_at()` does not invalidate the snapshot;
it reads at most the remaining logical bytes, returns zero at EOF, rejects an
offset past EOF, and rejects content reads on directories. Confirmed partial
progress may accompany an I/O error; callers must check status before treating
the buffer as complete.

`next_range()` returns a positive-length DATA or ZERO range starting at the
requested logical offset and bounded by the file length. At EOF it returns
DONE. Ranges cover the file without gaps when the caller advances by the
returned length. The result contains logical offsets, not raw disk locations
that let an application bypass validation. DATA can still contain zero bytes;
ZERO guarantees zero bytes without reading uninitialized storage.

A new reader has no current entry. EOF is stable and repeated `next()` returns
DONE. Parse, I/O, resource, source-change, or cancellation errors make it FAILED
and invalidate the current snapshot. Argument/state errors detected before
work do not advance it. Rewinding means opening a new reader, not retaining a
second tree or index in the same handle.

## Extraction and verification

```c
void fsimg_extract_options_init(fsimg_extract_options_t *opts);
fsimg_status_t fsimg_extract_at(fsimg_reader_t *reader, int parent_fd,
    const char *name, const fsimg_extract_options_t *opts,
    fsimg_error_t *error);
fsimg_status_t fsimg_verify(fsimg_reader_t *reader, unsigned flags,
    fsimg_verify_report_t *out, fsimg_error_t *error);
```

Both operations require a reader before its first `next()` call and consume
its traversal. A selection path in extraction options chooses one entry or
subtree; the default is `/`. Selection preserves the hierarchy below the image
root and includes required ancestors. Matching is by validated components,
not raw string prefix. A missing selection is an error.

Extraction creates `name` exclusively as a private directory beneath the
borrowed parent descriptor. An existing file, symlink, or directory is
`FSIMG_EEXISTS`, even if empty. Options control logical-output quota, safe
timestamp preservation, and declared optional-metadata losses. Preserving a
wall-clock timestamp requires an explicit caller offset interpretation; the
library never invents a timezone. They do not
enable symlink following, existing-tree merge, privileged metadata, or device
creation. The [security specification](design-security.md) defines cleanup and
partial-output behavior. In-process extraction does not establish confinement
against a memory-corruption exploit in its host application.

Verification returns bounded counters and a named scope: structural or
structural-plus-payload-read. It reports corruption and unsupported features,
performs no repair, and never claims success for an incomplete traversal.
Detailed findings stream through the bounded reporting interface; they do not
accumulate in the report struct.

## Format inspection

```c
fsimg_status_t fsimg_format_next(size_t *cursor, fsimg_format_info_t *out,
    fsimg_error_t *error);
```

Initialize the caller-owned cursor to zero. Each successful call returns one
fixed-size capability record and advances it; exhaustion returns DONE.
Records contain bounded format/profile names, independent reader/writer flags,
entry and metadata capabilities, legal sector/allocation-unit masks and limits,
and documented profile constraints. Enumeration uses the immutable compiled
registry and allocates no per-format catalog. It does not promise that a host
can represent every theoretical format size. Runtime OS sandbox reporting is
owned by the CLI and is separate from filesystem capabilities.

## Ownership summary

| Value | Owner | Valid through |
| --- | --- | --- |
| Context and handles returned by constructors | Caller | Matching destructor; context outlives children |
| Duplicated FD adapter/source-root/scratch descriptors | Corresponding fsimg object | Object destruction; original caller FD remains caller-owned |
| Callback table | Copied by fsimg | Owner destruction |
| Callback argument/content | Caller | Owner destruction or documented synchronous call completion |
| I/O object passed to reader | Caller, borrowed by reader | Reader destruction |
| Output I/O passed to emit | Caller, borrowed by writer | End of emit call |
| Slice parent | Caller, borrowed by slice | Slice destruction |
| Entry passed to writer | Caller; values copied | Caller can release after add returns |
| Entry returned by reader | Reader | Next iteration call, fatal error, or reader destruction |
| Plan | Writer | Writer destruction after successful planning |
| Error/report value | Caller | Independent of the failed object |

Destruction releases memory, scratch, and owned descriptors on every path.
Success-sensitive flush/close operations occur before successful operation
return, not solely in void destructors. Destruction cannot convert a failed
operation into success or remove a successfully published output.

## Resource and byte-adapter records

The public header defines fixed-size error and statistics records. Errors carry
operation/resource/output-state enums and bounded 256-byte path/message arrays.
Cumulative I/O statistics saturate at `UINT64_MAX`; live-resource counters are
checked before reservation and retain their exact value on failure. Allocation
accounting includes the context and alignment-preserving allocation headers.
The statistics query is `fsimg_ctx_get_stats(ctx, out, error)`; status labels are
available through `fsimg_status_string(status)` without allocation.

`fsimg_options_t` uses `uint64_t` byte/count limits, `uint32_t` depth/path/FD
limits, separate callback arguments, and zeroed reserved words. Its `flags`
field accepts zero. Progress snapshots borrow a phase string for the duration
of the synchronous callback. Destructors called from a callback do nothing;
fallible reentry returns `FSIMG_ESTATE`. This protects the active context without
introducing deferred destruction or callback-driven lifecycle transitions.

`fsimg_io_ops_init()` initializes the callback table. Its `max_transfer` limits
each callback request; zero selects the host transfer maximum. Read/write
capabilities follow callback presence. `FSIMG_IO_DURABLE` requires a flush
callback and is the only table flag. Built-in regular-file FDs advertise durable
flush. Sparse-range discovery is optional and has no bearing on byte correctness.

Byte helpers use the same status/error convention:

* `fsimg_io_get_size(io, out, error)` reports the declared logical length.
* `fsimg_io_read_at()` and `fsimg_io_write_at()` take an offset, buffer,
  `size_t` length, and confirmed-transfer output. A non-exact transfer may be
  limited by the adapter's maximum transfer.
* `fsimg_io_read_exact()` and `fsimg_io_write_exact()` take the same arguments
  and loop until completion or error. Exact EOF is an I/O error; zero-length
  requests at the object's end succeed without calling the adapter.
* `fsimg_io_resize(io, length, error)` requires a writable whole-object adapter
  with resize support. A slice never gains that capability. Shrinking a backing
  object makes any no-longer-contained slice unusable until it fits again.
* `fsimg_io_zero_range(io, offset, length, error)` establishes zeros with a
  callback or a charged bounded zero buffer. A failure after possible mutation
  reports incomplete output, including cancellation between successful batches.
* `fsimg_io_flush(io, durable, error)` performs available flush semantics.
  Required durable mode rejects a sink without durable capability before
  invoking a callback; logical completion without a flush callback succeeds.
