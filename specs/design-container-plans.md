# Container-owned immutable plans

A container owns the resolved settings needed to encode a disk. Its descriptor
constructs one private plan with a small common summary, supplies a bound for
encoding the final disk length, and destroys the complete plan. The disk plan
owns that allocation; an encoder only borrows it. This is the same ownership
model as the filesystem writer's base-first private plan.

The [layer design](design-layers.md) owns the stack, format profiles, sizing,
file settings, and transcode behavior. This design specifies their internal
planning boundary. Public options, handles, writer states, image bytes,
identifiers, error classes, and reporting retain their existing contracts.
There is no public container-plan handle or additional lifecycle state machine.

## Ownership and representation

| Owner | Responsibility |
| --- | --- |
| Container descriptor | Construct resolved private state, validate profile settings, bound the encoded disk, encode framing, destroy private allocations |
| Common disk plan | Select the container, own its plan, retain file-setting records, place volumes, derive final disk length, compose encoded bounds and reports |
| Common input handling | Record input backing identities and lengths before reads, reject output aliases, check source changes |
| Transcode | Borrow the plan, allocate the transfer buffer, schedule passes, charge work, poll cancellation, close encoder state on every outcome |
| Resource context and operation session | Account for plan and encoder memory, metadata scratch, work, and limits under the existing shared budgets |
| Output coordinator | Adopt and validate sinks, stage, finalize, publish, and report output outcomes |

`fsimg_container_plan_t` is an internal base structure. Each writable container
embeds it as the first member of its private plan type, in one context-charged
allocation. Only that container's functions cast the base pointer to the
extension. Common code neither computes extension offsets nor switches on a
container name to interpret its fields. The selected descriptor supplies the
constructor, bound operation, encoder operations, and destructor as one write
contract; a read-only descriptor supplies none of them.

The disk plan keeps the selected descriptor and one owned plan pointer. It
does not copy the extension or retain a second ownership pointer for any
extension buffer. The descriptor's lifetime is the static registry's lifetime.
A plan needs no independently selected operation table, reference count, or
public identity of its own. Destruction dispatches through the same descriptor
that constructed it.

Installing a prepared disk plan into a writer transfers ownership, rather
than copying it into two live owners. The receiving disk plan takes the
container pointer and other owned members; the source is cleared before its
cleanup can run. The private plan stores no pointer to its enclosing disk
plan or its file-record array, so this transfer cannot invalidate private
references. A shallow copy with both instances still owning resources is
invalid.

The common base holds only facts used outside the container:

| Fact | Meaning and consumer |
| --- | --- |
| Disk granularity, in bytes | Nonzero unit used by common placement to round the disk and by the exact-volume rule to reject padding containers |
| Transfer chunk size, in bytes | Nonzero, profile-bounded unit for the common transcode buffer and chunk traversal |
| Pass count | Positive number of complete, ordered disk inspections required by the encoder; supported profiles use one or two |
| Container identifier | The resolved 16-byte identifier exposed by the existing image report; all zero when the profile has no such identifier |
| Resolved codec dependency | Whether these output settings depend on system zlib, for the existing reproducibility version report |

The base does not contain VHD timestamps, DMG compression levels, simg CRC
flags, boot addresses, or a generic buffered file setting. These are private
encoding facts. An identifier already present in the base is the encoder's
identifier too; there is no second private copy to keep synchronized.

The codec dependency describes the selected encoding, not all encodings a
reader supports. DMG with zlib declares it; DMG with raw chunks does not.
Common reporting combines that fact with the outer compression's dependency
and reports the system zlib version once. It never infers a codec from a
nonzero private compression level. The contract does not require a new codec
registry or public report field.

The final encoded-size bound belongs to the common disk plan, alongside the
final disk length. It is the result of a pure operation on the immutable
container plan and that length, not a field filled into the container plan
later. Disk-dependent report facts likewise come from the placed disk plan.
Information known only after inspecting or encoding payloads is not invented
by planning; existing plan-report semantics remain in force.

## Construction and partial failure

The constructor accepts the context, a borrowed operation session, validated
layer options, recorded file-setting inputs, write flags and epoch, and the
carried volume's resolved byte order. These inputs have the meanings in the
layer design. Common
settings validation determines applicability; the container resolves its own
defaults, identifiers, profile constraints, and encoding choices once.

Before calling it, common code records the file-setting objects' lengths and
available host identities, checks context membership and applicability, and
rejects an incompatible carried-filesystem stack. The exact-volume rule
rejects a scheme before container resolution and rejects a padding container
once its resolved granularity is available. Constructor and bound refusals
occur before any output sink opens or is mutated.

The constructor's result pointer starts NULL. Before any fallible work with
an allocated extension, it initializes that extension to a destructible empty
state and attaches its base pointer to the result slot. All subsequent owned
allocations are attached to that extension as they are acquired. A failure
can therefore leave either NULL or a partially constructed plan; common code
uses the same disk-plan cleanup on both success and failure. A failed
constructor never makes the plan available to a bound, encoder, or report
consumer. Its partial pointer is solely for cleanup.

