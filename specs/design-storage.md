# Bounded storage and image algorithms

## Resource model

The [architecture](architecture.md#bounded-resources) owns budget defaults and
accounting. This document defines how algorithms stay within those budgets.
Payload size, entry count, directory width, allocation-unit count, and fragment
count are separate scaling dimensions. Tests vary each independently.

The resource context supplies charged allocation, a shared bounded page cache,
scratch creation, descriptor accounting, and cancellation. A caller cannot
evade the total by opening several readers or writers in one context. Dirty
page eviction checks writes before reusing memory. A failed allocation does
not lose the old object or alter a published count.

The implementation does not map entire images, allocation tables, catalogs, or
scratch files. A huge mapping is not an acceptable replacement for a huge
allocation: its resident pages, address-space consumption, and failure modes
still need control. Ordinary positional I/O with fixed-size buffers is the
baseline. All library-controlled variable-size buffers are charged, including
path construction, merge heaps, arrays of sort runs, and error aggregation.

## Byte I/O

An I/O object exposes a logical length and read/write capabilities. A slice
borrows its parent and defines a checked offset and length. Zero-length access
at the end is valid; nonzero access outside the slice fails before invoking
the adapter. The adapter has no authority to widen a slice for a driver. A slice
has a fixed logical length, retains backing identity and translated ranges for
alias checks, and never resizes or truncates its parent.

Writable FD adapters reject `O_APPEND`: Linux can append despite a positional
write's supplied offset, as documented in
[pread/pwrite](https://man7.org/linux/man-pages/man2/pread.2.html). They also
reject unsupported direct-I/O modes. Do not clear shared file-status flags to
make an adapter fit; callers must keep those flags stable for its lifetime.

Exact reads and complete writes loop over partial progress, retry permitted
interruptions, and stop on EOF, zero progress, cancellation, or terminal error.
Requests are split to fit `size_t`, `ssize_t`, the adapter's maximum transfer,
and the configured buffer allowance. Every conversion to `off_t` is checked.
Nonblocking callbacks return a terminal unsupported/I/O result; the core does
not busy-wait for readiness.
After 1024 consecutive interruptions without progress, the next interruption
is returned as an I/O error retaining EINTR. Cancellation is polled between
retries, including flush retries, so interruption cannot create an endless
uncancellable loop.

Capabilities distinguish positional access, resize, flush, known-zero ranges,
and optional host sparse-range discovery. Only an adapter that owns a fresh,
exclusively created file can advertise its unwritten ranges as known zero.
A callback's zero-range operation promises logical zeros, not just a seek.
For a generic reused sink, the emitter explicitly writes zeros or invokes a
verified zero-range operation for every gap. Extending a file does not prove
that bytes already inside its old length are zero.

Sparse source discovery is an optimization. A reported hole supplies zeros;
the absence of hole support causes ordinary bounded copying. A host-file hole
does not mean that a filesystem allocation unit is free. Reflinks and
`copy_file_range` are not required and cannot bypass source validation or slice
bounds. Metadata and content copies remain correct without them.

## Metadata scratch primitives

Scratch files are private, disposable, versioned internal records. They are
not a persistence format, interchange format, or user database. The context
creates them beneath an explicitly supplied directory capability, using
exclusive creation, restrictive permissions, and no symlink following. Files
are unlinked while open where practical, and otherwise removed by their owner.
No raw process pointer, uninitialized padding, or file payload is serialized.

Only three primitives are needed:

1. **Record streams.** Length-checked records with fixed-width IDs and offsets
   refer to bounded UTF-8 strings and source-path records. Streams append and
   iterate sequentially; a record larger than the per-record/path allowance
   fails before allocation.
2. **Paged arrays and bitsets.** Dense numeric IDs address fixed-size records
   through the shared cache. Sparse bitsets mark visited directory clusters.
   Their logical length is derived from validated geometry and charged to the
   scratch budget even if the host stores few physical blocks.
3. **External sort and merge.** A bounded run buffer produces sorted runs;
   bounded fan-in merge passes produce one ordered stream. Run descriptors are
   themselves stored in streams. Fan-in obeys memory and descriptor budgets,
   and inputs are released only after replacement output is complete.

These primitives have in-memory representations for small inputs under the
same semantics and budget. Spill is automatic when scratch is enabled.
Changing representation does not change IDs, comparison rules, or image bytes.
There is no WAL, recovery, concurrent update protocol, or reusable persistent
index. A scratch failure aborts the operation; scratch is rebuilt from inputs.

Before extending a scratch file, reserve its additional logical bytes. A merge
reserves space for live inputs and output together, not only the eventual
result. Track peak logical and physical scratch use separately. Sparse files
must not bypass the quota. Scratch exhaustion is `FSIMG_ELIMIT`; host ENOSPC is
`FSIMG_EIO` with the OS error. Both leave publication unperformed.

Scratch can reveal names and source metadata. Do not log scratch contents or
promise secure erasure after unlinking. Callers handling sensitive data choose
an appropriate scratch filesystem. A RAM-backed temporary filesystem consumes
system memory despite not appearing as fsimg heap; the CLI documents that a
disk-backed scratch directory is required for its large-catalog deployment
profile.

### Shared metadata representation

Metadata objects use one context-owned cache of 4 KiB pages. Cache storage and
page descriptors are allocated together and charged once; objects do not own
independent caches. The cache uses bounded least-recently-used replacement.
Its default capacity is 256 pages, and private validation controls can select
smaller capacities before any metadata object exists. Releasing the last object
releases the cache. No page pointer escapes an operation, so eviction needs no
caller-managed pinning protocol.

A metadata byte store starts resident. Missing pages contain zeros. Evicting a
modified resident page creates the store's backing file and reserves its entire
logical length, including the internal version header, before extending it.
Unmodified zero pages need no backing write. The sum of resident stores' logical
page counts fits the cache capacity, including untouched zero pages. Growth
beyond that aggregate capacity acquires backing storage immediately, so many
untouched sparse arrays cannot bypass either budget. Without scratch authority,
exceeding resident capacity returns a memory limit error.
Growth and writes can leave partial private metadata on failure; that object
must be discarded. Failed eviction retains the dirty page and never reuses it.
Backing creation is prepared before it is attached to a resident store; failure
releases its temporary descriptor and scratch reservations without invalidating
the resident contents.

Each metadata object borrows its containing reader/writer session. That session
outlives its metadata and owns cumulative work accounting across growth, page
access, and sort comparisons. Sorting borrows the input's session for every
temporary and result. Comparison work is reserved before invoking the callback,
and quota/cancellation errors propagate immediately through heap operations.

Scratch creation uses descriptor-relative exclusive, no-follow opens with mode
0600 and immediate unlinking. Names contain a version and a checked context
sequence, never addresses or source names. Bounded collision retries prevent an
untrusted directory from causing an endless creation loop. The supplied scratch
directory must exclude untrusted writers; file permissions and unlinking do not
protect against a hostile owner of that directory. Descriptor reservation
precedes creation, and adoption does not duplicate the resulting descriptor.
Logical scratch reservations include cached dirty bytes. Allocated filesystem
blocks are sampled after scratch writes and extension and reported separately
from logical lengths; neither value describes secure erasure or device usage.

Record streams encode a version and a 32-bit payload length in an eight-byte
little-endian frame. A stream declares its maximum record length at creation;
iteration validates the frame and remaining range before copying into a
caller-provided bounded buffer. Empty records are valid. Byte offsets are stable
record references. Sealing prohibits further appends. Fixed-record arrays use
checked 64-bit multiplication and byte ranges, and bitsets use checked byte
lengths rather than allocating a resident bit for every possible ID. Callers
encode fields explicitly; native structs and their padding are not records.

### Sorting and run ownership

Sorting consumes a sealed stream and prepares a separate sealed result. It
preserves input order among equal keys, including across run sizes and merge
fan-in. Comparators are synchronous trusted callbacks subject to the context's
non-reentrancy rule. Run generation holds a charged byte arena and a bounded
array of record references. In-place heapsort orders references by key and
original ordinal, leaving payload storage in the arena.

Runs are concatenated in one byte store. Each run has a fixed-width header
containing its byte length and record count; the next header is found by checked
range arithmetic. This embedded run registry requires no per-run RAM or open
descriptor. A merge group holds only a bounded heap of current records and
positional cursors into the shared input store. Equal keys favor the earlier
run. Each pass writes a replacement store, and releases the input pass only
after replacement is complete. There are at most three live data stores during
sorting, including the original stream, independent of the number of runs.
The resource owner charges all three, headers, cache pages, buffers, and
descriptors. Failure discards temporary stores and leaves the original stream
available; no incomplete replacement is published.

## Collection and catalog validation

A writer accepts entries in any order and records them without retaining an
object or open file descriptor per entry. Host-tree scanning stores relative
source paths and identity records under a bounded number of opened source
roots. Source payload descriptors are opened on demand and promptly closed.
Programmatic content uses one registered provider and opaque numeric source
IDs, not one callback closure or retained descriptor per file.

The image root is implicit. Every other entry has an explicit parent directory
entry; missing parents, duplicate paths, file/directory conflicts, or alternate
spellings of the same format-equivalent parent are errors. The host scanner
emits directories as well as files. There is no order-dependent merging of
metadata and no silent synthesis of arbitrary parent attributes.

Catalog preparation uses these passes:

1. Validate each input record and its bounded component sequence. Record raw
   source identity separately from normalized image names.
2. External-sort image paths by tuples of components, using a specified byte
   order and format comparison keys. Component-tuple ordering keeps a parent's
   descendants together; raw string ordering is not assumed to do so.
3. Assign stable entry IDs in that order. A bounded ancestor stack resolves
   parents, validates type relationships, and produces parent IDs. Depth is
   checked before pushing; a very wide directory never becomes a RAM vector.
4. Sort child records by parent ID and the format's name/collision key. Detect
   collisions by adjacent comparison and produce names and directory record
   counts. Re-sort by stable ID when needed; do not build a global hash table.
5. Store fixed-size results in a paged array keyed by entry ID, with strings in
   immutable streams. Check all references even though records are internal.

No phase lists all children of a directory in memory. The scanner itself uses
bounded traversal state and descriptor accounting; a depth/descriptor limit
failure is explicit. Scratch or output objects encountered beneath a source
root cause an error rather than being recursively imported or silently ignored
by filename patterns. Output image staging begins after collection/planning.

## Planning

Planning seals the catalog. A writer cannot add entries after planning begins,
including after failure. It exposes a borrowed, immutable plan backed by the
writer's context and scratch storage. The caller can inspect totals without
materializing the plan.

The planner performs separate operations:

* Determine format-specific encoded names, aliases, directory slots, required
  metadata, and rounded content allocation lengths.
* Select a geometry from the format's supported sector and allocation-unit
  candidates. Explicit volume size is a constraint, not a hint. Free-space
  reserve is a minimum after metadata and allocation rounding.
* Solve coupled allocation-table and cluster-count equations with bounded,
  checked searches. Each search has a documented monotonic predicate and an
  iteration bound derived from the integer width. An infeasible geometry is
  an error, never an unbounded convergence loop.
* Allocate mandatory format structures, directories, and file ranges in stable
  order. The FAT/exFAT writers use contiguous content allocations where legal;
  they do not implement a free-space allocator for arbitrary edits.
* Produce ordered emission records and aggregate plan statistics. Check that
  required regions fit, do not overlap illegally, and cover the logical output
  with data or known zeros.

Automatic sizing chooses the smallest valid volume for the first legal
geometry candidate satisfying the requested free reserve, or under the
minimization option the smallest volume over every open candidate, as the
[sizing design](design-sizing.md) states. It does not claim to optimize over
unsupported geometries. An entry that cannot fit or be named is identified in
the error.

FAT aliases require special care for huge directories. Reserve legal native
short names in a sorted stream. Assign long-name aliases from a deterministic
numeric namespace, merging against reserved names and skipping conflicts in
order. The namespace and exhaustion behavior are defined in the FAT driver;
an implementation cannot use repeated whole-directory scans or probabilistic
hash uniqueness. Assignment order depends on canonical names, not collection
order or a hash seed.

## Emission

The [emission design](design-emission.md) owns the order of pre-mutation
checks, sink initialization, payload copying, and failure marking. This section
states its bounds.

The emitter iterates the plan in physical output order where practical.
Metadata encoders generate bounded windows from plan records. FAT contents
are generated from allocated cluster runs; FAT copies are regenerated or
streamed without retaining a complete FAT. exFAT bitmaps are generated from
ordered allocation runs, not a resident bit per volume cluster.

Payload copying uses a bounded buffer, with 1 MiB as the ordinary transfer
target when the budget allows it. Adjacent ranges are coalesced up to that
buffer limit. The copy never loads a complete file, and scratch never holds
payload bytes.

Unused clusters, alignment gaps, reserved bytes, and allocation tails are
defined zeros. A newly created sparse regular image represents such ranges
without writing every zero byte. Metadata required by the filesystem must
still be emitted; sparse capacity does not eliminate the cost of an on-disk
FAT or bitmap. Sparse host allocation is an optimization and is not promised
for every output filesystem.

A fixed-size sink or slice must exactly match the planned logical length. Only
a resizable whole-object sink can change its own length; an output slice never
resizes its backing object. An adapter without flush promises completed writes
are already visible under its own semantics; that is logical completion, not
persistent durability. A required durable mode needs an explicit durable-flush
capability and fails before writing if absent. An optional internal read-back
check has a named scope and does not replace independent validation.

## Reading and traversal

Opening a volume reads bounded probe/header regions and validates geometry
against the actual slice length. It does not scan all entries, read free
payload space, or materialize allocation metadata. Detection follows the
[bounded probe contract](design-formats.md#format-descriptors-and-profiles):
ambiguous recognition requires an explicit format choice, and forcing a format
never disables checks.

The [prepared-directory design](design-directories.md) owns preparation,
namespace validation, and traversal. Its bounds: each directory is prepared once
per session, with one external sort of its key stream. Traversal is iterative,
not C recursion proportional to image depth. Only the current entry snapshot,
bounded per-ancestor cursor state, and shared cache pages are resident. The
first `next()` can scan a whole wide directory, but `open()` remains bounded
and no whole-volume tree is materialized. No buffer is sized directly from an
untrusted directory length.

For FAT/exFAT, a paged exact cluster bitset records directory clusters when
first entered. Re-entering an already claimed directory cluster from a new
chain is corruption, including shared-directory chains and ancestor cycles.
Resuming the same cursor does not claim its current cluster again. Special
`.` and `..` records are validated and excluded from recursion. This requires
no in-memory visited set proportional to the directory count. A future driver
with a different object identity must supply an equally bounded exact scheme;
a Bloom filter cannot determine correctness.

Every allocation-chain walk is bounded by validated geometry and the file's
required allocation. A constant-space cycle detector rejects repeated links;
step counts prevent an endlessly changing chain from running forever. Cache
misses and callback calls consume work counters. Cancellation is checked at
each buffer/metadata batch, including sorting and verification. No retry loop
runs without measurable progress or a finite retry allowance.

The [logical content design](design-content.md) owns delivery and the optional
extent index. Its bounds: sequential reads advance a file's allocation once and
coalesce adjacent clusters, with no restart per buffer. A random-access index
costs one forward pass, bounded memory, and scratch proportional to actual
fragments. Insufficient indexing resources produce a limit error rather than a
hidden quadratic fallback.

## Verification scope

Normal reads validate everything needed for the metadata and content they
expose. They do not certify unreachable allocations or all other files.
`info` reports that limited scope rather than claiming a clean filesystem.

Full verification walks the directory graph and file allocations, emits
ownership records for allocation runs, externally sorts those records by
physical range, and detects forbidden overlap. It streams allocation tables or
bitmaps to compare claimed and allocated storage and report orphaned allocations
and inconsistent copies. It need not read free data clusters or every payload byte
to verify structural consistency. Payload read-through is a distinct option.
An unsupported critical feature prevents a complete verification claim.

Exclusivity has a narrow, driver-defined scope:

> Within a driver-defined exclusive ownership domain, bounded claims from
> distinct owners cannot overlap.

The driver defines the domain, identifies owners, accounts for repeated
references, and supplies every required claim; a list of extents alone does not
establish complete ownership. FAT and exFAT use one domain, the cluster heap,
with one owner per directory or file allocation. The QNX6 design distinguishes
snapshots, objects, logical ranges, and roles, and permits cross-snapshot
sharing. A common interval utility can own record encoding, bounds checks,
external sorting, and the sequential overlap cursor. Reconciliation against
allocation tables or bitmaps, reserved structures, and legal sharing stay with
the driver.

Cross-linked regular files cannot amplify default extraction beyond the
logical-output budget. Detection of a cross-link during full verification is
corruption; normal selected-file reads do not claim global cross-link absence.
No repair is performed. A failure partway through verification reports
incomplete verification, not a partially clean result.

## Complexity and measurable obligations

Let `K` be catalog bytes, `B` the record/block transfer size, and `M` the memory
budget. An external sort uses bounded fan-in passes, approximately
`O((K/B) log_(M/B)(K/B))` block transfers when external storage is needed.
Planning has a fixed documented number of sorts and sequential passes; it does
not repeat an image-wide scan per entry. Depth-dependent work is bounded by
the configured depth, and benchmarks report it separately.

Sequential content reading is `O(P + referenced allocation metadata)` in bytes
processed, with fragmentation reflected in I/O calls. Directory traversal adds
decoding and the external-sort cost of each visited directory's lookup-key
stream. Preparation runs once per directory in a session, not once per child
or selection comparison. Full verification additionally scans allocation
metadata and sorts ownership records. Emission is
`O(P + emitted metadata + explicitly written zero bytes)`.
It is not necessarily sublinear in allocation-unit count: the format may require
an allocation table entry for each unit. That cost is streamed, not resident.

Tests assert operation counts as well as elapsed time: no chain restart per
buffer, no quadratic alias generation, bounded merge fan-in, bounded run
registry, and no I/O through free payload space during `info` or `ls`. Resource
tests force spills with small budgets and compare results against large-budget
runs. They cover wide directories, deep trees, sparse capacity, many fragments,
scratch ENOSPC, descriptor exhaustion, and arithmetic near supported limits.
