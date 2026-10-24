# Common driver contract tests

## Purpose and boundary

The common contract suite checks that every advertised filesystem operation
obeys the library's lifecycle, bounded I/O, resource, source-consistency and
metadata-loss contracts. A driver supplies a small declarative test profile;
shared scenarios perform the operations and assert their results. Registration
creates a coverage obligation, not an expectation oracle.

This design owns the C contract tests. The [architecture](architecture.md) owns
the behavior being tested, [format design](design-formats.md) owns format
semantics, and [specification-driven testing](design-spec-testing.md) owns the
independent Ruby models and their rule-derived expectations. Test profiles do
not change production descriptors, public options, capabilities or failure
semantics. In particular, metadata-loss tests exercise the common writer loss
policy and its driver encodings; they do not introduce another loss policy.

Filesystem and image-layer profiles are separate. Filesystem scenarios operate
on a raw bounded volume, its entries and its content. Layer scenarios operate
on the image stack, decoded disk bytes, selection, transcode and output
ownership. A container carrying ImageFS is a layer with a constrained payload,
not an extra filesystem variant. Sharing their callback I/O helpers does not
make their operation models interchangeable.

The suite does not replace format geometry tests, independently decoded writer
images, native qualification, CLI protocol and sandbox tests, fuzzers, or the
Ruby rule harness. It does not acquire mounts, privileges, network access or
external utilities during a C test. It has no plugin discovery, test DSL,
serialized profile language or generic callback-driven execution engine.

## Evidence and responsibilities

The relevant test surfaces make the separation concrete:

| Surface | Responsibility retained there |
| --- | --- |
| `test/emit.c` | Common source and sink scenarios; direct emission-copy tests and private corrupt-plan tests remain focused unit tests |
| `test/posix.c`, `test/metadata-loss.c` | Independent capability expectations, unsupported kinds, metadata omission and encoding-loss policy |
| `test/format.c` | Probe claims, ambiguity, forced variants, bounded prefixes and fake operation tables |
| `test/fat.c`, `test/exfat.c`, `test/qnx4.c`, `test/qnx6.c`, `test/ifs.c` | Format-local readers, content mapping, malformed images and verification witnesses |
| `test/*-write.c` | Independent byte/layout checkers, option matrices, special entry semantics and corrupt private plan records |
| `test/extract.c`, `test/publish.c` | Host paths, destination safety, aliases, publication and durability semantics |
| `test/layers.c`, `test/layers-write.c`, `test/containers.c`, `test/compression.c`, `test/gzip-write.c`, `test/hbcifs.c`, `test/ifsboot*.c` | Layer parsing, encoding, constrained profiles and composition seams |
| `test/spec/` and integration scripts | Independently built/classified images and external implementations |

A shared scenario replaces a copied scenario only when it preserves its
assertions and fault reachability. A raw offset mutation, private row corruption
or native checker stays with the format that explains its meaning. Common
coverage never counts an assertion that only a local test happens to make:
the common runner actually executes the required scenario for the profile.
Conversely, a local regression remains useful even when it overlaps a broad
common scenario because it fixes a specific input and failure path.

## Profiles are test facts

A filesystem profile identifies a public variant by name and, where needed, a
named configuration within that variant. It declares these facts independently
of the production registry:

* Expected read, write and verify capability bits.
* A bounded valid input recipe and expected entry/content manifest for readers.
* Explicit valid creation options and a source manifest for writers.
* Expected stored metadata, loss fields and counts for selected metadata cases.
* Expected verification scope and counts for the fixture.
* The finite scenario families that the fixture exercises, with a reason for
  each conditional family that does not apply.
* Fixture byte, entry and host-resource ceilings and a stable diagnostic ID.

A public name can have several profiles, for example different supported byte
orders or an option combination whose resource path differs. Exactly one is its
coverage profile. Additional profiles add targeted witnesses; they cannot hide
a missing mandatory scenario in the coverage profile. FAT12, FAT16 and FAT32
are separate coverage keys even though one production descriptor serves them.
The same rule covers each separately enumerated QNX profile. Internal numeric
variant IDs are not stable test identities.

