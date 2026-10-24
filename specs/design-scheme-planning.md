# Scheme-owned disk planning

A partition scheme resolves its own disk settings, partition metadata, and
identity rules. Common disk planning places the resulting partitions, fits
volumes to fixed disks, composes container bounds, and checks output budgets.
Create, wrap, and compose reach the same scheme operations. Neither common
planning nor the create and wrap adapters select settings by comparing a
scheme name or treating every scheme other than GPT as MBR.

The [layer design](design-layers.md) owns stack semantics, GPT and MBR profiles,
partition types, identifiers, and emitted bytes. This design owns their private
planning interface. The [settings catalogue](design-settings.md) still owns
option representation, syntax, presence, and applicability. The
[container-plan design](design-container-plans.md) owns container state; the
[storage design](design-storage.md) and [API design](design-api.md) own budgets,
bounded I/O, public lifetimes, and output behavior. Public structures, worker
wire records, accepted profile combinations, and native qualification gates do
not change as a consequence of this interface.

## Ownership and the size of the change

| Owner | Responsibility |
| --- | --- |
| Settings catalogue and shared option checks | Bounded text, typed values, presence, operation scope, wrong-layer settings, compose disk-only settings |
| Scheme write operations | Effective sector size, requested disk size, disk identity, option-to-partition normalization, type and name conversion, partition identity, scheme-wide constraints, scheme report fields |
| Common disk plan | Selected descriptor, charged record allocation, slot order, placement, fixed-disk fit, exact-volume rule, final disk and encoded lengths |
| Filesystem writer or input identification | Volume length, filesystem identity for type defaults, format geometry and native qualification |
| Scheme encoder | Defined table bytes at planned offsets, on-disk field encoding, checksums, bounded table buffers |
| Output and resource owners | Aliases, source consistency, cancellation, accounting, sink adoption, staging, finalization, publication |

The write boundary uses the bounded `fsimg_scheme_table_t` and
`fsimg_partition_record_t` representation already shared with GPT and MBR
encoding and reporting. It does not introduce an opaque extension allocation,
reference counting, a settings map, or a partition graph. GPT writes at most
128 records and MBR at most four. Both plans fit the existing record model;
neither needs a second representation of the same GUID, name, or active flag.
The table's `kind` describes that closed representation. Common placement does
not interpret it.

The read model remains the explicit GPT/MBR model: probes, decoders, verification,
type matching, and name selection retain their contracts. A write dispatch
boundary is not an assertion that the reader supports arbitrary schemes.
Adding a genuinely different read representation requires its own evidence
and design. A synthetic writer descriptor tests extensibility of planning
without advertising a third on-disk format.

For comparison, the model with scheme branches makes `plan.c` responsible for
GPT disk GUIDs, MBR identifiers, GPT names, active-flag legality, duplicate
GUIDs, and format-specific disk diagnostics. `writer.c` and the wrap path in
`output.c` must separately know which option fields supply a partition.
Changing a scheme rule can therefore require coordinated changes in all three
places. In the descriptor model those rules have one scheme owner, while
common placement still has one algorithm. The added concepts are a small
resolved-facts record and three semantic operations plus report projection;
the removed obligation is keeping scheme-specific interpretation synchronized
across entry points. No new public state or owned object is added.

`plan_disk` removes the disk setting and identifier branch in `fsimg_disk_plan_init`; `plan_partition`
removes both callers' field selection and the type/name/GUID/active branch in
`fsimg_disk_plan_partition`; `validate_plan` removes the common GPT collision
check and MBR-specific maximum-size diagnostic. `report_plan` removes the
remaining scheme identity branch in planned reports. Each operation has a
different input lifetime or completion point; none creates an additional
state. Closed read-side codecs and type selectors remain explicit two-kind
operations because this split gives them no new responsibility.

## Private interface

The following sketches state the data flow; these names describe private C
interfaces, not an installed ABI. `fsimg_scheme_ops_t` retains its read
operations and its existing footprint and encode operations. It gains the
planning operations below. There is no separately selected writer identity.
Every writable scheme supplies the complete planning/encoding set. A read-only
scheme has no write set and is refused with `FSIMG_EUNSUPPORTED` naming it
before any planner callback or output mutation.

