# Scalar byte access and zlib memory

## Purpose and ownership

Two small facilities remove repeated mechanisms without sharing format policy:
explicit unsigned scalar byte access, and an adapter from zlib allocation
callbacks to the resource context. They are independent modules, not a general
utility framework. The [architecture contracts](architecture.md#contracts)
own byte boundaries and resource accounting; this document specifies the
private interfaces that implement those contracts.

`lib/core/fsimg-endian.h` owns scalar loads and stores. It includes only the standard
integer and limits headers, has no dependency on `internal.h`, and exposes
nothing in installed `fsimg.h`. The library and CLI use the same private
header. Its project-prefixed filename avoids shadowing system `<endian.h>`
when the private core directory appears on the compiler's include path. Its
include guard is `FSIMG_ENDIAN_H`; it defines no host-endianness macros and
does not reuse system header guards. `lib/core/zlib-memory.h` and
`lib/core/zlib-memory.c` own the allocation
adapter; only zlib users include its header. Neither zlib types nor callbacks
belong in the broad `internal.h` or `layer.h` interface.

The [layer design](design-layers.md) owns framing, decompression limits,
compression settings and stream completion. The [storage design](design-storage.md)
owns scratch records; each record's owner specifies its layout and validates
its meaning. The [CLI design](design-cli.md) owns worker framing, signed wire
values and protocol validation. None of those responsibilities moves here.

## Unsigned scalar interface

The following private functions are `static inline` definitions in
`fsimg-endian.h`. Their names state the encoded order, not the host's order:

```c
uint16_t fsimg_load_le16(const unsigned char *);
uint32_t fsimg_load_le32(const unsigned char *);
uint64_t fsimg_load_le64(const unsigned char *);
uint16_t fsimg_load_be16(const unsigned char *);
uint32_t fsimg_load_be32(const unsigned char *);
uint64_t fsimg_load_be64(const unsigned char *);

void fsimg_store_le16(unsigned char *, uint16_t);
void fsimg_store_le32(unsigned char *, uint32_t);
void fsimg_store_le64(unsigned char *, uint64_t);
void fsimg_store_be16(unsigned char *, uint16_t);
void fsimg_store_be32(unsigned char *, uint32_t);
void fsimg_store_be64(unsigned char *, uint64_t);
```

These functions access exactly 2, 4 or 8 bytes. A load requires that many
initialized readable bytes; a store requires that many writable bytes. The
pointer is non-NULL and may be unaligned. No function reads surrounding bytes,
retains a pointer, allocates, performs I/O, polls cancellation or reports an
error. Stores write every byte of their scalar, and leave neighboring bytes
alone. No native-struct cast, integer-pointer dereference, packed structure,
host-endian conditional or platform endian API is part of the implementation.

The implementation asserts `CHAR_BIT == 8` and uses exact-width unsigned
types. Every input byte is an `unsigned char`; ordinary `char` signedness
cannot affect decoding. Shifts use an unsigned operand of sufficient width
before the shift, including the high byte of a 32-bit value and the high word
of a 64-bit value. The 16-bit implementation widens to `uint32_t` for shifts
and narrows the completed result. Shift counts are fixed and strictly less
than the operand width. Stores shift unsigned values and explicitly narrow
each selected byte. Expressions do not shift a signed promoted byte into
its sign bit. Loops, where clearer, have a fixed two-, four- or eight-byte
extent; there is no runtime-width decoder or macro evaluating arguments
multiple times.

An argument's C type is the scalar's width. Callers check representability
before narrowing a wider field to a store parameter. The helper does not
silently impose a format maximum, interpret a sentinel, check a flag mask,
or reject a perfectly representable bit pattern.

### Bounds belong to the record boundary

These are unchecked primitives with explicit preconditions, not parsers.
Checking the complete fixed record once establishes the spans of its fields.
A variable record checks its fixed prefix first and each variable span before
constructing a pointer into that span. For an offset `o`, width `w` and
available byte count `n`, the prerequisite is `o <= n && w <= n - o`.
Validation precedes `bytes + o`, not merely the load through that pointer.
Checked arithmetic and `size_t` conversion precede a calculated offset.

Format probes retain their short-prefix checks. Exact I/O must succeed before
decoding a buffer; short I/O is not permission to decode its initialized
prefix as a complete record. Scratch reads retain record-length, version and
semantic checks even though the scratch bytes were produced by fsimg.
Protocol decoders validate the complete frame or fixed prefix before reading
fields and validate field ranges before converting to enums or smaller types.

There is no parallel family of twelve checked scalar wrappers. Such wrappers
would duplicate the record boundary while lacking its error classification,
field name and cross-field invariants. An owner with a genuinely incremental
parser keeps its bounded read operation local and calls these primitives only
on complete contiguous scalars. In particular, a streaming codec that collects
two bytes in separate calls keeps that state; it does not pretend they are
adjacent in memory.

### Signed values and other interpretations

The shared interface is unsigned. The worker protocol interprets a decoded
64-bit bit pattern as its specified signed two's-complement value using:

```c
value <= INT64_MAX ? (int64_t)value :
    -1 - (int64_t)(UINT64_MAX - value)
```

Each cast's operand is representable, including the `INT64_MIN` case. Encoding
a signed value as `uint64_t` uses C's defined modulo conversion. The
protocol-owned `fsimg_wire_get_signed64()` calls `fsimg_load_le64()` and owns
this interpretation; job epoch decoding uses that same protocol operation.
An out-of-range unsigned-to-signed cast or a reinterpretation of a native
signed object is not a portable substitute. Signed metadata-field validation
and narrower signed conversions stay with their owners.

DOS timestamps, exFAT UTC-offset flags, QNX profile rules and ImageFS type
bits remain format/calendar operations. `lib/core/calendar.c` continues to
accept decoded integer values and has no byte-order dependency. FAT12 packed
entries, FAT32 reserved bits, checksums, GUID component ordering, bit streams,
Unicode and variable-length integers are not scalar codecs. They may use a
scalar primitive for a complete underlying field, but their interpretation
and validation remain explicit.

### Call-site ownership map

The target map is exhaustive for the named helper families, not a rule that
every expression containing a shift becomes a shared call:

| Owner and call sites | Shared operation | Responsibility retained locally |
| --- | --- | --- |
| `lib/layers/layer.h`: `fsimg_layer_le16/32/64`, `fsimg_layer_put_le16/32/64`, `fsimg_layer_be32/64`, `fsimg_layer_put_be32/64`, and their layer callers | Direct matching `fsimg_load_*` / `fsimg_store_*`; no layer aliases | Partition/container offsets, checksums, magic and bounds |
| `lib/meta/meta.h`, `records.c`: `fsimg_meta_get_u32/u64`, `fsimg_meta_put_u32/u64`, and scratch callers throughout the library | LE32/LE64 | Frame version, record length, scratch-row interpretation and validation |
| `lib/formats/fat/codec.c`: `fsimg_fat_u16/u32`, `fsimg_fat_put16/put32`, and FAT callers | LE16/LE32 | BPB geometry, FAT entry packing, name and directory rules |
| `lib/formats/exfat/codec.c`: `exfat_u16/u32`, `exfat_put16/put32`; exFAT fields using metadata U64 helpers | LE16/LE32/LE64 | Stream lengths, entry sets, allocation and checksums |
| `lib/formats/qnx4/codec.c`: `qnx4_u16/u32`, `qnx4_put16/put32`, and QNX4 callers | LE16/LE32 | Complete record encoders/decoders, reserved ranges and slot classification |
| `lib/formats/qnx6/codec.c`: `qnx6_u16/u32/u64`, `qnx6_put16/put32/put64` | Small local byte-order dispatch to matching LE/BE functions | Magic establishes volume order; profile, tree and inode semantics |
| `lib/formats/ifs/records.c`: `ifs_u16/u32`, `ifs_put16/put32` | Small local dispatch using `ifs_byte_order_t` | Established order, record kind/length, path and checksum rules |
| `cmd/fsimg/protocol.c`: unsigned `fsimg_wire_get64/put64`, including `job.c` field helpers and other protocol users | Direct LE64 calls; signed helper stays protocol-owned | Wire width, offsets, frame version, field presence, signed meaning and transport |
| `lib/core/sha256.c`: local `be32dec`, `be32enc`, `be64enc` | BE32/BE64 | Vector loops, SHA algorithm and all upstream attribution/license notices |
| `lib/codec/lzo1x.c`: contiguous four-byte hash input | LE32 after the existing four-byte availability proof | Hash computation, streaming operand assembly and match encoding |
| `lib/codec/nrv2b.c` and streaming LZO instruction fields | No forced conversion | Incremental bit/byte parsing, history and termination |

Only runtime-order wrappers remain, because they express a format fact that
the fixed-order primitives do not. QNX6's boolean order and ImageFS's order
enum remain private to those drivers; there is no shared order enum or generic
record visitor. Internal headers include `fsimg-endian.h` where they expose inline
users; implementation files otherwise include it directly. Headers are not
retained solely to obtain an unrelated subsystem's scalar functions.

### Byte compatibility

Image and wire layouts, magic, version numbers, field offsets and reserved
bytes are unchanged. Scratch records keep their exact LE representation,
including the metadata frame's version word and length word. This design
does not make scratch a persistent external format.

Record encoders initialize padding and reserved bytes themselves. A store of
one field cannot initialize a record's other bytes. Output still passes through
bounded I/O and the existing emission/publication owners. Comparison checks
cover full encoded records, not only decoded values: two inverse bugs in
paired helpers must not pass through a self-round-trip.

## Context-backed zlib allocation

The adapter has one fixed-size state value per `z_stream`. It borrows a context
and holds a first allocation error. It owns no stream, allocation list, quota,
cache or compression setting. A stream's owner embeds both values at stable
addresses, or uses stack storage whose lifetime includes every zlib call and
cleanup. The `opaque` pointer addresses the adapter, not a larger gzip, DMG or
chunk object with an unrelated layout.

```c
typedef struct fsimg_zlib_memory {
    fsimg_ctx_t *ctx;
    fsimg_error_t error;
} fsimg_zlib_memory_t;

void fsimg_zlib_memory_bind(fsimg_zlib_memory_t *, fsimg_ctx_t *, z_stream *);
fsimg_status_t fsimg_zlib_memory_error(const fsimg_zlib_memory_t *,
    fsimg_error_t *);
```

The header includes the private context declarations and system `zlib.h`.
The two callback functions are private to `zlib-memory.c`, with zlib's exact
`voidpf (voidpf, uInt, uInt)` and `void (voidpf, voidpf)` signatures. Binding
sets `zalloc`, `zfree` and `opaque`; callers cannot accidentally install only
one callback and fall back to an uncharged system allocation.

`bind` requires a valid borrowed context and a fresh, zero-initialized stream
with no live zlib state. It initializes both adapter fields explicitly and
does not allocate or start compression. It does not zero or alter input/output
fields: the owner initializes the whole fresh stream before binding. A live
stream and its adapter cannot be copied, moved, rebound or individually reused.
The context outlives both and all allocations made through them. The existing
context threading contract applies; the adapter adds no locks or concurrency.
Separate streams use separate adapters even when they borrow the same context.

### Allocation and first-error preservation

For `items` and `size`, the callback checks multiplication in `size_t` before
performing it: if `size != 0 && items > SIZE_MAX / size`, it records
`FSIMG_ERANGE`, `FSIMG_OP_ALLOCATE`, and an allocation-size-overflow diagnostic,
then returns `Z_NULL`. On the supported 64-bit hosts the two `uInt` factors
fit `size_t`; that platform prerequisite is explicit and checked at build
time. The context allocator separately checks its allocation-header addition.
An overflow is not a memory-budget exhaustion and does not fabricate an
`FSIMG_RESOURCE_MEMORY` limit.

For a representable product, the callback calls `fsimg_alloc(ctx, bytes,
&detail)` using a local error record. A successful allocation is returned
unchanged. On failure it copies `detail` to the adapter only if the adapter
has no earlier error, then returns `Z_NULL`. Later allocation attempts after
the first failure return `Z_NULL` immediately and do not change that error.
Freeing an allocation never modifies it. In particular, a successful context
allocation's error clearing cannot erase a preceding failure within the same
zlib call.

Zero factors request a zero-byte allocation through `fsimg_alloc`, rather
than returning NULL unconditionally or inventing a one-byte payload. The
context's allocation header produces a distinct freeable pointer on success
and is charged normally; a zero-byte request can therefore still fail its
budget or allocation injection. No bytes may be accessed through that result.
This is an fsimg adapter convention: zlib's documented callback contract
requires NULL for allocation failure but does not prescribe special success
semantics for a zero-size request or require calloc-style zero filling.

The adapter returns the context allocator's storage without another
initialization pass. `fsimg_alloc` normally zeroes storage; its poison mode
deliberately supplies nonzero contents, which zlib's allocation contract
permits. Tests retain that mode to expose dependencies on unspecified initial
contents. zlib initializes its own private state. Stream buffers, enclosing
object fields and every emitted byte remain their owner's initialization
responsibility; owners expose only bytes actually produced by zlib and
initialize their own framing and padding. There is no secure-erasure claim.
`zfree`
uses `fsimg_dealloc`, including its NULL no-op behavior. Allocation headers
remain private to the context and are never passed to zlib or emitted.

Accounting follows the existing context allocator exactly: payload plus its
project-owned allocation header is charged once before use, and released once
on free. Host allocator bookkeeping remains excluded as specified by the
architecture. There is no zlib-specific budget, reserve, hidden fallback or
allocation counter. Multiple live streams compete for the same context budget.

`fsimg_zlib_memory_error()` requires a non-NULL output error and returns the
stored first status and a complete copy of its diagnostic. If no allocation
failure was recorded, it reports `FSIMG_ENOMEM`, `FSIMG_OP_ALLOCATE`, with a
fixed zlib allocation-failure message. It is used only to interpret
`Z_MEM_ERROR`; it does not consume or clear the stored error. Captured
`FSIMG_ELIMIT` retains its resource, limit and observed values; captured
`FSIMG_ENOMEM` remains allocation failure; other allocator statuses such as
`FSIMG_ERANGE` are preserved. Layer attribution may add the layer identity
without replacing this cause.

### Stream lifetime and error classification

The first error lasts from binding through ending the stream. No successful
allocation, streaming step, reset or end clears it. Allocation failure is
terminal for that stream's fsimg operation: it is not retried on the same
stream after increasing a budget. Fresh use requires ending the stream and
binding a fresh stream/adapter pair. This removes per-call error-clearing
protocols and avoids a stale error escaping into another compression session.

| Point | Owner obligation and adapter behavior |
| --- | --- |
| Before init | Initialize the full stream, bind stable adapter, then call the appropriate system-zlib initializer with the format's parameters. |
| Init succeeds | Mark the owner's existing initialized flag; all subsequent allocations use the bound callbacks, including a delayed inflate window. |
| Init fails | Preserve the returned error before cleanup. Do not set the initialized flag. zlib owns partial internal allocations and unwinds failed initialization; failure-injection tests prove context accounting returns to its pre-init baseline. Do not free opaque zlib internals manually or call End on an uninitialized stream. |
| Streaming | Owner supplies bounded input/output and checks the zlib return before publishing output from that call. The adapter captures allocation failures, including delayed allocations. |
| `inflateReset` / `deflateReset` | Owner invokes reset only on initialized, usable streams; retain callback addresses, context, allocations and error state. Reset is not a new resource session and cannot erase failure. |
| Normal finish | Owner proves framing and compressed-stream completion before declaring success. End releases resources; it does not finish pending compression. |
| Failure/abort | End an initialized stream exactly once while its adapter/context are alive, release owner buffers, and preserve the primary error. Premature deflate End may report discarded data; that must not replace the operation's primary failure. |
| After End | Owner clears its initialized flag. Only now may the enclosing object be released or explicitly reconstructed for a fresh bind. |

The allocation adapter does not translate every zlib return. The format owner
interprets the closed return-code set in the context of the operation:

| zlib result | fsimg interpretation |
| --- | --- |
| `Z_MEM_ERROR` | Use the adapter's first error, or its `FSIMG_ENOMEM` fallback. Never report corruption or infer a limit without evidence. |
| `Z_VERSION_ERROR` during initialization | `FSIMG_ESTATE`, fixed diagnostic identifying incompatible zlib/header ABI; never an image defect or memory failure. |
| `Z_STREAM_ERROR` | `FSIMG_ESTATE`, fixed diagnostic identifying invalid zlib state/parameters. Options are validated before this boundary. |
| `Z_DATA_ERROR` while inflating | `FSIMG_ECORRUPT`, attributed to the compressed input. |
| `Z_NEED_DICT` while inflating supported gzip/DMG data | `FSIMG_ECORRUPT`: these profiles do not permit an external dictionary. |
| `Z_BUF_ERROR`, `Z_OK`, `Z_STREAM_END` | Owner interprets progress and completion using available input/output and the format's expected end. These are not interchangeable success or corruption codes. |
| Other impossible results for the invoked API | `FSIMG_ESTATE` with operation and numeric code; no allocation-error substitution. |

DMG compression can choose an uncompressed chunk when its bounded compressed
buffer does not produce a smaller complete stream; that remains a normal
format decision. It cannot choose that fallback after `Z_MEM_ERROR` or another
fatal zlib error. Inflate's no-progress handling still distinguishes an
exhausted/truncated input from output space that must be replenished. A stale
allocation error never masks a non-memory return code. Diagnostics use bounded
fsimg-owned text, not arbitrary unescaped `stream.msg` bytes.

### zlib call-site map and contract evidence

`chunk_disk_t` in `lib/layers/chunks.c` embeds `fsimg_zlib_memory_t` instead of
a separate `zlib_error`; its zlib-specific allocation callbacks disappear.
The existing `stream_ready` flag and inflate/reset/end control remain there.
`dmg_encoder_t` in `lib/layers/dmg.c` makes the same replacement; raw chunks,
compression selection and encoder cleanup remain DMG-owned. Both
`gzip_decoder_t` and `gzip_encoder_t` in `lib/layers/gzip.c` embed the common
adapter instead of `gzip_memory_t`; gzip's private allocation callbacks
disappear. Header/trailer parsing, concatenated members, spool accounting,
sink errors and initialized flags remain gzip-owned.

The system `zlib.h` is the build's ABI authority. Its stream-usage, init,
inflate/deflate, reset and end contracts establish callback setup, delayed
inflate allocation, reset reuse and cleanup semantics. The corresponding
public reference is the [official zlib manual](https://zlib.net/manual.html).
The reference implementation's
[`deflateInit2_`](https://github.com/madler/zlib/blob/v1.3.1/deflate.c#L412-L478)
allocates several buffers before testing their combined outcome, then releases
partial state on failure. First-error capture therefore must survive further
allocation attempts in one initialization call.
[`inflateInit2_`](https://github.com/madler/zlib/blob/v1.3.1/inflate.c#L168-L204)
also frees its allocated state if reset rejects initialization parameters.
These are cleanup evidence, not permission to depend on internal layouts or
fixed allocation counts.
The adapter uses the installed system library, does not vendor zlib or depend
on its private state layout, and imports no implementation source. Changes in
zlib's allocation count or block sizes do not change the adapter contract.

## Validation and acceptance

Tests verify the mechanism and the boundaries that justify its unchecked
operations. All fixtures and expected byte arrays are independently authored;
a production store is not the oracle for a production load.
The implementation is ordinary handwritten C11, with no source-generation
step. Test enumeration or independently constructed fixture data introduces
neither generated production codecs nor an additional runtime/public API.

| Scalar case | Evidence required |
| --- | --- |
| Known widths/orders | `0x80f1`, `0x80f123a5` and `0x80f123a5b6c7d8e9` decode from literal LE and BE arrays; stores equal those arrays. Include zero, maximum and each single-bit value. |
| Concrete 64-bit vectors | LE `e9 d8 c7 b6 a5 23 f1 80` and BE `80 f1 23 a5 b6 c7 d8 e9` both mean `0x80f123a5b6c7d8e9`. |
| Alignment and extent | Exercise offsets zero through seven in a byte buffer; canaries before/after stores remain unchanged. Sanitizer fixtures place a scalar at the very end of its valid allocation. |
| Signed wire values | Independent bit patterns for zero, `INT64_MAX`, `INT64_MIN`, and minus one; job epoch and timestamp/offset decoding agree without implementation-defined casts. |
| Boundary failure | For every converted record/probe family, truncate at each required prefix/field boundary; checked owner rejects before a scalar call. Do not deliberately pass short buffers to an unchecked helper and expect an error. |
| Full representation | Compare complete golden image records, scratch frames/rows and worker frames, including zero padding and field offsets. Include QNX6 and ImageFS in both byte orders. |
| Format semantics | Retain reserved-bit, checksum, geometry, FAT12 packing, GUID ordering, Unicode and signed-range tests at their owners. Shared byte access must not weaken their checks. |
| Independent consumers | Existing FAT/exFAT and QNX/ImageFS evidence, protocol fixtures and SHA digest vectors remain independent of the helper implementation. |

The zlib adapter tests call the installed callback through a bound `z_stream`
for direct allocation cases, and exercise actual system-zlib streams for
lifetime cases. They use the context's existing allocation-failure injection
and statistics; there is no production allocation registry for testing.

| Allocation/stream case | Expected result |
| --- | --- |
| `items * size` success, zero factors | Correct charged delta including context header, unchanged allocator-provided contents in normal and poison modes, non-NULL/freeable zero-byte success, baseline restored after free. |
| Unrepresentable product | `Z_NULL`, first `FSIMG_ERANGE`, no charged allocation when the platform's `uInt` range can express such factors. With 64-bit `size_t` and 32-bit `uInt`, neither product overflow nor the small context-header addition is reachable through this callback; report that case inapplicable, and retain direct allocator-header-overflow coverage in the context tests. |
| Budget below required allocation | First `FSIMG_ELIMIT`, exact memory resource/limit/observed diagnostics, no uncharged fallback. |
| Injected allocator failure | First `FSIMG_ENOMEM`, no leaked charge, subsequent callback attempts preserve the same complete error. |
| Failure then another attempted allocation/free | Error remains sticky; no successful callback or free clears it. |
| Every init allocation ordinal | Enumerate until a successful non-injected run for inflate and deflate initialization; every failing run returns the captured class and returns accounting to baseline after owner cleanup. Do not fix the ordinal count to a particular zlib build. |
| Delayed inflate allocation | A fixture and output chunking force a continuing stream needing a window; fail its allocation after successful init and verify End releases all state. |
| Successful reset/reuse | Multiple chunks/members produce correct bytes; callbacks/context remain bound, live memory stays charged, and no work quota resets. |
| Fatal failure followed by owner cleanup and fresh bind | Failed operation stays failed; reconstructed pair starts without the old diagnostic and succeeds when injection is disabled. |
| Two simultaneous streams sharing one context | Combined allocations obey one budget, errors are per-stream, closing one stream does not release another's storage. |
| Memory error without callback evidence | Translation helper reports ENOMEM, not ELIMIT or OK. Non-memory zlib codes never consult the saved allocation error. |
| Version/state/data/progress paths | Test owner translation with deterministic zlib result fixtures or invalid initialization/state parameters; corrupted streams remain ECORRUPT and state/version failures remain ESTATE. |
| Poisoned allocation/output buffers | Byte-identical valid output and initialized trailers/padding; only produced bytes reach the sink. |
| Input, sink, cancellation and limit failures | Correct primary status, exact-once stream/owner cleanup, no successful partial image or spool. |

Compression output equivalence uses the same system-zlib version and settings;
the scalar/allocator extraction does not promise compressed bytes are stable
across different zlib releases. Independently produced valid and malformed
gzip/DMG fixtures establish framing and decoded-byte compatibility. Peak
charged memory and scratch remain governed by the existing layer budgets;
the adapter adds only its fixed embedded value and no per-allocation metadata
beyond the context's own header.

Implementation validation follows [build.md](../docs/build.md): the ordinary
suite and affected independent-tool tests, ASan and UBSan for byte access and
allocation lifetimes, and supported leak/initialization checks. Both signed-
and unsigned-`char` compilation exercise the helper tests. Platform evidence
names macOS, Linux and FreeBSD separately; skipped or unsupported checks are
not passing evidence. A nontrivial defect correction requires a failing
regression before the runtime fix. This specification itself changes no
runtime behavior.

## Complexity balance and exclusions

The scalar facility adds one header, twelve fixed operations and one precise
byte-span precondition. It removes duplicated shift/cast implementations and
cross-subsystem dependencies used only to borrow them. It does not add state,
ownership, allocation, byte-order configuration or a parser abstraction.
Record-boundary proofs remain necessary and local; renaming their helpers
cannot eliminate that responsibility.

The zlib facility adds one small embedded adapter and two private entry points.
It replaces three callback pairs and separate error-capture implementations,
and removes repeated callback setup and per-call clearing rules. Context
ownership, zlib initialized flags, stream progress, framing and cleanup remain
explicit because the adapter cannot determine them. A lifetime-sticky first
error uses the operation's terminal failure rule instead of adding error
epochs, a reset protocol or allocation lists.

There is no universal record codec, schema generator, Relic dependency,
generic compressor/decompressor wrapper, dynamically registered endian
backend, checked-cursor language, new public ABI or additional runtime
dependency. Cryptographic algorithm structure and imported-code attribution
are preserved. A change needing one of those concepts has its own design
justification; scalar and allocation consolidation does not authorize it.