A profile records explicit values rather than deriving legal options or
metadata expectations from `fsimg_format_info_t`. It can use public initializer
functions for size/version/default setup, then assigns every value whose
meaning the scenario tests. Defaults themselves have dedicated assertions.
Production helpers for timestamp encoding, name folding, layout sizing or loss
classification are not expectation generators.

The following sketch shows the intended test-only surface. The complete entry
and metadata expectation records contain ordinary typed fields; these sketches
omit those fields rather than prescribe a second public API.

```c
typedef struct contract_tree contract_tree_t;
typedef struct contract_metadata_case contract_metadata_case_t;
typedef struct contract_fixture contract_fixture_t;

typedef struct contract_fs_profile {
	const char	*id;
	const char	*format;
	bool		 coverage_profile;
	unsigned	 expected_capabilities;
	const fsimg_write_options_t *write_options;
	const contract_tree_t *source_tree;
	const contract_tree_t *expected_tree;
	const contract_metadata_case_t *metadata_cases;
	size_t		 metadata_case_count;
	fsimg_validation_scope_t expected_scope;
	uint64_t	 max_fixture_bytes;
	/* Optional independently authored reader fixture. */
	fsimg_status_t	(*materialize)(contract_fixture_t *, fsimg_error_t *);
} contract_fs_profile_t;

static void contract_check_registry(void);
static void contract_run_reader(const contract_fs_profile_t *);
static void contract_run_writer(const contract_fs_profile_t *);
static void contract_run_verify(const contract_fs_profile_t *);
```

The profile arrays have static lifetime. Option values are copied into a local
initialized options object before a scenario changes its flags or limits; no
scenario mutates a profile. The fixture materializer is the only format-specific
callback in this surface. It supplies valid bytes, with size bounded by the
profile, into runner-owned storage. It receives neither scenario selection nor
fault settings, cannot run the assertions, and returns normal status on setup
failure. It does not transfer an open reader or opaque cleanup state.

When a format both reads and writes, a profile can use the common writer to
produce a round-trip fixture. Such a fixture counts as integration evidence,
not independent format evidence. A reader-only profile requires an independent
materializer or checked-in bounded fixture with provenance and expected tree.
A writer-only profile runs writer lifecycle and byte checks without inventing
reader or verification support. The owning format suite supplies independent
on-disk validation for its emitted bytes. Verify scenarios require an actual
verify capability; read capability alone does not imply it.

The source and expected trees are fixed small tables. An entry holds its path,
kind, logical size, explicit metadata validity mask and values, and a named
payload recipe. The common recipes include empty, zeros and a deterministic
byte pattern derived from file identity and offset. Mixed DATA/ZERO providers
use explicit ranges. No image-sized expected buffer is necessary. Symlink
text, hard-link relationships and unsupported kinds appear only in the
appropriate semantic cases, not as assumptions made of every driver.

Filesystem-specific metadata tables state exact representable values,
rounding, omission, required-field refusal and optional-loss acceptance. The
same source field can have different expected stored values in different
profiles. Counts distinguish fields from callbacks: one callback can report
several fields, and repeated observations of a field do not multiply the
writer's loss total. Callback grouping or order is asserted only where the
public contract specifies it. Expected masks and counts are written from the
format and metadata contracts, never copied from an observed successful run.

## Registry reconciliation and applicability

The runner enumerates `fsimg_format_next()` before running any fixture. It
matches public names, not array positions, and checks both directions:

1. Every advertised variant has exactly one coverage profile.
2. Every profile names an advertised variant; profile IDs are unique.
3. The independently declared capability mask equals the advertised mask.
4. Every advertised operation has its required fixture facts and common
   scenario families. Each profile's optional cases are internally consistent.
5. The run records completion for each scheduled `(profile, scenario)` pair.
   Every scheduled pair reaches a pass or a reported failure.

