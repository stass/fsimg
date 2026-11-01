# UDF reader and writer

## Scope and evidence

`udf` denotes the Universal Disk Format: OSTA's profile of the ECMA-167
volume and file structures, revisions 1.02 through 2.60, as republished in
ECMA TR/112. It is not an extension of ISO 9660: a bridge image carries both
structure sets, and this driver reads only the UDF set. The reader consumes
one volume as it is on disk, bare or as a slice given with `--offset=` and
`--length=`; the writer constructs a new, closed volume. Neither modifies an
existing volume, and reader and writer remain independently registered
capabilities.

The reader covers four partition profiles, each a separate acceptance gate
in the [UDF plan](../docs/udf-plan.md): physical Type-1 partitions at every
revision, metadata partitions (2.50 and 2.60), sparable partitions (1.50 and
later) and virtual partitions with a VAT (1.50 and later) in closed flat
images. The writer emits one profile: UDF 2.01 with extended file entries in
one overwritable physical partition with a space bitmap. The
[driver design](design-udf-driver.md) maps the rules below onto the modules
of `lib/formats/udf/`; the [research record](../docs/udf-research.md) holds
the observations and the open questions. The research draft that supplied
the questions is not a source of rules.

This document is an original description of disk-format facts and fsimg
requirements. It contains no imported source, copied declarations or copied
standard prose. GPL and LGPL implementations (udftools, the Linux driver,
libudfread, pycdlib, genisoimage) are black-box oracles, never code donors.

Evidence has three meanings here:

* **Standard**: a rule of ECMA-167 [E1] or of the exact UDF revision in
  TR/112 [U1].
* **Observation**: a property measured on a named image in the research
  record ([M1], [G1], [P1], [Z1]).
* **fsimg policy**: a validation, capability or construction rule chosen
  here. It can be stricter than the standard without redefining the format.

Classification is uniform. A value the standard forbids, or that contradicts
another field, is `FSIMG_ECORRUPT`. A value the standard permits that the
supported profile does not read is `FSIMG_EUNSUPPORTED`, so a valid volume
using a feature fsimg lacks is never reported corrupt. A field no fsimg rule
consumes is not interpreted, so a writer's harmless variation, such as a tag
serial number of 0 or 1 or a different implementation identifier, never fails
a volume. Source keys in brackets are listed at the end.

## Shared owners and representation

