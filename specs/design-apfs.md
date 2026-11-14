# APFS reader and writer

## Scope and evidence

`apfs` denotes the Apple File System: an APFS container holding one or more
APFS volumes. The reader opens one container, selects one committed
checkpoint and one volume, and exposes that volume's namespace, optionally as
it stood at one of its snapshots. The writer constructs a new container
holding one volume, suitable for subsequent use by macOS; it does not modify
an existing container, add a volume to one, or implement copy-on-write
transactions. Reader and writer remain independently registered
capabilities, and the writer is registered only after native qualification
(see [acceptance](#acceptance-and-independent-validation)).

The APFS container is the filesystem's own structure, not a layer. The
[layer design](design-layers.md) calls a disk-image format a container and
always writes this structure out as "APFS container"; this document does the
same. A DMG, a GPT or Apple Partition Map partition, or a bounded slice
around an APFS container is peeled by the layers before the driver opens it,
so the driver reads a bare APFS container whose block 0 is its superblock;
[probe and identity](#probe-and-identity) states what the layers cover.

This document is an original description of disk-format facts and fsimg
requirements. It contains no copied reference prose, C declarations, or code.
Its input is the research draft
[APFS on-disk format and fsimg implementation specification](../fsspecs/fsimg_apfs_spec.md)
("the draft"), which is not itself authoritative; the
[implementation plan](../docs/apfs-plan.md) records where this design departs
from it. The research record, `docs/apfs-research.md`, which the plan's
evidence stage writes, holds sources, licenses, tool identities and
observations.

Evidence has three meanings here, as in the QNX designs:

* **Established layout**: a byte interpretation given by Apple's public
  reference [A1]. It establishes field positions and meanings, not that every
  combination is valid, nor what current macOS emits; the reference predates
  several macOS releases.
* **Observation**: a measured property of a named native fixture from the
  plan's experiments (E1 through E17). An observation is not a universal
  format rule.
* **fsimg policy**: a deliberate validation, capability or construction rule.
  It can be stricter than macOS without redefining the format.

A rule or value stated ahead of its observation is **provisional**. It names
the experiment that closes it, and "E8 checks" or "E1 shows" names an
experiment that is to settle the point, not a result. Each provisional item
also has a default for an experiment that cannot answer, for example
because no specimen exists or a native tool refuses the probe:

* A provisional reader rule stays as written unless its passage gives
  another default. It is the stricter reading, so an image outside it is
  refused with the stated status and never misread.
* The writer's provisional values and their defaults are collected in
  [writer values](#writer-values).

Offsets and constants below come from [A1]. Before code relies on a table,
the plan's evidence stage checks it against native fixtures with an
independently written structure dumper; a disagreement is resolved in this
document, not in the code. An unresolved encoding is a compatibility gate, not
permission to write zero, copy a template image, or infer semantics from a
field's name. The [driver design](design-apfs-driver.md) maps these rules
onto the modules, state and algorithms of `lib/formats/apfs/`.

## How APFS works

This section explains the operating model the rules in the rest of this
document serve.

### A container is a block space shared by volumes

An APFS container is an array of `N` blocks of `B` bytes, addressed by
physical block number from the container's first byte. Block 0 holds a copy of
the container superblock. The container owns all allocation: one space
manager tracks every block, and every volume allocates from the same free
space. A volume has no byte range of its own; its metadata and file data are
scattered through the container, interleaved with other volumes'. A volume
is therefore not a slice, and selecting a volume is a filesystem operation,
not a partition selection.

### Everything is an object

Metadata is stored as objects. An ordinary object occupies one or more whole
blocks and begins with a 32-byte header holding a Fletcher-64 checksum of the
rest of the object, its object identifier (OID), the transaction identifier
(XID) that wrote it, and its type. An object is addressed in one of three
ways, its storage class:

* **Physical**: the OID is the object's block number.
* **Virtual**: the OID is a stable name that an object map translates, for a
  given XID, into a block number. Volume superblocks and filesystem-tree
  nodes are virtual, so a modified node can move without rewriting the
  references to it.
* **Ephemeral**: the object lives in the checkpoint data area and is found
  through the selected checkpoint's mapping table. The space manager and the
  reaper are ephemeral.

### Changes never overwrite committed state

macOS never overwrites a block that the committed state uses. A transaction
writes new versions of changed objects to free blocks, records new
object-map entries for changed virtual objects, and then commits a
checkpoint: a set of checkpoint-mapping blocks followed by a new container
superblock in a ring of descriptor blocks, together with the ephemeral
objects it maps in a ring of data blocks. The committed state is the valid
checkpoint with the largest XID. A crash before the superblock write leaves
the previous checkpoint intact; there is no journal to replay.

### Object maps make history addressable

An object map is a B-tree from `(OID, XID)` to a physical address. Looking up
a virtual object at XID `T` takes the entry with that OID and the largest XID
not above `T`. Older entries remain while a snapshot needs them, so reading a
snapshot is the same lookup with the snapshot's XID. The container has one
object map, which maps volume superblocks; each volume has its own, which maps
its filesystem-tree nodes.

### Almost everything is a B-tree

Object maps, the filesystem tree, the extent-reference tree, the snapshot
metadata tree, the space manager's free queues and a sealed volume's
file-extent tree share one B-tree node format. Nodes are whole objects;
internal nodes point at children by OID, physical or virtual according to
the tree. Each tree has its own key comparator.

### A volume is one filesystem tree

A volume's filesystem tree holds every record of its namespace in one key
space: inodes, directory entries, extended attributes, data-stream reference
counts, file extents, hard-link sibling records and others. A key starts
with a 64-bit word whose top four bits name the record kind and whose low 60
bits name the owning object, so all records of one object are adjacent and a
directory's entries form one contiguous key range. File content lives in
data streams identified by a private ID, usually the inode number; file
extents map a stream's logical byte ranges to physical blocks. Extended
attributes are records too; symbolic-link targets and compression metadata
are stored as attributes with reserved names.

### Space is managed by the container

The space manager is an ephemeral object describing allocation chunks, each
covering one bitmap block's worth of blocks. Chunk-information blocks list
each chunk's bitmap block and free count; for large containers,
chunk-information address blocks list the chunk-information blocks. An
internal pool reserves blocks for the space manager's own bitmaps. Freed
blocks pass through free queues, keyed by the XID that freed them, before
becoming allocatable, so a block a checkpoint still references is never
reused before a newer checkpoint commits.

### Features layered on the model

Clones share physical extents between data streams; the extent-reference
tree counts references. Snapshots retain old object-map entries and their
own extent-reference trees. Transparent compression stores a file's bytes in
a `com.apple.decmpfs` attribute or a resource-fork attribute instead of its
data stream. A sealed volume records a hash tree over its filesystem tree and
stores file extents in a separate tree. Encryption wraps
metadata and data in keys held in keybags. Fusion spreads one container over
two devices.

### Consequences for fsimg

* A reader consumes exactly one checkpoint, chosen by the
  [selection rule](#checkpoint-selection), and resolves every virtual object
  at one XID: the checkpoint's, or a selected snapshot's. It never merges
  mappings from different checkpoints and never needs recovery logic.
* A volume is selected, never assumed: a container with several volumes
  needs the caller's choice, and allocation is verified container-wide
  because volumes share it.
* The writer emits a container with exactly one committed checkpoint and no
  pending work: no free-queue entries, no reaper work, no history. All blocks
  are written before the image is published by the output coordinator; a
  partially written image is never reported as success.

## Compatibility profiles

| Profile | Reader contract | Writer contract |
| --- | --- | --- |
| Unencrypted container | See the list below | See the [writer profile](#profile) |
| Snapshots | A named snapshot of the selected volume, read at its XID; verification including snapshot-retained blocks | Never emitted |
| Sealed volume | The sealed file-extent tree and hashed nodes; the metadata integrity chain checked to the stored root hash, reported as not authenticated | Never emitted |
| Encrypted volume, Fusion container, noncontiguous checkpoint area, incomplete restore | Recognized and refused as unsupported, naming the feature | Never emitted |

The unencrypted-container reader accepts:

* version-2 containers on one device, with block sizes 4096 through 65536
  and contiguous checkpoint areas;
* any number of volumes, one of them selected;
* case-insensitive and case-sensitive volumes, with hashed or plain
  directory keys;
* regular files, directories, symbolic links and FIFOs, with hard links
  exposed as separate entries, and character devices, block devices and
  sockets as read-only kinds;
* sparse files, clones, and transparent compression with the zlib, LZVN and
  LZFSE codecs.

The writer emits a new container with 4096-byte blocks and one unencrypted,
unsealed, case-insensitive or case-sensitive volume holding regular files,
directories, symbolic links and FIFOs. A feature outside these profiles is
refused with `FSIMG_EUNSUPPORTED`, never skipped. The
[plan](../docs/apfs-plan.md) orders the reader profiles as stages of one
driver.

## Shared owners and representation

The resource context owns memory, scratch, work accounting and cancellation.
Bounded I/O owns all container-relative byte access. The common source
catalog owns input identities and repeatable reads; the output coordinator
owns publication. The common walker owns traversal, paths and selection, and
common delivery owns content reads. APFS code obtains neither ambient host
paths nor unbounded descriptors, and the extractor keeps its safe
file-and-directory profile.

An APFS reader object holds the validated container geometry, the selected
checkpoint, the selected volume's superblock, the read XID and a fixed set of
B-tree cursors. One private B-tree module serves every tree, and one object
reader resolves every object reference; it introduces no mutable object
cache, inode cache or VFS. Reader state and immutable writer plan rows remain
separate.

Offsets and products use checked `uint64_t` arithmetic. Every APFS integer is
little-endian and decoded from bytes; no native structure is laid over image
bytes. The one big-endian structure the driver reads is the classic
resource-fork header inside a compressed file's resource fork, which
[compression](#transparent-compression) describes. A physical address is a signed 64-bit field; a negative value or one
at or past `N` is corrupt wherever an address is required. UUIDs are byte
arrays kept in their on-disk order, which is RFC 4122 text order.

## Container

### Object header and checksum

Every object the reader decodes, except where a rule below says a node has
no header, starts with:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | Checksum |
| 8 | 8 | OID |
| 16 | 8 | XID |
| 24 | 4 | Type: object kind in bits 0 to 15, flags in bits 16 to 31 |
| 28 | 4 | Subtype |

The checksum is Fletcher-64 over the object's bytes from offset 8 to its end,
read as little-endian 32-bit words, with both sums taken modulo
`2^32 - 1`: for each word `w`, `s1 = (s1 + w) mod m` and `s2 = (s2 + s1) mod
m`, starting from zero. The stored value is `c1 | c2 << 32` with
`c1 = m - ((s1 + s2) mod m)` and `c2 = m - ((s1 + c1) mod m)`. A stored
checksum is valid exactly when recomputing it gives the stored value. The
arithmetic reproduces the stored checksum of every object E1 decoded, in
every capture (E1, apfs-newfs-107374182400-plain among them). A block of
zeros computes to all ones, so it never carries a valid checksum.

The type flags this document uses are the storage class in bits 30 and 31
(`0` virtual, `0x80000000` ephemeral, `0x40000000` physical), no-header
`0x20000000`, encrypted `0x10000000` and nonpersistent `0x08000000`. The
object kinds are those of [A1]; this document names them by role. A decoded
object is accepted only with the kind, subtype and storage class its
reference demands, its OID equal to the reference's OID (physical: its block
number), and an XID that is nonzero and not above the XID it is read at. An
object with the encrypted or nonpersistent flag is unsupported wherever it is
met.

### Container superblock

The superblock is `NXSB` (`0x4253584e` little-endian) at offset 32 of an
object of kind 1 with OID 1, whose header carries the ephemeral storage
flag: the type word is `0x80000001` in block 0 and in every ring
superblock of every E1 capture. The fields the driver uses:

| Offset | Size | Field |
| --- | --- | --- |
| 36 | 4 | Block size `B` |
| 40 | 8 | Block count `N` |
| 48 | 8 | Optional features |
| 56 | 8 | Read-only-compatible features |
| 64 | 8 | Incompatible features |
| 72 | 16 | Container UUID |
| 88 | 8 | Next OID |
| 96 | 8 | Next XID |
| 104 | 4 | Descriptor-area block count; bit 31 marks a noncontiguous area |
| 108 | 4 | Data-area block count; bit 31 likewise |
| 112 | 8 | Descriptor-area base block |
| 120 | 8 | Data-area base block |
| 128 | 4 | Next descriptor index |
| 132 | 4 | Next data index |
| 136 | 4 | Descriptor index of this checkpoint's first block |
| 140 | 4 | Descriptor block count of this checkpoint |
| 144 | 4 | Data index of this checkpoint's first block |
| 148 | 4 | Data block count of this checkpoint |
| 152 | 8 | Space manager OID (ephemeral) |
| 160 | 8 | Container object map OID (physical) |
| 168 | 8 | Reaper OID (ephemeral) |
| 176 | 4 | Test type; zero |
| 180 | 4 | Maximum volume count |
| 184 | 800 | One hundred volume superblock OIDs (virtual), zero when unused |
| 984 | 256 | Thirty-two counters |
| 1240 | 16 | Blocked-out range (Fusion) |
| 1256 | 8 | Eviction mapping tree OID (Fusion) |
| 1264 | 8 | Flags |
| 1272 | 8 | EFI jumpstart block |
| 1280 | 16 | Fusion UUID |
| 1296 | 16 | Keylocker range |
| 1312 | 32 | Four ephemeral-information words |
| 1344 | 8 | Test OID; zero |
| 1352 | 32 | Fusion middle-tree OID, write-back cache OID and range |
| 1384 | 8 | Newest mounted version |
| 1392 | 16 | Media keylocker range |

The geometry rules are fsimg's acceptance of [A1]'s limits: `B` a power of
two from 4096 through 65536; `N * B` computed checked; `N` at least the
container minimum of [A1], 1 MiB worth of blocks; the maximum volume count
from 1 through 100, every OID slot at or past it zero. The two areas lie
inside `[1, N)` and do not overlap each other or block 0. Bytes past offset
1408 within block 0 are not interpreted by the reader. A slice longer than
`N * B` has ignored slack past it; a shorter one is a
[truncated container](#truncated-containers). The 1 MiB minimum is the
reader's bound; the smallest container the writer emits is the one
[writer values](#writer-values) gives, 1024 blocks, the smallest
`fsck_apfs` opens.

The keylocker and media keylocker ranges locate keybags, which only
encrypted volumes use. A disk-image container holding a software-encrypted
volume has a one-block keylocker and the software-cryptography flag, also
when its other volumes are unencrypted; the keybag is wrapped, so the
block is not a plain object (E11, every encrypted capture). Reading an unencrypted volume ignores
both ranges; a nonzero range must lie inside `[1, N)`, verification claims
it as one range, and its contents are never read.

Feature handling is a closed table per mask:

| Mask | Known bits | Reader | Writer |
| --- | --- | --- | --- |
| Incompatible | `0x2` version 2 | Required | Emitted |
| Incompatible | `0x1` version 1, `0x100` Fusion | Unsupported | Never |
| Incompatible | Any other bit | Unsupported | Never |
| Read-only compatible | None defined | Any bit accepted for reading | Zero |
| Optional | `0x1` defragmentation, `0x2` low-capacity Fusion mode | Accepted; `0x2` with the Fusion bit only | Never: the native default is zero (E1, every capture) |
| Optional | Any other bit | Accepted, ignored | Never |
| Flags | `0x4` software cryptography | Accepted; informs the encryption report | Never: the native default is zero (E1, every capture) |

### Probe and identity

The identity check is the `NXSB` magic at offset 32 of block 0 and nothing
else. The header's object kind and OID, the checksum, the geometry, the
features and each checkpoint candidate's magic, kind and checksum are
validation, in the division the
[format contract](design-formats.md#format-descriptors-and-profiles) makes
for every format.

The probe inspects the first 4096 bytes, the smallest legal block, cut to
the volume length. The shared detection prefix is the largest any probe
asks for, so a probe reading the largest block, 65536 bytes, would raise
every image's detection read beyond the 34816 bytes the ISO 9660 probe
needs, to serve one format. The checksum covers the whole block, so the
probe verifies it only for a 4096-byte block; for a larger one, `open`
reads the whole block and verifies it. Without the magic the probe makes
no claim, `FSIMG_EFORMAT`. With it, it claims the image: `FSIMG_OK` when
the volume holds `B` bytes and the header's kind and OID, the checksum of
a 4096-byte block, the geometry and the incompatible features pass;
`FSIMG_ECORRUPT` for a volume shorter than one block, a wrong kind or OID,
a bad checksum or bad geometry; `FSIMG_EUNSUPPORTED` for an incompatible
feature outside the table. The checksum is never waived: a container with
a larger block and a bad block-0 checksum is detected as APFS and fails to
open. The probe proves only that the image is an APFS container; the
checkpoint `open` selects may be newer than block 0.

Opening with an assumed identity (`--force`, `FSIMG_READ_ASSUME_IDENTITY`)
runs no probe. `open` waives the magic of block 0 alone, through
`fsimg_reader_identity()`, which records the failed check so that full
verification fails as the format contract states, and it runs every
validation check the probe makes, at any input length. The checksum is one
of them and is never waived. Fletcher-64 covers the magic, so the flag
changes the result only for a block 0 whose magic differs and whose
checksum is valid for the bytes it holds; a block whose magic was damaged
after its checksum was computed fails the checksum, with or without the
flag. A descriptor block is a checkpoint candidate only with its own magic,
whatever was assumed about block 0.

A disk or DMG never reaches this probe with its own header at offset 0,
because the layers peel it first:

* On a GPT disk the container is the partition of type
  `7C3457EF-0000-11AA-AA11-00306543ECAC`.
* On an [Apple Partition Map](design-layers.md#apm) disk no partition type
  is tied to `apfs`. macOS makes no such disk directly: `hdiutil create
  -layout SPUD -fs APFS` fails and `diskutil` refuses an APFS container on
  the scheme, but `newfs_apfs` formats an existing partition and leaves its
  type, `Apple_HFS` when `diskutil` made it (E13). A partition holding a
  container, whatever its type, is a candidate like every other used entry
  and is selected because this probe claims it. The layers read that
  scheme and never write it, so no APFS image fsimg creates has one.
* A [DMG](design-layers.md#dmg-udif) is read when its chunks are raw, zlib
  (UDZO) or bzip2 (UDBZ). A UDRW image from `hdiutil create` is no DMG at
  all but a flat raw disk without a trailer (E13), read as a bare disk.
  LZFSE (ULFO) and LZMA (ULMO) chunks fail in the layer as unsupported
  chunk compression, naming the type, before any APFS code runs: that is a
  layer limit, not an APFS rule. A ULMO image's first chunk is an xz stream
  at offset 0, so detection without an explicit read-side stack finds both
  the xz compression and the DMG trailer and reports the input as ambiguous
  (E13).

### Checkpoint selection

The descriptor area is a ring of `D` blocks from its base. Each descriptor
block is either a container superblock or a checkpoint-mapping block (kind
12, physical). A checkpoint is the run of descriptor blocks from its
superblock's first-block index for its block count, wrapping modulo `D`;
the last block of the run is the superblock and the others are mapping
blocks. The data run is the superblock's data index and count, wrapping
modulo the data area's size.

A candidate superblock is a descriptor block whose checksum, magic, kind and
geometry are valid, whose block size, UUID, descriptor base and descriptor
block count equal block 0's, and whose XID is nonzero. Block 0 serves only
to find the ring: [A1]'s mounting procedure reads the descriptor base from
it and says block 0 may be an old version, used "only to find the
checkpoints". A candidate's block count and data area may therefore differ
from block 0's, and `N` and the data area are the selected superblock's.
A native resize is not such a case: it moves the descriptor area too, and
once `diskutil apfs resizeContainer` returns, block 0 already holds the
resize checkpoint's XID, block count and areas, even while the disk is
attached (E18, every live clone). A block 0 from before a resize would
name the old ring, and the reader would select among the checkpoints left
there, as for any older block 0. A never-written slot is all zero
(E1: slots 4 to `D - 1` of every capture with a volume, slots 2 to 7 of
apfs-newfs-372736-plain); its checksum fails, so it is unused, neither a
candidate nor a mapping block, and never a fault in itself. Two candidates with the same XID are an
ambiguous tie, `FSIMG_ECORRUPT`: each checkpoint writes its superblock once,
and E8 finds no XID twice in any of its nine ring captures. A candidate's **descriptor run is whole** when:

1. the run has at least two blocks, lies in the ring, and ends at the
   candidate's own slot;
2. every mapping block in the run is valid, has the candidate's XID, and has
   a count that fits the block; exactly the run's last mapping block carries
   the last-map flag `0x1`, and no flag outside it is set;
3. every mapping names an object kind and subtype and a byte size that is a
   positive multiple of `B`;
4. the candidate's data run (its first index below the data area's size and
   its length at most that size) and its next-index fields lie within their
   rings, and its next XID exceeds its XID.

Each of these depends only on one descriptor block and its slot, so one pass
over the ring records them per slot, and runs are judged from those records
without reading a block again. A slot that matches a candidate's XID matches
no other candidate's, because XIDs are distinct, so judging every run costs
at most `D` record reads plus one per candidate. The selected checkpoint is
the candidate with the largest XID whose descriptor run is whole; if no
candidate's run is whole, the image is `FSIMG_ECORRUPT`. A candidate with
a larger XID whose run is not whole is a checkpoint a crash interrupted:
the reader passes over it and reports the volume's `clean` state as false.
`fsck_apfs` does the same in E8, passing over a valid newest superblock
whose mapping block is zeroed. A newest superblock that is itself invalid
is no candidate, so the next older checkpoint is selected without
affecting `clean`; in E8 `fsck_apfs` checks it, and the kernel mounts it.
Older checkpoints remain readable because the native driver reuses neither
their slots nor their freed blocks at once: in E8 the 1 GiB container's
ring keeps its last 8 checkpoints, and the free queues hold freed blocks
for more transactions than that.

The selected checkpoint's mappings are then checked together: every mapped
block run lies inside the checkpoint's data run without straddling the data
ring's wrap, every ephemeral OID is distinct, no two mapped runs overlap,
and the space manager and reaper OIDs are mapped with their kinds. Each mapped object is
read once and must have a valid header whose OID, kind and subtype equal the
mapping's and whose XID is not above the checkpoint's, and the container
object map must be a valid physical object. A failure in this phase is
`FSIMG_ECORRUPT`, or `FSIMG_EUNSUPPORTED` for an object outside the
profile, such as an encrypted one: the reader does not fall back to an older checkpoint after
judging a newer one's descriptor run whole. This is fsimg policy, stricter
than macOS: in E8 `fsck_apfs` and the kernel pass over a newest checkpoint
whose descriptor blocks are valid and whose mapped objects are zeroed, and
mount the older one. A newest superblock and mapping block that are whole
vouch for that checkpoint, so fsimg reports the damage rather than present
the older state it superseded. That macOS never splits a multi-block
object across the data ring's wrap is provisional: E12 shows two-block
space managers from 3 to 7.8 TiB and from 50 TiB, and E8 runs on 1 GiB
containers, whose objects are one block. An E8 mount-cycle run that wraps
the ring of a container whose space manager is two blocks, such as 4 TiB,
closes it; if no such run is made, the rule stands. Selection therefore
reads the `D` descriptor blocks once, the selected run's mapping blocks a second time,
and one checkpoint's data run.

Block 0 does not select the checkpoint; it constrains the candidates. It is
a copy of the superblock the last unmount committed: E8 finds it equal,
after each unmount, to the newest ring superblock except for the checksum
and counter 0 at offset 984, which is 3 larger (in
apfs-newfs-372736-plain 4 larger, as in every E1 container without a
volume, whose ring holds the first form, and 3 larger in the second form
of a container with one, as the
[E1 notes](../docs/apfs-research.md#e1-empty-containers) record) and shows block 0
checksummed after the ring copy, and finds it left at the previous unmount's XID while a volume
is mounted. A crash after a mount has committed a checkpoint therefore
leaves block 0 older than the newest checkpoint. A block-0 XID larger than every whole candidate's is
`FSIMG_ECORRUPT`, as a committed checkpoint lost from the ring. This is
fsimg policy, and it rests on block 0 reaching the disk after the ring
superblock at unmount, which E8 infers from the counter but cannot observe;
if it does not, a crash between the two writes leaves a container that
fsimg refuses and macOS opens. In E8 `fsck_apfs` checks the older
checkpoint of a copy whose block 0 is newer without remark, and the kernel
mounts the copy.

A noncontiguous area (bit 31 set) is `FSIMG_EUNSUPPORTED`: its tree layout
has no evidence. Every E1 and E12 dump uses contiguous areas, from the
minimum container to 128 TiB (apfs-newfs-140737488355328-e12-128t) and a
1 PiB container of 16384-byte blocks (`e12-sweep/n68719476736-b16384`).
The bit is tested with block 0's
geometry, before any ring rule, and as part of each candidate's geometry,
whose data area need not match block 0's, so every `FSIMG_ECORRUPT` of this section
describes a container whose areas are contiguous.

A descriptor or data block past the end of a
[truncated container](#truncated-containers) is not a slot that fails these
rules: its read fails the open.

### Truncated containers

A slice shorter than `N * B` opens by the
[common rule for a truncated volume](architecture.md#byte-and-object-boundaries).
The driver has no truncation check of its own. Every read goes through the
common read funnel, which fails a request past the slice with
`FSIMG_ECORRUPT`, "image is truncated", and volume information reports
`N * B` as the container's length, from which the reader core derives
`missing_bytes`. The rest follows:

* **Selection never passes over a missing block.** The ring pass reads all
  `D` descriptor blocks and the selected checkpoint's data run through the
  funnel, so a cut through either fails the open. Of the data run it reads
  the mapped objects and the run's highest block: a truncated image is cut
  at its tail, so that block is the first a cut through the run reaches. No candidate is judged
  "not whole" because its blocks are missing, which would silently select
  older state from a truncated image.
* **Nothing is sized from `N` while opening.** The mapping array is sized by
  mapping blocks already read, and the space manager object is read whole
  before its free count is used. The chunk-information blocks and bitmaps,
  whose number follows `N`, are read only by verification.
* **A missing tail nothing references does not stop reading.** Open, list
  and extract succeed when every object, node and extent they touch lies in
  the slice; one that does not fails where it is reached, and a partial
  extraction is not success.
* **Addresses are still judged against `N`.** An address at or past `N` is
  corrupt; an address below `N` and past the slice is truncation.
* **Full verification fails on every truncated container**, in the reader
  core, before the driver's verification runs, and wrap and compose refuse
  to package one.

These rules are fsimg policy. In E8 macOS opens none of three cut copies,
cut inside the descriptor ring, inside the selected data run and past both
checkpoint areas: the attached disk publishes no volume and `fsck_apfs`
fails to open it with `ERANGE`, which suggests, without a test of a short
tail cut, that macOS refuses any container shorter than its block count.
fsimg reads a container whose missing tail nothing it touches references,
and like macOS selects no older checkpoint from a cut image.

### Ephemeral objects and the space manager

A checkpoint-mapping block (kind 12, physical) holds u32 flags at offset 32,
a u32 mapping count at 36, and from 40 the mappings, 40 bytes each:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | Object kind and flags |
| 4 | 4 | Subtype |
| 8 | 4 | Object byte size |
| 12 | 4 | Reserved, zero |
| 16 | 8 | Owning volume superblock OID, zero for a container object |
| 24 | 8 | Ephemeral OID |
| 32 | 8 | Physical block |

The selected checkpoint's mappings are the only way to resolve an ephemeral
OID. Their number grows with the data run, since ephemeral free-queue trees
can have many nodes, so the driver keeps them sorted by OID in a paged array
in metadata scratch rather than resident memory. The space manager supplies
the container's free-block count for the volume information; its chunk
structures are read only by verification. Its layout:

| Offset | Size | Field |
| --- | --- | --- |
| 32 | 4 | Block size, equal to `B` |
| 36 | 4 | Blocks per chunk, `8 * B` |
| 40 | 4 | Chunks per chunk-information block |
| 44 | 4 | Chunk-information blocks per address block |
| 48 | 48 | Main device: block count, chunk count, chunk-information block count, address block count, free count, offset of its address array in this object |
| 96 | 48 | Tier-2 device: outside Fusion every count is zero and only its address offset is set, naming its empty array |
| 144 | 4 | Flags |
| 148 | 36 | Internal-pool transaction multiplier, block count, bitmap size and block count, bitmap base, pool base |
| 184 | 16 | Reserve block count and reserve allocation count |
| 200 | 120 | Three free queues (internal pool, main, tier 2): count, tree OID, oldest XID, node limit |
| 320 | 16 | Internal-pool bitmap ring: free head and tail, XID and bitmap offsets, free-next offset |
| 336 | 4 | Version |
| 340 | 4 | Structure size: 2520 in every E1 capture, the end of the data-zone field |
| 344 | 2176 | Data-zone allocation state: two devices of eight zones, 136 bytes each; all zero in every E1 capture |

The arrays follow the structure, at the offsets its fields name; in every
E1 and E12 dump they are packed in this order: the pool ring's XID array at
2520, its bitmap-index array right after it, its free-next array at the
next multiple of 8, the main device's address array right after that and
the tier-2 device's after it. With a one-block pool bitmap they lie at
2520, 2528, 2536 and 2568; with 97 bitmap blocks at 2520, 3296, 3496 and
6600 (apfs-newfs-140737488355328-e12-128t). The pool's bitmap
ring has `bitmap block count` slots of one bitmap each at the bitmap base.
The XID array holds `bitmap size` u64 values, the XID each current bitmap
was written in; the bitmap-index array holds `bitmap size` u16 slot indexes
naming the current bitmap's slots; and the free-next array holds one u16
per slot, linking the free slots from the free head, `0xffff` ending the
list and marking a slot in use. This reading of the arrays fits every E1
capture (apfs-newfs-4194304-plain: head 2, tail 0, slot 1 current, slot 0
last in the list) and E12's XID-2 state at `b = 97`, but [A1] does not
state it and no capture shows the ring after mount cycles move it, so it is
provisional: E8's runs did not record the pool ring, and an E8 mount-cycle
run that dumps it closes the question. Verification reads the current
bitmap through the index array, each index below the bitmap block count and
each XID not above the checkpoint's, or the container is `FSIMG_ECORRUPT`,
and checks nothing else of the ring; that stays the rule if the run is not
made.

Its counts are tied to the geometry, so no loop over them is bounded only by
the work budget. These relations come from [A1], and every E1 capture and
size-sweep dump satisfies them: the main device's
block count is `N` and its chunk count
`ceil(N / (8 * B))`; chunks per chunk-information block are
`floor((B - 40) / 32)` and chunk-information blocks per address block
`floor((B - 40) / 8)`; the chunk-information block count is
`ceil(chunks / chunks per block)`; the address-block count is zero when the
chunk-information block count is at most the chunk-information blocks per
address block, and otherwise
`ceil(chunk-information blocks / per address block)` (E12, bisected at
both block sizes `newfs_apfs` chooses: at 4096 bytes 507 chunk-information
blocks have no address block and 508 have two, at 16384 bytes 2043 none and
2044 two). At other block sizes the threshold is provisional, closed by the
same bisection with `newfs_apfs -b`; until then a container of such a block
size whose address-block count breaks the relation is
`FSIMG_EUNSUPPORTED` rather than corrupt. The arrays
lie inside the object, each at or past the structure's 2520 bytes, aligned
to its entry size, disjoint from the others and ending no later than the
packed order above ends for the same counts, and the object's mapped
size must be the smallest whole number of blocks holding the structure and
its arrays (two blocks in E12 from 3 to 7.8 TiB and from 50 TiB); the
reader checks it before reading the object's blocks. A resize keeps the
packing: it writes a new space manager whose counts, pool, ring and array
offsets are those a fresh format of the new block count has, and `S`
follows them (E18, every state of its four captures; in
apfs-diskutil-9895604649984-e18-large `S` goes from 2 to 1 and back to 2
as the container grows past 507 chunk-information blocks and shrinks
below them). Whether an aged container moves an array past the packing
is provisional, closed by E16's used volume. Until then, and if E16 does
not answer, a space manager with an array past the packing
is `FSIMG_EUNSUPPORTED`, so no block past its first is read either way.
The pool's bitmap
size in blocks is `ceil(P / (8 * B))` for a pool of `P` blocks, its ring
holds at least that many slots and fewer than `0xffff`, and the tier-2
device's counts are zero outside Fusion. Any other value is corrupt.

A chunk-information block (kind 7) holds an index, a count and 32-byte
records: last-modifying XID, first block, block count, free count and bitmap
block. A chunk-information address block (kind 6) holds an index, a count and
block numbers of chunk-information blocks. The main device's address array
names chunk-information blocks when its address-block count is zero and
address blocks otherwise. The `i`th chunk-information block of the device
carries index `i` and holds `min(K, chunks - i * K)` records, `K` being the
chunks per block, and the `i`th address block likewise; since a block's
index is its position, no block is listed twice. Chunk `k` starts at block
`8 * B * k`, covers `min(8 * B, N - 8 * B * k)` blocks, and has a free
count not above them and an XID not above the checkpoint's. Each bitmap block covers one chunk, bit `i` of
byte `j` (least significant first) standing for block `8j + i` of the chunk.
The internal pool is a block range reserved as a whole in the main bitmaps,
from which the space manager allocates its own blocks, tracked by the pool's
bitmap ring. In every E1 capture the pool holds the chunk-information
blocks, the bitmap blocks and the blocks of the internal-pool free queue,
and the main bitmaps hold no space-manager block outside it. Address
blocks live in the pool too. The native formatter places each before the
chunk-information blocks it names (E12, apfs-newfs-140737488355328-e12-128t),
but a later transaction rewrites them anywhere in the pool, so the order is
no reader rule; the [writer values](#writer-values) hold the placement.

The **fixed regions** of a container are the block ranges its superblock
and space manager state by position and size: block 0, the checkpoint
descriptor area, the checkpoint data area, the internal pool, and the
pool's bitmap ring, which lies at the bitmap base, outside the pool. This
list is defined once. Verification claims exactly these ranges, from the
fields it decoded, and the writer lays out exactly these ranges, from the
fields it is about to encode, so the two cannot disagree about which
blocks are fixed.

A chunk with no allocated block has no bitmap block, stored as a zero
bitmap address (E1: 61 of the 64 chunks of apfs-newfs-8589934592-plain).
A zero address with a free count equal to the chunk's block count is an
entirely free chunk and any other zero address is corrupt. How the native
formatter sizes the internal pool and the checkpoint areas is a function of
`N` that [writer values](#writer-values) states (E1). The native formatter
leaves the bits past `N` in the last chunk's bitmap clear (E1,
apfs-newfs-4194304-plain). `fsck_apfs -n` accepts them set and leaves
them out of the free counts (E15, `e15-pastn-all`), so verification does
not check them; `apfsck` refuses them with `Chunk-info: wrong count of
free blocks`.

### Free queues

The space manager names three free queues: the internal pool's, the main
device's and tier 2's. Each is a 40-byte entry: a u64 count of queued
blocks, the OID of its tree, the oldest XID in it, a u16 node limit, and
reserved bytes. The tier-2 entry is all zero outside Fusion.

A queue's tree is an ephemeral B-tree of subtype 9 with fixed 16-byte keys,
`(XID u64, first block i64)`, ordered by XID and then block, and an 8-byte
value, the run's block count. A ghost record has no value and stands for one
block. Its nodes are ephemeral objects in the checkpoint data area, found
through the checkpoint's mappings like the space manager itself.

An empty queue has two legal forms, and the reader accepts both: a zero tree
OID with a zero count, and a tree whose root is a leaf with no keys. In
every queue the count equals the sum of its records' runs, and each run lies
inside its domain: `[1, N)` for the main queue, the internal pool's range
for the pool's. A queued block is still allocated in its domain's bitmap
until the queue releases it.

The native formatter commits two checkpoints (E1). At XID 1 both queues
are empty in the zero-OID form, oldest XID zero, and the whole of
apfs-newfs-372736-plain is in that state. At XID 2, the state a fresh
container is left in, neither queue is empty: each is a root leaf holding
the blocks the second transaction freed, with oldest XID 2. The internal
pool's queue holds one record of two blocks in the captures up to 1 GiB and
one ghost record in those of 8 GiB and 100 GiB; the main queue holds one
record of two blocks (E1, apfs-newfs-33554432-plain and
apfs-newfs-8589934592-plain). A free-queue root leaf with no keys appears in no
capture. The node limits are the functions of `N` that
[writer values](#writer-values) gives. In a native queue with entries the oldest XID is the smallest record
XID, and the node limit keeps the value the formatter wrote (E8); a
resize writes the formatter's value for the new block count (E18). The
reader requires only an oldest XID not above the checkpoint's and does
not interpret the node limit.

### Reaper

The reaper (kind 17, ephemeral) holds the state of deletions too large for
one transaction, such as a deleted volume's trees:

| Offset | Size | Field |
| --- | --- | --- |
| 32 | 8 | Next reap ID |
| 40 | 8 | Completed reap ID |
| 48 | 8 | Head of the reap list: the OID of a reap-list object (kind 18, ephemeral), zero when there is none |
| 56 | 8 | Tail of the reap list |
| 64 | 4 | Flags: `0x1` reserved and always set, `0x2` an object is being reaped |
| 68 | 4 | Reap-list object count |
| 72 | 4 | Kind of the object being reaped |
| 76 | 4 | Its size |
| 80 | 8 | OID of the volume that owned it |
| 88 | 8 | Its OID |
| 96 | 8 | Its XID |
| 104 | 4 | Flags of its reap-list entry |
| 108 | 4 | State buffer size |
| 112 | | State buffer |

A reap-list object (kind 18, ephemeral) is one link of the list:

| Offset | Size | Field |
| --- | --- | --- |
| 32 | 8 | OID of the next reap-list object, zero in the last |
| 40 | 4 | Flags |
| 44 | 4 | Entry capacity |
| 48 | 4 | Entry count |
| 52 | 4 | Index of the first entry, `0xffffffff` when there is none |
| 56 | 4 | Index of the last entry, likewise |
| 60 | 4 | Index of the first free entry |
| 64 | 40 each | Entries: next index (u32), flags (u32), kind (u32), size (u32), OID of the owning volume, OID and XID (u64 each) |

The reap list is **well formed** in exactly one of two shapes: head, tail
and reap-list object count all zero; or reading from the head yields
exactly *count* objects, *count* being the reap-list object count at 68,
each resolving as a reap-list object of the selected checkpoint whose
entry count is at most its capacity, the last one's next OID zero and its
OID equal to the tail. Any other shape, including a list that ends early,
runs past *count* or names an object of another kind, is
`FSIMG_ECORRUPT`. The reaper is **idle** when flag `0x2` is clear and
every object of its well-formed list has an entry count of zero; a list
with entries is pending work. Entries off the list keep stale contents
and are not read. A reap-list object is an ephemeral object
of the selected checkpoint whose blocks the checkpoint data area's claim
covers, so an empty list adds no claim. The definition does not constrain
the six fields describing an object being reaped: they mean nothing while
the flag is clear, and after a reap they keep the last completion record
(kind 13, OID 0, XID 1, entry flags `0xf` in E17).

Native containers hold both shapes. Every E10 and E17 container before a
deletion has the zero shape with next reap ID 1 and completed reap ID 0;
after a completed reap the reaper keeps one empty reap-list object as head
and tail, object count 1, and a completed reap ID one below the next (E17,
apfs-diskutil-2147483648-e17-clean and apfs-diskutil-2147483648-e17-force;
E10, apfs-diskutil-2147483648-e10-hole). Whether a container in daily use
holds another idle shape is provisional (E16), and this definition is the
default if E16 cannot answer. The state buffer must lie inside the
object, 112 plus its size at most the object's size, or the reaper is
`FSIMG_ECORRUPT`.
Every fresh native container holds an idle reaper with head and tail zero,
flags `0x1`, next reap ID 1, completed reap ID 0, the six fields of an
object being reaped zero, and a state buffer of `B - 112` bytes, 3984, all
zero (E1, every capture); the reader does not constrain the reap IDs or the
state buffer size.

While the reaper works, its state buffer starts with the reap state of
[A1]: the last block reaped (u64), the XID of the snapshot being reaped
(u64) and the phase (u32; 2 reaps the live tree), packed.

Reading never depends on the reaper: a volume being deleted has a zero
slot in the volume array, so no selection reaches it. Verification of a
container whose reaper is not idle is incomplete with
`FSIMG_EUNSUPPORTED`, naming the pending reap. While a volume is reaped,
the container object map still maps its superblock, its trees stay
reachable, and they still reference blocks the reaper has already freed:
each reaping transaction queues what it freed in the main free queue at
its own XID, and earlier transactions' queues drain into the bitmap. An
allocation check that followed those trees would find blocks claimed but
free, and one that skipped them would not know which allocated blocks the
reap still owns. `fsck_apfs -n` reports such a container as not verified
completely, listing blocks the half-deleted volume references that the
bitmap shows free (E17, the during-003 and during-005 copies of
apfs-diskutil-2147483648-e17-clean). A deleted snapshot's reap is
unobserved, and the same rule applies to it. In E17's containers, a
2 GiB container whose deleted volume held up to 100000 files of at most
16 KiB, the reap finished within about three seconds and before
`diskutil apfs deleteVolume` returned.

## Object maps

An object-map object (kind 11, physical) holds:

| Offset | Size | Field |
| --- | --- | --- |
| 32 | 4 | Flags |
| 36 | 4 | Snapshot count |
| 40 | 4 | Tree type (kind and storage class of its tree) |
| 44 | 4 | Snapshot tree type |
| 48 | 8 | Tree OID |
| 56 | 8 | Snapshot tree OID |
| 64 | 8 | Most recent snapshot XID |
| 72 | 16 | Pending revert range |

Its tree is physical, with fixed 16-byte keys `(OID, XID)` and 16-byte values
`(flags u32, size u32, address i64)`, ordered by OID then XID numerically.
Value flags: deleted `0x1`, saved `0x2`, encrypted `0x4`, no-header `0x8`,
crypto generation `0x10`.

Resolving virtual OID `o` at XID `T` seeks the greatest key not above
`(o, T)`. The lookup misses when no such key has OID `o`, or when the found
value is deleted; it never falls through to an older entry. A found value
must have a size that is a positive multiple of `B`, an address range inside
`[1, N)`, and no flag outside `{saved, no-header}`. Encrypted is
unsupported; it marks the filesystem-tree nodes of an encrypted volume
(E11, every encrypted capture).

No-header is required on every filesystem-tree node of a
[sealed volume](#sealed-volumes) and forbidden on every other object, the
sealed volume's integrity object included (E11: 316 and 6196 node
mappings with `0x8` alone, the integrity object's with no flag, in the two
captured cryptexes). The reference being resolved says which it expects,
so a flag that disagrees is `FSIMG_ECORRUPT`. A headerless node has no
header to check; its parent's digest identifies it instead, which is why
only an intact seal is read.

An object with a header, read at the address, must satisfy the header
rules with its OID equal to `o` and its XID equal to the found key's XID (E1 for the one record of every capture with a volume,
E3 for its 258 filesystem-tree mappings, and E8 for the volume superblock
across mount cycles; if a capture shows an object older than its key, the
rule becomes "not above the key's XID"). A miss for
a reference the format requires is `FSIMG_ECORRUPT`.

Object-map flags follow a closed table:

| Flag | Container object map | Volume object map |
| --- | --- | --- |
| `0x1` manually managed (no snapshot support) | Required | Forbidden |
| `0x2` encrypting, `0x4` decrypting, `0x8` key rolling | Unsupported | Unsupported |
| `0x10` crypto generation | Accepted; clear in every E1 capture | Accepted; clear in every E1 capture |
| Any other bit | Unsupported | Unsupported |

The container map's flags are `0x1` alone in every E1 capture, and the
volume map's are zero, the crypto-generation flag clear. That flag
changes no lookup, so it is accepted and ignored. The snapshot count and the most recent snapshot XID must
agree with the volume's snapshot tree, and the container map has no
snapshots. A nonzero pending revert range is unsupported. The flags the
writer emits are a [writer value](#writer-values).

The container object map resolves volume superblocks; a volume's object map
resolves that volume's filesystem-tree nodes and, where present, its other
virtual objects. A reader never resolves a volume object through the
container map or the reverse.

## B-trees

### Node layout

A node is one object of `B` bytes, kind 2 for the root and 3 for any other
node, with the tree's subtype and storage class:

| Offset | Size | Field |
| --- | --- | --- |
| 32 | 2 | Node flags: root `0x1`, leaf `0x2`, fixed key and value size `0x4`, hashed `0x8`, no header `0x10`, transient `0x8000`, which [A1] says never appears on disk |
| 34 | 2 | Level; leaves are level 0 |
| 36 | 4 | Key count |
| 40 | 4 | Table of contents: offset and length (u16 each), relative to offset 56 |
| 44 | 4 | Free space: offset and length, relative to the key area |
| 48 | 4 | Key free list: offset and length |
| 52 | 4 | Value free list: offset and length |
| 56 | | Table of contents, key area, free space, value area |

A root node ends with a 40-byte information footer: tree flags, node size,
fixed key size and fixed value size (u32 each), the longest key and value
(u32 each), the key count and the node count (u64 each). The tree flags are:
64-bit keys `0x1`, sequential insert `0x2`, ghosts allowed `0x4`, ephemeral
`0x8`, physical `0x10`, nonpersistent `0x20`, keys and values not aligned
`0x40`, hashed `0x80`, no header `0x100`.

The table of contents holds one entry per key. With fixed sizes an entry is
two u16 offsets, key then value; otherwise it is four u16 fields, key offset
and length then value offset and length. A key offset is relative to the
start of the key area, which follows the table of contents. A value offset
counts backward from the end of the value area, which is the end of the node,
or the start of the footer in a root node: the value occupies
`[end - offset, end - offset + length)`. In a fixed-size tree, leaf values
have the footer's fixed value size and index values are 8-byte child OIDs.
A hashed index value is the child's OID minus the root node's OID (u64),
then the child's digest at the hash type's length: 40 bytes for SHA-256,
not [A1]'s 64-byte maximum (E11: the root of one captured cryptex, OID
1378, names its first child 57, which the map resolves as OID 1435). The
base is provisional, with no planned experiment: in both captured trees
the root has the lowest OID, so the root's OID and the tree's lowest OID
read the same, and a tree whose root is not its lowest OID would close
it. The reader uses the root's OID; a wrong base fails the child's digest
check as `FSIMG_ECORRUPT`, never a silent misread, because only an intact
seal is read.

A value offset of `0xffff` is a ghost, legal only in a tree whose flags
allow ghosts.
An empty key or value free list has offset `0xffff` and length zero [A1].

### Validation

Every node a reader visits passes these checks before any entry is used:

* header and checksum; a headerless node of a sealed tree instead matches
  its parent's digest, or the root the integrity object's root hash;
* node flags consistent with its position: root exactly at the tree's top,
  leaf exactly at level 0, fixed-size, hashed and no-header flags equal to
  the tree's, the root's included (E11: root `0x19`, index `0x18`, leaf
  `0x1a` in a tree whose flags are `0x1c2`), neither the transient flag nor
  an unknown one;
* a root's footer node size equal to `B`, its tree flags known and consistent
  with the tree's storage class, and fixed sizes equal to the tree's codec;
* level equal to the parent's level minus one, and the root's level at most
  15 (16 levels), fsimg's bound on height, beyond which a tree is
  `FSIMG_EUNSUPPORTED`;
* a key count of at least one in every non-root node;
* table of contents inside the node data with length at least the key count
  times the entry size; every key range inside the key area and before the
  free space, every value range after the free space and inside the value
  area, offsets aligned to 8 bytes unless the tree says not aligned;
* every key length within its record codec's minimum and maximum, and every
  value length within the tree's; in the filesystem tree each key is also
  decoded by its kind's codec, and a value is judged by the record decoder
  that uses it, so a damaged value costs its record and not its node;
* keys strictly increasing under the tree's comparator; the first key of a
  child not below the separator naming it, and every key of a child below the
  next separator in the parent. Keys whose tie-break is undecided
  ([comparators](#comparators)) may instead be equal under the comparator,
  in either order and across nodes, a child's last key then equal to the
  next separator; equal keys of such a class are their consumer's to refuse.

A node's address is never revisited on one root-to-leaf path, and the height
bound and the work charge of each load bound a seek. A walk from one seek
loads at most as many nodes as the slice has blocks, plus the height, since
a valid tree's walk loads each node once; more is `FSIMG_ECORRUPT`.
The two failures differ in kind. A repeated address is `FSIMG_ECORRUPT`: no
valid tree has one. A height above 16 levels is `FSIMG_EUNSUPPORTED`: such a
tree can be valid, and the bound is fsimg's. They cannot be confused,
because levels fall by one per step: a cycle shows as a level mismatch or a
repeated address within 16 steps, never as excess height.
The footer's longest-key, longest-value and count fields are statistics: they
are never used to size a buffer, and full verification checks the counts.
The longest key and value are not checked: a native footer can state more
than every live record holds (E3: longest value 924 bytes in
`apfs-newfs-268435456-e3-ci`, whose largest live value is 912).

### Comparators

| Tree | Key order |
| --- | --- |
| Object map | OID, then XID |
| Filesystem | Owner (low 60 bits), then kind (top 4 bits), then the kind's suffix: hashed directory record by its 22-bit hash, then name bytes; plain directory record, extended attribute and snapshot name by name bytes; file extent by logical offset; sibling link by sibling ID; file information by its packed word |
| Extent reference | Block, then kind |
| Snapshot metadata | XID, then kind, then name bytes for names |
| Free queue | XID, then block |
| Sealed file extents | Private ID, then logical offset |

Integer fields compare numerically, never as serialized bytes. Hashed
directory records order by the 22-bit hash alone, not by the 32-bit word
that also holds the length; records of equal hash order by their stored
name bytes as unsigned bytes, a name that is a prefix of another first:
a bytewise comparison of the names with their terminating NULs (E4: the 34
and 29 equal-hash pairs of captures `apfs-newfs-268435456-e4-names-ci` and
`-cs`, and `apfs-newfs-33554432-e4-prefix-ci` and `-cs`). Attribute names
of different lengths compare the same way, as their bytes with the
terminating NUL, so a proper prefix sorts first (E5, `e5-xattr`). How
snapshot names of different lengths compare is provisional (E9). No
experiment can show how plain directory-record names compare, since no
native tool formats a plain-key volume (E2), so their tie-break is
permanently undecided and takes the lenient check below. The reader's
ordering checks use the observed rules, so a wrong one would make valid
images fail rather than misread. If an
experiment leaves a tie-break undecided, the ordering check accepts either
order of the keys it concerns, and a lookup scans the records that share
the decided part of the key instead of seeking among them.

## Volumes

### Volume superblock

A volume superblock is `APSB` (`0x42535041`) at offset 32 of a virtual object
of kind 13, resolved through the container object map:

| Offset | Size | Field |
| --- | --- | --- |
| 36 | 4 | Index in the container's volume array |
| 40 | 8 | Optional features |
| 48 | 8 | Read-only-compatible features |
| 56 | 8 | Incompatible features |
| 64 | 8 | Last unmount time |
| 72 | 8 | Reserve block count |
| 80 | 8 | Quota block count |
| 88 | 8 | Allocated block count |
| 96 | 20 | Metadata crypto state |
| 116 | 12 | Root tree, extent-reference tree and snapshot-tree types |
| 128 | 32 | Object map OID (physical), root tree OID (virtual), extent-reference tree OID and snapshot tree OID (physical) |
| 160 | 16 | Revert-to XID and superblock OID |
| 176 | 8 | Next object identifier |
| 184 | 56 | Counts of files, directories, symbolic links, other objects, snapshots; blocks allocated and freed |
| 240 | 16 | Volume UUID |
| 256 | 8 | Last modification time |
| 264 | 8 | Volume flags |
| 272 | 48 | Formatting software: 32-byte identifier, time, last XID |
| 320 | 384 | Eight modification records of the same shape |
| 704 | 256 | Volume name, NUL-terminated UTF-8 |
| 960 | 4 | Next document ID |
| 964 | 2 | Role |
| 966 | 2 | Reserved |
| 968 | 8 | Root-to XID |
| 976 | 8 | Encryption-rolling state OID |
| 984 | 16 | Clone-information epoch and XID |
| 1000 | 8 | Snapshot extended-metadata OID |
| 1008 | 16 | Volume-group UUID |
| 1024 | 8 | Integrity metadata OID |
| 1032 | 12 | Sealed file-extent tree OID and type |

The superblock's index equals its slot in the container array, its UUID is
not all zero (verification checks that it differs from every other
volume's), its name is valid UTF-8
of at most 255 bytes with a terminator inside the field, and its tree types
are the ones [A1] gives for each tree: the root tree virtual, the others
physical. A sealed volume follows the same rule: its filesystem tree is
virtual and its sealed file-extent tree physical (E11: `0x00000002` and
`0x40000002` in both captured cryptexes). A nonzero revert-to XID or superblock
OID, a nonzero encryption-rolling state, or a nonzero root-to XID is a
transition native tools complete before use and is `FSIMG_EUNSUPPORTED`.

| Mask | Bits | Reader | Writer |
| --- | --- | --- | --- |
| Incompatible | `0x1` case-insensitive, `0x8` normalization-insensitive | Accepted; select the name rules | Case-insensitive: `0x1`; case-sensitive: `0x8`, as `newfs_apfs` and `hdiutil` set them (E2) |
| Incompatible | `0x2` dataless snapshots | Accepted for the live view; a snapshot view of such a volume is unsupported | Never |
| Incompatible | `0x4` encryption rolled | Accepted only with the unencrypted flag | Never |
| Incompatible | `0x10` incomplete restore | Unsupported | Never |
| Incompatible | `0x20` sealed | The sealed profile | Never |
| Incompatible | Any other bit | Unsupported | Never |
| Read-only compatible | None defined | Any bit accepted for reading | Zero |
| Optional | `0x1`, `0x4` defragmentation, `0x2` hard-link map records, `0x8` strict access time, `0x10` volume-group inode space | Accepted | `0x2`, the native default of `newfs_apfs` and `hdiutil` in both personalities (E2) |
| Optional | Any other bit | Accepted, ignored | Never |
| Flags | `0x1` unencrypted | Required; without it the volume is encrypted and unsupported | Emitted |
| Flags | `0x8` one key, `0x10` spilled over, `0x20` spillover cleaner | One key: encrypted, unsupported; spillover: Fusion, unsupported | Never |
| Flags | `0x40` always check extent references | Accepted | Never |
| Flags | Any other bit | Unsupported | Never |

### Volume selection

A container's volumes are the nonzero slots of its volume array, each
resolved at the read XID. The read setting `volume=` (`volume_index` in the
read options) names one by its slot index, which the volume superblock also
stores; unlike `diskutil` numbering, this index is a stored fact. Without the
setting, a container with exactly one volume opens it, and a container with
several fails with `FSIMG_EINVAL`, whose message gives the count and the
lowest and highest occupied indexes but no name. A named slot that is empty, or an index at or
past the maximum volume count, is also `FSIMG_EINVAL`. Information for a
selected volume reports its index, the container's volume count and its
role, so `i -o volume=N` enumerates a container's volumes one by one.

Only the selected volume's superblock is required to resolve for reading.
Verification resolves every volume, because allocation is container-wide.

Roles are reported as numbers, never acted on: a role is not a mount path,
and a container need not hold a bootable set. Any value is accepted. The
values [A1] names, with the ones `diskutil apfs addVolume -role` stores
(E10, apfs-diskutil-8589934592-e10-allroles):

| Role | [A1] name | `diskutil` |
| --- | --- | --- |
| 0 | None | none or `0` |
| 1 | System | `S` |
| 2 | User | |
| 4 | Recovery | `R` |
| 8 | VM | `V` |
| 16 | Preboot | `B` |
| 32 | Installer | `I` |
| 64 (1 << 6) | Data | `D` |
| 128 (2 << 6) | Baseband | |
| 192 (3 << 6) | Update | `E` |
| 256 (4 << 6) | xART | |
| 320 (5 << 6) | Hardware | `H` |
| 384 (6 << 6) | Backup | `T` |
| 448 (7 << 6) | Sidecar (heading Reserved 7) | `C` |
| 512 (8 << 6) | Reserved 8 | |
| 576 (9 << 6) | Enterprise | |
| 640 (10 << 6) | Reserved 10 | |
| 704 (11 << 6) | Prelogin | |

A System and a Data volume
sharing a volume-group UUID are separate volumes; the reader presents each
on its own namespace and does not follow firmlinks. When `diskutil` adds
a System volume with `-groupWith` an existing Data volume, both get Data's
volume UUID as their group UUID and optional feature `0x10` (E10).

## Filesystem records

### Key families

| Kind | Record | Key suffix | Reader |
| --- | --- | --- | --- |
| 1 | Snapshot metadata | none | Snapshot tree only |
| 2 | Physical extent | none | Extent-reference tree only |
| 3 | Inode | none | Decoded |
| 4 | Extended attribute | u16 name length, NUL-terminated name | Decoded |
| 5 | Sibling link | u64 sibling ID | Validated |
| 6 | Data-stream ID | none | Validated |
| 7 | Crypto state | none | Unsupported in an unencrypted volume |
| 8 | File extent | u64 logical byte offset | Decoded |
| 9 | Directory record | plain or hashed name | Decoded |
| 10 | Directory statistics | none | Validated |
| 11 | Snapshot name | u16 name length, name | Snapshot tree only |
| 12 | Sibling map | none | Validated |
| 13 | File information | u64 packed type and offset | Sealed profile only |
| 0, 14, 15 | Invalid | | `FSIMG_ECORRUPT` |

A record of a kind outside its tree's table is corrupt. Every key and value
length is checked against its record's fixed part before a field is read, and
every variable part is checked against the record's end.

### Inodes

An inode record's key owner is its inode number; its value has a 92-byte
fixed part:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | Parent directory inode |
| 8 | 8 | Private ID of its data stream |
| 16 | 8 | Creation time |
| 24 | 8 | Modification time |
| 32 | 8 | Change time |
| 40 | 8 | Access time |
| 48 | 8 | Internal flags |
| 56 | 4 | Child count (directory) or link count (otherwise), signed |
| 60 | 4 | Protection class |
| 64 | 4 | Write generation |
| 68 | 4 | BSD flags |
| 72 | 4 | Owner |
| 76 | 4 | Group |
| 80 | 2 | Mode, POSIX type and permission bits |
| 82 | 2 | Reserved |
| 84 | 8 | Uncompressed size |
| 92 | | Extended fields |

Inode numbers 0 and 1 are never inodes: 1 is the parent of the root. Inode 2
is the root directory and 3 the private directory, both directories with
parent 1. Numbers below 16 are reserved, among them 6 and 7 for the snapshot
and purgeable directories, and no directory record names one other than
the root and private-directory records under parent 1 (E3: in
`apfs-newfs-268435456-e3-ci` no other record does, and the first inode the
system creates, `.fseventsd`, is 16). Ordinary inodes are at least 16, or at least
`0x0800000000000000` in a System volume whose volume-group inode space is
enabled. The internal flags are accepted from the closed table below,
[A1]'s valid flags, which hold every value the native captures use (E2, E3,
E5, E6, E7); any other bit, `0x20` (`FLAG_UNUSED`, outside [A1]'s valid mask)
among them, is unsupported. No flag of the table changes where the reader
finds content: the data stream, the file extents and the compression
attribute determine it, and nothing past the size is delivered.

| Flag | [A1] name | Native use (E2, E3, E5, E6, E7) |
| --- | --- | --- |
| `0x1` | `IS_APFS_PRIVATE` | |
| `0x2` | `MAINTAIN_DIR_STATS` | |
| `0x4` | `DIR_STATS_ORIGIN` | |
| `0x8` | `PROT_CLASS_EXPLICIT` | |
| `0x10` | `WAS_CLONED` | A clone and its source (E6) |
| `0x40` | `HAS_SECURITY_EA` | `.fseventsd`, with its `com.apple.system.Security` attribute |
| `0x80` | `BEING_TRUNCATED` | |
| `0x100` | `HAS_FINDER_INFO` | |
| `0x200` | `IS_SPARSE` | A file with a hole, with the sparse-bytes field (E3, E6) |
| `0x400` | `WAS_EVER_CLONED` | A clone and its source (E6) |
| `0x800` | `ACTIVE_FILE_TRIMMED` | |
| `0x1000` | `PINNED_TO_MAIN` | |
| `0x2000` | `PINNED_TO_TIER2` | |
| `0x4000` | `HAS_RSRC_FORK` | A compressed file whose resource-fork attribute holds bytes (E7) |
| `0x8000` | `NO_RSRC_FORK` | Every new inode of each kind E3 creates, the root and private directories included; a compressed file without a resource-fork attribute (E7) |
| `0x10000` | `ALLOCATION_SPILLEDOVER` | |
| `0x20000` | `FAST_PROMOTE` | |
| `0x40000` | `HAS_UNCOMPRESSED_SIZE` | Every compressed file, with the uncompressed-size field (E7) |
| `0x80000` | `IS_PURGEABLE` | |
| `0x100000` | `WANTS_TO_BE_PURGEABLE` | |
| `0x200000` | `IS_SYNC_ROOT` | |
| `0x400000` | `SNAPSHOT_COW_EXEMPTION` | |

Neither clone flag proves that the stream is shared: was cloned stays on an
inode while it keeps the stream it had when cloned, the source's included
(E6).

BSD flags are stored as `chflags(2)` sets them: E3 shows `0x1` `nodump`,
`0x2` `uchg` and `0x8000` `hidden` set by an unprivileged user, and zero on
every other entry. A new FIFO or socket has protection class 4, and every
other new inode class 0 (E3).

| Mode type | Directory record type | Entry kind | Content |
| --- | --- | --- | --- |
| `0100000` | 8 | `FSIMG_ENTRY_FILE` | Data stream, or decompressed data when compressed |
| `0040000` | 4 | `FSIMG_ENTRY_DIRECTORY` | Directory records |
| `0120000` | 10 | `FSIMG_ENTRY_SYMLINK` | Target from the symbolic-link attribute |
| `0010000` | 1 | `FSIMG_ENTRY_FIFO` | None |
| `0020000`, `0060000`, `0140000` | 2, 6, 12 | `FSIMG_ENTRY_CHAR_DEVICE`, `FSIMG_ENTRY_BLOCK_DEVICE`, `FSIMG_ENTRY_SOCKET` | None; read-only kinds of the [device node design](design-device-nodes.md#readers). A device's number is the [device number extended field](#extended-fields) when present |
| other | | | `FSIMG_ECORRUPT` |

A directory record's type must equal its inode's mode type. Entry metadata
sets `FSIMG_META_OWNER`, `FSIMG_META_MODE` with the low twelve mode bits, and
all four timestamps. A time counts nanoseconds since the Unix epoch: seconds
are the floor of its quotient by `10^9`, nanoseconds the remainder, the
basis is UTC and the precision one nanosecond. The reference declares the
fields unsigned, but macOS stores a time before 1970 as a two's-complement
value and clamps every time it is given to `-2^63` through `2^63 - 1`
nanoseconds (E3, E14: `apfs-newfs-67108864-e14-ci`), so reader and
[writer](#writer-values) treat the fields as signed 64-bit values.
Attributes are absent. BSD flags, the
protection class and the remaining fields have no common representation and
are not reported; the `UF_COMPRESSED` flag `0x20` selects
[compression](#transparent-compression), and an inode with the dataless flag
`SF_DATALESS` `0x40000000`, whose content is not on the volume, is
unsupported, on every kind, since a dataless directory would otherwise list
as empty.

### Extended fields

Inode and directory-record values may end in an extended-field blob: a u16
field count, a u16 count of used data bytes, then four bytes per field (type
u8, flags u8, data size u16), then the field data, each field's data
starting at a multiple of 8 bytes from the start of the data area. Counts,
sizes and padding must fit the record exactly; the used-data count must
equal the padded sum. A field type appears at most once. The meaning of a
type depends on the record kind. Inode fields the reader decodes: the data
stream (type 8, 40 bytes), the name (type 4, NUL-terminated UTF-8), the
sparse byte count (type 13, 8 bytes) and the device number (type 14, 4
bytes: the macOS `dev_t`, bits 24 to 31 `dev_major` and bits 0 to 23 `dev_minor`,
valid only on a character or block device, `FSIMG_ECORRUPT` on another
kind, and a device without it has no number). A directory
record's sibling ID is type 1. Other known types are validated by size and
ignored, at the sizes [A1] gives in the table below; an unknown type with a
valid size is ignored when its flags do not mark it data-dependent, and is
unsupported otherwise. Padding bytes are zero.

| Inode field type | Content | Data size | Flags native volumes give it (E2, E3) |
| --- | --- | --- | --- |
| 1 | Snapshot XID | 8 | |
| 2 | Delta-tree OID | 8 | |
| 3 | Document ID | 4 | |
| 4 | Name | NUL-terminated, 2 or more | `0x2` do-not-copy, on every inode |
| 5 | Previous size | 8 | |
| 6, 9, 12 | Reserved | Any | |
| 7 | Finder information | 32 | |
| 8 | Data stream | 40 | `0x20` system field, on every file with content |
| 10 | Directory statistics | 32 | |
| 11 | File-system UUID | 16 | |
| 13 | Sparse bytes | 8 | `0x28` system field and children-inherit, on a file with a hole |
| 14 | Device number | 4 | |
| 15 | Purgeable flags | Any | |
| 16 | Original sync-root inode | 8 | |

The field flags are [A1]'s: `0x1` data-dependent, `0x2` do-not-copy, `0x8`
children-inherit, `0x10` user field, `0x20` system field, and `0x4`,
`0x40` and `0x80` reserved. In the E3 captures a file of size zero carries
no data-stream field and no directory record carries extended fields; E3
makes no hard links, whose records carry the sibling ID.

### Directory records and names

A directory record's owner is its parent directory's inode. Its key suffix is
either plain, a u16 length and the name with its NUL, or hashed, a u32 word
whose low 10 bits are the name's length with its NUL and whose high 22 bits
are the name hash, then the name with its NUL. A volume that is
case-insensitive or normalization-insensitive uses hashed keys and others
plain keys; a record of the other variant is corrupt. E2 shows hashed keys
on both personalities from `newfs_apfs` and `hdiutil`; neither tool formats
a volume without one of the two flags, so the plain variant rests on [A1]
alone. The
value is the child inode (u64), the time the entry was added (u64), u16
flags whose low four bits are the type, and extended fields.

A name is the stored bytes without the NUL: nonempty, valid UTF-8 (macOS
refuses any other byte string with `EILSEQ` on both personalities, E14),
without NUL or `/`, and neither `.` nor `..`; anything else is corrupt.
macOS also refuses some valid UTF-8 with `EILSEQ`: on both personalities
a name holding a noncharacter, such as U+FFFF (E14), a code point
unassigned in Unicode 17.0.0 or a character Unicode 18.0.0 adds (E4), while
private-use code points are accepted (E4). The writer refuses a name holding
a noncharacter or a code point unassigned in Unicode 17.0.0, the driver's
version, as macOS does. The reader accepts every valid UTF-8 name whatever
code points it holds, and classifies a stored hash that does not match the
name's form by the name: when the name holds such a code point the
mismatch is `FSIMG_EUNSUPPORTED`, since a newer macOS may hash it with newer
tables, and otherwise it is `FSIMG_ECORRUPT`. The reader
declares no [omitted-names profile](design-directories.md#omitted-names):
every valid name is delivered, including names with control characters such
as Finder's `Icon\r`, and the CLI escapes them for display as it does every
name.

**Name length.** There are two limits on the stored name without its NUL,
and no other passage states a third:

* **The reader accepts up to 1022 UTF-8 bytes.** A hashed key's 10-bit
  length field holds at most 1023 with the NUL. A plain key's u16 length
  can state more, and a longer plain name is `FSIMG_EUNSUPPORTED`.
* **The writer accepts up to 255 UTF-16 code units**, the limit macOS
  applies when it creates a name: a character above U+FFFF counts two, any
  other one. The longest such name is 765 bytes, 255 three-byte
  characters, inside the reader's limit (E14, on both personalities:
  `apfs-newfs-67108864-e14-ci` and `-cs`). The writer counts the name as
  given and also requires its NFD to be at most 255 units. Whether macOS
  counts the name as given or its NFD is provisional: E14's characters have
  no decomposition and E4 did not probe length, so an E14 run with names of
  decomposing characters at the limit closes it, and requiring both forms,
  which satisfies either rule, stays if that run is not made.

The comparison form of a name depends on the volume:

* case-insensitive: the canonical caseless form, the canonical
  decomposition (NFD) of the case folding of the name's NFD;
* case-sensitive and normalization-insensitive: the NFD of the name;
* otherwise: the name's bytes.

macOS 26.6.2 applies Unicode 17.0.0 to these forms (E4). Case folding is
full folding, the `C` and `F` entries of `CaseFolding.txt` without the
Turkic `T` entries: `ß`, `ẞ` and `ss` are one name, `İ` (U+0130) folds to
`i` followed by U+0307 and so equals `i̇` but not `i`, and `ı` (U+0131) is
a name of its own. Final and medial sigma fold together, U+0345 folds to
`ι`, Georgian Mtavruli folds to Mkhedruli, and Hangul syllables decompose
algorithmically, so a precomposed syllable equals its conjoining jamo. A
CJK compatibility ideograph with a canonical decomposition, a singleton,
equals its unified ideograph on both hashed personalities; the twelve
ideographs of the compatibility block without one, such as U+FA0E, are
unified ideographs and stay as they are. NFD comes before folding, which
matters for a name such as `a` U+0345 U+0301. The outer NFD changes the
form of no single code point at Unicode 17.0.0 and of no name E4 created,
and is kept as the definition.

The evidence is in [the research record](../docs/apfs-research.md#e4-names):

* Each of 20000 names was created in turn in capture
  `apfs-newfs-268435456-e4-names-ci` and again in `-cs`. A creation failed
  with `EEXIST` exactly when an earlier name had the same comparison form,
  and every stored hash matches the form's hash below.
* Unicode 16.0.0 tables mispredict the hash of 431 of those names, each
  holding a character that 17.0.0 adds with a case folding, such as U+A7CE
  and U+16EA0. In captures `apfs-newfs-33554432-e4-u17marks-ci` and `-cs`
  the 34 combining marks 17.0.0 adds reorder as 17.0.0 says, which 16.0.0
  tables do not predict.
* In captures `apfs-newfs-33554432-e4-u18-ci` and `-cs` macOS refuses with
  `EILSEQ` each of the 79 names E4 made that hold a character 18.0.0 adds.

A record stores the name in the bytes it was created with, neither
normalized nor folded, and its length field counts those bytes and the
NUL; a name equal to an existing one under the comparison form is refused
and the existing spelling stays. Distinct names with equal hashes are both
stored and ordered by their bytes, as the [key order](#comparators)
states; equality is decided by the comparison form, never by the hash.

The hash is computed from the comparison form as a sequence of code points:
CRC-32C (the Castagnoli polynomial, reflected) over the code points as
little-endian 32-bit units, starting from `0xFFFFFFFF`, without the final
complement and without a terminating NUL, keeping the low 22 bits. `root`
hashes to 2989177 and `private-dir` to 2828707. [A1] states the same value
as the complement of a standard CRC-32C, but over a NUL-terminated string;
the NUL is not hashed. E4 reproduces the stored hash of every name, 15774
in the case-insensitive capture and 17595 in the normalization-insensitive
one; `test/fixtures/apfs/name-hashes.txt` holds a curated subset of them
for the tests.

The tables are the driver's own. `tools/gen-apfs-tables.rb` generates
`lib/formats/apfs/unicode-tables.h` from the Unicode Character Database
17.0.0, with the Unicode-3.0 license notice. The
generator requires that version as its input and has no default, so no
table is generated from a guess. `docs/references.md` records the digests
of the pinned input files. HFS
Plus owns its tables the same way, and the two are not shared: the formats
apply different Unicode versions and folding rules, so one owner would hold
two table sets behind one name. The profile name carries the version.

A comparison form is at most three times its name's length in bytes, which
is what lets the [namespace key](design-directories.md#namespace) hold the
form of a 1022-byte name. The generator computes the largest expansion of
any code point and fails if it exceeds three; the Unicode 17.0.0 tables
stay within it.

The reader checks each hashed record's length and hash, a hash mismatch
classified as [above](#directory-records-and-names), and the namespace key
it emits is the comparison form encoded as UTF-8, so two names one volume
treats as equal are an ambiguous namespace, `FSIMG_ECORRUPT`. A selection
query is converted the same way, so `t /Readme` finds `README` on a
case-insensitive volume while extraction writes the stored spelling.

### Extended attributes

Extended attributes are records keyed by owner inode and name. A value is u16
flags, a u16 data length, and either the data inline (flag `0x2`) or a 48-byte
stream reference (flag `0x1`): a stream OID and a data-stream field. Flag
`0x4` marks an attribute the filesystem owns. Exactly one of the inline and
stream flags is set, an inline length fits the record and is at most 3804
bytes, and a stream's extents follow the [data-stream rules](#data-streams-and-file-extents).
Flag `0x10`, which [A1] does not define, may accompany the stream flag and
is ignored: native volumes set it on stream attributes of user names, not
on a compressed file's resource fork. A flag outside `0x1`, `0x2`, `0x4`
and `0x10` is unsupported (E5, E6,
`e5-xattr`, `e6-xclone`). [A1] gives 3804 both as the largest inline size
and as a bound inline data must stay below. The reader accepts inline data
of up to 3804 bytes. Native volumes store at most 3800 bytes inline, and
fewer with a long name: 3794 with a 127-byte name, the longest
`setxattr(2)` accepts. Larger values are streams (E5, `e5-xbound`,
`e5-xlong`). Where between 63 and 127 bytes the name starts to lower the
limit is unmeasured and matters to no rule here. A stream attribute's
value holds its stream ID, the key its file extents are filed under. No
data-stream ID record exists for it, and each stream has exactly one
attribute using it: a clone made with `cp -c` gets a stream ID of its own
for each stream attribute, whose extents name the source's blocks (E6,
`e6-xclone`).

The common entry model has no extended attributes, so the reader delivers
none: attribute names and values are neither listed nor extracted. The
driver consumes three attributes itself, each only on the inode it serves:
`com.apple.fs.symlink` on a symbolic link, `com.apple.decmpfs` on a
compressed file, and `com.apple.ResourceFork` on a file whose compression
type stores its content there, or of size zero beside an inline type.

An inode with any other attribute record holds data the entry model cannot
carry: Finder information, a resource fork that compression does not
consume, quarantine flags and every other name. Each entry naming that
inode sets
[`FSIMG_META_EXTRA_DATA`](design-metadata.md#metadata-fields), as the
[HFS Plus reader](design-hfsplus.md#stored-attributes-and-the-extra-data-mark)
does. The bit makes the omission visible per entry in listings, in `--json`
and in the extraction loss report; no format stores it, so the capability
record's `metadata_fields` does not list it. An inode's attributes are the
kind-4 records under its own number, next to its inode record, so finding
them costs no second lookup. Verification validates every attribute record.

### Hard links and siblings

Several directory records may name one non-directory inode, whose link count
counts them. Each name of an inode that has had several names has a
sibling-link record under the inode (sibling ID, parent inode, name), and
its directory record carries the sibling ID; a volume with the hard-link map
feature also has a sibling-map record under each sibling ID naming the
inode. Sibling IDs are object IDs of their own. The records stay when the
names drop back to one, so an inode with link count 1 has either no
sibling records and no sibling ID on its directory record, or one of each.
The inode's parent and name field are those of one of its names and move to
a remaining name when that one is removed, and hard links do not use the
private directory (E5, `e5-links`). The common model has no hard-link
representation, so the reader delivers every name as its own entry with the
inode's metadata and content; a file's content is read through each name
independently. Verification checks that the link count, the directory
records, the sibling links and the sibling maps agree.

A directory has exactly one name. A directory inode's parent must be the
directory whose record names it, and the root's parent is 1. This makes a
directory cycle impossible: the first directory to repeat on a path would
need two parents. A directory inode also carries the name extended field,
which must equal the name of the record naming it (E3: every native inode,
directories included, carries it on macOS 26.6.2; whether volumes from
other releases or converted from HFS Plus do is provisional, E16 and the
third-party specimens, and the rule stands if neither answers), so a second record naming the directory
under another name in the same parent is corrupt when it is replayed, and a second
record under the same name is a namespace collision. Directory sharing is
therefore detected exactly, without a claimed-object set.

### Data streams and file extents

A data-stream field is five u64 values: logical size, allocated bytes,
default crypto ID, total bytes written and total bytes read. A regular file
without one has size zero. Its extents are file-extent records whose owner is
the inode's private ID: the key suffix is the extent's logical byte offset,
and the value is a u64 whose low 56 bits are the extent's length in bytes and
whose high 8 bits are flags, the extent's first physical block (u64), and a
crypto ID (u64). A data-stream ID record under the private ID holds its
reference count, the number of inodes using the stream: clones share one
stream, and so one private ID, until a write gives the writing inode a
stream of its own (E6, `e6-clone`).

The extents of a stream tile a prefix of its logical range in logical order,
without gaps or overlap: each starts where the previous one ends, and each
length is a positive multiple of `B`. No extent extends past the larger of
the size rounded up to whole blocks and the data-stream field's allocated
bytes, which exceed the rounded size when space is preallocated
(provisional, E16: neither E3 nor E6 shows native preallocation; if E16
cannot answer, the rule stands). The allocated bytes count holes as well
as mapped extents: a sparse native file's allocated bytes equal the total
length of its extents (E6, `e6-sparse`), and in
`apfs-newfs-268435456-e3-ci` a file of `2^32 + 4107` bytes written only at
its end has one hole extent and one mapped block and allocated bytes the
size rounded up to whole blocks (E3). Extents whose physical block is
zero mark holes and read as zero. Native files mark every hole with such an
extent, at the start, in the middle and after the last data, and their
extents tile the whole rounded size (E6, `e6-sparse`). Logical bytes past
the last extent and below the size, which native volumes do not write,
also read as zero: a policy, since such a tail delivers no stored bytes and
reads like a trailing hole.
A sparse native file has the internal flag `0x200` and a sparse-bytes field
equal to the total length of its hole extents (E3, E6).
A mapped extent's blocks lie inside `[1, N)`. Extent flags must be zero, and
crypto IDs follow a closed table for unencrypted volumes. The table is
provisional (E16) and holds zero alone, the only value on every E3 extent,
until a capture adds a value, so
no extent that might be encrypted is delivered. Content past the size,
including preallocated extents, is never delivered. Two streams may name the same blocks: that is a clone, legal
when the extent-reference tree accounts for it.

The reader resolves an offset by seeking the last extent at or below it, so
reading needs no per-file index and a random offset costs one tree descent.
Holes and tails are delivered as [zero extents](design-content.md), never
read.

The extent-reference tree (physical) accounts for physical ownership. Its
key is the 64-bit word whose low 60 bits are the extent's first block and
whose kind is 2; its value is a u64 whose low 60 bits are the extent's
length in blocks and whose high 4 bits are its kind (`1` new or `2` update
on disk; any other is corrupt), the owning object's ID (u64), and a signed
32-bit reference count. In the live tree the count is positive and kinds
are new; how a snapshot's tree records update kinds and counts is
provisional (E9). A live record's count is the number of distinct streams,
file and attribute streams alike, that map each of its blocks, so clones
sharing one data stream count once. Records split where clone branches
diverge, so every block of a record has the same count, one file extent may
span several adjacent records, and one record may back several file
extents. The record's owner field names the object whose write allocated
the blocks: the inode number for file data, which differs from the private
ID once a clone branch has a stream of its own, the inode number for a
compressed file's resource fork, and the stream ID for a user attribute.
[A1] names the private ID and the attribute record's ID. The owner may
since have moved to another stream or been removed, so verification checks
no rule on it (E5, E6, `e5-xattr`, `e6-clone`, `e6-xclone`).

### Symbolic links

A symbolic link's target is the `com.apple.fs.symlink` attribute, owned by
the filesystem, inline, ending in one NUL that is not part of the target.
The target is 1 to 1023 bytes, the limit the capability record declares:
macOS creates a 1023-byte target and refuses a 1024-byte one with
`ENAMETOOLONG` (E14); a longer stored target
is `FSIMG_EUNSUPPORTED`, an empty or unterminated one, or one with an
interior NUL, is corrupt. The entry's size is the target length and its
content the target bytes, delivered from the decoded record.

### Transparent compression

A regular file with `UF_COMPRESSED` has a `com.apple.decmpfs` attribute: a
16-byte header (a u32 magic whose on-disk bytes are `fpmc`, a u32
compression type, a u64 uncompressed size) followed, for the inline types,
by the compressed bytes. The entry's size is the header's
uncompressed size, which must equal the inode's uncompressed size when the
inode carries one, and the data stream must be empty.

The `com.apple.decmpfs` value may be held in its record or in a stream
([extended attributes](#extended-attributes)), and the reader reads both
forms, a stream through the content cursor. A value shorter than 16 bytes
or longer than 16 + `FSIMG_LZFSE_INPUT_MAX` is corrupt, a length checked
before a streamed value is read; after the header is decoded, a
resource-fork type's value must be exactly 16 bytes in either form, and
an inline type's payload, the bytes after the header, is what the reader
reads and decodes. A resource-fork type's `com.apple.ResourceFork` is
read as a stream; one held in its record is unsupported, a form no native
volume shows (E7). A resource-fork attribute of size zero beside an
inline type, in either form, is consumed and holds no content; its record
and extents are verified like any other.

On native volumes (E7, `e7-compression`, `e7-decmpfs-split`), which the
reader does not require:

* the inode has BSD flag `UF_COMPRESSED` (`0x20`),
  `INODE_HAS_UNCOMPRESSED_SIZE` with the header's size, no data-stream
  field and no file extents of its own;
* `INODE_HAS_RSRC_FORK` is set when the resource-fork attribute holds
  bytes and `INODE_NO_RSRC_FORK` when there is no such attribute; `ditto`
  gives an inline type a resource-fork attribute of size zero and neither
  flag;
* a `com.apple.decmpfs` value of up to 127 bytes is in its record and one
  of 129 bytes or more in a stream; every non-empty resource fork is in a
  stream.

| Type | Storage | Codec | Reader |
| --- | --- | --- | --- |
| 1 | Inline | None | Unsupported: no producer or specimen (E7), until a specimen shows its framing; the shared type table knows it and HFS Plus admits it, but admission is per driver |
| 3 | Inline | zlib | System zlib |
| 4 | Resource fork | zlib chunks | System zlib |
| 7 | Inline | LZVN | Imported decoder |
| 8 | Resource fork | LZVN chunks | Imported decoder |
| 9, 10 | Inline, resource fork | None, after a `0xCC` marker | Copied |
| 11 | Inline | LZFSE | Imported decoder |
| 12 | Resource fork | LZFSE chunks | Imported decoder |
| 13, 14 | Inline, resource fork | LZBITMAP | Unsupported: no public decoder |
| other | | | Unsupported |

**Inline types.** The compressed bytes follow the header in the attribute.
A payload whose first byte is its codec's raw marker holds the content
uncompressed after that byte: `0xFF` for zlib and for LZFSE, neither of
whose streams begins with it, and `0x06` for LZVN, its end-of-stream code
(E7, `e7-compression`). Every type 9 payload macOS writes begins with the
marker `0xCC`; the reader treats one that does not as corrupt, a rule of
its own, since no specimen shows what macOS does with it. Inline content
is decoded whole into one 64 KiB buffer: an inline type's uncompressed
size is at most 64 KiB, and a larger one is unsupported. macOS stores a
type inline only when the size is at most 65536 bytes and the whole
attribute, header included, fits about 3800 bytes (3802 measured for
`ditto`; E7).

**Resource-fork types.** The compressed bytes are in the
`com.apple.ResourceFork` attribute, read as a stream. The fork holds one
chunk per 64 KiB of content, `ceil(size / 65536)` chunks, in content order;
every chunk decodes to exactly 64 KiB except the last, which decodes to
the remainder.

The zlib type wraps its chunk table in the classic resource-fork format.
The fork begins with four big-endian u32 words: the data offset, the map
offset, the data length and the map length. At the data offset is a
big-endian u32 resource length, then a little-endian u32 chunk count and
that many little-endian u32 (offset, length) pairs, offsets counted from
the data offset plus 4. The reader checks the relations the
[shared validator](design-apfs-driver.md#compression-framing) checks: the
data holds at least the resource length and count, data and map end the
fork with the map after the data, the resource length is the data length
minus 4, the count is `ceil(size / 65536)` and the pairs end inside the
data. Every native fork has data offset 256 with zeros from byte 16 to
256, a 50-byte map holding one `cmpf` resource, and pairs back to back
from offset `4 + 8 * count` to the data's end (E7).

Types 8, 10 and 12 begin the fork with count + 1 little-endian u32 offset
words from the fork's start: the first is the table's end, which lies
inside the fork and holds the words, and each chunk runs from its word to
the next. In every native fork the first word is `(count + 1) * 4` and the
last is the fork's length (E7).

A chunk whose first byte is its codec's raw marker, the inline marker
above, holds that byte and the chunk's content uncompressed, so a raw
chunk is at most 65537 bytes. Every type 10 chunk macOS writes is `0xCC`
and its content, and the reader treats one that does not begin with
`0xCC` as corrupt. A stored chunk is at most `FSIMG_LZFSE_INPUT_MAX` bytes,
69632, the input bound the
[codec adapter](design-codecs.md#imported-lzfse-and-lzvn-decoders) states,
which the driver applies to every codec's chunk as the HFS Plus reader
does. A short or long result, an out-of-range, overlapping or longer chunk,
a table that does not cover the size, or input left after a chunk's end
marker is corrupt. macOS writes chunks back to back with no padding and
ends each compressed chunk with its codec's end marker, LZVN's 8-byte end
code, LZFSE's `bvx$` or zlib's Adler-32 with no input after it (E7).

Decoded bytes count toward the extraction output limit like any other
content, and that limit defaults to 16 times the selected volume length, so extracting
a volume whose compressed files, holes, clones or hard links expand past
that needs an explicit `-l output=BYTES`, or `-l output=unlimited` for a
trusted image, as the [security design](design-security.md#safe-extraction)
states.

**Delivery.** Delivery receives decoded bytes one chunk at a time as
[held extents](design-content.md), so memory is one 64 KiB output buffer,
one stored chunk of at most `FSIMG_LZFSE_INPUT_MAX` bytes and the LZFSE
decoder's scratch, regardless of file size, and decoding work is charged
per byte produced. The LZVN and LZFSE decoders are Apple's BSD-3-Clause
reference code imported under the
[codec design](design-codecs.md#imported-lzfse-and-lzvn-decoders). Both
return `FSIMG_EUNSUPPORTED` on a big-endian host, so types 7, 8, 11 and 12
are unsupported there. The zlib types and the rest of the reader are
unaffected, since every APFS field is decoded from bytes.

## Snapshots

The snapshot metadata tree (physical) holds, per snapshot, a metadata record
keyed by its XID (the snapshot's extent-reference tree OID and type, volume
superblock OID, creation and change times, an inode number, flags and its
name) and a name record mapping the name to the XID. The read setting
`snapshot=NAME` selects a snapshot of the selected volume by its exact name
bytes, valid UTF-8 of 1 to 255 bytes; any other setting value is
`FSIMG_EINVAL`. Only names of the selected volume match; a missing name is
`FSIMG_EINVAL`.

A snapshot view searches the volume's live object map, which retains the
mappings snapshots need, with the snapshot's XID as the lookup XID, and
reads the snapshot's volume superblock from the address its metadata record
gives. Each object is checked against the XID of the structure that reaches
it: a physical object of the current checkpoint, such as the object map
itself, against the checkpoint's XID; a virtual object against its lookup
XID. Which object map the snapshot superblock names, and that copy's header
OID and storage class, are provisional (E9) and settled before the profile
is enabled; the reader otherwise reads exactly as for the live volume. A
snapshot of a dataless-snapshot volume is unsupported. Snapshot flags and
extended metadata are accepted from a closed table that is provisional
(E9). If E9 has no specimen, because no native tool or owner image
provides a snapshot, the snapshot profile is not enabled and `snapshot=`
fails with `FSIMG_EUNSUPPORTED`. The writer emits an empty snapshot tree.

## Sealed volumes

A sealed volume has the sealed incompatible bit, a nonzero integrity
metadata OID, and a nonzero sealed file-extent tree. [A1] also names the
system role, but the captured sealed volumes have role none, so the role is
not checked (E11: the host's application and OS cryptexes,
`e11-sealed`).

Its filesystem tree is virtual, hashed and headerless. Every node is mapped
with the no-header flag and its first 32 bytes, where a header would be,
are zero; every node, the root included, has the hashed and no-header node
flags, and the tree flags are `0x1c2`: sequential insert, not aligned,
hashed and no header. Each index value carries its child's OID relative to
the root and the child's digest, as [the node layout](#node-layout) states.
A digest is the hash of the whole `B`-byte child node, its zero header bytes
included (E11: 315 and 6195 digests, every one in both cryptexes). The
leaves hold no file-extent records.

File extents are in the sealed file-extent tree, a physical tree with
fixed 16-byte keys `(private ID u64, logical offset u64)` and 16-byte
values `(length and flags u64, physical block u64)`, length in the low 56
bits, ordered by private ID then logical offset. Each extent's flags must
be zero and its length a positive multiple of `B` (E11: 2178 and 6071
records).

The integrity metadata object (kind 30) is a virtual object with an
ordinary header. It holds a version, flags, a hash type (1 SHA-256, 2
SHA-512/256, 3 SHA-384, 4 SHA-512), the offset of the root hash within the
object, and the XID at which the seal was broken, if any (E11: version 2,
flags 0, hash type 1, offset 128, broken XID 0 in both). The version must
be 2, the only one captured; any other is `FSIMG_EUNSUPPORTED`. The offset
must be at least 128, past the header and version 2's fixed and reserved
fields, and leave the whole hash inside the object.

Neither captured cryptex has a snapshot or a broken seal, so both cases
are provisional, with no planned experiment, and refused by default with
`FSIMG_EUNSUPPORTED`: a snapshot view of a sealed volume until a specimen
such as a used macOS System volume shows whether a snapshot is itself
sealed, and a nonzero broken-seal XID until a sealed volume modified after
sealing shows which nodes keep no header. Without digests a headerless
node would have no check binding it to its OID, so the default is refusal
rather than reading without checks.

The sealed profile reads names and content through the sealed tree. It
checks every visited node's digest against its parent and the root's
against the stored root hash with SHA-256, the hash fsimg already carries;
other hash types are unsupported. `fsck_apfs -n` makes the same check: one
changed byte in the free space of an index node or of a leaf is "invalid
node hash for node 1435" or "for node 1379" (E11, `e11-tamper-fsck.log`,
`e11-leaf-tamper-fsck.log`). A successful check covers the filesystem
metadata only, relative to the stored root: file content is covered by
the file-information records' hashes, which fsimg does not check, and no
authenticity claim is possible without a trusted root, so the information
never reports one.

## Refused features

Encryption, a Fusion container, a container whose checkpoint descriptor or
data area is noncontiguous, an incomplete restore, a pending revert, and
object or record flags marking encryption are recognized and refused with
`FSIMG_EUNSUPPORTED`, naming the feature in the error message. The draft's
`KEY_REQUIRED` and hardware-key states are diagnostics of that one status:
fsimg has no key input, so the distinction would change no caller's action.
Encrypted bytes are never decoded as metadata or delivered as content.

An encrypted volume lacks the unencrypted flag and has the one-key flag,
and its filesystem-tree mappings carry the encrypted object-map flag. Its
superblock, object map, extent-reference and snapshot trees are plaintext
with valid checksums, as are the container's shared structures, so the
refusal is made at volume open from the superblock's flags, and an
unencrypted volume of the same container reads normally. A volume
encrypted after creation also has the encryption-rolled bit, and its
rolling-state OID is zero once encryption completes (E11, `e11-enc-*`).
`hdiutil create -encryption` is not APFS encryption: it wraps a plain
container in an `encrcdsa` disk image, a layer fsimg does not peel (E11,
`e11-enc-dmg`).

## Verification

Verification (`W`) checks the selected checkpoint, the container's shared
structures and every volume's trees, and the selected volume's namespace
through the common walker.

**Objects.** Every superblock, checkpoint mapping, ephemeral object, object
map, tree node and record reachable from the checkpoint passes the rules
above, in every volume. Volume UUIDs are distinct. An unknown optional or
read-only-compatible feature bit, which reading may ignore, makes the
verification incomplete, since its structures are not checked.

**Records.** Every volume's filesystem tree is scanned once in key order
and its records reconciled:

* each directory record names an existing inode whose mode matches its
  type, and each directory's child count equals its records;
* each non-directory's link count equals its names; an inode with more than
  one name, or with any sibling link, has one sibling link per name whose
  sibling ID, parent and name equal those of the directory record carrying
  that ID, and, in a volume with the hard-link map feature, one sibling map
  per sibling link;
* every inode other than the root and private directories is named by a
  directory record, or lies in the private directory, which is where native
  volumes place unnamed inodes (provisional, E16: E3 holds no unnamed
  inode; if E16 cannot answer, the rule stands);
* every file extent belongs to a live stream, an inode's private ID or the
  stream ID one attribute record names, and a data-stream ID record's
  reference count equals the inodes using its stream:
  * an inode's stream always has a record;
  * an attribute's stream, used by its one attribute alone, has none:
    native user attribute streams (E5) and the `com.apple.decmpfs` and
    `com.apple.ResourceFork` streams of compressed files (E7) carry none,
    and a record present is a violation;
  * each stream's extents respect the size and allocated bytes of the
    data-stream field that names it;
* every attribute is well formed, and names equivalent under the volume's
  rules never share a parent;
* the volume superblock's object counts equal the counted inodes by kind.

**Allocation.** Every block is claimed exactly as the space manager says.
Claims in the main domain come from the
[fixed regions](#ephemeral-objects-and-the-space-manager), one range each;
the object map itself (kind 11, physical) of the container and of each
volume, and every node of each map's tree; every live object version an
object map maps, tombstones excluded, including the versions snapshots
retain; every physical tree node, including snapshot trees; a nonzero
keylocker range; every entry of the main free queue; and the data extents
described below. A reaper that is not idle makes the verification
incomplete, as [its section](#reaper) states.

The internal pool is a domain of its own. Space-manager blocks inside it
and the entries of the internal-pool free queue are claims in that domain,
never in the main one, and the pool's bitmap must be set exactly for them.
Space-manager blocks outside the pool are main claims.

Data blocks are claimed through the physical-extent records of every
extent-reference tree of a volume, the live tree and each snapshot's. In a
sealed volume, whose file extents live in the sealed file-extent tree,
those extents are checked against the reference records in the same way.
Both captured sealed volumes have an extent-reference tree, with one
physical-extent record of kind new and reference count 1 for each sealed
file extent over the same block range (E11: 2178 and 6071 records); the
rules that follow hold for them as written, with no sealed case.
Records of one tree never overlap; records of different trees of one
volume may, since a snapshot and the live volume share unchanged extents,
so a block a snapshot alone still uses is claimed by that snapshot's
record. Every block of a mapped file extent of the live filesystem tree
lies inside a live physical-extent record; one extent may span several
adjacent records. File extents of different streams overlap only inside
such records, and every block of a live record is mapped by as many
distinct live streams, file and attribute streams alike, as its reference
count (E6, `e6-clone`, `e6-xclone`). How a
snapshot's references enter the counts is provisional (E9).

Metadata claims are exclusive, and data claims never overlap metadata. A
block is allocated in the main bitmap exactly when it is claimed; each
chunk's free count and the device's free count equal the clear bits below
`N`; the bits past `N` are not checked (E15). One check is provisional and
is not made until its experiment gives the rule: the allocated block count
of a volume that has snapshots (E9). Every other volume's allocated
block count equals the nodes of its object-map, filesystem,
extent-reference and snapshot trees, one block more, and the data blocks
its physical-extent records cover, a block that clones share counted once;
a sealed volume adds the nodes of its sealed file-extent tree and its
integrity object (E11: 10552 and 584389 blocks, exact in both cryptexes).
E3 counts the one block more as the object-map object; E5 and E6 cannot
tell it from the volume superblock, and the count is the same either way.
The evidence: 5 on an empty volume and 571 = 1 + 4 + 258 + 1 + 1 + 306 in
`apfs-newfs-268435456-e3-ci` (E2, E3); every E5 and E6 capture, clones
included; and `e6-xclone`, which removed a 315-block file and counts
156 = 151 + 5, so freed blocks leave the count (E6). That a volume which
has freed blocks many times follows the same rule is provisional (E16); if
E16 cannot answer, the check stands, since E6's freed file follows it.

A [truncated container](#truncated-containers) never reaches these checks:
the reader core fails its verification first.

**Namespace.** The selected volume's walk performs namespace validation,
path limits and, with payload read-through, reads every file's content
through common delivery, decompressing compressed files. The report counts
the selected volume's files, directories and logical bytes and the
container's allocated and free blocks. A refused feature in any volume
makes the verification incomplete with `FSIMG_EUNSUPPORTED`.

## Writer

### Profile

The writer builds a new container in one deterministic layout that
reproduces what the native formatter, `newfs_apfs`, produces for an empty
container of the same size (E1, E2), then adds the catalog's entries in one
transaction. This list names what it emits; [writer values](#writer-values)
owns every number and field value:

* block size 4096 and one checkpoint, in the native checkpoint areas for the
  container's size;
* one volume in slot 0, unencrypted, role none, named by the label;
* case-insensitive by default; case-sensitive with `case=sensitive`, with
  the native normalization flag;
* the native-required records of an empty volume: the root and private
  directories, and every other record E1 shows `newfs_apfs` writing;
* for each regular file: an inode with a name field and, when nonempty, a
  data-stream field, a data-stream ID record, one contiguous run of
  blocks, the file extents that tile it, one unless the run is longer than
  the [extent cap](#writer-values), and one physical-extent record owned by
  the inode;
* for each directory: an inode with a name field and child count; for each
  symbolic link: an inode and an inline filesystem-owned
  `com.apple.fs.symlink` attribute; for each FIFO: an inode with no data
  stream;
* a directory record in the parent for every entry, in the key variant the
  volume's flags select, with the type of its kind;
* an empty extent-reference tree when there are no files, an empty snapshot
  tree, empty [free queues](#free-queues) and an idle [reaper](#reaper);
* the space manager's chunk structures and bitmaps marking exactly the
  allocated blocks.

Every file's blocks are allocated in full, as the
[architecture](architecture.md#output-and-determinism) requires of every
writer, so the writer emits no holes. It emits no clones, compression,
extended attributes, hard links, snapshots, sealing, encryption or second
volume. A catalog name is checked against the volume's rules: the catalog
already refuses exact duplicates, and the writer refuses two siblings whose
comparison forms are equal with `FSIMG_EINVAL`, whose path names one of
them: an error carries one path. A name longer
than the writer's [name length](#directory-records-and-names) limit or
outside the reader's name rules, or holding a code point the writer
refuses, fails the same way.

### Settings and identities

| Setting | Field | Meaning |
| --- | --- | --- |
| `-L` | `label` | Volume name, UTF-8 of at most 255 bytes; NULL or empty selects the [default label](#writer-values) |
| `uuid=` | `volume_uuid` | Volume UUID in text order. All zero is refused outside reproducible mode; in reproducible mode it is derived from the epoch by the [layer identifier rule](design-layers.md#identifiers) with role 5. Without `-R` and `uuid=` the CLI supplies a random version 4 UUID |
| `case=` | `case_sensitive` | `insensitive`, the default, or `sensitive` |
| `cluster=` | `allocation_unit_bytes` | Zero or 4096 |
| `sector=` | `sector_bytes` | Zero or 4096: APFS addresses whole blocks |

The writer does not carry `FSIMG_SETTING_PLACEMENT`, so a non-empty
[placement](design-placement.md) list fails the common applicability check.

The container UUID is derived from the volume UUID: the first 16 bytes of
the SHA-256 of the ASCII bytes `fsimg-apfs-container` followed by the volume
UUID, with the version nibble set to 8 and the variant bits to `10`. It is
therefore distinct from the volume UUID, reproducible, and needs no setting.
An absent owner, group or mode takes the
[default owner and modes](#writer-values) under the
[metadata policy](design-metadata.md#writer-policy).

A time is stored in nanoseconds. The **format time** is the UTC host clock
sampled once during planning or, in reproducible mode, the epoch.
Directory-record add times, the superblock times and the formatting
record's time are the format time, and in reproducible mode so is every
stored time. Otherwise:

* an absent time takes the format time, which is no loss;
* an entry's time outside the writer's [time range](#writer-values) takes
  the format time and is a counted loss under the policy;
* a format time outside that range is refused, not counted: the host clock
  with `FSIMG_ERANGE` and an epoch at option validation, as the
  [loss design](design-metadata-loss.md#defaults-and-reproducibility)
  states.

### Layout

The layout is a pure function of the catalog and the options. Every record's
encoded length depends only on the catalog: physical block numbers and
counts have fixed widths. Planning can therefore pack every tree and count
its nodes before any block is placed, then place metadata and data, and
emission fills in the addresses without changing a node's contents
otherwise.

The allocated blocks are the fixed regions and the run:

* The [fixed regions](#ephemeral-objects-and-the-space-manager) are the
  one list verification claims: block 0, the checkpoint descriptor area,
  the checkpoint data area, the internal pool's bitmap ring, and the
  internal pool, which holds the space manager's chunk-information, address
  and bitmap blocks. Each sits at the block and has the size the native
  formatter gives it for `N`, as [writer values](#writer-values) state. The
  native formatter places them in that order as one gapless prefix from
  block 0; the writer checks that the list it derives for `N` does, and a
  list that does not is an internal error rather than a layout that skips a
  region.
* The **run** holds everything else, contiguous from the first block after
  the prefix: the container object map, the volume's superblock, object
  map, filesystem tree, extent-reference tree and snapshot tree, then file
  data in catalog order. The allocation is therefore one range, from block
  0 to the run's end.

Trees are built bottom-up from their sorted records, with virtual OIDs and
object-map records assigned in node order. The bitmaps are a run of ones
and then zeros, built from the allocated range without a pass over blocks. The
order above fixes block numbers only: emission may write the blocks in any
order and produces the same bytes.

Automatic sizing chooses the first block count of its search that holds the layout,
the writer's minimum container size, the requested free space and the native
reserve, if any; the [sizing design](design-sizing.md#dimensions-per-format)
states the dimensions. Whether a block count fits is not monotone in it: a
fixed region grows in jumps, one of which can take more blocks than a size
step adds, and moves the run's start with it. The search assumes nothing
about how the regions change with `N` beyond what each probe measures, and
its result is the first block count of a deterministic upward sequence
that fits. When the prefix never shrinks as `N` grows, that first fit is
also the smallest fitting count, since each probe's successor is the
smallest count that could hold its prefix; the search does not rely on
it. Reproducibility needs only the sequence to be fixed:

* It starts at a lower bound that holds for every `N`: block 0, the run's
  blocks and the requested free space, none of which depends on `N`, raised
  to the minimum container size and rounded up to the size step.
* A probe that fails measures its shortfall: the blocks by which the run's
  end plus the requested free space exceeds `N`. The next probe is `N`
  plus the shortfall rounded up to the size step, and at least one step.
  No block count below `N` plus the shortfall can hold the layout `N`
  planned. The first probe that fits ends the search.
* A probe evaluates the layout's arithmetic over the fixed-region list of
  its `N`. It places nothing and allocates nothing, so its cost is
  proportional to the number of regions.
* Each failed probe advances past its whole shortfall, so the probes
  number about the times growth of the `N`-dependent regions outruns the
  previous jump, a handful in practice. The [probe cap](#writer-values)
  bounds them whatever the regions do; a search that reaches it fails with
  `FSIMG_ERANGE` naming the cap.

### Writer values

This section owns every value the writer emits; other sections refer to it
instead of repeating a number. The columns:

* **Source** is [A1] for a value the reference's rules determine, an
  experiment for a native observation, and policy for fsimg's choice.
* **Provisional, closed by** is empty for a settled value. A row that names
  an experiment there is provisional: its *Emitted* cell is the working
  value until that experiment runs.
* **Otherwise** is what the writer emits when the experiment contradicts
  the working value or cannot isolate the question. *Stays* keeps the
  working value. *Native* takes the value decoded, field by field, from the
  E1 capture of the same personality and size, and is used only for a
  constant of the empty layout: a value that is the same for every catalog
  and every format time. *Formula* is for a value that depends on the
  catalog or on time, such as a child count, an object count, an
  allocated-block count, a footer count, a next identifier or a timestamp.
  Such a value is always built by the construction in its *Emitted* cell.
  If the experiment contradicts that construction, the one that reproduces
  the experiment's captures replaces it, and if none is found the writer
  stays unregistered. A captured value is never copied into a populated
  container.

E1 formats containers on the development host with `newfs_apfs`; its
captures and size sweep are in the
[research notes](../docs/apfs-research.md#e1-empty-containers). `X` is the checkpoint's
XID.

`newfs_apfs` commits two checkpoints (E1). The **first form**, at
XID 1, holds the container alone: a mapping block and its superblock in
descriptor slots 0 and 1, the space manager and the reaper in data slots 0
to `S` (0 and 1 for a one-block space manager), an empty container object
map, both free queues empty in the
zero-OID form and every object at XID 1; apfs-newfs-372736-plain is
entirely in it. The **second form**, at XID 2, adds the volume: a second
checkpoint in descriptor slots 2 and 3 and data slots 2 to 5 (`S + 1` to
`2 * S + 3` for a space manager of `S` blocks; E12: 3 to 7), the free
queues' tree roots holding the blocks that transaction freed, and XID 2 on
every object it rewrote. The writer emits the first form with the volume
and the catalog added in the same transaction, so `X` is 1, the container
has one checkpoint and the queues are empty; a row citing E1 gives the first
form's value unless it says otherwise. `fsck_apfs -n` accepts the first
form without a volume (E1: apfs-newfs-4194304-plain with its descriptor
slots 2 and 3 zeroed and block 0 replaced by slot 1's superblock) and with
a volume (E15, `e15-x1vol-all`: apfs-newfs-4194304-plain rewritten to
the first form, with the rings, counters, newest mounted version, free
queues, pool, XIDs, OID numbering, formatting identifier and
volume-superblock tail the rows below give, which also mounts). That
probe is at 1024 blocks: one chunk, `C = 1`, `A = 0`, `S = b = 1`; the
pool ring at `b = 97` is probed on its own (`e15-ring97`).

In the formulas below, `C` is the chunk-information block count, `F` the
maximum volume count and `P` the internal pool's block count, and
`q(x, k)` is `ceil(x / k)`, except that a quotient of 2 becomes 3. `A` is
the address-block count, `S` the space manager's mapped size in blocks and
`b` the pool bitmap's size in blocks, as the rows below give them. These
formulas, `q` and the `8 * M * F` term among them, are fits to the
captures, not rules [A1] states, and `E` is a quarter of the data area.
Every formula reproduces all eight E1 captures, the 90 dumps of the E1
size sweep, which formats each block count the
[research notes](../docs/apfs-research.md#e1-empty-containers) list, from 92
to 100663296 (384 GiB), and the two captures and 30 sweep dumps of
[E12](../docs/apfs-research.md#e12-large-containers), from 512 GiB to
34359738368 blocks (128 TiB). No container above 34359738368 blocks is
written: `validate` and the size search fail with `FSIMG_ERANGE` above it.

**Transactions**

| Value | Emitted | Source | Provisional, closed by | Otherwise |
| --- | --- | --- | --- | --- |
| Object XID `X` | 1, in every object header and every object-map key | E1: the first form; [A1] requires only a nonzero XID not above the checkpoint's; E15: the first form with a volume (`e15-x1vol-all`) | | |
| Container next XID | `X + 1` | [A1] | | |
| Object-map snapshot count, latest-snapshot XID, snapshot tree and pending revert range | Zero | [A1]: no snapshots | | |
| Volume revert-to, root-to and encryption-rolling fields | Zero | [A1]: no transition | | |

**Object identifiers**

| Value | Emitted | Source | Provisional, closed by | Otherwise |
| --- | --- | --- | --- | --- |
| Container superblock OID | 1 | [A1] | | |
| Ephemeral and virtual OIDs | From 1024, the first OID [A1] does not reserve, in this order: space manager, reaper, volume superblock, then the filesystem tree's nodes in the order they are completed | E1 for 1024 to 1026: the second form has space manager 1024, reaper 1025, volume superblock 1026, free-queue roots 1027 and 1029 and next OID 1030; policy for the tree's nodes; E1: 1028 holds the filesystem root, its root tree OID in apfs-newfs-4194304-plain's volume superblock; E15: `fsck_apfs -n` accepts the root at 1027 with next OID 1028 (`e15-x1vol-oid`) | | |
| Container next OID | One past the largest OID assigned | [A1] | | |
| OID of a physical object: checkpoint-mapping block, object map, object-map node, extent-reference or snapshot tree node, chunk-information or address block | Its block number | [A1] | | |
| Volume superblock's tree references | Object map, extent-reference tree and snapshot tree by block number; root tree by virtual OID | [A1] | | |

**Inodes and records**

| Value | Emitted | Source | Provisional, closed by | Otherwise |
| --- | --- | --- | --- | --- |
| Inode numbers | Root 2 and private directory 3, both with parent 1; the entry with catalog ID `i`, from 1, gets `15 + i`, so entries are numbered from 16 in catalog order | [A1] for 2, 3 and the minimum 16; policy for the order | | |
| Private ID | The inode number | [A1]: the value of a file never cloned | | |
| Volume next object identifier | One past the largest inode number, 16 for an empty volume. It counts inode numbers and is not the container's next OID | [A1]; E2: 16 on every empty volume | | |
| Directory inode | Child count equal to the directory records it owns, which for the root excludes the private directory; no data-stream field; uncompressed size zero | [A1]; E3 | | |
| Non-directory inode | Link count 1 | [A1]: one name | | |
| Name field | On every entry's inode, the name of its directory record, flags `0x2`; on the root and private directories, native | E1, E3 | | |
| Internal flags, protection class, write generation | Internal flags `0x8000`. Protection class 4 on a FIFO, 0 on every other inode. Write generation 1 plus the child count on a directory, 2 on a file with content, 1 on any other inode: the values of a native entry created and written once, which the root and private directories of an empty volume (E2) also follow | E2, E3 | | |
| BSD flags | Zero | Policy | | |
| Directory statistics records and their inode field, sibling records, document IDs, purgeable and firmlink fields | None | Policy; E3: none on any native directory, the root's `purgeable-drecs-fixed` attribute aside; E15: `fsck_apfs -n` accepts the E3 root without that attribute (`e15-noxattr`) | | |

**Checkpoint and container superblock**

| Value | Emitted | Source | Provisional, closed by | Otherwise |
| --- | --- | --- | --- | --- |
| Checkpoint area sizes and bases | With `E = L + I + 2 * S + 1 + 8 * M * F`: the descriptor area at block 1, of `4 * (ceil(E / 101) + 1)` blocks, 101 being the mappings a block holds; the data area right after it, of `4 * E` blocks; `L`, `I`, `M` and `S` as the rows below give them | E1; E12 for the `S` term (`S = 2` at 128 TiB: `E = 7429`) | | |
| Descriptor ring | One mapping block in slot 0 and the superblock in slot 1: first-block index 0, block count 2, next index 2; every other slot zero | E1: the first form; E15 (`e15-x1vol`) | | |
| Mapping block | One mapping per ephemeral object in ascending OID order, with the object's kind, subtype and size, 4096 or the space manager's `4096 * S`, owning volume zero; the last-map flag set | [A1]; E1; E12 for `S` | | |
| Data ring | The space manager in slots 0 to `S - 1` and the reaper in slot `S`: data index 0, data count `S + 1`, next index `S + 1` | E1: the first form; E12: the XID-1 mapping block of apfs-newfs-140737488355328-e12-128t, slots 0 to 2 for `S = 2`; E15 for `S = 1` (`e15-x1vol`) | | |
| Block 0 | Byte for byte the superblock in the descriptor ring | [A1]: a copy; E1: natively it differs only in counter 0; E15: `fsck_apfs -n` accepts it equal (`e15-block0copy`, `e15-x1vol`) | | |
| Maximum volume count | `F = min(100, ceil(N * B / 512 MiB))` | [A1]; E1; E10: 4 at 2 GiB and 16 at 8 GiB, and `diskutil` adds no volume past it; E15: `fsck_apfs -n` accepts counts 2 and 100 at 4 MiB and 15, 17 and 100 at 8 GiB, off the formula, and refuses 0 and 101; `apfsck` refuses 100 at 4 MiB and 15 at 8 GiB (`e15-maxvol-*`) | | |
| Features and flags | Incompatible `0x2`; read-only compatible, optional features and flags zero | [A1]; E1 | | |
| Counters | Zero | Policy; natively only counter 0, objects checksummed, is nonzero (E1, E12); E15: `fsck_apfs -n` accepts every counter zero (`e15-counters0`, `e15-x1vol`) | | |
| Ephemeral-information words | Word 0 `(M << 32) + 0x40001`, [A1]'s minimum block count, four structures per volume and version 1, the others zero, with `M = L` up to 128 MiB and 8 above, as [A1] states | [A1]; E1 | | |
| Newest mounted version | Zero, as in the first form | E1; E15: `fsck_apfs -n` accepts zero with a volume (`e15-nmv0`, `e15-x1vol`) | | |
| Fusion fields, keylocker ranges, EFI jumpstart, test fields | Zero | [A1]: features absent | | |

**Space manager**

| Value | Emitted | Source | Provisional, closed by | Otherwise |
| --- | --- | --- | --- | --- |
| Geometry | 32768 blocks per chunk; `ceil(N / 32768)` chunks; 126 chunks per chunk-information block; 507 chunk-information blocks per address block; `C = ceil(chunks / 126)` chunk-information blocks; `A` address blocks, zero when `C <= 507` and `ceil(C / 507)` otherwise | [A1]; E1; E12 for `A` | | |
| Chunk-information entry | XID `X`; first block `32768 * i` for chunk `i`; block count 32768, or `N - 32768 * i` in the last chunk; free count the chunk's clear bits; bitmap address zero for a chunk with no allocated block | [A1] for the fields; E1 | | |
| Bitmap bits past `N` | Zero | E1; E15: `fsck_apfs -n` accepts them set too, `apfsck` does not (`e15-pastn-all`) | | |
| Free counts | The device's free count is `N` less the allocated blocks and equals the sum of the chunks' free counts | [A1] | | |
| Internal pool | The pool's bitmap ring of `16 * b` blocks, the transaction multiplier 16 times a bitmap of `b = ceil(P / 32768)` blocks, right after the data area; the pool right after the ring, of `P = 3 * (chunks + C + A)` blocks; both allocated in the main bitmaps; the pool's bitmap set exactly for the address, chunk-information and bitmap blocks placed in it; `S` the smallest whole number of blocks holding the structure and its packed arrays | E1; E12 for `A`, `b` and `S` | | |
| Internal-pool blocks | Inside the pool, ascending from its base: the chunk-information blocks, or with address blocks address block 0, the up to 507 chunk-information blocks it names, address block 1, the next 507, and so on; then one bitmap block per chunk that holds an allocated block, in chunk order | E1: the first form, as apfs-newfs-107374182400-plain's XID-1 blocks show (seven chunk-information blocks from the base, then the bitmaps of chunks 0 and 22); E12 for the address blocks (apfs-newfs-140737488355328-e12-128t: an address block at the base, chunk-information blocks 0 to 506, the next address block, and so on) | | |
| Internal-pool ring fields | Free head `b` and tail `16 * b - 1`; slots 0 to `b - 1` current, with XID `X`, as bitmap indexes 0 to `b - 1`; free-next `0xffff` for slots 0 to `b - 1`, `i + 1` for slots `b` to `16 * b - 2` and `0xffff` for the last | E1: the first form, `b = 1`; E12's XID-2 state at `b = 97` is that form advanced by one bitmap; E15: `fsck_apfs -n` accepts the form at `b = 97` (`e15-ring97`) | | |
| Reserve counts, flags, version, structure size | Zero reserves; flags `0x1`; version 1; structure size 2520 | E1 | | |
| Tier-2 device and queue | Zero | [A1]: no Fusion | | |
| Free queues | Internal pool and main: zero tree OID, count zero, oldest XID zero; node limits `I = min(512, q(P, 1136))` (E12, bisected: 511 at `P` 580494, 512 at 580497 and at 581634, where `q` is 513) and `L`: `q(N, 4544)` below 2^18 blocks (1 GiB), `q(N, 2272)` from 2^18 and 512 from 2^20 (4 GiB) | E1: the first form; E15: with a volume (`e15-x1vol`) | | |
| Reaper | Idle: head, tail, reap-list object count and the six fields of an object being reaped zero; flags `0x1`; next reap ID 1, completed reap ID 0; state buffer of `B - 112` bytes, zero | [A1] for idle and the flag; E1 | | |

**Volume superblock**

| Value | Emitted | Source | Provisional, closed by | Otherwise |
| --- | --- | --- | --- | --- |
| Slot, index, role, name | Slot 0, index 0, role 0, the label | Policy | | |
| Features and flags | Incompatible `0x1` on a case-insensitive volume and `0x8` on a case-sensitive one; flags `0x1`; read-only compatible zero; optional features `0x2` | [A1]; E2 | | |
| Object counts | Files: regular files. Directories: directory entries, the root and private directories not counted. Symbolic links. Other: FIFOs, as native volumes count FIFOs and sockets. Snapshots: zero | [A1]; E3 | | |
| Allocated block count, total blocks allocated and freed | Allocated: the nodes of the volume's object-map, filesystem, extent-reference and snapshot trees, one block more for the object-map object, and its file-data blocks, as [verification](#verification) states; total allocated the file-data blocks alone, the native value of a volume that has freed no data; total freed zero | [A1]; E3, E5, E6 | | |
| Reserve and quota | Zero | [A1]: none | | |
| Modification and unmount times | Modification time the format time; unmount time zero, as a fresh native volume holds it | Policy for the modification time: E1's fresh volumes store a modification time some 100 to 200 microseconds after their formatting record's, which a single format time does not reproduce; E1 for the unmount time, zero on every fresh volume (apfs-newfs-4194304-plain, apfs-newfs-1073741824-plain, apfs-hdiutil-33554432-plain) | | |
| Formatting record | Identifier `fsimg` followed by the package version, NUL-padded to 32 bytes; the format time; last XID `X` | Policy; E15: `fsck_apfs -n` accepts another identifier and reports it, `formatted by fsimg 0.1.0` (`e15-fmtby`) | | |
| Modification records | Zero | E2: all empty | | |
| Fields past [A1]'s layout, from offset 1056 | Zero | Policy: [A1] does not give their meaning, and zero carries no residue; E15: `fsck_apfs -n` accepts them zero and a read-only mount reads the volume (`e15-x1vol-tail0`, `e15-tail0`); a read-write mount is untested. Natively nonzero words sit at offsets 1056, 1064, 1068, 1104, 1120 and 1128; `fsck_apfs` names the one at 1056 `apfs_doc_id_index_xid`, natively the formatting XID, accepts it equal to `X` (`e15-x1vol-docx1`) and warns when it is not below the container's next XID (`e15-x1vol`) | | |
| UUIDs | Volume: the setting. Container: derived as [settings](#settings-and-identities) state. Volume group: zero | Policy | | |
| Next document ID, clone-information epoch and XID | Next document ID 3; the epoch and XID zero | E2, E3 for the document ID; E1: the epoch and XID are zero on every fresh volume (apfs-newfs-4194304-plain, apfs-newfs-1073741824-plain, apfs-hdiutil-33554432-plain) | | |
| Snapshot, sealing and integrity fields | Zero | [A1]: features absent | | |
| Metadata crypto state | Zero | Policy: an unencrypted volume reads no key, and zero carries no host build number; E1's fresh volumes hold major version 5, persistent class 6, a key OS version and key revision 1. `fsck_apfs -n` accepts zero, the volume mounts read-only and read-write, and modifications pass `fsck_apfs -n` again (the writer's qualification outputs, `tools/apfs/qualify.rb`, both personalities) | | |

**Trees and content**

| Value | Emitted | Source | Provisional, closed by | Otherwise |
| --- | --- | --- | --- | --- |
| Node fill | Greedy against the root's capacity, 4096 bytes less the 56-byte header and the 40-byte footer: a node, root or not, takes records until the next does not fit, so a level's only node is always a valid root and 40 bytes stay unused in every other node | Policy; E15: `fsck_apfs -n` accepts a leaf packed to 3960 of the 4000 bytes (`e15-fill`); E3's native leaves leave as little as 12 bytes free | | |
| Last node of a level | What remains when the records end, as few as one record | Policy; E15: `fsck_apfs -n` accepts a last node of one record at the leaf level (`e15-lastleaf1`) and at an index level (`e15-last1`) | | |
| Table-of-contents capacity, key-area start, alignment, tree flags, and the footer's node size and fixed key and value sizes | Native, per tree. The container object map's root leaf: node flags `0x7`, a 448-byte table of contents at offset 0, tree flags `0x12`, node size 4096, keys and values of 16 bytes (E1). In the filesystem tree: flags `0x42`, node size 4096, fixed sizes zero, and a node's table of contents its key count rounded up to a multiple of 8 entries, the shape of 253 of the 254 leaves of `apfs-newfs-268435456-e3-ci`, where every table is a multiple of 8 entries and index nodes hold up to 8 more (E3). The volume object map has the container map's shape: a root leaf of node flags `0x7`, a 448-byte table (112 entries) and tree flags `0x12` in every E1 capture (`apfs-newfs-4194304-plain`, `apfs-newfs-1073741824-plain`, `apfs-newfs-8589934592-plain`, `apfs-hdiutil-33554432-plain` and the four volumes of `apfs-diskutil-2147483648-e10-roles`); in `apfs-newfs-268435456-e3-ci` and `-cs`, an index root of node flags `0x5` whose table holds 144 entries, 576 bytes, the most 16-byte keys with 8-byte child OIDs a node has room for, over leaves of node flags `0x6` with 448-byte tables (E3). A fixed-size tree's table is so sized for the records of the node's kind, leaf or index. The extent-reference and snapshot trees are variable-size and unaligned: tree flags `0x52`, node size 4096, fixed sizes zero, a root leaf of node flags `0x3`, and a table of contents of the key count rounded up to a multiple of 8 entries and at least 8: 64 bytes when empty in every E1 capture, 128 for 9 records (`apfs-newfs-67108864-e5-links`), 192 for 19 (`apfs-newfs-67108864-e6-clone`) and 256 for 27 and 28 (E3). The snapshot tree is empty in every capture, so the tables of a populated one are provisional, taken as the extent-reference tree's; the writer emits it empty, the shape every capture shows, so nothing it writes depends on them. A fixed-size leaf of ghosts that outgrows its native table grows in steps of 8 entries, the table increment [A1] gives; that step is provisional, since no capture shows such a table, and a free-queue leaf of more than 144 ghosts in a native capture would show it. Without one the step stays the increment [A1] gives; the writer's free queues are empty, so it never builds such a leaf. | E1, E3, E5, E6 | A capture with a snapshot, for a populated snapshot tree; a free-queue leaf of more than 144 ghosts, for the step | Native |
| Footer counts | Key count: the tree's leaf records. Node count: its nodes. Longest key and longest value: the largest encoded lengths among its leaf records, zero in an empty tree; an index key repeats a leaf key | [A1]; E3 for the counts and the longest key | | |
| Object-map flags | Container map `0x1`; volume map zero | [A1]; E1: the container map `0x1` and the volume map zero, crypto-generation flag clear (apfs-newfs-4194304-plain) | | |
| Empty extent-reference and snapshot trees | One root leaf with no keys | E1, every capture's volume; E2: one node each among an empty volume's 5 allocated blocks | | Native |
| File extent | A nonempty file's run, its size rounded up to 4096 bytes, is tiled by consecutive extents of at most the extent cap, so a file no longer than the cap has one; flags and crypto ID zero | [A1] | | |
| Extent cap | `2^56 - 4096` bytes, the largest block-aligned length the 56-bit length field holds | [A1]: the field's width; E15: native single extents of 671088640 and 5368709120 bytes, the second above `2^32`, which `fsck_apfs` accepts (apfs-newfs-1073741824-e15-big, apfs-newfs-8589934592-e15-huge) | Lengths above 5368709120 bytes: `fsck_apfs -n` on the writer's large-file test output holding a longer extent, or a capture of a longer native file | 5368709120 bytes (1310720 blocks), the longest extent E15 shows accepted, if neither shows a longer one accepted |
| Data-stream field | Size; allocated bytes the size rounded up to 4096; crypto ID zero; total bytes written the size and total bytes read zero; flags `0x20`. A file of size zero has none, and no data-stream ID or reference record either | [A1]; E3 | | |
| Reference records | Data-stream ID count 1; one physical extent over the file's run, of kind new, its length in blocks, owner the inode number, which is the private ID, count 1 | [A1]; E3, E5, E6; E15: native records of 163840 and 1310720 blocks (apfs-newfs-1073741824-e15-big, apfs-newfs-8589934592-e15-huge) | Runs above 1310720 blocks, closed with the extent cap | One physical-extent record per file extent, which record generation and the extent-reference tree's planning then count per extent rather than per file |

**Metadata and limits**

| Value | Emitted | Source | Provisional, closed by | Otherwise |
| --- | --- | --- | --- | --- |
| Name length | 255 UTF-16 code units, as [name length](#directory-records-and-names) states | E14 | | |
| Link target | 1 to 1023 bytes, as [symbolic links](#symbolic-links) state | E14 | | |
| Default owner and modes | Owner and group 0; `0755` for directories and symbolic links; `0644` for files and FIFOs | Policy; E3 for the `0755` macOS gives a new symbolic link under umask 022 | | |
| Default label | `Untitled`, the volume name when the label is NULL or empty | Policy | | |
| Time range | `-2^63` through `2^63 - 1` nanoseconds, 1677-09-21 00:12:43.145224192 through 2262-04-11 23:47:16.854775807 UTC, the range macOS clamps times to | E14 | | |
| Format time | The UTC host clock sampled once during planning; the epoch in reproducible mode | Policy | | |
| Minimum container size | 1024 blocks (4 MiB), the smallest container `fsck_apfs` opens. `newfs_apfs` exits 0 from 91 blocks but writes no volume below 93, and `fsck_apfs` refuses every container below 1024 blocks | E1; policy: native qualification must be able to check the output; E15: `fsck_apfs -n` accepts the first form at 1024 blocks (`e15-x1vol-all`) | | |
| Container size step | One block: `newfs_apfs` formats every block count of the size sweep, odd counts included | E1 | | |
| Probe cap | `2^16` probes of the automatic size search. Each failed probe jumps past its whole shortfall, so a search needs a probe only when `N`-dependent regions outgrow the previous jump; the cap bounds a pathological region table, not an expected search | Policy | | |
| Unused bytes | Zero: reserved and unused fields, node free space, unused ring slots, the bitmap bits past `N`, the tail of a file's last block and every free block | Policy | | |

## Information and capabilities

The capability record names `apfs` with the profile `apfs-unicode-<version>`,
files, directories, symbolic links and FIFOs, the read-only devices and
sockets, owner, mode and all four
timestamps; block sizes 4096 through 65536 for reading and 4096 for writing,
in both the sector and allocation-unit masks since APFS addresses whole
blocks;
`max_label_bytes` 255; `max_link_target_bytes` 1023; `max_name_utf16_units`
255, the writer's [name length](#directory-records-and-names) limit;
`write_settings` with the volume UUID and case sensitivity, and without
`FSIMG_SETTING_PLACEMENT`; and `read_settings` with the volume index and
snapshot. Three points follow from rules stated elsewhere:

* `metadata_fields` lists neither the device number nor
  `FSIMG_META_EXTRA_DATA`: the reader reports both and no writer stores
  either.
* `codecs` names the compression this build decodes in compressed files, in
  type order: `deflate, zlib <version>; lzvn; lzfse`, or
  `deflate, zlib <version>` alone on a big-endian host, where the imported
  decoders refuse.
* APFS is little-endian only, so the descriptor has no byte-order operation
  and the writer no byte-order setting.

Volume information reports `apfs` and the profile, the volume name as the
label, the volume UUID, the container's declared byte length `N * B`, from
which the reader core derives `missing_bytes` for a
[truncated container](#truncated-containers), its block count, block size
and free bytes from the space manager, the block size as the sector size,
UTC time basis, header scope, and `clean` false when a newer incomplete
checkpoint was passed over. Its details, in order: `volume_index`,
`volume_count`, `role`, `case_insensitive`, `norm_insensitive`,
`checkpoint_xid`, `read_xid`, `files`, `directories`, `symlinks`,
`other_objects`, `snapshots`, `volume_blocks` and `sealed`: fourteen
values, all read while opening.

## Failure classification

| Condition | Status |
| --- | --- |
| No `NXSB` magic at offset 32 of block 0, unless the identity is assumed | `FSIMG_EFORMAT` |
| Recognized but outside the profile: features in the refusal tables, encryption, Fusion, noncontiguous areas, revert or rolling state, unknown data-dependent fields or content-changing flags, names above 1022 bytes, a stored name hash that does not match on a name holding a code point unassigned in Unicode 17.0.0 or a noncharacter, link targets above 1023 bytes, LZBITMAP or unknown compression, tree height above 16 levels, sealed hash types other than SHA-256 | `FSIMG_EUNSUPPORTED` |
| Checksum, header, range, ordering, tiling, tie, hash, count, reference, repeated node on a path, tombstoned required object, namespace collision in a stored directory, or allocation inconsistency | `FSIMG_ECORRUPT` |
| A read past the end of a [truncated container](#truncated-containers) | `FSIMG_ECORRUPT`, "image is truncated", from the common read funnel |
| No `volume=` for a container with several volumes, a `volume=` naming an empty or out-of-range slot, an unknown or malformed snapshot name, any other invalid setting, a catalog name outside the writer's rules, colliding catalog names | `FSIMG_EINVAL` |
| A host clock outside the writer's [time range](#writer-values) | `FSIMG_ERANGE` |
| A layout that does not fit the exact size requested, or an automatic size not found within the [probe cap](#writer-values), which the message names | `FSIMG_ERANGE` |
| A requested size, or one the size search reaches, above the writer's largest container of 34359738368 blocks (128 TiB, [writer values](#writer-values)) | `FSIMG_ERANGE` |
| Budget, scratch, work or descriptor exhaustion | `FSIMG_ELIMIT` |

Two names with equal comparison forms in one directory are judged by who
supplied them: in an image they are damage, `FSIMG_ECORRUPT`; in a catalog
they are the caller's input, `FSIMG_EINVAL`.

The reader takes part in [salvage extraction](design-salvage.md). The
opening checks, from the probe through checkpoint and volume selection,
stay terminal. Damage met after them costs what salvage defines: a file
keeps the prefix before its first damaged extent or chunk, and a damaged
directory or entry is skipped and reported.

Diagnostics quote at most one bounded, escaped name and never node slack,
free space, attribute data or buffer contents.

## Acceptance and independent validation

| Area | Valid cases | Invalid cases |
| --- | --- | --- |
| Probe and checkpoints | Raw native containers and ones inside GPT, an Apple Partition Map and UDZO and UDBZ DMGs (a UDRW image from `hdiutil` is a raw disk); block sizes 4096 to 65536 in synthetic images; checkpoint rings with wrapped runs, several committed checkpoints and a torn newest one with block 0 no newer than the checkpoint selected, as a crash leaves it (E8); a container whose block 0 magic is rewritten, with the checksum repaired, opens with an assumed identity | Short prefix, bad checksum, wrong kind or OID, `N * B` overflow, overlapping areas, run not ending at its superblock, missing or misplaced last-map flag, overlapping mapped runs, mapped object XID above the checkpoint, equal-XID disagreement, block 0 newer than every checkpoint; the rewritten magic without an assumed identity is `FSIMG_EFORMAT`; with one, every other mutation of this row still fails, a magic rewritten without repairing the checksum among them, and `W` fails naming the assumed check; a ULFO DMG fails in the layer |
| Truncation | A container cut past everything a listing and extraction touch opens, lists, extracts and reports the missing bytes | A cut through the descriptor ring, the selected checkpoint's data run, a tree node or a file's extent fails `FSIMG_ECORRUPT` where it is reached and never selects an older checkpoint; `W`, wrap and compose fail on every cut |
| Object maps | Lookups below, at and above stored XIDs; tombstones; multi-level maps | Tombstone with an older live entry, unordered keys, duplicate keys, size not a block multiple, address past `N`, object XID different from the key's |
| B-trees | Root leaves, three or more levels, fixed and variable tables, aligned and unaligned trees, ghost records in free queues | Table overflow, key or value crossing free space or the footer, ghosts without the flag, wrong levels, repeated node on a path, unsorted keys, child key below its separator, height 17 |
| Volumes | One and several volumes, a gap in the volume array (apfs-diskutil-2147483648-e10-hole), each role E10 provides, case-insensitive and case-sensitive personalities | Missing selection with several volumes, selection of an empty slot, index mismatch, duplicate UUIDs, every refused feature bit |
| Records | Every kind in each tree, extended fields at their boundaries, names of 1, 255 and 1022 bytes and every Unicode class of E4; an inode with a Finder-information attribute sets the extra-data bit on each of its names | Truncated values, overlapping fields, misaligned field data, wrong hash or length, invalid UTF-8, embedded NUL or slash, dot names, a plain name of 1023 bytes, wrong record variant, type disagreeing with mode; a symbolic link or compressed file with only the attributes the driver consumes does not set the extra-data bit |
| Content | Files of 0, 1, 4095, 4096, 4097 bytes and above 4 GiB, many extents, holes in both forms, preallocated extents past the size, clones and their overwritten branches, every supported compression type with one and many chunks | Gaps, overlaps, unaligned lengths, extent past both the rounded size and the allocated bytes, extent past `N`, crypto ID outside the table, short or long decoded chunks, chunk table overruns |
| Links and kinds | Hard links across directories, symbolic links of 1 and 1023 bytes, FIFOs, a character and a block device with and without the device number field, a socket | Mismatched link counts, missing sibling records, directory with two parents, directory cycle, link target of 1024 bytes, the device number field on a file |
| Verification | Every native fixture the reader accepts verifies, the empty E1 containers of 1024 blocks and more among them, whose allocated blocks are exactly the fixed regions, both object maps with their trees, the empty volume's objects and the main free queue's entries; apfs-newfs-372736-plain, below the 1 MiB minimum, is refused; leaked and doubly claimed blocks, free-count mismatches and broken reference counts are found; the native zero-OID empty queue, a synthetic root leaf with no keys and both idle reaper shapes verify, and the reaping E17 copies are incomplete; a hash mismatch on a name holding U+0378 or U+FFFF is unsupported and any other mismatch corrupt | Each mutation reports its class; a report is never complete after an unsupported object or with a reaper that is not idle |
| Writer | Empty, small, deep, wide (forcing multi-level trees), large-file and nearly full containers in both personalities; names at the writer's limit; byte identity across source order, budget, scratch and time zone; poison scan of unused bytes, the bitmap bits past `N` among them, which neither verification nor `fsck_apfs` checks; an entry time outside the range is stored as the format time and counted as a loss | Colliding names under each personality, names and labels one byte over their limits, a host clock or epoch outside the time range, a non-empty placement list, sizes that do not fit |

Independent oracles, in order of authority: macOS `fsck_apfs -n`, macOS
mounting through `hdiutil attach` with file reads compared against the
manifest, and the write-after-create cycle; then the Linux checkers and
readers `apfsck`, `apfs-fuse` and `fsapfsinfo`, run as external programs on
copied images. The writer capability is registered only after its output
passes `fsck_apfs -n`, mounts, accepts native creation, modification and
deletion of files, passes `fsck_apfs -n` again after detaching and reads
back through fsimg, for both personalities and at the minimum, a small and
a multi-chunk container size. Self round trips are necessary and never
sufficient.

## Sources

| Key | Source | Standing |
| --- | --- | --- |
| A1 | Apple, *Apple File System Reference*, 2020-06-22 | Public format documentation; facts paraphrased, no prose or declarations copied |
| A2 | Apple, *File System Programming Guide*, APFS FAQ | Behavior and limits |
| A3 | macOS `newfs_apfs(8)`, `fsck_apfs(8)`, `hdiutil(1)`, `diskutil(8)` | Native oracles, run on disposable images only |
| U1 | Unicode Character Database 17.0.0 (E4): `UnicodeData.txt`, `CaseFolding.txt`, `NormalizationTest.txt` | Unicode-3.0 licensed data; generated tables and test vectors |
| Z1 | `lzfse` reference implementation | BSD-3-Clause; decoder files imported with their notice |
| L1 | `apfsprogs` (`apfsck`, `mkapfs`), `apfs-fuse`, `libfsapfs` | GPL-2.0 and LGPL-3.0 external oracles; no code, declarations or identifiers enter the tree |
| G1 | `go-filesystems/apfs` | BSD-3-Clause comparison point for structural questions; no code imported |
| D1 | [Research draft](../fsspecs/fsimg_apfs_spec.md) | Original project research; not authoritative |