Missing fixtures, stale profile names, duplicate coverage rows, an unexpected
capability and an unexecuted mandatory scenario fail the ordinary suite. They
are not skips. The suite does not silently filter profiles against what the
registry happens to contain. Build-conditioned registration, if present, has
an explicit corresponding build-conditioned expectation rather than a blanket
"unknown means unavailable" rule.

The registry selects obligations only. For example, a write bit schedules
writer scenarios, but their expected payload bytes, metadata and failures come
from the profile and scenario contracts. A changed descriptor cannot make both
the implementation and its expectations agree by construction.

Applicability has three separate meanings:

| Result | Meaning |
| --- | --- |
| Required | The capability and scenario preconditions require execution; setup failure fails the test |
| Not applicable | A declared semantic condition excludes the case, such as writer scenarios for a reader-only format |
| Unavailable | An external oracle or host facility needed by a separate integration check is absent; the check reports this and does not claim a pass |

There is no arbitrary per-profile mask that disables common lifecycle, source,
sink or resource tests. Conditional metadata cases name their precondition in
data, such as an optional representable timestamp or a supported symlink kind.
A missing fixture witness for a supported behavior is a coverage failure.
Tests of unsupported operations use the owning public API's specified refusal;
they do not call absent private function pointers.

## Scenario matrix

Each scenario uses a fresh operation and explicitly identifies its phase.
"All writers" means every filesystem coverage profile advertising write;
"readers" and "verifiers" have the corresponding meaning.

| Family | Applicability | Required evidence |
| --- | --- | --- |
| Empty and small tree | Readers and writers | Empty file, nonempty file, empty directory and nested file; exact names, kinds, lengths and bytes |
| Reader lifecycle | All readers | Open, info, iteration, stable end-of-iteration, current-entry content lifetime and documented state errors |
| Bounded volume | All readers and writers | Nonzero slice base, guard bytes around the volume, no backing access beyond the slice |
| Content delivery | All readers | Random reads including EOF, clipped reads, sequential ranges and consistency of logical bytes |
| Writer lifecycle | All writers | Collection seals at plan, repeated plan inspection, one emission attempt and terminal-state behavior |
| Source consistency | All writers | Changed empty/zero/data source, change after read, short provider EOF and injected provider I/O failure |
| Defined output | All writers | Clean and poisoned sinks, zero-range callback present/absent, positive short transfers; identical successful output |
| Reproducibility | All writers | Identical fixed inputs and identifiers yield byte-identical output across sink modes and supported cache budgets |
| Metadata | Profiles with applicable fields/kinds | Exact preservation, optional loss accepted and counted, disallowed/required loss refused, unsupported kind refused |
| Sink failure | All writers | First and later write failure, confirmed partial write, no-progress write, zeroing failure and flush failure |
| Allocation and scratch | Each applicable operation | Injected allocation failure; scratch open/read/write failure when the workload reaches each operation |
| Cancellation | Each applicable operation | Pre-work and in-progress cancellation, terminal state and bounded cleanup |
| Limits | Each applicable operation | Memory/scratch and cumulative entry/work limits with a witness that actually reaches the constrained resource |
| Verification | All verifiers | Fresh-reader requirement, exact scope/counts, payload flag behavior and incomplete reports on failure |
| Extraction | All readers with extractable entries | Private destination, exact extracted bytes and safe metadata; no publication success after failure |

Read errors distinguish malformed input from transport failure. A physically
short image presented with its true short length is a format-local truncation
case. An adapter that advertises a valid length and unexpectedly returns EOF
injects `FSIMG_EIO`. These cannot share one generic "truncated" expected result.

Traversal checks compare the expected entries by path, reject duplicates and
prove no expected entry is missing. Exact iteration order is asserted only
where a fixture independently specifies the documented on-disk traversal
order. A sorted expected manifest must not accidentally prescribe lexical
iteration to every reader.

The bounded-volume case places unrelated canary bytes before and after a
volume, accesses it through the public slice adapter and checks both bytes and
callback access records. A failed bounds check cannot be counted merely because
the canaries happened to survive. An out-of-range attempted backing callback
fails immediately, even if it would have transferred zero bytes. Callback
buffers and extents obey the public I/O contract themselves.