Capability reporting uses this same write-set presence: `fsimg_layer_next`
advertises `FSIMG_CAP_WRITE` exactly when the scheme has a complete write set.
It does not infer writing from the presence of read operations. The set consists
of `plan_disk`, `plan_partition`, `validate_plan`, `report_plan`, `footprint`,
and `encode`, with a positive slot capacity and maximum disk-sector count.
A registered descriptor supplies either all these operations or none; static
registry tests establish that invariant, so normal dispatch needs one presence
check, using `plan_disk`, rather than a repeated completeness protocol. Read
and verify capability bits continue to reflect their respective supported
operation contracts. GPT and MBR advertise the same capabilities as before.
A partial write set is a descriptor-definition defect, not a supported
read-only profile or a reason to advertise a partially usable writer.

```c
typedef struct fsimg_scheme_plan_args {
	const fsimg_layer_options_t *layers;
	int64_t		 epoch;
	uint32_t	 flags;
} fsimg_scheme_plan_args_t;

typedef struct fsimg_scheme_plan_facts {
	uint64_t	 requested_disk_bytes;
	const char	*disk_key;
} fsimg_scheme_plan_facts_t;

typedef struct fsimg_scheme_partition_args {
	const fsimg_layer_options_t *layers;
	const fsimg_partition_request_t *part;
	const char	*filesystem;
	int64_t		 epoch;
	uint32_t	 flags;
	uint32_t	 index;
} fsimg_scheme_partition_args_t;

/* Members of the scheme's private operations record. */
fsimg_status_t (*plan_disk)(const fsimg_scheme_plan_args_t *,
    fsimg_scheme_table_t *, fsimg_scheme_plan_facts_t *, fsimg_error_t *);
fsimg_status_t (*plan_partition)(const fsimg_scheme_partition_args_t *,
    fsimg_partition_record_t *, fsimg_error_t *);
fsimg_status_t (*validate_plan)(const fsimg_scheme_table_t *, uint64_t,
    fsimg_error_t *);
void (*report_plan)(const fsimg_scheme_table_t *, uint32_t,
    fsimg_image_info_t *, fsimg_partition_info_t *);
```

`plan_disk` receives already checked layer options. It resolves sector size,
table representation kind, disk identity, and the requested fixed disk length.
It returns `disk_key` as a pointer to the scheme's static canonical setting
name, such as `gpt:disk`; common fixed-size diagnostics use that value without
selecting a scheme. The requested length is zero for automatic sizing. Common
code consumes the facts immediately into its existing length/fixed fields and
one static diagnostic-key pointer. It does not retain a duplicate requested
length after initialization.

`plan_partition` combines normalization and resolution: a non-NULL `layers`
means create/wrap settings, while a non-NULL `part` means one compose request.
Exactly one is non-NULL. This is a two-case input contract, not an extensible
request language. `filesystem` is the borrowed canonical filesystem name when
one is known, or NULL; `index` is the one-based output slot. The owner reads
only its own option members in the settings case and the supplied partition
fields in the compose case. Its local normalization produces the same type,
name, GUID, filesystem, and active inputs to one resolver. There is no separate
normalization callback or lasting normalized object.

The common adapter rejects an invalid combination of `layers` and `part` as
`FSIMG_ESTATE`; entry points construct the two permitted cases directly. The
scheme operation writes metadata into a fresh zeroed record, whose index
common code has assigned. It does not write placement fields. It sets `used`
only after all metadata succeeds. A failure terminates construction; callers
do not retry into the partially resolved record.

`validate_plan` receives the complete, placed table and final disk length,
including container rounding. It performs scheme-wide identity and field
representability checks and produces scheme-specific diagnostics. It does not
move records, change length, allocate, read inputs, or encode tables. It is
where GPT rejects duplicate unique GUIDs and MBR rejects a final disk beyond
its addressable range. Fixed-disk limits that are decidable at `plan_disk`
still fail there, preserving early refusal; final validation also covers
automatically sized disks.

`report_plan` fills only scheme fields in an initialized image report and the
selected partition report. Its slot is one-based and checked by the common
caller. GPT and MBR use the existing partition-report conversion with an
explicit matching kind. Common code supplies disk/file lengths, compression,
container, and selected scheme name; the callback supplies sector size, slot
capacity, selected slot, disk identity and partition metadata. It does not
reinterpret a missing scheme as MBR. It is infallible over a successful plan
and retains no report pointer.

