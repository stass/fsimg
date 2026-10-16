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

Automatic sizing chooses the smallest valid volume among the documented
geometry candidates satisfying the requested free reserve. It does not claim
to optimize over unsupported geometries. An entry that cannot fit or be named
is identified in the error.

FAT aliases require special care for huge directories. Reserve legal native
short names in a sorted stream. Assign long-name aliases from a deterministic
numeric namespace, merging against reserved names and skipping conflicts in
order. The namespace and exhaustion behavior are defined in the FAT driver;
an implementation cannot use repeated whole-directory scans or probabilistic
hash uniqueness. Assignment order depends on canonical names, not collection
order or a hash seed.

## Emission

Before mutating the sink, compare its backing identity against the complete
host-source identity stream. Reject the same inode even if two slices appear
disjoint, because writer finalization may change whole-file length. Opaque
callback storage must be disjoint by caller contract. Different handles or
pathnames do not establish independence. The check precedes resize and zeroing
as well as payload writes; source revalidation after damage is insufficient.

The emitter iterates the plan in physical output order where practical.
Metadata encoders generate bounded windows from plan records. FAT contents
are generated from allocated cluster runs; FAT copies are regenerated or
streamed without retaining a complete FAT. exFAT bitmaps are generated from
ordered allocation runs, not a resident bit per volume cluster.

Payload copying uses a bounded buffer, with 1 MiB as the ordinary transfer
target when the budget allows it. Adjacent ranges are coalesced up to that
buffer limit. Each source is revalidated around its reads. The copy loop never
loads a complete file, and zero sources and known source holes use the output
zero-range capability.

Unused clusters, alignment gaps, reserved bytes, and allocation tails are
defined zeros. A newly created sparse regular image represents such ranges
without writing every zero byte. Metadata required by the filesystem must
still be emitted; sparse capacity does not eliminate the cost of an on-disk
FAT or bitmap. Sparse host allocation is an optimization and is not promised
for every output filesystem.

Before emission, a fixed-size sink or slice must exactly match the planned
logical length. Only a resizable whole-object sink can change its own length;
an output slice never resizes its backing object. After emission, validate the
logical length and invoke the adapter's flush when present. An adapter without
flush promises completed writes are already visible under its own semantics;
that is logical completion, not persistent durability. A required durable mode
needs an explicit durable-flush capability and fails before writing if absent.

An optional internal read-back check has a named scope and does not replace
independent validation. Emission is single-use even after an error; retry uses
a new writer and output. A caller-provided sink is partial and invalid on any
failure. The file-publication helper additionally owns file/directory durability.

## Reading and traversal

Opening a volume reads bounded probe/header regions and validates geometry
against the actual slice length. It does not scan all entries, read free
payload space, or materialize allocation metadata. Ambiguous recognition
requires an explicit format choice; forcing a format never disables checks.

Before exposing any child or resolving a selection within a directory, perform
a bounded directory prepass. Decode its entry records into a spillable stream
in on-disk order. Emit each effective lookup key with its entry ordinal into a
second stream, externally sort by key, and reject a key shared by distinct
entries. FAT keys include long names and short aliases, with the specified
case-equivalence rules. Multiple equivalent keys for the same entry are handled
according to the format; they cannot hide a collision with another entry.
This detects duplicate files/directories and alias collisions independently of
their allocation addresses. A selection never chooses the first ambiguous name.

Iteration then consumes the validated record stream in on-disk order and uses
iterative depth-first traversal, not C recursion proportional to image depth.
Each ancestor cursor retains only bounded state and references to its spillable
directory stream. Release a directory's scratch once its traversal finishes.
Only the current entry snapshot, bounded cursor state, and shared cache pages
are resident. Count an entry against the session quota when admitted by the
prepass, not again when returning the cached record. The first `next()` can scan
a whole wide directory, but `open()` remains bounded and no whole-volume tree
is materialized. No buffer is sized directly from an untrusted directory length.

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

Sequential `read_at` calls retain the chain cursor and coalesce adjacent
clusters. They do not restart from the first cluster for every buffer. A
nonsequential access that needs more than bounded checkpoints builds a
current-file extent index in scratch with one forward chain pass. Fixed-size
extent records support paged binary search by logical offset. Its memory is
bounded and scratch use is proportional to actual fragments. The index is
discarded when iteration advances. Insufficient indexing resources produce a
limit error rather than a hidden quadratic fallback.

Logical content ranges distinguish data from guaranteed zeros. exFAT bytes
beyond initialized content are zero without reading their underlying clusters.
Host holes beneath a data range can also be copied as zeros when the adapter
supports reliable sparse discovery. Ordinary FAT allocation does not by itself
identify holes or authorize skipping data reads.

## Verification scope

Normal reads validate everything needed for the metadata and content they
expose. They do not certify unreachable allocations or all other files.
`info` reports that limited scope rather than claiming a clean filesystem.

Full verification walks the directory graph and file allocations, emits
ownership records for cluster runs, externally sorts those records by physical
range, and detects forbidden overlap. It streams allocation tables/bitmaps to
compare claimed and allocated storage and report orphaned allocations and
inconsistent copies. It need not read free data clusters or every payload byte
to verify structural consistency. Payload read-through is a distinct option.
An unsupported critical feature prevents a complete verification claim.

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
stream. The prepass runs once per directory in a session, not once per child
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