Defined-output comparisons establish that stale sink contents cannot affect a
successful image. They do not prove format padding is zero: a writer could
consistently emit the same nonzero padding. Independent format checkers retain
the explicit assertions for reserved regions, unused allocation units, record
padding and file tails. Source ranges explicitly reported as ZERO also carry
a read counter; range discovery remains permitted, but no provider `read_at`
call is allowed for those ranges. A DATA range whose bytes happen to be zero
has no such no-read guarantee.

The common verification test uses a fresh reader for each verification scope
and payload setting. It asserts report completeness only on success and the
exact fixture counts, including the implicit root. It does not treat header
acceptance or successful selected extraction as whole-volume verification.
Corruption outside a selected subtree, sharing legality and allocation
reconciliation stay in the format-local verification cases.

Host breakout, symlink pivot, ACL and no-replace publication tests belong to
the common extractor/output owners. The per-profile extraction scenario
checks the driver-to-extractor seam without repeating every host attack for
every format. Entry kinds outside the safe extraction profile have explicit
refusal or skip-policy cases rather than weakening the destination policy.

## Scenario execution and lifetime

The runner uses normal named C functions. A scenario body reads as setup,
operation, assertion and cleanup, with a local fault setting when relevant.
There is no list of callbacks for each phase and no profile-owned teardown.

1. Validate the profile and choose one named scenario. Create a disposable
   private fixture directory and bounded test storage.
2. Create the resource context and record its baseline before operation-owned
   handles are allocated. Initialize options, callbacks and observations.
3. Materialize valid reader input or populate the writer using the explicit
   manifest. Confirm setup succeeded before arming a phase-specific fault.
4. Arm one fault, perform the named API operation and capture status, error,
   report and callback observations before another API can overwrite them.
5. Disarm the fault. Assert the result, state transition, output state and
   confirmed progress. Exercise the documented follow-up state operation.
6. Free operation handles, then their borrowed I/O objects and backing storage
   in dependency order. Compare live resource charges with the baseline.
7. Destroy the context, check caller-owned descriptors remain usable where
   borrowed, and remove the disposable directory. A nonempty scratch directory
   or failed removal is a test failure.

The runner owns the context, backing buffers/files, callback state and all
handles returned to it. A reader borrows its input until destruction. Writer
source callback state remains alive through writer destruction. A borrowed
plan is never separately freed. The runner respects output APIs that adopt a
raw-intermediate descriptor on every outcome, including refusal; it checks
closure without closing an already consumed descriptor again.

The assertion helper reports profile ID, scenario, phase, fault position,
expected and actual status, error operation/path and relevant counters. It
prints only synthetic fixture data. Handles remain initialized to NULL or -1
until acquired so an unexpected setup error has a straightforward cleanup
path. Failure diagnostics survive cleanup; cleanup failure does not replace
the original operation failure. The harness never follows an image-controlled
path when retaining or removing fixture files.

Resource assertions compare live `memory_bytes`, `scratch_bytes`,
`descriptors` and, for layer operations, `spool_bytes` with their baseline after
all operation handles are released. Peak counters and cumulative I/O counters
are monotonic observations, not values expected to return to zero. The common
suite also checks configured peaks and aggregate accounting across two live
handles in one context. An operation cannot make a limit disappear by opening
a second metadata object or making another incremental API call.

Tests inspect private state only for invariants without a public observation,
such as an active source handle remaining after terminal failure. Such
assertions live in one named helper near the common owner. They do not inspect
each driver's plan layout. A new opaque plan representation therefore does not
require rewriting lifecycle scenarios.

## Fault injection and deterministic outcomes

Faults occur at owned boundaries: source callbacks, sink callbacks, context
allocation hooks, metadata-storage hooks and the cancellation callback. Existing
hooks such as `fsimg_test_fail_alloc_after()` and
`fsimg_test_meta_fail_after()` are reused. Production driver operations do not
gain fault callbacks, and fault configuration is per fixture/context rather
than process-global.