The existing descriptor facts remain ordinary data: `slots`,
`max_disk_sectors`, and `footprint(sector, &leading, &trailing)`. The footprint
returns leading and trailing sector counts for a sector size accepted by
`plan_disk`. It is pure, independent of disk size and volume contents, and
performs no allocation. Format-specific maximum-disk diagnostics belong to
`validate_plan`, rather than a common error string mentioning MBR for every
scheme. The descriptor's static bounds and each scheme's resolver are tested
together; common code checks arithmetic when converting those bounds to bytes.

## Request normalization and timing

The common disk initialization sequence is visible and has immediate error
checks between stages:

1. Establish the operation's existing session, clear the destination plan, and
   validate stack, compression, options, and applicability.
2. Record common file-setting identities and lengths. Reject an exact-volume
   filesystem beneath a scheme before resolving container settings.
3. Construct the container plan; reject a padding container for an exact
   volume; resolve outer compression settings.
4. Select the scheme descriptor. With no scheme, leave the table absent and
   continue through the existing unpartitioned path. Otherwise reject a
   read-only scheme and a count outside `1..slots`.
5. Call `plan_disk`, check its resolved sector and fixed-size constraints,
   and allocate the bounded record array through the context. Explicitly
   initialize records and assign indices `1..count`.
6. Resolve partition metadata and obtain volume lengths at each entry point
   as described below, place volumes, calculate encoded size bounds, validate
   the final scheme plan, and check output budgets at the existing planning
   boundaries.

The sequence retains established first-failure precedence for container
settings, disk settings, partition settings, geometry, and output budgets.
When several independent arguments are invalid, the shared settings catalogue
still determines structural/applicability order. Moving interpretation into a
scheme does not silently reorder those checks.

For direct create, `plan_layers` supplies layer options and the stack's
filesystem identity to the single-partition wrapper. The wrapper dispatches
`plan_partition` with `layers` and no compose part. It contains no ternary over
`gpt_type` and `mbr_type`. The filesystem resolves its sector size before a
fixed-disk fit; an automatically fitted size is then revalidated by its writer.
Filesystem-specific native qualification remains in that writer and its
existing capability checks.

For wrap, the existing input checks establish length and a claimed filesystem
where required. A supplied partition type permits the existing explicitly
typed unrecognized-input behavior; it does not bypass a named filesystem or
carried-volume requirement. Common wrap passes the resulting identity or NULL
and layer options to the same single-partition wrapper. It does not select
GPT/MBR fields itself. Without a scheme, the input is the disk and no partition
metadata operation runs.

For compose, common checks require a named scheme and no filesystem in the
stack, reject partition settings in outer layer options, validate input
context membership, and preserve part order. Each part supplies its type,
name and GUID. The public `fsimg_part_t` has no active member, so compose
normalization supplies false; adding an active setting is not part of this
private refactoring. An absent or empty type requests filesystem identification;
an explicit type avoids that default-type probe under the existing contract.
The adapter passes a compose request and no layer-options pointer to
`plan_partition`. Error attribution uses `partition N`. It never merges outer
partition options into a part.

The request, its strings and GUID pointers, and its layer-options record are
borrowed only for synchronous resolution. A successful record contains copied,
validated metadata and retains none of their addresses. This is separate from
the existing longer borrow of volume I/O and file-setting I/O objects through
output. Scheme planning has no access to those I/O objects or host paths.

## GPT metadata and identity resolution

GPT's disk resolver accepts a sector size of 512 or 4096 bytes, with zero
selecting 512, and reads `gpt_disk_bytes`. It copies a nonzero disk GUID
verbatim. An unset disk GUID is derived in reproducible mode and otherwise
fails as `FSIMG_EINVAL` at `gpt:disk-guid`. A requested fixed length must be
whole sectors; common code also checks container granularity.

GPT normalizes layer partition inputs from `gpt_type`, `gpt_name`, and
`gpt_guid`. Compose supplies the same semantic fields from its part. The
shared partition-type catalogue remains the single list for write defaults,
read selectors, and reporting; GPT owns interpreting its GPT column:

1. A nonempty type is parsed as a known name, GUID literal, or MBR-byte
   literal. An omitted/empty type looks up the supplied filesystem identity.
   No known filesystem/default is an error naming the type or compose part.
