# Image emission

Emission turns a sealed plan into image bytes on a sink. Format drivers decide
what goes where. The common writer owns everything else: the order of
pre-mutation checks, sink initialization, payload copying and its source
lifecycle, and the finish of the sink. Drivers express metadata writes and
payload copies. They never hold the raw sink or open a source.

[Storage](design-storage.md#emission) states the bounds. The
[sources design](design-sources.md) defines source identity and reopening.
Driver designs ([FAT](design-fat-writer.md), [exFAT](design-exfat.md),
[QNX6](design-qnx6.md)) define only placement and encoding.

## Owners

| Owner | Owns |
| --- | --- |
| Writer lifecycle (`lib/write/`) | State transitions, pre-mutation checks, sink initialization, flush, failure marking of the output, the [link-target read](#encoded-link-targets), the [measured read](#measured-reads-and-encoded-copies) |
| Emission object | Bounded metadata writes, the source copy operation, the [encoded copy](#measured-reads-and-encoded-copies), the transfer buffer, progress |
| Driver | Layout iteration, metadata encoding, placement: which bytes belong at which offset |

## Order

`fsimg_writer_emit()` performs these steps; each completes before the next
begins:

1. Seal and plan the catalog. Planning refuses an output whose bound, plus
   the raw intermediate of an encoded output ([layer
   design](design-layers.md)), exceeds the output limit; when a [measured
   read](#measured-reads-and-encoded-copies) plans the layout, an output
   whose least volume already exceeds the limit is refused before any
   content is read. Planning then checks the
   digests a manifest claims for generated content, as the [manifest
   design](design-mtree.md#digests) states. The raw intermediate of a bare
   stream is charged when the stream is written, before any byte is emitted.
2. Reject a sink that aliases any collected host source, using the backing
   identity rule in the [architecture](architecture.md#output-and-determinism).
   The same inode is rejected even when two slices look disjoint, because
   finalization can change the whole file's length.
3. Check sink capabilities: write, resize or exact fixed size, and durability
   when requested. An output encoded from a raw intermediate checks its own
   sink here as well, before the raw disk is emitted into the intermediate.
4. Resize a resizable whole-object sink to the planned length.
5. Zero every byte of the planned volume through the zeroing operation,
   which writes only where zeros are not already proven. On a fresh file,
   one fsimg adopted empty, the operation writes nothing: the file's
   [written-range record](design-holes.md#the-written-range-record) proves
   every unwritten range zero, and keeps proving it for the ranges later
   writes do not touch, so those ranges stay sparse without any guess from
   names or sizes and whatever order the writes come in. Only two facts
   establish that a file is fresh: exclusive empty creation, which the
   output coordinator performs for the staged output, or the adoption check
   of a [handed-over file](design-layers.md#handed-over-files), the raw
   intermediate and the spool, which proves on the descriptor that it is an
   empty regular file with no link. Any other sink, a reused file or a
   caller's positional object, has no record and is zeroed explicitly.
6. Construct the emission object and pass it to the driver.
7. Flush the sink after the driver returns successfully.

Steps 2 and 3 precede any mutation, so a rejected alias or capability never
resizes, zeroes, or writes the sink. Only step 5 establishes that the
destination holds zeros, and the emission object exists only after it succeeds.

## Emission object

The emission object binds the writer, the bounded destination, the writer's
session, and one transfer buffer. It borrows the writer's lifetime and session.
It is not another resource context, source owner, or handle.
Drivers receive it only for the duration of their emit operation. A driver may
encode metadata in the transfer buffer between operations; a payload copy
overwrites it.

It offers two operations:

* **Write metadata:** write encoded bytes at a volume offset. The operation checks
  the range against the planned volume length and charges work. Placement
  correctness, such as whether a record belongs at that directory slot,
  remains the driver's responsibility. A range that fits the volume is not
  thereby correct.
* **Copy payload:** copy catalog entry `id` into its planned regions: an
  ordered list of `(image offset, capacity)` ranges that the driver supplies
  in logical order, together with the file length the plan records and the
  planned initialized length. FAT and exFAT files have one region; an empty
  file has none; a fragmented QNX6 file has several. The driver may pass an
  observer, a callback `(argument, bytes, length, volume offset)` that the
  copy invokes over every byte it writes, once per transfer batch in the
  file's logical order, which is volume order only within one region, and
  never for a ZERO range or an empty source. A format
  whose trailer depends on the payload, such as ImageFS's additive
  checksum, keeps its own running state in the argument; common code
  holds no checksum algorithm and makes no promise about batch sizes, so
  an observer that needs word alignment keeps the partial-word phase
  itself. The observer runs inside the copy's work accounting and cannot
  fail: it records, and the driver judges after the copy returns.

## Source copy

The copy operation owns the complete source lifecycle for one entry: open by
stable catalog ID, check the planned length against the catalog and the planned
regions' total capacity, copy across the regions in order, validate at finish,
and close. Every region must lie within the planned volume. A planned length
that differs from the catalog, or a payload exceeding its regions, is a corrupt
plan. A driver cannot pair a region with
a different entry's source or leave a source open.

* The planned initialized length cannot exceed the declared length or the
  total capacity. Bytes from the initialized length to the end of the last
  region stay zero.
* DATA ranges are copied in bounded batches. A source that ends before its
  declared length fails the copy; it is never a short success. A change that
  validation detects is reported as a source change.
* ZERO ranges, zero sources, and empty files write nothing. Skipping is valid
  only because step 5 established that the destination is zero. Empty and
  zero-only sources are still opened and validated, so a changed source fails
  even when no bytes move.
* Confirmed I/O progress is preserved in errors. Cancellation propagates.
* The current source is released on every outcome, including errors in the
  middle of a batch.

## Encoded link targets

Most formats store a symbolic link's target verbatim through the payload
copy. UDF encodes path components in a body or inline area, APFS stores an
inline attribute, and Rock Ridge encodes it inside directory
metadata as `SL` components, whose sizes depend on the target's bytes:
the number of components and which of them are `.`, `..` or the root. Such
a driver needs the target while it plans and again while it encodes the
records. The writer offers it one operation for both: read entry `id`'s
target into a caller buffer of `FSIMG_SYMLINK_TARGET_MAX` bytes.

The operation owns the source lifecycle exactly as the copy does: it opens
the source by stable catalog ID, requires a symbolic-link entry, reads
exactly the catalog's declared length, which collection bounded, validates
the source and closes it on every outcome. It works during planning and
during emission, never holds a source between calls, and charges the read
to the session. A target whose bytes change between the planning read and
the emission read is a changed source when validation detects it; when
the change keeps the recorded metadata, the driver compares the record
sizes it encodes with the plan's and fails with `FSIMG_ESOURCE_CHANGED`
on a difference, so a plan never describes records of another size. A
driver that stores the target verbatim through the payload copy never
needs this operation.

A failure closes the source and fails the planning or emission step that
asked, and the writer with it, under the
[source failure rule](design-sources.md); a failed plan publishes no losses
and no summary.

## Measured reads and encoded copies

A format that stores a file as an encoding of its bytes, such as EROFS's
LZ4 clusters, needs the encoding's size before it places anything, and the
encoding itself when it emits. Staging the encoded payload would hold data
the plan cannot bound, so the writer offers two operations that run the
driver's encoder over the source twice, once to measure and once to store;
the encoder's determinism makes the two agree.

* **Measured read:** stream entry `id`'s content to a driver consumer, a
  callback `(argument, bytes, length)` that may fail. It opens the source
  by stable catalog ID, requires the catalog's length to equal the length
  the plan records, a corrupt plan otherwise, and delivers every byte of
  `[0, length)` once, in logical order, in batches of at most the emission
  buffer's size: DATA ranges are read exactly, and ZERO ranges arrive as
  zero bytes, since an encoder needs them, with one work unit charged per
  zero batch as each read charges one, an I/O transfer or computed
  content alike. After the last byte the consumer
  receives one call with no bytes, which marks the end of the content so
  it can finish its last unit; an empty source receives only that call.
  The source is then validated and closed; a consumer failure or a failed
  read closes it at once. The read works during planning and during
  emission, never holds a source between calls, allocates its own batch
  buffer for each call and charges every read to the session, under the
  source failure rule above. A driver that measures during planning first
  states the least volume its plan can reach whatever the measurements
  find, and the writer refuses the plan when that volume, held once by the
  disk and again by a raw intermediate, already exceeds the output limit:
  every output's bound holds its disk and every disk its volume, so the
  full check after planning would refuse it too. An image whose least
  volume fits but whose final bound does not is refused by the full check
  once planning ends, after its measured reads.
* **Encoded copy:** during emission, a measured read whose consumer is the
  driver's encoder, together with the regions the plan gives the encoding
  and the number of bytes the plan says it stores. The regions are checked
  before the source is opened, as the copy checks its own. The encoder
  hands each finished unit to the emission object, which writes it at the
  next position of those regions in logical order, calls the driver's copy
  observer over it as the copy does, and refuses a unit that would pass
  the planned stored length; when the content ends, a stored length short
  of the plan's fails the copy. Both are `FSIMG_ESOURCE_CHANGED`: planning
  measured the same bytes with the same deterministic encoder, so other
  output means other bytes. The driver compares each unit it forms with
  the one planning recorded and fails the same way on a difference, so a
  source whose bytes changed under the same length and identity still
  cannot produce output the plan does not describe. Batches come from the
  emission buffer, which the emission object does not hand out while the
  copy runs.

Common code holds no encoder and knows nothing of units beyond their bytes:
the consumer is the format's encoder and the driver owns every comparison
with its plan. Neither operation adds a source lifecycle of its own; both
are the copy's open, range walk, exact reads, validation and close, with
the destination replaced by a consumer.

## Failure

Every failure from step 4 on marks the output incomplete. A caller-supplied
sink then holds partial output and must be discarded. Emission is single-use
even after failure; a retry uses a new writer and a new sink. The output
coordinator owns publication and durability of files it creates.

The emission object serves the concrete FAT, exFAT, and QNX6 layouts. It is not
an emission instruction language. Plans stay driver-owned and immutable, and
there is no second plan representation.
