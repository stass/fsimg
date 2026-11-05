# HFS Plus and HFSX reader and writer

## Scope and evidence

`hfsplus` denotes Apple's HFS Plus volume format (Mac OS Extended) and
`hfsx` its HFSX variant, which differs in signature, version and the
catalog's name comparison. One driver serves both names. The reader consumes
one existing volume as it is on disk, raw or embedded in a legacy HFS
wrapper; the writer constructs a new, unjournaled, unwrapped volume that
native macOS checks, mounts, modifies and remounts. Neither modifies an
existing volume, replays a journal or repairs damage. Readers and writers
remain independently registered capabilities.

The compatibility target is data-volume interoperability with macOS and
with independent readers: volumes created by macOS (`hdiutil`, `newfs_hfs`)
and by Linux `mkfs.hfsplus` read correctly, and fsimg volumes pass
`fsck_hfs -n` and survive native use. Bootable system volumes, startup files
and Finder blessing are outside this design. The
[driver design](design-hfsplus-driver.md) maps the rules below onto the
modules of `lib/formats/hfsplus/`; the research record
`docs/hfsplus-research.md` holds the native evidence, the experiments E1 to
E11 that the [plan](../docs/hfsplus-plan.md#1-native-evidence-corpus)
defines; and the [qualification work](../docs/todo-hfsplus.md) holds the
open gates. A rule that cites an experiment rests on the observation that
experiment records, and changes only when the record does. The
research draft [HFS+ / HFSX on-disk specification](../fsspecs/HFSPLUS_HFSX_FSIMG_SPEC.md)
supplied the questions; it is not a source of rules.

This document is an original description of disk-format facts and fsimg
requirements. It contains no imported source, copied declarations or copied
documentation prose. Apple's technical note and its open-source HFS and
xnu sources are read for facts only; the Linux, hfsprogs, hfsfuse, Sleuth Kit and libfshfs
implementations are black-box comparison oracles, never code donors.
NetBSD's BSD-2-Clause `libhfs` is a cross-check for the comparison rules,
not a source of tables. Apple's BSD-3-Clause LZFSE and LZVN decoders are the
one imported code, under the [codec design](design-codecs.md#imported-lzfse-and-lzvn-decoders).

Evidence has three meanings here:

* **Documented layout**: byte positions and meanings that Apple's technical
  note [TN] states and that the native captures corroborate. It establishes
  interpretation, not that macOS accepts every combination.
* **Observation**: a property measured on a named native capture [R]. It
  fixes the closed acceptance tables and every place where the technical
  note is silent or out of date.
* **fsimg policy**: a validation, capability or construction rule chosen
  here. It can be stricter than macOS without redefining the format.

Acceptance tables are closed: a value in an observed tuple is accepted, an
unobserved value is `FSIMG_EUNSUPPORTED`, and a value that contradicts the
format is `FSIMG_ECORRUPT`. The tables widen only from evidence recorded in
the research record. Source keys in brackets are listed at the end.

## Shared owners and representation

The resource context owns memory, scratch, work accounting and cancellation.
Bounded I/O owns every volume-relative byte access. The common source catalog
owns input identities and repeatable reads; the output coordinator owns
publication. The common contracts cover HFS Plus:
[format descriptors and probes](design-formats.md#format-descriptors-and-profiles),
[prepared directories](design-directories.md),
[logical content and the extent index](design-content.md),
[emission](design-emission.md), [sizing](design-sizing.md),
[POSIX metadata](design-metadata.md),
[verification helpers](design-verification-helpers.md) and the
[writer loss policy](design-metadata-loss.md). The format adds five things
to those contracts, each specified by the contract's owner:

| Addition | Owner | Why HFS Plus needs it |
| --- | --- | --- |
| The `FSIMG_META_EXTRA_DATA` validity bit | [POSIX metadata](design-metadata.md#metadata-fields) | A file's resource fork and stored extended attributes have no place in the entry model; the bit shows per entry, in listings and in the extraction loss report, that they were not delivered |
| Decoded content spans | [Logical content](design-content.md#decoded-spans) | A compressed file's bytes are not an image range; the driver decodes one chunk into a window and delivery copies from it |
| A 765-byte label | [C API](design-api.md) volume information and the worker job record | A volume name is up to 255 UTF-16 units, at most 765 UTF-8 bytes |
| An origin for the unsigned-seconds helper | [Writer loss policy](design-metadata-loss.md#unsigned-whole-second-selection) | Stored times count seconds from 1904, so a valid Unix time can be negative |
| The imported LZFSE and LZVN decoders | [Codecs](design-codecs.md#imported-lzfse-and-lzvn-decoders) | macOS compresses files with them |

Common code gains no other HFS-specific branch. The other format-specific
facts, comparators, Unicode tables and B-tree mechanics stay in the driver.

Every disk integer is big-endian and decoded from bytes, except the
little-endian fields of the compression header and its chunk tables, which
are named where they occur. Native structure layout, host alignment and host
`stat` constants are not disk codecs. Offsets and products use checked
`uint64_t` arithmetic; a value that fails a check is corrupt, never clamped.
All project-defined C types have `_t` typedefs.

## Volume layout and geometry

### The volume and its slice

The volume is the slice the driver opens. `V` is its length in bytes, or the
header's `N * B` when that is longer, which proves the slice lost the
volume's tail and makes it a
[truncated volume](architecture.md#byte-and-object-boundaries). `V` is a
multiple of 512; a slice that reaches `N * B` with a length that is not is
`FSIMG_ECORRUPT`, because the alternate header's position is undefined. A
volume inside a partition,
disk image or wrapper is reached by the [layer design](design-layers.md) or
the [wrapper rule](#the-hfs-wrapper); the driver never treats the start of a
disk or a DMG file as the start of a volume. [TN]

| Byte range of the slice | Content |
| --- | --- |
| `[0, 1024)` | Boot blocks; not interpreted beyond the [probe's](#probe-identity) test |
| `[1024, 1536)` | Primary volume header, 512 bytes |
| `[V - 1024, V - 512)` | Alternate volume header, a copy of the primary |
| `[V - 512, V)` | Reserved |

### Allocation blocks

The volume is divided into `N` allocation blocks of `B` bytes, where `B` is
the header's `blockSize`, a power of two from 512, and `N` is the header's
`totalBlocks`. Block `b` occupies `[b * B, (b + 1) * B)`. Every extent,
bitmap bit and block count uses these numbers, starting at zero. The reader
requires `N >= 1`, and `N * B <= V` follows from `V`; the bytes from `N * B`
to `V`, fewer than `B` on a native volume, are outside allocation and may
hold the alternate header. [TN]

Blocks that overlap `[0, 1536)` or `[V - 1024, V)` are reserved: they
belong to no fork and are marked allocated in the bitmap. With `r =
ceil(1536 / B)` and `t` the number of blocks below `N` that overlap
`[V - 1024, V)`, the **content domain**, the image range any fork can
occupy, is `[r * B, (N - t) * B)`. It is the domain every
[extent index](design-content.md#extent-index) of the reader declares. For
`B = 4096` and `V = N * B`, `r = 1` and `t = 1`: the first and last blocks
are reserved. A truncated volume has lost its tail, so the alternate header
is not there to protect: it reserves no tail, `t = 0`, and its content
domain ends at `N * B`. [TN]

A formatter marks the last `ceil(1024 / B)` blocks below `N` for the
alternate header wherever it lies: `newfs_hfs` marks them unconditionally
(its `WriteBitmap`), and the `mozilla-1.3-machO` and `mozilla-1.7.13` volumes,
whose Apple_HFS partitions end 2048 bytes past `N * B` at `B = 4096`, have
block `N - 1` set and no owner (E10).
Such a volume has `t = 0`, so the rule above leaves those blocks in the
content domain: verification claims nothing for them and accepts them set or
clear. Whether a host ever allocates one when its bit is clear, as `t = 0`
permits, is unproven; no specimen does.

### Probe identity

The identity signature has two parts. At offset 1024 is either a volume
header signature and version (`H+` with 4, `HX` with 5) or a classic HFS
master directory block (`BD`, `0x4244`) whose embedded signature at its
offset `0x7C` is `H+`. And the boot blocks `[0, 1024)` are either all zero
or begin with the classic boot-block signature `LK` (`0x4C4B`), the two
forms native formatters write, including over media that held another
volume (E1). The second part makes the signature exclusive. FAT and exFAT
volumes begin with a jump instruction, an ImageFS with `imagefs`, and a
QNX4 volume carries its root record at byte 512, so none has a zero or `LK`
boot area. A QNX6 volume can have a zero boot area, but its bytes at 1024
are zero too, because its superblock follows the boot area. FAT12 and FAT16
are the only registered formats whose bytes at 1024 are otherwise
unconstrained, because their first FAT can begin at 512. A `BD` block
without the embedded `H+` is a classic HFS volume, which no registered
reader claims. [TN], [R]

## The volume header

### Fields

The header is 512 bytes, decoded at fixed offsets: [TN]

| Offset | Width | Field | Reader rule |
| ---: | ---: | --- | --- |
| 0 | 2 | `signature` | `H+` (`0x482B`) or `HX` (`0x4858`) |
| 2 | 2 | `version` | 4 with `H+`, 5 with `HX`; another pairing is `FSIMG_EUNSUPPORTED` |
| 4 | 4 | `attributes` | The [closed table](#volume-attributes) |
| 8 | 4 | `lastMountedVersion` | Any value; `FSK!` records a failed journal replay, so the volume is reported unclean |
| 12 | 4 | `journalInfoBlock` | Under the [journal rules](#journal) |
| 16 | 4 | `createDate` | Local time of the creating host (`newfs_hfs`, `hdiutil` and `diskutil` write wall-clock time; `mkfs.hfsplus` writes UTC, E1); reported as a wall-clock detail, never converted |
| 20, 24, 28 | 4 each | `modifyDate`, `backupDate`, `checkedDate` | Reported; not checked |
| 32 | 4 | `fileCount` | Catalog file records; checked by verification |
| 36 | 4 | `folderCount` | Catalog folder records except the root; checked by verification |
| 40 | 4 | `blockSize` | `B`: a power of two from 512 to `2^31` |
| 44 | 4 | `totalBlocks` | `N`: at least 1, with `N * B <= V` |
| 48 | 4 | `freeBlocks` | At most `N`; verification requires it to equal the bitmap's clear bits |
| 52 | 4 | `nextAllocation` | An allocation hint; ignored. It can exceed `N` on a native volume: `newfs_hfs` with a large catalog clump writes 860166 on a 163840-block volume, which `fsck_hfs -n` calls a minor repair and the first mount rewrites (E9) |
| 56, 60 | 4 each | `rsrcClumpSize`, `dataClumpSize` | Hints; ignored |
| 64 | 4 | `nextCatalogID` | At least 16; verification requires it above every catalog node ID unless the reuse bit is set |
| 68 | 4 | `writeCount` | Ignored |
| 72 | 8 | `encodingsBitmap` | Ignored |
| 80 | 32 | `finderInfo` | Eight words; words 6 and 7 are the 64-bit volume identifier, reported as a detail; the others are boot data, not interpreted |
| 112 | 80 | `allocationFile` | [Fork data](#fork-data) of the bitmap, required |
| 192 | 80 | `extentsFile` | Fork data of the extents overflow tree, required |
| 272 | 80 | `catalogFile` | Fork data of the catalog tree, required |
| 352 | 80 | `attributesFile` | Fork data of the attributes tree; a zero fork means the volume has none |
| 432 | 80 | `startupFile` | Fork data of the startup file; a zero fork means none |

### Volume attributes

The attribute word is a closed table. Bits outside it are
`FSIMG_EUNSUPPORTED` until an observation adds them, because a set bit can
change how other fields are interpreted, as bit 29 does: [TN], [HF]

| Bit | Meaning | Reader |
| ---: | --- | --- |
| 7 | Hardware write lock | Accepted; a runtime indication |
| 8 | Cleanly unmounted | The clean flag `info` reports |
| 9 | Bad blocks spared | Accepted; bad blocks are listed by the extents tree either way |
| 10 | No cache required | Accepted |
| 11 | Boot volume inconsistent | Accepted; reported unclean |
| 12 | Catalog node IDs reused | Accepted; relaxes the `nextCatalogID` check |
| 13 | Journaled | Under the [journal rules](#journal) |
| 14 | Volume inconsistent | Accepted; reported unclean |
| 15 | Software write lock | Accepted |
| 29 | Expanded timestamps | `FSIMG_EUNSUPPORTED`: the dates no longer count seconds from 1904 |
| 30 | Per-file content protection | `FSIMG_EUNSUPPORTED`: content is encrypted |
| 31 | Unused B-tree nodes zeroed | Accepted |

`info` reports the volume clean when bit 8 is set, bits 11 and 14 are clear
and `lastMountedVersion` is not `FSK!`. The flag does not certify structural
consistency; verification does. `newfs_hfs`, `hdiutil` and `mkfs.hfsplus`
write `0x80000100`, bits 8 and 31, and `0x80002100` with a journal; a mount
clears bit 8 and unmounting sets it again (E1, E2). Volumes from 2001 to
2003 carry `0x00000100`, bit 8 alone (E10).

### Alternate header

Opening uses the primary header alone. Verification reads the alternate
header at `V - 1024` and requires its signature, version, `blockSize`,
`totalBlocks` and the five fork-data records to equal the primary's.
Nothing else is compared: macOS rewrites the alternate when a special
file's fork data changes, that is when the extents, catalog, allocation,
attributes or startup file grows or moves, when the journal is turned on
or off, and when the volume is resized; an ordinary mount, unmount or sync
writes the primary alone (the record's Apple source cross-check, and E2).
So after native use the alternate keeps the counts, `freeBlocks`,
`nextAllocation`, `nextCatalogID`, `writeCount`, `modifyDate`,
`lastMountedVersion` and `attributes` of an earlier moment (E2), and old
discs differ in up to nine such fields while the fork records and geometry
agree (E10). An alternate that holds no volume header, such as the
all-zero sectors a disk-image container left out of the `mozilla-1.0-smi`,
`mozilla-1.3-machO` and `mozilla-1.7.13` specimens, whose last sectors its
chunk table marks as ignored (E10), fails this comparison like a disagreeing one: verification
reports `FSIMG_ECORRUPT` at its last step, after every other pass has run,
and reading, listing and extraction, which use the primary alone, are
unaffected. Verification never runs on a truncated volume, so the 1024
bytes at `V - 1024` lie inside the slice, and
the read itself can fail only for operational reasons, and those keep
their own classes: a read the I/O callback fails is `FSIMG_EIO`, and
cancellation and resource exhaustion are reported as common code reports
them, never recast as corruption. A primary that fails validation is never
replaced by the alternate: fsimg does not repair, and a reader that
silently chose between disagreeing copies could expose a stale catalog.

### Dates

Catalog and most header dates are unsigned 32-bit seconds since 1904-01-01
00:00:00 UTC, so the representable instants run from that moment to
2040-02-06 06:28:15 UTC. The Unix time is the stored value minus
2082844800, computed in signed 64-bit arithmetic so that dates before 1970
stay negative. The header's `createDate` alone is the creating host's local
time and is reported as a wall-clock value without a zone. Volume bit 29 and
catalog flag `0x1000` select an expanded representation that this design
does not decode. [TN], [HF]

## Forks and extents

### Fork data

A fork is described by an 80-byte record: [TN]

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 8 | `logicalSize`: valid bytes |
| 8 | 4 | `clumpSize`: an allocation hint, ignored |
| 12 | 4 | `totalBlocks`: blocks in all the fork's extents, overflow included |
| 16 | 64 | Eight extent descriptors, each `startBlock` (4) and `blockCount` (4) |

### Extent rules

* Used descriptors precede unused ones, and an unused descriptor is
  `(0, 0)`. A descriptor with a zero count and a nonzero start, or a used
  descriptor after an unused one, is `FSIMG_ECORRUPT`.
* Each used extent lies in the content domain: `startBlock >= r` and
  `blockCount <= N - t - startBlock`, checked before the sum is formed.
  Block zero is a real location, never a hole marker: HFS Plus has no sparse
  extents.
* The fork's extents, the eight inline ones followed by its
  [overflow records](#the-extents-overflow-tree) in key order, sum exactly
  to `totalBlocks`, and `logicalSize <= totalBlocks * B`. A fork with
  `totalBlocks = 0` has `logicalSize = 0` and eight unused descriptors.
* Content is the fork's extents in that logical order, never sorted by
  physical position. Allocation beyond `logicalSize` belongs to the fork,
  is claimed by verification and is never delivered.
* Two forks never share a block. Hard links share an object through the
  catalog, not through extents.

### Special files

| Node ID | Object | Where its fork is |
| ---: | --- | --- |
| 1 | Parent of the root | None; it names no record |
| 2 | Root folder | Catalog |
| 3 | Extents overflow tree | `extentsFile`; its own extents never overflow |
| 4 | Catalog tree | `catalogFile`, continued by overflow records |
| 5 | Bad blocks | Overflow records only, data fork type, no catalog record |
| 6 | Allocation bitmap | `allocationFile`, continued by overflow records |
| 7 | Startup file | `startupFile`, continued by overflow records |
| 8 | Attributes tree | `attributesFile`, continued by overflow records |
| 9 to 15 | Reserved | Never a catalog record |
| 16 and above | Ordinary files and folders | Catalog records |

The extents tree's `totalBlocks` must equal the sum of its eight inline
extents; an extents-tree fork that needs overflow is `FSIMG_ECORRUPT`,
because finding its continuation would require reading itself. The startup
file is claimed and reported but never exposed as an entry. [TN]

### The extents overflow tree

A fork with more than eight extents continues in this tree. Its key is 12
bytes: `keyLength` (2, always 10), `forkType` (1: `0x00` data, `0xFF`
resource), a pad byte (zero), `fileID` (4) and `startBlock` (4), the
**logical** block offset within the fork at which the record's extents
begin. Keys compare as the tuple `(fileID, forkType, startBlock)`, all
unsigned, not in their byte order. A leaf record's data is eight extent
descriptors, 64 bytes. [TN]

The first overflow record of a fork has `startBlock` equal to the blocks of
its inline extents, and each further record's `startBlock` equals the
previous one plus the blocks of the previous record's used extents. The
records of a fork end when the sum reaches `totalBlocks`. A gap, an overlap,
a record past the end, or a record whose used extents are empty is
`FSIMG_ECORRUPT`. Bad-block records use node ID 5 and fork type `0x00`,
start at logical block 0 and follow the same continuity rule. Pad bytes and
unused descriptors are zero.

## The allocation bitmap

The bitmap file holds one bit per allocation block: block `b` is bit
`7 - (b mod 8)` of byte `floor(b / 8)`, and a set bit means the block is in
use. Its `logicalSize` is at least `ceil(N / 8)`; bits from `N` to the end
of the last logical byte are zero, and verification requires them zero. The
bitmap's own blocks, the B-tree files, every fork, the reserved blocks, the
journal and bad blocks are set, and a tail block of a volume with `t = 0` may
be, by the [reserved rule](#allocation-blocks). `freeBlocks` counts the clear bits below
`N`. [TN]

## B-trees

The extents, catalog and attributes files are B-trees with one node format
and different keys and records. A tree is the logical byte stream of its
fork: node `n` occupies fork bytes `[n * M, (n + 1) * M)` for the tree's
node size `M`, wherever the fork's extents put them, so a node may cross an
allocation block or an extent boundary. [TN]

### Nodes and records

Every node begins with a 14-byte descriptor: `fLink` (4), `bLink` (4),
`kind` (1, signed: -1 leaf, 0 index, 1 header, 2 map), `height` (1),
`numRecords` (2) and a reserved field (2). Records grow from offset 14; a
table of `numRecords + 1` two-byte offsets grows back from the node's end,
entry `i` at `M - 2 * (i + 1)`. Offset `i` is record `i`'s start, offset
`i + 1` its end, and the last entry is the first free byte. The node is
well formed when `14 = offset[0] < offset[1] < ... < offset[numRecords]
<= M - 2 * (numRecords + 1)` and every offset is even, the two-byte
alignment the technical note gives records; E5 confirms it on inline
attribute records of odd value length. [TN], [R]

A keyed record begins with a two-byte `keyLength` that excludes itself,
then the key; the record data starts at the next even offset. In a leaf
the data is the tree's record. In an index node it is one four-byte child
node number. The catalog and attributes trees store index keys at their
actual length; the extents tree stores them at `maxKeyLength`, which for its
fixed ten-byte keys is the same length. [TN]

### The header node and node maps

Node 0 is the header node: kind 1, height 0, three records. The first is
the 106-byte header record at offset 14; the second is 128 bytes of user
data at offset 120; the third is the node map, from offset 248 to `M - 8`.
The header record: [TN]

| Offset | Width | Field | Reader rule |
| ---: | ---: | --- | --- |
| 0 | 2 | `treeDepth` | 0 for an empty tree; otherwise the root's height, at most 16 |
| 2 | 4 | `rootNode` | 0 for an empty tree; otherwise a node below `totalNodes` |
| 6 | 4 | `leafRecords` | Equal to the records of every leaf, checked by verification |
| 10, 14 | 4 each | `firstLeafNode`, `lastLeafNode` | 0 for an empty tree; the ends of the leaf chain otherwise |
| 18 | 2 | `nodeSize` | `M`, a power of two in the [tree's range](#tree-parameters) |
| 20 | 2 | `maxKeyLength` | The tree's value |
| 22 | 4 | `totalNodes` | `totalNodes * M` equals the fork's `logicalSize` |
| 26 | 4 | `freeNodes` | Equal to the clear map bits below `totalNodes`, checked by verification |
| 30 | 2 | `reserved1` | Not interpreted |
| 32 | 4 | `clumpSize` | A hint; ignored |
| 36 | 1 | `btreeType` | 0 |
| 37 | 1 | `keyCompareType` | The tree's value |
| 38 | 4 | `attributes` | The tree's flags |
| 42 | 64 | `reserved3` | Not interpreted |

The node map has one bit per node, most significant bit first, set for a
node in use. When `totalNodes` exceeds the header map's `8 * (M - 256)`
bits, the header's `fLink` names the first map node: kind 2, height 0,
`bLink` 0, one record from offset 14 whose length the node's offset table
gives, its own `fLink` naming the next. The reader takes each map record's
length from the validated offset table and requires the chain to cover
`totalNodes`; it imposes no record length of its own, because the
validated offsets and the coverage are what make the bits safe to read.
The catalog of `h-hfsplus-671088640-e9-create` has two such nodes at
`M = 4096`, each record `M - 20` bytes (E9); that is the layout the
writer emits at its one catalog node size, and no other node size is
observed. Map nodes are themselves marked in use. A map chain that
loops, ends before covering `totalNodes`, or names a node outside the tree
is `FSIMG_ECORRUPT`. Map bits at or beyond `totalNodes` are zero. [TN]

### Searching and order

An index record pairs a child with the smallest key in the child's subtree,
which on a valid tree is the key of the child's first record; macOS keeps
that true through deletions, rewriting the index key when a child's first
record goes, so every index record of the E9 stages equals its child's
first key. A non-root index node holding one record is valid: macOS leaves
such nodes in `h-hfsplus-671088640-e9-sparse` and `fsck_hfs -n` accepts the
volume (E9). A search
follows the greatest index key not above the search key, so a key smaller
than a node's first index key is absent from it. Every level's nodes are
joined in key order by `fLink` and `bLink`; the leftmost has `bLink = 0`
and the rightmost `fLink = 0`. Keys are strictly ascending under the tree's
comparator within a node and across each level. A leaf has height 1 and an
index node one more than its children. [TN]

### Tree parameters

| Tree | Node size | `maxKeyLength` | Flags | `keyCompareType` |
| --- | --- | ---: | --- | --- |
| Extents | 512 to 32768 | 10 | big keys (`0x02`) set, variable index keys (`0x04`) clear | 0 |
| Catalog | 4096 to 32768 | 516 | big keys and variable index keys set | `H+`: 0 or `0xCF`, folded; `HX`: `0xCF` folded or `0xBC` binary |
| Attributes | 4096 to 32768 | 266 | big keys and variable index keys set; big keys alone in an empty tree | 0, or `0xBC` with `btreeType` `0xFF` |

`btreeType` is 0 in the extents and catalog trees. The technical note leaves
the attributes tree's `btreeType` and `keyCompareType` to later definitions,
so the evidence decides them: E1 shows 0 and 0 for the tools it ran, and
volumes written by macOS in 2009 show `0xFF` and `0xBC`, the values macOS
itself writes when it creates the tree. The pair is taken together: one value
of either field with the other's from the other pair is outside the table.
`keyCompareType` on an attributes tree never changes how its keys compare:
the key's name is always compared unit by unit as unsigned 16-bit values,
after the file ID, whichever pair the header holds. An empty attributes tree
may have the variable index keys flag clear, as the python 3.3.5 installer
volume's does (depth 0, no records, a header and no other node in use):
with no index node, no index key exists for the flag to describe. A tree of
any other depth needs the flag set. Native
node sizes: `newfs_hfs`, `hdiutil` and `mkfs.hfsplus` write 4096 for the
extents tree and 8192 for the attributes tree at every size, and for the
catalog 4096 up to 128 MiB and at 1 GiB with blocks of 512 to 2048, 8192 at
1 GiB with blocks of 4096 and above (E1); volumes from 2001 to 2003 have
extents nodes of 1024 and catalog nodes of 4096 (E10). The
bad-close flag (`0x01`) is accepted and makes the volume unclean in `info`.
A power-of-two `nodeSize` outside the tree's range, other flag bits, another
`keyCompareType`, `btreeType` or `maxKeyLength` are `FSIMG_EUNSUPPORTED`
until observed; a `nodeSize` that is no power of two is `FSIMG_ECORRUPT`.
HFSX's comparison is
taken from the catalog header, never inferred from the `HX` signature: an
HFSX volume can be case-insensitive. [TN], [HF]

## The catalog

Every file and folder has one primary record keyed by its parent's node ID
and its name, and one thread record keyed by its own node ID and an empty
name. The root folder has node ID 2 and parent ID 1, and its name is the
volume name. Because keys sort by parent first, a folder's children are
adjacent, preceded by the folder's own thread. [TN]

### Keys

A catalog key is `keyLength` (2, equal to `6 + 2 * n`), `parentID` (4),
`nameLength` (2, `n` from 0 to 255) and `n` big-endian UTF-16 units. Primary
keys have nonempty names; thread keys have empty ones. Keys compare by
`parentID` as unsigned 32-bit, then by name under the volume's
[comparator](#comparators). A key length that disagrees with `nameLength`,
or a name longer than 255 units, is `FSIMG_ECORRUPT`. [TN]

### Records

The record type is the first two bytes of the leaf data: [TN]

| Type | Record | Length |
| ---: | --- | ---: |
| 1 | Folder | 88 |
| 2 | File | 248 |
| 3 | Folder thread | `10 + 2 * n` |
| 4 | File thread | `10 + 2 * n` |

A record shorter than its type's length, with trailing bytes beyond it, or of
another type is `FSIMG_ECORRUPT`.

Folder record:

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 2 | `recordType` = 1 |
| 2 | 2 | `flags` |
| 4 | 4 | `valence`: direct children, files and folders |
| 8 | 4 | `folderID` |
| 12, 16, 20, 24, 28 | 4 each | `createDate`, `contentModDate`, `attributeModDate`, `accessDate`, `backupDate` |
| 32 | 16 | BSD information |
| 48 | 16 | Folder Finder information |
| 64 | 16 | Extended folder Finder information |
| 80 | 4 | `textEncoding` |
| 84 | 4 | `folderCount`: direct subfolders, meaningful with flag `0x0010` |

File record:

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 2 | `recordType` = 2 |
| 2 | 2 | `flags` |
| 4 | 4 | Reserved; on an indirect node it names the newest hard-link stub (E4) |
| 8 | 4 | `fileID` |
| 12, 16, 20, 24, 28 | 4 each | `createDate`, `contentModDate`, `attributeModDate`, `accessDate`, `backupDate` |
| 32 | 16 | BSD information |
| 48 | 16 | File Finder information: type (4), creator (4), flags (2), location (4), reserved (2) |
| 64 | 16 | Extended file Finder information |
| 80 | 4 | `textEncoding` |
| 84 | 4 | Reserved |
| 88 | 80 | Data fork |
| 168 | 80 | Resource fork |

Thread record: `recordType` (2, 3 or 4), reserved (2), `parentID` (4),
`nameLength` (2) and the name. It holds the primary record's parent and
name, and its type matches the primary's: 3 for a folder, 4 for a file.

Node IDs are unique: `folderID` and `fileID` are at least 16, except the
root's 2, and no two primary records share one. [TN]

### Flags

The flag word of folder and file records is a closed table: [TN], [HF]

| Mask | Meaning | Reader |
| ---: | --- | --- |
| `0x0001` | File locked | Accepted; not delivered |
| `0x0002` | File thread exists | Accepted |
| `0x0004` | Has extended attributes | Consults the [attributes tree](#extended-attributes) |
| `0x0008` | Has a security attribute | Accepted; the attribute itself is stored data |
| `0x0010` | Folder count valid | Folders only; `folderCount` checked by verification |
| `0x0020` | Hard-link chain | Accepted on [hard-link](#hard-links-and-private-directories) records |
| `0x0040` | Contains a directory hard link | Accepted; the link itself is refused |
| `0x0080` | Date added recorded | Accepted; the date is Finder data, not delivered |
| `0x0100` to `0x0800` | The four Fusion placement hints | Accepted; they do not change interpretation |
| `0x1000` | Expanded timestamps | `FSIMG_EUNSUPPORTED` |

Any other bit is `FSIMG_EUNSUPPORTED` until observed.

### BSD information and kinds

The 16-byte BSD information is `ownerID` (4), `groupID` (4), `adminFlags`
(1), `ownerFlags` (1), `fileMode` (2) and `special` (4). `ownerFlags`
`0x20` marks a [compressed](#transparent-compression) file. `special` holds a
hard-link reference, a link count or a device number, by kind; elsewhere it
is not interpreted. macOS reports an ordinary file's `special` as its link
count (E4: a file patched to 20 shows `nlink` 20), but the entry model has
no link count, so fsimg reads nothing from it. [TN], [HF]

The [stub test](#file-hard-links) runs before the kind lookup, because a
stub's `fileMode` is `0100000` on current macOS and 0 on the Mac OS X 10.0
and 10.1 discs (E4, E10). For a record that is not a stub, the kind comes
from the record type and the type bits of `fileMode`:

| Record | `fileMode & 0170000` | Entry |
| --- | --- | --- |
| Folder | `040000` | Directory |
| Folder | 0, with all of `fileMode` zero | Directory; owner and mode absent |
| File | `0100000` | Regular file, or a compressed file, by the rules below |
| File | `0120000` | Symbolic link |
| File | `010000` | FIFO |
| File | 0, with all of `fileMode` zero | Regular file; owner and mode absent |
| File | `0140000`, `020000`, `060000` | Socket, character device, block device: read-only kinds under the [device node design](design-device-nodes.md#readers) |
| Either | another value | `FSIMG_ECORRUPT` |

A zero `fileMode` is legal: creators older than Mac OS X left BSD
information uninitialized (every record of the Mac OS 9-formatted
`mozilla-1.0-smi` volume, E10), and macOS then applies its own defaults, which
are a host policy fsimg does not reproduce. An unsupported kind fails its
directory's preparation with `FSIMG_EUNSUPPORTED` naming the entry, as the
[metadata design](design-metadata.md#entry-kinds) states.

The entry's metadata carries owner and group, the permission bits
`fileMode & 07777`, and the times: `createDate` as created,
`contentModDate` as modified, `attributeModDate` as changed and
`accessDate` as accessed, each a UTC instant in whole seconds. The backup
date, BSD flags, text encoding and Finder information are not delivered.

### Finder information

The 32 bytes of Finder and extended Finder information are opaque to fsimg
except where the [hard-link rules](#hard-links-and-private-directories)
read a file's type and creator, and where the writer sets `slnk`/`rhap` on a
symbolic link. The codes alone mean nothing: `fdrp`/`MACS` is also every
Finder alias, and any file can carry `hlnk`/`hfs+`, so a record is a link
stub only under the full test those rules state. Finder information is not
delivered:
the entry model has no field for it, and a macOS view of it as the
`com.apple.FinderInfo` attribute is synthesized from these bytes, not
stored. [TN], [HF]

### Counts

A folder's `valence` equals its primary children. With flag `0x0010` its
`folderCount` equals its child folders. The header's `fileCount` equals the
file records and `folderCount` the folder records except the root,
format-owned objects included. `nextCatalogID` exceeds every node ID unless
volume bit 12 is set. Verification checks each. [TN], [HF]

## Names

### Stored form

A name is 1 to 255 UTF-16 units, surrogate pairs counting two. Stored names
are canonically decomposed under the [HFS Plus rule](#decomposition): the
form macOS produces and searches for. The units are not a C string; an
embedded U+0000 is a unit like any other, and macOS stores one wherever a
POSIX name holds U+2400 (E3). [TN]

### Display and query mapping

The image path component of a stored name is its UTF-8 encoding with every
U+002F `/` written as U+003A `:` and every U+0000 written as U+2400. macOS
presents names the same way: a POSIX name containing `:` is stored with
`/`, because the Mac path separator is `:` and the POSIX one `/`, and a
POSIX name containing U+2400, the symbol for NUL, is stored with U+0000,
which no POSIX path can carry. These two are the only exchanges, on both
variants (E3). A query component maps back: UTF-8 to UTF-16, `:` to `/`,
U+2400 to U+0000. No recomposition happens on display: a listing shows the
stored decomposed spelling. [TN], [R]

The reader declares no omission profile: like FAT and exFAT it derives
validated names by its own rules, and every admitted child is delivered.

| Stored unit sequence | Reader |
| --- | --- |
| Empty, an unpaired surrogate, or more than 255 units | `FSIMG_ECORRUPT` |
| `.` or `..` | `FSIMG_EUNSUPPORTED`: legal through the classic Mac interfaces, not an image path |
| Display then query mapping does not reproduce the units | `FSIMG_EUNSUPPORTED`: no image path selects the entry |
| Anything else, control units such as the carriage return of `Icon\r` included | Delivered |

The one admission rule is that a stored name is admitted only if mapping it
to its image path and the path back to stored units gives the same units.
It covers a stored U+003A `:` or U+2400, each of which already denotes a
stored `/` or U+0000 so that no spelling is left for it, and a name that is
not a fixed point of [decomposition](#decomposition), a spelling fsimg's
tables do not reproduce; it covers any exchange the mapping gains later
without a row of its own. Percent text is ASCII and delivered as it is:
macOS stores each byte of a three-byte UTF-8 spelling of U+FFFE, U+FFFF
or a surrogate value, and each byte of malformed UTF-8, as `%` and two
uppercase hex digits, so `%EF%BF%BF` for U+FFFF (E3, and the record's
Apple source cross-check); `%` itself is never escaped, so a stored
percent text is indistinguishable from a name typed that way, and the
reader does not try to reverse it.

These refusals fail the directory, as the
[architecture](architecture.md#names-and-metadata) states for the formats
with strict UTF-16 name rules, where the QNX readers would omit the entry.
The consequence is that one such name makes its whole directory unlistable,
and `-k` cannot pass over it, because preparation fails before any child is
delivered; the manual says so. The reason is that a listing holding a name
no query can select would show an entry that selects nothing, or another
entry, and an omission profile in common code would add a mechanism for
names no native capture holds: no macOS interface can create a stored `:`
or U+2400, `.` and `..` need the classic Mac interfaces, and the four
volumes of 2001 to 2003 that E10 decodes hold none of them.

### Decomposition

HFS Plus decomposition is canonical decomposition followed by a stable sort
of combining marks by class, with these differences from Unicode
normalization form D: units in U+2000 to U+2FFF and U+F900 to U+FAFF are
never decomposed; no supplementary value decomposes, not even the thirteen
musical symbols Unicode gives a canonical decomposition, so a value above
U+FFFF is stored as its surrogate pair as given; Hangul syllables decompose
into their two or three conjoining jamo; compatibility decompositions never
apply. E3 observes this for every BMP scalar value and every supplementary
value Unicode 3.2 assigns, on both variants. [TN], [R]

Decomposition is a table lookup per scalar value followed by a stable sort
by combining class of each maximal run of nonzero classes, applied after
every value has been decomposed. The order of the two steps is observed: the
three class 0 values U+0F73, U+0F75 and U+0F81 decompose into marks with no
starter, and `a` U+0F72 U+0F73 is stored as `a` U+0F71 U+0F72 U+0F72, which
only a sort after decomposition gives (E3's `class0` names). Supplementary
values have class 0 in the table: macOS never reorders them, so `a` U+0300
U+1D165 and `a` U+1D165 U+0300 are two names (E3). The character data is
Unicode 3.2, which macOS has used since Mac OS X 10.3. The table and the
combining classes are [pinned](#unicode-tables-and-their-evidence); no host
Unicode library or locale decides a stored name.

Volumes written by Mac OS 8.1 to 9.2 may hold names decomposed under
Unicode 2.1, whose spelling of some values differs; no such name is in the
corpus: the one Mac OS 9-formatted HFS Plus volume E10 decodes holds ASCII
names alone, the other discs of that era are classic HFS, and the non-ASCII
names of the Mac OS X 10.0 and 10.1 discs each equal their decomposition as
the record states (E10). The name table above makes such a name an explicit
`FSIMG_EUNSUPPORTED` rather than a wrongly selected entry, and the
directory scan's key-order check, which applies the pinned comparator to
stored units, refuses a volume whose order disagrees as `FSIMG_ECORRUPT`;
a specimen widens the tables from evidence.

### Comparators

The catalog comparator is the volume's. Both compare `parentID` first, as
unsigned 32-bit, and then the names, where an empty name sorts before every
nonempty one, so a thread precedes the folder's children whatever their
names fold to; nonempty names compare as follows:

* **Folded** (`H+`, or `HX` with `0xCF`): each UTF-16 unit maps
  independently through the fold table, a function from every one of the
  65536 unit values to a 16-bit value, stored as a two-level table in the
  generated header; a unit that maps to zero is ignorable and skipped; the
  remaining mapped units compare lexicographically as unsigned values, and
  when one sequence is a prefix of the other the shorter sorts first. Equal
  mapped sequences are equal names, which cannot coexist in one folder. A
  name of one ignorable unit is a legal name: macOS creates U+200C alone,
  stores the unit, ranks it first among the folder's children, and refuses
  a second all-ignorable sibling as existing (E3); a name of several
  ignorable units follows the same rule, and is not observed. The fold
  table maps a unit on the high-byte pages `0x00`, `0x01`, `0x03`, `0x04`,
  `0x05`, `0x21` and `0xFF`, which hold Basic Latin and Latin-1, Latin
  Extended-A and the first half of Latin Extended-B, Greek, Cyrillic,
  Armenian, Roman numerals and fullwidth Latin, to its Unicode 3.2.0
  lowercase unless the unit decomposes, in which case it stays itself, and
  except the 30 units that stay themselves although they have a lowercase,
  the [exception rows](#unicode-tables-and-their-evidence); it maps the
  Georgian capitals U+10A0 to U+10C5 to U+10D0 to U+10F5; it marks the
  joiners and directional controls U+200C to U+200F, U+202A to U+202E and
  U+206A to U+206F and the byte-order mark U+FEFF ignorable; it maps U+0000
  to `0xFFFF`, so that it sorts after every unit up to U+FFFD, and a stored
  U+FFFF compares equal to a U+0000, which the strict key ascent of a
  directory scan reports as `FSIMG_ECORRUPT` should the two ever be
  siblings, as no POSIX name stores U+FFFF; and it leaves every other unit
  unchanged, so case pairs on other pages, such as U+021C to U+0224, U+24B6
  to U+24CF and the supplementary Deseret letters, are distinct names.
  Surrogate units are compared like any other unit, so supplementary values
  sort in unsigned unit order between U+D7FF and U+E000. [TN], [NB], [R]
* **Binary** (`HX` with `0xBC`): the units compare lexicographically as
  unsigned values, shorter prefix first, with nothing ignorable and U+0000
  first. [TN]

Neither comparator decomposes; both compare stored units. E3 fixes every
fold row a POSIX name can reach, on both comparators, against the catalog
order of about 614000 names. The U+0000 rule places the private data folder
last among the root's children on a folded volume, as the E10 discs show;
a writer that emitted the folder would depend on it.

### Namespace keys

The driver's namespace key for a stored name, and for a query, is the
volume's comparison applied to the decomposed units: decompose, then on a
folded volume fold and drop ignorable units, and encode the result as
big-endian units. Decomposing stored names is the identity on a valid
volume and makes every spelling of a name, decomposed or not, select the
same entry. Two siblings with equal keys are `FSIMG_ECORRUPT`: on a valid
catalog that can only mean two names that compare equal, which violates the
format (two all-ignorable siblings included, since macOS refuses the
second). A query component that is not valid UTF-8, or that decomposes
beyond 255 units, is `FSIMG_EINVAL`.

### Unicode tables and their evidence

Two tables are generated from pinned inputs by a project-owned script,
`tools/gen-hfsplus-tables.rb`, and compiled into the driver as
`lib/formats/hfsplus/unicode-tables.h`: the decomposition table with
combining classes, and the fold table. The inputs are Unicode 3.2.0's
`UnicodeData.txt` [UC], whose digest the generator checks, and the
exceptions file `tools/hfsplus-unicode-exceptions.txt`. The generator
derives decompositions from the canonical decomposition field under the
exclusions above, with class 0 for every supplementary value, and the fold
table by one rule: a unit on the folded pages maps to its simple lowercase
unless the unit itself decomposes, in which case it maps to itself; plus
the Georgian capitals' mapping to Mkhedruli, the ignorable set and the
U+0000 rule. Each exceptions row overrides one derived value and names the
observation that requires it; the 30 rows are the units E3 observed on the
folded pages that have a lowercase yet fold to themselves: U+01A6, U+01F6,
U+01F7, U+03D8, U+03DA, U+03DC, U+03DE, U+03E0, U+03F4, U+048A, U+048C,
U+048E, U+04C5, U+04C9, U+04CD, U+04D4, U+04D8, U+04E0, U+04E8, the even
units U+0500 to U+050E, U+2126, U+212A and U+212B. The Unicode data's
permission notice is already in `third-party/Unicode-LICENSE.txt`.

The tables are accepted by observation and confirmed by source. E3 creates every BMP scalar value
and every supplementary value Unicode 3.2 assigns as a name on a native
case-insensitive HFS Plus and a case-sensitive HFSX volume, records the
stored units macOS returns, which names collide and the catalog order, and
derives from them a per-unit fold table with a status per row. The gate
compares the generated tables with the observable rows, and a per-row
membership test is not the gate, because the observation fixes some folds
only jointly:

* every stored unit equals its own decomposition, and every decomposing
  value's stored units equal the table's;
* a `determined` row's fold equals the generated fold;
* the `ambiguous` rows, 124 collision classes on `H+` whose fold the
  catalog order fixes only up to a monotone choice, satisfy the chain
  constraint: each generated fold is a member of its class and the folds
  strictly ascend along the chain of classes in catalog order;
* an `ignorable` row is ignorable in the generated table, and only those;
* the `pair-only` rows, the 2048 surrogate units that occur only inside a
  pair, strictly ascend in the generated table within each of the two
  groups, U+D800 to U+DBFF and U+DC00 to U+DFFF; a value for them is not
  observed, since no name holds a surrogate unit alone;
* the `position` row, U+0000, sorts after every unit up to U+FFFD, which
  the value `0xFFFF` satisfies; its relation to U+FFFE and U+FFFF is not
  observed;
* NetBSD's independently derived folding table [NB] equals the generated
  table on every row.

The `unobservable` rows are the 12146 units no stored name holds on macOS:
the 12142 BMP values that decompose, U+FFFE and U+FFFF, which macOS escapes
as percent text, and U+003A and U+2400, which it maps away. For these the
derivation rule stands unobserved, with NetBSD's table as the witness
through the last row of the gate. The gate also checks the decomposition
rule's stable sort of mark runs against the E3 small-names vectors, which
cover the rule's three-or-more-mark and precomposed-starter cases: the
record's [E3 section](../docs/hfsplus-research.md#e3-unicode) holds the
vectors the gate compares. The writer capability is not registered until
the gate passes, because a writer whose order differs from macOS's emits a
catalog that macOS cannot search.

The record's Apple source cross-check confirms every row: the fold table
equals the three copies in Apple's kernel and `fsck_hfs` on all 65536
units, the 30 exception units and the 16 ignorable units included; the
970 decompositions and the 297 nonzero classes equal the kernel's; and
NetBSD's table agrees. Apple's class table carries 25 more nonzero units,
all assigned after Unicode 3.2.0, none of which belongs to the set that
bounds a run, so they are never sorted, and class 0 for them gives the
same names. The generated tables are therefore not altered.

`fsck_hfs -n` calls a name holding one of 23 adjacent sequences, in the
final sorted stored name, an illegal name, anywhere in the name and after
any starter, and the volume fails its check: U+0306 U+0307; U+0308 U+030D;
U+00A8 U+030D; U+030D after each of the Greek letters U+0391, U+0395,
U+0397, U+0399, U+039F, U+03A5, U+03A9, U+03B1, U+03B5, U+03B7, U+03B9,
U+03BF, U+03C5, U+03C9 and U+03D2; U+09AC U+09BC; U+0A21 U+0A3C; U+0B2F
U+0B3C; U+0E4D U+0E32; and U+0ECD U+0EB2. They are the sequences a repair
routine for names written under the pre-10.2 decomposition rules
rewrites, and the host's `fsck_hfs` flags every one (E3, the record's
`fsck-sequences.tsv`, and the cross-check). macOS itself creates and
stores such names. The reader delivers them: the format defines nothing
against them. The writer refuses them, under its
[name rules](#writer-names), from one table in its name code, so an fsimg
volume never fails `fsck_hfs` because of a name.

## Extended attributes

### Keys and order

The attributes tree holds attributes by node ID, not by path. Its key is
`keyLength` (2, `12 + 2 * n`), a pad (2), `fileID` (4), `startBlock` (4,
zero for a primary record, the logical block offset for an overflow record),
`nameLength` (2, `n` from 1 to 127) and the name in UTF-16. Keys compare by
`fileID`, then by name, then by `startBlock`: the name precedes
`startBlock` in comparison although it follows it in the bytes. Names
compare as unsigned UTF-16 units with no folding on either variant:
`com.fsimg.CASE`, `com.fsimg.Case` and `com.fsimg.case` are three
attributes in that order on HFS Plus and on HFSX alike (E5). [HF], [R]

### Records

The leaf data begins with a four-byte `recordType`: [HF]

| Type | Record | Layout |
| ---: | --- | --- |
| `0x10` | Inline | type, two reserved words, `dataSize` (4), then `dataSize` bytes |
| `0x20` | Fork | type, a reserved word, an 80-byte fork data record |
| `0x30` | Extents | type, a reserved word, eight extent descriptors |

An inline record's `16 + dataSize` fits its record; that is the only
inline bound, since the size at which macOS switches to a fork record is a
host choice, not a format limit (3802 bytes at a 16-unit name in a tree of
8192-byte nodes, with 3803 the first fork record, E5). A fork record's extents
follow the [extent rules](#extent-rules), continued by `0x30` records with
the same `fileID` and name whose `startBlock` follows the continuity rule.
Attribute fork blocks belong to the attribute, not to the file. Any other
type is `FSIMG_EUNSUPPORTED`. An absent attributes tree is valid and means
that no object has stored attributes; the volumes of 2001 to 2003 have none
(E10).

### Stored attributes and the extra-data mark

A stored attribute is a leaf record of this tree other than the
`com.apple.decmpfs` attribute of a compressed file, which the
[compression rules](#transparent-compression) consume. Every other record
holds data the entry model cannot carry, the ACL in
`com.apple.system.Security` included, which macOS writes for `chmod +a` and
on its `.fseventsd` folder (E2, E5); macOS keeps no attribute of its own on
a file hard link's indirect node (E4, E5), and writes
`com.apple.system.hfs.firstlink` only for directory hard links, keyed by
the target folder in the private directory folder, which the reader does
not walk (the record's Apple source cross-check). Finder information and the
resource fork, which macOS also presents as attributes, are not stored
here. An entry whose object has a stored attribute, or a resource fork with
`logicalSize > 0` that compression does not consume, carries
`FSIMG_META_EXTRA_DATA`. A compressed file of declared size 0 consumes
nothing, since no read reaches its content, so a nonempty resource fork or
attribute payload behind it carries the bit too. The reader delivers its data fork and POSIX
metadata; the resource fork, attribute names and attribute values are not
delivered. The bit makes that omission visible in
[listings](design-cli.md#listing), where `t -v` marks the entry and plain `t`
counts it, in `--json` and in the extraction loss report, under the
[metadata design's rule](design-metadata.md#metadata-fields). [HF], [R]

A record whose `fileID` names no catalog primary is `FSIMG_ECORRUPT` under
verification. Catalog flag `0x0004` is set exactly when the tree holds a
record for the node ID: macOS sets it with the first attribute and clears
it when the last one is removed (E5). The flag tells the reader whether to
look, and verification requires the equivalence.

## Symbolic links, FIFOs, devices and sockets

A symbolic link is a file record with `S_IFLNK`; its target is the data
fork's first `logicalSize` bytes, which is the entry's size, and its
resource fork is empty. A target of 0 or more than 1023 bytes, or one
containing a NUL byte, is `FSIMG_EUNSUPPORTED`: 1023 bytes is the longest
target fsimg holds evidence for, the bound the capability record states.
The bytes are never interpreted, resolved or translated between `/` and
`:`. A FIFO is a file record with `S_IFIFO`, size zero and no delivered
content; its forks are claimed like any file's. A socket and a character
or block device are file records with the matching type, both forks empty,
else `FSIMG_ECORRUPT`; a device's `special` is the macOS `dev_t`, bits 24
to 31 `dev_major` and bits 0 to 23 `dev_minor`, and a socket's `special` is
not interpreted. [TN], [R]

## Hard links and private directories

### File hard links

A file hard link is a set of stub records, one per name, each a file record
with empty forks and a nonzero link reference in `special`. The object
itself is the **indirect node**: a file named `iNode` followed by the
reference in decimal, without leading zeros, inside the private data
folder. Its forks hold the content and its record the owner, mode, times
and, in `special`, the link count. The first `link(2)` of a file turns it
into the indirect node and gives every name, the first included, a new stub
with a new node ID; unlinking back to one name leaves the node and that one
stub (E4). [TN], [R]

A file record is a file hard-link stub exactly when macOS treats it as one:
its Finder type is `hlnk`, its creator `hfs+`, and its `createDate` equals
the root folder's catalog `createDate` or that of the file hard-link data
folder, `\0\0\0\0HFS+ Private Data`. Both are UTC catalog dates; the
volume header's `createDate`, which holds local time, is not one of them,
and neither is the directory hard-link folder's date, which macOS never
uses in this test. A record with those codes and another date is an
ordinary file, as macOS reads it, and flag `0x0020` is not part of the
test: a stub without it still reads as a link (E4's patched records). Both
dates occur natively: current macOS dates new stubs from the file hard-link
data folder, and the Mac OS X 10.0 disc's stubs match the root folder's
date alone (E4, E10). The test is the macOS filesystem's, the same for a
lookup and for a listing [HL].

What the records hold natively (E4, both variants): a stub has flags
`0x0022`, Finder flags `0x0100`, text encoding 126, mode `0100444`, owner
flags `0x02`, all four dates equal to the private data folder's
`createDate`, and `special` the reference; the pre-10.2 stubs of E10 have
`fileMode` 0 instead. The indirect node has flags `0x00A2`, no Finder
codes, and the file's own owner, mode and times, which macOS reports for
every name. Nothing else in a stub is interpreted: the stub's mode, owner
flags and dates are not the entry's. The chain fields are navigation aids
fsimg does not read: the indirect node's reserved word names the newest
stub, and each stub's `ownerID` and `groupID` hold the next stub toward
the newest and toward the oldest, 0 at the ends.

The reader resolves a stub by looking the name up in the private data
folder. The entry takes its kind and size from the indirect node, which is
a regular file or a symbolic link, its metadata from the indirect node, and
its name from the stub. Every stub is a separate entry, because the entry
model has no hard-link representation, as the
[architecture](architecture.md#writer-and-reader-independence) states; content is
delivered from the one indirect node. A missing indirect node, a zero
reference, an indirect node that is a stub itself or of another kind, or a
stub with a nonempty fork is `FSIMG_ECORRUPT`. Verification checks that
each indirect node's link count equals its stubs and that no stub lies in
the private data folder, where no walk reaches it; the chain fields are not
checked. macOS creates no hard link to a symbolic link: `link(2)` follows
the link, `linkat(2)` without `AT_SYMLINK_FOLLOW` and `ln -P` fail with
`ENOTSUP`, the one refusal in the kernel's link path (E4, the record's
Apple source cross-check). Whether an older system or another
implementation ever wrote one is not established. When reading, macOS
resolves a stub to whatever record the indirect node holds without
testing its kind, so the rule that the node's own kind decides follows
macOS and is the safe default: it covers the case without a mechanism of
its own, and is tested on synthetic fixtures only. [TN], [HF], [HL], [R]

### Directory hard links

A directory hard link is a file record that refers to a folder named
`dir_` and a number in the private directory folder. A record is one
exactly when its Finder type is `fdrp`, its creator `MACS`, flag `0x0020` is
set, and its `createDate` equals the root folder's catalog `createDate` or
the file hard-link data folder's, the same two dates as for a file stub:
macOS creates both stub kinds with the file folder's date, and the
directory folder's date is never used [HL]. The reference is not part of
the identification, as it is not for a file stub; macOS's lookup accepts
any value, and only its directory listing wants one of at least 16. The
native stub (`h-hfsplus-16777216-e4-dirlinks-J`, where a journaled volume
let `link(2)` succeed on a directory) passes every part: `fdrp`/`MACS` with
Finder flags `0x8000`, flags `0x0022`, mode `0100444`, owner flags 2, all
four dates equal to the file hard-link data folder's `createDate`,
`special` the target folder's ID, and empty forks; its
target `dir_22` sits in `.HFS+ Private Directory Data\r` with flags
`0x00A4`, the directory's own owner, mode and times, and `special` the
number of names. A Finder folder alias carries the same codes and Finder
flags but is an ordinary file: the look-alike of the same capture has flags
`0x0082` and its own date, so it fails two parts of the test
(E4). Directory hard links occur mainly in Time Machine backups. They are
an unsupported kind: the directory holding one fails preparation with
`FSIMG_EUNSUPPORTED` naming the link. Following such links safely needs a
visited set and a path-explosion bound that no other format needs.

### Private directories and format-owned files

These root children are format-owned. The reader validates them and does
not expose them; full verification checks and owns their contents:

| Name (stored units) | Object | Rule |
| --- | --- | --- |
| Four U+0000 then `HFS+ Private Data` | Private data folder, the file hard-link data folder | A folder; holds indirect nodes and `temp` files of open-but-unlinked objects; its `createDate` is the date both stub kinds carry |
| `.HFS+ Private Directory Data` then U+000D | Private directory folder | A folder; holds directory hard-link targets |
| `.journal` | Journal file | Format-owned only on a journaled volume, where its data fork is exactly the journal, one extent of `size` bytes, and locates it under the [info block rule](#journal-info-block) |
| `.journal_info_block` | Journal info block file | Format-owned only on a journaled volume, where its data fork is exactly the journal info block |

On an unjournaled volume the two journal names are ordinary files. A
private folder that is not a folder, a journaled volume missing either
journal file, or a journal file whose fork disagrees with the journal info
block, is `FSIMG_ECORRUPT`. A `temp` file in the
private data folder is a legal leftover of a volume not cleanly unmounted;
verification owns its storage and counts it. Other root entries such as
`.Trashes`, `.fseventsd` and `.HFS+ Private Directory Data` without its
final carriage return are ordinary. No formatter creates the private
folders; macOS's first writable mount creates both, with owner 0, modes
`040000` and `041555`, owner flags 2 and flags `0x0010`, and adds
`.fseventsd` on unmount (E1, E2, E4). [TN], [HF], [R]

## Transparent compression

A regular file with `ownerFlags` `0x20` is compressed: its content is not in
its data fork but in its `com.apple.decmpfs` attribute and, for some types,
its resource fork. Reading only the data fork would deliver an empty file,
so the reader decodes it, and an undecodable one is refused, never
delivered short. [HF], [DC]

### Header

The attribute's value begins with a 16-byte header: the four bytes
`66 70 6d 63`, a compression type (4, little-endian) and the uncompressed
size (8, little-endian); a value shorter than 16 bytes is `FSIMG_ECORRUPT`.
The uncompressed size is the entry's size and the
only content length; the data fork's `logicalSize` is zero on every type
(E6). An uncompressed size above `2^63 - 1` is `FSIMG_ECORRUPT`. [DC], [R]

### Types

| Type | Codec | Payload |
| ---: | --- | --- |
| 1 | None | The bytes after the header are the content |
| 3 | zlib | The bytes after the header |
| 4 | zlib | Chunks in the resource fork |
| 7 | LZVN | The bytes after the header |
| 8 | LZVN | Chunks in the resource fork |
| 11 | LZFSE | The bytes after the header |
| 12 | LZFSE | Chunks in the resource fork |

Any other type, including the dataless types of newer systems, is
`FSIMG_EUNSUPPORTED`, found at directory preparation and naming the entry.
zlib is the system library; LZVN and LZFSE are the
[imported decoders](design-codecs.md#imported-lzfse-and-lzvn-decoders).
`afsctool` writes types 3, 4, 7, 8, 11 and 12 and `ditto --hfsCompression`
types 7 and 8; neither writes type 1, which the table keeps for its
documented meaning [DC]. A compressed file's catalog record has
`ownerFlags` `0x20`, flag `0x0004` for the attribute, and an empty data
fork (E6). [DC], [R]

### Inline payloads

Types 1, 3, 7 and 11 hold the whole content after the header. A payload
whose first byte is the codec's raw marker holds the content uncompressed
after that byte: `0xFF` for zlib, which no zlib stream begins with, and
`0x06` for LZVN, its end-of-stream code. LZFSE has no raw marker:
incompressible content is an LZFSE uncompressed block, `bvx-` with a count
and the bytes, which the decoder reads like any other block (E6). The
decoded length equals the uncompressed size exactly. That size is at most
65536 bytes, the decode window, so an inline payload decodes once and
whole; a larger inline size is `FSIMG_EUNSUPPORTED` naming the entry. No
size alone decides where a native compressor stores a file: the whole
attribute, its 16-byte header included, is at most 3802 bytes, the size
the kernel's attribute reader bounds and the largest ordinary attribute
E5 measured, so a file stays inline only when its encoded form fits, and
otherwise goes to the resource fork. Zeros of 65536 bytes stay inline,
65537 bytes never do, and text of a few thousand bytes moves to the fork
as soon as its encoding exceeds the bound; the largest inline compression
attribute observed is 3612 bytes (E6, and the record's Apple source
cross-check). A larger native inline size would widen this rule with a
decoder that streams the payload, not with a larger window. [DC], [R]

### Resource-fork payloads

Types 4, 8 and 12 split the content into chunks of 65536 decoded bytes,
the last one shorter, and keep them in the resource fork, read through its
extents like any fork:

* **Type 4** frames them as a classic resource file: a 16-byte header of
  big-endian data offset, map offset, data length and map length; at the
  data offset a big-endian resource length; then a little-endian chunk
  count followed by that many little-endian `(offset, length)` pairs,
  offsets relative to the start of the count. The header's fields are
  related: the resource length is the data length less 4, and the fork's
  `logicalSize` is the data offset plus the data length plus the map
  length; the data offset is 256 and the map 50 bytes on every native file
  (E6). The map offset is at or after the end of the data and the map ends
  inside the fork, so the map never overlaps the data. The reader requires
  the relations and does not assume the constants. Two intervals of the fork, with `D` the data offset, are
  distinct. The table interval holds the count and the pairs, `[D + 4,
  D + 8 + 8 * count)`: the length word is at `D`, the count after it, and
  a chunk's offset is counted from the count, so the first chunk starts
  `4 + 8 * count` bytes after the count (E6). The payload interval follows
  the table interval and ends where the declared resource data ends, `D`
  plus the data length, the resource length of bytes after the length
  word. The table interval must end inside the payload's declared end;
  table entries are read from the table interval and every chunk lies in
  the payload interval, so a chunk that overlaps the table or reaches
  past the declared end, into the map or beyond, is `FSIMG_ECORRUPT` even
  when it lies within the fork. Type 4 stores each chunk's own length
  beside its offset; types 8 and 12 store only successor offsets, below.
  [DC], [R]
* **Types 8 and 12** begin the fork, at offset zero, with a table of
  `count + 1` little-endian words, the entry for chunk `i` at byte `4 * i`.
  Three quantities are distinct: the table's base, which is fork offset
  zero, its entries being successor-offset words and not pairs; the table's end `T`, which is the first word's value; and the chunk
  offsets, which are the words themselves. Chunk `i` spans
  `[offset[i], offset[i + 1])`, so `offset[0]` is `T`, the first chunk
  starts where the table ends, the offsets ascend, and `offset[count]` is
  the end of the last chunk. The reader takes `count` from the uncompressed
  size, reads the words from offset zero, and requires `T` to lie in
  `[4 * (count + 1), logicalSize]`: the table must hold its words, and it
  cannot extend past the fork. The table's length is the writer's choice,
  not the chunk count's: `ditto` writes exactly `4 * (count + 1)` bytes,
  `afsctool` pads the table with zero words to `8 * count` bytes (E6), so
  `T` is never computed from the count, and words between the last offset
  and `T` are not interpreted. [DC], [R]

The chunk count equals `ceil(uncompressed size / 65536)`. Every table and
chunk lies within the fork's `logicalSize`; a stored chunk is at most 69632
bytes, 65536 and a margin above the worst-case expansion of every codec and
the raw marker, and the largest native chunk observed is 66280 bytes, an
LZFSE block that grew over random content (E6); chunks are ascending and
do not overlap; each decodes to exactly its length, and a chunk beginning
with the raw marker holds raw bytes: a zlib chunk `0xFF`, an LZVN chunk
`0x06`, an LZFSE chunk an uncompressed block as above. Reading checks each
chunk it decodes against its table neighbours and the fork; verification
checks the whole table. [DC], [R]

### Content and failures

A compressed file's content is delivered as [decoded spans](design-content.md#decoded-spans):
the driver decodes the chunk containing an offset into its decode window,
one chunk at a time, so sequential reading decodes each chunk once and
memory does not grow with the file. Compression with no attribute, a bad
signature, an uncompressed size above `2^63 - 1`, a table, chunk or decoded
length that violates the rules above, or a nonempty data fork is
`FSIMG_ECORRUPT`. A `com.apple.decmpfs` attribute on a file without
`ownerFlags` `0x20` is an ordinary stored attribute. The writer never sets
the flag.

## Journal

HFS Plus journaling records metadata transactions in a circular log so that
a mount can replay them after a crash. fsimg never replays: a volume whose
journal holds transactions is refused, and one whose journal is empty is
read as its base structures describe it. [TN]

### Journal info block

With volume bit 13 set, `journalInfoBlock` is the allocation block holding
a 180-byte journal info block: `flags` (4), a 32-byte device signature, the
journal's byte `offset` from the volume start (8), its `size` (8) and 128
reserved bytes. Flag `0x1` places the journal inside the volume, `0x2` on
another device, and `0x4` marks it as never initialized: `newfs_hfs -J`
and `mkfs.hfsplus -J` leave flags `0x5` and an all-zero journal, and the
first mount writes `0x1` and the header (E1, E8). The block lies in the
content domain.

The journal's place in the volume comes from the catalog, not from the
info block: on a journaled volume the root holds the format-owned file
`.journal`, whose data fork is exactly the journal, so the journal is the
one extent of that fork, `size` bytes long, in the content domain and in
whole allocation blocks, and the header, when there is one, is read at the
extent's start (E1, E8). The info block's `offset` must then equal
that location counted from the volume's own start. Any other value, a
`size` that differs from the fork's `logicalSize`, a fork of more than one
extent, or a journaled volume without the file is `FSIMG_ECORRUPT`.
Nothing is converted, and nothing is searched for, because the location
comes from the validated catalog fork rather than from the stored
offset. With bit 13 clear, `journalInfoBlock` is not interpreted. The
flags are read against this table, and the first matching row decides:

| Flags | Rule |
| --- | --- |
| `0x2` set | External journal: `FSIMG_EUNSUPPORTED`; no header is read and the `.journal` file, the `offset` and the `size` are not examined |
| Neither `0x1` nor `0x2` | `FSIMG_ECORRUPT` |
| `0x4` set | Never initialized: no header is read; the location rule applies |
| `0x1` set, volume [wrapped](#the-hfs-wrapper) | `FSIMG_EUNSUPPORTED`; no other journal check runs |
| `0x1` set | Local journal: the location rule applies and the header is read at the fork's extent, once |

A wrapped volume with an initialized journal is refused because the stored
offset's base is not established: macOS's early mount path takes it as
counted from the embedded volume's start, its late path adds the embedded
start and writes the sum back, so the one `mkfs.hfsplus -w -J` image holds
12288 under flags `0x5` and 57344, the embedded start plus 12288, after one
macOS mount, with the header at image byte 57344 (E11, and the record's
Apple source cross-check). A stored `E + L` is read as `E + L` by one path
and as `2E + L` by the other, and each location can hold a different
header, so a pending journal could be hidden at the one fsimg does not
read. A wrapped volume with flags `0x5` has no header and reads normally,
its offset counted from the embedded volume's start as E11 records it.
[TN], [R]

### Journal header

macOS's journal code also accepts an older `magic`, `0x4A484452`, at open and
at replay, converts it to the current value and checks the checksum only of a
current header. The reader refuses it. [HJ]

At the journal's offset is a 44-byte header: `magic` `0x4A4E4C78`, `endian`
`0x12345678`, `start` (8), `end` (8), `size` (8), `blhdr_size` (4),
`checksum` (4) and `jhdr_size` (4). The journal writes its header in the
byte order of the host that created it, which the two constants reveal:
every macOS-written header in the corpus is little-endian, `78 4C 4E 4A 78
56 34 12`, with `blhdr_size` 8192 (16384 on the NPS `hfsjtest1` specimens)
and `jhdr_size` 512 (E1, E8); a big-endian header comes from a PowerPC-era
host and is read by the same rule. The checksum covers the 44 bytes with
the checksum field zero: starting from zero, each byte `c` updates the sum
`s` to `(s << 8) ^ (s + c)` in 32-bit arithmetic, and the stored value is
the complement of the result. `size` equals the info block's size and lies
in 256 KiB to 1 GiB, the range macOS opens; `start` and `end` lie in
`(0, size]` and are multiples of 512, the bounds macOS accepts, and a
value equal to `size` is read as `jhdr_size`, the position the log wraps
to, before the two are compared. After that, `start == end` means no
transaction is pending, at whatever position the pair sits (63488, 113664,
316928 and others on clean volumes), and residue of completed transactions
past `end` means nothing; `start != end` means transactions are pending,
and `start > end` is valid once the log has wrapped (E8, and the record's
Apple source cross-check). A value outside these bounds is
`FSIMG_ECORRUPT`, where macOS refuses the mount. A wrong checksum is also
`FSIMG_ECORRUPT`: macOS only logs one and reads the state fields anyway,
but fsimg, which never replays, cannot trust `start` and `end` from a
header that fails its own check, and no native volume in the record fails
it; the rule stands unless one does. [TN], [R]

### Reader policy

| State | Reader |
| --- | --- |
| Bit 13 clear | Read normally |
| Journaled; local journal; valid header; `start == end` after the wrap rule | Read normally; `info` reports a clean journal |
| Journaled; info block flag `0x4` | Read normally; `info` reports an uninitialized journal, which holds no transactions |
| Journaled; wrapped volume; local journal (flag `0x1`) | `FSIMG_EUNSUPPORTED`: macOS's own paths disagree on the stored offset's base, so fsimg cannot establish which location controls replay |
| Journaled; `start != end` after the wrap rule | `FSIMG_EUNSUPPORTED`: the base structures may be stale until a mount replays the journal (the volume header of E8's interrupted image counts 1135 files and the replay makes it 1137) |
| Journaled; flag `0x2` | `FSIMG_EUNSUPPORTED`: the journal is on another device |
| Journaled; header magic `0x4A484452`, in either byte order | `FSIMG_EUNSUPPORTED`: an older journal header form the reader does not decode |
| Journaled; info block or header invalid, `.journal` missing, or its fork disagreeing with the info block | `FSIMG_ECORRUPT` |

Open checks the header and the trees before the journal, so a pending
journal's stale base structures can fail one of those earlier checks with
`FSIMG_ECORRUPT` instead of reaching the refusal above, as that order requires.

A volume whose journaling `diskutil disableJournal` turned off has bit 13
clear, no journal files and `journalInfoBlock` still set (E8); the first
row covers it.

## The HFS wrapper

An HFS Plus volume can be embedded in a classic HFS volume whose master
directory block, at byte 1024 with signature `BD` (`0x4244`), carries the
embedded signature `H+` at its offset `0x7C` and the embedded extent at
`0x7E`: a start block and a count, both two-byte, in the wrapper's
allocation blocks. With the wrapper's allocation block size `drAlBlkSiz` (4
bytes at `0x14`) and the first allocation block's 512-byte sector
`drAlBlSt` (2 bytes at `0x1C`), the embedded volume starts at
`512 * drAlBlSt + start * drAlBlkSiz` and is `count * drAlBlkSiz` bytes
long. [TN]

The reader opens that range as a slice and reads it as an `H+` volume, its
alternate header at the end of the embedded range. `drAlBlkSiz` is a nonzero
multiple of 512, the range lies inside the outer slice, and the embedded
volume's signature is `H+` with version 4; anything else is
`FSIMG_ECORRUPT`. The embedded volume's block size need not equal the
wrapper's: `mkfs.hfsplus -w` and the Blender 2.28 image use 4096 for both,
with `drAlBlSt` 8 and the embedded volume at byte 45056, and the Mac OS X
install discs wrap 4096-byte volumes in 73728-byte outer blocks; in every
case the embedded range equals `N * B` of the volume inside it (E10, E11).
The wrapper is an envelope around the HFS Plus volume, as a partition table
is around a partition: its own catalog, bitmap, which marks every block
used, and files, among them the bad-block file whose one extent covers the
embedded range (E11), are not part of the HFS Plus volume and are neither
interpreted nor exposed. Verification of a wrapped volume therefore covers
the embedded volume and the wrapper fields that locate it, and is complete
when they are; the manual states that the outer HFS volume is not checked.
The writer never creates a wrapper: a usable one is a complete classic HFS
volume, and no current macOS tool formats one.

A wrapped volume whose journal is initialized (info block flag `0x1`) is
refused with `FSIMG_EUNSUPPORTED`, because the [journal info
block](#journal-info-block) offset has no established base on a wrapped
volume. This is a limitation of the reader; refusing is the safe default,
and a wrapped volume with a never-initialized journal reads normally.
[TN], [R]

## Reader profile

| Property | Accepted |
| --- | --- |
| Signatures | `H+` version 4, `HX` version 5, `BD` with an embedded `H+` |
| Geometry | `B` a power of two from 512 to `2^31`; `N >= 1`; `N * B <= V`; `V` a multiple of 512 |
| Volume attributes | The closed table; bits 29 and 30 unsupported |
| Trees | The tree parameter table; depth at most 16; node maps chained |
| Catalog flags | The closed table; `0x1000` unsupported |
| Comparators | Folded and binary catalogs; binary attribute names |
| Kinds | Regular, directory, symbolic link, FIFO; sockets and devices read-only; file hard links resolved, the indirect node's own kind deciding, so a symbolic-link indirect node, which macOS does not create and no other writer is known to, reads as a symbolic link; directory hard links unsupported |
| Names | 1 to 255 units, decomposed under Unicode 3.2 tables; `:`, U+2400, `.`, `..` and spellings the tables do not reproduce unsupported; a Mac OS 8.1 to 9.2 volume whose names follow Unicode 2.1 is refused explicitly until a specimen widens the tables |
| Symbolic-link targets | 1 to 1023 bytes |
| Compression | Types 1, 3, 4, 7, 8, 11 and 12 |
| Attributes | Inline, fork and extent records; marked as extra data, not delivered |
| Journal | Unjournaled, or journaled with an empty or uninitialized local journal; on a wrapped volume, only an uninitialized one |
| Wrapper | Read; outer volume not verified; an initialized journal inside it unsupported |

The reader capability advertises files, directories, symbolic links, FIFOs
and the read-only socket and device kinds; owner, mode and the created, modified, changed and accessed times in
whole UTC seconds; 255-unit names; a label of 255 UTF-16 units, so
`max_label_bytes` is 510, which the volume information's 765-byte UTF-8
label holds; a link-target limit
of 1023 bytes; a 512-byte sector; and allocation units from 512 bytes to
`2^31`. Volume information reports the volume name as the label, the volume
identifier, the clean flag, the journal state, the header counts and the
node sizes as details, and the profile as one of `HFS Plus`,
`HFS Plus in HFS wrapper`, `HFSX case-sensitive` and
`HFSX case-insensitive`; the `journal` detail says whether the volume is
journaled, so the profile does not.

## Writer profile

The writer builds one clean, unjournaled, unwrapped volume per plan, every
B-tree bulk-built from sorted records and every fork one contiguous extent,
so it never inserts into a tree, splits a node, or emits an overflow record.
Where the format leaves a choice, the writer follows what `newfs_hfs`
writes for an empty volume of the same geometry, as E1 records.

Four choices depart from `newfs_hfs` and are provisional: no native check
has accepted them yet. Each names the step of the
[native qualification cycle](../docs/hfsplus-plan.md#9-native-qualification-and-writer-registration)
that closes it and the default the writer takes if that step refuses it.

| Provisional value | Closing check | Safe default |
| --- | --- | --- |
| `lastMountedVersion` `fsim` | `fsck_hfs -n` and a mount accept the volume | `10.0`, the value every unjournaled native volume carries (E1, E2) |
| 4096-byte catalog nodes at every volume size | The cycle on a volume of 1 GiB and above with blocks of 4096 and above | `newfs_hfs`'s 8192-byte nodes at those sizes (E1) |
| A one-node extents tree and a one-node attributes tree | The cycle's append-until-fragmentation and attribute steps, which make macOS insert into both | `newfs_hfs`'s eight-node trees (E1) |
| The smallest volume minimization yields | The cycle on that volume | A floor of 1 MiB, the smallest volume `newfs_hfs` makes (E1) |

### Volume

| Field | `hfsplus` | `hfsx` |
| --- | --- | --- |
| Signature, version | `H+`, 4 | `HX`, 5 |
| Catalog `keyCompareType` | `0xCF` | `0xBC`: case-sensitive |
| `attributes` | `0x80000100`: clean, unused B-tree nodes zeroed | the same |
| `lastMountedVersion` | `fsim` | `fsim` |
| `journalInfoBlock` | 0 | 0 |

`fsim` identifies fsimg as the volume's last writer without claiming to be
Apple's implementation. Bit 31 states a fact about the output, every unused
node is zero, and is what every native formatter sets (E1). `createDate`
is meant to hold local time, which `newfs_hfs` writes and `mkfs.hfsplus`
does not (E1), but fsimg asserts no zone: it holds the format time's UTC
calendar seconds since 1904, the same value as the root folder's
`createDate`, so the result does not depend on the host's time zone.
`modifyDate` and `checkedDate` hold the format time; `backupDate` is zero.
`writeCount` is 0 and `encodingsBitmap` 1; `dataClumpSize` and
`rsrcClumpSize` are `min(16 * B, 65536)`, and the bitmap's and each tree
fork's `clumpSize` and each B-tree header's `clumpSize` are its
`logicalSize`, the native formatter's values (E1). A tree fork of `2^32` bytes
or more states `2^32 - 1` instead, because the field holds no more; that
saturation is provisional, since no observed volume has a tree fork that large.
The native cycle on a catalog of 4 GiB closes it; if that cycle cannot run the
saturation stands, because `clumpSize` is a hint that readers ignore and the
alternative of a wrapped value would state a smaller clump than the fork.
`nextCatalogID`
is one more than the largest node ID. Finder information is zero except
words 6 and 7, the volume identifier: the first eight bytes of
`volume_uuid`, zero when no identifier is given. The writer creates no
case-insensitive HFSX volume: `hfsplus` already is one.

### Layout

| Blocks | Object |
| --- | --- |
| `0` to `r - 1` | Reserved: zero boot blocks and the primary header |
| next | Allocation bitmap: `ceil(ceil(N / 8) / B)` blocks |
| next | Extents tree |
| next | Attributes tree |
| next | Catalog tree |
| next | Each file's and symbolic link's data fork, in catalog ID order |
| free | |
| last `t` blocks | Reserved: the alternate header |

The order is the native one: every formatter lays out the bitmap, the
extents tree, the attributes tree and the catalog tree in that sequence
(E1), with free space between the last two that the writer does not leave.
The writer creates an attributes tree on every volume, as `newfs_hfs`,
`hdiutil` and `mkfs.hfsplus` do (E1); it is one header node, empty, and
spares macOS from creating the tree when it first writes an ACL,
`.fseventsd`'s attribute or a compressed file. No private folder is
written: no formatter creates them, and the first writable macOS mount
does (E1, E2).

The writer's `t` is `ceil(1024 / B)` at every volume size, the blocks a
native formatter marks wherever the alternate header lies. A size that puts
the alternate past `N * B` still reserves them, unlike the reader's content
domain: `fsck_hfs -n` reports a clear bit there as under-allocation and a wrong
free count, so the writer cannot leave them free.

Allocation is one prefix `[0, A)` plus the reserved tail; `freeBlocks` is
`N - A - t` and `nextAllocation` is `A`. The bitmap's `logicalSize` is its
whole allocation, and its bits are set for `[0, A)` and the tail and clear
elsewhere, padding included.

### B-trees

A tree is built from its complete, sorted leaf records: records are packed
into leaves in order, never split across nodes; each level above holds one
index record per node below, keyed by that node's first key, until one node
remains, the root. Node 0 is the header, nodes 1 onward the leaves in key
order, then each index level bottom-up, then map nodes when the header map
cannot cover `totalNodes`. The fork is the used nodes rounded up to whole
allocation blocks, every unused node zero and clear in the map. The extents
and attributes trees hold no record. The node sizes are fixed at every
block size and volume size: 4096 bytes for the extents and catalog trees
and 8192 for the attributes tree. These are the sizes `newfs_hfs` writes
on every volume up to 128 MiB and at 1 GiB with blocks below 4096; it
switches the catalog to 8192 at 1 GiB with blocks of 4096 and above (E1),
a size-dependent choice the writer does not copy, so that a tree's node
count depends on the catalog alone and the block size only rounds its fork
up to whole blocks; the 4096 bytes at those sizes are a
[provisional value](#writer-profile). 4096 is also the catalog node size of
every old volume (E10) and of the native tree whose map nodes E9 records,
so the writer's map nodes, when a catalog needs them, have the one native
layout observed: one record from offset 14 of `M - 20` bytes, the layout
E9 shows at `M = 4096`. Greedy packing can leave a non-root index level whose last
node holds one record; macOS leaves such nodes itself and `fsck_hfs -n`
accepts them (E9), so the packer has no rule against them.

### Catalog records

Node IDs follow catalog ID order: the root is 2 and catalog ID `i >= 1` is
`15 + i`. Every file, symbolic link, FIFO and folder gets a primary record
and a thread; every file record sets flag `0x0002`. Folders carry `valence`;
on `hfsx` every folder, the root included, also sets flag `0x0010` and its
folder count, and on `hfsplus` neither, which is how `newfs_hfs` formats and
macOS maintains each variant (E1, E2). Symbolic links carry Finder type
`slnk` and creator `rhap`; every other Finder byte, the BSD flags and
`textEncoding` are zero. The BSD `special` word is 1 on every record, the
value macOS writes on its own files, folders, links and FIFOs (E2): on a
file macOS reports the word as the link count, so 1 states the truth, one
name, where 0 would present a file with no names. The root's `textEncoding` is therefore 0, as
`mkfs.hfsplus` writes it, where `newfs_hfs` writes 126; the date-added
word and write counter macOS keeps in the Finder information of its own
records are zero with flag `0x0080` clear, the state of every native
symbolic link and FIFO (E1, E2). A regular file's data fork is its content
in one extent of `ceil(size / B)` blocks; a symbolic link's is its target;
a FIFO's and every resource fork are empty. No attribute, compression or
hard link is written: the entry model carries none.

### Writer names

An image-path component is converted to UTF-16, `:` becomes `/` and U+2400
becomes U+0000, the two exchanges macOS makes, and the result is
decomposed; it must then be 1 to 255 units, or planning fails with
`FSIMG_EINVAL` naming the entry. The same status refuses, as invalid
names:

* a component that is not valid UTF-8, or whose UTF-8 encodes U+FFFE,
  U+FFFF or a surrogate value; macOS stores these as ASCII percent text
  (E3), which fsimg never writes, because a name that reads back as other
  characters is a silent loss;
* on `hfsplus`, a name whose units are all ignorable: its namespace key is
  empty, the key the record stream reserves for threads, and a name that
  no listing shows is not worth a second key form;
* a name holding, after decomposition and sorting, one of the 23 adjacent
  sequences `fsck_hfs -n` [calls illegal](#unicode-tables-and-their-evidence)
  (E3), so that an fsimg volume never fails the native check. The 23
  sequences are one table in the writer's name code, applied to the final
  stored units; the generated Unicode tables are not altered for them.

Siblings whose namespace keys are equal
under the variant, such as `A` and `a` on `hfsplus` or two spellings of `é`
on either, fail planning with `FSIMG_EINVAL`. Because macOS searches for the
decomposed form, the writer stores it, and a listing shows that spelling:
this is the format's name encoding, which the manual states. A root entry
whose key equals that of one of the four
[format-owned root names](#private-directories-and-format-owned-files) is
refused with `FSIMG_EINVAL`, so a later native journal or link cannot
collide with it. The private data folder's name is among them because the
U+2400 exchange lets an image path spell it: four U+2400 then
`HFS+ Private Data` would store its exact units. The driver keeps the four
names in one table that the reader recognizes them by and the writer refuses
them by, so the two lists cannot drift.
The label follows the same encoding and limit; a NULL or empty label from
a library caller selects `untitled`, the native formatter's default, while
the CLI passes its own default `FSIMG`.

### Metadata and defaults

Owner and group are stored exactly; absent, they are 0. The mode is stored
exactly; absent, it is `0755` for directories and symbolic links and
`0644` for files and FIFOs. Times are stored through the shared
[unsigned-seconds helper](design-metadata-loss.md#unsigned-whole-second-selection)
with origin 2082844800 and maximum 4294967295: created, modified, changed
and accessed fill `createDate`, `contentModDate`, `attributeModDate` and
`accessDate`, and `backupDate` is zero. An absent time takes the format
time, the UTC host clock sampled once during planning; a time outside 1904
to 2040 takes the format time as a counted optional loss; in reproducible
mode every time is the epoch. A format time outside the range is
`FSIMG_ERANGE`. A file whose size needs more than `2^32 - 1` blocks fails
with `FSIMG_EUNSUPPORTED`.

### Settings and sizing

| Setting | Rule |
| --- | --- |
| `allocation_unit_bytes` | 0 or a power of two from 512 to 65536; 0 selects 4096 |
| `sector_bytes` | 0 or 512 |
| `volume_bytes` | 0 or a multiple of 512 holding the plan, with `floor(V / B) <= 2^32 - 1` |
| `label` | At most 255 units after encoding |
| `volume_uuid` | Its first eight bytes are the volume identifier |
| `epoch` | Within 1904 to 2040 in reproducible mode |

A volume of `N` blocks holds the plan when
`r + bitmap(N) + extents + catalog + attributes + objects +
ceil(min_free_bytes / B) + t <= N`, where the tree sizes depend on the
catalog and the block size, never on `N`. Automatic sizing raises `N` until the inequality holds, which ends
within a few steps because the bitmap grows one block per `8 * B` blocks,
and the volume is `N * B` bytes. Under [minimization](design-sizing.md) the
block size is searched over the writer's sizes, 4096 first and then the
others in increasing order. An exact size takes `N = floor(V / B)` and fails
with `FSIMG_ERANGE` when the plan does not fit.

## Deterministic planning and emission

Planning is a fixed sequence of passes over the frozen catalog: entries and
names, the sorted catalog record stream with its collision check, tree
packing, geometry, layout and summary. Every planned value derives from the
catalog, the options, the pinned Unicode tables and the rules above, so
output is byte-identical across source order, budgets, sort fan-in, scratch
placement, locale and time zone. Emission writes payload copies, the B-tree
nodes, the bitmap and both headers, the alternate last; every emitted byte
comes from the plan, the geometry or a profile constant, and the common
emission object has zeroed everything else, every unused node and record
gap included. The [driver design](design-hfsplus-driver.md) fixes the
passes and the emission order.

## Resource and failure requirements

Only the geometry, the header, a bounded set of B-tree node buffers, the
current record, the current fork's extent cursor and one decode window are
resident per reader. Scratch holds extent indexes, directory records,
verification streams and bitsets, and plan records and rows, never copied
payload. A directory costs one tree search and a scan of its leaf records;
entering a folder validates its thread, so the namespace is a tree by
construction and needs no visited set. A compressed chunk is decoded once
per sequential pass. Full verification reads every node of every tree once
per pass and the bitmap once. Every loop charges the session's work limit
and observes cancellation; scratch exhaustion fails explicitly.

Failures use the common statuses: unrecognized image (`FSIMG_EFORMAT`),
recognized but unobserved or unsupported (`FSIMG_EUNSUPPORTED`),
contradiction (`FSIMG_ECORRUPT`), invalid setting, name or selection
(`FSIMG_EINVAL`), content that fits no size (`FSIMG_ERANGE`), exhaustion
(`FSIMG_ELIMIT`), and source change, cancellation and I/O failure as common
code reports them. A failed lookup never becomes end of file or a zero
extent; a compressed file never reads short. A partial output is never
success. Diagnostics carry at most one bounded, escaped name and never node
bytes, attribute values or buffer contents.

## Acceptance matrix

Synthetic fixtures come from an original generator with its own offset
tables; native captures are read from a path the environment gives and are
never redistributed. Every passing case is paired with a mutation of the
same fixture that must fail with its class, so a test proves the checked
code ran. The minimum matrix:

| Area | Required cases |
| --- | --- |
| Probe and geometry | Each signature and version pairing; slice shorter than 1536 bytes and not a multiple of 512; `B` of 0, 511, 513, 512 to 65536 and `2^31`; `N` of 0 and 1; `N` above `slice / B`, which is truncation and opens reporting the missing bytes; the largest `N` and `B` together; every reserved-block layout from `B = 512` to `B = 4096`; probe exclusivity against FAT, exFAT, QNX4, QNX6 and ImageFS fixtures in both directions; a forced format |
| Header | Every attribute bit alone, bits 29 and 30 refused, an unknown bit; `FSK!`; `freeBlocks` above `N`; `nextCatalogID` below 16 and below a used ID with and without bit 12; a missing catalog or extents fork; an alternate header that disagrees in each compared field and one that is zero-filled, both `FSIMG_ECORRUPT`, and a read failure injected on the alternate's sectors kept as `FSIMG_EIO` |
| Forks | 0, 1, 8 and 9 extents on data and resource forks; 17 extents over two overflow records; a gap, overlap, repeat and extra overflow record; a zero-count used descriptor; a used descriptor after an unused one; an extent into the reserved blocks, past `N` and across the tail; `logicalSize` above allocation; an extents tree needing overflow; bad-block records |
| Bitmap | A referenced clear block; an allocated orphan; two forks sharing a block; a set padding bit; a short bitmap; `freeBlocks` off by one |
| B-trees | Node sizes at each bound for each tree; header node faults in kind, height, record count and offsets; offsets overlapping, odd and out of node; a key longer than its record; equal and descending keys within a node and across siblings; an index key that differs from its child's first key by one unit; a child of the wrong height, the header or itself; sibling cycles and broken back links; `leafRecords`, `firstLeafNode`, `lastLeafNode` and `treeDepth` off; an unmarked used node and a marked free node; map chains of 0, 1 and 2 nodes, a looping and a short chain, a map record shorter than `M - 20` that still covers `totalNodes`; a fork shorter than `totalNodes * M`; the variable-index flag toggled on each tree; empty extents and attributes trees |
| Catalog | Each record type at its exact length, one byte short and one long, an unknown type; a missing, duplicated and mismatched thread; a thread naming the wrong parent or name; a folder entered from a second parent; a cycle disconnected from the root; parent missing or a file; `valence`, `folderCount`, `fileCount` and `folderCount` each off by one; every flag bit alone, `0x1000` refused, an unknown bit; every kind in the kind table; zero BSD information on a file and a folder; node IDs 9 to 15 |
| Names | 1, 254 and 255 units; a surrogate pair at the limit; an unpaired surrogate; `:`, `/`, U+0000, U+2400, `.`, `..`, `Icon\r` and percent text; the private names exact and with one unit changed; prefix names `abc` and `abcd`, `a` and `aa`; `A` and `a` on both comparators; an exception unit against its lowercase; precomposed and decomposed `é`, Hangul, U+2000 to U+2FFF and U+F900 to U+FAFF units, a supplementary value with a Unicode decomposition, U+0F73 after U+0F72, ignorable units and an all-ignorable name before its siblings; each of the 23 `fsck_hfs` sequences delivered; selection by every spelling; a stored name that is not its own decomposition refused; two all-ignorable siblings |
| Unicode tables | The observable-rows gate of the [table evidence](#unicode-tables-and-their-evidence) on both comparators, and the digest of each generated table |
| Attributes | Inline at 0, 1 and the largest inline size; fork records with 1, 8 and 9 extents; a continuation gap; an unknown record type; a name of 127 units and one of 128; names differing only in case; an attribute for a missing node ID; a flag disagreeing with the tree in each direction, a record with an attribute and a clear flag included; the extra-data mark on a resource fork, a stored attribute, a consumed compression attribute, a size-0 compressed file with and without a fork or payload and none, listed and named in the extraction loss report |
| Links and kinds | Symbolic links of 1, 1023 and 1024 bytes and one with a NUL; FIFOs, sockets and devices; hard-link stubs to files and symbolic links, two and three names across directories, a dangling stub, a zero reference, a link count off by one; a stub with `fileMode` 0; a stub dated from the root folder and one from the private data folder; `hlnk`/`hfs+` with another date, read as a file; a directory hard link refused and its Finder-alias look-alike read as a file; `temp` files |
| Compression | Every type with content of 0, 1, 65535, 65536, 65537 and 200000 bytes; raw-marker chunks; a type 4 header, table and chunk out of range, a chunk inside the fork but past the resource payload, in the map, and a header whose relations fail; for types 8 and 12 a first word below `4 * (count + 1)`, beyond the fork, padded and unpadded, and a table whose words are read from offset zero while the first word points elsewhere; a chunk above the input bound; an inline size above 65536; an LZFSE `bvx-` block; overlapping and descending chunks; a chunk decoding one byte short and one long; an unknown type; a header shorter than 16 bytes; the flag without the attribute; the attribute without the flag; a nonempty data fork; sequential reading decoding each chunk once |
| Journal | Unjournaled, clean empty journal in each byte order, uninitialized journal, transactions present with `start < end` and with `start > end`, `start == end` at a position other than `jhdr_size`, `start == size` with `end == jhdr_size` read as clean, `start` or `end` of 0, above `size` or not a multiple of 512, `size` below 256 KiB and above 1 GiB, external journal, flags with neither `0x1` nor `0x2`, a bad checksum, magic or size; journal files absent on a journaled volume and present on an unjournaled one; a `.journal` fork of two extents, of a length other than `size`, and at a start other than the info block's `offset`; an external journal with and without a `.journal` file; flags `0x3` and `0x6`, which the external row decides |
| Wrapper | A wrapped volume with its embedded extent at the start, the middle and the end; an extent outside the outer slice; an embedded `HX`; a wrapped journal in each state: uninitialized, which reads (`h-hfsplus-16777216-e11-mkfs-w-J`), and initialized (`h-hfsplus-16777216-e11-mkfs-w-J-mounted`), clean, pending and with `offset` in either candidate base, each `FSIMG_EUNSUPPORTED`; an uninitialized wrapped journal whose `offset` does not match the fork |
| Writer | Both variants; every block size of the writer profile; exact and automatic sizing at each bitmap-block boundary; an empty catalog; a full volume; names at each length after decomposition, each of the four reserved root names with the private data folder's spelled through four U+2400, colliding siblings on each variant, U+2400 stored as U+0000, and the refused names: percent-text values, an all-ignorable name on `hfsplus`, each of the 23 `fsck_hfs` sequences, alone and after a starter, and a Greek letter with U+0301 accepted beside it; label default, 255 units and 256; owner and mode extremes; times at 1904, 1970, 2040 and beyond as counted losses and refusals; symbolic links and FIFOs; a catalog wide enough for map nodes; byte identity across source order, cache size, scratch path and time zone; a poison scan of every unused byte |
| Failure | Allocation, scratch, short read, short write, premature EOF, source change, cancellation, flush and publication failures at every stage, each followed by a cleanup check |

Independent acceptance is bidirectional: fsimg reads each native capture
and corpus specimen and gives the status a manifest of expected results
records for it, comparing names, sizes, modes, times and checksums with the
capture's manifest where it reads, and the refusal class where it does
not (`FSIMG_EUNSUPPORTED` for the interrupted journal and the directory
hard link, `FSIMG_ECORRUPT` at the alternate step for the specimen without
one); `fsck_hfs -n` reports no finding on fsimg output, macOS mounts it,
creates, appends until a file fragments, renames, makes and removes
directories, sets an attribute, makes a hard link and a compressed file,
unmounts, checks again and remounts, and fsimg reads and verifies the
result; `fsck.hfsplus -n`, `hfsdump` and the Sleuth Kit list and read every
fsimg file. The writer capability is registered only after that native
record exists.

## Source keys

Provenance details are in the research record.

| Key | Source |
| --- | --- |
| TN | Apple Technical Note TN1150, *HFS Plus Volume Format*, March 2004: the on-disk layout, B-trees, catalog, Unicode rules, hard links, journal and wrapper; facts paraphrased |
| HF | Apple's open-source `hfs_format.h` (APSL 2.0): later volume and catalog flags, the attribute key and records, private directory names; read for facts only |
| HL | Apple's open-source `hfs_catalog.c` and `hfs_link.c` (APSL 2.0): the tests that make a catalog record a file or directory link stub, and the dates links carry; read for facts only |
| NB | NetBSD `sys/fs/hfs/libhfs.c` (BSD-2-Clause): an independent folding table and comparators, a cross-check |
| DC | Apple's open-source xnu `decmpfs.h` (APSL 2.0): the compression attribute's header and type numbers, read for facts only; the chunk framings and raw markers are fixed by native captures (E6), with implementations such as hfsfuse used only as black-box oracles |
| UC | Unicode 3.2.0 `UnicodeData.txt`, under the Unicode license |
| R | The native experiments E1 to E11 the plan defines, and the Apple source cross-check that follows them, recorded in `docs/hfsplus-research.md` |

[TN]: https://developer.apple.com/library/archive/technotes/tn/tn1150.html
[HF]: https://github.com/apple-oss-distributions/hfs/blob/main/core/hfs_format.h
[NB]: https://github.com/NetBSD/src/blob/trunk/sys/fs/hfs/libhfs.c
[HL]: https://github.com/apple-oss-distributions/hfs/blob/main/core/hfs_link.c
[HJ]: https://github.com/apple-oss-distributions/hfs/blob/main/core/hfs_journal.c
[DC]: https://github.com/apple-oss-distributions/xnu/blob/main/bsd/sys/decmpfs.h
[UC]: https://www.unicode.org/Public/3.2-Update/UnicodeData-3.2.0.txt
[R]: ../docs/hfsplus-plan.md#1-native-evidence-corpus