2. An MBR-byte literal is invalid for GPT. A named type without a GPT GUID,
   including `qnx4`, is invalid unless another explicit type was selected.
   A nil literal type GUID is invalid because it marks an unused entry.
3. Copy the selected type GUID in textual byte order. The default name is
   the named type's conventional GPT name; a literal GUID uses the first
   matching catalogue row's name, or the empty string if none matches.
4. A nonempty explicit name replaces that default. Validate UTF-8 and convert
   to at most 36 UTF-16 units, allowing surrogate pairs to consume two units.
   Zero-fill the rest. Empty names retain the existing default behavior;
   they do not introduce a distinct explicit-empty setting.
5. An active flag is invalid. Copy a supplied nonzero unique GUID, derive an
   unset GUID in reproducible mode, or fail naming `gpt:guid`/the part.
6. Mark the record used after all conversions and identity checks succeed.

GUID derivation remains the common pure `fsimg_identifier_derive` primitive:
cast the epoch to its 64-bit unsigned representation, store its eight bytes
little-endian in textual GUID byte order, store role in byte 8 and slot in
byte 9, clear bytes 10 through 15, replace byte 6's upper nibble with 4, and
replace byte 8's top two bits with binary `10`. Disk role is 1 with slot zero;
partition role is 2 with its one-based slot. GPT's maximum written slot 128
fits the existing byte-sized derivation input. Negative epochs retain their
unsigned bit representation. Explicit identifiers are not normalized to a
particular GUID version and are not replaced in reproducible mode.

The GPT encoder alone converts textual GUID bytes to GPT's mixed byte order.
The planner does not pre-encode GUIDs or derive them from output data, clock,
randomness, addresses, host paths, or budget state. The same epoch and logical
options yield the same metadata through create, wrap, and compose.

Final validation compares each used partition GUID against the disk GUID and
against earlier partitions in slot order. At 128 entries the bounded pairwise
comparison needs no set, hash table, scratch stream, or ownership protocol.
A collision reports the existing one or two partition indices and fails before
output mutation. The common planner does not need to know which schemes have
unique identifiers.

## MBR metadata and identity resolution

MBR fixes sector size to 512 and reads `mbr_disk_bytes`. A supplied identifier
uses `mbr_id` verbatim when `mbr_id_set` is true, including explicit zero. The
shared settings check rejects a nonzero inactive value when the flag is false.
An absent identifier becomes the low 32 bits of the epoch in reproducible
mode and otherwise fails at `mbr:id`. A fixed disk exceeds the profile when
its sector count is greater than `2^32`; exactly `2^32` sectors is permitted.

MBR normalizes layer partition inputs from `mbr_type` and `mbr_active`.
Compose can also supply name and GUID fields, which it validates instead of
dropping. Nonempty names and nonzero GUIDs are invalid; empty names and unset
GUIDs remain absent. The error attribution retains the existing setting/part
contract. The type resolver uses the same catalogue and lexical parser as
GPT, but interprets the MBR column:

* GUID literals are invalid. A known name needs a nonzero MBR byte and writes
  that row's first byte; read aliases do not change this default.
* A literal byte must be nonzero and use the established `0xNN` syntax.
  Extended types `0x05`, `0x0f`, `0x85`, and protective type `0xee` cannot be
  written. They require layouts outside the writer profile.
* A missing type needs a filesystem catalogue entry with an MBR byte. The
  normalized active flag is copied, then the record is marked used.

The first-byte rule preserves FAT16 `0x06`, FAT32 `0x0c`, QNX6 `0xb2`, and
QNX4 `0x4d`. Read selection still accepts all aliases listed by the catalogue;
for example `qnx6` matches `0xb1`, `0xb2`, and `0xb3` without changing the
written default or claiming that all such bytes uniquely identify QNX.
MBR final validation checks disk sector count and each encoded first-LBA/count
field before narrowing to 32 bits. It does not add a uniqueness rule for MBR
partition types, active flags, or disk identifiers across separate images.

## Footprint, placement, and exact size

GPT's written table has 128 entries of 128 bytes regardless of used count.
For sector size `S`, its entry array occupies `16384 / S` sectors, leading
footprint is `2 + 16384 / S`, and trailing footprint is `1 + 16384 / S`.
The values are 34/33 at 512 bytes and 6/5 at 4096 bytes. MBR declares one
leading sector and no trailing sectors. The same scheme-local footprint
helper serves planning and encoding so they cannot independently choose how
much space the tables need.