The successful bounded run records the relevant operation counts. A fault
sweep recreates the fixture and injects at each selected position within a
named phase, with a separate final run proving success when no fault fires.
An observer proves the injection actually fired. A run that succeeds because
its target was not reached is not counted as a successful fault case.
Allocation or metadata sequences altered by a preceding failure are separate
runs; a success at one injection index is not proof that every cleanup path
has been exercised. A fixed, generous test ceiling bounds sweep setup and
operation counts; exceeding it fails with measured counts rather than hanging
or silently truncating coverage.

The ordinary suite exhausts allocation and metadata fault positions for its
small fixtures where the path reaches them. Large-scale fixtures use selected
phase boundaries and belong to extended tests. Cancellation samples include
the first poll and observed in-progress phases; exhaustive poll enumeration is
unnecessary because repeated polling is not a separate allocation/cleanup
path. No test waits for a wall-clock race to produce cancellation.

| Injected event | Expected result and independent assertion |
| --- | --- |
| Required context allocation denied | `FSIMG_ENOMEM`; no live charge or handle leak |
| Optional allocation denied with a documented bounded fallback | The owner's permitted fallback result; prove the hook fired and check the complete successful result and resource bounds |
| Resource reservation over configured limit | `FSIMG_ELIMIT`; exhausted resource identified, no unbounded fallback |
| Source validation reports change | `FSIMG_ESOURCE_CHANGED`; active source released, failed writer cannot emit again |
| Provider premature EOF or explicit I/O failure | `FSIMG_EIO`; no successful image result |
| Positive short transfer | Operation completes by retrying as required; exact final bytes |
| Zero-progress transfer before required bytes complete | `FSIMG_EIO`; finite termination |
| Sink error after confirmed partial transfer | `FSIMG_EIO`; error/progress preserves exactly the confirmed contribution |
| Metadata-storage I/O error | `FSIMG_EIO`; no successful report and scratch cleanup |
| Cancellation callback requests stop | `FSIMG_ECANCELLED`; incomplete operation and released transient resources |
| Required/forbidden metadata loss | Contract-specific refusal before successful planning; no fabricated loss acceptance |

