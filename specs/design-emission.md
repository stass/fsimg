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
| Writer lifecycle (`lib/write/`) | State transitions, pre-mutation checks, sink initialization, flush, failure marking of the output |
| Emission object | Bounded metadata writes, the source copy operation, the transfer buffer, progress |
| Driver | Layout iteration, metadata encoding, placement: which bytes belong at which offset |

## Order

`fsimg_writer_emit()` performs these steps; each completes before the next
begins:

1. Seal and plan the catalog.
2. Reject a sink that aliases any collected host source, using the backing
   identity rule in the [architecture](architecture.md#output-and-determinism).
   The same inode is rejected even when two slices look disjoint, because
   finalization can change the whole file's length.
3. Check sink capabilities: write, resize or exact fixed size, and durability
   when requested. Check the planned length against the output limit.
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
  file has none; a fragmented QNX6 file has several.

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

## Failure

Every failure from step 4 on marks the output incomplete. A caller-supplied
sink then holds partial output and must be discarded. Emission is single-use
even after failure; a retry uses a new writer and a new sink. The output
coordinator owns publication and durability of files it creates.

The emission object serves the concrete FAT, exFAT, and QNX6 layouts. It is not
an emission instruction language. Plans stay driver-owned and immutable, and
there is no second plan representation.