The resource context owns memory, scratch, work accounting and cancellation.
Bounded I/O owns every volume-relative byte access. The common source catalog
owns input identities and repeatable reads; the output coordinator owns
publication. The common contracts cover UDF:
[format descriptors and probes](design-formats.md#format-descriptors-and-profiles),
[prepared directories](design-directories.md),
[logical content and the extent index](design-content.md),
[emission](design-emission.md), [sizing](design-sizing.md),
[verification helpers](design-verification-helpers.md) and
[POSIX metadata](design-metadata.md). UDF needs five common additions and
one rename, each recorded in the contract that owns it:

* **Zero extents.** A resolved extent can be a zero extent, read as zeros
  without touching storage, because UDF files carry unrecorded and
  unallocated extents anywhere in their body
  ([logical content](design-content.md#zero-extents)).
* **Decoded link targets.** A symbolic link's target is decoded from path
  components into a reader-owned buffer, because the stored bytes are not the
  target ([logical content](design-content.md#decoded-link-targets)).
* **Storage bytes in payload verification.** Payload read-through counts only
  bytes read from storage, so synthesized zeros are never reported as read
  ([verification helpers](design-verification-helpers.md#read-and-failure-algorithm)).
* **Link-target reads on writing.** A writer that encodes a symbolic link's
  target reads it through one common operation, because the stored bytes
  are not the target and drivers never open a source
  ([emission](design-emission.md#encoded-link-targets)).
* **A 255-byte label.** The volume information's label, the writer's label
  storage and the worker's records hold 255 bytes and a terminator, because
  a UDF logical volume identifier decodes to at most 252 bytes of UTF-8
  ([C API](design-api.md#reader)).
* **The portable name profile.** UDF declares the name profile the QNX
  readers declare, under the
  [omitted-names contract](design-directories.md#omitted-names), so the
  profile carries a format-neutral name.

Every numeric field is little-endian and decoded from bytes; native structure
layout and host alignment are not disk codecs [E1], [N1]. Offsets and
products use checked `uint64_t` arithmetic; a value that fails a check is
corrupt, never clamped. All project-defined C types have `_t` typedefs.

## Terms and address spaces

A volume is `N` logical blocks of `B` bytes. Volume block `v` starts at byte
`B * v` of the slice; byte 0 of the slice is the volume origin, and the
driver never adds a slice offset of its own. [E1] `N` is the slice length
divided by `B`, rounded down: a formatter on a device whose size is not a
multiple of the block size uses the whole blocks, so the remainder is not
part of the volume and is not interpreted (fsimg policy).

Three address spaces exist:

| Space | Unit | Used by |
| --- | --- | --- |
| Volume | volume block `v` | anchors, volume descriptor sequences, the integrity sequence, sparing tables, unallocated-space extents |
| Partition | logical block `p` of one partition map, reached through `(map index, p)` | the file set, every ICB, allocation extent, directory, space bitmap and file body |
| Byte | offset in the slice | bounded I/O only |

A **map index** is a zero-based position in the logical volume descriptor's
partition-map table. It is not the partition descriptor's partition number,
which a map names. Every partition-space address passes through its map's
translation to a volume block before any byte is read, and every
descriptor's tag location is checked in the space it was addressed in, never
against the translated block. [E1], [U1]

## Volume layout

### Fixed regions

| Bytes or blocks | Content |
| --- | --- |
| Bytes `[0, 32768)` | System area: not interpreted by the reader; zero from the writer |
| From byte 32768 | Volume recognition sequence |
| Block 256 | Anchor volume descriptor pointer |
| Blocks `N - 257` and `N - 1` | Further anchor positions |
| Anywhere else in volume space | The main and reserve volume descriptor sequences, the integrity sequence, the partitions, and for sparable volumes the sparing tables and spare area |

Only the anchor positions are fixed; every other structure is located from
an anchor. [E1], [U1]

### Volume recognition sequence

The sequence is a run of 2048-byte volume structure descriptors starting at
byte 32768, one per slot, where slot `k` begins at byte `32768 + k * s` and
`s = max(2048, B)`. [E1], [M1]

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 1 | Structure type |
| 1 | 5 | Standard identifier |
| 6 | 1 | Structure version |
| 7 | 2041 | Structure data |

| Identifier | Meaning | Accepted |
| --- | --- | --- |
| `CD001`, `CDW02` | ISO 9660 or ECMA-168 descriptor of a bridge | Any number before the extended area; not interpreted |
| `BEA01` | Beginning of the extended area | Type 0, version 1 |
| `NSR02` | ECMA-167 2nd edition structures: UDF 1.02 and 1.50 | Type 0, version 1, inside the extended area |
| `NSR03` | ECMA-167 3rd edition structures: UDF 2.00 and later | Same |
| `BOOT2` | Boot descriptor | Inside the extended area; not interpreted |
| `TEA01` | End of the extended area | Type 0, version 1 |

A UDF sequence is optional leading `CD001`/`CDW02` slots, `BEA01`, then
`NSR02`, `NSR03` or `BOOT2` slots, then `TEA01`, with exactly one `NSR` slot.
A slot with any other identifier, or an all-zero slot, ends the scan. A lone
`CD001` sequence is not UDF. [E1], [G1], [P1]

### Anchors

The anchor (tag 2) names the main and reserve volume descriptor sequences:
[E1], [N1]

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 16 | Descriptor tag, identifier 2, location = its volume block |
| 16 | 8 | Main sequence extent: length in bytes, volume block |
| 24 | 8 | Reserve sequence extent |
| 32 | 480 | Reserved |

A closed volume records anchors at two or more of the three positions [E1].
Every observed writer records one at 256 and one at `N - 1`, with or without
`N - 257` [M1], [G1], [P1] (Q3); genisoimage additionally fills every block
of its tail padding with anchors, each located at its own block [G1]. A
write-once session, as in M1's VAT images, records the one at 256 alone
[M1], and the virtual profile accepts that.

The reader opens through block 256, so it classifies that block first. A
block 256 whose first 16 bytes form a tag with identifier 2 and a valid
checksum is an anchor: any other fault in it is `FSIMG_ECORRUPT`. A block
256 that holds no such tag leaves two cases: an anchor at `N - 257` or
`N - 1` marks a tail-anchored volume, outside the profile
(`FSIMG_EUNSUPPORTED`), and no anchor anywhere is `FSIMG_ECORRUPT`.

An anchor position can fall inside a partition: mkudffs runs its partition
across `N - 257` at 4096 bytes per block and writes no anchor there [M1].
Such a position holds partition content unless its block is a valid anchor,
which is then a volume structure the partition holds, owned like any other.

Each sequence extent is at least 16 blocks long and a whole number of
blocks, lies within the volume, and is disjoint from the other sequence, the
anchors and every partition [U1].

## Descriptor tags

Every descriptor begins with a 16-byte tag: [E1], [N1]

| Offset | Bytes | Field | Rule |
| --- | --- | --- | --- |
| 0 | 2 | Tag identifier | Known in the context that addresses it |
| 2 | 2 | Descriptor version | 2 or 3 on any volume; observed writers use 2 with `NSR02` and 3 with `NSR03` |
| 4 | 1 | Tag checksum | Sum modulo 256 of bytes 0 to 15 except byte 4 |
| 5 | 1 | Reserved | Zero |
| 6 | 2 | Tag serial number | Not interpreted |
| 8 | 2 | Descriptor CRC | CRC of the CRC-covered bytes |
| 10 | 2 | Descriptor CRC length | Bytes after the tag covered by the CRC |
| 12 | 4 | Tag location | The address the descriptor was read at, in its own space |

The CRC is CRC-16 with generator `0x1021`, initial remainder 0, no
reflection and no final exclusive or, over the CRC-length bytes that follow
the tag. Every descriptor in the observed images validates under exactly
this definition [M1], [G1], [P1]. The CRC length is at most the descriptor's
decoded size minus 16 and may be shorter, as mkudffs's space bitmap's 8
covers only its counts [M1]. A descriptor's **fixed part** is the run of
fields after the tag that this design's tables name, up to its first
variable-length or trailing reserved area: 16 bytes for an anchor, 0 for a
terminating descriptor, 8 for a space bitmap, 22 for a FID. A CRC length
shorter than the fixed part leaves fields fsimg consumes unprotected; it is
`FSIMG_EUNSUPPORTED` (fsimg policy; research question Q13 covers every
descriptor kind). A descriptor's size always comes from its identifier and
its validated length fields, never from the CRC length. A wrong checksum,
CRC, reserved byte, version or location is `FSIMG_ECORRUPT`.

An all-zero block has a zero checksum and a zero CRC; it is never a
descriptor, whatever identifier 0 means elsewhere [M1].

| Identifier | Descriptor | Space |
| --- | --- | --- |
| 0 | Sparing table | Volume |
| 1 | Primary volume descriptor (PVD) | Volume |
| 2 | Anchor volume descriptor pointer | Volume |
| 3 | Volume descriptor pointer | Volume |
| 4 | Implementation use volume descriptor (IUVD) | Volume |
| 5 | Partition descriptor (PD) | Volume |
| 6 | Logical volume descriptor (LVD) | Volume |
| 7 | Unallocated space descriptor (USD) | Volume |
| 8 | Terminating descriptor (TD) | Volume or partition |
| 9 | Logical volume integrity descriptor (LVID) | Volume |
| 256 | File set descriptor (FSD) | Partition |
| 257 | File identifier descriptor (FID) | Partition |
| 258 | Allocation extent descriptor (AED) | Partition |
| 259, 260 | Indirect and terminal entries | Partition; unsupported |
| 261 | File entry (FE) | Partition |
| 262 | Extended attribute header descriptor | Inside an FE or EFE |
| 263 | Unallocated space entry (USE) | Partition |
| 264 | Space bitmap descriptor (SBD) | Partition |
| 265 | Partition integrity entry | Forbidden by UDF |
| 266 | Extended file entry (EFE) | Partition |

## Shared structures

| Structure | Bytes | Layout |
| --- | --- | --- |
| `extent_ad` | 8 | length in bytes (u32), volume block (u32) |
| `lb_addr` | 6 | logical block (u32), map index (u16) |
| `short_ad` | 8 | length and type (u32), logical block in the containing ICB's map (u32) |
| `long_ad` | 16 | length and type (u32), `lb_addr`, implementation use (6): flags (u16), UDF unique ID (u32) |
| `ext_ad` | 20 | Forbidden by UDF for file allocation; `FSIMG_ECORRUPT` |
| `charspec` | 64 | type (u8), information (63) |
| `regid` | 32 | flags (u8), identifier (23), suffix (8) |
| `timestamp` | 12 | see [Timestamps](#timestamps) |

The top two bits of a `short_ad` or `long_ad` length word are its extent
type and the low 30 bits its byte length: [E1], [N1]

| Type | Meaning | Content | Storage |
| --- | --- | --- | --- |
| 0 | Recorded and allocated | the extent's bytes | owned |
| 1 | Allocated, not recorded | zeros | owned |
| 2 | Neither allocated nor recorded | zeros | none |
| 3 | Continuation | the next allocation extent descriptor | the AED block, owned |

The only character set is CS0, recorded as type 0 with the information
`OSTA Compressed Unicode` padded with zeros; any other is `FSIMG_ECORRUPT`
where UDF requires CS0 [U1].

Entity identifiers are compared on their 23 identifier bytes. Their suffix
meaning depends on the identifier's role: [U1], [M1]

| Role | Suffix |
| --- | --- |
| Domain (`*OSTA UDF Compliant`) | UDF revision (u16), domain flags (u8: bit 0 hard, bit 1 soft write protect), 5 reserved |
| UDF identifiers (`*UDF LV Info`, partition-map identifiers, `*UDF Sparing Table`) | UDF revision (u16), OS class (u8), OS identifier (u8), 4 reserved |
| Implementation identifiers | OS class (u8), OS identifier (u8), 6 implementation use |

Flags of entity identifiers, OS fields and implementation identifiers are
not interpreted: observed writers set flags such as `0x02` on the partition
contents identifier [G1], [P1]. Domain write-protect flags are reported, not
enforced, since the reader never writes.

## Volume descriptor sequences

### Traversal

A sequence is read block by block from its extent's first block. Each block
is a descriptor of identifier 1, 3, 4, 5, 6, 7 or 8 located at its own volume
block; an all-zero block, a terminating descriptor or the extent's end ends
the sequence. A volume descriptor pointer (3) continues the sequence at its
next extent, which counts toward one bound: at most 64 extents and at most
`N` blocks in total per sequence; exceeding either, or reaching an extent
twice, is `FSIMG_ECORRUPT`. Any other identifier is `FSIMG_ECORRUPT`. [E1]

Each volume descriptor carries a volume descriptor sequence number at offset
16. Of several descriptors of one kind, the PVD, the LVD, the USD, the IUVD
with `*UDF LV Info` and the PD of each partition number with the highest
sequence number prevails; two with the same number that differ in any field
the reader consumes are `FSIMG_ECORRUPT`. Observed numbers are 1 [M1] and 0
[G1], [P1]; order within the sequence varies and is not interpreted. [E1]

### Primary volume descriptor

| Offset | Bytes | Field | Rule |
| --- | --- | --- | --- |
| 16 | 4 | Sequence number | as above |
| 24 | 32 | Volume identifier, dstring | valid dstring; reported in no field |
| 56 | 2 | Volume sequence number | 1 |
| 58 | 2 | Maximum volume sequence number | 1; a multi-volume set is `FSIMG_EUNSUPPORTED` |
| 72 | 128 | Volume set identifier, dstring | its first 16 characters, when hexadecimal digits, are reported as a 64-bit detail |
| 200 | 64 | Descriptor character set | CS0 |
| 376 | 12 | Recording time | not interpreted |

Interchange levels, character-set lists, application and implementation
identifiers and the remaining fields are not interpreted. [E1], [N1], [M1]

### Implementation use volume descriptor

Of the IUVDs, the one whose implementation identifier is `*UDF LV Info`
carries the logical volume information at offset 52: the LVI character set
(64), the logical volume identifier (128), three 36-byte information
dstrings, an implementation identifier (32) and 128 bytes of implementation
use. The reader validates its dstrings; other IUVDs are not interpreted.
[U1], [N1]

### Partition descriptor

| Offset | Bytes | Field | Rule |
| --- | --- | --- | --- |
| 20 | 2 | Partition flags | bit 0 set |
| 22 | 2 | Partition number | the key a map names |
| 24 | 32 | Partition contents | `+NSR02` or `+NSR03`, agreeing with the recognition sequence |
| 56 | 128 | Partition header | [Space management](#space-management) |
| 184 | 4 | Access type | per profile below |
| 188 | 4 | Starting volume block | the partition begins here |
| 192 | 4 | Length in blocks | nonzero |

| Access type | Meaning | Profile |
| --- | --- | --- |
| 0 | Pseudo-overwritable | metadata profile, UDF 2.60 only |
| 1 | Read-only | physical and metadata |
| 2 | Write-once | virtual |
| 3 | Rewritable | sparable |
| 4 | Overwritable | physical and metadata |

Any other value is `FSIMG_ECORRUPT`; a value outside the opened profile is
`FSIMG_EUNSUPPORTED`. The partition `[start, start + length)` lies within
the volume and is disjoint from the anchor at 256, both sequences, the
integrity sequence and, on sparable volumes, the sparing tables and spare
packets; a tail anchor it holds is owned as the [anchors](#anchors) section
states. A
write-once partition is the one exception to containment: its length
describes the medium's capacity and runs past the recorded session [M1], so
it may extend beyond the volume, while every block a translation produces
still lies inside it. [E1], [U1], [M1]

### Logical volume descriptor

| Offset | Bytes | Field | Rule |
| --- | --- | --- | --- |
| 20 | 64 | Descriptor character set | CS0 |
| 84 | 128 | Logical volume identifier, dstring | the label |
| 212 | 4 | Logical block size | equals `B` |
| 216 | 32 | Domain identifier | `*OSTA UDF Compliant`; suffix revision `0x0102`, `0x0150`, `0x0200`, `0x0201`, `0x0250` or `0x0260` |
| 248 | 16 | Logical volume contents use | `long_ad` of the file set descriptor sequence |
| 264 | 4 | Map table length | bytes of maps |
| 268 | 4 | Number of maps | at least 1 |
| 432 | 8 | Integrity sequence extent | nonzero length, within the volume |
| 440 | table length | Partition maps | exactly tile the table |

The revision must agree with the recognition sequence: `NSR02` with 1.02 and
1.50, `NSR03` with 2.00 and later, else `FSIMG_ECORRUPT`. Another revision
is `FSIMG_EUNSUPPORTED`. The descriptor's size is `440 + table length`, at
most `B`. [E1], [U1], [M1], [G1]

### Partition maps

| Map | Bytes | Layout |
| --- | --- | --- |
| Type 1 | 6 | type 1, length 6, volume sequence number (u16, 1), partition number (u16) |
| Type 2 | 64 | type 2, length 64, 2 reserved, partition-type identifier (32), then by identifier: |
| `*UDF Virtual Partition` | | volume sequence (u16 at 36), partition number (u16 at 38), 24 reserved |
| `*UDF Sparable Partition` | | volume sequence (36), partition number (38), packet length in blocks (u16 at 40), number of sparing tables (u8 at 42, 1 to 4), size of each table in bytes (u32 at 44), table volume blocks (u32 each from 48) |
| `*UDF Metadata Partition` | | volume sequence (36), partition number (38), metadata file block (u32 at 40), mirror file block (u32 at 44), bitmap file block (u32 at 48, `0xffffffff` when absent), allocation unit in blocks (u32 at 52), alignment unit in blocks (u16 at 56), flags (u8 at 58, bit 0 duplicated), 5 reserved |

A map whose type is neither 1 nor 2, whose length disagrees with its type,
or that overruns the table is `FSIMG_ECORRUPT`; maps are parsed with a
cursor that must advance by each map's length and end exactly at the table
length. A Type-2 identifier outside the three is `FSIMG_EUNSUPPORTED`. Every
map names a partition number some prevailing PD has, else
`FSIMG_ECORRUPT`. [E1], [U1], [N1], [M1]

The accepted map sets are closed:

| Profile | Maps | Revisions |
| --- | --- | --- |
| Physical | one Type-1 | all |
| Metadata | one Type-1 and one metadata map naming it | 2.50, 2.60 |
| Sparable | one sparable map | 1.50 and later |
| Virtual | one Type-1 and one virtual map naming it | 1.50 and later |

Any other combination, including two physical partitions, is
`FSIMG_EUNSUPPORTED`. [U1]

### Unallocated space descriptor and terminating descriptor

The USD records, at offset 20, a count of `extent_ad` entries that follow at
offset 24, at most `(B - 24) / 8`: unallocated volume space outside every
partition. mkudffs lists the gaps between its structures; genisoimage and
pycdlib list none [M1], [G1], [P1]. Its extents lie in the volume and outside
every structure the reader knows. A terminating descriptor's 496 bytes after
the tag are not interpreted. [E1]

## Logical volume integrity

The integrity sequence extent holds LVIDs read block by block; a terminating
descriptor, an all-zero block or the extent's end ends it, and a nonzero next
integrity extent continues it under the same bound as a volume descriptor
sequence. The last LVID read prevails: [E1], [N1], [M1]

| Offset | Bytes | Field | Rule |
| --- | --- | --- | --- |
| 16 | 12 | Recording time | not interpreted |
| 28 | 4 | Integrity type | 0 open, 1 closed; other values `FSIMG_ECORRUPT` |
| 32 | 8 | Next integrity extent | as above |
| 40 | 8 | Next unique ID | greater than every unique ID the walk meets, under full verification |
| 72 | 4 | Number of partitions | the number of maps |
| 76 | 4 | Implementation use length | at least 46 |
| 80 | `4n` | Free space table | per map, `0xffffffff` unknown |
| `80 + 4n` | `4n` | Size table | per map, `0xffffffff` unknown |
| `80 + 8n` | 46 | Implementation identifier (32), number of files (u32), number of directories (u32), minimum read, minimum write and maximum write revisions (u16 each) | the minimum read revision is at most `0x0260`, else `FSIMG_EUNSUPPORTED` |

`info` reports the integrity type as the volume's clean state. An open
integrity does not block reading; full verification then skips the LVID's
tables and counts, which an open volume does not keep current, and checks
everything else. A virtual volume records an open LVID and keeps its counts
in the VAT [M1].

## Partitions

### Physical (Type 1)

A Type-1 map translates logical block `p < length` to volume block
`start + p`. A run of blocks translates as one run. [E1]

### Metadata partition (2.50, 2.60)

The metadata partition is a logical address space backed by a file. The
metadata file's FE or EFE (file type 250) lies in the named physical
partition at the map's metadata file block; its body, described by short
allocation descriptors in that physical partition, is the metadata
partition's content, so metadata block `m` is the block at byte `m * B` of
the file's body. Its length in blocks is the file's information length over
`B`, which must be a whole number. The mirror file (251) holds the same content.
The relation between their extents, compared as coalesced block runs, takes
one of two forms: with the duplicated flag set they are disjoint and both
are owned; with the flag clear they are equal and the storage is owned once.
A partial overlap, or equal extents under the flag, is `FSIMG_ECORRUPT`. Disjoint
extents without the flag may be permitted, but nothing observed writes them,
so they are `FSIMG_EUNSUPPORTED` (fsimg policy; research question Q12). The
two accepted forms need no exception anywhere else. The bitmap file (252),
when present, is the metadata partition's space bitmap. [U1]

The file set descriptor, every directory, ICB and allocation extent, and the
content of directories and symbolic links lie in the metadata partition, and
regular file bodies in the physical partition through `long_ad`s naming it.
A `short_ad` resolves in its containing ICB's map, and a continuation in the
map of the descriptor that holds it. [U1]

The metadata file, mirror and bitmap are decoded as ordinary files of the
physical partition before the metadata map exists, so the map's construction
never consults itself: the dependency graph is acyclic by construction. An
extent of type 1 or 2 in the metadata file is a hole in the address space; a
descriptor addressed into a hole is `FSIMG_ECORRUPT`. Reading uses the
metadata file; full verification compares a duplicated mirror's content
with it block for block, while an unflagged mirror shares its blocks and
needs no comparison. [U1]

### Sparable partition (1.50 and later)

A sparable map replaces the Type-1 map of its partition. Its sparing tables
(tag 0, `*UDF Sparing Table`) list relocations: [U1], [N1], [M1]

| Offset | Bytes | Field |
| --- | --- | --- |
| 16 | 32 | Sparing identifier |
| 48 | 2 | Relocation table length: the entry count |
| 50 | 2 | Reserved |
| 52 | 4 | Sequence number |
| 56 | `8n` | Entries: original packet's first logical block (u32), mapped volume block (u32) |

The rules, in the order a reader applies them:

1. **Meaning.** An entry relocates the packet whose first logical block is
   its original to the packet starting at its mapped volume block. An
   original of `0xffffffff` marks an available spare and `0xfffffff0` a
   defective one; such entries relocate nothing. M1's tables hold 32
   available entries naming spare packets 288 onward [M1].
2. **Validity.** Entries are sorted by original, the two markers last.
   Originals are packet-aligned, unique and inside the partition. Mapped
   blocks are packet-aligned, unique and inside the volume. NetBSD's header
   allows a spare packet inside a partition [N1]; such a spare is
   `FSIMG_EUNSUPPORTED` (research question Q1).
3. **Translation.** Logical block `p` of packet length `P` translates through
   the entry whose original is `p - p % P` to `mapped + p % P`, else to
   `start + p`. The mapped block is a volume block, so the partition start is
   never added to it. A run is clipped at every packet boundary.
4. **Copies.** Reading uses the first table whose tag validates; full
   verification requires every copy to validate and to equal it.

### Virtual partition and VAT (1.50 and later)

A virtual map names the physical partition that holds the VAT. In a flat
image of a write-once session the VAT's ICB is the volume's last block,
`N - 1`: mkudffs truncates write-once images to the recorded session, so
their last block is the VAT ICB and only the anchor at 256 exists [M1]. A
last block that holds an FE or EFE tag with a valid checksum is the VAT ICB,
and any other fault in it is `FSIMG_ECORRUPT`; a last block without such a
tag is `FSIMG_EUNSUPPORTED`, since other writers' images may end otherwise
(Q17). The ICB is read through
the physical map (its tag location is `N - 1 - start`) and has file type 248
in revisions 2.00 and later; in 1.50 its file type is 0 and its body ends
with a 36-byte trailer holding `*UDF Virtual Alloc Tbl` and the previous VAT
location. The 2.00 header is: [U1], [N1], [M1]

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 2 | Header length, normally 152 |
| 2 | 2 | Implementation use length |
| 4 | 128 | Logical volume identifier, dstring; the label when present |
| 132 | 4 | Previous VAT ICB, `0xffffffff` none |
| 136 | 4 | Number of files |
| 140 | 4 | Number of directories |
| 144 | 6 | Minimum read, minimum write, maximum write revisions |
| 150 | 2 | Reserved |

In 2.00 and later the header length is at least 152, the header plus the
implementation use lies within the VAT's body, and u32 entries fill the
rest, whose length is a multiple of 4. In 1.50 there is no header: the
entries fill the body before the 36-byte trailer, and the body's length
minus 36 is a multiple of 4. In both, entry `i` maps virtual block `i` to a
block of the physical partition, `0xffffffff` unmapped. The virtual
partition's length is the entry count. Each virtual block translates
separately, as a run of one block. The previous-VAT pointer is range
checked and not followed: fsimg reads the latest state only. The two codecs
are distinct and chosen by revision, never by guessing from a length.

## File set

The logical volume contents use names the file set descriptor sequence,
read block by block in its map until a terminating descriptor, an all-zero
block or the extent's end; mkudffs records one FSD in a one-block extent,
genisoimage and pycdlib an FSD and a TD in two blocks [M1], [G1], [P1]. The
FSD with the highest file set descriptor number prevails: [E1], [N1]

| Offset | Bytes | Field | Rule |
| --- | --- | --- | --- |
| 40 | 4 | File set number | 0 for every FSD; another file set is `FSIMG_EUNSUPPORTED` |
| 44 | 4 | File set descriptor number | prevailing rule |
| 48 | 64 | Logical volume identifier character set | CS0 |
| 240 | 64 | File set character set | CS0 |
| 304 | 32 | File set identifier, dstring | valid dstring |
| 400 | 16 | Root directory ICB, `long_ad` | resolves to a directory |
| 416 | 32 | Domain identifier | as the LVD's, same revision |
| 448 | 16 | Next extent | zero; a nonzero one is `FSIMG_EUNSUPPORTED` |
| 464 | 16 | System stream directory ICB | zero, or a stream directory walked by verification |

## ICBs and file entries

### ICB tag

Every FE, EFE and USE carries a 20-byte ICB tag at offset 16: [E1], [N1]

| Offset | Bytes | Field | Rule |
| --- | --- | --- | --- |
| 0 | 4 | Prior recorded direct entries | not interpreted |
| 4 | 2 | Strategy type | 4; 4096 is `FSIMG_EUNSUPPORTED` |
| 6 | 2 | Strategy parameter | not interpreted |
| 8 | 2 | Maximum entries | not interpreted |
| 11 | 1 | File type | table below |
| 12 | 6 | Parent ICB | not interpreted |
| 18 | 2 | Flags | below |

| Flag bits | Meaning | Use |
| --- | --- | --- |
| 0 to 2 | Allocation form: 0 `short_ad`, 1 `long_ad`, 2 `ext_ad`, 3 inline | 2 is `FSIMG_ECORRUPT`; 4 to 7 `FSIMG_ECORRUPT` |
| 5 | Archive | the archive attribute |
| 6, 7, 8 | Set-user-ID, set-group-ID, sticky | the mode's high bits |
| 10 | System | the system attribute |
| 11 | Transformed | set: `FSIMG_EUNSUPPORTED` |
| 12 | Multiple versions | set: `FSIMG_EUNSUPPORTED` |
| 13 | Stream | set only on a named stream's entry |
| others | sorted, non-relocatable, contiguous | not interpreted |

| File type | Object | fsimg |
| --- | --- | --- |
| 4 | Directory | `FSIMG_ENTRY_DIRECTORY` |
| 5 | Regular file | `FSIMG_ENTRY_FILE` |
| 9 | FIFO | `FSIMG_ENTRY_FIFO` |
| 12 | Symbolic link | `FSIMG_ENTRY_SYMLINK` |
| 13 | Stream directory | never an entry; walked by verification |
| 6, 7, 10 | Block device, character device, socket | `FSIMG_EUNSUPPORTED` naming the entry |
| 249 | Real-time file | `FSIMG_EUNSUPPORTED` naming the entry |
| 248, 250, 251, 252 | VAT, metadata, mirror, bitmap file | internal; named from a directory, `FSIMG_ECORRUPT` |
| 0 | Unknown | internal for a 1.50 VAT; named from a directory, `FSIMG_EUNSUPPORTED` |
| others | | `FSIMG_ECORRUPT` |

### File entry and extended file entry

| Field | FE offset | EFE offset | Rule |
| --- | --- | --- | --- |
| Owner and group | 36, 40 | 36, 40 | u32 each; `0xffffffff` unspecified |
| Permissions | 44 | 44 | [Owner, mode and attributes](#owner-mode-and-attributes) |
| File link count | 48 | 48 | verification |
| Record format, display attributes, record length | 50, 51, 52 | 50, 51, 52 | all zero, else `FSIMG_EUNSUPPORTED` |
| Information length | 56 | 56 | the logical length (u64) |
| Object size | | 64 | at least the information length; equal without a stream directory |
| Logical blocks recorded | 64 | 72 | verification |
| Access time | 72 | 80 | |
| Modification time | 84 | 92 | |
| Creation time | | 104 | |
| Attribute time | 96 | 116 | |
| Extended attribute ICB | 112 | 136 | zero, else `FSIMG_EUNSUPPORTED` |
| Stream directory ICB | | 152 | zero or a stream directory |
| Unique ID | 160 | 200 | u64 |
| Extended attribute length `L_EA` | 168 | 208 | multiple of 4 |
| Allocation descriptor length `L_AD` | 172 | 212 | |
| Extended attributes, then descriptors or inline data | 176 | 216 | |

The fixed part, `L_EA` and `L_AD` must fit one block: `176 + L_EA + L_AD`
or `216 + L_EA + L_AD` at most `B`, by checked arithmetic. Inline data is
the `L_AD` bytes, which must equal the information length, with logical
blocks recorded 0. `L_AD` is a multiple of 8 for `short_ad` and of 16 for
`long_ad`. An FE and an EFE may appear in one `NSR03` volume, since UDF 2.00
and later permit both; an EFE on an `NSR02` volume is `FSIMG_ECORRUPT`,
since the second edition of ECMA-167 defines none. [E1], [U1], [N1], [M1]

### Extended attributes

A nonzero `L_EA` begins with an extended attribute header descriptor (tag
262, 24 bytes, located at the ICB's block) holding the implementation and
application attribute offsets. Attributes follow, each a type (u32), a
subtype (u8, 1), 3 reserved bytes and an attribute length (u32, at least 12,
a multiple of 4), tiling the area exactly. File times (type 5) supplies the
creation time of an FE: after the 12-byte attribute header, a data length
(u32), an existence mask (u32) whose bit 0 marks a creation time, then the
12-byte times in mask order, which the data length covers. Implementation-use (2048) and application-use
(65536) attributes are skipped after their framing is checked, as UDF
defines them skippable; any other type is skipped after its framing is
checked, since none changes how fsimg reads the entry. A framing failure is
`FSIMG_ECORRUPT`. [E1], [U1], [N1]

## Allocation descriptors and content

### Descriptor lists

An object's storage is its descriptor list: the `L_AD` bytes of its ICB,
continued through allocation extent descriptors. Descriptors are read in
order; a descriptor of length zero ends the list, and the bytes after it in
the area must be zero. The lengths of descriptors of types 0 to 2 sum to the
information length; every one but the last is a whole number of blocks, and
the last may end inside its last block. A `short_ad`'s block is in its
containing descriptor's map and a `long_ad`'s in the map it names. [E1], [U1]

A continuation (type 3) names an AED, whose length is at most `B` and covers
the AED's header and descriptors: [E1], [N1]

| Offset | Bytes | Field |
| --- | --- | --- |
| 16 | 4 | Previous allocation extent location, not interpreted |
| 20 | 4 | Descriptor length `L_AD`, with `24 + L_AD` at most the continuation's length |
| 24 | `L_AD` | Descriptors |

The AED is located at its own block in its map. A continuation ends the area
that holds it: descriptors after it are `FSIMG_ECORRUPT`. An AED that holds
no descriptor of nonzero length other than its continuation is
`FSIMG_EUNSUPPORTED` (fsimg policy): it may be legal, but no writer needs
one. So every AED read but the last advances the body by at least one block,
and a chain has at most `ceil(information length / B) + 1` AEDs whatever its
blocks. Every AED block is also owned by the object, so a chain
that returns to a block it already used fails the ownership claim on the
object's first walk.

### Content model

An object's logical bytes `[0, information length)` are the concatenation of
its extents of types 0 to 2:

| Type | Delivered as |
| --- | --- |
| 0 | the extent's bytes, through its map's translation |
| 1, 2 | zeros, never read from storage |
| inline | the `L_AD` bytes after the extended attributes |

Allocation slack after the last extent's byte length is never content. The
16 KiB example below, at `B = 2048`, reads blocks `p` and `p + 1`, two zero
extents and blocks `q` to `q + 3`, and no read touches the zero extents'
storage, though block `r` stays owned:

| File offset | Length | Type |
| --- | --- | --- |
| 0 | 4096 | 0 at block `p` |
| 4096 | 2048 | 2 |
| 6144 | 2048 | 1 at block `r` |
| 8192 | 8192 | 0 at block `q` |

fsimg's [zero extents](design-content.md#zero-extents) carry types 1 and 2.
A range query still reports the whole body as data, so a caller cannot infer
holes from it; content reads return the zeros.

## Directories

### File identifier descriptors

A directory's body is a stream of FIDs: [E1], [N1]

| Offset | Bytes | Field | Rule |
| --- | --- | --- | --- |
| 16 | 2 | File version number | 1, else `FSIMG_EUNSUPPORTED` |
| 18 | 1 | Characteristics | bit 0 hidden, 1 directory, 2 deleted, 3 parent, 4 stream metadata |
| 19 | 1 | Identifier length `L_FI` | |
| 20 | 16 | ICB, `long_ad` | on a live FID, type 0 and length at least `B`; not interpreted on a deleted one |
| 36 | 2 | Implementation use length `L_IU` | |
| 38 | `L_IU` | Implementation use | when nonzero, at least 32 bytes beginning with an entity identifier |
| `38 + L_IU` | `L_FI` | File identifier, CS0 | |
| | 0 to 3 | Padding to a multiple of 4 | zero |

A record is `align4(38 + L_IU + L_FI)` bytes by checked arithmetic and lies
inside the information length; it may cross a block or extent boundary of
the body. Its tag location is the block, in the directory's map, holding its
first byte: the ICB's own block for an inline directory [M1], [G1].

* The first live FID is the parent FID: parent and directory bits set, `L_FI`
  0, naming the parent directory, and the root names itself [M1], [G1],
  [P1]. No later FID has the parent bit.
* A deleted FID's framing and tag are validated; its name, which UDF 2.00
  and later may mark with compression ID 254 or 255, is never decoded, and
  it is not an entry.
* Every other live FID has `L_FI` of at least 2 and a name. Its directory bit
  agrees with its target's file type, and the stream metadata bit is set only
  inside a stream directory; a disagreement is `FSIMG_ECORRUPT`.
* The hidden bit is the entry's hidden attribute.
* The `long_ad` implementation use carries the target's unique ID in its low
  32 bits in every observed image [G1], [P1]. For a target with link count
  1, full verification requires it to equal the target's on `NSR03` volumes
  and to equal it or be zero on `NSR02` volumes. A hard-linked target's FIDs
  may carry IDs of their own, which no observation covers, so they are not
  compared (Q10).

### Identity and links

An object's identity is its ICB address `(map index, block)`. A directory is
reached through exactly one live non-parent FID; a second name, or a cycle,
is `FSIMG_ECORRUPT`. Any other object may be reached through several FIDs,
each an entry of its own, and its storage is owned once. A directory's link
count is `1 + subdirectories` [G1]; every other object's is the number of
live FIDs naming it.

## Names and identifiers

### OSTA compressed Unicode

A CS0 string is a compression ID followed by code units: [U1]

| ID | Units |
| --- | --- |
| 8 | one byte each, the code point U+0000 to U+00FF |
| 16 | two bytes each, big-endian UTF-16 |
| 254, 255 | deleted FID names only |

A compression ID outside 8 and 16 in a live name, or an odd byte count under
16, is `FSIMG_ECORRUPT`. A paired surrogate decodes to its supplementary
code point. A file identifier is at most 255 bytes including its compression
ID, `L_FI` being one byte, so a name holds at most 254 eight-bit characters
or 127 UTF-16 units. Every limit in this design counts the encoded bytes
after the compression ID, never UTF-8 output: 254 for a name, 126 for a
128-byte dstring.

A decoded name has one byte form, its WTF-8: each Unicode scalar value,
paired surrogates combined into one, written as UTF-8, and each unpaired
surrogate written in the three-byte pattern UTF-8 would give it (`ED A0 80`
to `ED BF BF`), which strict UTF-8 rejects. The form is injective, so it
serves as the component and as the namespace key.

### Name profile

UDF names are Unicode, and the standard defines translation rules for names
a host cannot store, so names a POSIX host cannot represent are natively
legal. The reader declares the portable name profile the QNX readers
declare, under the [omitted-names contract](design-directories.md#omitted-names):

| Decoded name | Treatment |
| --- | --- |
| Unicode scalar values only, with no character from U+0001 to U+001F, no U+007F, slash or NUL, and not `.` or `..` | delivered, as its UTF-8 |
| A character from U+0001 to U+001F, U+007F or an unpaired surrogate | omitted and counted; its component is its WTF-8, which the profile's byte rules reject, and the omitted-name rendering escapes |
| NUL, slash, `.` or `..` | `FSIMG_EUNSUPPORTED` for the directory, naming the directory and the FID's position, since the common walker cannot admit such a component |

Names compare by exact code points: case-sensitive and without Unicode
normalization, as UDF compares them. The key is the WTF-8, so two FIDs whose
different encodings decode to the same name collide and fail namespace
validation with `FSIMG_ECORRUPT`. NUL, slash and the dot names are refused
rather than omitted because the common walker admits no component that
holds them; the
[omitted-names contract](design-directories.md#omitted-names) records the
exception. G1 stores UTF-8 bytes as eight-bit
characters, so its `café.txt` decodes as `cafÃ©.txt`, which is what the
volume says and what 7-Zip lists [G1], [Z1].

### Dstrings

A dstring is a fixed field whose last byte is the length of the used bytes,
the compression ID included; bytes between the used part and the last byte
are zero, and an empty dstring is all zero. A length beyond the field, a
nonzero unused byte or an invalid CS0 body is `FSIMG_ECORRUPT`. [E1], [U1]

### Label

The label is the LVD's logical volume identifier, or the VAT's when a VAT
supplies one. A label whose decoded text holds an unpaired surrogate or NUL
cannot be represented: `info` omits it and verification is unaffected. A
label whose dstring is invalid is omitted by `info` and makes full
verification fail with `FSIMG_ECORRUPT`. A 128-byte dstring holds 126 eight-
bit characters, at most 252 bytes of UTF-8, which the 255-byte label field
holds. The PVD's volume identifier and the FSD's file set identifier are
validated, not reported.

## Symbolic links

A symbolic link's body is a sequence of path components: [E1], [N1], [P1]

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 1 | Component type |
| 1 | 1 | Identifier length `L_CI` |
| 2 | 2 | Component file version, 0 |
| 4 | `L_CI` | Identifier, CS0 |

| Type | Decoded | Rule |
| --- | --- | --- |
| 1, root | a leading `/` | first component only, `L_CI` 0; a nonzero `L_CI` is `FSIMG_EUNSUPPORTED` |
| 2, mount root | a leading `/` | first component only, `L_CI` 0 |
| 3, parent | `..` | `L_CI` 0 |
| 4, current | `.` | `L_CI` 0 |
| 5, name | the decoded identifier | `L_CI` at least 2; a slash, NUL, `.` or `..` in it is `FSIMG_EUNSUPPORTED` |
| 0, 6 to 255 | | `FSIMG_ECORRUPT` |

The target is the root's `/`, if any, followed by the other components
joined with `/`, so `/a` and `a/b` decode exactly and never as `//a`; it is
the UTF-8 of the result, never resolved or followed. Type 2 means the volume's root rather than the
host's; reading it as a leading `/` follows the observed practice that
pycdlib writes [P1] and that the Linux reader shares, and is the reading
fsimg's byte-string target model can express (question Q7). A nonzero
component version is `FSIMG_EUNSUPPORTED`. A component whose identifier has
an unpaired surrogate is `FSIMG_EUNSUPPORTED`, since a target is delivered as
UTF-8. Decoding stops with `FSIMG_EUNSUPPORTED` as soon as the target
exceeds 1024 bytes, so a component stream bounds its own cost: every
component adds at least one byte. An empty stream is `FSIMG_ECORRUPT`.

## Metadata mapping

### Timestamps

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 2 | Type (bits 12 to 15) and time zone (bits 0 to 11, signed minutes) |
| 2 | 2 | Year, signed, 1 to 9999 |
| 4 to 9 | 6 | Month, day, hour, minute, second, centiseconds |
| 10 | 1 | Hundreds of microseconds |
| 11 | 1 | Microseconds |

| Encoding | fsimg timestamp |
| --- | --- |
| All twelve bytes zero | absent, as no time was recorded (fsimg policy, Q15) |
| Type 1, zone -1439 to 1439 | `FSIMG_TIME_OFFSET`, the calendar fields as local time at that offset |
| Type 1, zone -1440 or 1440 | `FSIMG_EUNSUPPORTED` naming the entry: an fsimg offset lies strictly within a day |
| Type 1, zone -2047 | `FSIMG_TIME_WALL` |
| Type 0 | `FSIMG_TIME_UTC`; the zone is not interpreted |
| Type 2 | `FSIMG_EUNSUPPORTED` naming the entry |
| Other types, an impossible date, a field out of range | `FSIMG_ECORRUPT` |

Centiseconds, hundreds of microseconds and microseconds are 0 to 99 each;
the fraction is `centiseconds * 10000 + hundreds * 100 + microseconds`
microseconds, and `precision_ns` is 1000. Every observed timestamp is type 1
with zone 0 [M1], [G1], [P1]. Conversion uses the common calendar functions,
never the host time zone. [E1], [U1]

| fsimg field | FE | EFE |
| --- | --- | --- |
| `accessed` | access time | access time |
| `modified` | modification time | modification time |
| `created` | file times attribute, creation | creation time |
| `changed` | attribute time | attribute time |

The attribute time records the latest change of the file's attributes,
which is POSIX change time; the mapping matches the Linux and NetBSD readers
and writers (question Q1 confirms).

### Owner, mode and attributes

Owner and group are valid together when neither is `0xffffffff`; otherwise
`FSIMG_META_OWNER` is absent, since unspecified is not uid 4294967295 [U1],
[G1]. Permissions hold three five-bit classes: [E1], [N1]

| Class | Bits | Within each class |
| --- | --- | --- |
| Other | 0 to 4 | bit 0 execute, 1 write, 2 read, 3 change attributes, 4 delete |
| Group | 5 to 9 | same |
| Owner | 10 to 14 | same |

The mode's permission triplets are `perm & 7` for other, `(perm >> 5) & 7`
for group and `(perm >> 10) & 7` for owner; set-user-ID, set-group-ID and
sticky come from ICB flag bits 6 to 8. The change-attribute and delete bits
have no POSIX representation and are not reported. M1's root with mode 0755
stores `0x3ca5`: the triplets plus the owner's change-attribute bit [M1].

Attributes: hidden from the FID's characteristics, archive and system from
the ICB flags. UDF has no read-only attribute.

### Named streams

An EFE's stream directory and the file set's system stream directory are
stream directories: file type 13, FIDs whose parent FID names the owning
object (the system stream directory names itself [M1]) and whose other FIDs
name stream entries. Streams are never delivered as entries or content;
full verification walks them so their storage is owned and reconciled.

## Space management

The partition header in the PD's contents use is five `short_ad`s: [E1], [N1]

| Offset in header | Descriptor | Rule |
| --- | --- | --- |
| 0 | Unallocated space table | a USE |
| 8 | Unallocated space bitmap | an SBD |
| 16 | Partition integrity table | zero |
| 24 | Freed space table | zero, else verification is `FSIMG_EUNSUPPORTED` |
| 32 | Freed space bitmap | zero, else verification is `FSIMG_EUNSUPPORTED` |

At most one of the unallocated table and bitmap is nonzero. The space bitmap
descriptor (tag 264) holds the bit count (u32 at 16, the partition length),
the byte count (u32 at 20, `ceil(bits / 8)`) and the bitmap from offset 24,
one bit per block, least significant first, 1 meaning free; it spans
`ceil((24 + bytes) / B)` blocks from its address, which its `short_ad`'s
length covers. Bits past the partition length are not interpreted. The
unallocated space entry (tag 263, ICB file type 1) lists free extents as
`short_ad`s from offset 40, `L_AD` at 36. Read-only partitions observed carry
neither, or a bitmap [M1], [G1]. [E1], [U1]

Ownership has one rule for every partition: each block of a partition is
owned by at most one object, the space descriptors themselves included, and
with a bitmap or table every block is either owned or free, never both and
never neither. Without either, unowned blocks are permitted and the free
count is zero, as a read-only medium has no free space. The LVID's free-space
and size tables, when closed and known, equal the free count and the
partition length. A metadata partition has its own ownership domain in
metadata blocks, reconciled against its bitmap file; the physical partition
owns the metadata file's, mirror's and bitmap file's blocks like any file's.

## Reader profile

| Property | Accepted |
| --- | --- |
| Block size | 512, 1024, 2048, 4096; larger sizes `FSIMG_EUNSUPPORTED` |
| Recognition | one `NSR02` or `NSR03` in the extended area, after any `CD001`/`CDW02` slots |
| Anchors | block 256 opens the volume; verification requires two valid, agreeing anchors among the examined positions, one on a virtual volume |
| Revisions | 1.02, 1.50, 2.00, 2.01, 2.50, 2.60 in the domain identifier |
| Maps | the four profiles' closed sets |
| Access types | per profile |
| ICB strategy | 4 |
| Allocation | `short_ad`, `long_ad`, inline, AED chains, all four extent types |
| Kinds | directory, regular file, symbolic link, FIFO |
| Names | CS0 8 and 16; outside the profile omitted or refused as tabled |
| Symbolic-link targets | 1 to 1024 bytes |
| Metadata | owner, mode, four timestamps, hidden, archive, system |
| Streams | validated, never delivered |
| Integrity | open or closed |
| Bridges | the UDF view; ISO 9660 structures not interpreted |

The capability advertises files, directories, symbolic links and FIFOs;
owner, mode, the four timestamps and attributes; 512- to 4096-byte sectors
and allocation units; names of up to 254 UTF-16 units, the eight-bit maximum;
a 126-byte label; and a 1024-byte link-target limit.

Unsupported here, each with a `FSIMG_EUNSUPPORTED` result rather than a
guess: block sizes above 4096, multi-volume sets, more than one physical
partition, strategy 4096, indirect entries, external extended-attribute
files, real-time files, transformed or versioned files, devices, sockets and
Secure UDF. Later sessions of a multisession image are a limitation rather
than a detected case: a flat image carries no table of contents, so the
reader reads the session whose anchor is at block 256 of the slice. A later
session's descriptors address the disc absolutely, so a slice starting at
that session does not read it either; reading one needs a design that
states a session origin.

## Writer profile

The writer constructs a closed UDF 2.01 volume of one overwritable physical
partition with a space bitmap, every object in one extended file entry. It
never emits a VAT, sparing table, metadata partition, named stream, extended
attribute or hard link.

### Volume layout

| Volume blocks | Content |
| --- | --- |
| Bytes `[0, 32768)` | zero |
| From byte 32768, slots of `max(2048, B)` | `BEA01`, `NSR03`, `TEA01` |
| 96 to 111 | main sequence: PVD, LVD, PD, USD, IUVD, TD, then zero |
| 128 to 129 | integrity sequence: LVID, TD |
| 256 | anchor |
| 257 to `N - 18` | the partition, `L = N - 274` blocks |
| `N - 17` to `N - 2` | reserve sequence, identical to the main one except each descriptor's location |
| `N - 1` | anchor |

Every other block is zero. The positions are mkudffs's for its main
sequence, integrity sequence and partition start at every block size, and
its reserve sequence and second anchor at 4096 bytes per block [M1]; block
96 lies past the recognition sequence at every block size. Two anchors
satisfy UDF, and leaving `N - 257` to the partition removes a hole from the
partition's middle. The smallest volume is `N = 277`, a partition of three
blocks.

### Partition layout

Allocation is one prefix of the partition, in two regions:

| Partition blocks | Content |
| --- | --- |
| 0 to `S - 1` | space bitmap, `S = ceil((24 + ceil(L / 8)) / B)` |
| `S` | file set descriptor |
| then, for each catalog ID in order, the root first | the entry's EFE; its AEDs; a directory's or symbolic link's body when not inline |
| then, for each regular file in catalog ID order | its body when not inline, contiguous |
| `A` to `L - 1` | free |

Putting every directory, ICB and AED before the file bodies mirrors the
metadata partition's split: with the first region moved into a metadata
file, the same plan order is a metadata-partition layout.

### Descriptors

| Descriptor | Values |
| --- | --- |
| Tags | version 3, serial number 1, checksum and CRC over the whole descriptor; the space bitmap's CRC length 8 [M1] |
| PVD | sequence 1, number 0, volume identifier the label's longest prefix of whole characters that fits its 32-byte dstring, volume sequence 1 of 1, interchange levels 2 and 3, character-set lists 1, volume set identifier sixteen lowercase hexadecimal digits: the format time's low 32 bits then `volume_serial`, CS0 character sets, recording time the format time, application and implementation identifiers `*fsimg` with zero suffixes |
| IUVD | `*UDF LV Info` with revision `0x0201`, CS0, the label as logical volume identifier, empty information dstrings, `*fsimg` |
| PD | flags 1, number 0, `+NSR03`, the bitmap's `short_ad` (length `S * B`, block 0), access type 4, start 257, length `L` |
| LVD | CS0, the label, block size `B`, `*OSTA UDF Compliant` revision `0x0201` with no flags, the FSD's `long_ad` (length `B`, block `S`), one Type-1 map (volume sequence 1, partition 0), integrity extent of two blocks at 128 |
| USD | no extents |
| LVID | the format time, closed, next unique ID 16 plus the largest catalog ID (16 for an empty catalog, as mkudffs records [M1], since UDF reserves 1 to 15), one partition, implementation use 46: `*fsimg`, the non-directory and directory counts, revisions `0x0201` three times; free table `L - A`, size table `L` |
| FSD | the format time, interchange levels 3 and 3, character-set lists 1, file set and descriptor numbers 0, the label, the label's 32-byte prefix as file set identifier, the root's `long_ad`, the domain identifier, no next extent, no system stream directory (question Q6) |
| Space bitmap | `L` bits, `ceil(L / 8)` bytes; zero bits for blocks `[0, A)`, one bits to `L - 1`, zero padding bits |

`volume_serial` is the common writer option, so the identifier is
reproducible and needs no format setting. UDF names no universal UUID field;
`info` reports the volume set identifier's 16 digits as a detail only.

### Entries

| Object | EFE |
| --- | --- |
| Directory | file type 4, link count `1 + subdirectories`, body the parent FID then one FID per child in catalog ID order |
| Regular file | file type 5, link count 1, body the source's bytes |
| Symbolic link | file type 12, link count 1, body the target's components |
| FIFO | file type 9, link count 1, no body |

Every EFE has strategy 4, maximum entries 1, information length equal to
object size, checkpoint 1, no extended attributes, an implementation
identifier `*fsimg` and a unique ID: 0 for the root and `15 + id` for
catalog ID `id`, so IDs start at 16 as mkudffs's do [M1]. Its body takes one
of four forms, chosen by length alone:

| Body length | Form | Descriptors | Logical blocks recorded |
| --- | --- | --- | --- |
| 0 | `short_ad` form, `L_AD` 0 | none | 0 |
| 1 to `B - 216` | inline (form 3) | the body itself | 0 |
| above `B - 216`, needing at most `c` descriptors | `short_ad` over one contiguous run | `ceil(length / (2^30 - B))`, each at most `2^30 - B` bytes | the run's blocks |
| needing more than `c` descriptors | the same run, the EFE holding `c - 1` descriptors and a continuation | AEDs of `a` descriptors each, the last slot of every AED but the last a continuation | the run's blocks |

Here `c = floor((B - 216) / 8)` and `a = floor((B - 24) / 8)`: at 512 bytes
per block an EFE holds 37 descriptors, so files above about 37 GiB use AEDs;
at 2048 it holds 229.

A FID has version 1, the directory bit for a directory, the hidden bit for a
hidden entry, a `long_ad` of length `B` naming the EFE with the target's
unique ID in its implementation use, no implementation use of its own, and
the name; records are packed (question Q4). A name is encoded with
compression ID 8 when every code point is at most U+00FF and 16 otherwise,
supplementary characters as surrogate pairs (question Q11). A name of more
than 254 bytes after the compression ID fails planning with `FSIMG_EINVAL`,
and so does a name holding a character from U+0001 to U+001F or U+007F,
which the reader's profile would omit: every name the writer stores is one
fsimg reads back. The catalog's names are distinct UTF-8 strings and CS0 is
a bijection from them, so no two FIDs collide.

A symbolic link's target is split at `/`: a leading `/` is a type-1
component, `.` and `..` are types 4 and 3, and every other component is type
5 under the name encoding. The target `/` alone is the root component
alone. An empty component anywhere else, as in `a//b` or `a/`, has no
representation and fails planning with `FSIMG_EUNSUPPORTED` naming the
entry, as does a component of more than 254 bytes after the compression ID
and a target that is not valid UTF-8, which CS0 cannot encode; nothing is
collapsed silently.

### Metadata

Owner, mode, times and attributes follow the
[metadata policy](design-metadata.md#writer-policy):

| Field | Stored | Absent | Loss |
| --- | --- | --- | --- |
| Owner and group | as given | `0xffffffff`, unspecified | 4294967295 stored as unspecified |
| Mode | triplets as read, owner's change-attribute bit always, no delete bits (question Q9), high bits in the ICB flags | 0755 directories, 0644 files, 0777 links and FIFOs | none |
| Times | type 1 with the timestamp's offset, zone 0 for UTC, zone -2047 for a wall time; microseconds | the format time | sub-microsecond digits, years outside 1 to 9999 |
| Attributes | hidden, archive, system | none set | read-only |

UDF records local time with an offset, so a UTC time is stored at offset 0
and reads back as `FSIMG_TIME_OFFSET` with offset 0: the same instant, and
not a loss.

Every loss is an optional loss under the common policy: counted and
reported, and a failure only for a caller that forbids losses. The format
time is the UTC host clock sampled once during planning; in reproducible
mode it is the epoch, and every stored time is the epoch. Either must lie in
years 1 to 9999. The catalog's root
carries no metadata, so the root has mode 0755, an unspecified owner and the
format time.

### Settings and sizing

`sector_bytes` is zero, selecting 2048, or one of 512, 1024, 2048 and 4096;
`allocation_unit_bytes` is zero or equal to it. `volume_bytes` is zero or a
multiple of `B` from `277 * B` to `2^32 * B`. The label is at most 126
encoded bytes, under the name encoding, without characters U+0001 to U+001F
or U+007F; an absent or empty label stores empty identifiers. The writer
stores no format-specific setting. A volume of `N` blocks holds the plan when
`S(L) + 1 + M + D + ceil(min_free_bytes / B) <= L` with `L = N - 274`,
where `M` counts the first region's EFE, AED and body blocks and `D` the file
bodies' blocks. Automatic sizing takes the smallest such `N`, found by
raising `L` until the inequality holds, since `S` grows by one block per
`8 * B` partition blocks. UDF has no other dimension, so
[minimization](design-sizing.md) returns the default volume.

## Deterministic planning and emission

Planning is a fixed sequence of passes over the frozen catalog: entries,
geometry, layout, summary. Every planned value derives from the catalog, the
options and the rules above, so output is byte-identical across source
order, budgets, scratch placement, locale and time zone. Emission writes
payload copies, inline bodies, EFEs, AEDs, directory bodies, link bodies,
the bitmap, the FSD, both sequences, the integrity sequence, the anchors and
the recognition sequence; every emitted byte comes from a plan row, the
geometry or a profile constant, and the common emission object has zeroed
everything else. The [driver design](design-udf-driver.md) fixes the passes
and the emission order.

## Resource and failure requirements

Keep only the geometry, the decoded volume model (descriptor fields, maps
and the translation state they need), the current ICB, a bounded number of
block windows and the current descriptor-list position resident. Scratch
holds the claimed-block bitsets, the exposure bitset, extent indexes, a
sparing table's entries, the VAT's and metadata files' indexes, directory
records, link-count streams and plan rows, never copied payload. Opening
reads the anchor, the sequences and the file set, and on a Type-2 profile
walks the backing file's or VAT's descriptor list or reads a sparing table,
work proportional to those structures; it never scans a partition. A random
content lookup costs one index search after the object's list is walked
once; sequential reads batch adjacent blocks. Full verification can cost work proportional to the
partition's bitmap and the metadata partition's size; opening and selected
reads do not imply that. Every loop charges the session's work limit and
observes cancellation; scratch exhaustion fails explicitly.

Failures are classified by the common statuses: unrecognized image
(`FSIMG_EFORMAT`), recognized but outside the profile
(`FSIMG_EUNSUPPORTED`), contradiction (`FSIMG_ECORRUPT`), invalid setting or
selection (`FSIMG_EINVAL`), content that fits no size (`FSIMG_ERANGE`),
exhaustion (`FSIMG_ELIMIT`), and source change, cancellation and I/O failure
as common code reports them. A failed lookup never becomes end of file or a
zero extent. A partial output is never success. Diagnostics name a map
index, a logical block and the resolved volume offset where they help, at
most one bounded, escaped name, and never descriptor bytes, stale deleted
names or buffer contents.

## Acceptance matrix

Synthetic fixtures come from an original generator with its own offset
tables; oracle images are created at test time by the tools the build
finds, and captured images are read from paths the environment gives and
never redistributed. Every passing case is paired with a mutation of the
same fixture that must fail with its class, so a test proves the checked
code ran; a semantic mutation repairs the checksum and CRC so it reaches the
rule, and a separate raw mutation leaves them broken.

| Area | Required cases |
| --- | --- |
| Probe and geometry | Each block size with each slot stride; leading `CD001` slots, none and several; `BEA01` without `NSR`, two `NSR`s, a lone `CD001`, an all-zero slot, `BOOT2` inside the extended area; strides above 4096 as unsupported; a slice length with a remainder after whole blocks; an image shorter than block 257; two block sizes both carrying a valid anchor at 256; a nonzero slice offset; probe exclusivity against FAT, exFAT, QNX6, QNX4 and ImageFS fixtures in both directions; the M1-mbr whole-disk MBR under the layer rule the plan's owner decision 7 settles |
| Anchors and sequences | Anchor checksum, CRC, CRC length, reserved byte, version and location faults; a CRC length below the fixed part, as unsupported; anchors at 256 only, 256 and `N - 1`, all three, only at the tail (unsupported) and nowhere (corrupt); a damaged anchor at 256 with a good one at `N - 1`, corrupt; `N - 257` inside the partition holding data and holding a valid anchor, which is claimed; G1's padding anchors; disagreeing anchors; sequence extents under 16 blocks, overlapping each other, the partition or an anchor; pointer chains, a pointer cycle, 65 extents; an unknown identifier in a sequence; equal sequence numbers with different contents; a damaged reserve sequence, and one differing in a consumed field and in an unconsumed one |
| Volume descriptors | Every rule of the PVD, PD, LVD and IUVD tables; maps of each type and length, overrunning and short tables, unknown Type-2 identifiers, every accepted and refused map set, a map naming no PD; revision and `NSR` disagreement; block size disagreement |
| Integrity | Open and closed; a chained integrity extent; wrong free and size tables; wrong counts; next unique ID not above the largest; minimum read revision above 2.60 |
| File set and ICBs | Multiple FSDs, a second file set, a next extent; root of the wrong type; strategy 4096; every file type; every flag; record fields; `L_EA` and `L_AD` overflowing the block; inline data longer than its block; extended-attribute framing faults and a file times creation time on an FE |
| Allocation | Short, long and inline forms; AED chains of 1, 2 and many blocks; an AED returning to itself and to an earlier AED; an AED holding only a continuation, as unsupported; a continuation followed by descriptors; descriptor lengths summing short of and beyond the information length; a non-final partial extent; each extent type in the middle and at the end, with storage containing nonzero bytes that must never be read; a recorded extent into a sequence, an anchor, the bitmap or another file; a block-number overflow; a file above 4 GiB |
| Directories | Parent FID missing, misplaced, named, not a directory, naming the wrong directory; a second parent FID; deleted FIDs with compression IDs 8, 16, 254 and 255; a FID crossing a block and an extent boundary; `L_IU` with and without an entity identifier; record overrunning the body; nonzero padding; directory bit disagreeing with the target; a cycle; a directory with two names; a hard-linked file listed twice and owned once; a wide directory that spills to scratch |
| Names | Compression IDs 8 and 16 with ASCII, Latin-1, BMP and paired supplementary characters; an odd 16-bit length; an unknown ID; 254 bytes after the compression ID; `A` and `a` coexisting; two encodings of one name colliding; a control character, `U+007F` and a lone surrogate on a file, a link and a directory with descendants, each omitted and counted once with its subtree undelivered and verified; NUL, slash, `.` and `..` refused |
| Symbolic links | Every component type, `L_CI` and version rule; relative, absolute by type 1 and by type 2 targets; a target of 1024 and 1025 bytes; an empty stream; a lone surrogate in a component |
| Metadata | Timestamps of each type, zone -2047, zones -1439 and 1439, zones -1440 and 1440 as unsupported, all-zero, impossible dates, out-of-range fractions; owner unspecified in one and both fields; permission triplets for each class, set-user-ID, set-group-ID, sticky; hidden, archive and system; real-time and device files refused |
| Space and ownership | Bitmap and table forms; a referenced free block; an allocated orphan; a block with two owners, metadata and data alike; bitmap counts disagreeing with the partition; freed-space descriptors; read-only partitions without space information |
| Metadata partition | The physical bootstrap; FSD, directories and ICBs through the metadata file and bodies through the physical map; a metadata block in a hole; a metadata file naming the metadata map; mirror content equal and different; a duplicated mirror disjoint from, partly overlapping and equal to the main file's extents; an unflagged mirror with equal extents, with disjoint extents (unsupported) and partly overlapping ones (corrupt); a mirror overlapping unrelated content; a bitmap file describing too few blocks |
| Sparable partition | Remapped and unmapped packets; a run crossing a packet boundary; duplicate originals and destinations; misaligned packets; table copies equal and different; a spare packet inside a partition, as unsupported |
| Virtual partition | The 2.00 header and the 1.50 trailer; a header length below 152, an implementation use past the body, an entry area not a multiple of 4; a VAT index past the table; an entry outside the partition; a last block that is not a VAT ICB, as unsupported; a forged VAT trailer with a bad tag; the partition extending past the image |
| Writer | Exact and automatic sizing at each bitmap-block boundary and each block size; the empty catalog at 277 blocks; a full volume; inline and block bodies at `B - 216` and `B - 215` bytes; an AED chain at each block size; names at 254 eight-bit characters and 127 UTF-16 units and one over; symbolic links of every shape and the refused ones; owner 4294967295, sub-microsecond times and read-only attributes as counted losses and as refusals when losses are forbidden; byte identity across source order, cache size, scratch path and time zone; a poison scan of every unused byte |
| Failure | Allocation, scratch, short read, short write, premature EOF, source change, cancellation, flush and publication failures at every stage, each followed by a cleanup check |

Independent acceptance is bidirectional and per profile. fsimg reads every
oracle image: mkudffs at every media type, block size and revision it
writes, genisoimage and pycdlib bridges, and the NetBSD, Windows and macOS
images of the plan's corpus, comparing names, sizes, checksums and
metadata with each image's manifest. fsimg output is checked by NetBSD
`fsck_udf -fn`, read by `udfinfo`, extracted by 7-Zip and by the NetBSD,
Linux and Windows readers, and mounted read-write by Linux and NetBSD, which
create, extend, rename and remove files, unmount and check again, after
which fsimg verifies the result. The writer capability is registered only
after that native record exists for every advertised block size.

## Source keys

Provenance and the observations are in the
[research record](../docs/udf-research.md).

| Key | Source |
| --- | --- |
| E1 | ECMA-167 3rd edition, volume and file structure |
| U1 | ECMA TR/112 parts 2 to 7, UDF 2.60 to 1.02 |
| N1 | NetBSD `ecma167-udf.h` 1.17: field-offset cross-check |
| M1 | mkudffs 2.3 and udfinfo images |
| G1 | genisoimage 1.1.11 bridge |
| P1 | pycdlib 1.22.0 bridge |
| Z1 | 7-Zip 23.01 listing |

[E1]: https://ecma-international.org/wp-content/uploads/ECMA-167_3rd_edition_june_1997.pdf
[U1]: https://ecma-international.org/publications-and-standards/technical-reports/ecma-tr-112/
[N1]: ../docs/udf-research.md#sources
[M1]: ../docs/udf-research.md#observations
[G1]: ../docs/udf-research.md#observations
[P1]: ../docs/udf-research.md#observations
[Z1]: ../docs/udf-research.md#observations
