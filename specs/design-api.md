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
typedef struct fsimg_stack fsimg_stack_t;
typedef struct fsimg_layer_options fsimg_layer_options_t;
typedef struct fsimg_image fsimg_image_t;
typedef struct fsimg_image_options fsimg_image_options_t;
typedef struct fsimg_image_info fsimg_image_info_t;
typedef struct fsimg_partition_info fsimg_partition_info_t;
typedef struct fsimg_selector fsimg_selector_t;
typedef struct fsimg_sink fsimg_sink_t;
typedef struct fsimg_part fsimg_part_t;
typedef struct fsimg_layer_report fsimg_layer_report_t;
typedef struct fsimg_layer_info fsimg_layer_info_t;

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

A struct that hands the library a descriptor, to adopt or to duplicate,
begins with `size` too and is checked by it before any descriptor field is
examined: the context options' scratch directory, the image options'
spool and the sink's raw intermediate. Each initializer sets every
descriptor field to `-1`, and a struct whose `size` its initializer did not
set is `FSIMG_EINVAL` with nothing adopted, duplicated or closed, so a
zero-filled struct never hands over descriptor 0, standard input. Write
options and the wrap, unwrap and compose arguments carry no descriptor:
their output's descriptors travel in the sink.

| `fsimg_options_t` field group | Contract |
| --- | --- |
| Limits | Memory, scratch, spool, entries, depth, path bytes, open files, optional work/deadline ceilings; defaults in the architecture |
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
The object is an external file. Read-only, it answers zero ranges from the
host's hole report after syncing the file once, bounded by the length the
query observes and never covering the chunk that holds its last byte, fails
with `FSIMG_ESOURCE_CHANGED` when the file is observed shorter than its
declared length, and answers nothing once the sync fails, as the
[hole design](design-holes.md#external-files-the-hosts-report) states;
under Capsicum a descriptor needs `CAP_FSYNC` for that, and without it the
file is simply read in full. Its hole queries seek the file offset the
descriptor shares with the caller's and restore it before returning, so
nothing else may use that offset while an operation reads the object.
Writable, it answers nothing; zeroing never
consults a hole report, and a sink without a fresh file's record is zeroed
in full.
A slice borrows its parent, retains backing identity/ranges for alias checks,
and has a fixed length; it never resizes its parent. The parent must outlive it.
A reader/writer borrows its I/O object for the documented operation lifetime.
Objects from different contexts cannot be combined.

`fsimg_io_ops_t` provides `read_at`, `write_at`, `get_size`, and optional
`resize`, `flush`, `zero_range`, and `next_zero_range` operations. Offset and
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
zeros. A `next_zero_range` callback reports the next range at or after an
offset that reads as zero, under the contract in the
[hole design](design-holes.md#the-callback): it requires `read_at`, the
library checks that its answer lies within the object and makes progress,
and the library relies on the answer's contents as it relies on
`zero_range`, encoding a reported range as zeros without reading it. An
application-supplied callback is trusted application code; the library
cannot sandbox it or account for allocations it makes independently.

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
    const char *image_path, fsimg_error_t *error);
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

The `format` argument is a [stack](design-layers.md#the-stack) whose
filesystem slot is present: `fat32` writes a bare volume and `fat32/gpt/dmg`
a partitioned disk in a container. Write options specify exact or automatic
volume size, minimum free bytes, sector/allocation-unit constraints, label,
required/optional metadata policy, reproducibility policy and epoch,
logical-completion versus required-durability mode, typed format options, and
a borrowed `layers` pointer to an `fsimg_layer_options_t` with the typed
settings of every layer (NULL selects every default). The writer copies both
structures. A layer setting whose layer is absent from the stack must hold its
default, exactly as a format setting a format does not store; an
[identifier](design-layers.md#identifiers) of a layer in the stack must be
set unless the reproducible flag is given. Compression is not a stack slot:
the write options' `compression` field, the library's `-z` and the
write-side counterpart of the image options' field of that name, names one
registered compression around whatever the stack writes, or is empty for
none. It is resolved through the [layer registry](design-layers.md#the-stack),
so exactly one compression is representable and a new codec is one registry
entry: a name the registry lacks as a compression is `FSIMG_EINVAL`, and one
this build cannot write `FSIMG_EUNSUPPORTED`, each naming it. The named
compression's settings, `gzip_level` for gzip, then apply; without it they
must hold their defaults like the settings of any absent layer. The field
applies equally to wrap, unwrap and compose, whose write options carry the
mode. Raw emission
defaults to logical completion; the file-publication helper always performs
its durable publication protocol.
Automatic sizing
uses `volume_bytes == 0`; `FSIMG_WRITE_MINIMIZE` selects the
[minimization policy](design-sizing.md) over the settings left open and is
invalid with an exact size. A zero label pointer selects the documented format
default. Invalid geometry is not silently substituted.

`add()` copies the entry strings and metadata before returning. Source IDs are
values, not handles to caller entry structs. `set_sources()` is called once,
before provider-backed entries are added. `add_tree()` accepts a directory or
regular-file root descriptor, as the [sources design](design-sources.md)
defines: a directory root contributes its tree beneath `image_path`, a
regular-file root one file at a non-root `image_path`. It scans
synchronously, duplicates the root descriptor for subsequent source access
(reopening beneath a directory, reading a regular file in place), and
records metadata rather than payload bytes. It rejects symlinks and unsupported host file
kinds. A non-root image destination directory must be explicitly represented
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
such. Its image record reports the planned disk as an opened image of it
would, the compression the write options name included; the output
bound is the disk's encoding by the container and then the compression. It does not return an O(entries) list. No progress callback can mutate a
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

`fsimg_plan_get_info()` reports the image around the volume as an opened
image reports it: an `fsimg_image_info_t` whose disk length is the whole
output, and, with a scheme, the volume's `fsimg_partition_info_t` (its offset,
its whole-sector length, type, name and unique GUID). Every identifier the
plan derived appears there, so a caller prints them before any byte is
written.

```c
void fsimg_sink_init(fsimg_sink_t *sink, fsimg_sink_kind_t kind);
fsimg_status_t fsimg_writer_output(fsimg_writer_t *writer,
    const fsimg_sink_t *sink, fsimg_error_t *error);
```

`output()` generalises both: `fsimg_sink_t` is a caller-owned value naming a
file (parent descriptor and basename, as `write_at()`), a positional I/O
object (as `emit()`), or a stream descriptor written sequentially, plus
`raw_fd`, the [raw intermediate](design-layers.md#handed-over-files): an
exclusively created, empty, read/write regular file the caller has unlinked,
adopted on every outcome once the sink is accepted, required whenever the
stack has a container or compression or the sink is a stream, and `-1`
otherwise. `fsimg_sink_init()` makes an empty sink of a kind, its `size`
set and every descriptor `-1`, under the
[descriptor rule](#options-context-and-callbacks). The library never
creates that file by name, and checks the one it adopts: a sink that needs
one and has none, or whose descriptor is not a regular file of size zero
with link count zero open for reading and writing, is `FSIMG_EINVAL` before
any mutation. `emit()` and `write_at()` are the
positional and file cases for stacks that need no raw intermediate. With one
the writer stages the raw disk there and transcodes it as the
[layer design](design-layers.md#writing) states. A stream sink has no
publication; a failure after the first byte reports
`FSIMG_OUTPUT_INCOMPLETE`, and the caller must discard what it received.

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

## Images and layers

```c
fsimg_status_t fsimg_stack_parse(const char *text, fsimg_stack_t *out,
    fsimg_error_t *error);
size_t fsimg_stack_format(const fsimg_stack_t *stack, char *buffer,
    size_t capacity);
void fsimg_layer_options_init(fsimg_layer_options_t *opts);
void fsimg_image_options_init(fsimg_image_options_t *opts);
fsimg_status_t fsimg_image_open(fsimg_ctx_t *ctx, fsimg_io_t *input,
    const fsimg_image_options_t *opts, fsimg_image_t **out,
    fsimg_error_t *error);
fsimg_status_t fsimg_image_open_stream(fsimg_ctx_t *ctx, int stream_fd,
    const fsimg_image_options_t *opts, fsimg_image_t **out,
    fsimg_error_t *error);
fsimg_status_t fsimg_image_get_info(fsimg_image_t *image,
    fsimg_image_info_t *out, fsimg_error_t *error);
fsimg_status_t fsimg_image_next_partition(fsimg_image_t *image,
    fsimg_partition_info_t *out, fsimg_error_t *error);
fsimg_status_t fsimg_image_select(fsimg_image_t *image,
    const fsimg_selector_t *selector, fsimg_io_t **volume,
    fsimg_error_t *error);
fsimg_status_t fsimg_image_verify(fsimg_image_t *image, unsigned flags,
    fsimg_layer_report_t *out, fsimg_error_t *error);
void fsimg_image_free(fsimg_image_t *image);
```

An image is the opened [stack](design-layers.md) around a volume: the
container peeled to a disk, the scheme's partitions enumerated, nothing of the
filesystem. `fsimg_stack_t` is a public value of three slots, each a name,
an explicit empty or unspecified; `parse()` applies the
[one grammar rule](design-layers.md#the-stack) and fails on any other text,
and `format()` writes the canonical text, omitting empty and unspecified
slots, and returns the length it needs. Image options carry a forced stack
(a named slot is forced, an explicit empty slot is forced empty, an
unspecified slot is detected), the read-side settings such as `gpt:sector`,
accepted only for a layer the forced stack names, and `spool_fd`: the
[spool](design-layers.md#handed-over-files), an exclusively created, empty,
read/write regular file the caller has unlinked, adopted on every outcome
after the same check as `raw_fd` (regular, size zero, link count zero,
read/write, else `FSIMG_EINVAL`), or `-1`. `open()` uses it only when the
input turns out to be compressed and
otherwise closes it; `open_stream()` requires it and reads `stream_fd`, a
pipe or other non-seekable descriptor borrowed for the call, to EOF into it,
deciding compression from the first block. Without a spool, stream and
compressed input fail as a spool limit error with limit zero. The options'
`compression`, the library's `-z`, forces a registered compression by
name, resolved as the write options' field is, and empty detects one: only
the forced compression's probe runs at step 1, so an input it does not
claim fails with `FSIMG_EFORMAT` naming it, and an ambiguous input reads as
compressed. No image option flags are defined; `flags` must be zero.

The image information record exposes the canonical detected stack text, the
compression name, the length of the file the layers were peeled from (the
input, or the spool holding a stream or a decoded gzip; a plan reports its
planned disk), disk length, the container's name, chunk count, the
encodings its chunks use and its identifier (the DMG segment ID or the VHD
unique ID), the scheme's sector size, disk identifier and partition count,
and the layer validation scope. `next_partition()` iterates the scheme's slots in order
from 1, returning DONE at the end: slot number, used flag, disk offset and
length, type as table name and raw value, name as UTF-8 (GPT), unique
identifier, active flag (MBR), and the bounded probe's claim for the
partition's prefix (format name, unsupported, corrupt, ambiguous when
several formats claim it, or none). `get_info()` takes a mutable image
because the scheme step runs on the first operation that needs it. A disk
without a scheme has no partitions: the information record reports no
scheme and a partition count of zero, `next_partition()` returns DONE at
once, and `select()` accepts only the automatic, range and disk selectors
for it, the automatic one yielding the whole disk as the volume.

`select()` applies one selector (automatic, index, name, type, range or
disk) under the [selection rules](design-layers.md#partition-selection) and
returns a checked read-only slice of the disk. The slice borrows the image,
which must outlive it, and is released with `fsimg_io_free()`; it is the
input to `fsimg_reader_open()`, which stays unaware of layers. `verify()`
checks every layer with the [layer verification scope](design-layers.md#verification-scope);
`FSIMG_VERIFY_PAYLOAD` adds the read-through checks. Its report names the
container and the scheme it checked and the partitions whose bounds it
checked, with payload scope when read-through decoded the container's
chunks, and the compression whose member checks passed at open. It does not require an untouched image and does not consume
anything.

```c
fsimg_status_t fsimg_image_wrap(fsimg_ctx_t *ctx, fsimg_io_t *input,
    const char *stack, const fsimg_write_options_t *opts,
    const fsimg_sink_t *sink, fsimg_error_t *error);
fsimg_status_t fsimg_image_unwrap(fsimg_image_t *image,
    const fsimg_selector_t *selector, const fsimg_write_options_t *opts,
    const fsimg_sink_t *sink, fsimg_error_t *error);
fsimg_status_t fsimg_image_compose(fsimg_ctx_t *ctx, const char *stack,
    const fsimg_part_t *parts, size_t count,
    const fsimg_write_options_t *opts, const fsimg_sink_t *sink,
    fsimg_error_t *error);
```

`wrap()` puts a raw volume or disk into a stack; `unwrap()` writes the bytes
a selector names, raw; `compose()` builds a partitioned disk from `count`
parts, each a borrowed volume I/O object with a partition name and type
(NULL selects the derived default) and the partition's unique GUID, which
follows the [identifier rule](design-layers.md#identifiers): required
outside reproducible mode, `FSIMG_EINVAL` naming the partition when unset,
derived from the partition index under it. Each follows the
[wrap, unwrap and compose rules](design-layers.md#wrap-unwrap-and-compose).
They take write options for the mode flags, epoch and `layers`; filesystem
settings must hold their defaults. Inputs are borrowed for the call and
checked against the sink for backing-identity aliases before any mutation.
Outputs follow the same publication, stream and failure contracts as
`fsimg_writer_output()`.

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
A format without full verification advertises no verify capability, and
`fsimg_verify` on it fails with `FSIMG_EUNSUPPORTED` before any work, leaving
the reader untouched.
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

```c
fsimg_status_t fsimg_layer_next(size_t *cursor, fsimg_layer_info_t *out,
    fsimg_error_t *error);
```

`fsimg_layer_next()` enumerates schemes, containers and compressions the same
way: name, kind, reader/writer flags, the settings the layer stores, the file
suffixes that name it, its codecs and a bounded codec version string, so a
reproducibility claim for compressed bytes can name the zlib it depends on.

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
| Input I/O passed to image open, wrap or compose | Caller, borrowed by the image or for the call | Image destruction or end of the call |
| Spool descriptor in image options | Adopted by the open on every outcome | Image destruction |
| Raw intermediate descriptor in a sink | Adopted by the output call on every outcome | End of the call |
| Volume slice returned by select | Caller, borrows the image | Slice destruction; image outlives it |
| Sink value and its descriptor or I/O object | Caller, borrowed for the call | End of the output call; a staged file is library-owned until published or removed |
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

`fsimg_options_t` uses `uint64_t` byte/count limits, including `spool_bytes`,
`uint32_t` depth/path/FD limits, separate callback arguments, and zeroed
reserved words. Its `flags` field accepts zero. `FSIMG_RESOURCE_SPOOL`
identifies the spool limit in errors, and the statistics report the live
spool length, its peak and the storage the spool file allocated (its
`st_blocks`, sampled when the spool is sealed), and `FSIMG_OP_IMAGE` identifies a
failure in an image's scheme, container or compression layer, or in wrap,
unwrap or compose; a layer's own failure, on open, verification or a read
through its chunks, carries the layer's registry name in `error.path`, so
a caller such as `cW` attributes it without parsing the message. Progress snapshots borrow a phase string for the duration
of the synchronous callback. Destructors called from a callback do nothing;
fallible reentry returns `FSIMG_ESTATE`. This protects the active context without
introducing deferred destruction or callback-driven lifecycle transitions.

`fsimg_io_ops_init()` initializes the callback table. Its `max_transfer` limits
each callback request; zero selects the host transfer maximum. Read/write
capabilities follow callback presence. `FSIMG_IO_DURABLE` requires a flush
callback and is the only table flag. Built-in regular-file FDs advertise durable
flush. `next_zero_range` is optional and spares reads only; output bytes never
depend on it.

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