The destructor accepts every partial state the constructor can return. It
releases all extension-owned buffers and the base allocation exactly once,
without reading borrowed input objects, closing them, emitting bytes, or
performing fallible finalization. Common disk-plan cleanup skips a NULL
pointer, calls the selected destructor otherwise, and clears its owned
pointer. No common cleanup code knows that `ifsboot` owns a prefix buffer.
Only the common disk-plan owner invokes that destructor for a returned
partial or complete result. Constructors do not free an attached result and
leave its pointer behind. A NULL disk-plan member is a valid cleanup input;
the descriptor destructor itself is called only with a non-NULL allocation.

Successful construction freezes the base and extension. Bound calculation,
reporting, and encoding receive const pointers. Options and constructor
argument records are not retained by address; needed scalar values are
resolved into the plan. Mutable checksums, pass counters, compression state,
and scratch streams belong to the encoder invocation, not the plan.
The context outlives the allocation and accounts for its memory. Common code
establishes the operation session before container construction and passes it
explicitly for file-setting reads. Those reads use session-aware bounded I/O,
so even a caller-supplied I/O object with no bound session charges each
transfer to the operation. They also charge byte-processing work according to
the existing work model. Common code does not bind or replace the session on
the caller's I/O object. The constructor does not retain the borrowed session
or the argument record after returning.

For create, the writer and its session exist at a stable address before
layer planning. That same session continues through collection, planning,
and emission; construction failure destroys the unpublished writer and its
partial disk plan. For wrap and compose, the common entry point owns a
session spanning container construction and output. The assembled disk borrows
that session instead of initializing a fresh output session. Encoding uses
the same counters and deadline for work and metadata scratch. No construction-
only budget, session reset at output, or copy of a live session is involved.
Moving a disk plan does not move its operation session, and a plan neither
retains a temporary session nor opens its own scratch directory.

## File settings and borrowing

The common disk plan retains `fsimg_planned_file_t` records for all supplied
file settings: borrowed I/O objects, recorded lengths, and available host
identities. The private constructor reads those records for validation; the
encoder receives them from the common owner. The private plan need not retain
their array's address. The records and plan remain valid throughout encoding.

A bounded file setting, such as an `ifsboot` prefix, is copied once into
context-charged private plan memory. Its profile's small fixed maximum bounds
the allocation. The constructor validates the copied bytes and initializes
all padding; the encoder emits that same immutable buffer. A streamed file
setting, such as startup code, is never copied into plan memory: planning
checks its length, and encoding reads its bytes in bounded pieces at the
profile-defined position.

The public borrowing period is unchanged: file-setting objects remain valid,
read-only, and unchanged from writer construction through the output call,
or through the synchronous wrap or compose operation that borrows them.
Buffering a bounded input does not shorten that obligation or remove its
common alias and change checks. A successfully planned writer retains its
plan until destruction, including after emission failure; freeing that plan
does not dereference input objects whose public borrow has ended.

Common input handling compares available host identities after copying and
before successful completion or publication. It rejects same-inode output
aliases regardless of slice ranges. For opaque callback objects, stability
and disjoint storage remain the caller's explicit obligation. An early EOF
in a streamed setting is `FSIMG_ESOURCE_CHANGED` naming the setting; encoding
does not pad the missing payload into an apparent success. These checks are
not an atomic snapshot against a hostile concurrent writer.

## Placement and encoded bounds

Container settings are resolved before the disk length is final because
common placement needs the granularity. Common code sizes the filesystem,
places partitions, checks fixed disk constraints, rounds the disk, and places
trailing scheme tables according to the layer design. A container never owns
partition placement or decides how much filesystem space is available.

After placement, the bound operation receives the immutable container plan
and the final disk length. It validates disk-dependent field limits and
returns a checked upper bound in bytes for the complete container, including
headers, tables, padding, and trailers. It performs no input reads, output
writes, clock reads, plan mutation, or new allocation. Repeating it with the
same arguments produces the same result. The disk length must satisfy the
resolved granularity; arithmetic overflow and unrepresentable profile fields
remain explicit failures with the profile's established attribution.

The common owner stores this bound, applies the outer compression's bound,
and checks the aggregate output budget, including a raw intermediate when
one is required. This order preserves pre-output rejection of a disk too
large for simg block counts, DMG tables, VHD lengths, or `ifsboot` size fields.
The final sink and each compression filter enforce their respective bound
on every append. Exceeding an accepted bound is an internal error, never
permission to write beyond the reserved output.

Without a container there is no private allocation and no destructor call.
Common access to the composition facts supplies granularity 1, a 1 MiB
transfer chunk, one pass, an absent identifier, and no container codec
dependency. The container byte bound is the disk length, and the existing
copy encoder supplies the sequential path when needed. Outer compression
still has its own settings and bound. This is absence of a layer, not a
synthetic registered container with another lifecycle.