An allocation hook firing does not universally require terminal failure.
[Shared metadata storage](design-storage.md#shared-metadata-representation)
permits opportunistic cache growth to fail while the operation continues by
bounded page replacement. A named fallback case identifies that owner contract,
proves the optional allocation was denied, and independently checks the full
successful output or reader/verification result, its live and peak resource
charges, and final cleanup. It does not assert identical I/O counts or scratch
usage to a run with more cache, since replacement can legitimately change both.
If replacement itself fails, its specific failure contract applies.

Allocation sweeps distinguish denial of a required allocation from a permitted
optional-allocation fallback. They cannot accept every `FSIMG_OK` or every
negative status indiscriminately. A successful injected run needs evidence that
it exercised a documented fallback, not merely that its hook fired somewhere;
a targeted owner test supplies that evidence when the aggregate sweep cannot
identify the allocation's role. Mandatory-allocation failures retain their
error and cleanup assertions. A no-fault final run and a successful fallback
run are distinct observations.

The expected status is fixed by the named operation and fault, including any
explicitly documented bounded fallback, not a broad "any negative result"
assertion. Error details are checked where contractual:
resource, operation, relevant path, layer attribution and output state. Human
prose and internal allocation counts are not frozen baselines.

Preflight rejection and failure after output begins are separate cases. The
first checks absence of writes and its specified output state; the second
checks incomplete output and confirmed progress. A durability failure after
publication can report `FSIMG_EDURABILITY` with a retained visible result; the
output coordinator's tests own that distinction. Common raw writer tests do
not equate every flush failure with a post-publication durability failure.

Only one cause is injected at a time except in explicit error-precedence tests.
A source fault case does not also rely on an undersized budget. Expected
terminal-state behavior follows the operation contract: a reader argument
error detected before work can preserve its current entry, while a fatal
reader error invalidates it. Tests do not impose "every error destroys every
object" as a convenience for the runner.

## Layer profiles and composition

A separate layer profile identifies its registry name, layer kind and explicit
supported modes. Coverage reconciles with `fsimg_layer_at()` and advertised
operation support. Each registered layer has one coverage row; mode IDs within
that row are unique and have independently declared read/write/verify
expectations. Registry coverage establishes operation coverage, while the
explicit mode list establishes witnesses for documented subprofiles that the
registry does not enumerate. It supplies a bounded raw disk recipe, explicit stack and
options, expected decoded length/selection and a small list of semantic modes.
It does not carry filesystem source-provider or metadata-loss callbacks.
Scheme, container and compression operation families remain separate named
scenario functions; a common layer table does not imply one universal API.

| Layer family | Common assertions |
| --- | --- |
| Partition scheme | Selected slice offset/length, bounded partition access, metadata and untouched gaps, scheme-owned identity |
| Disk container | Decoded bytes equal the raw recipe, encoded-output bounds, zero ranges, exact logical length and malformed/transport distinction |
| Filesystem-carrying container | Required filesystem and byte-order compatibility, absence of forbidden slots, exact payload boundaries |
| Compression | Decoded bytes, stream/spool accounting, bounded expansion, transcode failures and concatenation policy where supported |
| Output composition | Raw-intermediate ownership, output budget including simultaneously live storage, file/IO/stream failure and finalization |

Read-only container modes do not acquire writer tests by sharing a name with a
writable mode. Mode witnesses, such as supported DMG encodings or boot-image
machine settings, are explicit test data checked against their documented
support. A constrained payload uses a small independent materializer with the same
bounded-byte ownership rule as filesystem reader fixtures. Specialized
assertions beyond the common contract stay in the owning local test; they do
not discharge mandatory common coverage. If a registered operation cannot
reach its common scenario through the documented API with valid bounded
input, that is a missing fixture or a contract discrepancy to resolve, not a
reason to add phase callbacks or a prose waiver.

Byte comparison is streamed against a deterministic raw-disk recipe. Full
container output need not equal another implementation's encoding, and
compressed-byte reproducibility is tested only within the stated codec/version
contract. Decoded bytes must be exact. Checksums and raw malformed offsets
remain with their layer-local independent checkers.

Composition uses representative seams rather than every filesystem multiplied
by every scheme, container, compression and output mode. Every filesystem runs
its raw-volume contracts. Every layer runs its own modes on a suitable payload.
Explicit combinations cover translated slice boundaries, container-required
ImageFS, exact-volume padding refusal, gzip around a container and failure
during the final transcode. The seam table says which ownership boundary each
case exercises and its exact expected result. A new boundary requires a new
case; a new unrelated filesystem name does not multiply the whole table.

## Fixture bounds and independent evidence

Normal fixtures contain a small fixed tree with payloads crossing at least one
copy batch, a mixture of zeros and data, and a nested directory. Volume size is
an explicit legal per-profile choice: a FAT32 fixture need not have the same
length as FAT12 or QNX6. No test allocates a buffer proportional to an
unchecked plan length. The harness checks the length against the profile's
fixture ceiling and `SIZE_MAX` before allocation. Larger legal minimum volumes
use bounded callback/file storage and chunk comparisons rather than expanding
all fixtures into RAM.

Profile ceilings bound test apparatus separately from the library context.
Application-owned buffers are not falsely charged as library allocations.
Scale tests measure RSS as well as charged memory and enforce scratch and time
bounds; a tiny fixture passing resource assertions does not establish
asymptotic boundedness. Wide directories, long allocation chains,
fragmentation and huge sparse logical ranges remain scale/fuzz workloads with
explicit independent limits.

Independent evidence has three complementary forms:

* Handwritten expected trees, payload recipes and metadata values establish
  API semantics without consulting production capability/codec helpers.
* Format-local byte checkers and the Ruby models validate on-disk structure
  independently of fsimg's reader. Local malformed witnesses establish each
  format's corruption and unsupported-feature classifications.
* Integration scripts read images from other implementations and validate
  fsimg's output with them. FAT uses mtools/dosfstools and applicable native
  tools, exFAT uses exfatprogs, and the layer scripts use their documented
  partition/container/compression tools. QNX native qualification retains its
  own evidence and licensing boundaries.

Self-round-trip success cannot substitute for any of these categories. A
materializer reusing fsimg's writer is labeled accordingly. Checked-in bytes
carry provenance and permitted redistribution terms; use of an external tool
as an oracle does not authorize importing its code. Test execution stays
offline, including fixture generation and replay.

The Ruby harness continues to classify images with its independently authored
rules; it does not import the C profile's expected outcome or call production
validators. Sharing stable public format names and fixture descriptions is
useful, but sharing an expectation algorithm would remove independence.
Harness rule coverage and common API-contract coverage are reported separately.

## Build integration and regression discipline

`test-driver-contracts` and `test-layer-contracts` are ordinary Automake check
programs linked with `libfsimg.la`, with test-only profile/helper sources under
`test/`. They are in `test_programs`, hence `check_PROGRAMS` and `TESTS`, and
run under `make -j32 check` without optional tools. Distribution includes all
profile sources and required bounded fixtures. There is one profile inventory
per family; no second manually maintained list in a shell wrapper determines
which driver receives common tests.

A successful run reports exercised profile/scenario counts and conditional
cases not applicable. On failure it reports the stable case ID and a direct
replay selector. The selector is a test executable argument, not a public CLI
option. A targeted replay deliberately bypasses unrelated scenarios but the
ordinary full run always performs registry reconciliation. Build logs retain
the distinction between common-suite success, optional-tool skips and native
qualification evidence.

The commands and platform/sanitizer requirements in [build documentation](../docs/build.md)
apply without a separate test policy. Ordinary tests include these programs;
ASan/UBSan exercise them when their implementation or the covered library
changes requires those checks. Linux LSan/MSan, FreeBSD qualification, fuzzing
and scale runs remain separately reported. A passing C contract suite does not
turn an unavailable sanitizer or skipped independent oracle into a pass.

A nontrivial defect receives a named regression that fails against the unfixed
tree, followed by the fix and a passing rerun. Put the regression at the owner:
a common lifecycle defect becomes a shared scenario; a malformed FAT chain or
QNX inode record remains a local input with independent expectations. Promotion
of a repeated local test into the common runner preserves its concrete witness
and assertions before removing duplication.

Acceptance requires all registered capabilities to have executed coverage,
all mandatory scenarios to pass, fault cases to prove reachability, and the
independent/local checks to remain intact. Review examines the main scenario
functions for visible setup, ownership, assertions and cleanup. Table-driven
selection is useful only when a reviewer can explain a failing row without
tracing a framework protocol.

## Complexity balance and limits

The design adds two inventories of test facts, shared scenario functions and a
small set of bounded callback helpers. These prevent a likely regression:
adding a driver or capability without exercising the shared lifecycle and
failure contracts. They remove copied emission fixtures, handwritten format
loops for common scenarios and the obligation to remember which test programs
need another format string.

The design deliberately retains independent expected values, format-local
fixtures and separate layer scenarios. They are evidence with different
owners, not duplication to eliminate. Profiles cost maintenance when a public
capability or documented metadata policy changes; that failure is the desired
signal requiring an independently reviewed expectation change.

There is no per-driver scenario operation table, inheritance hierarchy,
callback-owned lifetime, generic mutation language or runtime registration.
If a profile needs custom setup and teardown at every phase, its case belongs
in the local test rather than enlarging the common interface. If extracting a
helper merely hides the same resource obligations from each caller, the
extraction does not satisfy this design.

Common contract coverage proves behavior for its witnesses. It does not prove
all legal geometries, all interleavings, native interoperability, or every byte
of a driver's parser. Those remain explicit obligations of the local tests,
independent harnesses, sanitizers and bounded fuzz/scale work.
