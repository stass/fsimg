# Verification helpers and exclusive ownership runs

Full verification is a driver operation composed from common bounded mechanisms.
Two mechanisms have useful independent contracts: reading the initialized bytes
of the current object, and ordering exclusive allocation claims for a sequential
reconciliation pass. Neither mechanism decides whether a filesystem is clean.

The [architecture](architecture.md#writer-and-reader-independence) assigns the
cross-subsystem owners. [Logical content](design-content.md) defines delivered
bytes and physical domains; [prepared directories](design-directories.md) defines
the one traversal, including omitted entries; [storage](design-storage.md#verification-scope)
defines verification scope, scratch budgets, and external sorting. Driver
specifications remain authoritative for allocation, identities, references,
reserved regions, and supported features.

## Responsibilities and boundaries

| Owner | Responsibility |
| --- | --- |
| Common verification entry point | Validate flags and untouched-reader state, choose structural or payload scope, enable descent into omitted directories, invalidate the snapshot on failure, mark the report complete only after driver success |
| Driver verifier | Establish a complete verification domain, traverse it, decide which distinct objects need payload reading, supply allocation claims, reconcile allocation and references, validate format-specific checksums and reserved structures |
| Initialized-payload helper in `lib/read/` | Bounded sequential delivery of the current object's initialized bytes and confirmed payload-byte accounting |
| Exclusive-run utility in `lib/meta/` | Encode bounded unit intervals, seal and externally sort them, decode and validate each record, reject overlap, own the stream and its sequential cursor |
| Existing content, I/O, session and metadata primitives | Content-domain confinement, source stability, exact physical reads, cancellation, resource accounting, spill, sort passes and backing-store cleanup |

The helpers have private C interfaces, not public ABI, descriptor capabilities,
or user settings. They own no host paths or unrestricted descriptors. There is
no common filesystem verifier, sharing-policy callback, owner registry, relation
database, or mandatory content index. A driver whose ownership representation
already provides the invariant keeps that representation.

The payload helper removes four copies of the read loop. The run utility removes
FAT/exFAT's duplicate record codecs, comparators, stream lifecycle, bounds checks,
and overlap cursors. It does not merely move their reconciliation loops behind
callbacks: those loops remain explicit driver code and inspect decoded intervals.

## Initialized-payload read-through

### Private interface and borrowing

The operation is declared beside common reader operations in
`lib/read/reader.h` and implemented in `lib/read/`:

```c
fsimg_status_t fsimg_verify_payload(fsimg_reader_t *reader,
    unsigned char *buffer, size_t capacity, fsimg_verify_report_t *report,
    fsimg_error_t *error);
```

The reader, buffer, and report are borrowed for the call. The driver retains one
context-charged buffer for its whole verification operation, normally 64 KiB,
and releases it on every exit. ImageFS reuses its checksum buffer. The helper
allocates no buffer, retains no locator or extent, and never advances the walker.
Buffer capacity is an actual allocated byte count, independent of cluster size;
a nonempty request requires a non-NULL buffer and nonzero capacity. Invalid
private arguments fail before I/O. A driver cannot use capacity zero as a way to
skip requested payload verification.

The reader's current snapshot and `initialized` value are stable for the call.
The helper requires `initialized <= entry.size`. A violation is an internal
contract failure, `FSIMG_ESTATE`; entry decoding and replay reject corrupt image
lengths before this point. It does not require a publicly delivered path or
restrict the entry to a public file kind: verification also reads omitted files
and symbolic-link targets. Directory selection and treatment of other kinds
belong to the driver. A supported FIFO with initialized length zero needs no
payload operation.

### Read and failure algorithm

The helper visits exactly `[0, reader->initialized)` in logical byte offsets:

1. Set the local logical offset to zero. No allocation resolution occurs for
   initialized length zero.
2. Set the batch length to the smaller of the remaining initialized bytes and
   buffer capacity, converting to `size_t` only after bounding it by capacity.
3. Call `fsimg_reader_content()` for that batch. It resolves and checks physical
   extents, clips reads to the content domain, and uses bounded session I/O.
4. Account the returned confirmed byte count before testing the return status.
   On failure, return that status and error immediately. No retry starts again
   at the beginning of the object or the failed batch.
5. On success, the count equals the batch length. Advance by the confirmed
   count; stop exactly at the initialized end.

The content operation already promises exact completion for a bounded batch.
An impossible successful short result is an internal contract failure, not a
new retry protocol; tests of the helper exercise this guard. Adapter short reads
and interrupted reads are handled by bounded I/O, which retains progress and
rejects premature EOF or zero progress. No helper loop can spin on zero bytes.

The zero tail `[initialized, logical)` and allocation slack are never requested,
synthesized, or counted. Thus an exFAT file with 6 KiB initialized, 10 KiB logical,
and 16 KiB allocated contributes 6 KiB of payload reads, while all 16 KiB remain
subject to structural ownership checks. A zero-initialized preallocated file
performs structural work but no payload read.

Bytes confirmed by a failing read are still bytes read. This includes a read
whose post-read source-stability check reports `FSIMG_ESOURCE_CHANGED`. The
report remains incomplete, and the common verification lifecycle invalidates
the entry after the driver unwinds. Helpers do not call terminal reader cleanup
while the driver still borrows its current locator or buffer.

The report's payload-byte accounting follows the report-counter rule below.
It never derives progress from the context's cumulative I/O statistics: those
include metadata, prior operations and other handles, and may be saturated.

### Report counters and saturation

`payload_bytes_read` is an observed-I/O counter. Both the common helper and FAT's
fused loop add each confirmed count with the existing `fsimg_stat_add()`
saturating primitive: the result is exact while representable and remains
`UINT64_MAX` after saturation. Saturation does not cause a new refusal, replace
a read failure, or imply completion. This is the same convention as cumulative
I/O statistics, applied explicitly to this report field; metadata and checksum
reads still do not contribute to it.

The local offset and remaining length never use this report counter. They use
checked range arithmetic and the actual count returned for the current batch,
so saturation cannot cause replay, skipped data, or a nonterminating loop. The
report's logical-byte total retains its exact checked-arithmetic contract, as
do allocation-domain bounds, stream lengths, and live-resource reservations.
The session's work counter is not a proof of byte-count representability: I/O
charges calls rather than bytes. Saturation is an observation-counter convention,
not a replacement for work, image-size or allocation bounds.

### Charging and cancellation

The helper introduces no additional charge per batch. Common delivery and
`fsimg_reader_bytes()` retain their existing source checks; session I/O polls
and charges each adapter call, including short-read retries, and driver
resolution retains its existing metadata/work charges. Sharing the outer loop
must not duplicate these charges or reset the reader's session. Buffer capacity
and adapter transfer caps bound each I/O call; a metadata resolution can perform
its own bounded, charged work before it returns an extent.

An empty object does not need a synthetic payload I/O or work charge. Traversal,
entry admission, and the driver's final stability check remain responsible for
observing cancellation and source changes around that empty operation. Cached
resolutions still pass through common delivery and bounded I/O.

### FAT's fused allocation walk

FAT's `verify_file()` walks each chain once, records every physical run, and,
when payload verification is enabled, immediately reads that run clipped to
the file's logical length. It retains this fused operation. Calling the common
payload helper after that chain walk would resolve the chain again or require
an index solely for verification; neither is acceptable.

For each validated run, FAT checks the logical start against the file length
before subtraction and reads at most
`min(run.length, entry.size - run.logical_offset)` bytes. The chain contract
rejects excess allocation and requires cluster zero for an empty file. Physical
reads remain bounded by the validated data-region run, use
`fsimg_reader_bytes()`, and count confirmed bytes even on failure. The report
counter rule is identical to the common helper's. Structural mode neither
allocates a payload buffer nor reads these bytes. This exception preserves one
owner for chain discovery and creates no generic callback-based read engine.

## Exclusive ownership-run utility

### Meaning of a domain and a claim

One utility instance represents one exclusive allocation domain in one complete
verification operation. Its bounds are a half-open range of allocation-unit
numbers, **not byte offsets**, `[first_unit, end_unit)`. A claim is
`[first, first + count)` in those same units. FAT and exFAT use
`[2, (uint64_t)clusters + 2)`. The utility never multiplies by sector or cluster
size and never translates a unit to an image offset.

A driver converts a validated physical run into units. It checks the byte run
lies in its heap, its start and length are whole allocation units, and arithmetic
is representable before subtracting the heap origin or dividing. FAT uses its
validated chain runs; exFAT uses its checked extent-index walk. A byte range
being inside the volume alone is insufficient to authorize a cluster claim.

Every overlapping pair of claims in this domain is forbidden, including two
identical records or repeated storage within one object's allocation. There is
no equal-owner exception. The driver emits one allocation traversal per object,
so valid repeated references never become duplicate claims. The instance is
itself the scope identifier; records need no domain ID or owner ID. Separate
instances cannot establish exclusivity across their domains, and placing two
potentially conflicting owners in separate instances is not a valid check.

An empty domain (`first_unit == end_unit`) and an empty stream are valid utility
inputs. A format may independently require nonempty geometry. Domain bounds
are supplied as endpoints, so even an exclusive end of `UINT64_MAX` is
representable; a range requiring the mathematical endpoint `2^64` is not.

### Private interface

The private metadata header declares an opaque object and a decoded interval:

```c
typedef struct fsimg_owner_runs fsimg_owner_runs_t;

typedef struct fsimg_owner_run {
    uint64_t first;
    uint64_t end;
} fsimg_owner_run_t;

fsimg_status_t fsimg_owner_runs_new(fsimg_session_t *session,
    uint64_t first_unit, uint64_t end_unit, fsimg_owner_runs_t **out,
    fsimg_error_t *error);
fsimg_status_t fsimg_owner_runs_append(fsimg_owner_runs_t *runs,
    uint64_t first, uint64_t count, fsimg_error_t *error);
fsimg_status_t fsimg_owner_runs_seal(fsimg_owner_runs_t *runs,
    fsimg_error_t *error);
fsimg_status_t fsimg_owner_runs_next(fsimg_owner_runs_t *runs,
    fsimg_owner_run_t *out, fsimg_error_t *error);
void fsimg_owner_runs_free(fsimg_owner_runs_t *runs);
```

`new` initializes `*out` to NULL and borrows the existing reader session until
`free`. It rejects reversed bounds with `FSIMG_EINVAL`. The wrapper owns one
stream, its replacement during sorting, and one cursor. The caller never owns
a second sorted-stream handle. `free(NULL)` is harmless. The driver installs
its owned pointer only after construction succeeds and clears it when released;
driver close accepts partially constructed state and an already cleared pointer.

The logical states are collecting, reading, exhausted, and failed. They describe
one forward lifecycle, not separately configurable flags:

* `append` is valid only while collecting.
* `seal` ends collection, seals the stream and sorts it once; success enters
  reading with the cursor at the beginning. It does not consume the first run.
* `next` is valid while reading or exhausted; `FSIMG_OK` publishes one validated
  interval, `FSIMG_DONE` publishes none and remains exhausted.
* An operational failure requires discarding the object; no partial append,
  sort, or cursor can be retried. Argument/state rejection before work does not
  mutate it. Repeated sealing or appending after sealing is `FSIMG_ESTATE`.

`out` is zeroed before `next` work, so an error or end cannot expose stale data.
A successful seal proves only that sorting completed; it is not certification
that every record is valid or the claim inventory complete.

### Encoding and append validation

Each record is exactly 16 payload bytes in the existing metadata stream frame:

| Payload offset | Width | Encoding | Meaning |
| --- | --- | --- | --- |
| 0 | 8 | Little-endian unsigned 64-bit | First allocation unit |
| 8 | 8 | Little-endian unsigned 64-bit | Positive unit count |

The existing eight-byte version/length frame makes each stored record 24 bytes.
There is no new file header, checksum, native structure, pointer, padding, owner
number, or format tag. The stream is transient private scratch, not a persistent
interchange format. It uses the existing scalar metadata codecs.

Append requires `first >= first_unit`, `first < end_unit`, `count != 0`, and
`count <= end_unit - first`. The subtraction is performed only after validating
`first`. These tests prove the endpoint fits before calculating it. Invalid
claims are `FSIMG_ECORRUPT`; driver geometry validation distinguishes malformed
image values before they reach the utility. Zero-length allocations produce no
claim, rather than a zero-length record. Claims may arrive in any physical order.

Append does not coalesce records. Driver discovery already emits compact runs;
coalescing here could hide a repeated claim or erase a useful boundary. An append
failure leaves a stream that is only safe to destroy, as specified for existing
metadata mutations. No successfully appended claim is silently discarded.

### Seal, external sort and cursor

Sealing calls `fsimg_stream_seal()` and `fsimg_stream_sort()` with the existing
bounded sort defaults and the same session. Sorting is stable by first unit.
Equal starts retain their original ordinal; equal records are never deduplicated.
The comparator tests record lengths before loading any field. If either length
is not 16, it compares lengths without decoding; equal malformed lengths compare
equal. This is deterministic and safe for short records, and makes no assertion
that a malformed record is valid. Comparator callbacks cannot report errors.
The stream layer rejects invalid framing, and cursor decoding rejects wrong
payload lengths. A separate eager validation pass is unnecessary.

Sorting holds the original stream until the replacement is complete. Failure
releases sorter temporaries under the sort contract, leaves no replacement
published, and retains the owned original only for destruction. Success replaces
the stream and frees the original before reconciliation starts. The wrapper
adds no fourth sort store or per-run registry: the existing sorter bounds arena,
merge fan-in, cache pages, descriptors and simultaneous input/output storage.

`next` reads through a local candidate cursor. It checks the returned length is
16 **before** loading either field, applies the append range checks again, and
calculates the checked endpoint. Scratch is untrusted even after successful
append and sort. It rejects a candidate whose first unit is below the previous
accepted end. Because accepted intervals are nonempty and ordered, this one
comparison detects backward order, nesting, partial overlap, exact duplicates,
and a second run inside a long earlier run. It does not need an active interval
set or an all-pairs comparison. Adjacency (`next.first == previous.end`) is legal;
gaps are legal until the driver's allocation scan interprets them.

Only after all checks succeed does the wrapper publish the interval and commit
its cursor and previous endpoint. The first record is distinguished by the
absence of a previous interval, not by treating unit zero as invalid. End is
`FSIMG_DONE`, never a sentinel unit value. Empty sorted streams end immediately;
repeated end calls remain stable. The final interval can end exactly at the
domain end.

The driver keeps a single local current interval and an explicit availability
boolean. For each unit `u` in its sequential allocation scan, it advances while
a current run exists and `u >= current.end`. The unit is claimed exactly when
a run exists and `u >= current.first` (the loop already establishes
`u < current.end`). At the domain end it advances once beyond the last run and
requires `FSIMG_DONE`. This final drain is required to detect a trailing duplicate
or malformed record that the final allocation unit did not cause it to read.
No successful verifier leaves an unread suffix of ownership claims.

### Errors, attribution and failure precedence

| Condition | Result and ownership |
| --- | --- |
| Invalid private pointer, output, or reversed constructor bounds | `FSIMG_EINVAL`, no image verdict |
| Incorrect lifecycle call | `FSIMG_ESTATE`, no image verdict |
| Out-of-domain or empty claim, malformed decoded interval, forbidden overlap | `FSIMG_ECORRUPT`; helper identifies the generic condition and bounded unit range |
| Invalid scratch framing or truncated record | Existing metadata error, normally `FSIMG_ECORRUPT`, propagated without pretending it proves image corruption |
| Store I/O error or unexpected backing-file EOF | `FSIMG_EIO` with existing OS detail |
| Scratch, memory, descriptor or work budget exhausted | Existing `FSIMG_ELIMIT` with resource details; allocation failure remains `FSIMG_ENOMEM` where applicable |
| Cancellation or source change | Existing status and detail propagated; incomplete verification |

Malformed scratch and corrupt image claims can share `FSIMG_ECORRUPT`, under the
[architecture's existing classification](architecture.md#specification-driven-testing).
This utility introduces no new public distinction or assumption that an internal
record cannot be damaged. Diagnostic wording identifies scratch decoding when
that is what failed. Drivers retain their format-specific allocation errors;
common mechanics use operation `FSIMG_OP_VERIFY` for semantic claim failures and
preserve lower-level metadata/I/O attribution for those failures. Unit numbers
are not written into a byte-offset field without an explicit format conversion.
No raw entry names or key bytes are copied into message text.

The first encountered failure ends the operation. Sorting preserves deterministic
order for equal starts, but an allocation-table contradiction may be found before
an unread malformed ownership record. The design does not add a validation pass
to choose a globally preferred error. Healthy-store results and format verdicts
must not depend on sort spill, merge fan-in or memory budget; injected failures
retain the attribution of the operation that actually failed.

## Closed verification scopes and driver integration

Sealing closes the append stream; only the driver can establish that its inventory
is complete. Full verification requires an untouched reader and the full common
walk, with no selected-subtree pruning. Unknown critical features or unsupported
objects prevent a complete claim. Omitted names do not narrow the inventory:
verification descends into their directories and checks their allocation and
payload under the same rules, while common reporting retains its separate
omitted-entry semantics.

| Driver | Payload mechanism | Ownership and reconciliation |
| --- | --- | --- |
| FAT12/16/32 | Fused chain/claim/read in `verify_file()` | Common exclusive runs over heap cluster IDs; directory preparation and each file chain emit all runs; single FAT scan plus mirrored-copy and reserved metadata checks |
| exFAT | Common initialized-payload helper | Common exclusive runs; allocation discovery emits entire chains or contiguous allocations, including preallocation; one bitmap scan and FAT lookups only where format semantics require |
| QNX6 | Common helper for first name of each inode | Private owner-tagged runs and selected-snapshot bitmap merge; tree roles, system reservation and inode-reference reconciliation stay private |
| QNX4 | Common helper for first name of each object | Existing paged claimed-block bitset, direct bitmap comparison, inode references and long-name occupancy; no interval stream |
| ImageFS | Common helper for each derived file/link node | No allocation ownership stream; length, whole-image checksum, boot/script references and derived-tree validation remain private |

### FAT and exFAT claim production

FAT installs the run collector before the root is prepared. Directory chain
preparation records the full chain, including allocated tails after a directory
end marker. File verification records every run once regardless of payload mode.
The fixed FAT12/16 root is outside the heap and produces no heap claim; the FAT32
root produces its ordinary chain claims. Dot records and alternate names of one
directory entry do not produce extra allocations. Distinct entries pointing at
the same clusters remain forbidden overlap, even if their contents are identical.

exFAT installs its collector before traversal, then explicitly claims the bitmap,
upcase, and root indexes constructed at open. Each subsequent index construction
records its runs at the same allocation-discovery boundary that validates the
bitmap and the extent. Replaying an existing index is not another allocation
claim. The bootstrap claims cover storage that predates the collector; the
ordinary discovery funnel covers later objects. No second catalogue of objects
or scan of every already-built index is introduced.

exFAT claims allocation beyond logical and valid-data lengths, including a
zero-length file with a nonempty chain. NoFatChain allocations do not acquire FAT
semantics merely because the interval utility is shared with FAT. The exFAT
merge still distinguishes unowned bad storage from orphan allocation; the FAT
merge still validates all meaningful table entries and copies. These are not
options on the utility.

### Why QNX6 retains its ownership records

QNX6's selected-snapshot runs use 16 bytes: `u32 first`, `u32 count`, and
`u64 owner`. The owner is an inode number or a reserved system identity. Its
allocation merge needs that identity to enforce which trees can own reservation
blocks; it also accounts for the allocated but unowned twin reservation. Tree
walks include indirect blocks, while their content indexes contain leaves only.
The verification walks of the system trees therefore cannot be replaced by a
walk of existing content indexes.

The FAT/exFAT representation has no place for the owner. Adding a 64-bit owner
makes every record payload 24 bytes instead of 16 (framed size 32 rather than
24), increases sort scratch and I/O, and buys FAT/exFAT no new check. A second
owner stream requires joining and another ownership rule. A configurable record
codec and comparison callback keeps three layouts and disperses the very bounds
checks this extraction removes. None satisfies this utility's cost and ownership
contract. QNX6 therefore shares payload reading and existing metadata primitives,
and retains its private run codec and cursor.

Within the selected snapshot, each tree contributes ownership once: the system
and long-name trees during verifier setup, each directory on descent, and each
other inode under its first name. Later names retain reference multiplicity for
link counts but emit no new claims and trigger no second payload read. The
existing exposed-inode bitset and tree-walk policies establish this fact; the
payload helper does not infer identity from paths or bytes. Cross-snapshot sharing
is legal and is never collapsed into one exclusive domain. Both header checks
and the selected snapshot's allocation/inode checks retain their distinct scope.

### Why QNX4 and ImageFS do not use runs

QNX4 already claims every owned block exactly once in a paged bitset, including
extent-chain metadata and fixed/system blocks. Duplicate claims fail at discovery,
and verification compares that set with the on-image bitmap in bounded windows.
Adding intervals would duplicate ownership storage and add a sort without removing
the bitset used during ordinary reading. Later `.inodes` names are references to
the already claimed object; payload selection continues to use `later_name`.
Its invalid-label and nonempty `lost+found` scope failures still precede traversal.

ImageFS permits ranges without a heap-allocation model and has no free-space
inventory to reconcile. Its checksum pass is structural work even without the
payload flag; it is not added to `payload_bytes_read`. Each visible or omitted
derived node is checked, while validated shadowed records remain governed by the
ImageFS table rules. Equal inode words or overlapping physical ranges do not
justify inventing FAT-like ownership identities or deduplicating payload work.

## Resource costs and acceptance

For `R` emitted intervals and `P` initialized bytes selected by the driver:

* Payload helper memory is the caller's one bounded buffer, no scratch. It makes
  the same content requests as the replaced loop at the same capacity. Resolution
  and bounded-I/O call counts remain unchanged.
* FAT/exFAT collect `24 * R` framed bytes before sorting, with checked stream
  arithmetic. The utility keeps only bounds, lifecycle and one cursor/interval
  beyond the underlying stream. Driver runs stay compact; no per-cluster facts
  are materialized for the sort.
* Sorting has the existing bounded external-sort pass and peak-store costs.
  Reconciliation consumes each sorted record once and scans allocation metadata
  once, with the same format-dependent table/bitmap I/O as the private loops.
* Append, decode, and comparisons use existing metadata page and sort charges.
  Arithmetic on an already read record needs no additional quota. Allocation
  scans retain their format-specific work charges even on cache hits. Long loops
  never rely only on payload I/O for cancellation.

Acceptance compares identical fixtures and limits before and after extraction:
record count and encoded size, metadata passes, image reads, allocation resolutions,
work charged, peak resident memory, peak scratch, and descriptors. No additional
image-wide pass, chain restart, mandatory extent index, duplicate ownership
catalogue, per-block sort record, or whole-volume resident set is acceptable.
A small fixed wrapper allocation is charged and reported; a limit error never
selects a slower unbounded fallback. No public image bytes or traversal order
change as a consequence of sharing these helpers.

## Tests

Tests isolate helper contracts and retain independent driver fixtures; agreement
between two drivers using the same helper is not independent format evidence.

### Payload and progress

* Read lengths 0, 1, capacity minus one, capacity, and capacity plus one across
  one and several physical extents; assert content calls and confirmed totals.
* Exercise initialized length below logical length with backing zero-tail and
  slack reads configured to fail. Verification succeeds without touching them.
* Return positive short adapter reads, interrupted reads with and without
  progress, premature EOF, and an error after earlier extents succeeded. Assert
  only confirmed bytes count and no byte is retried by the helper.
* Fail the post-read stability check, and cancel or exhaust work on a later
  batch. Confirm partial counts survive, the report is incomplete, and the
  current snapshot and private resources are released by the common lifecycle.
* Read omitted files, links, and omitted directory descendants. Verify file/name
  report counts retain their existing semantics while payload bytes count actual
  reads. Repeated QNX4/QNX6 names increment reference counts without rereading
  content; distinct ImageFS nodes follow their own rules.
* Test count arithmetic at its representation boundary through a bounded fixture
  or private accounting test seam, without issuing impractical amounts of I/O.
* Keep a fragmented FAT fixture with allocation-call counters: structural and
  payload verification each discover its chain once, and neither needs a random
  access index. Tail bytes are never read.

### Run encoding, state and corruption

* Round-trip unit values through the explicit record codec, including values
  above 32 bits. Construct domain zero, nonzero lower bounds, empty domains,
  and a representable endpoint of `UINT64_MAX`.
* Reject zero count, first below the domain, first at/past end, and counts whose
  mathematical end overflows. Accept exactly-ending and adjacent intervals.
* Append unsorted disjoint ranges; compare output in memory and forced spill.
  Reject duplicate, nested, crossing and backwards intervals; equal keys retain
  stable order and are never deduplicated.
* Corrupt stream framing, payload lengths 0 through 15, and valid-length scalar
  fields both before sorting and after sorting. The comparator never reads a
  short record. The cursor rejects corrupt lengths before decoding and rejects
  changed endpoints or order before publishing them.
* Test empty input, one interval ending at the domain end, repeated `DONE`,
  append-after-seal, repeated seal, next-before-seal, and destruction in every
  lifecycle state. A trailing duplicate is detected by the final drain.
* Inject allocation, scratch open/grow/read/write, merge, cancellation and work
  failures at collection and sort replacement. No partial sorted stream escapes;
  all temporary stores and reservations return to baseline after destruction.

### Format and scope evidence

FAT fixtures cover FAT12/16 fixed roots, FAT32 root chains, cross-linked files,
file/directory sharing, cycles, allocated directory tails, invalid mirrored values,
unowned allocations and bad clusters. exFAT fixtures cover bootstrap metadata
claims, contiguous and fragmented streams, NoFatChain with irrelevant FAT bytes,
preallocation beyond logical length, zero initialized data, free owned units,
unowned bad units, and orphan allocations. Mutations must fail with the same
image error classes as the format contracts, independent of spill settings.

QNX6 fixtures preserve both header validation and selected-snapshot semantics,
legitimate cross-snapshot reuse, duplicate indirect/data ownership, system-tree
reservation violations, and repeated inode references. QNX4 fixtures preserve
bitmap-only reconciliation and fixed-block claims; ImageFS fixtures preserve
checksum-before-walk ordering, shadowed records and references. Unsupported
scope, omitted subtrees and resource failures never produce `complete = true`.

Runtime implementation uses the ordinary suite, affected independent format
oracles, and the sanitizer/fault requirements in [build instructions](../docs/build.md)
and [AGENTS.md](../AGENTS.md). The design does not replace those checks with
helper-only tests. Production and test code follow the style guide, including
ASCII source, explicit `_t` types, and `bcopy(src, dst, n)` argument order.

## Generated verification

The [generated-verification design](design-generated-verification.md) specifies
the Relic backend, complete input domains, concrete inode-reference lowering,
and the shared [generation artifacts](design-relic-generation.md). The helpers
here remain the owners of their bounded runtime mechanisms.

A generated verifier uses these same operations when its rules lower to initialized
content reading or strict exclusive runs. It borrows the existing reader session
and uses the same metadata stores, resource accounting and failure lifecycle.
Generation does not introduce a parallel ownership database or a resident tuple
fallback. Rules needing owner-aware sharing remain outside this utility; their
execution strategy must preserve the format's scope and bounds. This contract
provides no Relic runtime, generator, rule language, or new dependency.
