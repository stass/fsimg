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
  experiments (E1 through E15), produced by a pinned `mkfs.erofs` [U1] or,
  for E15, hand-built from its skeleton, and checked by `fsck.erofs`,
  `dump.erofs` and a Linux kernel mount [K1]. An
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
| Core | Block sizes 512 to 4096 ([geometry](#volume-identity-and-geometry)); compact and extended inodes, a compact inode's time counted from the superblock's epoch; flat and flat-inline data; every POSIX kind; extended attributes validated and marked as extra data | Emitted, 512- to 4096-byte blocks, flat and flat-inline layouts, compact and extended inodes, no extended attributes |
| Chunked | Chunk-based files with 4-byte and 8-byte indexes and holes, primary device only | Never emitted |
| Compressed | Full and compact indexes, big physical clusters, plain clusters, compressed inline tails, packed fragments, shared clusters and compressed directories; LZ4 and DEFLATE always, MicroLZMA and Zstandard when the build has the optional library | LZ4 with full indexes and one-block physical clusters, under the `compression=lz4` setting |
| 48-bit layout | Wide block numbers and root NID, implicit link counts, directories without `.`, byte-addressed encoded extents | Emitted under `addressing=48`: wide fields and compact times, encoded extents for LZ4 files, no omitted dots |
| Metadata box | Inodes and shared attributes in a second metadata space carried by a file, plain or compressed | Emitted under `metabox=plain` or `metabox=lz4` |
| External data devices | Recognized and refused at open | Never emitted |

The 48-bit layout and the metadata box are experimental upstream. E10 and
E12 confirm their rules below against `mkfs.erofs` 1.9.4 images and the
Linux 7.0 kernel that mounts them (`e10-*`, `e12-*`); the few forms neither
experiment observed are marked open where they stand, with a refusal as
their default. Neither layout is ever emitted unless the caller asks for it.

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

**Block size.** `B = 2^blkszbits`. The reader accepts `blkszbits` 9 to 12;
a larger value is `FSIMG_EUNSUPPORTED`. No pinned producer emits a larger
block: `mkfs.erofs` 1.9.4 refuses `-b8192`, and the kernel on 4 KiB pages
refuses `blkszbits` 13, 14 and 16 as it refuses 8 (E1, `e1-geom-b512` to
`-b4096`). `dirblkbits` must be zero; another value is
`FSIMG_EUNSUPPORTED`, since no published layout defines it.

**Superblock region.** The superblock, its extension slots and its
[configuration records](#configuration) lie in `[1024, 4096)`, the bytes
Linux reads with the superblock: one 4 KiB page less the 1024-byte offset.
The region is independent of `B`, so a 512- or 1024-byte-block volume keeps
its superblock outside block 0. For the slots this is the kernel's own
bound: 184 slots, ending at byte 4096, mount at 512-, 1024- and 4096-byte
blocks and 185 are refused (E1, `e1-slot-base-*`, kernel with 4 KiB pages).
For the records it is fsimg policy: the kernel reads configuration records
past byte 4096, but no pinned producer writes one (`mkfs.erofs` 1.9.4 writes
none past the first 1.2 KiB: E1; all four of its records on a superblock
without slots end at byte 1200: E9, `e9-config-four`), and bounding them
keeps open's read one bounded prefix. Extension
slots that end past byte 4096, or a configuration record that does, are
`FSIMG_EUNSUPPORTED`. A structure running past the end of a volume shorter
than 4096 bytes is `FSIMG_ECORRUPT`.

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
the reader core reports. Every pinned producer writes `blocks * B` equal to
its image length (E1, every fixture family). The kernel ignores the count
and mounts a volume whose count is smaller than the data in use; fsimg
refuses such a volume's data as `FSIMG_ECORRUPT` by policy, since the
declared length is the bounded I/O domain and no producer writes a count its
data exceeds.

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
| `0x0E` | 2 | root NID | The root directory's NID; under the 48-bit layout's [wide form](#48-bit-layout), the block count's high 16 bits instead |
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
| `0x6C` | 4 | Build time | Reported, below |
| `0x70` | 8 | Wide root NID | Nonzero under the 48-bit layout selects its [wide form](#48-bit-layout) and is the root's NID; zero otherwise |
| `0x78` | 8 | Reserved | Zero |
| `0x80` | 8 | Metadata-box NID | In the first extension slot; the [metadata box](#metadata-box) inode |
| `0x88` | 8 | Reserved | Zero, when an extension slot covers it |

The four bytes at `0x6C` hold the build time in seconds after the epoch.
`mkfs.erofs` 1.9.4 writes it only under the 48-bit layout, where the epoch is
zero, so it is the absolute build time there; every other pinned producer
writes zero (E1, `e1-bt-*`, `e2-v12-*`, `e2-v15-*`). The reader reports the
word and accepts any value, and the writer writes zero. A nonzero reserved
superblock byte does not prevent
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
`[1024, B)` when `B > 1024` and `[1024, 1024 + B)` otherwise, with the
checksum field read as zero, the register seeded with all ones and stored
without the final inversion: for the input `123456789` the published check
value `0xE3069283` is the inverted register, and the stored form is
`0x1CF96D7C`. The kernel and `fsck.erofs` agree on every point: a flipped
bit anywhere inside the range is refused and one just past it is accepted
at every block size (E1, `e1-chk-b512` to `-b4096`, 46 single-bit flips).
[D1]'s description of the range as `[1024, 1024 + B)` for every `B` is
wrong at 2048 and 4096.

A mismatch is `FSIMG_ECORRUPT` at open: a reader that ignored it would accept
a superblock the kernel refuses to mount. A volume without `SB_CHKSUM` is
read without a checksum, and verification reports that the superblock is
unprotected. The checksum covers the bytes to the range's end, whatever they
hold, so on a 4096-byte-block volume the inodes and data in block 0 are
covered and on a 512-byte-block volume the range ends inside the first
metadata blocks; it is not an integrity claim about the volume. The writer
therefore computes it after every byte of the range is final.

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
metadata space without `METABOX`. The kernel does not check every pairing:
it reads a head-2 record on a volume without `COMPR_HEAD2` (E9,
`e9-head2-lzma`), a chunk-based inode on a volume without `CHUNKED_FILE`
(E14, `e14-tar-index-b4096`), and encoded extents and omitted dots on a
volume without `48BIT` (E10, `e10-edit-lz4-no48`; E12,
`e12-flat-box32-lz4`, whose box is mapped by encoded extents). fsimg
requires the bit by policy: the bit is
what tells a reader, before the first inode, which structures to expect, and
no pinned-producer volume reaches the rule without it. The `--tar=i` volume
is refused at open for its nonzero extra-device count before any inode is
read, and every other pinned-producer volume sets the bit of each structure
it carries. A refused feature is refused at open, before the root is read,
so no partial view of a refused volume exists.

## Inodes

### Addressing

The inode with NID `n`, bit 63 clear, begins at byte
`meta_blkaddr * B + 32 * n` of the volume, computed with checked arithmetic.
With bit 63 set, it begins at byte `32 * (n with bit 63 cleared)` of the
[metadata box's](#metadata-box) content. A NID is a slot index, not an inode
number, and a directory entry's NID is the only key that finds an inode. NIDs
are 64-bit in every layout: without the 48-bit layout an inode may still lie
beyond slot 2^32, as `mkfs.erofs` places inodes after their data (E13,
`e13-limits-nid`), and nothing limits a NID to 32 bits. The root NID comes
from the superblock: the wide field when the 48-bit layout's wide form
selects it, otherwise the 16-bit field, on a metadata-box volume too. A
reader accepts any `meta_blkaddr` whose metadata area lies in the
volume; `mkfs.erofs` moves it from 0 only when the root would otherwise pass
the 16-bit field (source), and the writer keeps it 0. An inode's fixed part,
attribute region and layout bytes lie inside the address domain; the fixed
part and attribute region may cross a block boundary, but an inline tail may
not (see [flat data](#flat-and-flat-inline)).

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
| `0x0C` | 4 | Time, unsigned seconds after the superblock's epoch |
| `0x10` | 4 | Layout word: start block, compressed block count, device number or chunk format |
| `0x14` | 4 | Serial number |
| `0x18` | 2 | Owner |
| `0x1A` | 2 | Group |
| `0x1C` | 4 | Reserved, zero |

A compact inode's time is the superblock's epoch plus the unsigned 32-bit
word at `0x0C`, with the superblock's fixed nanoseconds, on every volume;
the sum is checked. That is how the kernel reads the word, with or without
the 48-bit layout and with or without `MTIME` (E2, hand-edited copies of
`e2-alltime`). Producers without the 48-bit layout write zero there, since
they choose the extended form for any time other than the epoch, so the
word is nonzero only on 48-bit volumes (`e1-bt-48bit`, `e2-vm-48bit`); a
nonzero word is a time, never corruption. The kernel ignores the word at
`0x1C` and the extended inode's reserved bytes; every pinned producer writes
them zero (E2, 3202 inodes), and the reader requires that by fsimg policy: a
nonzero one is `FSIMG_ECORRUPT`.

### Extended inode

| Offset | Size | Field |
| --- | --- | --- |
| `0x00` | 2 | Format word |
| `0x02` | 2 | Attribute count |
| `0x04` | 2 | Mode |
| `0x06` | 2 | Reserved, zero; under the [48-bit layout](#48-bit-layout), the high bits of the start block, or of the compressed block count on a compressed inode |
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

An inode's size is a 64-bit count with no bound of its own and no class of
its own. The structures it implies, a flat inode's blocks, a chunk-based
inode's index and chunks and a compressed inode's `ceil(size / B)` records,
must lie inside the domain, which the body rule above and each layout's
extent rules already require, and every count derived from the size is
checked 64-bit arithmetic whose overflow is `FSIMG_ECORRUPT`. A size is
therefore corrupt exactly when a structure it implies breaks a rule, and a
size no structure contradicts is accepted whatever its magnitude.

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
  20 to 31, so `0xFFFFFFFF` is the legal device 4095:1048575. The kernel
  decodes twelve `mknod` values, majors to 4095 and minors to 1048575, to
  the numbers the source holds (E5, `e5-kinds-b4096`); the reader delivers
  `dev_major` and `dev_minor` as decoded, under the
  [device-node design](design-device-nodes.md).

A device, FIFO or socket uses layout 0; another layout, or a nonzero size,
is `FSIMG_ECORRUPT`. `mkfs.erofs` 1.9.4 writes size 0, layout 0 and, for
FIFOs and sockets, a zero layout word, compact and extended alike (E5,
`e5-kinds-*`). Such an inode has no start block, so where the word at
`0x06` carries start-block bits 32 to 47 under the
[48-bit layout](#48-bit-layout), on an extended inode or on a compact inode
with format-word bit 4, it is zero: a nonzero one is `FSIMG_ECORRUPT`, a
reserved field required zero. On a compact inode with bit 4 clear the word
is the link count, under the link-count rules. `mkfs.erofs` never sets bit
4 on such an inode: it writes the extended word zero and the compact link
count explicitly, on a 48-bit volume too (E10, `e10-48-kinds`, compact and
extended FIFOs, sockets and devices; `e10-48-large`).

### Times

EROFS stores one time per inode. The kernel presents it as the access,
change and modification time alike; fsimg delivers only what the format
states. Under `MTIME` the stored time is the source's modification time and
is delivered as `modified`. Without `MTIME` the stored time is delivered as
`changed`: `mkfs.erofs` v1.2, the producer that omits the bit, stores the
source's change time in an extended inode and the volume's build time, the
epoch, in a compact inode, so a compact inode's time is then the volume's
and not the source's (E2, `e2-v12-*`). The kernel presents one time either
way and ignores the bit. Access and creation times are not stored. Times are
UTC; an extended inode's time has nanosecond precision, and a compact
inode's, whole seconds after the epoch sharing the epoch's nanoseconds, has
one-second precision.

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
`startblk` is the layout word. Its blocks lie inside the address domain.

A flat-inline inode stores its first `floor(size / B)` blocks the same way
and its last `size mod B` bytes, the tail, in its body. The tail lies inside
one block: a tail that crosses a block boundary is `FSIMG_ECORRUPT`, and
one that ends exactly at the block's end is not, which is the kernel's rule
to the byte (E3, `sBm33-tail-fits-exact` and `-crosses`). The inode's fixed
part and attribute region before it may lie in an earlier block. A
flat-inline inode whose `size` is a nonzero multiple of `B` is
`FSIMG_ECORRUPT`: the kernel and `fsck.erofs` read its last block as the
tail and refuse it as inline data across blocks, and producers write layout
0 for such sizes (E3, `e3-*`). Size 0 with either layout is accepted.

The layout word of a flat inode is read whenever `size > 0`, since every
byte of a flat inode lies in its blocks, a one-byte file included (E3,
`e3-b4096-noinline`, whose one-byte file is flat with start block 1). The
word of a flat-inline inode is read when the inode has a full block,
`floor(size / B) > 0`. Otherwise, in an empty inode of either layout and in
a flat-inline inode shorter than `B`, the word addresses nothing and is
neither read nor validated: every pinned producer writes `0xFFFFFFFF` there
in a flat-inline inode under one block and 0 in an empty file, and the
kernel reads neither (E3, `e3-b4096-inline`, where 21 of 25 flat-inline
inodes carry `0xFFFFFFFF`; `e2-v12-default`, `e2-v15-default`). Under the
[48-bit layout](#48-bit-layout) the unused start is 48 bits wide, and an
extended inode's high word is then `0xFFFF`, unread like the rest (E10,
`e10-48-kinds`, the extended flat-inline inodes under one block). Where the
word is read it is an address like any other, and one outside the domain,
`0xFFFFFFFF` included, is `FSIMG_ECORRUPT`. For `0xFFFFFFFF` in a flat
inode with content, and for the 48-bit null with it, that is fsimg policy: the kernel reads such a file as
zeros (E3), and no producer writes the word in a flat inode.

### Chunk-based

The low 16 bits of a chunk-based inode's layout word are its chunk format:
bits 0 to 4 are `chunkbits`, bit 5 selects 8-byte index entries, bit 6
permits the wide block numbers of the [48-bit layout](#48-bit-layout) and
requires it, and bits 7 to 15 are reserved and zero; a set one is
`FSIMG_ECORRUPT`, and the kernel refuses it too (E7,
`e7-edit-format-0x80`). The high 16 bits are unspecified, not reserved:
the reader and the verifier ignore them, since `mkfs.erofs` 1.9.4 writes
uninitialised bytes there, so two builds of one tree differ in them (E7,
`e7-chunk8k` and `e7-chunk8k-again`), and a byte comparison against its
output must exclude them. The chunk
size is `B << chunkbits`, at most 2^43 bytes by the fields' widths, so it
needs no bound of its own; `chunkbits` is
per inode and not the producer's requested chunk size, since `mkfs.erofs`
raises it to the largest power of two dividing every run of contiguous
chunks, and a wholly sparse file is one hole chunk of its size rounded up to
a power of two (E7, `e7-chunk4k`). The file has
`ceil(size / chunk)` chunks, and the index starts at the attribute region's
end rounded up to the entry width:

* a 4-byte entry is a block number;
* an 8-byte entry is a 16-bit high part, zero unless chunk format bit 6
  widens the block number to 48 bits, a 16-bit device identifier, which is
  zero because the volume has no extra devices, and a 32-bit block number.

A block number of all ones at the block number's width, the 32-bit field,
or the 48 bits of the high part and the field under chunk format bit 6, is a
hole: that chunk reads as zeros, delivered as a
[zero extent](design-content.md#zero-extents). Without chunk format bit 6 an
8-byte hole entry is therefore a zero high part and device identifier
before `0xFFFFFFFF` (E7, `e7-chunk8k-forceidx`); under bit 6 it is a high
part of `0xFFFF`, a zero device identifier and `0xFFFFFFFF`. Any other chunk occupies
`min(chunk, size - offset)` bytes from `block * B`, inside the domain (E7,
`e7-chunk*`, hole and shared entries included; E10, `e10-48-chunk`, whose
entry `01 00 00 00 05 00 00 00` under bit 6 names block 2^32 + 5, the data
moved there by hand because `mkfs.erofs` places no chunk past 2^32, and the
kernel and `fsck.erofs` read it). A nonzero high part without
bit 6, bit 6 without `48BIT`, bit 6 with 4-byte entries, or a nonzero
device identifier, is `FSIMG_ECORRUPT`. That is fsimg policy, stricter than
the kernel and `fsck.erofs`, which ignore the high part and the device
identifier and accept bit 6 on any volume (E7, `e7-edit-high-part`,
`-device-id`, `-format-0x40`) and, by their source, with either entry
width: no pinned fixture
holds any of them on a volume without extra devices, a device identifier
names a backing file fsimg does not have, the high part is address bits the
volume says are not there, and a 4-byte entry has no high part for bit 6 to
widen. `mkfs.erofs` sets bit 6 whenever a chunk's block number exceeds 32
bits, with either entry width, and a 4-byte entry then stores only the low
32 bits (source `blobchunk.c`), so bit 6 with 4-byte entries marks block
numbers the entries lost, not a wider encoding. Chunks may share blocks
with each other and with other files.

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
Producers fill a block's unused bytes with zeros (E4, every block of
`e4-dirs-b4096`); the reader requires nothing of them.

The file type is 0 unknown, 1 regular file, 2 directory, 3 character device,
4 block device, 5 FIFO, 6 socket, 7 symbolic link. Any other value is
`FSIMG_ECORRUPT`. A nonzero type that disagrees with the child inode's mode is
`FSIMG_ECORRUPT`; type 0 defers to the mode.

### Order and dot entries

Names, including `.` and `..`, increase strictly in byte order across the
whole directory: within each block and from each block's last name to the
next block's first. A repeated or decreasing name is `FSIMG_ECORRUPT`. Dot
entries are ordinary records in that order, not necessarily first; a name
such as `-a` sorts before them (E4, `e4-dirs-b4096`, where `.` is the
eighth entry of the root and the kernel looks every name up).

`.` names the directory itself and `..` its parent; the root's `..` names the
root. Neither is delivered as a child. Under the 48-bit layout a directory
whose format word has bit 4 set omits `.`; the reader treats it as present
and requires that no record is named `.`. `mkfs.erofs -E dot-omitted`
writes `..` and the names with no `.`, and the kernel appends a synthetic
`.` after the stored entries, listing a stored `.` twice when the bit is set
anyway, so a stored `.` under the bit is wrong by the kernel's reading too
(E10, `e10-48-dotomit` and a hand-edited `e10-48-small`).

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
source host stores, normally UTF-8, and `mkfs.erofs`, `fsck.erofs` and the
kernel all accept invalid UTF-8 and control bytes (E4, `e4-dirs-b4096`).
EROFS declares the
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
excess is `FSIMG_ECORRUPT`. Producers zero the reserved header bytes and
the entry padding (E6, every inode of the `e6-xattr-*` fixtures); the reader
requires nothing of them.

### Names

A name index below `0x80` selects a built-in prefix: 1 `user.`, 2
`system.posix_acl_access`, 3 `system.posix_acl_default`, 4 `trusted.`, 6
`security.`. The two ACL indexes name complete attributes and take an empty
suffix; a nonempty one is `FSIMG_ECORRUPT`. Index `0x80 | k` names the
`k`-th long prefix, which must exist. Any other index is
`FSIMG_EUNSUPPORTED`, naming the attribute index, since no published
document defines it.

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
through the packed inode's data layout, decoded like any content when that
inode is compressed. Both placements are observed: `mkfs.erofs` sets
`PLAIN_XATTR_PFX` itself whenever it writes no packed inode and puts the
table directly after the shared pool (E6, `e6-xattr-pfx-plain`, byte 1188 =
297 * 4), and under `fragments` it puts the table at offset 0 of an
LZ4-compressed packed inode (`e6-xattr-pfx-packed`). Under the
[metadata box](#metadata-box) without `PLAIN_XATTR_PFX` the table begins
instead at byte `xattr_prefix_start * 4` of the box's content, whether or not
a packed inode exists: `mkfs.erofs` writes it at offset 0 of the box, padded
to whole box blocks so the root inode follows at NID bit 63 plus
`k * B / 32` after `k` table blocks, one in every observed image, and the
kernel reads it there (E12, `e12-box-pfx-plain`, `-pfx-plain-b512`,
`-pfx-lz4`, `-lz4-pfx-frag`; `-pfx-lz4-plainpfx` keeps the plain
placement). A volume with long prefixes but no packed inode, no metadata
box and no `PLAIN_XATTR_PFX` is `FSIMG_ECORRUPT`. Because an incompatible
bit can move the table, a reader judges these placements only after
refusing the incompatible bits it does not implement. A record whose
base index is not a built-in prefix, or that runs past its region, is
`FSIMG_ECORRUPT`.

### Name filter

Under `XATTR_FILTER`, when the superblock's filter reserved byte is zero,
each inode's header carries a 32-bit filter in which a set bit promises that
no attribute name hashes to that bit. The hash is XXH32 with seed
`0x25BBE08F` plus the index of the name's built-in prefix, over the name
with that built-in prefix removed, taken modulo 32: for an index below
`0x80` the stored suffix; for a long prefix `0x80 | k`, the `k`-th prefix's
infix followed by the stored suffix, with the seed taking the prefix's base
index. Every attribute, shared and inline, counts. A zero filter word
promises nothing; it is what producers write with the feature off and when
every bit is claimed. The rule reproduces the filter of every inode in every
producer image (E6, 26 of 26 inodes each in `e6-xattr-default`,
`-shared0`, `-pfx-plain` and `-pfx-explicit-plain`; the suffix alone fails
on 4 of 26 with long prefixes). The reader never uses the filter.
Verification checks that no present attribute's bit is set, since a wrong
filter makes the kernel hide an attribute. A nonzero filter byte turns the
filters off, as the kernel treats it; verification of such a volume under
`XATTR_FILTER` is incomplete, since the byte is reserved and fsimg cannot
claim to know what a producer meant by it.

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
| 0 | LZ4 | One LZ4 block, no frame, ending at the cluster's last byte | Always, the project-owned [LZ4 codec](design-codecs.md) |
| 1 | MicroLZMA | MicroLZMA, no `.xz` or `.lzma` container: the inverted properties byte, then the LZMA1 stream without its leading zero byte and without an end mark, decoded to the extent's length | Optional liblzma |
| 2 | DEFLATE | Raw DEFLATE, no zlib or gzip framing, ending at its final block | Always, the system zlib |
| 3 | Zstandard | One complete Zstandard frame with its content size and no checksum | Optional libzstd |

Each framing is confirmed by decoding every cluster of the producer fixtures
with a library independent of `erofs-utils` and the kernel (E9, `e9-lzma-*`,
`e9-deflate-*`, `e9-zstd-*`). A physical cluster whose algorithm this build
lacks is `FSIMG_EUNSUPPORTED` naming the library, raised when that content
is read.

### Configuration

Without `COMPR_CFGS` the superblock word at `0x54` is LZ4's maximum match
distance and LZ4 is the only algorithm. With it, the word is a mask of the
algorithms present, and one record per set bit, in increasing algorithm
order, follows the superblock and its extension slots, each starting on a
4-byte boundary. A record is a 16-bit payload length and the payload:

| Algorithm | Payload | Fields |
| --- | --- | --- |
| LZ4 | 14 bytes | Maximum distance (16 bits), maximum physical cluster blocks (16 bits), 10 reserved bytes |
| MicroLZMA | 14 bytes | Dictionary size 4096 to 8 MiB (32 bits), format 0 (16 bits), 8 reserved bytes |
| DEFLATE | 6 bytes | Window bits at most 15 (8 bits), 5 reserved bytes |
| Zstandard | 6 bytes | Format 0 (8 bits), window log less 10 at most 10 (8 bits), 4 reserved bytes |

A record's reserved bytes are treated as the superblock's: a nonzero one
does not prevent reading, `i` reports it, and verification of the volume is
incomplete (E9 records every producer record with them zero).

A length other than these, a mask bit above 3, or a record running past the
[superblock region](#volume-identity-and-geometry) is `FSIMG_EUNSUPPORTED`;
a record running past the end of a volume shorter than 4096 bytes is
`FSIMG_ECORRUPT`, a truncation. A field outside the bounds in the table is
`FSIMG_EUNSUPPORTED`: they are the kernel's limits, which it reports as
invalid, and the pinned producer's maxima, which both oracles read at the
bounds (E9, `e9-lzma-dict8m`, `e9-zstd-dict1m`, `e9-deflate-dict128` with
window bits 7). The MicroLZMA lower bound of 4096 rests on the kernel's
source alone: `mkfs.erofs` refuses to write a smaller dictionary (E9), so no
fixture carries one. The producer derives every one of these values from
its cluster size (E9), but only the DEFLATE window is no stream parameter:
raw DEFLATE inflates with a 32 KiB window whatever the record says. The
MicroLZMA dictionary size and the Zstandard window log bound the decoder's
memory, so the reader hands liblzma the record's dictionary size, or the
extent's length when that is smaller (at least 4096: a fresh decoder cannot
reach back further than it has produced), and libzstd
a `ZSTD_d_windowLogMax` of the record's window log plus 10, at most 20,
charges that dictionary or window to the context before the first decode
([decode windows](design-erofs-driver.md#resource-bounds)), and refuses a
stream that needs more as `FSIMG_ECORRUPT`. Without the record's bound a
hostile frame could declare a window the library allocates outside the
budget: libzstd's own default admits 128 MiB. An
inode naming an algorithm absent from the mask is `FSIMG_ECORRUPT`. The mask
lists only configured algorithms, so a `-zlzma` volume has mask `0x2` and no
LZ4 record; `COMPR_CFGS` is set for any non-LZ4 codec or big cluster (E9,
`e9-config-four`, `e9-mixed-lz4-lzma`). An empty mask under `COMPR_CFGS`
needs no rule of its own: it configures no algorithm, so every compressed
inode on such a volume is `FSIMG_ECORRUPT` by the rule above and an
uncompressed one reads as any other; no producer writes it and the kernel
accepts it.

### Map header

The 8-byte map header sits at the attribute region's end rounded up to 8:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | Fragment offset bits 0 to 31 (bits 32 to 63 are the fragment head's block field on a full index, [below](#packed-fragments)), or a reserved 16-bit word and the 16-bit compressed tail size |
| 4 | 2 | Advice flags |
| 6 | 1 | Algorithms: the head-1 algorithm in bits 0 to 3, head-2 in bits 4 to 7 |
| 7 | 1 | Cluster bits: `lclusterbits - blkszbits` in bits 0 to 3; bit 7 marks a whole-file fragment; bits 4 to 6 carry nothing and are not read on an index. On an [encoded-extent](#encoded-extents) inode of 4- or 8-byte records bits 4 to 6 are zero, else `FSIMG_ECORRUPT`; with 16- or 32-byte records bytes 6 and 7 are the extent count's high half. Bit 7 keeps its whole-file meaning in every form and is tested first, as the kernel does, so a count with bit 47 set is a whole-file fragment |

When bit 7 of byte 7 is set, the whole file lives in the packed inode: the
header, with that bit cleared, is a 64-bit offset into the packed inode's
content, there is no index, and the inode's body ends at the header. A
whole-file fragment is read only in an inode of the full-index layout,
the only one producers write it in (E8, `e8-lz4-allfrag`, `e8-lz4-zdir`);
in a compact-index inode it is `FSIMG_EUNSUPPORTED` naming the
combination, since the readers' agreement there is unchecked. Otherwise the
logical cluster size is
`L = 2^lclusterbits`, and on a full or compact index the reader requires
`L = B` as fsimg policy:
every pinned producer writes `lclusterbits = blkszbits` there (E8) and none
can write another value, so accepting one would add rules no volume needs.
Another value is `FSIMG_EUNSUPPORTED` naming the logical cluster size.
[Encoded extents](#encoded-extents) of 4 and 8 bytes have their own
logical cluster rule, since `mkfs.erofs` writes them with `L > B`. With
`L = B` the record formats' own ceilings, `L` at most 2^16 for a full
record's `clusterofs` and `lclusterbits` at most 14, or 12 with 32-byte
groups, for the compact index, hold on every accepted volume. The kernel,
`fsck.erofs` and `dump.erofs` do read `L = 2B` and `4B` on full and compact
indexes (E15, hand-built `e15-a-*`), and the rules they follow are recorded
here so that lifting the refusal is a known change: each record describes
one logical cluster of `L` bytes and `ceil(size / L)` records are read, the
last holding the file's last byte; the compact group layout, the initial
8-byte run, the 32-byte groups and the final 8-byte groups, follows
`ceil(size / B)` (`totalidx` in `zmap.c`; groups laid out for the `L` count
fail in the kernel and both tools, `e15-a-compact-b2048-l4096-adv1-lcount`);
compact fields are `max(lclusterbits, 12)` bits wide, so a compact inode
with `L > 4096` needs advice `0x01` clear, since a 32-byte group's 14-bit
field holds only 12 bits of value (the kernel returns `EOPNOTSUPP`,
`e15-a-compact-b4096-l8192-adv1flip`); and the inline tail follows record
`(size - 1) >> lclusterbits`, or the end of the compact group holding it
(the full index observed, `e15-c-l2b-tail-at-L`; the compact index from
the kernel's source).

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
the extent record size; on a compact inode the bit keeps its 2-byte-group
meaning there too, and a 48-bit volume holds compact and full indexes
beside encoded extents, since `mkfs.erofs` picks the smallest form per file
(E10, `e10-48-lz4`: `half-rand-1m` compact, `half-rand-40k` encoded). On a
compact inode bits `0x02` and `0x04` are equal,
both set or both clear, as the kernel requires; unequal is `FSIMG_ECORRUPT`.
`mkfs.erofs` writes `0x07` on every compact inode with big clusters or a
non-LZ4 codec and `0x02` alone on full-index inodes (E8, `e8-lz4-big*`; E9).
Other advice bits are reserved; any of them set is `FSIMG_EUNSUPPORTED`.
Bits `0x08` and `0x20` together would give the header's first word two
meanings, a tail size and a fragment offset; no producer writes both and
the reference readers do not agree on one, so the pair is
`FSIMG_EUNSUPPORTED` naming it.

### Index records

Each logical cluster `i` has one record of type 0 plain, 1 head-1, 2
non-head or 3 head-2. A head or plain record starts a physical cluster at
logical offset `i * L + clusterofs`, with `clusterofs < L`, and names its
first block. A non-head record belongs to the cluster begun by the nearest
head before it and carries two distances: back to that head's logical
cluster and forward to the next head's. There are `ceil(size / L)` records,
which is `ceil(size / B)` because the reader requires `L = B`
([map header](#map-header)). A physical cluster's logical extent runs from
its head to the start of the next head or plain record, or to `size` when
no record
follows, so the last extent ends at `size` by construction. A head or plain
record that starts at `size` begins an empty cluster, which is never read;
one that starts beyond `size` is `FSIMG_ECORRUPT`. The reader needs no
other end rule.

The producer's last record takes one of three forms, all of which the
extent rule covers. When the last physical cluster starts inside the last
logical cluster, its own head or plain record is the last record, with
`clusterofs < size mod L` (`e9-zstd-l3` `/text026`, size 106696, record 26
plain at `clusterofs` 165 holding the final 35 bytes; 36 of the 176 files
in `e8-lz4-compact-counts`; an inline tail in `e2-count-zt`; a fragment in
`e8-lz4-frag`). Otherwise, when `size mod L` is nonzero, the last record is
an end marker, a plain record with `clusterofs = size mod L` that begins an
empty cluster at `size`; and when `size mod L` is zero there is no marker
and the last record is the last cluster's head, plain or non-head record.
Over the 592 indexed inodes E8 tallies across the E2, E8 and E9 fixtures,
69 take the first form, 395 the marker and 128 the third. `mkfs.erofs`
writes records for the logical clusters an extent fully covers and keeps
the offset where the extent stops as pending; a final extent that begins
inside its last logical cluster writes its own head or plain record there
and clears the pending offset, otherwise a nonzero pending offset becomes
the marker, whose block field is 0 on a full index (source `compress.c`).
The kernel ends an extent at `size` once the next record index reaches the
record count (source `zmap.c`).

A physical cluster is one block, unless its type's advice bit permits
big clusters: `0x02` for a head-1 cluster, `0x04` for a head-2 cluster and
for a plain cluster, the pairing of `erofs-utils` (source `zmap.c`). The
producer sets both bits on a compact inode and `0x02` alone on a full-index
inode, where it stores every plain cluster in one block with no record
after it that a marker could occupy (source `compress.c`), so no producer
fixture separates the pairings.
When it does, the first non-head record of every physical
cluster, a one-block cluster included, carries the count marker: `0x800` in
its backward distance field with the cluster's block count in the low 11
bits, its backward distance being one (E8, `e8-lz4-big16k`, where markers
with count 1 occur). A head directly followed by another head or plain
record, or by no record, has no marker and is one block. A first non-head
record without the marker, a count of zero, or one for which the
algorithm's maximum cluster blocks do not allow it, is `FSIMG_ECORRUPT`.
LZ4's maximum is its configuration record's field, where zero means one
block, the reference decoder's reserved case (source `decompress.c`); no
other algorithm's record carries one, so only the
[cluster limit](#limits) bounds it. A
marker without the advice bit is `FSIMG_ECORRUPT`, also where the inode
permits big clusters only for the other type; `erofs-utils` then reads the
cluster as one block without looking at the record after it, a laxity no
producer image needs.

**Full index.** Records are 8 bytes, starting 16 bytes after the map
header's start, so 8 bytes after its end:
a 16-bit advice word with the type in bits 0 to 1 and a partial-reference
mark in bit 15, a 16-bit `clusterofs`, and either a 32-bit block number on a
head or plain record or two 16-bit distances on a non-head record. Bits 2
to 14 of the advice word are reserved; a set one is `FSIMG_EUNSUPPORTED`
naming the record bit, since the kernel's development tree gives bit 14 a
meaning, a compressed hole, that no pinned producer writes. Bit 15 means
nothing on a plain or non-head record and is not judged there.

**Compact index.** Records are packed in groups, each ending in a 32-bit
anchor block number; a group of 2 records in 8 bytes, or, under advice bit
`0x01`, of 16 records in 32 bytes. A record's field is 16 bits in an 8-byte
group and 14 bits in a 32-byte group; it holds, from its low end, 12 bits of
value (the kernel's `max(lclusterbits, 12)`, which `L = B` fixes at 12) and
then 2 bits of type. On a head the value is `clusterofs`; on a non-head it
is the backward distance, except in a group's last slot, which holds the
forward distance instead; a count marker in the last slot keeps its
meaning, since the marker bit is tested first. The last slot's backward distance is
derived from the slot before it: 1 when that slot is a head or plain record,
2 when it is a count marker, and otherwise that slot's backward distance
plus one. Groups are laid out from the header's end: first 8-byte groups
until the offset reaches a multiple of 32, then 32-byte groups while 16 or
more records remain, then 8-byte groups; an odd final record occupies a
whole 8-byte group whose second slot is zero and never read. The packing
agrees with `dump.erofs -e` at every header phase and every record count
from 2 to 71 (E8, `e8-lz4-compact-counts`, `e8-lz4-compact-xattr`, 348
files). A compact inode with advice bit `0x01` clear lays every record in
8-byte groups from the header's end, with no initial run and no 32-byte
groups, under the anchor, last-slot and block rules above; the kernel,
`fsck.erofs` and `dump.erofs` read one (E15, hand-built `e15-b-compact-*`,
non-head records in every slot). It is `FSIMG_EUNSUPPORTED` naming the
advice as fsimg policy: no producer writes one, since `mkfs.erofs` writes
`lclusterbits = blkszbits`, at most 12, and sets `0x01` on every compact
inode with `lclusterbits` at or below 12 (source `compress.c`).

A head or plain record's block number is its group's anchor plus a sum
gathered by walking back over the records before it in the same group. The
anchor is the running block counter `mkfs.erofs` carries into the group.
Without big clusters it is the block of the last head or plain record
before the group, or one less than the first cluster's block for the first
group. With big clusters it is the start block of the first physical
cluster not yet accounted for when the group begins, where a cluster is
accounted for once its count marker, or its head when it has no marker,
lies before the group's first record: a group that begins inside a cluster
whose marker lies in the group has that cluster's start block as its
anchor, and a group that begins at a head, or inside a cluster whose marker
lies in an earlier group, has the block after the previous cluster. The walk
visits the records from the one before the target down to the group's first
record and never leaves the group: a jump that would land before the first
record ends the walk. Without big clusters the sum starts at one, a head or
plain record met adds one, and a non-head record adds nothing and jumps to
its head, `backward distance` records before it. With big clusters
permitted the sum starts at zero: a non-head record carrying the count
marker adds its count and skips the head directly before it; a head or
plain record met directly, so not skipped by a marker, is a one-block
cluster without a marker and adds one; a non-head record without the marker
adds nothing and jumps to its cluster's marker, `backward distance - 1`
records before it. A cluster accounted for in the anchor contributes
nothing more, and a cluster that begins in an earlier group but whose
marker lies in this one adds its count when the marker is met (E8,
`e8-lz4-big16k` `/mixed` record 44, block 25 = anchor 21 + 4;
`e9-mixed-lz4-lzma`). The two forms agree with `dump.erofs -e` on every
file of every compact fixture. The index is `FSIMG_ECORRUPT` when a
non-head record's backward distance is zero; when, with big clusters, a
non-head record without the marker has backward distance one, since the
record directly after a head is that cluster's marker; when a count marker
is directly preceded in its group by a non-head record; or when a jump
inside the group lands on a record of the wrong type, a non-head without
big clusters or anything but a marker with them.

**Lookup.** Mapping logical offset `o < size` reads record `floor(o / L)`. A
head whose start lies at or below `o` begins the cluster. A head whose start
lies above `o` sends the lookup back one record, and a non-head sends it
back by its backward distance, until a head at or below `o` is found. The
cluster ends at the start of the next head or plain record after it, found
by reading the following records one at a time, or at `size`. Every
non-head record of the cluster leads back into it: to its head, or to an
earlier non-head of the cluster, which leads on to the head, a chain an
extent longer than the 2047 records a distance field holds needs (no
producer fixture has one: the longest extent, `e8-lz4-zeros40m`, spans
2000 records). A backward
distance reaching before the head would give a read starting at that
record another cluster than a read starting at the head, so it is
`FSIMG_ECORRUPT`; the reference readers follow it, and no producer writes
one. A zero
backward distance or a backward walk that passes record 0 is
`FSIMG_ECORRUPT`; a walk that would go back, or ahead, further than the
largest decoded extent the [limits](#limits) allow is `FSIMG_EUNSUPPORTED`,
so the steps are bounded by the limit as well as by the record count. The
forward distances non-head records carry, a full record's second distance
and a compact group's last slot, are not read. They only accelerate
finding the next head, and trusting them would let a hostile distance that
skips a head give the bytes after it one cluster for a read starting before
that head and another for a read starting after it; the record types give
every offset exactly one cluster. On producer images the two agree:
`dump.erofs -e`, which follows the distances, reports the mapper's extents
for every compressed file of the E8 fixtures (`test/erofs-fixtures.c`).

### Physical clusters

A head-1 or head-2 cluster holds a compressed stream that decodes to the
cluster's logical extent. Every stream fsimg reads is right-aligned in the
cluster's blocks with leading zero bytes, and the decoder starts at the
first nonzero byte: LZ4 under `LZ4_0PADDING`, and every other algorithm
always (E8, E9). An LZ4 cluster on a volume without `LZ4_0PADDING` is
`FSIMG_EUNSUPPORTED` naming the feature, raised when the content is read
like a codec the build lacks. That is fsimg policy: no pinned producer
writes such a volume (`mkfs.erofs` 1.9.4 sets the bit on every build, `-E
legacy-compress` included), and without the bit nothing records where a
stream ends. The kernel skips leading zero bytes whatever the bit says, so
it reads a bit-cleared volume whose streams are still right-aligned (E8,
`e8-lz4-flag-cleared`), while neither it nor `fsck.erofs` reads a
left-aligned stream shorter than its cluster (E8, `e8-lz4-unpadded`). Every
stream fsimg reads is therefore bounded on both sides: it starts at the
first nonzero byte, and it either ends at the cluster's last byte (LZ4,
which carries no end mark), carries its own end (DEFLATE's final block and
Zstandard's frame end, at or before the cluster's last byte; bytes after
them are not judged) or is decoded until the extent is full (MicroLZMA,
which marks no end). Each decodes to exactly its extent: fewer bytes, or a
stream that would write beyond the extent, is `FSIMG_ECORRUPT`, outside a
partial reference. A compressed cluster, outside a partial reference, whose
extent is shorter than its stored blocks and does not end the file is
`FSIMG_ECORRUPT`, the kernel's rule (E8, source `zmap.c`, "too much
compressed data").

A partial reference (bit 15 of a full record, or bit 27 of an
[encoded extent's](#encoded-extents) length word; the compact index has no
such mark, and producers write shared clusters only with those two forms:
E8, `e8-lz4-dedupe`; E10, `e10-48-lz4-b512`) marks a cluster whose stream decodes to more than this
file's extent, because the cluster is shared. Nothing on disk records the
stream's full length, so the decoder decodes partially: it produces the
extent's length and stops, without requiring the stream to end there, and a
stream that ends before the extent is full is `FSIMG_ECORRUPT`, the
kernel's rule; no producer image has such a stream.

A plain cluster stores its extent uncompressed in its blocks. Its extent
starts at the cluster's first byte (shifted), or, under advice bit `0x10`,
is rotated by `clusterofs` within the cluster (interlaced): extent byte `j`
lies at block byte `(clusterofs + j) mod B` (E8, `e8-lz4-frag`, every plain
cluster). A plain cluster longer than its blocks is `FSIMG_ECORRUPT`, and so
is an interlaced cluster of more than one block, which the rotation does not
define and the reference decoder refuses (source `decompress.c`); the
producer writes plain clusters of one block (source `compress.c`).

### Compressed inline tail

Under advice bit `0x08` the physical cluster holding the file's last byte
is stored in the inode's body: its stored bytes are the `h_idata_size`
bytes that begin at the index end: the byte after the full record, or the
end of the compact group, holding the file's last byte, record
`(size - 1) >> lclusterbits`, which is the last record and group because
the reader requires `L = B` (E15, `e15-c-l2b-tail-at-L` and `-at-B`: with
`L = 2B`, a tail placed after `ceil(size / B)` rather than `ceil(size / L)`
records is read as garbage by the kernel and `fsck.erofs`). The tail lies
within one block. A tail size of zero, or a tail that crosses a block boundary,
is `FSIMG_ECORRUPT`, the kernel's rule. The tail is decoded by its
record's type like any other cluster: a head when the producer compressed
it, a plain cluster when compression gained nothing (E8, `e8-lz4-ztail`, 76
head and 5 plain tails). The tail's record's block number names a block
holding none of this file's bytes and is not read, and neither is a count
marker after it (see [packed fragments](#packed-fragments) for the marker a
producer writes there). The tail's block is not necessarily the block holding
the
inode's start: when an inode starts near a block's end, its index runs
into the next block and the tail follows it there (E8, `e8-lz4-ztail`, 5
of 81 tails, such as `/text008` at `0x23dfe0` with the index ending and
the tail starting at `0x23e030`; `e2-count-zt`).

A plain tail on an interlaced inode, advice `0x10` with the tail's record
of type 0, is `FSIMG_EUNSUPPORTED` naming the combination, which no
producer fixture holds: `mkfs.erofs` never inlines the final extent of a
file that `fragments` may pack, and `fragments` is what sets `0x10`
(source `compress.c`, `may_inline` requires `!may_packing`; 51 files under
`-E ztailpacking,fragments` give whole-file fragments or advice `0x31`,
never `0x08` with `0x10`). The reference readers have no agreed meaning
for one: for a tail of `E` bytes at `clusterofs` `C`, the kernel reads it
correctly only when `E > B - C` and the stored bytes are the extent
rotated left by `B - C`; when `E <= B - C` it copies bytes preceding the
tail in the inode's block into the file, and `fsck.erofs` reads bytes
after the tail in every interlaced case (E15, hand-built `e15-c-il-*`).

### Packed fragments

Under `FRAGMENTS` the packed inode, named by the superblock's packed NID,
is a regular file whose content holds fragments of other files. A file
whose last cluster is a fragment (advice bit `0x20`) reads its last bytes,
from the end of its last ordinary cluster to `size`, from the packed
inode's content at the fragment offset; a whole-file fragment reads all of
`[0, size)` from there. The fragment's own record is a head or plain record
like any other. A count marker after it holds no cluster's length and is
not read, whatever its count: `mkfs.erofs` 1.9.4 with big clusters writes
one of zero blocks after a fragment's head (`e8-lz4-big16k-frag`, made
with `-C16384 -E fragments`, for `/f3` at NID 52 among others), and the
kernel reads no length for a fragment or an inline tail (source `zmap.c`).
A zero count after any other head is `FSIMG_ECORRUPT`. Its block field is no
address: on a full index it holds bits 32 to 63 of the fragment offset,
whose bits 0 to 31 are the map header's field, and the reader assembles the
64-bit offset from both; on a compact index the record stores no block
number and the offset is the header's 32 bits, so a fragment there lies
within the packed inode's first 4 GiB. The producer switches a file to the
full index once its fragment offset passes 32 bits (source `compress.c`),
and the reference decoder assembles the offset so (source `zmap.c`); no
producer fixture reaches that size. The packed inode is read through its
own data layout, which may be compressed, and its fragments are never
fragments of it: a packed inode that uses a fragment, or a fragment range
past the packed inode's size, is `FSIMG_ECORRUPT`. The packed inode is not in
the namespace: [verification](#verification) reports a directory entry
naming it as `FSIMG_ECORRUPT`. A packed NID of 0 names no packed inode,
and neither does any packed NID without `FRAGMENTS`, as the kernel loads
the packed inode (source `super.c`); a fragment on a volume without a
packed inode is `FSIMG_ECORRUPT`, and so is a long-prefix table outside a
plain region on a volume without a packed inode or a metadata box
([long prefixes](#long-prefixes)).

### Shared clusters

Under `DEDUPE` several inodes may name the same physical cluster, and two
clusters may overlap. The reader decodes whatever cluster a record names;
sharing changes nothing for reading and is permitted by verification.

### Limits

A physical cluster is at most 1 MiB stored and its logical extent at most
12 MiB decoded, the kernel's limits [K1], which the pinned producer stays
inside: its largest stored cluster is exactly 1 MiB (E8, E9,
`e8-lz4-words6m-big1m` and the `e9-*-words6m-big1m` fixtures) and its
largest decoded extent 8,192,000 bytes (E8, `e8-lz4-zeros40m-big1m`). A
larger cluster is `FSIMG_EUNSUPPORTED`, except an encoded extent's stored
length, which the [encoded extents](#encoded-extents) make
`FSIMG_ECORRUPT`. The decoded
extent is charged to the context when it is decoded, so a volume whose
clusters exceed the session's memory budget fails with the budget error,
naming the cluster size, never as corruption.

## 48-bit layout

Incompatible bit `0x080` widens block numbers past 32 bits and adds two
compact encodings. It changes these fields, and no others; a field this list
does not widen keeps its core meaning. E10 observed the rules in this
section in `mkfs.erofs -E 48bit` images mounted by the Linux 7.0 kernel
(`e10-48-*`), except where a rule says it rests on source reading, and the
forms listed as open at the end of [encoded extents](#encoded-extents) are
refused as `FSIMG_EUNSUPPORTED` naming them rather than guessed.

* **Superblock.** The superblock has two forms. In the wide form the 64-bit
  field at `0x70` is nonzero and is the root NID, and the 16-bit field at
  `0x0E` holds the block count's bits 32 to 47. When `0x70` is zero, the
  narrow form, `0x0E` is the root NID and the block count is the 32-bit
  field alone. `mkfs.erofs` writes the narrow form until the block count
  passes 32 bits or the root NID passes 16 (`e10-48-small`,
  `e10-48-lz4-big` narrow; `e10-48-large` wide), and the kernel mounts
  both, the wide form on a small volume included (`e10-edit-small-wide`).
  A root NID of 0 cannot select the wide form; no producer places the root
  at NID 0. Without `48BIT` a nonzero `0x70` is a reserved superblock
  field like any other: the kernel ignores it, and the reader reports it
  and marks verification incomplete, as for every nonzero reserved
  superblock byte.
* **Start blocks.** A flat inode's start block is the layout word plus, as
  bits 32 to 47, the 16-bit word at offset `0x06`: the extended inode's
  reserved word, or the compact inode's link count when that inode
  carries the implicit link count below. A compact directory's link count
  stays explicit, bit 4 or not, so its start block is 32 bits, and only an
  extended directory can lie beyond block 2^32; that case is unobserved,
  since `mkfs.erofs` failed to build one (E10), and is read by the uniform
  extended rule.
  An unused start is unread at 48 bits as at 32
  ([flat rules](#flat-and-flat-inline)). On a compressed inode the word
  holds the compressed block count's high bits by source reading, zero in
  every observed image; the reader does not use that count.
* **Implicit link count.** On a compact non-directory inode, format-word bit
  4 means the link count is 1 and the link-count word holds start-block bits
  instead (`e10-48-large`, `nl-c` at block 2^32). `mkfs.erofs` sets the bit
  only when the start block needs it and writes a compact inode with a
  wide start and another link count extended; the kernel accepts the bit
  with zero high bits on every file and symbolic link (`e10-edit-small-nlink1`).
  On an extended non-directory inode the bit has no published meaning and
  is `FSIMG_EUNSUPPORTED` by policy: the kernel and `erofs-utils` ignore it
  there (`e10-edit-kinds-extbit4`) and no producer sets it.
* **Omitted dot.** On a directory, format-word bit 4 means its `.` entry is
  absent, [above](#order-and-dot-entries) (`e10-48-dotomit`).
* **Chunks.** Chunk format bit 6 permits 48-bit block numbers in 8-byte
  entries, [above](#chunk-based) (`e10-48-chunk`); that the kernel needs
  the bit to read the high word is source reading, unobserved.
* **Encoded extents** replace the full index's records on a full-index
  inode whose advice bit `0x01` is set, [below](#encoded-extents).

A wide block number is still checked against the address domain, so a volume
needs no more backing bytes than its structures reach; a 48-bit fixture can
be a sparse file. A NID, offset or block count that does not fit the
checked 64-bit arithmetic is `FSIMG_ECORRUPT`.

### Encoded extents

An encoded-extent inode stores a list of extents in place of index records.
The record size is `4 << ((advice >> 1) & 3)`: 4, 8, 16 or 32 bytes, and
`mkfs.erofs` 1.9.4 emits all four (E10: 4 for uniform contiguous extents,
`half-rand-40k`; 8 for uniform extents made non-contiguous by `dedupe`,
`xyxy`; 16 otherwise, `text200k`; 32 for a file beyond 4 GiB,
`e10-48-lz4-big`). Records start at the first multiple of the record size
at or after the [map header's](#map-header) end. The map header's meaning
depends on the width:

* For 16- and 32-byte records it holds a 48-bit extent count: its low 32
  bits in bytes 0 to 3 and its high 16 bits in bytes 6 to 7, around the
  advice word at bytes 4 to 5. The count is at most `ceil(size / B)`, by
  fsimg policy: every extent but the last covers at least one block of
  content in every observed image and in the writer's output, and the
  bound is checked before the records' extent is computed, so a count can
  never make the body longer than the size allows; a larger count is
  `FSIMG_ECORRUPT`. The kernel uses the high half and never checks the
  count: a high half of 1 reads a file at its full size with wrong content
  (`e10-edit-lz4-counthi`), which that bound refuses when the inode loads.
* For 4- and 8-byte records, header bytes 0 to 3 and 6 are zero, else
  `FSIMG_ECORRUPT`, and bits 0 to 3 of byte 7 are the logical cluster
  shift above the block shift, so the logical cluster is `B << shift`; it
  may exceed `B` (shifts 1, 3 and 4 observed) and is bounded by the
  [decoded extent limit](#limits), beyond which it is `FSIMG_EUNSUPPORTED`
  naming the cluster size. There is no count: the file has
  `ceil(size / logical cluster)` extents, each covering one logical cluster
  and the last covering the rest, so the count is bounded by the size and
  the body it implies must lie in the domain like any other.

The 32-byte record holds a 32-bit physical length word, a 64-bit physical
byte offset in two 32-bit halves, a 64-bit logical byte offset in two halves,
and 12 reserved zero bytes. The 16-byte record holds the length word, the
64-bit physical offset and a 32-bit logical offset. The 8-byte record holds
the length word and a 32-bit physical byte offset; its logical offset is
its index times the logical cluster. The 4-byte record holds the length
word alone: a 64-bit physical byte offset at the records' start precedes
the first record, each extent starts where the previous one ends, and the
logical offsets are as for 8 bytes. Every width is read.

The physical length word's low 21 bits are the stored byte length, bit 27
marks a partial reference, bits 21 to 26 are reserved and zero, and bits 28
to 31 are the format: 0 for a plain extent, otherwise the algorithm plus one
(1 LZ4, 3 DEFLATE observed). The partial bit means what a full record's
bit 15 does, [above](#physical-clusters), and like it is ignored on a plain
extent: `mkfs.erofs -E dedupe` sets it on LZ4 extents that decode a prefix
of a shared stream (`e10-48-lz4-b512`, `/t10-d/tutu` records 1 and 3, read
by the kernel to the manifest). A plain extent is stored shifted; under
the interlaced advice `0x10`, which `mkfs.erofs` sets with fragments
(`e10-48-frag`, advice `0x35`), a plain extent whose physical offset is a
block multiple is interlaced instead, rotated by its logical start within
a block, as the kernel decides it per extent (source `zmap.c`), and as on
an index it is one block, any other length `FSIMG_ECORRUPT`.
Explicit logical offsets start at 0, increase strictly and lie below
`size`; an extent runs to the next extent's offset or to `size`. Under
advice bit `0x20` the last extent is a fragment in the packed inode at
offset `pstart_lo << 32 | length word`, the length
word carrying the offset's low 32 bits and no format or partial bits, and
it runs to `size` (`e10-48-frag`); the 4-byte form has no `pstart_lo`, so
its fragment offset is the length word alone (kernel source), and a
32-byte record whose physical offset passes 32 bits cannot name a 64-bit
offset and is `FSIMG_ECORRUPT`. A whole-file fragment keeps the
[legacy header](#packed-fragments) with advice `0x01` clear. The
inline-tail advice `0x08` has no reading under encoded extents, which the
kernel skips with the header's tail fields, and is `FSIMG_EUNSUPPORTED`
naming the combination. The big-cluster advice bits give the record size
there and need no `COMPR_CFGS`, and a stored length is bounded by the
[cluster limit](#limits) alone: an LZ4 configuration's maximum cluster
blocks bound an index's clusters, whose block counts it describes, not a
byte-granular extent. A body too
short for its records, a stored length beyond the
[cluster limit](#limits), a reserved length bit, or a logical offset out of
order or out of range is `FSIMG_ECORRUPT`. A lookup of 16- or 32-byte
records is a binary search over logical offsets, one record read per step,
and each record a search reads must lie strictly between the offsets of
the records it already read on either side, so two lookups cannot return
overlapping extents whatever the records neither read hold; verification
reads every record once and checks the order whole. A 4- or 8-byte record
is found by its index; a 4-byte record's physical offset is the start
plus every stored length before it, so the reader keeps the last sum it
reached for the inode and a sequential read sums each record once, while
a lookup before that point sums from the first record.
Physical offsets and lengths need not be block multiples: the kernel and
`fsck.erofs` read 32-byte records with exact stored lengths and unaligned
starts (`e10-hand-lz4-exact`, `-bytes`), though `mkfs.erofs` writes only
block multiples.

Open after E10, each `FSIMG_EUNSUPPORTED` naming it until a producer image
or a kernel-mounted hand-built one shows its reading: a length word of
zero, which the kernel reads as an unmapped extent and no producer writes,
and a 16-byte record whose physical high half is nonzero, which no
producer run reached.

## Metadata box

Incompatible bit `0x100` adds a second metadata space. E12 confirms the
rules below for flat and encoded-extent boxes against `mkfs.erofs -m`
images at 512- to 4096-byte blocks, with chunks, fragments and long
prefixes inside, and the Linux 7.0 kernel that mounts them (`e12-box-*`);
the open forms at the end are refused as `FSIMG_EUNSUPPORTED` naming them.

* The metadata-box inode is named by the superblock's field at `0x80`,
  which needs at least one extension slot. Its NID has bit 63 clear: the
  inode itself lives in the primary space. The kernel refuses a box NID
  with bit 63 and a box without a slot at mount (E12).
* The second space is the metadata-box inode's content, read through its
  own data layout: flat, or compressed behind a full or compact index or,
  on a `48BIT` volume, [encoded extents](#encoded-extents). `mkfs.erofs`
  writes a flat box, or an LZ4 box mapped by 4-byte encoded extents (advice
  `0x0001`, logical cluster shift 1) or, at 1024-byte blocks, by a compact
  index (layout 3, advice `0x0001`), and stores the box flat when
  compression would not shrink it (`e12-box-plain`, `e12-box-lz4`,
  `e12-box-lz4-m16k`, `e12-box-lz4-b1024`). A box behind a full index is accepted because the
  [writer](#metadata-box-profile) emits it under `addressing=32`; no
  producer writes one and no kernel has read one, so it is provisional on
  the box-writer stage's qualification like the writer form. An inode
  whose NID has bit 63 set begins at byte `32 * n'` of that content, where
  `n'` is the NID without bit 63, and its whole body, including any inline
  tail, is read from that content. An inline tail may not cross a multiple
  of `B` within that content; the fixed part and attribute region may
  (`e12-box-plain-b512`).
* The box and `48BIT` are independent. `mkfs.erofs` sets both, because it
  puts the root in the box and only the wide root field holds bit 63; the
  kernel and `fsck.erofs` also accept a box volume without `48BIT`, whose
  16-bit field names a root in the primary space (`e12-flat-box32-plain`
  and `-poolbox`, producer images hand-patched at the root; no producer
  writes the form). The reader takes the root from whichever field the
  [superblock form](#48-bit-layout) selects and resolves it in its space.
  `e12-flat-box32-lz4`, the same patch on a box mapped by encoded extents,
  is `FSIMG_ECORRUPT` by the [feature-pairing policy](#features), which the
  kernel does not apply.
* Under `SHARED_EA_IN_METABOX` the shared attribute pool begins at byte
  `xattr_blkaddr * B` of the metadata box's content rather than of the
  volume (`e12-box-plain-poolbox`, hand-built: `mkfs.erofs` 1.9.4 never
  sets the bit and leaves the pool in the primary space). Both placements
  are read by the format; Linux 7.0 returns wrong shared attributes for
  some box inodes under the producer's placement, a kernel defect the
  [research record](../docs/erofs-research.md#e12-metadata-box) describes,
  so fsimg's attributes may differ from that kernel's on such a volume.
* Without `PLAIN_XATTR_PFX` the long-prefix table lives in the box's
  content, [above](#long-prefixes), not in the packed inode.
* An inode's data blocks, chunks and physical clusters are always in the
  primary volume, wherever the inode lives (`e12-box-chunk`,
  `e12-box-lz4`; the box's own stored blocks follow the data).
* The metadata-box inode and the packed inode never live in the second
  space and are never fragments (`e12-box-lz4-frag`: box NID 38, packed
  NID 40). Resolving a metadata-box NID therefore never needs the metadata
  box, and the nesting is at most one level: an inode in the box, read
  through the box's layout, whose clusters lie in the volume.

A NID with bit 63 set in a volume without the metadata box is
`FSIMG_ECORRUPT`, as is a NID whose inode lies outside its space; the kernel
lists such an entry and fails to read it (E12). Open after E12, each
`FSIMG_EUNSUPPORTED` naming it until a producer image or a kernel-mounted
hand-built one shows it read: a box inode with the flat-inline or
chunk-based layout; a packed NID with bit 63; and
`SHARED_EA_IN_METABOX` on a compressed box, behind an index or encoded
extents, whose rule is the same but which no fixture holds. `SHARED_EA_IN_METABOX`
without `METABOX` is `FSIMG_ECORRUPT`; the kernel mounts such a volume and
fails every shared attribute read.

## External data devices

A nonzero extra-device count marks a volume whose chunks or clusters may
live in other backing files, named by opaque tags in a device table. Such a
volume is recognized and refused at open with `FSIMG_EUNSUPPORTED`: fsimg
has no input for a second backing file, and a device tag is never resolved
as a host path. The refusal keys on the count alone, because the count is
the one field both producer forms set: a `--blobdev` volume also sets
`DEVICE_TABLE` and `CHUNKED_FILE`, while a `--tar=i` volume sets
`DEVICE_TABLE` only, uses 512-byte blocks and holds chunk-based files
without `CHUNKED_FILE` (E14, `e14-blobdev-b4096`, `e14-tar-index-b4096`).
Both carry an empty device tag and a nonzero device identifier in every
chunk entry. The refusal runs at open after the probe's own checks (the
magic, the block size, `dirblkbits`, the extension slots and the
incompatible-bit table) and the extension slots' truncation on a volume
shorter than 4096 bytes, and before the checksum and the feature-pairing
and chunk rules.

## Verification

Verification walks the whole namespace through the common walker, reads
every regular file's and symbolic link's content to its size, and then
checks the cross-structure rules below. Every structure the walk touches is
validated as reading validates it.

* **Superblock.** The checksum, when present; nonzero reserved bytes,
  which mark the result incomplete; the inode
  count against the number of distinct reachable NIDs plus the packed inode,
  reported as a difference and not a failure, since producers differ on what
  they count: `mkfs.erofs` 1.9.4 and 1.5 count distinct inodes, a hard link
  once, plus the packed inode, and v1.2 writes 0 (E2, `e2-count-*`,
  `e2-v12-*`).
* **References.** Every directory entry contributes a reference to its
  child NID. No entry may name the packed inode. Each directory NID is
  referenced exactly once outside its own
  `.` and its children's `..`; each non-directory NID is referenced as many
  times as its link count. A directory's link count is two plus its
  subdirectories (E4, every directory of the `e4-*` fixtures), except that
  `mkfs.erofs` 1.9.4 writes 1 on a compact directory inode whose count would
  exceed 65535, and the kernel reports 1; verification accepts 1 there.
* **Ownership.** Every reachable structure claims its byte range in one of
  three classes. Exclusive metadata: the superblock with its extension slots
  and configuration records, `[1024, end of the last record)`; each distinct
  inode's body in the primary space; the long-prefix table when
  `PLAIN_XATTR_PFX` makes it a plain region (otherwise it is content of the
  packed inode, whose clusters already claim it as data, or of the metadata
  box); every directory's stored blocks; and the metadata box's stored
  blocks. Shared metadata: each referenced shared attribute entry in the
  primary space. Data: every flat block, chunk and physical cluster of
  regular files, symbolic links and the packed inode. Inode bodies, shared
  entries and the long-prefix table inside the metadata box claim byte
  ranges of its content in a second claim stream under the same rules. An
  exclusive range may not overlap any other claim; a
  shared-metadata range may overlap only an identical shared-metadata range;
  data ranges may overlap each other. A NID named several times claims its
  body once.
* **Content.** Every compressed cluster a file references decodes to its
  extent; every plain cluster fits; every fragment lies in the packed
  inode. A cluster with an unsupported codec makes the report incomplete
  and is counted.
* **Attributes.** Every shared index names an entry inside the pool; every
  long-prefix reference names a prefix; the [name filter](#name-filter)
  admits every present attribute.

Verification reports the volume's structure and decodability, never
authenticity: the superblock checksum protects one region, and EROFS
carries no content digest. It reports the reachable structures; bytes no
structure references are not examined, since the format keeps no record of
them.

## Failure classification

| Condition | Status |
| --- | --- |
| No magic at offset 1024, or a prefix too short to hold it | `FSIMG_EFORMAT` |
| A recognized volume using external data devices, a 48-bit or metadata-box form its section lists as open, an unknown incompatible bit, a reserved advice or full-record bit, an unknown attribute index or configuration, a block size or cluster size beyond the reader's range, a logical cluster larger than a block on an index or beyond the decoded limit on encoded extents, a codec the build lacks, an LZ4 cluster on a volume without `LZ4_0PADDING`, a symbolic-link target over 1024 bytes | `FSIMG_EUNSUPPORTED`, naming the feature |
| A checksum mismatch; a field outside its range; a structure outside the domain or crossing a boundary it must not cross; a reserved field or bit required zero, outside the superblock and other than the advice and record bits above; unordered or repeated names; a parent-rule violation; a decode that fails or produces the wrong length; a referenced structure past the end of a truncated volume | `FSIMG_ECORRUPT` |
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
  E11 confirms every size: the Linux kernel in the check environment,
  `fsck.erofs` and rust-fs-erofs read the writer's output at 512, 1024,
  2048 and 4096 bytes, including the layout below at its smallest, where
  the [covered range](#superblock-checksum) holds the first inodes and
  inodes end at, or move past, many block ends.
* Bytes 0 to 1023 are zero. The superblock has no extension slots, except
  the one the [metadata-box profile](#metadata-box-profile) needs. Its
  compatible features are `SB_CHKSUM` and `MTIME`; its incompatible
  features are none, plus `LZ4_0PADDING`, with the LZ4 configuration,
  whenever any LZ4 cluster is written, a file's under the
  [LZ4 profile](#lz4-profile) or the box's under `metabox=lz4`, `48BIT`
  under the [48-bit profile](#48-bit-profile) and `METABOX` under the
  [metadata-box profile](#metadata-box-profile).
* `meta_blkaddr` is 0, so a NID is the inode's byte offset divided by 32.
  The metadata area begins at the first 32-byte boundary at or after the
  superblock's end: byte 1152, NID 36, without slots, and byte 1184, NID
  37, with the box's one slot. Without the box the root's inode is the
  first, so the root NID always fits its 16-bit field. Under the box with
  `addressing=48` the root is the box content's first inode and its NID,
  bit 63, is in the wide field; under the box with `addressing=32` the root
  follows the box inode in the primary area and its NID is in the 16-bit
  field ([metadata-box profile](#metadata-box-profile)).
* The block count is the volume length divided by `B`, rounded down: a
  fixed disk fits the volume to 512-byte sectors, so a zero partial block
  may follow the last block. The inode count is the number of inodes
  written; the shared-attribute block, extra devices, long prefixes and
  packed NID are zero.
* The epoch and fixed nanoseconds are the format time, the UTC host clock
  sampled once during planning, or the reproducible epoch with zero
  nanoseconds under `-R`; the [48-bit profile](#48-bit-profile) chooses the
  epoch differently.
* The checksum is computed last, over the final bytes of the
  [covered range](#superblock-checksum), inodes and data in block 0
  included.

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
is a multiple of `B`, including zero, uses the flat layout. An empty inode, or
a flat-inline inode under one block, has an unused start word, written 0:
the kernel does not read it, and `mkfs.erofs` writes 0 for an empty file
(E3, `e3-*`). Every other inode's word is its start block, a flat inode's
tail block included. FIFOs use the flat layout with size and layout word
zero.

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
* physical clusters of one block, each holding the block the LZ4
  encoder's [prefix mode](design-codecs.md#destination-bounded-encoding)
  makes from a window of the next `min(remaining, 64 KiB)` input bytes
  into `B` bytes, the longest prefix of the window its greedy parse can
  end within `B`, right-aligned with leading zeros under `LZ4_0PADDING`.
  The 64 KiB cap is the writer's decision: it keeps every decoded extent
  far inside the readers' 12 MiB limit and bounds the encoder's input
  window, and it lies far below the 1,041,954 decoded bytes the pinned
  producer puts through one block (E8, `e8-lz4-zeros40m`), so a reader
  proven against the producer's one-block clusters accepts the writer's;
* blocks that meet the LZ4 [end-of-block
  conditions](design-codecs.md#end-of-block): the last sequence carries at
  least five literals, the last match starts at least 12 bytes before the
  end of the block's decoded input, and input of fewer than 13 bytes is
  stored as literals only. The kernel and `fsck.erofs` refuse a block that
  breaks either condition (E8, `e8-lz4-block-end`), and every producer block
  meets them;
* a plain cluster when the prefix mode's block holds fewer than
  `min(B, remaining)` input bytes: those bytes stored raw (shifted) at the
  block's start, the rest of the block zero. So every cluster but the last
  holds at least `B` logical bytes, and no two heads share a logical
  cluster. The rule is stated in the encoder's own terms because its greedy
  parse decides the prefix: another parse might fit the next `B` bytes
  where this one does not, and the rule must depend on the file's bytes
  alone;
* the full index's records from the clusters' heads: cluster `i`, starting
  at logical offset `h` and ending at `e`, gives logical cluster `h / B` a
  head-1 record, or a plain record for a plain cluster, with `clusterofs =
  h mod B` and block `start + i`, and each logical cluster from `h / B + 1`
  to `e / B - 1` a non-head record with backward distance to `h / B` and
  forward distance to `e / B`, where the next head or the end marker lies.
  When `size mod B` is nonzero and the last cluster's head is not in the
  last logical cluster, that cluster holds the producer's end marker, a
  plain record with `clusterofs = size mod B` and block 0. No record sets
  the partial-reference mark, since every cluster decodes to exactly its
  extent;
* the layout word holding the file's physical block count;
* no compressed tail, fragment, sharing, big cluster or configuration
  record; the map header is zero: LZ4, algorithm 0, for both heads, no
  advice and a logical cluster of one block. Its index follows the inode's
  fixed part, which the [layout](#layout) keeps in one block, and may
  continue across blocks.

The encoder is the project-owned [LZ4 block
encoder](design-codecs.md#the-lz4-block-encoder) of the codec design,
deterministic for its window and capacity; the LZ4
maximum distance field is written as 65535, LZ4's own maximum, which bounds
every distance the encoder produces in its 64 KiB window and is the value
`mkfs.erofs` writes, so the word matches every producer volume; the kernel
and `fsck.erofs` also read a volume with zero there (E8,
`e8-lz4-distance0`). The field and `LZ4_0PADDING` are written only when a
file is compressed, so a volume none of whose files compresses is the
uncompressed profile's volume byte for byte. Whether a cluster is
compressed depends only on the file's bytes, so the layout, and the image,
are reproducible.

### 48-bit profile

Under `addressing=48` the writer sets `48BIT` and:

* writes the superblock's wide form whatever the volume's size: the root
  NID in the wide field and the block count's high bits at `0x0E`, so
  volumes beyond 2^32 blocks can be built and one rule covers every size.
  `mkfs.erofs` writes the narrow form until a value needs the wide one;
  the kernel mounts a small volume in the wide form (E10,
  `e10-edit-small-wide`);
* writes start blocks above 2^32 with their high bits at offset `0x06`,
  setting the implicit link count on every compact non-directory inode, whose
  link count is 1 in any case, a form the kernel accepts with zero high
  bits too (E10, `e10-edit-small-nlink1`); every directory is written
  extended, since the implicit rule cannot carry its link count, so an
  inode's form follows from the catalog alone and never from where its
  data lands;
* chooses the epoch as the smallest modification second among the
  catalog's inodes, absent times counted as the format time, with fixed
  nanoseconds zero, and writes an inode compact when its time is a whole
  second within 2^32 seconds of the epoch and its other fields fit. That
  is stricter than `mkfs.erofs`, which stores a time outside that range
  modulo 2^32 and drops the nanoseconds, so the kernel reports a wrong
  time (E10, `e10-48-small`, years 1950 and 2110);
* under `compression=lz4`, writes compressed files as encoded extents of
  32-byte records, because a full index record cannot hold a block number
  past 32 bits, and the 32-byte record is the one width that carries both
  the physical and the logical offset at 64 bits, as the cluster former's
  variable-length clusters in files beyond 4 GiB need under this layout;
  without it a compact index of `B`-sized logical clusters covers such a
  file (E13, `e13-big5g-lz4`). The physical offsets and lengths are block
  multiples, as `mkfs.erofs` writes them. E10 confirms that the kernel
  and `fsck.erofs` read 32-byte records around the project's LZ4 blocks,
  plain extents included, placed block-aligned and padded, block-aligned
  with exact lengths, and byte-granular (`e10-hand-lz4-padded`, `-exact`,
  `-bytes`), and an LZ4 box mapped the same way (`e10-hand-box32`);
* never omits `.` entries.

The profile is never chosen automatically, not even when a volume would need
it; a plan that needs more than 32-bit block numbers without it fails with
`FSIMG_ERANGE` naming the setting.

### Metadata-box profile

Under `metabox=plain` or `metabox=lz4` the writer sets `METABOX` with one
superblock extension slot and moves every inode except the metadata-box
inode into the box:

* The primary metadata area holds the metadata-box inode at NID 37, the
  first slot after the superblock and its extension slot (`mkfs.erofs`
  writes its box inode at NID 38, after a shared entry the writer never
  has). The box's content is laid out as the core metadata area would be,
  from byte 0, and every directory entry names a child in the box with bit
  63 set.
* Under `addressing=48` the root is the box content's first inode, in the
  wide root field, and the primary area holds nothing but the box inode:
  the producer's layout (E12, `e12-box-*`).
* Under `addressing=32` the root directory's inode follows the box inode
  in the primary area, so its NID fits the 16-bit field, and every other
  inode is in the box; the root's `.` and `..` entries and its children's
  `..` entries name it without bit 63. The format does not couple the box
  to `48BIT`, and the kernel and `fsck.erofs` read this form on producer
  images hand-patched at the root (E12, `e12-flat-box32-plain`), but no
  producer writes it, so the form is provisional until the box-writer
  stage's kernel qualification mounts the writer's own output; if that
  fails, `metabox` with `addressing=32` is refused as `FSIMG_EUNSUPPORTED`
  naming both settings.
* The box's stored blocks follow the data area, so no data address depends
  on the box's stored size. Under `metabox=lz4` the box is compressed with
  the LZ4 profile's encoder and mapped as that profile maps a file at the
  same addressing: under `addressing=48` by 32-byte encoded extents, unlike
  `mkfs.erofs`'s 4-byte records, a form the kernel and `fsck.erofs` read
  (E10, `e10-hand-box32`), the body reserving room for `ceil(content / B)`
  records, the most the cluster former can produce, since each cluster
  consumes at least `B` input bytes or ends the content, with the header's
  count holding the real number and the bytes after the last record zero;
  under `addressing=32` by the core full index of exactly
  `ceil(content / B)` records, since encoded extents need `48BIT`
  ([features](#features)). Either way the box inode's size is known from
  the content's size, so the primary metadata area's end, and every data
  address, is fixed before the box is encoded.
* Directory and file data stay in the primary data area, and the writer
  stores no extended attributes, so there is no shared pool to place; a
  writer that stores one in a box volume keeps it in the primary space as
  `mkfs.erofs` does, [above](#metadata-box). No inode has an inline
  tail, so the box holds no source bytes and is generated from the plan
  alone; a tail goes in its own data block.

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
| `allocation_unit_bytes` | 0, 512, 1024, 2048 or 4096 (E11); 0 selects 4096 |
| `volume_bytes` | 0, or a multiple of 512 holding the plan; the block count is `floor(V / B)`, the image is `V` bytes long, and the excess blocks and any partial block after the last block are zero |
| `min_free_bytes` | 0; the image is exactly its plan, so a free-space request is `FSIMG_EINVAL` |
| `label` | At most 15 bytes, stored raw and NUL-padded to 16 |
| `volume_uuid` | Stored verbatim; unset in reproducible mode, it follows the [identifier rule](design-layers.md#identifiers) with the EROFS volume role; unset otherwise, `FSIMG_EINVAL`, and the CLI supplies a random version 4 UUID |
| `epoch` | The format time in reproducible mode, from `-e`: any signed 64-bit second count. The superblock's epoch field is a different value: it holds the format time, and under `erofs_addressing=48` the smallest modification second instead |
| `erofs_compression` | `none`, the default, or `lz4` |
| `erofs_addressing` | `32`, the default, or `48` |
| `erofs_metabox` | `none`, the default, `plain` or `lz4`, with either `erofs_addressing` |

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
| Inodes | Compact, extended and both in one volume; sizes 0, 1, 31, 32, B-1, B, B+1, 2B and above 4 GiB through a sparse source; every kind; owners above 65535; times before 1970, past 2038 and with nanoseconds; a nonzero compact time word on a volume without the 48-bit layout, read as the epoch plus the word | Reserved bits, layouts 5 to 7, nanoseconds out of range, unknown mode type, body past the domain, tail crossing a block |
| Directories | 1- and 255-byte names, invalid UTF-8, names sorting before `.`, one directory of 100000 entries, deep trees | Name offset zero, unaligned or too large; repeated, unordered or overlapping names; slash or NUL; bad type; type mismatch; parent-rule violations; a second name for a directory |
| Attributes | Inline, shared, long prefixes in both placements, filter on and off, ACLs and security labels | Overlong entries, bad shared index, unknown index, bad prefix |
| Chunks | Both entry widths, several chunk sizes, holes, shared chunks | Device identifiers, high parts, short index, chunk past the domain |
| Compression | Each algorithm; full and compact indexes across group boundaries; big, plain and interlaced clusters; partial references; inline tails; fragments; compressed directories; reads starting and ending inside a cluster | Bad distances and cycles, bad group packing, marker without big clusters, oversize clusters, truncated and corrupt streams, fragment past the packed inode; the hand-built E15 forms refused by policy: `L > B`, compact advice `0x01` clear, a plain inline tail on an interlaced inode |
| 48-bit layout | Both superblock forms, the wide one on a small volume; root NID above 65535 through a hand-built image, which no producer places; start blocks and chunk blocks above 2^32 and the bit-63 metadata-box root through sparse fixtures; implicit link counts with and without high bits; omitted dots; every encoded-extent width, fragments and the hand-built 32-byte placements (`e10-*`) | High bits without the feature; a nonzero wide root field without the feature, reported and ignored; encoded extents without the feature (`e10-edit-lz4-no48`), unordered, overcounted (`e10-edit-lz4-counthi`), overlong, with nonzero header bytes, reserved length bits or a logical cluster beyond the limit; a stored `.` under bit 4; bit 4 on an extended inode; a zero length word; a 16-byte record with a physical high half; a time sum past 64 bits |
| Metadata box | Plain and compressed boxes at 512 to 4096 bytes; directories, files, chunks, fragments and long prefixes inside; the pool in the primary space and in the box; the root in the box and, without `48BIT`, in the primary space (`e12-flat-box32-plain`, `-poolbox`); reads across box cluster boundaries (`e12-*`) | Box NID with bit 63 or naming a fragment; box inode in the box, without a slot, or in an open layout; a box of encoded extents without `48BIT` (`e12-flat-box32-lz4`); a packed NID with bit 63; tail crossing a box block; bit 63 without the feature; `SHARED_EA_IN_METABOX` without the box or on a compressed box |
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
| X1 | XXH32 and LZ4 block format descriptions by their author: for XXH32, the xxHash specification `doc/xxhash_spec.md` (version 0.2.0) by Yann Collet in the `Cyan4973/xxHash` repository at commit `d66a9cb6f223b1f15cceebee39bc07ed0bd4ad3d`, fetched on 2026-11-06, whose notice permits copying and distribution with the copyright notice preserved | Public format descriptions; the codec design's clean-room procedure applies. The name-filter hash is written from the XXH32 section alone and checked against `xxhsum -H0`; no xxHash implementation is read |
| T1 | [Research draft](../fsspecs/EROFS_fsimg_spec.md) | Original project research; not authoritative |