Common placement uses byte offsets and half-open ranges. Given sector size
`S`, leading/trailing counts `L` and `T`, alignment `A = 1048576`, and volume
lengths in slot order:

1. Checked-multiply `L * S` and `T * S`; initialize the cursor to the leading
   byte length. Never assume multiplication is safe because GPT/MBR constants
   happen to be small.
2. For each nonempty volume, round the cursor up to `A`, round volume length
   up to `S`, and checked-add start and rounded length. Store that offset and
   slot length and advance the cursor. Zero-length partitions fail naming
   their slot. Sector padding is zero, separate from the input volume bytes.
3. Checked-add trailing bytes to the cursor. A fixed disk must hold this
   minimum length. An automatic disk rounds it up to the resolved container
   granularity. Rounding uses remainder and checked addition, not unchecked
   `value + unit - 1`.
4. Require the final disk length to be a whole number of scheme sectors. The
   supported container granularities already produce this result for GPT/MBR;
   a descriptor cannot rely on truncating a partial final sector. Fixed disks
   separately require both sector and container divisibility.
5. Compose container and compression size bounds, then invoke final scheme
   validation with the final length, preserving the established ordering of
   bound refusals before final identity checks. The trailing table range ends
   at that length, not at the unrounded cursor. Apply common output accounting
   after successful placement and validation.

All divisors are validated nonzero before use. All products, additions, and
conversions to `size_t` or host offsets are checked at their owning boundary.
Common placement verifies final containment using subtraction after ordering,
not a possibly overflowing endpoint. MBR's `max_disk_sectors` is `2^32`, not
`UINT32_MAX`; a nonzero first LBA and that disk bound make the maximum
partition count representable, and explicit field checks document this fact
before emission. GPT has no additional practical sector-count cap below the
checked byte range; its encoder still validates usable LBAs and field widths.

For a single partition in a fixed disk, common fitting computes
`start = round_up(L * S, A)` and `end = disk_bytes - T * S`, checking each
operation and requiring `end > start`. The longest volume is
`floor((end - start) / filesystem_granule) * filesystem_granule`. A zero
requested volume becomes that exact length; a supplied volume larger than it
fails. A zero result is too small. This fitting operation does not allocate
space to compose inputs or resize wrap inputs. They keep their given lengths
and must fit when placed.

An exact-volume filesystem remains incompatible with any scheme and any
container whose granularity can pad its disk. Compression does not lengthen
the decoded slot. The common stack check owns this rule, including its
filesystem/layer diagnostic. Scheme callbacks cannot waive it because a
particular input happens already to be sector-aligned. A fixed disk is likewise
a constraint, never a suggestion to expand or silently shrink it.

## Failure, lifetime, and emission boundary

The common disk plan is cleared before any fallible construction. A selected
scheme descriptor is static and outlives the plan. Its planning operations are
allocation-free and retain no arguments. Common code alone owns the charged
record array; `fsimg_disk_plan_free` frees it once and clears its pointer and
count, alongside the independent container-plan cleanup.

A failed disk resolver may leave scalar fields filled but no owned scheme
allocation. A failed partition resolver may leave partial metadata only in
its destination record. Neither is exposed as a successful plan or passed to
reporting/encoding. No per-record destructor is needed because records contain
only values. Allocation failure, partial metadata failure at any slot, final
validation failure, and output failure all use the same owner cleanup. Moving
a disk plan transfers its record pointer and clears the source, as for its
container pointer; two live owners never share it.

Initialization, partition resolution, placement, and validation are phases of
the existing writer/operation lifecycle, not a second mutable scheme state
machine. The caller invokes emission only after the complete disk plan
succeeds. After successful placement and validation, records and resolved
facts are immutable. Encoders receive const tables and final disk length;
they do not repair metadata or resolve omitted options during output.

All argument-determined refusals occur before output resize, zeroing, or
writing. Sink descriptor adoption can precede planning under the existing
public ownership contract; adoption does not permit mutation. The interface
does not promise that every later resource failure precedes sink adoption or
output: table-buffer allocation, I/O, cancellation, changed sources and flush
failures retain their ordinary failure semantics. The existing output path
owns the timing of table encoding, staged-file cleanup, and publication.

