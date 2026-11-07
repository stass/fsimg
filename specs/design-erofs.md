# EROFS reader and writer

## Scope and evidence

`erofs` denotes the Enhanced Read-Only File System: a compact, immutable
filesystem image whose metadata is addressed in 32-byte slots and whose file
data is stored flat, in fixed-size chunks, or in independently compressed
physical clusters. The reader opens one EROFS volume and exposes its
namespace and content. The writer constructs a new volume from the source
catalog: uncompressed in its base profile, and on request LZ4-compressed,
with 48-bit addressing, or with its metadata in a metadata box. EROFS has no allocation bitmap, journal, free space or in-place
update, so the writer's output is complete when it is emitted, and nothing
in the format asks a reader to recover or a writer to reserve.

Reader and writer are independently registered capabilities. The writer is
registered only after native qualification (see
[acceptance](#acceptance-and-independent-validation)); its LZ4, 48-bit and
metadata-box profiles are settings of the same writer, each accepted only
after its own qualification.

This document is an original description of disk-format facts and fsimg
requirements. It contains no copied reference prose, C declarations or
code. Its input is the research draft
[EROFS filesystem: on-disk specification and fsimg implementation plan](../fsspecs/EROFS_fsimg_spec.md)
("the draft"), which is not itself authoritative; the
[implementation plan](../docs/erofs-plan.md) records where this design
departs from it. The research record, `docs/erofs-research.md`, which the
plan's evidence stage writes, holds sources, licenses, tool identities and
observations.

Evidence has three meanings here:

* **Established layout**: a byte interpretation given by the published
  EROFS on-disk documentation [D1] or the MIT-licensed on-disk header of
  `erofs-utils` [H1]. It establishes positions and meanings, not that every
  combination is valid or what current producers emit.
* **Observation**: a measured property of a named fixture from the plan's
  experiments (E1 through E14), produced by a pinned `mkfs.erofs` [U1] and
  checked by `fsck.erofs`, `dump.erofs` and a Linux kernel mount [K1]. An
  observation is not a universal format rule.
* **fsimg policy**: a deliberate validation, capability or construction
  rule. It can be stricter than the Linux kernel without redefining the
  format.

A value no observation has yet confirmed is marked **provisional** and names
the experiment that closes it, with the rule that applies if the experiment
cannot answer. Where a provisional rule names no other answer, the reader
refuses the unconfirmed structure with `FSIMG_EUNSUPPORTED` and the writer
never emits it. The [driver design](design-erofs-driver.md) maps these rules
onto the modules, state and algorithms of `lib/formats/erofs/`.

## How EROFS works

This section explains the model the rules below serve.

* **One superblock, no allocation state.** A 128-byte superblock at byte
  1024 of the volume names the block size, the root inode, the start of the
  metadata address space and the feature words. Nothing records which
  blocks are in use: a block is used because a reachable inode refers to
  it. The image is built once and never modified.
* **Metadata is a byte space addressed in 32-byte slots.** An inode's
  number on disk, its NID, is its slot index from the metadata start, so an
  inode is found by one multiplication and an addition. An inode is 32 bytes
  (compact) or 64 bytes (extended), followed by its extended attributes and
  then by layout-specific bytes: an inline tail of file data or a
  compression index. Inodes are packed back to back in that space, which may
  share blocks with file data.
* **Each inode chooses its data layout.** Flat data is one contiguous run
  of blocks, optionally with its last partial block stored inline after the
  inode. Chunked data maps fixed-size logical chunks to blocks through an
  index and represents holes explicitly. Compressed data maps logical
  clusters to physical clusters, each decoded independently, so random reads
  decode one cluster rather than a whole file.
* **Directories are files.** A directory's content, delivered through the
  same data layouts as a regular file's, is a sequence of blocks; each holds
  fixed 12-byte entries naming a child NID and a type, then the children's
  names. Names are raw bytes in strictly increasing byte order across the
  whole directory, so a lookup is a binary search.
* **Sharing is read-only.** A hard link is several entries naming one NID.
  Identical data may be stored once and referenced by several files, since
  nothing is ever rewritten.

### Consequences for fsimg

* Reading needs no table of the volume: each directory is decoded on its
  own, each inode is found from its NID, and each read maps one logical
  range. Nothing proportional to the volume is held in memory.
* Verification cannot compare claims against an allocation map, because
  there is none. It checks that every reachable structure is in bounds and
  that metadata structures do not overlap one another or data; data may be
  shared, because the format permits it.
* The writer plans the whole image before emitting a byte: inode sizes fix
  the NIDs, NIDs fix the directory blocks, and every block's address is
  final at the end of planning. There is no free space to size.

## Compatibility profiles

| Profile | Reader contract | Writer contract |
| --- | --- | --- |
| Core | Block sizes 512 to 4096 (provisional for 65536, [geometry](#volume-identity-and-geometry)); compact and extended inodes; flat and flat-inline data; every POSIX kind; extended attributes validated and marked as extra data | Emitted, 4096-byte blocks, flat and flat-inline layouts, compact and extended inodes, no extended attributes |
| Chunked | Chunk-based files with 4-byte and 8-byte indexes and holes, primary device only | Never emitted |
| Compressed | Full and compact indexes, big physical clusters, plain clusters, compressed inline tails, packed fragments, shared clusters and compressed directories; LZ4 and DEFLATE always, MicroLZMA and Zstandard when the build has the optional library | LZ4 with full indexes and one-block physical clusters, under the `compression=lz4` setting |
| 48-bit layout | Wide block numbers and root NID, per-inode compact times, implicit link counts, directories without `.`, byte-addressed encoded extents | Emitted under `addressing=48`: wide fields and compact times, encoded extents for LZ4 files, no omitted dots |
| Metadata box | Inodes and shared attributes in a second metadata space carried by a file, plain or compressed | Emitted under `metabox=plain` or `metabox=lz4` |
| External data devices | Recognized and refused at open | Never emitted |

The 48-bit layout and the metadata box are experimental upstream. Their
rules below are provisional as a whole until E10 and E12 confirm them
against a pinned producer and a kernel that mounts them, and neither is ever
emitted unless the caller asks for it.

A feature outside these profiles is refused with `FSIMG_EUNSUPPORTED` naming
it, never skipped or guessed. A compressed file whose codec this build lacks
is refused when its content is read; its entry is still listed, and
verification of a volume holding it is incomplete.

## Shared owners and representation

The resource context owns memory, scratch, work accounting and cancellation.
Bounded I/O owns all volume-relative byte access. The common source catalog
owns input identities and repeatable reads; the output coordinator owns
publication. The common walker owns traversal, paths and selection, and
common delivery owns content reads. EROFS code obtains neither host paths
nor unbounded descriptors, and the extractor keeps its safe
file-and-directory profile.

An EROFS reader holds the validated superblock, the derived geometry and
bounded decode windows, one for file content, one for metadata and one for a
compressed metadata box. It holds
no inode cache, directory cache or table proportional to the volume. Reader
state and the writer's plan rows are separate.

Every integer is little-endian and decoded from bytes after its span is
proved; no structure is laid over image bytes. Offsets and products use
checked `uint64_t` arithmetic, and the driver keeps four quantities apart:
block numbers, NIDs, logical file offsets and physical byte offsets. A block
number becomes a byte offset only through one checked multiplication, and a
NID only through the [inode address rule](#addressing).

## Volume, identity and geometry

All offsets are relative to the volume the reader is handed. A partition, a
DMG or a compressed container around an EROFS volume is peeled by the
[layers](design-layers.md) first, so offset 1024 is always the superblock.
EROFS does not reserve its first 1024 bytes for itself: they are left for a
boot record, which the format neither reads nor validates, and the volume
has no reserved system area beyond them.

**Probe identity.** The 32-bit value at volume offset 1024 is `0xE0F5E1E2`,
stored as the bytes `E2 E1 F5 E0`. The probe reads a prefix of 1152 bytes,
the base superblock's end. A prefix shorter than 1028 bytes, or a different
value, is not EROFS (`FSIMG_EFORMAT`). Once the magic matches, the probe
claims the volume and checks only the base superblock: a `blkszbits` below
9 is `FSIMG_ECORRUPT`; a `blkszbits` above the reader's bound, a nonzero
`dirblkbits`, extension slots that end past the
[superblock region](#volume-identity-and-geometry), or an
incompatible feature bit outside the [reader's set](#features) is
`FSIMG_EUNSUPPORTED`. Open applies the same classification. The probe
allocates nothing and reads nothing beyond its prefix. A forced open waives
only the magic.

**Block size.** `B = 2^blkszbits`. The reader accepts `blkszbits` 9 to 12.
Linux accepts larger blocks on hosts whose page size permits them; the
reader's upper bound is provisional at 12 and E1 tests whether any pinned
producer emits a larger block; if none does, 12 stays the bound and a larger
value is `FSIMG_EUNSUPPORTED`. `dirblkbits` must be zero; another value is
`FSIMG_EUNSUPPORTED`, since no published layout defines it.

**Superblock region.** The superblock, its extension slots and its
[configuration records](#configuration) lie in `[1024, 4096)`, the bytes
Linux reads with the superblock: one 4 KiB page less the 1024-byte offset.
The region is independent of `B`, so a 512- or 1024-byte-block volume keeps
its superblock outside block 0. The bound is provisional: E1 confirms it by
building volumes whose slots or records end on each side of byte 4096 and
recording which the kernel mounts, naming the page size of the kernel that
ran; if E1 cannot answer, 4096 stays, since it is the strictest bound a
producer is known to meet. Extension slots that end past byte 4096, or a
configuration record that does, are `FSIMG_EUNSUPPORTED` until E1 settles
the bound, since a kernel on a host with larger pages may read them. A
structure running past the end of a volume shorter than 4096 bytes is
`FSIMG_ECORRUPT`.

**Volume extent.** The superblock's block count `blocks` (the 32 bits at
`0x24`, widened by the [48-bit layout](#48-bit-layout)) is accounting
information, not proof of an address's validity. When it is nonzero the
driver declares `blocks * B` as the volume's length and its address domain
is `[0, blocks * B)`; when it is zero, the domain is the volume. Every
structure the reader touches lies inside the domain; the domain, never an
advisory count, decides validity. A declared length beyond the volume is a
truncation under the [common rules](architecture.md#byte-and-object-boundaries):
open, listing and extraction succeed while everything they touch lies in
the volume, a referenced structure past its end fails `FSIMG_ECORRUPT`
through the read funnel, and full verification fails on the missing bytes
the reader core reports. Whether pinned producers ever write a count larger
than their image is provisional, E1; if one does, the design records it
and the rule stands, since a count beyond the image is not a verified
volume.

## The superblock

Offsets are relative to the superblock at volume byte 1024. Unsigned
little-endian unless stated.

| Offset | Size | Field | Rule |
| --- | --- | --- | --- |
| `0x00` | 4 | magic | `0xE0F5E1E2` |
| `0x04` | 4 | checksum | CRC-32C under `SB_CHKSUM`, [below](#superblock-checksum) |
| `0x08` | 4 | compatible features | [Features](#features) |
| `0x0C` | 1 | `blkszbits` | 9 to 12 |
| `0x0D` | 1 | extension slots `n` | The superblock is `128 + 16 * n` bytes; it ends inside the [superblock region](#volume-identity-and-geometry) |
| `0x0E` | 2 | root NID | The root directory's NID; under the 48-bit layout, the block count's high 16 bits |
| `0x10` | 8 | inode count | Advisory; reported, never used to bound a NID |
| `0x18` | 8 | epoch | Signed seconds since 1970, the time base of compact inodes |
| `0x20` | 4 | fixed nanoseconds | Below 1000000000, else `FSIMG_ECORRUPT` |
| `0x24` | 4 | block count | [Volume extent](#volume-identity-and-geometry) |
| `0x28` | 4 | metadata block | Start of the metadata address space, in blocks |
| `0x2C` | 4 | shared-attribute block | Start of the shared attribute pool, in blocks |
| `0x30` | 16 | UUID | Reported as stored |
| `0x40` | 16 | volume name | Raw bytes up to the first NUL or the field's end |
| `0x50` | 4 | incompatible features | [Features](#features) |
| `0x54` | 2 | algorithm mask, or LZ4 maximum distance | [Compression configuration](#configuration) |
| `0x56` | 2 | extra device count | Zero, else the volume is refused (`FSIMG_EUNSUPPORTED`, external data devices) |
| `0x58` | 2 | device table slot | Ignored when the count is zero |
| `0x5A` | 1 | `dirblkbits` | Zero |
| `0x5B` | 1 | long-prefix count | [Long prefixes](#long-prefixes) |
| `0x5C` | 4 | long-prefix start | [Long prefixes](#long-prefixes) |
| `0x60` | 8 | packed NID | The [packed inode](#packed-fragments), when the fragments feature is set |
| `0x68` | 1 | attribute filter reserved byte | Nonzero disables the [name filter](#name-filter) |
| `0x69` | 1 | image-share prefix index | Reported; not interpreted |
| `0x6A` | 2 | Reserved | Zero |
| `0x6C` | 4 | Build time | Provisional, below |
| `0x70` | 8 | Wide root NID | The root's NID under the 48-bit layout; zero otherwise |
| `0x78` | 8 | Reserved | Zero |
| `0x80` | 8 | Metadata-box NID | In the first extension slot; the [metadata box](#metadata-box) inode |
| `0x88` | 8 | Reserved | Zero, when an extension slot covers it |

The four bytes at `0x6C` hold a build time relative to the epoch in recent
producers. The rule is provisional: E1 records what pinned producers write
there; until it does, the reader reports the word and accepts any value, and
the writer writes zero. A nonzero reserved superblock byte does not prevent
reading, since producers have given reserved bytes meaning without a feature
bit (`0x69` and `0x6C` were both reserved once): `i` reports it, and
verification of such a volume is incomplete. The writer writes every reserved
byte as zero. An extension slot's bytes are interpreted only
by the metadata box; without it a volume may carry any number of slots,
whose bytes are not read, and with it at least one slot is required.

The volume name is delivered as the label when it is valid UTF-8 without
control bytes; otherwise the label is absent and `i` shows the bytes
escaped. The UUID is reported in its stored byte order.

### Superblock checksum

Under `SB_CHKSUM` the checksum is CRC-32C (the Castagnoli polynomial) over
the bytes from 1024 to the covered range's end, with the checksum field read
as zero. The covered range is provisional: [D1] describes `[1024, 1024 +
B)`, while current producers and the kernel cover `[1024, B)` when `B >
1024` and `[1024, 1024 + B)` otherwise. E1 settles it by flipping the byte
on each side of both candidate boundaries in images of every block size and
recording which flips `fsck.erofs` and the kernel reject; the rule that
matches the kernel is the reader's and the writer's. The stored value's seed
and final inversion are likewise provisional: producers start from all ones
and store the register without the final inversion, which E1 confirms
against the published CRC-32C check value before any fixture is read. If E1
cannot answer, the kernel's observed acceptance decides, and a disagreement
between the kernel and `fsck.erofs` is recorded in the research record with
the kernel's rule adopted.

A mismatch is `FSIMG_ECORRUPT` at open: a reader that ignored it would accept
a superblock the kernel refuses to mount. A volume without `SB_CHKSUM` is
read without a checksum, and verification reports that the superblock is
unprotected. The checksum protects only the superblock region; it is not an
integrity claim about the volume.

## Features

The feature words are closed tables. The reader stores both words as read
and decodes the combination once at open.

| Compatible bit | Name | Reader treatment |
| --- | --- | --- |
| `0x01` | `SB_CHKSUM` | The [checksum](#superblock-checksum) is checked |
| `0x02` | `MTIME` | The extended inode's time is a modification time, [below](#times) |
| `0x04` | `XATTR_FILTER` | Inodes carry an attribute [name filter](#name-filter) |
| `0x08` | `SHARED_EA_IN_METABOX` | The shared attribute pool lies in the [metadata box](#metadata-box); requires `METABOX` |
| `0x10` | `PLAIN_XATTR_PFX` | The long-prefix table is a plain region, [below](#long-prefixes) |
| `0x20` | `ISHARE_XATTRS` | Reported; the attributes it marks are ordinary attributes to the reader |

An unknown compatible bit does not prevent reading, as the format intends;
`i` reports it, and verification of such a volume is incomplete, since
fsimg cannot claim to have checked semantics it does not know.

| Incompatible bit | Names | Reader treatment |
| --- | --- | --- |
| `0x001` | `LZ4_0PADDING` | Compressed streams are right-aligned in their physical clusters, [below](#physical-clusters) |
| `0x002` | `COMPR_CFGS`, also `BIG_PCLUSTER` | Configuration records follow the superblock; big physical clusters are permitted |
| `0x004` | `CHUNKED_FILE` | Chunk-based files are permitted |
| `0x008` | `DEVICE_TABLE`, also `COMPR_HEAD2` | The second algorithm slot is permitted; with a nonzero device count the volume is refused |
| `0x010` | `ZTAILPACKING` | Compressed inline tails are permitted |
| `0x020` | `FRAGMENTS`, also `DEDUPE` | Packed fragments and shared physical clusters are permitted |
| `0x040` | `XATTR_PREFIXES` | The long-prefix table exists |
| `0x080` | `48BIT` | The [48-bit layout](#48-bit-layout) |
| `0x100` | `METABOX` | The [metadata box](#metadata-box) |

Any other incompatible bit is `FSIMG_EUNSUPPORTED`, naming the bit. Bits
that carry two names are one feature each; the reader never counts an
aliased bit twice, and decides what it permits from the bit and the
structure that uses it. A layout or index type that needs an incompatible
bit the volume lacks is `FSIMG_ECORRUPT`: a chunk-based inode without
`CHUNKED_FILE`, a big physical cluster without `COMPR_CFGS`, a second
algorithm without `COMPR_HEAD2`, a compressed tail without `ZTAILPACKING`,
a fragment without `FRAGMENTS`, a long-prefix reference without
`XATTR_PREFIXES`, an encoded extent without `48BIT`, a NID in the second
metadata space without `METABOX`. A refused feature is refused at open,
before the root is read, so no partial view of a refused volume exists.

## Inodes

### Addressing

The inode with NID `n`, bit 63 clear, begins at byte
`meta_blkaddr * B + 32 * n` of the volume, computed with checked arithmetic.
With bit 63 set, it begins at byte `32 * (n with bit 63 cleared)` of the
[metadata box's](#metadata-box) content. A NID is a slot index, not an inode
number, and a directory entry's NID is the only key that finds an inode. The
root NID comes from the superblock: the 16-bit field, or the wide field under
the 48-bit layout. An inode's fixed part, attribute region and
layout bytes lie inside the address domain; the fixed part and attribute
region may cross a block boundary, but an inline tail may not (see
[flat data](#flat-and-flat-inline)).

### Format word

The 16-bit format word at inode offset 0 holds the version in bit 0 (0
compact, 1 extended) and the data layout in bits 1 to 3. Bit 4 has meaning
only under the [48-bit layout](#48-bit-layout) and bits 5 to 15 are
reserved; any of them set otherwise is `FSIMG_ECORRUPT`.

| Layout | Name | Profile |
| --- | --- | --- |
| 0 | Flat | Core |
| 1 | Compressed, full index | Compressed |
| 2 | Flat with inline tail | Core |
| 3 | Compressed, compact index | Compressed |
| 4 | Chunk-based | Chunked |
| 5 to 7 | Reserved | `FSIMG_ECORRUPT` |

### Compact inode

| Offset | Size | Field |
| --- | --- | --- |
| `0x00` | 2 | Format word |
| `0x02` | 2 | Attribute count `c`; the attribute region is 0 bytes when `c` is 0, else `12 + 4 * (c - 1)` |
| `0x04` | 2 | Mode, type and permission bits |
| `0x06` | 2 | Link count |
| `0x08` | 4 | Size |
| `0x0C` | 4 | Reserved, zero |
| `0x10` | 4 | Layout word: start block, compressed block count, device number or chunk format |
| `0x14` | 4 | Serial number |
| `0x18` | 2 | Owner |
| `0x1A` | 2 | Group |
| `0x1C` | 4 | Reserved, zero |

A compact inode's time is the superblock's epoch and fixed nanoseconds.
The word at `0x0C` becomes a per-inode time only under the 48-bit layout,
so without it a nonzero value there is `FSIMG_ECORRUPT`: producers of other
volumes write zero (provisional, E2), and a reader that accepted it would
have to guess its meaning.

### Extended inode

| Offset | Size | Field |
| --- | --- | --- |
| `0x00` | 2 | Format word |
| `0x02` | 2 | Attribute count |
| `0x04` | 2 | Mode |
| `0x06` | 2 | Reserved, zero; the start block's high bits under the 48-bit layout |
| `0x08` | 8 | Size |
| `0x10` | 4 | Layout word |
| `0x14` | 4 | Serial number |
| `0x18` | 4 | Owner |
| `0x1C` | 4 | Group |
| `0x20` | 8 | Time, signed seconds since 1970 |
| `0x28` | 4 | Nanoseconds, below 1000000000 |
| `0x2C` | 4 | Link count |
| `0x30` | 16 | Reserved, zero |

### Body

An inode's body is its fixed part, then its attribute region, then its
layout bytes:

* flat with inline tail: the tail, `size mod B` bytes, at the attribute
  region's end;
* compressed: the [map header](#map-header) at the attribute region's end
  rounded up to 8, then the index;
* chunk-based: the chunk index at the attribute region's end rounded up to
  the index entry width.

The body's length follows from these fields alone. Every byte of it lies
inside the address domain; a body that runs past the domain is
`FSIMG_ECORRUPT`. The serial number is reported but never used to identify
an inode, and two inodes may share one.

### Kinds and fields

The mode's type bits select the kind: regular file, directory, symbolic
link, FIFO, character device, block device or socket. Any other type is
`FSIMG_ECORRUPT`. The permission bits are the low 12 bits.

* **Regular files and directories** use any layout their profile permits.
* **Symbolic links** store their target as file content, through any
  layout. The target is 1 to 1024 bytes without NUL under the common
  [link model](design-metadata.md); a target of 0 bytes or one containing
  NUL is `FSIMG_ECORRUPT`, and a longer one is `FSIMG_EUNSUPPORTED`.
* **FIFOs and sockets** have size zero and a zero layout word.
* **Character and block devices** have size zero and carry the device number
  in the layout word, in the Linux 32-bit encoding: the minor's low 8 bits in
  bits 0 to 7, the major in bits 8 to 19, the minor's remaining bits in bits
  20 to 31. The encoding is provisional and E5 confirms it from `mknod`
  sources in the Linux check environment; the reader delivers
  `dev_major` and `dev_minor` as decoded, under the
  [device-node design](design-device-nodes.md).

A device, FIFO or socket uses layout 0; another layout, or a nonzero size,
is `FSIMG_ECORRUPT`. These size and layout rules are provisional: E5
records what producers write, and if a producer writes other values the
rule is relaxed here from that evidence rather than in code.

### Times

EROFS stores one time per inode. The kernel presents it as the access,
change and modification time alike; fsimg delivers only what the format
states. Under `MTIME` the stored time is the source's modification time and
is delivered as `modified`. Without `MTIME`, producers stored the change
time, and it is delivered as `changed` (provisional: E2 builds a volume with
the oldest pinned producer that omits the bit and records which source time
it stored; if no such producer can be built, the rule stands as written,
since it claims no more than the format promises). Access and creation
times are not stored. Times are UTC with nanosecond precision.

### Links

Several directory entries may name one NID; that NID is one inode with
several names, a hard link. The reader delivers each name as its own entry,
under the [common rule](design-formats.md#format-descriptors-and-profiles),
and verification requires a non-directory's link count, 1 when the
[48-bit layout](#48-bit-layout) makes it implicit, to equal the number of
entries naming it. A directory has exactly one name, and a second entry
naming it is `FSIMG_ECORRUPT`. Reading detects a second name in another
parent when it descends, through [the parent rule](#the-parent-rule);
a second name in the same parent satisfies that rule and only verification
detects it.

## Data layouts

Every layout maps the logical range `[0, size)` of one inode. Nothing past
`size` is content, even when it is stored.

### Flat and flat-inline

A flat inode's content is `size` bytes from byte `startblk * B`, where
`startblk` is the layout word. Its blocks lie inside the address domain. An
empty flat inode has no blocks and its start block is not interpreted.

A flat-inline inode stores its first `floor(size / B)` blocks the same way
and its last `size mod B` bytes, the tail, in its body. The tail lies inside
one block: a tail that crosses a block boundary is `FSIMG_ECORRUPT`. The
inode's fixed part and attribute region before it may lie in an earlier
block. When `size` is a multiple of `B` the
tail is empty; producers then write layout 0 (provisional, E3), and a
flat-inline inode without a tail is accepted.

A start block of all ones marks an unallocated flat inode in some
producers. Whether a pinned producer emits it is provisional (E3). Until E3
observes it, it is refused as `FSIMG_EUNSUPPORTED`, naming the null flat
address, rather than read as zeros or rejected as corrupt.

### Chunk-based

The layout word of a chunk-based inode is its chunk format: bits 0 to 4 are
`chunkbits`, bit 5 selects 8-byte index entries, bit 6 permits the wide
block numbers of the [48-bit layout](#48-bit-layout) and requires it, and
the other bits are reserved and zero. The chunk
size is `B << chunkbits`, which must not exceed 2^47 bytes (the reader's
bound on any file size), or the inode is `FSIMG_ECORRUPT`. The file has
`ceil(size / chunk)` chunks, and the index starts at the attribute region's
end rounded up to the entry width:

* a 4-byte entry is a block number;
* an 8-byte entry is a 16-bit high part, zero unless chunk format bit 6
  widens the block number to 48 bits, a 16-bit device identifier, which is
  zero because the volume has no extra devices, and a 32-bit block number.

A block number of all ones, at the entry's width, is a hole: that chunk
reads as zeros, delivered as a
[zero extent](design-content.md#zero-extents). Any other chunk occupies
`min(chunk, size - offset)` bytes from `block * B`, inside the domain. A
nonzero high part without bit 6, or a nonzero device identifier, is
`FSIMG_ECORRUPT`. Chunks may
share blocks with each other and with other files.

### Holes

A chunk hole is the format's only uncompressed hole. A compressed inode's
logical clusters are all mapped; a flat inode's blocks are all stored. The
reader never infers a hole from zero bytes.

## Directories

A directory's content is read through its data layout, flat, inline,
chunked or compressed, exactly as a file's. Its size is the length of its
directory data: every block but the last is `B` bytes, the last is
`size - (k - 1) * B` bytes for `k` blocks. An empty size is
`FSIMG_ECORRUPT`, since every directory holds `.` and `..`.

### Blocks

Each directory block starts with an array of 12-byte entries:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | Child NID |
| 8 | 2 | Name offset within the block |
| 10 | 1 | File type |
| 11 | 1 | Reserved |

The first entry's name offset, divided by 12, is the block's entry count. It
is nonzero, a multiple of 12, and less than the block's valid length. Names
follow the array, concatenated; entry `i`'s name runs from its name offset
to entry `i + 1`'s, and the last entry's name runs to the first NUL byte or
the block's valid end, whichever comes first. Name offsets increase
strictly. An empty name, which only the last entry can produce, a name
longer than 255 bytes, or one holding `/` or NUL, is `FSIMG_ECORRUPT`.
Whether producers fill a block's unused bytes with zeros is provisional
(E4); the reader requires nothing of them.

The file type is 0 unknown, 1 regular file, 2 directory, 3 character device,
4 block device, 5 FIFO, 6 socket, 7 symbolic link. Any other value is
`FSIMG_ECORRUPT`. A nonzero type that disagrees with the child inode's mode is
`FSIMG_ECORRUPT`; type 0 defers to the mode.

### Order and dot entries

Names, including `.` and `..`, increase strictly in byte order across the
whole directory: within each block and from each block's last name to the
next block's first. A repeated or decreasing name is `FSIMG_ECORRUPT`. Dot
entries are ordinary records in that order, not necessarily first; a name
such as `-a` sorts before them.

`.` names the directory itself and `..` its parent; the root's `..` names the
root. Neither is delivered as a child. Under the 48-bit layout a directory
whose format word has bit 4 set omits `.`; the reader treats it as present
and requires that no record is named `.`.

### The parent rule

When preparation decodes directory `D` reached from parent `P`, `D`'s `..`
must name `P` and its `.` must name `D`, and no other entry of `D` may name
`D` or `P`. A violation is `FSIMG_ECORRUPT`. Each reachable directory
therefore has one parent, and walking down from the root follows that
parent's single edge, so a reachable directory can never be its own
ancestor and no visited set is needed to stop cycles. Two entries in one
parent naming the same directory satisfy the rule; verification's
[reference stream](#verification) refuses them, and ordinary reading
traverses the shared subtree twice within the session's work budget.

### Names

Names are raw bytes with no defined encoding; producers write what the
source host stores, normally UTF-8. EROFS declares the
[portable name profile](design-directories.md#omitted-names): a name that is
strict UTF-8 without control bytes is delivered, and any other natively
legal name makes its entry omitted rather than the volume unreadable. The
namespace key is the raw name, compared bytewise, with no normalization or
case folding, which is how the kernel looks names up.

## Extended attributes

The reader validates extended attributes and marks the entries that have
them; it does not deliver their names or values, because the common entry
model has no representation for them.

### Inline region

A nonzero attribute count `c` gives the region `12 + 4 * (c - 1)` bytes after
the fixed inode. Its 12-byte header holds a 32-bit name filter, an 8-bit
shared count `s` and seven reserved bytes. `s` 32-bit shared-entry indexes
follow, then inline entries up to the region's end:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | Name suffix length |
| 1 | 1 | Name index |
| 2 | 2 | Value length |
| 4 | | Name suffix, then value |

Each entry is padded to a multiple of 4 bytes. The shared indexes must fit
the region, and each inline entry with its padding must end within it; any
excess is `FSIMG_ECORRUPT`. Whether producers zero the reserved header bytes
is provisional (E6); the reader requires nothing of them.

### Names

A name index below `0x80` selects a built-in prefix: 1 `user.`, 2
`system.posix_acl_access`, 3 `system.posix_acl_default`, 4 `trusted.`, 6
`security.`. The two ACL indexes name complete attributes and take an empty
suffix. Index `0x80 | k` names the `k`-th long prefix, which must exist. Any
other index is `FSIMG_EUNSUPPORTED`, naming the attribute index, since no
published document defines it.

### Shared pool

A shared index `x` names the entry at byte `xattr_blkaddr * B + 4 * x` of
the volume, or of the metadata box's content under `SHARED_EA_IN_METABOX`,
encoded like an inline entry. The entry lies inside that space. Several
inodes may reference one shared entry.

### Long prefixes

Under `XATTR_PREFIXES` the volume has `xattr_prefix_count` long prefixes.
Each record is a 16-bit length `L`, a base index byte naming a built-in
prefix, then `L - 1` infix bytes, padded to 4 bytes; the full prefix is the
built-in prefix followed by the infix. Under `PLAIN_XATTR_PFX` the table
begins at byte `xattr_prefix_start * 4` of the volume; otherwise it begins
at byte `xattr_prefix_start * 4` of the packed inode's content and is read
through the packed inode's data layout. Both placements are provisional,
E6, and E12 checks whether a metadata box moves either; a table a pinned
producer places elsewhere is `FSIMG_EUNSUPPORTED` until the design records
it. A volume with long prefixes but no packed inode and no
`PLAIN_XATTR_PFX` is `FSIMG_ECORRUPT`. A record whose
base index is not a built-in prefix, or that runs past its region, is
`FSIMG_ECORRUPT`.

### Name filter

Under `XATTR_FILTER`, when the superblock's filter reserved byte is zero,
each inode's header carries a 32-bit filter in which a set bit promises that
no attribute name hashes to that bit. The hash is XXH32 over the name suffix
with seed `0x25BBE08F` plus the name index, taken modulo 32 (provisional:
E6 confirms against producer images before verification checks it). The
reader never uses the filter. Verification checks that no present
attribute's bit is set, since a wrong filter makes the kernel hide an
attribute; if E6 cannot confirm the hash, verification does not check the
filter and reports it unchecked.

### The extra-data mark

An entry whose inode holds at least one shared or inline attribute carries
`FSIMG_META_EXTRA_DATA`. The mark makes the omission visible in
[listings](design-cli.md#listing), `--json` and the extraction loss report
under the [metadata design's rule](design-metadata.md#metadata-fields).
POSIX ACLs, security labels and capabilities are attributes like any
other; none is translated into mode bits.

## Compression

A compressed inode maps logical clusters of its content to physical
clusters, each decoded independently. Compression is a per-inode layout,
not a layer: a regular file, a directory, a symbolic link and the packed
inode may each be compressed.

### Algorithms

| Identifier | Algorithm | Stream | Build |
| --- | --- | --- | --- |
| 0 | LZ4 | One LZ4 block, no frame | Always, the project-owned [LZ4 codec](design-codecs.md) |
| 1 | MicroLZMA | MicroLZMA, no `.xz` or `.lzma` container | Optional liblzma |
| 2 | DEFLATE | Raw DEFLATE, no zlib or gzip framing | Always, the system zlib |
| 3 | Zstandard | Zstandard, framing provisional (E9) | Optional libzstd |

A physical cluster whose algorithm this build lacks is
`FSIMG_EUNSUPPORTED` naming the library, raised when that content is read.

### Configuration

Without `COMPR_CFGS` the superblock word at `0x54` is LZ4's maximum match
distance and LZ4 is the only algorithm. With it, the word is a mask of the
algorithms present, and one record per set bit, in increasing algorithm
order, follows the superblock and its extension slots, each starting on a
4-byte boundary. A record is a 16-bit payload length and the payload:

| Algorithm | Payload | Fields |
| --- | --- | --- |
| LZ4 | 14 bytes | Maximum distance, maximum physical cluster blocks, reserved |
| MicroLZMA | 14 bytes | Dictionary size, format, reserved |
| DEFLATE | 6 bytes | Window bits 8 to 15, reserved |
| Zstandard | 6 bytes | Format, window log less 10, reserved |

A length other than these, a mask bit above 3, or a record running past the
[superblock region](#volume-identity-and-geometry) is `FSIMG_EUNSUPPORTED`;
a record running past the end of a volume shorter than 4096 bytes is
`FSIMG_ECORRUPT`, a truncation. A MicroLZMA dictionary above
8 MiB or a Zstandard window above 2^20 bytes is `FSIMG_EUNSUPPORTED`; these
are the producers' limits [H1], provisional until E9 confirms them. An
inode naming an algorithm absent from the mask is `FSIMG_ECORRUPT`.

### Map header

The 8-byte map header sits at the attribute region's end rounded up to 8:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | Fragment offset, or a reserved 16-bit word and the 16-bit compressed tail size |
| 4 | 2 | Advice flags |
| 6 | 1 | Algorithms: the head-1 algorithm in bits 0 to 3, head-2 in bits 4 to 7 |
| 7 | 1 | Cluster bits: `lclusterbits - blkszbits` in bits 0 to 3; bit 7 marks a whole-file fragment |

When bit 7 of byte 7 is set, the whole file lives in the packed inode: the
header, with that bit cleared, is a 64-bit offset into the packed inode's
content, and there is no index. Otherwise the logical cluster size is
`L = 2^lclusterbits`, at least `B` and at most 2^20 bytes (provisional, E8;
a larger value is `FSIMG_EUNSUPPORTED`).

| Advice bit | Meaning |
| --- | --- |
| `0x01` | Compact index with 2-byte groups permitted |
| `0x02` | Head-1 clusters may be big |
| `0x04` | Head-2 clusters may be big |
| `0x08` | The last physical cluster is a compressed inline tail |
| `0x10` | Plain clusters are interlaced |
| `0x20` | The last physical cluster is a fragment |

On a full-index inode of a 48-bit volume, advice bit `0x01` instead selects
[encoded extents](#encoded-extents), and bits `0x02` and `0x04` then give
the extent record size. Other advice bits are reserved; any of them set is
`FSIMG_EUNSUPPORTED`.

### Index records

Each logical cluster `i` has one record of type 0 plain, 1 head-1, 2
non-head or 3 head-2. A head or plain record starts a physical cluster at
logical offset `i * L + clusterofs`, with `clusterofs < L`, and names its
first block. A non-head record belongs to the cluster begun by the nearest
head before it and carries two distances: back to that head's logical
cluster and forward to the next head's. The physical cluster's logical
extent runs from its head to the next head, or to `size`.

A physical cluster is one block, unless its algorithm's advice bit permits
big clusters; a big cluster's first non-head record carries `0x800` in its
backward distance field with the cluster's block count in the low 11 bits,
and its backward distance is then one. A count of zero, or one for which the
algorithm's maximum cluster blocks do not allow it, is `FSIMG_ECORRUPT`.
A big-cluster marker without the advice bit is `FSIMG_ECORRUPT`.

**Full index.** Records are 8 bytes, starting 16 bytes after the map
header's start, so 8 bytes after its end:
a 16-bit advice word with the type in bits 0 to 1 and a partial-reference
mark in bit 15, a 16-bit `clusterofs`, and either a 32-bit block number on a
head or plain record or two 16-bit distances on a non-head record.

**Compact index.** Records are packed in groups, each ending in a 32-bit
anchor block number; a group of 2 records in 8 bytes, or, under advice bit
`0x01`, of 16 records in 32 bytes. A record's field is 16 bits in an 8-byte
group and 14 bits in a 32-byte group; it holds, from its low end, `lobits =
max(lclusterbits, 12)` bits of value and then 2 bits of type. On a head the
value is `clusterofs`; on a non-head it is the backward distance, except in a
group's last record, where it is the forward distance. A head's block number
is the anchor plus the blocks of the heads before it in the group, a big
cluster contributing its count. Groups are laid out from the header's end:
first 8-byte groups until the offset reaches a multiple of 32, then 32-byte
groups while 16 or more records remain, then 8-byte groups. 32-byte groups
need `lclusterbits` at most 12 and 8-byte groups at most 14; the inode is
`FSIMG_ECORRUPT` otherwise. The packing rules are provisional and E8
confirms each boundary against `dump.erofs -e` before the code relies on
them.

**Lookup.** Mapping logical offset `o < size` reads record `floor(o / L)`. A
head whose start lies at or below `o` begins the cluster. A head whose start
lies above `o` sends the lookup back one record, and a non-head sends it
back by its backward distance, until a head at or below `o` is found. The
cluster ends at the next head after it, found by the forward distance, or at
`size`. A distance of zero, a backward walk that passes record 0, or one
that would take more steps than there are records, is `FSIMG_ECORRUPT`; the
number of steps is bounded by the record count.

### Physical clusters

A head-1 or head-2 cluster holds a compressed stream that decodes to the
cluster's logical extent. Under `LZ4_0PADDING`, and for every algorithm
other than LZ4, the stream is right-aligned in the cluster's blocks with
leading zero bytes; the decoder starts at the first nonzero byte. Without it
an LZ4 stream starts at the cluster's first byte and may be followed by
padding. A stream that decodes to fewer bytes than its extent is
`FSIMG_ECORRUPT`, and so, outside a partial reference, is one that would
write beyond it; a stream may stop early only where its
algorithm's framing allows the decoder to know it is complete (provisional
per algorithm, E8 and E9).

A partial reference (bit 15 of a full record, and its compact equivalent)
marks a cluster whose stream decodes to more than this file's extent,
because the cluster is shared. Nothing on disk records the stream's full
length, so the decoder decodes partially: it produces the extent's length
and stops, without requiring the stream to end there, and a stream that
ends before the extent is full is `FSIMG_ECORRUPT` (provisional, E8; if E8
finds no producer image with a partial reference, the rule stands as
written, since it is the only reading the format permits).

A plain cluster stores its extent uncompressed in its blocks. Its extent
starts at the cluster's first byte (shifted), or, under advice bit `0x10`,
is rotated by `clusterofs` within the cluster (interlaced); the rotation is
provisional and E8 confirms it. A plain cluster longer than its blocks is
`FSIMG_ECORRUPT`.

### Compressed inline tail

Under advice bit `0x08` the last physical cluster is stored in the body,
after the index, `h_idata_size` bytes long, within the block that holds the
inode's start. A tail that crosses that block, or a size of zero, is
`FSIMG_ECORRUPT`.

### Packed fragments

Under `FRAGMENTS` the packed inode, named by the superblock's packed NID,
is a regular file whose content holds fragments of other files. A file
whose last cluster is a fragment (advice bit `0x20`) reads its last bytes,
from the end of its last ordinary cluster to `size`, from the packed
inode's content at the header's fragment offset; a whole-file fragment
reads all of `[0, size)` from there. The packed inode is read through its
own data layout, which may be compressed, and its fragments are never
fragments of it: a packed inode that uses a fragment, or a fragment range
past the packed inode's size, is `FSIMG_ECORRUPT`. The packed inode is not in
the namespace.

### Shared clusters

Under `DEDUPE` several inodes may name the same physical cluster, and two
clusters may overlap. The reader decodes whatever cluster a record names;
sharing changes nothing for reading and is permitted by verification.

### Limits

A physical cluster is at most 1 MiB stored and its logical extent at most
12 MiB decoded, the producers' interoperability limits [H1], provisional
until E8 confirms them. A larger cluster is `FSIMG_EUNSUPPORTED`. The decoded
extent is charged to the context when it is decoded, so a volume whose
clusters exceed the session's memory budget fails with the budget error,
naming the cluster size, never as corruption.

## 48-bit layout

Incompatible bit `0x080` widens block numbers past 32 bits and adds two
compact encodings. It changes these fields, and no others; a field this list
does not widen keeps its core meaning. Every rule in this section is
provisional until E10 checks it against `mkfs.erofs -E 48bit` images of a
pinned release and a kernel that mounts them; a rule E10 cannot confirm is
refused as `FSIMG_EUNSUPPORTED` naming it rather than guessed.

* **Superblock.** The root NID is the 64-bit field at `0x70`, and the
  16-bit field at `0x0E` holds the block count's bits 32 to 47.
* **Start blocks.** A flat inode's start block is the layout word plus, as
  bits 32 to 47, the 16-bit word at offset `0x06`: the extended inode's
  reserved word, or the compact inode's link count when that inode
  carries the implicit link count below.
* **Implicit link count.** On a compact non-directory inode, format-word bit
  4 means the link count is 1 and the link-count word holds start-block bits
  instead.
* **Compact times.** A compact inode's word at `0x0C` is an unsigned
  second count added to the epoch, with the fixed nanoseconds; the sum is
  checked.
* **Omitted dot.** On a directory, format-word bit 4 means its `.` entry is
  absent, [above](#order-and-dot-entries).
* **Chunks.** Chunk format bit 6 permits 48-bit block numbers in 8-byte
  entries, [above](#chunk-based).
* **Encoded extents** replace the full index's records when advice bit
  `0x01` is set, [below](#encoded-extents).

A wide block number is still checked against the address domain, so a volume
needs no more backing bytes than its structures reach; a 48-bit fixture can
be a sparse file. A NID, offset or block count that does not fit the
checked 64-bit arithmetic is `FSIMG_ECORRUPT`.

### Encoded extents

An encoded-extent inode stores a list of extents in place of index records.
The map header holds a 48-bit extent count: its low 32 bits in bytes 0 to
3 and its high 16 bits in bytes 6 to 7, around the advice word at bytes 4 to
5 that selects encoded extents (provisional, E10). The record size
is `4 << ((advice >> 1) & 3)`: 4, 8, 16 or 32 bytes. Records start at the
first multiple of the record size at or after the map header's end. The
32-byte record holds a 32-bit physical length word, a 64-bit physical byte
offset in two 32-bit halves, a 64-bit logical byte offset in two halves,
and 12 reserved bytes. Smaller records omit fields that a rule supplies
instead: the 16-byte record has explicit logical offsets, the 8-byte record
places extents at logical-cluster spacing, and the 4-byte record stores
only lengths and derives each physical offset by accumulation from a
leading position. The 32-byte record needs no derivation, so the reader
always accepts it. The exact derivation for each smaller width is
established by E10 from every width a pinned producer emits; a smaller
width no producer emits is `FSIMG_EUNSUPPORTED`.

The physical length word's low 21 bits are the stored byte length, bit 27
marks a partial reference, and bits 28 to 31 are the format: 0 for a plain
extent, otherwise the algorithm plus one. Logical offsets increase strictly;
an extent runs to the next extent's offset or to `size`. Under advice bit
`0x20` the last extent is a fragment whose offset field addresses the
packed inode. A count larger than the records the body holds, a stored
length beyond the [cluster limit](#limits), or a decreasing logical offset
is `FSIMG_ECORRUPT`. A lookup is a binary search over logical offsets, so
its cost is logarithmic in the extent count with one record read per step.

## Metadata box

Incompatible bit `0x100` adds a second metadata space. Its rules are
provisional until E12 checks them against `mkfs.erofs` metadata-box images
of a pinned release, plain and compressed, and a kernel that mounts them;
a rule E12 cannot confirm is refused as `FSIMG_EUNSUPPORTED`.

* The metadata-box inode is named by the superblock's field at `0x80`,
  which needs at least one extension slot. Its NID has bit 63 clear: the
  inode itself lives in the primary space.
* The second space is the metadata-box inode's content, read through its
  own data layout, which may be flat or compressed but never a fragment.
  An inode whose NID has bit 63 set begins at byte `32 * n'` of that
  content, where `n'` is the NID without bit 63, and its whole body,
  including any inline tail, is read from that content. An inline tail may
  not cross a multiple of `B` within that content.
* Under `SHARED_EA_IN_METABOX` the shared attribute pool begins at byte
  `xattr_blkaddr * B` of the metadata box's content rather than of the
  volume. Without `PLAIN_XATTR_PFX`, the long-prefix table lives in the
  packed inode as in the core profile.
* An inode's data blocks, chunks and physical clusters are always in the
  primary volume, wherever the inode lives.
* The metadata-box inode and the packed inode never live in the second
  space and are never fragments. Resolving a metadata-box NID therefore
  never needs the metadata box, and the nesting is at most one level: an
  inode in the box, read through the box's layout, whose clusters lie in
  the volume.

A NID with bit 63 set in a volume without the metadata box is
`FSIMG_ECORRUPT`, as is a NID whose inode lies outside its space.

## External data devices

A nonzero extra-device count marks a volume whose chunks or clusters may
live in other backing files, named by opaque tags in a device table. Such a
volume is recognized and refused at open with `FSIMG_EUNSUPPORTED`: fsimg
has no input for a second backing file, and a device tag is never resolved
as a host path.

## Verification

Verification walks the whole namespace through the common walker, reads
every regular file's and symbolic link's content to its size, and then
checks the cross-structure rules below. Every structure the walk touches is
validated as reading validates it.

* **Superblock.** The checksum, when present; nonzero reserved bytes,
  which mark the result incomplete; the inode
  count against the number of distinct reachable NIDs plus the packed inode,
  reported as a difference and not a failure, since producers differ on what
  they count (provisional, E2).
* **References.** Every directory entry contributes a reference to its
  child NID. Each directory NID is referenced exactly once outside its own
  `.` and its children's `..`; each non-directory NID is referenced as many
  times as its link count. A directory's link count is checked against two
  plus its subdirectories when E4 shows producers write that rule; otherwise
  it is not checked, and the report says so.
* **Ownership.** Every reachable structure claims its byte range in one of
  three classes. Exclusive metadata: the superblock with its extension slots
  and configuration records, `[1024, end of the last record)`; each distinct
  inode's body in the primary space; the long-prefix table when
  `PLAIN_XATTR_PFX` makes it a plain region (otherwise it is content of the
  packed inode, whose clusters already claim it as data); every directory's
  stored blocks; and the metadata box's stored blocks. Shared metadata: each
  referenced shared attribute entry in the primary space. Data: every flat
  block, chunk and physical cluster of regular files, symbolic links and the
  packed inode. Inode bodies and shared entries inside the metadata box
  claim byte ranges of its content in a second claim stream under the same
  rules. An exclusive range may not overlap any other claim; a
  shared-metadata range may overlap only an identical shared-metadata range;
  data ranges may overlap each other. A NID named several times claims its
  body once.
* **Content.** Every compressed cluster a file references decodes to its
  extent; every plain cluster fits; every fragment lies in the packed
  inode. A cluster with an unsupported codec makes the report incomplete
  and is counted.
* **Attributes.** Every shared index names an entry inside the pool; every
  long-prefix reference names a prefix; the name filter, when E6 confirms its
  hash.

Verification reports the volume's structure and decodability, never
authenticity: the superblock checksum protects one region, and EROFS
carries no content digest. It reports the reachable structures; bytes no
structure references are not examined, since the format keeps no record of
them.

## Failure classification

| Condition | Status |
| --- | --- |
| No magic at offset 1024, or a prefix too short to hold it | `FSIMG_EFORMAT` |
| A recognized volume using external data devices, a 48-bit or metadata-box rule E10 or E12 left unconfirmed, an unknown incompatible bit, an unknown attribute index or configuration, a block size or cluster size beyond the reader's range, a codec the build lacks, a symbolic-link target over 1024 bytes | `FSIMG_EUNSUPPORTED`, naming the feature |
| A checksum mismatch; a field outside its range; a structure outside the domain or crossing a boundary it must not cross; reserved bits set outside the superblock; unordered or repeated names; a parent-rule violation; a decode that fails or produces the wrong length; a referenced structure past the end of a truncated volume | `FSIMG_ECORRUPT` |
| Memory, scratch, work or time exhausted, including a decoded cluster above the budget | The budget's error, never `FSIMG_ECORRUPT` |
| An I/O failure | The I/O error |

Salvage extraction steps over an unreadable entry as for every format and
never emits bytes from a cluster that failed to decode.

## Reader profile

| Common field | Source |
| --- | --- |
| Kind | The mode's type |
| Size | The inode's size |
| `uid`, `gid` | Owner and group |
| `mode` | The permission bits |
| `modified` or `changed` | The inode's time, by [`MTIME`](#times) |
| `dev_major`, `dev_minor` | Devices only |
| `FSIMG_META_EXTRA_DATA` | Entries with extended attributes |

Volume information reports the label, UUID, block size, block count, inode
count, epoch and both feature words with their decoded names, unknown bits
included.

## Writer profile

The writer builds one layout, deterministic for a given catalog, settings and
epoch. It writes no extended attributes, hard links, device nodes or
sockets, which the source catalog cannot express. Hard links in the source
become independent files under the [sources design](design-sources.md).

### Volume

* `B = 4096` unless `allocation_unit_bytes` selects 512, 1024 or 2048.
  The smaller sizes are provisional: the writer accepts them only once E11
  shows that the Linux kernel in the check environment mounts writer output
  at that size; if E11 cannot answer, the writer accepts only 4096.
* Bytes 0 to 1023 are zero. The superblock has no extension slots, except
  the one the [metadata-box profile](#metadata-box-profile) needs. Its
  compatible features are `SB_CHKSUM` and `MTIME`; its incompatible
  features are none, plus `LZ4_0PADDING` when the [LZ4 profile](#lz4-profile)
  compresses at least one file, `48BIT` under the
  [48-bit profile](#48-bit-profile) and `METABOX` under the
  [metadata-box profile](#metadata-box-profile).
* `meta_blkaddr` is 0, so a NID is the inode's byte offset divided by 32.
  The metadata area begins at the first 32-byte boundary at or after the
  superblock's end: byte 1152, NID 36, without slots, and byte 1184, NID
  37, with the box's one slot. Without the box the root's inode is the
  first, so the root NID always fits its 16-bit field; under the box the
  root is the box content's first inode and its NID is in the wide field.
* The block count is the volume length divided by `B`; the inode count is
  the number of inodes written; the shared-attribute block, extra devices,
  long prefixes and packed NID are zero.
* The epoch and fixed nanoseconds are the format time, the UTC host clock
  sampled once during planning, or the reproducible epoch with zero
  nanoseconds under `-R`; the [48-bit profile](#48-bit-profile) chooses the
  epoch differently.
* The checksum is computed last over the final bytes, under the rule E1
  establishes.

### Layout

The volume is the 1024 zero bytes and the superblock, the metadata area and
the data area, in that order:

1. **Metadata area.** Inodes in the catalog's deterministic order, root
   first. Each starts on the next 32-byte boundary. An inode whose body holds
   an inline tail lies entirely inside one block; one that would cross a
   block boundary starts at the next block instead. An inode without a tail
   starts on the next 32-byte boundary at which its fixed part does not cross
   a block, and its index, if any, may continue across blocks. The area ends
   at a block boundary.
2. **Data area.** Each inode's stored blocks, contiguous and in the same
   order: a flat inode's full blocks, a directory's full blocks, an LZ4
   inode's physical clusters.

No range is shared, so the writer's output passes verification's ownership
rule with every claim exclusive.

### Inodes

An inode is compact when its size fits 32 bits, its owner and group fit 16
bits, its link count fits 16 bits and its modification time equals the
format time to the nanosecond; otherwise it is extended. Under `-R` every
time is the epoch, so every inode of a reproducible volume whose owners fit
is compact, except a directory under the [48-bit profile](#48-bit-profile).
Reserved fields are zero. The serial number is the inode's 1-based position
in the metadata area. A file's link count is 1, a directory's is two plus
its subdirectories.

A regular file, directory or symbolic link with `t = size mod B` nonzero
uses the flat-inline layout when its fixed part and `t` fit one block, and
the flat layout otherwise, with its tail in its own data block. A size that
is a multiple of `B`, including zero, uses the flat layout; an empty inode's
start block is 0 (provisional, E3: if producers and the kernel require
another value for an empty inode, the writer writes theirs). FIFOs use the
flat layout with size and layout word zero.

### Directories

A directory's entries are its children plus `.` and `..`, sorted bytewise.
Blocks are filled in order: an entry goes into the current block while the
block's entries and names fit `B` bytes, else it starts the next block.
Unused bytes of a block are zero, so a full block's last name ends at its
first NUL or the block's end. The directory's size covers its full blocks
and the used bytes of its last block. Every entry's file type is set from
the child's kind. Names are the catalog's bytes, at most 255 per component.

### LZ4 profile

Under `compression=lz4` the writer compresses each regular file whose
compressed layout stores fewer blocks than its flat layout would, counting
an inline tail as no block. Other files, directories and symbolic links keep
the flat layouts. A compressed file uses:

* layout 1, the full index, with `L = B`, head-1 algorithm LZ4, no head-2,
  advice word zero;
* physical clusters of one block, each holding the LZ4 encoding of the
  longest prefix of the remaining input, at most 64 KiB, whose encoding
  fits `B` bytes, right-aligned with leading zeros under `LZ4_0PADDING`.
  The 64 KiB cap keeps every decoded extent far inside the readers' 12 MiB
  limit and bounds the encoder's input window; it is provisional, and E8
  records the largest decoded extent the pinned producer gives a one-block
  cluster, which the cap may rise to; if E8 cannot answer, 64 KiB stays;
* a plain cluster when not even the next `B` input bytes encode into one
  block: those `B` bytes stored raw (shifted), or the remaining bytes when
  fewer remain;
* the layout word holding the file's physical block count;
* no compressed tail, fragment, sharing, big cluster or configuration
  record.

The encoder is the project-owned LZ4 encoder of the
[codec design](design-codecs.md), deterministic for its input; the LZ4
maximum distance field is written as zero, which readers take as LZ4's own
maximum of 65535 and which bounds every distance the encoder produces in its
64 KiB window. The value is provisional: E8 records what producer images
write, and if E8 cannot answer, zero stays. Whether a cluster is
compressed depends only on the file's bytes, so the layout, and the image,
are reproducible.

### 48-bit profile

Under `addressing=48` the writer sets `48BIT` and:

* writes the root NID in the wide field and the block count's high bits at
  `0x0E`, so volumes beyond 2^32 blocks can be built;
* writes start blocks above 2^32 with their high bits at offset `0x06`,
  setting the implicit link count on every compact non-directory inode, whose
  link count is 1 in any case; every directory is written extended, since
  the implicit rule cannot carry its link count, so an inode's form follows
  from the catalog alone and never from where its data lands;
* chooses the epoch as the smallest modification second among the
  catalog's inodes, absent times counted as the format time, with fixed
  nanoseconds zero, and writes an inode compact when its time is a whole
  second within 2^32 seconds of the epoch and its other fields fit;
* under `compression=lz4`, writes compressed files as encoded extents of
  32-byte records, because a full index record cannot hold a block number
  past 32 bits, and the 32-byte record is the one width that carries both
  the physical and the logical offset at 64 bits, as the cluster former's
  variable-length clusters in files beyond 4 GiB need. E10 confirms that
  the kernel accepts it; if E10 cannot answer, the writer refuses
  `compression=lz4` and `metabox=lz4` with `addressing=48`;
* never omits `.` entries.

The profile is never chosen automatically, not even when a volume would need
it; a plan that needs more than 32-bit block numbers without it fails with
`FSIMG_ERANGE` naming the setting.

### Metadata-box profile

Under `metabox=plain` or `metabox=lz4` the writer sets `METABOX` with one
superblock extension slot and moves every inode except the metadata-box
inode into the box:

* The primary metadata area holds only the metadata-box inode, at NID 37,
  the first slot after the superblock and its extension slot.
  The box's content is laid out as the core metadata area would be, from
  byte 0, root first; every directory entry names its child with bit 63
  set.
* The root's NID carries bit 63, which only the wide root field can hold, so
  the profile requires `addressing=48`; a request without it is
  `FSIMG_EINVAL` naming both settings. This is provisional: if E12 shows
  that the kernel mounts a box with the root in the primary space without
  `48BIT`, the writer keeps the root there when `addressing=32`.
* The box's stored blocks follow the data area, so no data address depends
  on the box's stored size; under `metabox=lz4` the box is compressed with
  the LZ4 profile's encoder, and the box inode maps it with 32-byte encoded
  extents, as every LZ4 file under the [48-bit profile](#48-bit-profile) is.
  The box inode's body reserves room for `ceil(content / B)` records, the
  most the cluster former can produce, since each cluster consumes at least
  `B` input bytes or ends the content; the header's count holds the real
  number of extents, and the reserved bytes after the last record are zero.
  The primary metadata area's end, and so every data address, is therefore
  fixed before the box is encoded.
* Directory and file data stay in the primary data area, and there are no
  shared attributes to place. No inode has an inline tail, so the box holds
  no source bytes and is generated from the plan alone; a tail goes in its
  own data block.

The box's content is generated from the plan, not read from a source, so its
compressed size is measured by encoding it once during planning and
reproduced by encoding it again during emission.

### Metadata and defaults

Owner and group are stored exactly; absent, they are 0. The mode is stored
exactly; absent, it is `0755` for directories and symbolic links and `0644`
for files and FIFOs. The modification time is stored exactly in an extended
inode, and as the format time in a compact one, which is exact by the
compact rule. An absent time takes the format time. Access, creation and
change times are not stored and are counted as optional losses under the
[metadata-loss design](design-metadata-loss.md).

### Settings and sizing

| Setting | Rule |
| --- | --- |
| `allocation_unit_bytes` | 0, 512, 1024, 2048 or 4096 subject to E11; 0 selects 4096 |
| `volume_bytes` | 0, or a multiple of `B` at least the plan's length; the block count is `V / B` and the excess is zero |
| `label` | At most 15 bytes, stored raw and NUL-padded to 16 |
| `volume_uuid` | Stored verbatim; unset in reproducible mode, it follows the [identifier rule](design-layers.md#identifiers) with the EROFS volume role; unset otherwise, `FSIMG_EINVAL`, and the CLI supplies a random version 4 UUID |
| `epoch` | The format time in reproducible mode, from `-e`: any signed 64-bit second count. The superblock's epoch field is a different value: it holds the format time, and under `erofs_addressing=48` the smallest modification second instead |
| `erofs_compression` | `none`, the default, or `lz4` |
| `erofs_addressing` | `32`, the default, or `48` |
| `erofs_metabox` | `none`, the default, `plain` or `lz4`; not `none` requires `erofs_addressing=48` (provisional, E12); `lz4` also depends on E10's confirmation of 32-byte records |

The table names settings by their field in the write settings; the
[driver design](design-erofs-driver.md#writer-settings) gives each field's
CLI key or flag, and the prose names a setting by its key, as in
`compression=lz4`.

The image is exactly its plan, so automatic sizing has nothing to search, and
minimization searches the writer's accepted block sizes, 4096 first.

## Resource requirements

* Reading holds one directory block, the inode headers it needs, the range
  of a body a lookup reads, and the decode windows; each is bounded by `B`,
  one index group or record, or a decoded cluster. A body is never held
  whole, so its length is bounded only by the address domain it lies in:
  a large file's index costs reads, not memory. An inode in the metadata box
  is read through the box's decode window, and the box's own inode is
  decoded once at open and kept.
* No operation reads the whole metadata area, the whole index of a large
  file, or the whole namespace into memory. A compact-index lookup reads one
  group, and a backward walk reads at most the records it steps over.
* Verification's reference and ownership streams use the common external
  sort under the scratch budget.
* The writer's plan rows live in scratch-backed paged arrays indexed by
  catalog position; emission holds one directory block, one LZ4 window and
  one output block.

## Acceptance and independent validation

| Family | Valid witnesses | Invalid witnesses |
| --- | --- | --- |
| Geometry | Block sizes 512 to 4096; checksum present and absent; superblock with extension slots; volume inside GPT and a compressed outer layer | Short prefix, half magic, illegal block size, `dirblkbits` set, checksum flipped on each side of the boundary; a block count past the volume, which opens and lists what lies in the volume and fails verification as truncated |
| Features | Every compatible bit; every accepted incompatible bit | Each unknown bit, 48-bit, metadata box, extra devices |
| Inodes | Compact, extended and both in one volume; sizes 0, 1, 31, 32, B-1, B, B+1, 2B and above 4 GiB through a sparse source; every kind; owners above 65535; times before 1970, past 2038 and with nanoseconds | Reserved bits, layouts 5 to 7, nanoseconds out of range, unknown mode type, body past the domain, tail crossing a block |
| Directories | 1- and 255-byte names, invalid UTF-8, names sorting before `.`, one directory of 100000 entries, deep trees | Name offset zero, unaligned or too large; repeated, unordered or overlapping names; slash or NUL; bad type; type mismatch; parent-rule violations; a second name for a directory |
| Attributes | Inline, shared, long prefixes in both placements, filter on and off, ACLs and security labels | Overlong entries, bad shared index, unknown index, bad prefix |
| Chunks | Both entry widths, several chunk sizes, holes, shared chunks | Device identifiers, high parts, short index, chunk past the domain |
| Compression | Each algorithm; full and compact indexes across group boundaries; big, plain and interlaced clusters; partial references; inline tails; fragments; compressed directories; reads starting and ending inside a cluster | Bad distances and cycles, bad group packing, marker without big clusters, oversize clusters, truncated and corrupt streams, fragment past the packed inode |
| 48-bit layout | Root NID above 65535; start blocks and chunk blocks above 2^32 through sparse fixtures; compact per-inode times; implicit link counts; omitted dots; every encoded-extent width a producer emits | High bits without the feature; wide root zero; encoded extents unordered, overcounted or overlong; a time sum past 64 bits |
| Metadata box | Plain and compressed boxes; directories, files and shared attributes inside; reads across box cluster boundaries | Box NID with bit 63 or naming a fragment; box inode in the box; tail crossing a box block; bit 63 without the feature |
| Verification | Every producer fixture verifies | Every mutation above reports its class; overlapping metadata, a wrong link count |
| Writer | Empty, small, deep, wide, large-file volumes; every kind it writes; every profile and their combinations; byte identity across source order, budget, scratch and time zone; a poison scan of every unwritten byte | Settings out of range; a source changed between planning and emission |

Independent oracles, in order of authority: a Linux kernel mount in the
Linux check environment, comparing every file's bytes and metadata with the
source manifest; `fsck.erofs --extract` of the pinned `erofs-utils`
release; then the independent `rust-fs-erofs` reader, which shares no code
with `erofs-utils`. `dump.erofs` records layouts and extents for the
evidence stage. The `erofs-utils` tools share one library and count as one
opinion. `rust-fs-erofs` refuses the 48-bit layout and the metadata box, so
for those the kernel and `fsck.erofs` are the only oracles, and the plan
pins both. The writer capability, and later each of its LZ4, 48-bit and
metadata-box profiles, is registered only after its output passes
`fsck.erofs --extract` and mounts in the Linux kernel with every file's
bytes and metadata matching the source.
Self round trips are necessary and never sufficient.

## Sources

| Key | Source | Standing |
| --- | --- | --- |
| D1 | EROFS on-disk format documentation (`erofs.docs.kernel.org`): core structures, extended attributes, chunk-based files | Public documentation under CC-BY-SA-4.0; facts paraphrased, no prose or figures copied |
| H1 | `erofs-utils` `include/erofs_fs.h` at the pinned release | MIT-licensed header; constants and offsets restated as facts, no declarations copied |
| U1 | `erofs-utils` (`mkfs.erofs`, `fsck.erofs`, `dump.erofs`) at the pinned release | External oracles and fixture producers, run on disposable images |
| K1 | The Linux kernel's EROFS implementation in the Linux check environment | External oracle by mounting only; its source is GPL and is not read for code |
| R1 | `rust-fs-erofs` | MIT; independent external reader; no code imported |
| X1 | XXH32 and LZ4 block format descriptions by their author | Public format descriptions; the codec design's clean-room procedure applies |
| T1 | [Research draft](../fsspecs/EROFS_fsimg_spec.md) | Original project research; not authoritative |
