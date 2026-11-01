# Logical content and extent storage

Allocation and content answer different questions. Allocation identifies the
storage an object owns. Content identifies the bytes a caller may read and what
they mean. This document defines one common delivery operation for content.
Drivers only resolve logical offsets to validated physical storage. It also
defines an optional shared extent index that drivers can use for that
resolution.

The public contract of `fsimg_reader_read_at()` and
`fsimg_reader_next_range()` is in the [C API](design-api.md#reader). The
[architecture](architecture.md#writer-and-reader-independence) assigns the
owners, and [storage](design-storage.md#reading-and-traversal) states the
complexity bounds.

## Content model

A regular file's content has three explicit quantities, all in bytes:

| Quantity | Meaning |
| --- | --- |
| Logical length | The file size. No content exists at or beyond it. |
| Initialized length | `0 <= initialized <= logical`. Bytes below it come from storage. |
| Physical mapping | Validated image ranges that back the initialized bytes. |

Bytes in `[initialized, logical)` are zero and are synthesized without reading
backing storage. FAT and QNX6 files have `initialized == logical`. An exFAT file
uses its valid-data length.

For an exFAT file with 16 KiB allocated, a 10 KiB logical length, and 6 KiB
initialized:

| Logical range | Content | Allocation |
| --- | --- | --- |
| `[0, 6 KiB)` | Mapped image bytes | Owned |
| `[6, 10 KiB)` | Zeros, backing storage never read | Owned |
| Beyond 10 KiB | Not content | Slack still owned by the file |

Allocation slack and uninitialized backing bytes are never readable through any
content interface. Extent lookup alone never authorizes exposing them.

## Owners

| Owner | Owns |
| --- | --- |
| Common delivery (`lib/read/`) | Bounds against the logical length, the initialized/zero split, transfer batching, source-stability checks, cancellation, work charging, confirmed byte counts, content-domain enforcement, terminal failure |
| Driver | Content domain, initialized length, and resolution of a logical offset below it to one validated physical extent containing that offset, or for encoded content to one [decoded span](#decoded-spans) |
| Extent index (optional, `lib/meta/`) | Encoded extent records, bounded growth, ordering and coverage checks, sequential and binary lookup, cleanup |

Delivery calls one driver operation: resolve offset `o < initialized` to an
extent `(logical_offset, image_offset, length)` with
`logical_offset <= o < logical_offset + length`. The driver chooses how to
answer: a sequential chain cursor, a direct block-tree lookup, or the extent
index. Delivery checks containment, clips the extent to the request and to the
initialized length, and reads through bounded I/O only inside the reader's
content domain: the image range file storage can occupy, which the driver
declares when it opens the volume and which also bounds its extent indexes. A
driver defect therefore cannot deliver bytes outside that domain, such as
allocation tables or the boot area, as content. Metadata stored inside the
domain is not excluded; as with index records, ownership verification detects
a file extent that names another object's units.

Payload verification reads through the same delivery. A driver whose
ownership walk already yields each physical run, such as FAT, can instead read
those runs clipped to the logical length, so verification walks the
allocation once.

## Operations

`read_at()` and `next_range()` follow the same rules:

* A range query reports DATA for the whole initialized region and ZERO for
  the uninitialized tail. It answers from these lengths alone: it consumes
  nothing and resolves no storage, so a range query followed by reads of that
  range costs no additional allocation walk.
* `next_range()` can return a DATA range spanning several physical extents.
  Callers cannot infer fragmentation from range boundaries; range shapes are
  a delivery decision, not a driver one.
* A read crossing an extent boundary or the initialized boundary continues
  across it in one call. On failure it reports the bytes confirmed before the
  failure.
* Cached resolutions and synthesized zeros still run the source-stability check,
  cancellation, and work charging. A driver cannot bypass them by answering from
  cache.

Metadata reads of complete records keep their own contract: they succeed only
with the complete record. Partial progress is a content-read concept.

## Decoded spans

Some content is not an image range: an HFS Plus file that macOS compressed
keeps its bytes as encoded chunks in an attribute or its resource fork. For
such an entry the driver's resolution returns a decoded span instead of an
extent: `(logical_offset, bytes, length)`, with `bytes` borrowed from one
decode window the driver owns and `logical_offset <= o < logical_offset +
length`. The window is one charged buffer per reader, sized by the format's
fixed chunk, 65536 bytes for HFS Plus, and holds one decoded chunk at a
time. A span stays valid until the next resolution or the walker's advance.

Delivery treats a span as it treats an extent: it checks containment,
clips the span to the request and the initialized length, copies from the
window instead of reading the image, and keeps every other duty in the
owners table, so cancellation, work charging, confirmed counts, the
source-stability check and terminal failure stay common. Because delivery
reads nothing for a span, keeping the encoded bytes' reads in bounds is the
driver's obligation, as it is for its other metadata reads: it reads them
through a fork's extent index over the content domain, or from a validated
metadata record such as an inline attribute. It decodes exactly the chunk's
expected length or fails, and charges the bytes it consumed and produced as
work. A sequential read therefore decodes each chunk once, and memory does
not grow with the file. Range queries still answer from the lengths alone.

## Extent index

The extent index is a tool, not a requirement. A driver can keep a sequential
cursor and build an index only on a seek, prepare one when its metadata
validation already needs it, or resolve directly and never build one.

Records are three little-endian `uint64_t` values: logical offset, image
offset, and length. They sit in a paged scratch store charged to the reader's
session. The driver creates an index with its permitted physical domain: the
image range and allocation-unit alignment that file storage can occupy, such as
the FAT data region or the exFAT cluster heap. The index owns:

* append in strictly increasing logical order, coalescing adjacent ranges and
  rejecting gaps, overlap, zero lengths, and offset overflow;
* checked decoding of every record read back from scratch, because scratch is
  not trusted: logical bounds, and the image range against the physical domain,
  so a damaged record cannot redirect content outside the domain, for example
  into the FAT. A record can still name another object's units inside the
  domain; ownership verification detects that cross-link;
* an ordered walk that requires the records to tile the allocation exactly,
  for whole-object uses such as allocation checks, ownership claims and
  directory-cycle marking;
* lookup of the extent containing an offset, using a cached last extent for
  sequential access and binary search otherwise;
* release on owner request and on reader failure or destruction.

Drivers keep allocation discovery, geometry-specific checks, and chain or
bitmap validation. An index built for content is discarded when the walker
advances. Insufficient resources for an index are a limit error, never a
fallback to restarting the chain for each request.

## Validation timing

Payload verification uses the initialized-content operation in the
[verification-helper design](design-verification-helpers.md). It reads through
common delivery and accounts confirmed progress, including before failure,
without visiting an uninitialized tail or allocation slack. Drivers choose
which distinct objects to read; FAT may retain its fused chain-validation and
physical-read path to avoid a second allocation walk.

Each driver design states when allocation is validated: when the entry is
exposed, when content is first resolved, or during full verification. Listing
always validates entry metadata. Walking a file's allocation at exposure is a
driver choice with a cost proportional to fragments, charged to the session.
No choice can make allocation slack readable.