Encoders retain defense checks for an internally inconsistent table and return
`FSIMG_ESTATE`, rather than emitting truncated fields. A valid user request
that exceeds a profile limit fails during planning with its established
argument/range class. All encoded table bytes, unused records, GPT attributes,
boot-code areas, reserved fields, and gaps remain defined or zero. The
`fsimg_scheme_tables_t` two-region bound remains sufficient for these profiles;
no generic list of arbitrary table fragments is introduced.

Scheme operations receive neither a sink nor ambient host authority. They do
not allocate unbounded metadata, open a source, publish a file, reset a session,
or bypass the common alias check. The context accounts for records and encoded
tables separately; output accounting continues to include a raw intermediate
when present. Payload length never determines resident scheme metadata size.

## Diagnostics, reports, and selection

The common diagnostic helper names the scheme's own setting key, such as
`gpt:type`, for a layer-option partition request, or `partition N` for
compose. Formatting a selected name is
not dispatch on that name. The scheme chooses which setting failed and the
specific explanation; `disk_key` provides fixed-disk attribution. Errors retain
`FSIMG_OP_IMAGE`, structured limit/observed fields for excess count, and existing
status distinctions. Invalid options use `FSIMG_EINVAL`; arithmetic overflow
uses `FSIMG_ERANGE`; wrong internal sequencing uses `FSIMG_ESTATE`; resource
and I/O failures propagate without relabeling.

Common planning never prints messages, reveals host paths in scheme errors,
or includes arbitrary raw option bytes. The bounded key and relevant slot
indices suffice. Shared structural checks establish fixed-array termination
before normalization; compose strings retain their public borrowed-string
contract, and UTF conversion checks encoding before copying to records.

Planned reports keep their public meaning: file length is the planned raw disk
length, disk length is separate from volume length, selected slots are
one-based, and partition count describes available scheme slots (128 for GPT,
four for MBR), not merely supplied parts. GPT reports disk/type/unique GUIDs and
UTF-8 names; MBR reports its disk identifier, byte type and active flag. Common
reporting adds compression and container facts without reading scheme metadata.

`types.c` keeps the common catalogue, selector parser and read-side match/name
operations. Scheme-specific assignment is invoked explicitly by GPT and MBR,
through narrow GPT/MBR helpers or equivalent scheme-local code over that
catalogue; common disk planning no longer calls a scheme-kind switch to assign
metadata. This does not duplicate the catalogue into one table per scheme.
The read-side closed switch is appropriate to its closed representation and
need not become another callback family merely to remove a write branch.

Round-trip report equivalence checks planned metadata against reopened output.
Selection still compares GPT names in stored UTF-16 units, uses the documented
type aliases, rejects unsupported MBR extended selection, and creates checked
read-only slices. Writer normalization cannot change read ambiguity, corrupt
image handling, selection scope, or replacement-character display of invalid
names in externally created GPT tables.

## Extension acceptance without inventing a format

A test-only synthetic descriptor exercises the common write path through a
private test injection point, without entering the production registry or
public CLI capability list. It uses one supported table representation and
its own distinctive diagnostic key, slot bound, footprint, option default,
final-limit rule, and report callback. Its encoder can emit deterministic
bounded test table bytes or delegate to a compatible existing profile when
that profile's geometry is used. The test explicitly states which it does;
it does not claim independently valid new disk bytes.

The synthetic plan proves that common code:

* Selects operations from the supplied descriptor rather than a GPT string
  comparison or an MBR fallback.
* Receives disk and partition defaults only from that descriptor, places
  according to its declared footprint, and attributes a fixed-size failure
  using its key.
* Calls final validation after container rounding, routes report projection,
  and releases common allocations on failures at every callback boundary.
* Needs no new scheme-name branch in `plan.c`, `writer.c`, or the wrap/compose
  setup in `output.c` to pass these cases.
* Derives write capability and write admission from the same complete-set
  invariant. A read-only synthetic descriptor with the write set absent
  reports no write capability and fails creation before any planner callback
  or output mutation; registry validation rejects a deliberately incomplete
  set. These fixtures exercise the shared capability projection without
  registering test schemes in production.

The synthetic descriptor is compiled test data, not untrusted runtime code.
Tests deliberately large footprints exercise checked multiplication and
addition without requiring enormous image allocation. They also ensure a
partial final sector fails instead of producing a shortened encoded disk.

