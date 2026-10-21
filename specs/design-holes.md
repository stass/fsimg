# Hole knowledge

fsimg skips zero bytes in two places. A writer leaves ranges of a fresh output
file unwritten, so they stay holes on a host that keeps holes. A transcode or
a sequential copy spares the read of a range it knows is zero. Both rest on
one question an I/O object answers about itself: *which of my bytes are known
to read as zero?* This design states where that knowledge comes from, how it
stays within bounds, and what each kind of object answers.
[Storage](design-storage.md#byte-io) owns the byte I/O contract this refines,
[emission](design-emission.md) and the [layer design](design-layers.md)
consume it, and the [C API](design-api.md#positional-io) exposes the one
callback through which an application contributes knowledge of its own.

## Owners

| Owner | Owns |
| --- | --- |
| Bounded I/O (`lib/io/`) | The written-range record of a fresh file, the zero-range operation, the translation of both through slices, zeroing that writes only where the record cannot prove zeros |
| Container adapter (`lib/layers/chunks.c`) | The zero-range answer of a container's disk, from its chunk index |
| Assembled disk (`lib/layers/output.c`) | The zero-range answer of a wrap or compose disk, region by region |
| Compatibility layer (`compat/host.c`) | The one host hole query, `SEEK_HOLE` then `SEEK_DATA`, for files fsimg did not write |
| Transcode and sequential sink (`lib/layers/transcode.c`) | Consulting answers to spare reads, never to decide bytes |
| Application | The optional `next_zero_range` callback of a positional object it supplies, and the truth of its answers |

## Problem and evidence

A hole in a host file is a storage fact: the filesystem keeps no blocks for
the range and returns zeros when it is read. fsimg needs a contents fact: that
the range reads as zero. The two coincide only when the filesystem's report is
right about every byte, including bytes written moments ago and not yet on
disk, and `SEEK_HOLE` has been wrong there. OpenZFS reported a hole over
written, unsynced data when a dnode looked clean during transaction-group
sync while dirty records were still pending (OpenZFS issue 15526, fixed by
pull request 15571 in releases 2.2.2 and 2.1.14; the fault reproduced on
2.2.0, 2.2.1 and 2.1.x on FreeBSD 13 and 14). A later report against 2.2.3,
issue 15933, turned out to be a different defect in the same interface:
pages written through a memory mapping were not flushed before the hole
query, which upstream confirmed and fixed in pull request 16019. Two
independent faults in two years make the point: the report is only as right
as every path that dirties a file, and fsimg cannot audit them. The
documented remedy for each is the upgrade. `fsync` commits to the intent log
and is not documented to close either race. A transcode that trusted such a
report would encode written blocks as zeros and publish a corrupt image
whose checksums are all consistent.

fsimg does not need the host's report for the files it writes. Those files
are proven empty when fsimg adopts them, every byte they hold afterwards was
written through one I/O object, and that object can remember what it wrote.
For the disks fsimg reads through its own tables, a container's chunk index
declares zero runs outright. The remaining case, a file some other program
wrote, has no such knowledge of its own; there the host's report is used,
after a sync that narrows the known fault, as [Decisions](#decisions)
states.

## Central construction

Every I/O object answers one question about itself, **the next zero range at
or after an offset**, from what it is:

* a file fsimg created, from the **written-range record** its root keeps:
  every range not written is zero, because the file was empty when it was
  adopted;
* a slice, from its root, translated and clipped;
* a container's disk, from its chunk index: zero records, and fill records
  whose value is zero;
* an assembled disk, region by region: its own zero gaps, and for an input
  slice whatever that input answers;
* a caller's positional object, through an optional callback;
* a read-only file fsimg did not write, an external file whichever
  constructor built its object, from the host's hole report, after syncing
  the file once, bounded by the length the query observes and never for the
  chunk holding its last byte;
* anything else, nothing.

Zeroing a range of a writable object means making it read as zero. The
zeroing operation's authority to omit a write rests on one kind of
knowledge: a fresh file's record, which says the range was never written
into a file proven empty with fsimg its only writer. On such a file zeroing
writes zeros only over the granules its record marks possibly nonzero; on
every other object it writes zeros or invokes the caller's zero-range
callback for every byte. A zero-range answer, the host's or a callback's,
spares reads and never a zero write, so the risk accepted for the host's
report below cannot leave a stale byte in a reused output. Two other
consumers leave zeros unwritten on their own proof rather than through
zeroing: the spool filler and the extractor, each writing a fresh file it
knows to be zero wherever it has not written, as their entries under
[Consumers](#consumers) state.

Three obligations disappear under this construction. No consumer tracks
whether an object is still "known zero": the record is per range, so writing
one range does not make another range's zeros cost anything. No consumer
orders its writes to keep zeros cheap: the scheme emitter fills a staged disk
region by region in disk order, zero gaps included, and a gap costs nothing
whenever it comes. No object asks the host whether a file fsimg wrote has
holes, so the files fsimg writes are never synced for the host's benefit and
their answers do not depend on which filesystem holds them; the host is
consulted only where fsimg has no knowledge of its own.

## The written-range record

### Which objects keep one

A root keeps a written-range record when, and only when, two facts hold: the
file is empty at adoption, and fsimg is its only writer for the object's
lifetime. The first is checked on the descriptor; the second is a contract of
the trusted side that hands the file over, since no descriptor check can
exclude another descriptor or a mapping of the same file. Two adoptions
provide both, and nothing else does:

* **A fresh output**, created exclusively by the owner of its directory: the
  output coordinator's staged `image`, the worker's staged image, and each
  file the extractor creates. Adoption checks a regular file of length zero;
  exclusive creation in a private directory is what makes fsimg the only
  writer.
* **A handed-over file**, the spool and the raw intermediate: adoption checks
  a regular file of length zero with link count zero, open read/write
  without `O_APPEND`, as the
  [layer design](design-layers.md#handed-over-files) requires; the creator
  keeps no other descriptor or mapping of the file and gives none to anyone,
  which that design states as part of the handover.

Every other file root is an **external file**: a file some other program
wrote, or may still write. The common descriptor constructor builds every
file root, the public `fsimg_io_from_fd`, which duplicates a borrowed
descriptor, and the private adoption of a reserved descriptor alike, and a
root is external unless its adoption attaches a record or marks it scratch.
The CLI's image input, a wrap or compose input, a positional sink an
application supplies and the writer's source files are all external files
by that rule; how they are built does not enter into it. An external file,
a callback object and a resolver have no record, their unwritten ranges are
not known to be zero, and the zeroing operation writes real zeros through
them. What an external file knows instead comes from the host, under
[the host's report](#external-files-the-hosts-report).

### Representation

The record is a bitset over **granules** of the root's length, one bit per
granule. A set bit means *possibly nonzero*: some byte of the granule has
been the target of a write, or the granule inherited a set bit from a finer
one when the record was folded. A clear bit means every byte of the granule
is zero. The record is conservative in one direction only: it may call a
zero granule possibly nonzero, never a written granule zero. It lives in
memory charged to the context and is bounded by a constant, not by the
file: at most 1 MiB, that is 2^23 bits. When the record is created, and
whenever it must grow, the granule is the smallest power of two from 4 KiB
upward at which the whole length fits in that many bits:

| Root length up to | Granule | Record |
| --- | --- | --- |
| 32 GiB | 4 KiB | length / 32768 bytes, up to 1 MiB |
| 64 GiB | 8 KiB | 1 MiB |
| 1 TiB | 128 KiB | 1 MiB |
| 128 TiB | 16 MiB | 1 MiB |

The allocation grows with the length, in whole bytes, up to the cap: an
extracted 1 MiB file costs 32 bytes, a 256 MiB disk 8 KiB. The cap is policy:
the smallest budget the library supports, 16 MiB, holds the records of a
staged output and a raw intermediate at the cap beside the page cache's
first chunk. Granularity is a cost, not a correctness property. Marking is
conservative by whole granules: a one-byte write marks its 4 KiB granule
possibly nonzero. Zeroing writes only the intersection of the requested
range with the possibly-nonzero granules it meets, never a byte outside the
request, so what the coarseness costs is the zeros written over the
unwritten remainder of a marked granule inside the request. A transcode
reads the whole of every chunk that intersects a possibly-nonzero granule,
since chunks are not split, so a one-byte write costs a 1 MiB read under the
default chunk, whatever the granule. A coarser granule widens both by its
size; the 4 KiB granule, the host block, covers every file up to 32 GiB.

A bitset rather than a list of extents, because drivers write metadata in
many small scattered writes, and a list that stays sorted under that pattern
needs an ordered structure in memory or a sort before each query, while a
bitset marks and answers in bounded time whatever the order. Charged memory
rather than a metadata store, because the record never exceeds its cap, so
it needs neither the page cache nor scratch, and because it belongs to
objects that exist before any session does.

### Marking

Every write through the root, directly or through any slice, marks the
granules covering the *requested* range before the host or the callback is
invoked. A failed write and a short write therefore leave their whole
requested range marked: the record says "possibly nonzero", which is the
truth after a write whose effect is unknown. Zeroing never marks: a range
that reads as zero after zeroing and a range never written are the same to
every consumer. Reads never mark.

### Resize

A resize is prepared, then committed, so that a host truncation that fails
changes nothing:

* **Growth.** The object first prepares the record for the new length:
  it charges and allocates whatever more bytes the record needs, and when
  the new length does not fit at the current granule it builds the folded
  record beside the old one, doubling the granule until the length fits,
  each new bit the OR of the pair it replaces. Then it truncates the file to
  the new length. Only when that succeeds does it commit: the prepared
  record replaces the old one and the new granules beyond the old length
  are clear, since the host extends a file with zeros. A fold is
  conservative: a coarse granule whose halves were one set and one clear
  bit is now possibly nonzero in full, so the zero half may later be
  zeroed or read though it never held data; no granule holding written
  bytes becomes clear. Folding happens at most once per doubling of the
  length, so a spool growing by bounded steps to 64 GiB folds a few times
  over at most 1 MiB each.
* **Shrinking.** The object truncates the file first, and once that
  succeeds clears the bits of the granules lying wholly beyond the new
  length, so a later growth, which the host zero-fills, is clear there. A
  granule straddling the new length stays as it was: if it was possibly
  nonzero, it remains so, and after a later growth its bytes above the old
  length are zero on the host but still inside a possibly-nonzero granule,
  which costs a read or a zero write of at most one granule and is never
  wrong. The granule itself never becomes finer again: the record keeps the
  granule it has, and the "smallest granule" rule applies only when the
  record is created or must grow.

A resize that fails leaves the record describing the file as it is: a failed
growth has changed neither the file nor the committed record, and the
prepared record is discarded; a failed shrink has changed neither.

### Zeroing

`fsimg_io_zero_range` and its session form zero a range by writing zeros only
over the possibly-nonzero granules the range intersects, clipped to the
range, in bounded batches through the ordinary write path. Clear granules
are skipped. On an object without a record every byte is zeroed through the
zero-range callback or a charged zero buffer; zeroing never consults the
host's hole report or a `next_zero_range` callback, which describe what an
object reads as and not what fsimg may leave unwritten. A failure after any
zero has been written reports incomplete output.

So the writer's zeroing of the whole volume writes nothing on a fresh staged
file, and the scheme emitter's zero gaps cost nothing after the volume and
the tables are written, because those gaps were never written.

## The zero-range operation

### Contract

```c
fsimg_status_t fsimg_io_next_zero_range(fsimg_io_t *io, uint64_t offset,
    uint64_t *start, uint64_t *end, fsimg_error_t *error);
```

Given `offset <= length`, the object reports a range `[start, end)` with
`offset <= start <= end <= length` such that every byte in it reads as zero,
and promises nothing about bytes outside it. The object may stop looking
early: an empty range, `start == end`, says that no zero range the object
knows of begins before `start`, and the caller resumes there. Progress is
guaranteed: `start < end`, or `start > offset`, or `start == end == length`.
An object that knows nothing answers `start == end == length`.

Less is always correct, more never is. An answer is a promise about contents,
and the object answering is responsible for it: the record for a fresh file,
the sealed chunk index for a container, the regions for an assembled disk,
the application for a callback. The caller treats a reported range as zero
without reading it.

A consumer loops: ask; read `[offset, start)` as data; take `[start, end)` as
zero; continue at `end`, or at `start` when the answer was empty. The
transcode asks again only once its position passes `end`, so the number of
answers is about the number of zero runs, not of chunks.

### What each object answers

| Object | Answer at or after `offset` |
| --- | --- |
| Fresh file root | The next run of clear granules, clipped to `[offset, length)`; `start` is `offset` itself when it lies inside such a run |
| Slice, read-only slice | The root's answer for `base + offset`, clipped to the slice and translated back; over an external root, clipped further to end before the slice's own last byte |
| Container adapter | From the chunk index: the record containing `offset`; a zero record, or a fill record whose value is zero, yields the run through consecutive such records; any other record yields the empty answer at its end |
| Assembled disk | The region containing `offset`: a zero gap yields the gap; a table yields the empty answer at its end; an input volume yields that input's answer, translated into the disk and clipped to the region |
| Callback object | Its `next_zero_range` callback when present, validated against the contract; otherwise nothing |
| External file, read-only | The host's hole report for the file, `SEEK_HOLE` then `SEEK_DATA`, clipped to the length `fstat` then observes and to end before the object's last byte, once the file has been synced; `FSIMG_ESOURCE_CHANGED` when the observed length is below the declared one; nothing once the sync has failed or the host lacks the interface |
| External file, writable | Nothing: a sink is never asked for reads |
| Scratch object | Nothing; it is never asked |

A container's answer costs record reads through the index's cached-record
and successor path, so a sequential consumer crosses each record once. A
container that declares a vast zero disk in a few records is answered in a
few record reads, never in bytes. Those reads go through the sealed record
store and the context's bounded page cache, as every chunk lookup does: a
cold cache or a spilled index can grow the cache within its budget, write
back a dirty page and read scratch, and a failure there, an I/O error or a
limit, fails the answer and with it the operation, as the same failure
fails a read. An assembled disk's answer is one region lookup and at most
one input answer; adapters stack at most two deep, so there is no recursion.
A resolver object, the internal construction behind the container adapter
and the assembled disk, takes its zero-range function beside its resolve
function; both are library code.

### External files: the host's report

An external file, as [defined above](#which-objects-keep-one), is one fsimg
did not write: the image a reader opens, the input a wrap or compose reads, a
positional sink an application supplies, a writer's source file. fsimg knows
nothing of its contents and has no proof to rest an answer on. A read-only
external object answers from the host's report, `fsimg_host_next_hole` in
the compatibility layer: `lseek(SEEK_HOLE)` from the offset, then
`lseek(SEEK_DATA)` from the hole's start, then `fstat` for the file's
current length, mapped onto the contract as the hole `[start, end)`. The
query moves the descriptor's file position, which positional I/O never
uses. A writable external object, a sink an application supplies, answers
nothing: nothing asks a sink for reads, and read-only is what makes the
object a source.

**The length check.** The end of a file is a virtual hole that `SEEK_HOLE`
reports and `SEEK_DATA` answers with `ENXIO`, not a range of readable zeros,
so inside the one host query the observed length bounds the answer: `end`
is the data offset found or the observed length, whichever is smaller. If
the observed length is below the object's declared length the file has
shrunk, and the lookup fails at once with `FSIMG_ESOURCE_CHANGED` naming
the input: a shrink seen at a lookup is never left for a later read to
find, since the file may be restored, data and all, before the copy reaches
the range the answer covered, and that read would then succeed over a
cached answer that calls the restored data zero. The check is local to the
query; it brings back no end-of-pass validation and no second sync.

**The tail rule.** An external object never reports any byte at or after
its own length minus one as zero: every nonempty answer is clipped to end
at most at `length - 1`, and when no eligible range remains the answer is
the empty one at `length`, so an empty object or a lookup at its last byte
answers nothing and never subtracts from a zero length. The chunk holding the object's last byte is never covered
by an answer and is always read, with an ordinary exact read. This applies
at each object's own end: a slice over an external root clips its root's
answer to its own range and then to its own last byte, and in a wrap or
compose each input's last chunk is read as the transcode reaches that
input's end, whether or not that end falls on a chunk boundary, since a
chunk not wholly covered is read. The rule catches truncation after the
lookup: a file still short when its tail chunk is read comes back short,
and the operation fails as the exact-read path fails today, so zeros the
host invented past a new end are never published. The cost is one extra
chunk read per external input.

**What is and is not detected.** fsimg detects the truncation it can
observe: a file shorter than declared at a lookup, through the length
check, or at the exact read of a raw file's or slice's tail, through the
tail rule. A shrink that is undone between those observations, outside any
lookup, is concurrent modification of an input, which fsimg does not claim
to detect for any read, sparse or not; a shrink after an input's tail has
been consumed is the same case, as it is for a read without any hole
knowledge. A container's logical tail may come from a zero or fill record
or from the decoded-chunk cache without touching the backing file, so
reading it proves nothing about the backing file's length; payload bytes
still come only from records, whose reads validate every offset against
the file, so a backing file that shrank yields a failed read or a corrupt
record, never invented payload.

Before its first lookup the object syncs the file, once; it is never synced
again. The sync is a **workaround**, not a fix: the OpenZFS fault above
reported a hole over data that was written but not yet synced, and the
fault's fix is the upgrade. Upstream does not document `fsync` as closing
the race, and OpenZFS's `fsync` commits the intent log without waiting for
the transaction group whose sync the race lives in, so this design claims
only that the sync narrows the window, not by how much, and not that the
report is then correct. The decision to accept that residual risk, and why,
is in [Decisions](#decisions).

The object's state for this takes one of three values and only ever moves
forward: **not synced**, its state when built, in which the first lookup
syncs before it asks; **synced**, once the sync succeeded, in which lookups
ask the host without syncing; and **off**, terminal, in which lookups answer
nothing and no sync or host query is ever made again. The object moves to
off when the sync fails, with `EINVAL` or `EBADF` on a descriptor the host
cannot sync, `ENOTCAPABLE` under Capsicum when the descriptor lacks
`CAP_FSYNC`, or an I/O error, `EINTR` being retried as the flush loop
retries it; and when the host lacks `SEEK_HOLE` or a query fails. No write
or resize changes the state. The failure is not an error: the
operation continues and every byte is read and compared, which is always
correct, and nothing in the result depends on the sync. This discovery sync
is its own internal call, not `fsimg_io_flush`: an explicit flush of an
output, which the writer requires and whose failure fails the operation,
and cancellation, which the operation polls, are outside this fallback and
unaffected by it.

Under Capsicum the descriptors that may be asked therefore carry `CAP_FSYNC`
beside `CAP_READ` and `CAP_SEEK`: the image input of a read job in the CLI's
worker, and in an embedding application whatever read-only descriptor it
hands to `fsimg_io_from_fd` and wants answered. A descriptor without the
right is still valid; its object moves to off at its first lookup.
The writer's source files are external files too, but the payload copy
takes a source's ZERO ranges from its storage kind or its provider and never
asks its I/O object for zero ranges, so source roots need no right for this;
the spool and every other fresh file never ask, so their rights carry none.

### The callback

`fsimg_io_ops_t` has one optional operation for this:

```c
typedef fsimg_status_t (*fsimg_next_zero_fn_t)(void *argument,
    uint64_t offset, uint64_t length, uint64_t *start, uint64_t *end,
    fsimg_error_t *error);
```

`length` is the object's declared length and the answer follows the contract
above. The operation requires `read_at`, since it describes readable
contents, and is independent of `zero_range`, which establishes zeros. The
library validates each answer's shape: a range outside `[offset, length]`, or
an empty answer that makes no progress, is `FSIMG_EIO` naming an
inconsistent callback answer, as an over-reported transfer is. The library
cannot validate the contents. A callback that reports a zero range over data
has its answer encoded, as a `zero_range` callback that does not zero has
its promise relied on: the trusted-code obligation of every callback in the
API, which the API states.

## Consumers

* **Emission.** `fsimg_writer_emit()` zeroes every byte of the planned
  volume through the zeroing operation, which writes nothing on a fresh file.
  The copy operation skips ZERO ranges, zero sources and empty files because
  that step established zeros, as the
  [emission design](design-emission.md#order) states. A source file on the
  host is an external file whose I/O object the copy never asks about zero
  ranges: a source's ZERO ranges come from zero storage or from a provider's
  own ranges, and its data is read in full.
* **Scheme emission into a staged disk.** The scheme emitter fills the disk
  from its assembled regions in one pass in disk order: a zero gap is zeroed,
  which writes nothing; the volume is emitted into a checked slice; a table
  is written. The result is sparse wherever the host keeps holes, in that
  order or any other.
* **Transcode.** Each pass proceeds one chunk per step, and every step
  charges the work counter one unit, which polls cancellation. Before
  reading a chunk the pass asks the disk object for the next zero range,
  once its position has passed the last answer. A chunk lying wholly inside
  a reported zero range goes to the encoder as a zero chunk without a read;
  every other chunk is read and tested for zeros, and its decision is made
  on its bytes. Consecutive zero chunks merge in the encoders, into one DMG
  zero-fill entry, one simg `DONT_CARE` chunk, one unwritten run of a VHD or
  a raw copy, as the [layer design](design-layers.md#transcode) states. A
  private test control makes the pass read every chunk, and the output is
  identical.
* **Sequential sink.** A run of zeros appended to a positional output is
  zeroed through the zeroing operation at the sink's offset: on a fresh
  staged output that writes nothing and the run stays a hole; on any other
  object zeros are written; on a stream zero-filled buffers are sent. The
  sink keeps no knowledge of its own about the object.
* **Spool.** The filler skips the all-zero 4 KiB blocks of each append,
  which stay holes. It does so on its own proof, not through zeroing: the
  spool is a handed-over file, empty at adoption with fsimg its only writer,
  so a block the filler never writes is zero. The sealed spool's read slice
  answers from the spool's record, so an unwrap or compose that reads a
  spooled image spares the reads of its unwritten blocks.
* **Wrap, compose and readers over a user's file.** An input volume or an
  opened image that is a read-only external file answers from the host's
  report after its one sync. The assembled disk passes that answer through
  for its input regions, so a sparse host file is wrapped or composed
  reading only its data where the host reports its holes, and in full where
  it does not or where host answers are off. Each input's last chunk is
  always read, under the tail rule, as the transcode reaches that input's
  end, so an input that shrank fails there with a short exact read.
* **Extraction.** Each extracted file is a fresh output: DATA ranges are
  written and ZERO ranges skipped as the reader reports them. The skip rests
  on the extractor's own proof, not on zeroing: the file was created
  exclusively and empty in a private directory and resized, so a range the
  extractor never writes is zero. Its record costs a few bytes per file and
  is never consulted; it exists because every fresh file has one, not
  because extraction needs it.

## Resource bounds

* The record of one root is at most 1 MiB of charged memory and grows with
  the file only up to that cap. A context holds a few fresh roots at once: a
  staged output, a raw intermediate, a spool, the extracted file in
  progress.
* An answer from a record scans bits from `offset` and stops at the end of
  the first clear run; a sequential consumer scans each bit once.
* A container's answer costs record reads, bounded as
  [chunk index](design-layers.md#chunk-index-and-cache) lookups are, and
  may use the bounded metadata page cache those reads go through, including
  its growth within budget and its scratch I/O; an assembled disk's answer
  costs one region lookup and at most one input answer.
* No answer reads payload bytes or decodes a chunk. Only a read-only
  external file's answer calls the host for the file: one sync per object,
  then two seeks and one `fstat` per answer. The tail rule costs one chunk
  read per external input beyond its data.
* **Discovery is not the whole cost.** Discovery costs the zero ranges a
  few records. The transcode's own cost stays linear in the declared disk:
  one charged and polled step per chunk, whether the chunk is read or taken
  as zero from an answer, and for gzip, which has no zero entry, the
  compression of the logical zeros, and for a copy to a reused sink or a
  stream the writing of them. A hostile DMG declaring a 1 PiB disk in one
  zero record therefore costs compose a billion steps, each charged. The
  work limit and the deadline, which are trusted settings, bound that; the
  output limit does not, since its default is the planned length and the
  plan trusts the declared disk. A compose under a work limit fails with
  `FSIMG_RESOURCE_WORK` at the chunk whose charge crosses it.

## Security

* A zero write is skipped only on a fresh file, where the record owns the
  proof: the adoption check establishes that the file starts empty, a
  descriptor fact rather than a caller's word, and the trusted handover
  establishes that fsimg is its only writer, a contract the
  [layer design](design-layers.md#handed-over-files) and the output
  coordinator's exclusive creation carry. Skipping a write into any other
  file is not zeroing and never happens: such objects have no record and are
  zeroed explicitly, whatever the host or a callback reports about them.
* A read is skipped only when the object answering owns the proof: the
  record, the sealed and tiled chunk index, the assembled disk's own gaps,
  or, for an external file, the host's report after a sync, the one answer
  fsimg does not own and accepts under [Decisions](#decisions).
* A hostile container whose tables declare a vast zero disk costs the
  discovery a few records. What remains is the consumer's own cost, stated
  under [Resource bounds](#resource-bounds): bounded by the work limit and
  the deadline, not by the declaration.
* No host hole query is made on any file fsimg writes, so no fresh file is
  synced for one, and the spool's descriptor rights do not include
  `CAP_FSYNC`. The raw intermediate keeps `CAP_FSYNC` for the writer's
  ordinary flush of its sink. The image input of a read job carries
  `CAP_FSYNC` so its object can sync before asking the host.
* An external file's answer is the host's, and a host whose report is
  wrong after a sync yields a wrong image: a run of written bytes encoded as
  zeros. That residual risk is accepted in [Decisions](#decisions); it is
  confined to files fsimg did not write, and a reader that only lists,
  extracts or verifies a user's image reads every byte it reports, so a
  wrong hole report can affect only the bytes a wrap, compose or unwrap
  copies out of such a file. A shrunk file is not part of that risk where
  fsimg can observe it: the length check fails a lookup that sees the file
  short, and the tail rule keeps the chunk holding each raw external
  object's last byte out of every answer, so its exact read fails short on
  a file still truncated. A shrink undone between observations is
  concurrent modification, undetected for sparse and ordinary reads alike.
* The build check allows `SEEK_HOLE` and `SEEK_DATA` only in
  `compat/host.c`, so the one host query is the one the compatibility layer
  owns and no other path can grow one.
* A callback's answer is trusted application code. A confined worker
  constructs no callback objects, so that trust is the embedding
  application's alone.
* Every emitted byte is defined: data, a zero the zeroing operation wrote,
  or a zero range the responsible object proved.

## Determinism

Output bytes depend only on input bytes and options. Zero decisions in the
transcode depend only on chunk bytes, and a truthful zero range contributes
the same chunk the read would have: all zeros. Hole knowledge spares reads
and never changes a decision. The same image results with hole knowledge
consulted or ignored, on a host with or without sparse files, and at any
granule, provided every answer taken was truthful. fsimg's own answers, the
record, the chunk index and the assembled disk, are truthful by
construction; the host's report and a `next_zero_range` callback are taken
on trust, and a false one changes the bytes, which is the accepted risk of
[Decisions](#decisions) and the stated obligation of a callback.

## Failure

* A write that fails leaves its range marked, so a later zeroing writes real
  zeros over it and a later answer excludes it.
* Record growth that fails (`FSIMG_ENOMEM` or `FSIMG_ELIMIT`) fails the
  resize before the file changes; the object stays usable at its old
  length.
* An adoption that cannot allocate the record fails, and the descriptor is
  released as for any failed adoption.
* A callback answer outside the contract is `FSIMG_EIO`; the operation it
  served fails with it.
* A container's answer fails with the chunk index's error when a record
  cannot be read, an I/O error or a limit from the metadata store, and the
  operation fails with it, as a read of that chunk would.
* A failed discovery sync, or a host without `SEEK_HOLE`, is not an error:
  the object moves to off and the operation reads every byte. An explicit
  output flush that fails remains the error it is.
* An external file observed shorter than declared at a lookup fails the
  operation with `FSIMG_ESOURCE_CHANGED` naming the input; one still short
  when the chunk holding its last byte is read fails with the exact-read
  path's premature-EOF error. Nothing is published in either case.
* A transcode over a disk declared larger than the work limit admits fails
  with `FSIMG_RESOURCE_WORK` at the chunk whose charge crosses the limit;
  cancellation is reported at the chunk on which it is polled, read or not.
* Zeroing that fails after a zero was written reports incomplete output, as
  every partial mutation does.

Handles are used serially, as the architecture requires; the record is state
of one root and introduces nothing shared between workers.

## Tests and evidence

Every passing case is paired with a seeded failure that proves the code
under test ran.

| Area | Passing cases | Seeded failures |
| --- | --- | --- |
| Record marking | Writes through two slices of one fresh file mark the root; the next zero range answers exactly the clear runs; zeroing a disjoint range leaves `written_bytes` unchanged | A write failed by fault injection, and a short write, leave their whole requested range marked: the answer excludes it and zeroing it writes zeros |
| Zeros after writes | A table written at the end of a fresh disk, then the gap zeroed: `written_bytes` counts the table alone | The same sequence on an external file the test opened writes every zero byte |
| Zeroing never consults answers | A reused sink, whose host hole report through a private test hook would falsely claim the whole file is a hole, and a callback sink whose `next_zero_range` claims the same, are zeroed in full: `written_bytes` or `zero_range` calls cover every byte, the hook records no query at all from the writable object, and the callback records none from zeroing | The hook is consulted by a read-side lookup on a read-only object over the same file, and the callback by a read-side lookup, proving both are live |
| Scheme emission order | `test_fixed_disk_stays_sparse`: a 256 MiB fixed GPT disk around a 1 MiB volume writes a few MiB; and a direct test of the scheme emitter whose volume emit callback reads the staged file finds the protective MBR and primary GPT header already present and the backup header still absent, so the regions were visited in disk order | The same plan emitted into an external file writes 256 MiB; under a zero-volume-table pass order the primary header would be absent at the volume emit, and the order assertion fails |
| Granule and resize | A sparse 40 GiB fresh file keeps every possibly-nonzero granule across the fold to 8 KiB: a granule whose odd half alone was set reads set as a whole, and a clear pair reads clear; an unaligned shrink keeps its straddling granule set and the bytes below the new length, and the regrowth answers zero beyond that granule | Allocation failure, and a host truncation failure injected after the record was prepared, leave the committed record and the file unchanged and the object usable at its old length; a fold that clears any granule which held a set half, or a shrink that leaves a wholly removed granule set, fails the assertions. Hosts without sparse files skip the physical case explicitly |
| Record, not host | Zeros written explicitly to a fresh file, a hole on a filesystem that detects zero blocks, are reported possibly nonzero and not reported zero; an unwritten range of a fresh file on a filesystem without holes is reported zero | The build check fails on `SEEK_HOLE` or `SEEK_DATA` anywhere outside `compat/host.c` |
| Own files never ask | A private counter of discovery syncs, counting only the sync made before a host hole query and no ordinary flush, stays at zero across a writer's emission into a fresh staged file, the transcode of its raw intermediate, the extraction of files and the sealing and reading of a spool; the spool's Capsicum rights lack `CAP_FSYNC` and a spooled stream is read under them | The counter reads one at an external file's first lookup, proving it observes discovery syncs; separately, an ordinary flush failure injected at the writer's output flush fails the emission and publishes nothing, so suppressing required flushes cannot pass the counter test |
| User file, host report | A sparse read-only external file is wrapped and composed reading only its data chunks and its final chunk (`read_bytes`), with the discovery sync counted exactly once, before the first lookup; the image input of a read job is unwrapped under the worker's Capsicum rights with the same reads; a writable external sink is never synced or queried | Hole knowledge ignored reads the whole file with identical output; a file on a filesystem without holes reads the whole file |
| Shrink seen at a lookup | A sparse external file whose data lies after a long hole wraps correctly when intact | The file truncated below its data by a hook between the two seeks, so `SEEK_DATA` answers `ENXIO`, fails that lookup with `FSIMG_ESOURCE_CHANGED` naming the input even when the hook restores the file, data and all, before the copy continues; with the length check disabled by a private test control the same case publishes zeros over the restored data, proving the check is what catches it; truncated before the first lookup it fails the same way |
| Truncated input | A sparse external file, intact, wraps correctly, through a prefix slice, as the first of two compose inputs and as a compose input whose end falls inside a transcode chunk | The file truncated by a hook after a lookup and before its tail chunk is read fails at that read with the exact-read error and nothing published: for the whole file, for a prefix slice of it and for the first of two compose inputs truncated before its tail is reached, each ending on a chunk boundary, and under simg in the counting pass, the failure asserted within that pass; with the tail clip disabled by a private test control each of these publishes an image with zeros over the lost tail, proving the clip is what catches it. Under simg in the writing pass the file is truncated after the counting pass completed, and the same control publishes zeros. A compose input whose end falls inside a chunk fails at that chunk's read with or without the clip, because the assembled disk's answer stops at the region boundary and a chunk not wholly covered is read; its seeded control is a private switch that accepts partial chunk coverage, with which the same case publishes zeros |
| Undetected modification | The documented limit: the first of two compose inputs truncated by a hook after its tail chunk was consumed, while the second is processed, publishes an image holding the bytes read before the truncation, as a copy without hole knowledge would; a DMG input whose backing file is truncated behind a zero-record tail publishes its declared zeros, and one truncated under a compressed record that is not in the decode cache fails the chunk read with the exact-read premature-end error (`FSIMG_EIO`); corruption is expected only from malformed compressed bytes | The same truncations moved before the tail read, or under the record being read, fail as the rows above require |
| Tail cost | A 100 GiB sparse external input is wrapped reading only its data chunks plus its final chunk (`read_bytes`) | Hole knowledge ignored reads 100 GiB with identical output |
| Cancellation in skipped zeros | A run of chunks wholly covered by a zero answer is composed with no read, each chunk charged one unit and polled | Cancellation requested while the pass is inside the run stops it at the next chunk with no further charge; with the per-chunk poll omitted by a private test control the run finishes, proving the poll is what stops it |
| Sync failure fallback | An external file whose discovery sync fails (fault control; `ENOTCAPABLE` on a descriptor the test limits without `CAP_FSYNC`) is read in full, the output is identical and the operation reports success with no error; a second lookup on a synced object makes no sync | After the failure the fault control is cleared and a hole is punched in the file: the discovery sync counter does not move and the hole is still read, proving the off state is terminal; a host hook reporting no `SEEK_HOLE` support moves the object to off the same way |
| False host answer | A sparse external file whose report is truthful after a sync wraps to an image identical to one wrapped with hole knowledge ignored | A private hook that answers a hole over a data range after a successful sync yields an output with zeros there, and the test asserts it, documenting the accepted outcome as the callback row does |
| Transcode of fsimg's files | A 1 GiB sparse FAT32 raw intermediate to DMG reads at most the chunks that intersect its possibly-nonzero granules (`read_bytes`); a spooled sparse raw image unwrapped reads the same | The test control that ignores hole knowledge reads the whole disk; the output is identical |
| Container input | A DMG with a 16 MiB zero record composed into a GPT disk: `read_bytes` and the work counter show no chunk read or decoded for the run, and the output is byte-identical to the compose of the same disk with that record replaced by a compressed record of zeros, which the work counter shows decoded in full | The compressed counterpart with its compressed bytes corrupted fails the compose with `FSIMG_ECORRUPT` naming the layer, proving the decode ran |
| Container index metadata | A DMG whose index spilled to scratch under a small page cache answers zero ranges identically with a cold cache and with a warm one, with scratch reads visible in the counters | A metadata I/O failure injected at the record store read fails the compose with the store's error, and nothing is published |
| Callback input | A callback object whose `next_zero_range` reports its holes is wrapped reading only its data | An answer with `start < offset`, `end > length` or no progress fails `FSIMG_EIO` naming the callback; an answer claiming zero over data yields an output with zeros there, which the test asserts to document the obligation |
| Determinism | Byte-identical output with hole knowledge consulted and ignored for every container and gzip, across budgets and chunk-cache sizes | Inputs differing in one byte differ |
| Bounds | A 128 TiB fresh file's record is at most 1 MiB in the memory counters; a DMG declaring a 1 PiB zero disk opens and answers in record reads, with no chunk read or decoded; composed to a DMG under a fixed work limit it fails with `FSIMG_RESOURCE_WORK` through the per-chunk charge, at the chunk whose charge crosses the limit, with `read_bytes` grown over the zero run by nothing beyond the assembled table prefix and the index's record reads, measured as the difference between a reading taken once the pass enters the run and one taken at the failure, and under gzip the same limit stops the zero compression | Allocation failure injected at record growth fails the resize with the file unchanged; the same compose of a disk small enough for the limit completes, proving the limit was what stopped it |
| Sandbox | The spool's Capsicum rights lack `CAP_FSYNC` and the read job's image input carries it; the worker spools a stream, transcodes, and unwraps a sparse input under them | `fsync` on the spool descriptor in capability mode fails with `ENOTCAPABLE`, and an image input limited without `CAP_FSYNC` is read in full, proving the installed rights are the ones specified |

## Decisions

1. **Read-only files fsimg did not write answer from the host's report,
   synced once first, never for their last chunk.** This covers every
   read-only external file, whichever constructor built its object: the
   image a reader opens, the input a wrap or compose reads. A writable
   external object, a sink an application supplies, answers nothing. The
   object syncs once before its first lookup, then asks `SEEK_HOLE` and
   `SEEK_DATA`, fails as a changed input when `fstat` shows the file shorter
   than declared, and clips every answer to the observed length and to end
   before its own last byte, so the chunk holding that byte is always read
   and a file still short there fails; a failed sync or an absent interface
   moves the object to off for good. fsimg detects the truncation it can
   observe, at a lookup or at that read, and not a shrink undone between
   them. The sync is a workaround for the OpenZFS 15526 class of fault, not
   a proven fix, as [the host's report](#external-files-the-hosts-report)
   states with its evidence.

   Rationale: fsimg has no knowledge of its own about such a file, so the
   choice is between the host's report and reading every byte. Reading a
   100 GiB sparse input in full costs minutes of zero reads for an image
   whose data fits in seconds, on every host, for a fault that affected
   particular releases of one filesystem and that the sync narrows by an
   amount the design does not claim to know.
   The residual risk is confined to files fsimg did not write and to the
   bytes a wrap, compose or unwrap copies out of them; a wrong report cannot
   reach a file fsimg wrote, which answers from its record. The right the
   sync needs under Capsicum, `CAP_FSYNC` on the read job's image input, is
   a read-side right on a descriptor the worker already reads.

   Rejected: *trusting the report without a sync*, which leaves the
   reproduced fault open on the ordinary sequence `mkfs` then `fsimg` on
   the same file; and *never asking*, which deletes the host query, the
   sync and the right, and is always correct, but makes every sparse user
   input cost its full length in reads.

2. **The callback is named `next_zero_range`**, pairing with the
   `zero_range` operation and naming the contract it carries, a promise about
   contents rather than storage.

3. **The transcode does not split chunks.** A chunk is zero without a read
   only when a reported range covers it wholly; otherwise all of it is read.
   Splitting at the range boundary would spare part of one read per zero run
   for the copy and gzip encoders and nothing for DMG and simg, which encode
   whole chunks, for one more case in the pass.
