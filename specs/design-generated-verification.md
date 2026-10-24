# Generated whole-image verification

Whole-image rules express relationships between validated records: whether every
reference names an allocated object, whether the number of names agrees with a
link count, and whether a complete inventory leaves an unaccounted object. A
restricted Relic backend compiles those relationships into ordinary C over
fsimg's bounded metadata streams. The verifier does not load a rule program,
link librelic, retain an in-memory fact database, or run a general query engine.

The [generation design](design-relic-generation.md) owns the source, binding,
generation, diagnostic, provenance and checked-in artifact contracts. This
design adds one target profile to that pipeline; it introduces neither another
rule language nor an independently maintained schema or intermediate language.
The [verification helpers](design-verification-helpers.md),
[storage scope](design-storage.md#verification-scope),
[prepared directories](design-directories.md), [content](design-content.md),
[API](design-api.md#extraction-and-verification) and
[security](design-security.md) contracts govern execution. Format designs govern
what counts as a reference, an allocation, a supported object and a complete
verification domain.

## Ownership and the useful boundary

The driver validates geometry, decodes records, walks trees and chains, checks
checksums, identifies objects, and establishes the verification domain. The
common reader owns traversal, omitted-entry descent, content delivery and final
report publication. Generated code owns an admitted relational computation and
its private intermediate streams. Metadata storage owns spill, sorting,
backing-file cleanup and reservations; the session owns cancellation and work.

Generating a join does not make the input inventory complete. In particular,
extractable entries are not the input domain: omitted entries, dot references,
reserved objects and objects discovered at open can matter. The producer is the
format operation that already enumerates the relevant domain, not a second walk
of public paths. A failed parse never becomes a missing fact.

The primary application is QNX6 inode-reference reconciliation. QNX4 supplies a
second application with different identity and counting rules. The backend does
not require converting every format verifier into relations:

| Format | Native evidence and checks | Appropriate relational work |
| --- | --- | --- |
| QNX6 | Selected snapshot, system trees, inode table, every live directory slot including dots; compact owner-bearing runs and bitmap reconciliation | Group reference occurrences and reconcile the complete inode table |
| QNX4 | Exclusive claimed-block bitset, bitmap, direct/root/table object identities, prepared-directory counts and long-name back-references | Group links to `.inodes` slots and reconcile used table records |
| FAT | One chain discovery per object, fused optional payload reads, exclusive cluster runs, FAT copies and reserved entries | Only checks with a demonstrated reduction beyond the exclusive-run utility |
| exFAT | Checked allocation extents, initialized/logical distinction, exclusive runs, bitmap and system files | The same constraint; no second extent discovery for rules |
| ImageFS | Complete decoded records, visible/shadowed identity, checksum and fixed boot/script references | Fixed reference predicates can use local generated checks; a relation database adds no value |

FAT's fused payload/chain walk remains fused. QNX4's bitset remains its ownership
proof. ImageFS checksum reads remain native and do not count as optional payload
read-through. Generated reference rules do not certify allocation ownership,
checksums, namespace uniqueness, source stability or unsupported features.

## One binding contract

The fixed typed binding descriptors from the generation design describe each
relation's columns and generated C fields. Whole-image descriptors additionally
name its producer, domain, key, occurrence identity when applicable, natural
ordering, maximum encoded record size, and the evidence establishing its
cardinality. These are compiler inputs reviewed with the rules, not arbitrary
callback strings or a runtime registration mechanism. They select existing
metadata operations through checked target bindings.

A relation's declaration includes:

* The exact domain, such as all inode slots of the selected QNX6 snapshot, or
  all accepted live slots of directories reached by that snapshot's complete
  traversal. Different snapshots and volumes never share an implicit key space.
* Whether rows identify objects, reference occurrences or distinct projected
  values. A physical position is interpreted within its named volume/snapshot
  and logical object; a host pointer and a display pathname are not identities.
* Which validated source fields authorize a row. Unused records whose remaining
  bytes have no meaning contribute only their defined state, not invented
  values for stale mode, link count or extent bytes.
* The producer's success boundary, including end-of-domain validation, and its
  failure status. A complete empty relation is different from an absent optional
  structure, an unsupported domain and an interrupted producer.
* Key uniqueness and ordering evidence. Generated code validates stream framing,
  field ranges and monotonic order when reading scratch. A uniqueness claim
  needed by a merge is either structural, such as enumerating each inode number
  once, or checked before relying on it.

These declarations travel in the common artifact manifest and source mapping.
Changing a producer's omission policy, identity, unit, key or success boundary
changes the binding contract and invalidates its generated artifacts. The
manifest is a regeneration check, not a signature authenticating scratch data.

### Completeness without another state machine

An input stream is consumed as complete only after its producer succeeds and
seals it. For a native ordered scan, successful exhaustion is its completeness
boundary; an error is never translated to `FSIMG_DONE`. An optional absent table
has an explicit presence value and a format-defined empty domain. A required
missing table fails before dependent rules run.

No caller sets a free-standing `complete` boolean. The generated coordinator
owns collection and sealing, and exposes evaluation only through its successful
finish operation. For negative membership and aggregates, the relevant dependency domain is
closed before its absence or count is used. Materialized dependencies are sealed;
ordered native inputs use the per-key closure contract below. A parser, unsupported-feature, I/O, source-change
or resource failure exits the operation without an image verdict derived from
that partial domain. Positive observations already made are not permission to
publish a complete report.

An ordered native table need not be materialized before matching. Its complete
key domain follows from validated geometry, independently of reading its values:
QNX6 has every key `1..inode_count`. For predicates keyed by one inode, decoding
that inode successfully closes that key's allocation/link facts; later table
rows cannot change them. A negative lookup for that key is allowed only after
that successful decode. A count of references uses the already sealed reference
stream and the end of its sorted key group. An error decoding the current row
closes nothing. Exhaustion and the final drain are required for overall success.
The compiler admits this streaming schedule only when every such lookup is
key-local and the source's monotonic unique-key contract proves closure; an
unkeyed negative query over table values requires complete input instead.

Thus an early link mismatch is reported before an I/O failure in a later inode
window, exactly as in the native merge. Reading or validating the entire table
first would alter that precedence and add an unnecessary pass. Logical rules
state the answer over complete inputs; the application diagnostic schedule
states when a proven local violation ends the operational scan.

This boundary prevents the common false negative: a missing reference is not
proven by a truncated reference stream. It does not magically prove traversal
coverage. That proof remains a concrete driver contract, exercised by omitted,
reserved and multiply named object fixtures.

### Sets and occurrences

Relic relations have set semantics. `reference(Inode)` loses multiplicity and
cannot express a link count. The logical relation is instead
`reference(Inode, Directory, Slot)`, scoped to one selected snapshot. Two live
slots naming the same inode are different tuples. Replaying the same slot is
the same tuple, not another name. A projection to only the inode deliberately
means distinct referenced inodes and requires deduplication.

The native collector emits each slot once by construction through directory
preparation. Its physical stream can omit an occurrence column only if the
compiler proves that the column is used solely to preserve multiplicity for a
count. Exactly-once production is a separate, explicitly reviewed and tested
binding assumption about the native directory-preparation producer; the compiler
does not infer it by analyzing arbitrary C. The manifest records both the
compiler's use proof and the producer's uniqueness assumption. Tests compare
the compact stream against an independently collected full occurrence relation.
Appending twice is then a producer contract violation, not language-level bag
semantics. The simpler full occurrence encoding is used when that proof is
absent; duplicate full tuples are removed before a set aggregate. This fallback
is not automatically an acceptable replacement for the QNX6/QNX4 native checks.
Their adoption review compares encoded record bytes, simultaneous scratch,
sort passes and final scans against the compact native reference streams. A
fallback that defeats the claimed simplification or resource cost is rejected
for that application; its native check remains. Conflicting
values for one declared functional key are rejected, never arbitrarily chosen.

The slot identity is a logical directory slot, not the inode it targets. Dot
and dot-dot slots are distinct, including when both target the root. Names and
raw path bytes are unnecessary for this reconciliation and are not retained.

### Numeric representation

Relic integers are signed 64-bit values. Image offsets, lengths and identities
retain their native unsigned bounds. A binding never casts an unrestricted
`uint64_t` to `int64_t`, wraps it, uses floating point, or rejects a legal image
merely because a field exceeds `INT64_MAX`.

A field with a proven upper bound at most `INT64_MAX` maps directly to a Relic
integer. An unrestricted unsigned identity, offset or length stays in native C;
a rule module requiring it as a numeric column is rejected at generation time.
Unsigned range/end calculations remain native checked helpers; validated bounded
results or explicit boolean predicates can enter rules. Failure of such a helper
is an operational status, not the logical value false. This restriction is on
which rule modules can be compiled, not on which legal images fsimg accepts.
There is no implicit multi-column integer emulation or additional scalar type.

`count` requires a static bound on the complete occurrence domain that fits
signed 64-bit. In the QNX applications, non-overlapping validated directory
storage bounds the number of live fixed-size slots by addressable volume bytes
divided by slot width, below `INT64_MAX`. Native ownership validation establishes
that premise before any rule relies on it: QNX6 completes its allocation phase,
while QNX4 rejects duplicate/free block claims during traversal. A producer also
checks counters while collecting. A binding without such a proof uses native wide reconciliation,
rather than imposing a new image limit. Relic's evaluator tuple-ID limit is not
a format limit and is not inherited by the generated target.

## Accepted relational profile

The backend accepts safe, nonrecursive rules with an acyclic dependency graph.
Positive and negative dependencies and aggregate dependencies all participate
in that check. It resolves the actual Relic frontend representation, including
aggregate group bodies, rather than matching source text or treating the
interpreter's nested-loop plan as an external-memory execution plan.

The admitted operations are selection by bounded scalar predicates, projection,
set union, equality join with a unique side, keyed existence and anti-membership,
and keyed `count` over a complete occurrence-preserving relation. Every head
variable is bound; every negative key is bound by the driving relation. An
aggregate's grouping key comes from an explicit driving domain, so the absence
of group rows produces count zero. A stream of observed groups alone cannot
implement that rule for unreferenced inodes.

Computed scalar expressions obey the generation design's signed-range proofs.
There is no recursive reachability, transitive closure, general many-to-many
join, Cartesian product, string interning, regex evaluation, dynamic rule load,
`sum`, or arbitrary aggregate implementation in this profile. Rule compilation
fails with source attribution when the backend cannot prove a bounded lowering.
It does not fall back to the Relic runtime or silently omit a rule. Traversal
cycles remain the native walker's responsibility.

The compiler builds one finite operation graph from checked rules and the common
bindings. This is its internal lowering, not a separate application-authored
plan or second semantic source. Source-level predicate dependencies determine
which complete inputs each operation needs; key/cardinality proofs determine
whether that graph is admitted. Relic's frontend safety and stratification are
necessary but insufficient: an otherwise legal Relic program can have a
quadratic join or an unsupported numeric domain.

### Lowering and cost

Let `N` and `M` be input row counts, `R` the occurrence count, `K` the number of
distinct grouping keys, and `B` the configured bounded sort memory. Record widths
are fixed or bounded by the binding. The generated plan records the following
proof for every node:

| Logical operation | Native operation | Output bound and resident state |
| --- | --- | --- |
| Selection | Sequential validated decode and predicate | At most `N`; one record |
| Projection or union | Encode, external sort by full tuple, adjacent deduplication unless uniqueness is proved | At most `N`, or `N + M`; sort budget plus one previous tuple |
| Keyed count | Sort by key and full occurrence identity, deduplicate complete tuples, fold each key | At most `K <= R`; one key and checked count |
| Equality join | Sorted merge with a unique side; fan out only the nonunique side | At most the nonunique input count; one row from each side |
| Existence/anti-membership | Sorted merge against distinct keys | At most driving input count; two cursors |
| Count over explicit domain | Merge grouped counts with unique ordered domain, supplying zero for absent groups | Exactly domain size before filtering; two cursors |

A merge consumes each input once: `O(N + M)` comparisons and bounded cursor
state. Grouping after sorting is `O(R)` work. Sort uses the storage design's
external merge algorithm, bounded arena and fan-in; its comparisons are
`O(N log N)` and its sequential I/O passes depend on run count and fan-in.
The manifest names required orderings, which inputs already provide them, every
materialized intermediate, and each intermediate's row/byte bound. It does not
promise a constant number of passes independent of resource settings.

No operation retains an entire equal-key group in RAM. The unique-side merge
restriction prevents a large duplicate-key group from requiring an in-memory
bucket, group replay, or quadratic output. A group used by many driving rows is
folded once and merged, not rescanned per row. Reuse materializes a bounded
scratch stream or repeats an explicitly costed native scan; it never silently
introduces an in-memory index. Adjacent deduplication retains one tuple, and no
output is emitted per derivation when set projection requires deduplication.

Reservations include simultaneous original and replacement sort streams,
intermediate stores, framing, merge buffers and descriptors according to the
existing metadata contract. Checked products bound encoded bytes, counts,
offsets and endpoints before reservation or access. Insufficient scratch or
memory is `FSIMG_ELIMIT` or the existing allocation failure, never permission to
truncate relations. Semantic counts are exact, unlike saturating observational
payload statistics.

Each generated scan, comparison/fold loop and output operation participates in
session work accounting and polls cancellation at bounded batches, including
cached scans and a single enormous group. Existing sort and I/O charges are
reused without double charging; new CPU-only relational work has explicit
charges. Charging the original directory walk does not exempt a later sort or
merge from cancellation. There are no per-operation sessions that reset quotas.

## QNX6 inode-reference reconciliation

### Domain and facts

The verification instance identifies one selected snapshot. Its native walk
accounts for system trees, directories, ordinary and omitted objects, and reads
each distinct object's initialized payload when requested. Repeated hard-link
names contribute separate references but do not repeat allocation claims or
payload reads. The inode domain is exactly `1..inode_count`; the inode scan
reads every slot in bounded windows, including unreferenced slots.

The logical inputs are:

| Relation | Meaning |
| --- | --- |
| `inode(I)` | Every inode number in the selected table, once |
| `allocated(I)` | The defined allocation bit is set for that slot |
| `links(I, L)` | Link count of an allocated slot; stale free-slot bytes are not facts |
| `reference(I, D, S)` | One validated live slot `S` in directory `D`, including `.` and `..` |
| `expected_free(F)` | The selected header's validated free-inode count |

For this binding, `D` is a bounded inode number and `S` is a logical slot
number, not a byte offset. Dividing the bounded directory length by the 32-byte
slot width proves `S <= UINT64_MAX / 32`, so both fit signed 64-bit. The QNX4
binding likewise uses bounded target/source identities and 64-byte logical slot
numbers. The two pilots therefore use ordinary Relic integer columns. Wider native
values remain outside their rule interfaces. The physical binding
can pack booleans and bounded fields together without requiring one scratch
stream per predicate. The complete inode scan itself provides its natural
numeric ordering; it need not be copied into scratch just to satisfy the model.
Reference collection happens during preparation, before public-name omission.
Slots rejected by parsing terminate verification; they do not disappear from
`reference`.

Representative rules, shown with single bounded columns for occurrence keys,
are valid relational expressions of the intended checks:

```prolog
reference_count(I, N) :-
    inode(I), N = count { reference(I, D, S) }.

missing_inode(I) :- reference(I, D, S), not inode(I).
unnamed_allocated(I) :- allocated(I), reference_count(I, 0).
reference_to_free(I) :-
    reference_count(I, N), N > 0, not allocated(I).
wrong_links(I) :-
    links(I, L), reference_count(I, N), N > 0, N != L.

referenced_inode(I) :- reference(I, D, S).
free_mismatch(F, N) :-
    expected_free(F), N = count { inode(I), not referenced_inode(I) },
    F != N.
```

The count body retains `D` and `S`; replacing it with a projection onto `I`
changes the rule. The final count is meaningful only after per-inode free-target
and link checks and the trailing-reference domain check succeed, matching the
driver's invariant that all remaining unreferenced slots are free. The diagnostic binding specifies this phase order;
logical rule text alone does not select the first public error.

### Execution with bounded state

1. Collect one reference occurrence per accepted live directory slot in the
   instance's private stream. The native walk completes, including omitted
   directories. Allocation reconciliation succeeds before the reference phase
   uses its storage/cardinality premise.
2. Seal and externally sort references by target and occurrence key. Sorting
   completes before inode reconciliation, as in the native operation. Fetch and
   validate exactly one reference lookahead, checking length before fields.
3. Fuse grouping with the inode merge; do not materialize all grouped counts.
   For each inode number in ascending order, consume only its reference group,
   retaining the first record of the next group as lookahead, or successful
   exhaustion. Validate each consumed record before counting it. Missing groups
   mean zero, including an entirely empty reference stream. After that group's
   lookahead succeeds, fetch and charge the inode window if this inode begins a
   new window; otherwise use the current window. Decode this inode and perform
   its allocation/link checks immediately. Increment the distinct-referenced
   counter once for a positive group. Do not fetch another reference or inode
   window before those checks succeed.
4. After the final inode, require that the existing lookahead denotes exhaustion.
   A remaining validated lookahead is an out-of-domain reference and fails
   immediately; it is not permission to ignore trailing data or to read further
   records in search of another error.
5. Compare `inode_count - referenced_count` with the header's free-inode count.
   The merge proves `referenced_count <= inode_count` before subtraction.
   Return successful reference reconciliation to the driver; its final source
   stability check and common report completion still have to succeed.

This schedule preserves the native QNX6 failure boundary: failure reading the
current group's terminating lookahead precedes that inode's semantic check and
any newly needed inode-window read. A failure farther into a later reference
group or a later inode window cannot mask the current inode's link mismatch.
Sorting itself can fail before any inode check. Metadata buffering may fetch a
bounded page as part of a requested operation, but the generated merge adds no
eager logical reads or prefetch phase. General intermediate materialization is
allowed only under a separately declared compatible operational schedule; it is
not an alternative lowering for this QNX6 reference phase.

For a table containing root inode 1 and file inode 2, root slots `.` and `..`
contribute `(1, 1, 0)` and `(1, 1, 1)`. Two ordinary names of inode 2 contribute
`(2, 1, 2)` and `(2, 1, 3)`. The counts are two and two, although there are only
two distinct targets. An allocated inode 3 with no slot has count zero and fails.
A free inode 3 with arbitrary old link bytes is valid and contributes one free
inode. Changing a name's target beyond the table fails target-domain validation;
an I/O failure while reading the table fails the operation rather than treating
the unread tail as free.

The collection is `O(R)` records, sorting is `O(R log R)`, and the final scan is
`O(R + I)` for `I` inode slots. Resident memory is the bounded sorter plus one
inode window, one reference record and a few exact counters. There is no array
indexed by every inode and no nested scan of references for each inode.

## QNX4 as a separate binding

QNX4 object identity includes its place: a direct directory record, a root-block
record, or a logical `.inodes` slot. A table slot number alone cannot identify
every object, and directory-entry names are not interchangeable with QNX6's
all-slot reference count.

The QNX4 collector records each link whose resolved target is in `.inodes`, with
its source occurrence identity. The native table scan includes signature slot
zero and all table slots. Slot zero is not an inode; unused slots have no
meaningful link or mode fields. The generated reconciliation enforces:

* No reference names slot zero, an unused slot, or a slot outside the table.
* Every used nonsignature slot has at least one name.
* A used nondirectory slot's name count equals its stored link count.
* A used directory's table-name count is not compared with its link count.
  Native preparation checks `2 + subdirectories`, and directory admission owns
  the single-directory-parent invariant.

Direct and root-block nondirectories retain their native one-name requirement.
The empty `lost+found` directory's link count is checked even though it was
walked at open. An invalid label, nonempty unsupported `lost+found`, or unsupported
object prevents complete verification; no empty relation substitutes for it.
Long-name state/back-reference validation and the used-record/named-bitset pass
remain native. The exclusive claimed-block bitset is compared with the bitmap,
including the final-byte mask, without converting it into block facts.

The table merge uses the same admitted group/merge operations as QNX6 but a
different row predicate and phase order: QNX4 checks references, long names and
allocation in its driver order. Its reference bound derives from the validated
fixed-width source slots and bounded directory storage, not a premise that
requires moving its allocation phase. A directory with one table name and three
links can be valid when it has one subdirectory; treating QNX6's equation as a
shared rule would falsely reject it. A nondirectory with two names and one link
fails. A free table slot with nonsensical leftover bytes succeeds only when its
name count is zero.

QNX4 also fuses count grouping with table reconciliation, but its read order is
different. After sorting it fetches one reference lookahead, then reads and
charges the first table window before grouping references for that window's
first slot (including signature slot zero). For each slot it consumes only the
matching group and one terminating lookahead, then checks that slot immediately.
At a window boundary it reads and charges the next table window before consuming
that window's first group. It never materializes all counts or reads ahead to
another group before the current slot check succeeds. At table end it checks
the already fetched lookahead for exhaustion. Thus a QNX4 table-window read
failure precedes a failure consuming its first slot's group, whereas QNX6
consumes that group before fetching the new window. The bindings retain these
two explicit schedules instead of hiding their difference in an unspecified
merge callback order.

The implementation shares generated mechanics and their contracts, not a
parameterized universal inode policy. Format-specific predicates remain short,
reviewable expressions next to their explicit bindings.

## Ownership intervals are a separate question

Reference reconciliation does not require generic ownership relations. Where
an admitted ownership check is useful, inputs remain compact half-open runs
`[first, end)` in a named allocation-unit domain. They are not expanded into one
fact per block, nor compared by an all-pairs overlap join. End calculation and
unit-to-byte conversion stay in checked native code.

For a strictly exclusive domain, sorted nonempty runs need only one previous
end: any next start below it is forbidden, including identical claims from one
owner. The [exclusive-run utility](design-verification-helpers.md) already owns
that operation and its final drain. Generated code calls the utility when needed
rather than cloning it. A native bitmap/FAT scan then reconciles claimed and
allocated storage in bounded windows.

That algorithm does not implement legal sharing. QNX6 snapshot/object/logical
range/role identities and cross-snapshot sharing rules remain the QNX6 driver's
contract; splitting conflicting owners into different instances cannot prove
exclusivity. A rule requiring those semantics is rejected unless its binding
names an independently specified bounded native operation with equivalent
semantics. There is no generic owner registry, sharing callback, recursive
closure or active set of arbitrarily many overlapping intervals hidden behind
the generator. The mechanism is omitted when the driver already proves the
property with less state.

## Generated C entry points and lifetime

Generated APIs are private, format-specific C interfaces with `_t` typedefs and
ordinary `fsimg_status_t` results. The application binding fixes their names and
argument types; their common shape is construction, typed occurrence admission,
and one finish operation:

```c
typedef struct fsimg_qnx6_reference_check fsimg_qnx6_reference_check_t;

fsimg_status_t fsimg_qnx6_reference_check_new(fsimg_session_t *session,
    fsimg_qnx6_reference_check_t **out, fsimg_error_t *error);
fsimg_status_t fsimg_qnx6_reference_check_add(
    fsimg_qnx6_reference_check_t *check, uint32_t inode,
    uint32_t directory, uint64_t slot, fsimg_error_t *error);
fsimg_status_t fsimg_qnx6_reference_check_finish(
    fsimg_qnx6_reference_check_t *check, fsimg_reader_t *reader,
    unsigned char *window, size_t capacity, fsimg_error_t *error);
void fsimg_qnx6_reference_check_free(fsimg_qnx6_reference_check_t *check);
```

This sketch states the borrowing boundary, not an additional public API. `new`
sets `*out = NULL` before work and publishes only a complete collector. It
borrows the existing session until `free`; all owned streams and buffers are
charged there. `add` copies typed scalar values synchronously and retains no
pointer into a prepared directory or reader snapshot. Its use belongs to the
one directory-preparation producer, so replay cannot append another occurrence.

`finish` borrows the reader and actual allocated window for the call. It
requires that reader's session to be the collector's session and binds the
selected snapshot/domain before consuming inputs. The binding validates the
minimum capacity and batch alignment before I/O; capacity zero cannot mean
silently skipping the table. It does not advance the common walker, invalidate
a snapshot, retain a locator, change selection or mark the report complete.
The driver owns its payload buffer separately, or reuses it only after payload
work has ended and when its capacity satisfies the metadata window contract.

The lifecycle is collecting followed by a single consuming finish and disposal.
An operational failure leaves the object disposable only. Repeated finish or
admission after finish is `FSIMG_ESTATE`; argument rejection before work does
not mutate the object. The generator does not expose independent seal, sort,
rewind and retry flags to the driver. Internal cursors publish a row only after
successful decode; `FSIMG_DONE` is separate from every valid key, including zero.
`free(NULL)` is harmless, and free accepts all partially constructed or failed
states. Reader close can release the collector after unwinding without double
freeing streams already released by finish.

All native scans use bounded reader/session I/O and retain source-stability
checks. Generated operations obtain no host pathname or unrestricted descriptor,
create no private scratch directory, reset no budgets, and perform no ambient
logging. Scratch contains only necessary metadata, with explicit endian codecs
and initialized bytes, never native struct padding or borrowed addresses.

## Verdicts and deterministic diagnostics

The generation design's stable rule identifiers and diagnostic bindings map a
logical violation to the driver's existing status and operation. There is no
new public verification status. Semantic corruption is `FSIMG_ECORRUPT`;
unsupported scope is `FSIMG_EUNSUPPORTED`. ImageFS's non-file boot/script target
retains its unsupported classification, while a missing or ambiguous target
retains corruption. A generated boolean must not collapse that distinction.

Operational failures propagate unchanged: cancellation, source change, I/O,
allocation and resource failures do not become false predicates or violations.
Scratch decoding uses the metadata error contract and names scratch as the
source; its status does not falsely identify an image byte as corrupt. Record
length is checked before decoding any field, including comparator paths.

The first failure in the declared driver phase order ends verification. Within
a relational phase, the binding orders diagnostics by domain key and explicit
rule precedence, with deterministic occurrence tie-breaking when needed. Sort
spill, merge fan-in and input discovery order do not change healthy-store
verdicts or the chosen semantic diagnostic. There is no expensive global sort
of every possible violation, and no reordering of checksum, payload, scope or
allocation phases just to prefer a particular rule. Injected I/O failures keep
the attribution of the operation that actually failed.

Diagnostics contain stable format/rule context and bounded numeric provenance.
Allocation units and inode numbers are not placed into byte-offset fields
without a validated conversion. Raw names, scratch bytes and host paths are not
interpolated into messages. An omitted entry need not have a public pathname to
be checked or diagnosed. A diagnostic stream is not an unbounded retained list:
the ordinary first-error API remains authoritative.

Successful generated finish means its complete inputs satisfy its rules. The
driver still performs remaining native checks and final stability validation.
Only common verification publishes `report.complete` after all required work
succeeds. Payload bytes confirmed before failure retain the helper's saturating
observation-counter semantics; exact logical totals and relation cardinalities
never use saturation.

## Validation and independent evidence

Rule validation and runtime image verification have different oracles. A rule
can accurately express a wrong filesystem assumption, and correct rule text can
be lowered incorrectly. Acceptance requires evidence at both boundaries:

1. **Frontend/profile checks.** Reject recursion, unsafe negation, unknown input
   bindings, aggregate projection that loses required occurrence identity,
   unsupported numeric operations, missing completeness dependencies, unproved
   unique keys and many-to-many expansion. Include valid Relic programs that
   this target deliberately refuses. Diagnostics point to the original rule.
2. **Evaluator differential tests.** For bounded small fact sets, evaluate the
   admitted rules with Relic. The test oracle selects the expected first violation
   from the evaluator's complete answer set using the declared driver phase,
   key and rule precedence, then compares that status and diagnostic with the
   actual production `finish` result. Hand-authored cases isolate every exported
   violation so an earlier rule cannot mask an incorrect later rule; mixed cases
   exercise precedence. The test uses the ordinary typed collector and bounded
   memory-backed reader fixtures, not a diagnostic enumeration callback, a second
   generated evaluator or a runtime mode that continues after failure. Injected
   operational failures are compared against the specified operation schedule,
   not the evaluator's complete logical answer set. Include duplicates, shuffled input,
   zero-row domains, empty groups, key gaps and maximum bounded values. Test
   rejection of unsupported wide bindings at generation time, and native
   handling of legal values above `INT64_MAX`. Compare native unsigned predicates
   separately with an independent exact arithmetic oracle. The reference evaluator's
   implementation limits restrict test fixture size, not production images.
3. **Independent relational oracle.** A small, plainly written test oracle
   counts distinct occurrence tuples and checks table records without using the
   production sort/merge or generated codecs. It establishes expectations for
   sets, occurrence multiplicity, free-slot ignored bytes, trailing references
   and deterministic diagnostic precedence. It does not duplicate the lowering.
4. **Format corpus.** QNX6 tests cover root dots, subdirectory dot-dot links,
   regular hard links, omitted names/directories, allocated unnamed inodes,
   references to free/out-of-range slots, free-inode totals, selected snapshots
   and independent images. QNX4 tests cover direct/root/table identities,
   signature slot zero, directory versus file counts, long names, empty and
   unsupported `lost+found`, free-slot garbage and bitmap padding. Existing FAT,
   exFAT and ImageFS corpus checks ensure shared generation changes preserve
   their native status and scope. Independent format-tool and QNX evidence
   fixtures remain necessary; generated writers alone are not an oracle.
5. **Bounded execution and faults.** Compare in-memory and forced-spill runs,
   narrow fan-in and small windows. Exercise one huge group, many tiny groups,
   deeply omitted trees and highly fragmented allocations. Measure peak resident
   metadata, scratch high-water and work against the emitted plan. Inject short
   or malformed scratch records, allocation/sort failures, premature EOF,
   cancellation in grouping and merge, and source changes between collection
   and table scan. Verify incomplete reports, preserved status and cleanup.
   Paired failure tests place a link-count mismatch in an early inode and an
   injected scratch-read failure beyond its terminating lookahead, or a table
   read failure in a later window: the mismatch wins and the later read is not
   attempted. Complementary tests fail the terminating lookahead itself, which
   must win over that inode's mismatch. At a table-window boundary, inject both
   a reference-group read failure and a table-window failure: QNX6 reaches the
   group failure first; QNX4 reaches the table failure first. Use operation-local
   fault injection after successful sorting so sort I/O cannot accidentally
   satisfy these cases, and assert the actual requested-read trace as well as
   status. These tests enforce production early exit, not just logical verdicts.
6. **Artifact and compiler checks.** Apply the common deterministic regeneration,
   manifest, provenance, license, ASCII and standalone C compilation gates.
   Mutations that remove a violation, deduplicate references by target, suppress
   a final drain, or weaken a domain check must fail tests. Runtime generated
   code receives the same sanitizer and fuzz obligations as handwritten parser
   and metadata code; generator-only evaluation cannot substitute for them.

Small evaluator tests establish semantic equivalence, large native tests
establish bounded execution, and independent images establish the interpretation
of filesystem fields. None of these three claims implies the other two.

## Maintenance tradeoff

The design removes per-format reference record comparators and codecs, duplicate
sort/group/cursor cleanup, handwritten zero-group and trailing-reference merge
logic, and separate implementations of simple name-count predicates where the
admitted generated operation replaces them. A driver supplies validated
occurrences and invokes one consuming operation instead of coordinating sort
replacement streams and reconciliation cursors itself.

The driver still owns its domain, native scan, traversal coverage, payload work,
format scope and phase order. The resource context, bounded I/O and metadata
store retain their existing authority. There is no new persistent object model,
runtime optimizer, transaction protocol or generic filesystem verifier.

The cost is a maintained target backend: checked binding interpretation,
cardinality/range proofs, a small external-operation lowering, source mappings,
artifact regeneration and differential tests. These obligations are real even
when generated C is short. A rule needing another generic operator is admitted
only when the whole-system reduction justifies extending that compiler and its
proof/testing surface. Keeping a small native loop is preferable when generation
merely hides its obligations behind a new callback framework.