A real third scheme is accepted only with a specified profile, independently
validated images, explicit metadata/report/selection semantics, and native
qualification where required. The synthetic test establishes only the planning
boundary. If that scheme needs a different record model, more table regions,
or noncontiguous placement constraints, its concrete requirements justify a
separate change. This design does not preemptively introduce private scheme
heaps, arbitrary partition trees, geometry solvers, or an open-ended read ABI.

## Verification matrix

Tests assert behavior and byte identity through real entry points as well as
the focused descriptor seam. Fixtures establish expected on-disk fields
independently of the planning implementation.

| Subject | Required cases and observations |
| --- | --- |
| Normalization | Create and wrap with each scheme; compose with explicit and inferred types; NULL/empty defaults; wrong-layer options; compose outer partition settings; no scheme invokes no scheme planner |
| GPT sector and count | Default 512, explicit 512/4096, invalid sector, 1/128 partitions accepted, 0/129 rejected with count/limit; read limit remains independently 1024 |
| GPT type and name | Known names, known/unknown GUID literal, nil type rejected, MBR literal rejected, QNX4 missing default rejected; 36 UTF-16 units accepted and 37 rejected; supplementary characters and malformed UTF-8; deterministic zero padding |
| GPT identity | Explicit identifiers preserved; unset identifiers refused outside reproducible mode; negative/zero/positive epochs; slots 1 and 128; duplicate partition GUID and partition equal to disk GUID refused before mutation |
| MBR metadata | Explicit zero disk ID with presence accepted; absent and inactive-value cases; all first-byte defaults and read aliases; no-byte names, GUID literals, zero, extended and protective bytes rejected; nonempty name/nonzero GUID rejected; active flag preserved for single-partition creation |
| Placement | One and multiple volumes, exact 1 MiB boundary, odd byte length rounded to sectors, zero-length partition, fixed disk exact fit and one sector short, large footprint crossing alignment, zero trailing footprint, gap and tail zeros |
| Fixed fitting | Filesystem granule differs from scheme sector, absent versus explicit volume size, no room for one granule, overflow/underflow, revalidation of fitted filesystem size |
| Container interaction | GPT backup at final disk end after VHD/simg rounding; 4096-byte GPT sectors with allowed wrappers; incompatible fixed granularity; exact-volume refusal retains timing; automatic final size checked by scheme validation |
| Limits | MBR disk at `2^32` sectors and one sector beyond; maximum representable field values; checked leading/trailing multiplication, round-up and addition; record allocation failure; output/raw-intermediate aggregate limits |
| Failures and ownership | Failure before record allocation, at first/middle/last partition, during final validation and encoding; cleanup twice at common wrapper level is safe; no option/string pointers retained; no double free on plan transfer; no output mutation for planning refusals |
| Reporting and selection | Planned versus reopened reports; total slots versus used count; GUID textual byte order; GPT name and type selection; MBR aliases; unpartitioned disk behavior; diagnostic key and partition-number stability |
| Synthetic extension | Descriptor-owned defaults and reports, non-GPT/MBR name, altered footprint and cap, rejection after rounding, common cleanup; complete/absent/partial write-set capability coherence; no production registration or extra read enum |
| Byte compatibility | Reproducible GPT/MBR reference outputs through create/wrap/compose, with and without each compatible container/compression; headers, CHS, checksums, unused entries, padding and derived identities unchanged |

Independent gates use the layer profile's `sgdisk`/`gdisk`, `sfdisk`/`fdisk`,
`mtools`, `qemu-img`, and applicable `hdiutil` checks, including reading images
those tools create. Native QNX qualification is not inferred from a type GUID
or successful self-round-trip. Missing tools and platforms are named as
unavailable, not counted as passing. No new format or combination is advertised
on the strength of the synthetic descriptor.

Implementation validation follows [the build contract](../docs/build.md): plain
suite, the common driver-contract scenarios, allocation-failure coverage, and
full ASan/UBSan for changed records, arithmetic, buffers, and object lifetimes.
Supported Linux leak/memory sanitizer checks and platform gates retain their
requirements. The coordinating session runs sanitizer/fuzz suites on the
integrated tree. Review must explain how the common placement path can be read
without knowing GPT or MBR options, and how scheme-local resolution makes every
metadata and failure decision visible; passing byte comparisons alone do not
establish that the ownership split is understandable.