## Encoder contract and deterministic passes

The encoder opens with the immutable plan, recorded file settings, final disk
length, bounded sequential sink, operation session, and existing fact that
the disk is a verified wrap input. It may retain these borrowed objects for
its invocation, but not the address of a temporary argument record. It owns
only its mutable encoding state and resources acquired through the shared
context and session. Closing an encoder never frees the plan or its buffers.

Common transcode initializes encoder state to NULL before open. Every non-NULL
state returned by open, including on failure, is safe for the matching close
operation. Common code closes it on every outcome before the plan can be
destroyed. A close releases scratch streams and compression resources without
silently completing a failed encoding.

Common code visits the full disk in order for each declared pass, using the
same resolved transfer size and a shorter final unit when needed. Every unit
charges work and observes cancellation, including proven zero units. A NULL
chunk buffer means the unit's logical bytes are all zero; a host hole is only
an optimization for establishing that fact under the I/O contract. Disk bytes,
not hole discovery, budgets, or scratch placement, determine output bytes.

An earlier pass may inspect chunks and its finish may emit framing whose
values it establishes. Only the final pass emits encoded disk chunks. The
encoder retains learned counts, lengths, and integrity accumulators in its
mutable state and checks the profile's agreement conditions across passes.
It never writes those observations into the plan. simg counts chunks before
emitting its header; compressed `ifsboot` learns framed size before emitting
its leading size fields. Their existing source-change and corruption checks,
including the distinction between a verified input and a staged writer disk,
retain their error classes and timing.

Known invalid arguments and disk geometry are rejected by construction or
the bound before output. Resource failures, I/O failures, source changes,
and payload-dependent integrity failures can occur during encoding. They
follow the existing incomplete-output and staged-file cleanup rules; a
private plan does not make publication transactional for caller-owned sinks
or streams.

## Profile coverage and common callers

| Profile | Private resolved state | Shared summary and bound |
| --- | --- | --- |
| Fixed VHD | Creation timestamp and footer-specific choices | Resolved alignment, identifier, one pass, disk plus footer bound |
| DMG | Resolved compression mode and level | 512-byte granularity, resolved chunk size, identifier, explicit zlib dependency, one pass, data and table bound |
| simg | Block size and CRC choice | Block granularity, 1 MiB transfer unit, two passes, checked block/chunk fields and complete bound |
| `ifsboot` | Machine and header settings, startup length, owned padded prefix | Byte granularity, 65536-byte transfer unit, one or two passes by mode, checked startup/body/framing bound |
| `hbcifs` | No writable plan | Read-only refusal before output |

The common defaults are initialized explicitly when constructing a writable
plan; profile constructors replace only the facts their resolved settings
determine. Existing default values, machine qualification, identifier
derivation, timestamp resolution, compression settings, chunk boundaries,
zero padding, and byte order remain those of the layer profiles.

Create, wrap, and compose use the same constructor, placement/bound operation,
and destructor through the common disk plan. Create supplies its resolved
filesystem facts and stages a disk when required. Wrap supplies the verified
input facts the carried-volume contract requires. Compose retains common
partition and input handling. None duplicates container defaults or creates
a private alternate plan. Drivers do not gain source reopening, ambient host
paths, sink adoption, scratch policy, or publication authority.

## Validation and complexity

Contract coverage exercises VHD's fixed framing, DMG's compression choices,
simg's two passes, and `ifsboot`'s owned and streamed settings through their
applicable create, wrap, and compose paths. It includes:

* Allocation failure before and after extension publication, prefix read and
  validation failure, bound refusal, encoder-open failure, cancellation, and
  emission failure, with one destruction and balanced resource accounting.
* Prefix reads through an unbound caller I/O object, including short transfers,
  charge the operation session; accumulated construction work remains charged
  during output, and changing phases does not restart its deadline.
* A retained successful writer plan after output failure, and destruction
  after borrowed file-setting lifetimes end, without accessing those objects.
* Alias and source-change checks for buffered and streamed settings, short
  reads without synthesized payload tails, and callbacks recording that
  invalid settings or bounds cause no output open, resize, or write.
* Repeated bound/report queries, defaults and the no-container path, codec
  version reporting for raw DMG, zlib DMG, and outer gzip combinations.
* Identical bytes for equivalent inputs and resolved settings across budgets
  and zero-range knowledge, exercising every supported mode and pass count,
  with poisoned padding buffers and independent decoding checks under the
  layer profiles' acceptance tests.

The design adds one private allocation and a constructor/destructor contract
per writable container. It removes the shared aggregate of unrelated format
fields, common boot-buffer cleanup, and common interpretation of compression
levels. The disk plan still owns one container choice and one lifetime; the
encoder still owns invocation state. No reference counting, second plan owner,
mutable finalization stage, recursive layer graph, or new public object is
needed. Review checks that a new container's private state and cleanup fit
inside its driver without adding fields or format-specific branches to common
planning, transcode, or reporting.
