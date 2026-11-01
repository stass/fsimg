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
same. A DMG, a GPT partition or a bounded slice around an APFS container is
peeled by the layers before the driver opens it, so the driver reads a bare
APFS container whose block 0 is its superblock.

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
  plan's experiments (E1 through E16). An observation is not a universal
  format rule.
* **fsimg policy**: a deliberate validation, capability or construction rule.
  It can be stricter than macOS without redefining the format.

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
data stream. A sealed system volume authenticates its filesystem tree with a
hash tree and stores file extents in a separate tree. Encryption wraps
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
| Sealed system volume | The sealed file-extent tree and hashed nodes; the metadata integrity chain checked to the stored root hash, reported as not authenticated | Never emitted |
| Encrypted volume, Fusion container, noncontiguous checkpoint area, incomplete restore | Recognized and refused as unsupported, naming the feature | Never emitted |

The unencrypted-container reader accepts:

* version-2 containers on one device, with block sizes 4096 through 65536
  and contiguous checkpoint areas;
* any number of volumes, one of them selected;
* case-insensitive and case-sensitive volumes, with hashed or plain
  directory keys;
* regular files, directories, symbolic links and FIFOs, with hard links
  exposed as separate entries;
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
arithmetic is checked against native blocks (E1) before any fixture is read
through the driver.

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
flag (the exact type word is checked by E1). The fields the driver uses:

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
two from 4096 through 65536; `N * B`, computed checked, at most the slice
length, the excess beyond `N * B` being ignored slack; `N` at least the
container minimum of [A1], 1 MiB worth of blocks; the maximum volume count
from 1 through 100, every OID slot at or past it zero. The two areas lie
inside `[1, N)` and do not overlap each other or block 0. Bytes past offset
1408 within block 0 are not interpreted by the reader.

Feature handling is a closed table per mask:

| Mask | Known bits | Reader | Writer |
| --- | --- | --- | --- |
| Incompatible | `0x2` version 2 | Required | Emitted |
| Incompatible | `0x1` version 1, `0x100` Fusion | Unsupported | Never |
| Incompatible | Any other bit | Unsupported | Never |
| Read-only compatible | None defined | Any bit accepted for reading | Zero |
| Optional | `0x1` defragmentation, `0x2` low-capacity Fusion mode | Accepted; `0x2` with the Fusion bit only | Native default (E1) |
| Optional | Any other bit | Accepted, ignored | Never |
| Flags | `0x4` software cryptography | Accepted; informs the encryption report | Native default (E1) |

### Probe and identity

The probe's identity signature is the `NXSB` magic at offset 32 together with
object kind 1 and OID 1 in the header. The probe inspects the first 65536
bytes, the largest legal block, cut to the volume length. With the signature
present it claims the image: `FSIMG_OK` when the first `B` bytes are present
and their checksum, the geometry and the incompatible features pass;
`FSIMG_ECORRUPT` for a volume shorter than one block, a bad checksum or bad
geometry; `FSIMG_EUNSUPPORTED` for an incompatible feature outside the
table. The probe proves only that the image is an APFS container; the
checkpoint `open` selects may be newer than block 0. A GPT disk or DMG never
reaches this probe with its own header at offset 0, because the layers peel
it first.

### Checkpoint selection

The descriptor area is a ring of `D` blocks from its base. Each descriptor
block is either a container superblock or a checkpoint-mapping block (kind
12, physical). A checkpoint is the run of descriptor blocks from its
superblock's first-block index for its block count, wrapping modulo `D`;
the last block of the run is the superblock and the others are mapping
blocks. The data run is the superblock's data index and count, wrapping
modulo the data area's size.

A candidate superblock is a descriptor block whose checksum, magic, kind and
geometry are valid, whose block size, block count, UUID and areas equal
block 0's, and whose XID is nonzero. Two candidates with the same XID are an
ambiguous tie, `FSIMG_ECORRUPT`: each checkpoint writes its superblock once,
which E8 checks. A candidate's **descriptor run is whole** when:

1. the run has at least two blocks, lies in the ring, and ends at the
   candidate's own slot;
2. every mapping block in the run is valid, has the candidate's XID, and has
   a count that fits the block; exactly the run's last mapping block carries
   the last-map flag `0x1`, and no flag outside it is set;
3. every mapping names an object kind and subtype and a byte size that is a
   positive multiple of `B`;
4. the candidate's next-index fields lie within their rings and its next XID
   exceeds its XID.

Each of these depends only on one descriptor block and its slot, so one pass
over the ring records them per slot, and runs are judged from those records
without reading a block again. A slot that matches a candidate's XID matches
no other candidate's, because XIDs are distinct, so judging every run costs
at most `D` record reads plus one per candidate. The selected checkpoint is
the candidate with the largest XID whose descriptor run is whole; if no
candidate's run is whole, the image is `FSIMG_ECORRUPT`. A candidate with a larger XID whose run is not whole is a
checkpoint a crash interrupted: the reader passes over it, as macOS mounts
the newest superblock that is not malformed, and reports the volume's
`clean` state as false.

The selected checkpoint's mappings are then checked together: every mapped
block run lies inside the checkpoint's data run without straddling the data
ring's wrap, every ephemeral OID is distinct, no two mapped runs overlap,
and the space manager and reaper OIDs are mapped with their kinds. Each mapped object is
read once and must have a valid header whose OID, kind and subtype equal the
mapping's and whose XID is not above the checkpoint's, and the container
object map must be a valid physical object. A failure in this phase is
`FSIMG_ECORRUPT`: the reader does not fall back to an older checkpoint after
judging a newer one complete, and E8 checks whether macOS does. Selection
therefore reads the `D` descriptor blocks once, the selected run's mapping
blocks a second time, and one checkpoint's data run.

Block 0 does not select the checkpoint; it constrains the candidates and is
a copy of a committed superblock. A block-0 XID larger than every whole
candidate's is `FSIMG_ECORRUPT`, a rule E8 checks against the order in which
macOS writes block 0 and the descriptor ring. A noncontiguous area (bit 31
set) is `FSIMG_EUNSUPPORTED`: its tree layout has no evidence, and E1 and
E12 check that native containers use contiguous areas.

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
| 96 | 48 | Tier-2 device, all zero outside Fusion |
| 144 | 4 | Flags |
| 148 | 36 | Internal-pool transaction multiplier, block count, bitmap size and block count, bitmap base, pool base |
| 184 | 16 | Reserve block count and reserve allocation count |
| 200 | 120 | Three free queues (internal pool, main, tier 2): count, tree OID, oldest XID, node limit |
| 320 | 16 | Internal-pool bitmap ring: free head and tail, XID and bitmap offsets, free-next offset |
| 336 | 4 | Version |
| 340 | 4 | Structure size |

Its counts are tied to the geometry, so no loop over them is bounded only by
the work budget. These relations come from [A1] and are checked against
every E1 and E12 capture before the driver enforces them: the main device's
block count is `N` and its chunk count
`ceil(N / (8 * B))`; chunks per chunk-information block are
`floor((B - 40) / 32)` and chunk-information blocks per address block
`floor((B - 40) / 8)`; the chunk-information block count is
`ceil(chunks / chunks per block)`; the address-block count is zero when the
chunk-information addresses fit the object, as E1 and E12 establish, and
otherwise `ceil(chunk-information blocks / per address block)`; the address
array lies inside the object, whose mapped size must be the smallest whole
number of blocks holding the structure and that array. The internal-pool and
tier-2 fields are consistent with the same rules. Any other value is
corrupt.

A chunk-information block (kind 7) holds an index, a count and 32-byte
records: last-modifying XID, first block, block count, free count and bitmap
block. A chunk-information address block (kind 6) holds an index, a count and
block numbers of chunk-information blocks. The main device's address array
names chunk-information blocks when its address-block count is zero and
address blocks otherwise. Each bitmap block covers one chunk, bit `i` of
byte `j` (least significant first) standing for block `8j + i` of the chunk.
The internal pool is a block range reserved as a whole in the main bitmaps,
from which the space manager allocates its own blocks, tracked by the pool's
bitmap ring; which space-manager blocks live inside it is an observation
(E1, E12). A free queue is an ephemeral B-tree whose keys are
`(XID, block)` and whose value is a block count, absent in a ghost record
meaning one block. The reaper (kind 17) holds the state of deferred
deletions; the reader validates its header and requires no reaping in
progress only where verification states so.

How the native formatter sizes the internal pool, the checkpoint areas and
the address arrays, whether a fully free chunk has a bitmap block, and how
bits past `N` in the last chunk are set are observations (E1, E12) that the
verifier's and writer's tables record.

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
`[1, N)`, and no flag outside `{saved}`; encrypted is unsupported and
no-header is unsupported, since the sealed trees fsimg reads are physical
and never mapped (E11 checks this). The object read at the address must
satisfy the header rules with its OID equal to `o` and its XID equal to the
found key's XID. A miss for a reference the format requires is
`FSIMG_ECORRUPT`.

Object-map flags follow a closed table:

| Flag | Container object map | Volume object map |
| --- | --- | --- |
| `0x1` manually managed (no snapshot support) | Required | Forbidden |
| `0x2` encrypting, `0x4` decrypting, `0x8` key rolling | Unsupported | Unsupported |
| `0x10` crypto generation | As E1 shows | As E1 shows |
| Any other bit | Unsupported | Unsupported |

The snapshot count and the most recent snapshot XID must agree with the
volume's snapshot tree, and the container map has no snapshots. A nonzero
pending revert range is unsupported. The writer emits the flags of the
native empty container.

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
| 32 | 2 | Node flags: root `0x1`, leaf `0x2`, fixed key and value size `0x4`, hashed `0x8`, no header `0x10` |
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
A hashed index value appends the child's digest to its OID. A value offset of
`0xffff` is a ghost, legal only in a tree whose flags allow ghosts.

### Validation

Every node a reader visits passes these checks before any entry is used:

* header and checksum, unless the tree is a sealed tree with no-header nodes;
* node flags consistent with its position: root exactly at the tree's top,
  leaf exactly at level 0, fixed-size flag equal to the tree's, no unknown
  flag;
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
* every key and value length within its record codec's minimum and maximum;
* keys strictly increasing under the tree's comparator; the first key of a
  child not below the separator naming it, and every key of a child below the
  next separator in the parent.

A node's address is never revisited on one root-to-leaf path, and the height
bound with the per-tree key-count and node-count work charge bounds a scan.
The footer's longest-key, longest-value and count fields are statistics: they
are never used to size a buffer, and full verification checks the counts.

### Comparators

| Tree | Key order |
| --- | --- |
| Object map | OID, then XID |
| Filesystem | Owner (low 60 bits), then kind (top 4 bits), then the kind's suffix: hashed directory record by its hash and length word, then name bytes; plain directory record, extended attribute and snapshot name by name bytes; file extent by logical offset; sibling link by sibling ID; file information by its packed word |
| Extent reference | Block, then kind |
| Snapshot metadata | XID, then kind, then name bytes for names |
| Free queue | XID, then block |
| Sealed file extents | Private ID, then logical offset |

Integer fields compare numerically, never as serialized bytes. How names of
different lengths compare, and whether the hashed record's whole 32-bit word
or only its hash bits order the records, are evidence questions (E4); the
reader's ordering checks use the observed rule, so a wrong guess would make
valid images fail rather than misread.

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
are the ones [A1] gives for each tree: the root tree virtual (physical in a
sealed volume), the others physical. A nonzero revert-to XID or superblock
OID, a nonzero encryption-rolling state, or a nonzero root-to XID is a
transition native tools complete before use and is `FSIMG_EUNSUPPORTED`.

| Mask | Bits | Reader | Writer |
| --- | --- | --- | --- |
| Incompatible | `0x1` case-insensitive, `0x8` normalization-insensitive | Accepted; select the name rules | Case-insensitive: `0x1`; case-sensitive: `0x8` if native (E2) |
| Incompatible | `0x2` dataless snapshots | Accepted for the live view; a snapshot view of such a volume is unsupported | Never |
| Incompatible | `0x4` encryption rolled | Accepted only with the unencrypted flag | Never |
| Incompatible | `0x10` incomplete restore | Unsupported | Never |
| Incompatible | `0x20` sealed | The sealed profile | Never |
| Incompatible | Any other bit | Unsupported | Never |
| Read-only compatible | None defined | Any bit accepted for reading | Zero |
| Optional | `0x1`, `0x4` defragmentation, `0x2` hard-link map records, `0x8` strict access time, `0x10` volume-group inode space | Accepted | Native default (E2) |
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
and a container need not hold a bootable set. A System and a Data volume
sharing a volume-group UUID are separate volumes; the reader presents each
on its own namespace and does not follow firmlinks.

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
and purgeable directories, and no directory record names one (E3 checks
native volumes). Ordinary inodes are at least 16, or at least
`0x0800000000000000` in a System volume whose volume-group inode space is
enabled. The internal flags are accepted from a closed table
established from [A1] and E3 to E6; a flag that changes how content is read
and is not implemented, such as one marking data as dataless, is
unsupported.

| Mode type | Directory record type | Entry kind | Content |
| --- | --- | --- | --- |
| `0100000` | 8 | `FSIMG_ENTRY_FILE` | Data stream, or decompressed data when compressed |
| `0040000` | 4 | `FSIMG_ENTRY_DIRECTORY` | Directory records |
| `0120000` | 10 | `FSIMG_ENTRY_SYMLINK` | Target from the symbolic-link attribute |
| `0010000` | 1 | `FSIMG_ENTRY_FIFO` | None |
| `0020000`, `0060000`, `0140000` | 2, 6, 12 | unsupported | Device or socket: the directory's preparation fails naming it, under the [metadata contract](design-metadata.md#entry-kinds) |
| other | | | `FSIMG_ECORRUPT` |

A directory record's type must equal its inode's mode type. Entry metadata
sets `FSIMG_META_OWNER`, `FSIMG_META_MODE` with the low twelve mode bits, and
all four timestamps. A time counts nanoseconds since the Unix epoch: seconds
are the floor of its quotient by `10^9`, nanoseconds the remainder, the
basis is UTC and the precision one nanosecond. The reference declares the
fields unsigned; whether macOS stores times before 1970 as two's-complement
values is checked by E14, and the reader and writer read the field as
signed exactly when E14 shows it. Attributes are absent. BSD flags, the
protection class and the remaining fields have no common representation and
are not reported; the `UF_COMPRESSED` flag `0x20` selects
[compression](#transparent-compression).

### Extended fields

Inode and directory-record values may end in an extended-field blob: a u16
field count, a u16 count of used data bytes, then four bytes per field (type
u8, flags u8, data size u16), then the field data, each field's data
starting at a multiple of 8 bytes from the start of the data area. Counts,
sizes and padding must fit the record exactly; the used-data count must
equal the padded sum. A field type appears at most once. The meaning of a
type depends on the record kind. Inode fields the reader decodes: the data
stream (type 8, 40 bytes), the name (type 4, NUL-terminated UTF-8), the
sparse byte count (type 13) and the device number (type 14). A directory
record's sibling ID is type 1. Other known types are validated by size and
ignored; an unknown type with a valid size is ignored when its flags do not
mark it data-dependent, and is unsupported otherwise. Padding bytes are
zero.

### Directory records and names

A directory record's owner is its parent directory's inode. Its key suffix is
either plain, a u16 length and the name with its NUL, or hashed, a u32 word
whose low 10 bits are the name's length with its NUL and whose high 22 bits
are the name hash, then the name with its NUL. A volume that is
case-insensitive or normalization-insensitive uses hashed keys and others
plain keys (E2); a record of the other variant is corrupt. The value is the
child inode (u64), the time the entry was added (u64), u16 flags whose low
four bits are the type, and extended fields.

A name is the stored bytes without the NUL: nonempty, valid UTF-8 (E14
checks that macOS refuses other byte strings), without NUL or `/`, and
neither `.` nor `..`; anything else is corrupt. A name is at most 1022
bytes, the most a hashed key's length field holds with the NUL; a longer
plain name is `FSIMG_EUNSUPPORTED`. macOS limits names to 255 characters
(E14 measures the unit), so native names stay well below that bound. The reader declares no
[omitted-names profile](design-directories.md#omitted-names): every valid
name is delivered, including names with control characters such as Finder's
`Icon\r`, and the CLI escapes them for display as it does every name.

The comparison form of a name depends on the volume:

* case-insensitive: the canonical caseless form, the canonical
  decomposition (NFD) of the case folding of the name's NFD;
* case-sensitive and normalization-insensitive: the NFD of the name;
* otherwise: the name's bytes.

The hash is computed from the comparison form as a sequence of code points:
CRC-32C (the Castagnoli polynomial, reflected) over the code points as
little-endian 32-bit units, with the conventions [A1] gives for the initial
value, the final complement and the terminating NUL, keeping the low 22
bits. E4 settles, with test vectors, those conventions, the order of
folding and decomposition, whether case folding is full or simple, how CJK
compatibility ideographs decompose, and which Unicode version macOS
applies, before the driver checks any hash. The tables are generated from
the Unicode Character Database at the version E4 establishes, with their
Unicode-3.0 license notice, by the same generator family as the FAT tables;
the profile name carries the version.

The reader checks each hashed record's length and hash, and the namespace key
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

The common entry model has no extended attributes, so the reader exposes
none: Finder information, resource forks, quarantine flags and other
attributes are not listed, extracted or counted as losses, a limitation the
user documentation states. The driver reads three names itself:
`com.apple.fs.symlink`, `com.apple.decmpfs` and `com.apple.ResourceFork`.
Verification validates every attribute record.

### Hard links and siblings

Several directory records may name one non-directory inode, whose link count
counts them. Each name of an inode with several names has a sibling-link
record under the inode (sibling ID, parent inode, name), and its directory
record carries the sibling ID; a volume with the hard-link map feature also
has a sibling-map record under each sibling ID naming the inode (E5). The common model has no hard-link
representation, so the reader delivers every name as its own entry with the
inode's metadata and content; a file's content is read through each name
independently. Verification checks that the link count, the directory
records, the sibling links and the sibling maps agree.

A directory has exactly one name. A directory inode's parent must be the
directory whose record names it, and the root's parent is 1. This makes a
directory cycle impossible: the first directory to repeat on a path would
need two parents. A directory inode also carries the name extended field,
which must equal the name of the record naming it (E3 checks that native
directories always carry it), so a second record naming the directory under
another name in the same parent is corrupt when it is replayed, and a second
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
reference count.

The extents of a stream tile a prefix of its logical range in logical order,
without gaps or overlap: each starts where the previous one ends, and each
length is a positive multiple of `B`. No extent extends past the larger of
the size rounded up to whole blocks and the data-stream field's allocated
bytes, which exceed the rounded size when space is preallocated; mapped
extents, not holes, are what the allocated bytes count (E3 and E6 check
native preallocation and sparse accounting). Logical bytes
past the last extent and below the size read as zero, as do extents whose
physical block is zero, which mark holes (E6 checks both representations).
A mapped extent's blocks lie inside `[1, N)`. Extent flags must be zero, and
crypto IDs follow the closed table E3 and E6 establish for unencrypted
volumes. Content past the size, including preallocated extents, is never
delivered. Two streams may name the same blocks: that is a clone, legal
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
are new; how a snapshot's tree records update kinds and counts is checked
by E9.

### Symbolic links

A symbolic link's target is the `com.apple.fs.symlink` attribute, owned by
the filesystem, inline, ending in one NUL that is not part of the target.
The target is 1 to 1023 bytes, a bound E14 checks; a longer stored target
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

| Type | Storage | Codec | Reader |
| --- | --- | --- | --- |
| 1 | Inline | None | Copied |
| 3 | Inline | zlib | System zlib |
| 4 | Resource fork | zlib chunks | System zlib |
| 7 | Inline | LZVN | Imported decoder |
| 8 | Resource fork | LZVN chunks | Imported decoder |
| 9, 10 | Inline, resource fork | Uncompressed | Copied (E7) |
| 11 | Inline | LZFSE | Imported decoder |
| 12 | Resource fork | LZFSE chunks | Imported decoder |
| 13, 14 | Inline, resource fork | LZBITMAP | Unsupported: no public decoder |
| other | | | Unsupported |

**Inline types.** The compressed bytes follow the header in the attribute.
Each inline codec marks stored-uncompressed data with a leading byte, which
E7 records per codec. Inline content is decoded whole into one 64 KiB
buffer: an inline type's uncompressed size is at most 64 KiB, which E7
checks against native files, and a larger one is unsupported.

**Resource-fork types.** The compressed bytes are in the
`com.apple.ResourceFork` attribute, read as a stream. The zlib type wraps
its chunk table in the classic resource-fork format, whose header is
big-endian, while the chunk table inside it is little-endian; the LZVN and
LZFSE types begin the attribute with a little-endian table of chunk
offsets. E7 tabulates both framings field by field. The table gives each
64 KiB chunk's compressed range; every chunk decodes to exactly 64 KiB
except the last, which decodes to the remainder. A short or long result, an
out-of-range or overlapping chunk, a table that does not cover the size, or
input left after a chunk's end marker (unless E7 shows macOS pads chunks) is
corrupt.

Decoded bytes count toward the extraction output limit like any other
content, and that limit defaults to the volume's length, so extracting a
volume whose compressed files, holes, clones or hard links expand past the
container needs an explicit `-l output`, as the
[security design](design-security.md#safe-extraction) states.

**Delivery.** Delivery receives decoded bytes one chunk at a time as
[held extents](design-content.md), so memory is one 64 KiB output and one
compressed chunk regardless of file size, and decoding work is charged per
byte produced. The LZVN and LZFSE decoders are Apple's BSD-3-Clause
reference code imported under the
[codec design](design-codecs.md#imported-lzfse-and-lzvn-decoders).

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
OID and storage class, are checked by E9 before the profile is enabled; the
reader otherwise reads exactly as for the live volume. A snapshot of a
dataless-snapshot volume is unsupported. Snapshot flags and extended
metadata are accepted from a closed table (E9). The writer emits an empty
snapshot tree.

## Sealed volumes

A sealed volume has the sealed incompatible bit, a nonzero integrity
metadata OID, and a nonzero sealed file-extent tree. Its filesystem tree is
physical and hashed: each index value carries the digest of its child, and
leaf nodes may have no header (the tree types and node flags are checked by
E11). Its file extents are in the sealed file-extent
tree, keyed by private ID and logical offset, instead of the filesystem tree.
The integrity metadata object (kind 30) holds a version, flags, a hash type
(1 SHA-256, 2 SHA-512/256, 3 SHA-384, 4 SHA-512), the offset of the root hash
within the object, and the XID at which the seal was broken, if any.

The sealed profile reads names and content through the sealed tree. When the
seal is intact (a zero broken-seal XID), it checks every visited node's
digest against its parent and the root's against the stored root hash with
SHA-256, the hash fsimg already carries; other hash types are unsupported. A
successful check covers the filesystem metadata only, relative to the
stored root: file content is covered by the file-information records'
hashes, which fsimg does not check, and no authenticity claim is possible
without a trusted root, so the information never reports one. A broken seal
is reported through the `seal_broken_xid` detail and the volume is read
without digest checks, because nodes written after the break no longer
match their parents' digests.

## Refused features

Encryption, a Fusion container, a container whose checkpoint descriptor or
data area is noncontiguous, an incomplete restore, a pending revert, and
object or record flags marking encryption are recognized and refused with
`FSIMG_EUNSUPPORTED`, naming the feature in the error message. The draft's
`KEY_REQUIRED` and hardware-key states are diagnostics of that one status:
fsimg has no key input, so the distinction would change no caller's action.
Encrypted bytes are never decoded as metadata or delivered as content.

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
* each non-directory's link count equals its names, which its sibling links
  and sibling maps match;
* every inode other than the root and private directories is named by a
  directory record, or lies in the private directory, which E3 checks is
  where native volumes place unnamed inodes;
* each data-stream ID's reference count equals the inodes and attributes
  using it, and every file extent belongs to a live stream;
* every attribute is well formed, and names equivalent under the volume's
  rules never share a parent;
* the volume superblock's object counts equal the counted inodes by kind.

**Allocation.** Every block is claimed exactly as the space manager says.
Claims in the main domain come from block 0 and the checkpoint areas; the
internal pool, as one range; every object-map node and every live object
version an object map maps, tombstones excluded, including the versions
snapshots retain; every physical tree node, including snapshot trees; the
reaper's lists; every entry of the main free queue; and the data extents
described below.

The internal pool is a domain of its own. Space-manager blocks inside it
and the entries of the internal-pool free queue are claims in that domain,
never in the main one, and the pool's bitmap must be set exactly for them.
Space-manager blocks outside the pool are main claims.

Data blocks are claimed through the physical-extent records of every
extent-reference tree of a volume, the live tree and each snapshot's. In a
sealed volume, whose file extents live in the sealed file-extent tree,
those extents are checked against the reference records in the same way,
and whether a sealed volume has an extent-reference tree at all is checked
by E11; without one, its sealed file extents are its data claims.
Records of one tree never overlap; records of different trees of one
volume may, since a snapshot and the live volume share unchanged extents,
so a block a snapshot alone still uses is claimed by that snapshot's
record. Every mapped file extent of the live filesystem tree lies inside a
live physical-extent record, and file extents overlap one another only
inside one such record, whose reference count equals the live streams
sharing it. How a snapshot's references enter the counts is checked by E9.

Metadata claims are exclusive, and data claims never overlap metadata. A
block is allocated in the main bitmap exactly when it is claimed; each
chunk's free count and the device's free count equal the clear bits; bits
past `N` follow the observed rule (E1). Each volume's allocated block count
follows the observed accounting (E3).

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
transaction. It emits:

* block size 4096, one checkpoint, XID 1 for every object unless E1 shows a
  native requirement otherwise, and the native checkpoint-area sizes for the
  container's size;
* one volume in slot 0, unencrypted, role none, named by the label;
* case-insensitive by default; case-sensitive with `case=sensitive`, with
  the native normalization flag;
* the native-required records of an empty volume: the root and private
  directories, and every other record E1 shows `newfs_apfs` writing;
* for each regular file: an inode with a name field and a data-stream field,
  a data-stream ID record with count 1, and, when nonempty, one file extent
  over a contiguous run of blocks and one physical-extent record with count
  1 owned by the stream;
* for each directory: an inode with a name field and child count; for each
  symbolic link: an inode with mode `0120755` unless the entry gives a mode,
  and an inline filesystem-owned `com.apple.fs.symlink` attribute; for each
  FIFO: an inode with no data stream;
* a directory record in the parent for every entry, in the key variant the
  volume's flags select, with the type of its kind;
* an empty extent-reference tree when there are no files, an empty snapshot
  tree, empty free queues and an idle reaper in the native form;
* the space manager's chunk structures and bitmaps marking exactly the
  allocated blocks.

Every file's blocks are allocated in full, as the
[architecture](architecture.md#output-and-determinism) requires of every
writer, so the writer emits no holes. It emits no clones, compression,
extended attributes, hard links, snapshots, sealing, encryption or second
volume. A catalog name is checked against the volume's rules: the catalog
already refuses exact duplicates, and the writer refuses two siblings whose
comparison forms are equal, naming both, with `FSIMG_EINVAL`. A name longer
than 255 bytes or outside the reader's name rules fails the same way; E14
measures whether macOS's own limit counts bytes or characters, and the
writer's limit follows the bytes macOS accepts.

### Settings and identities

| Setting | Field | Meaning |
| --- | --- | --- |
| `-L` | `label` | Volume name, UTF-8 of at most 255 bytes; NULL selects `Untitled` |
| `uuid=` | `volume_uuid` | Volume UUID in text order. All zero is refused outside reproducible mode; in reproducible mode it is derived from the epoch by the [layer identifier rule](design-layers.md#identifiers) with role 5. Without `-R` and `uuid=` the CLI supplies a random version 4 UUID |
| `case=` | `case_sensitive` | `insensitive`, the default, or `sensitive` |
| `cluster=` | `allocation_unit_bytes` | Zero or 4096 |
| `sector=` | `sector_bytes` | Zero or 4096: APFS addresses whole blocks |

The container UUID is derived from the volume UUID: the first 16 bytes of
the SHA-256 of the ASCII bytes `fsimg-apfs-container` followed by the volume
UUID, with the version nibble set to 8 and the variant bits to `10`. It is
therefore distinct from the volume UUID, reproducible, and needs no setting.
Owner and group default to 0 and modes to `0755` for directories and
symbolic links and `0644` for files and FIFOs, under the
[metadata policy](design-metadata.md#writer-policy). A time is stored in
nanoseconds; an absent time takes the format time, the planning clock or, in
reproducible mode, the epoch, and a time outside the field's range, which
E14 decides as the [inode rules](#inodes) state, is a loss under the
policy. Directory-record add times, the
superblock times and the formatting record's time are the format time; the
formatting identifier is `fsimg` followed by the package version.

### Layout

The layout is a pure function of the catalog and the options. Every record's
encoded length depends only on the catalog: physical block numbers and
counts have fixed widths. Planning can therefore pack every tree and count
its nodes before any block is placed, then place metadata and data, and
emission fills in the addresses without changing a node's contents
otherwise. The order of regions follows the native empty container (E1): block 0,
the checkpoint areas, the space manager's internal pool, then the container
object map, the volume's superblock, object map, filesystem tree,
extent-reference tree and snapshot tree, then file data in catalog order,
each file one run. Trees are built bottom-up from their sorted records, each
node filled as far as its free space allows, with virtual OIDs and
object-map records assigned in node order. Every allocated block is a prefix
of the container plus the native fixed regions, so the bitmaps are runs of
ones and zeros.

Automatic sizing chooses the smallest block count that holds the layout,
the native minimum container size, the requested free space and the native
reserve, if any (E1); the [sizing design](design-sizing.md#dimensions-per-format)
states the dimensions. The maximum volume count follows the native rule for
the container's size (E1).

## Information and capabilities

The capability record names `apfs` with the profile `apfs-unicode-<version>`,
files, directories, symbolic links and FIFOs, owner, mode and all four
timestamps; block sizes 4096 through 65536 for reading and 4096 for writing,
in both the sector and allocation-unit masks since APFS addresses whole
blocks;
`max_label_bytes` 255; `max_link_target_bytes` 1023; `max_name_utf16_units`
255; `write_settings` with the volume UUID and case sensitivity; and
`read_settings` with the volume index and snapshot.

Volume information reports `apfs` and the profile, the volume name as the
label, the volume UUID, the container's byte length, block count, block size
and free bytes from the space manager, the block size as the sector size,
UTC time basis, header scope, and `clean` false when a newer incomplete
checkpoint was passed over. Its details, in order: `volume_index`,
`volume_count`, `role`, `case_insensitive`, `norm_insensitive`,
`checkpoint_xid`, `read_xid`, `files`, `directories`, `symlinks`,
`other_objects`, `snapshots`, `volume_blocks`, `sealed` and
`seal_broken_xid`: fifteen values, all read while opening.

## Failure classification

| Condition | Status |
| --- | --- |
| No `NXSB` signature | `FSIMG_EFORMAT` |
| Recognized but outside the profile: features in the refusal tables, encryption, Fusion, noncontiguous areas, revert or rolling state, unknown data-dependent fields or content-changing flags, device or socket entries, names above 1022 bytes, link targets above 1023 bytes, LZBITMAP or unknown compression, tree height above 16 levels, sealed hash types other than SHA-256 | `FSIMG_EUNSUPPORTED` |
| Checksum, header, range, ordering, tiling, tie, hash, count, reference, tombstoned required object, namespace collision or allocation inconsistency | `FSIMG_ECORRUPT` |
| No `volume=` for a container with several volumes, a `volume=` naming an empty or out-of-range slot, an unknown or malformed snapshot name, any other invalid setting, a name outside the writer's rules, colliding names | `FSIMG_EINVAL` |
| Budget, scratch, work or descriptor exhaustion | `FSIMG_ELIMIT` |

Diagnostics quote at most one bounded, escaped name and never node slack,
free space, attribute data or buffer contents.

## Acceptance and independent validation

| Area | Valid cases | Invalid cases |
| --- | --- | --- |
| Probe and checkpoints | Raw, GPT and DMG-wrapped native containers; block sizes 4096 to 65536 in synthetic images; checkpoint rings with wrapped runs, several committed checkpoints and a torn newest one (E8) | Short prefix, bad checksum, wrong kind, geometry past the slice, overlapping areas, run not ending at its superblock, missing or misplaced last-map flag, overlapping mapped runs, mapped object XID above the checkpoint, equal-XID disagreement, block 0 newer than every checkpoint |
| Object maps | Lookups below, at and above stored XIDs; tombstones; multi-level maps | Tombstone with an older live entry, unordered keys, duplicate keys, size not a block multiple, address past `N`, object XID different from the key's |
| B-trees | Root leaves, three or more levels, fixed and variable tables, aligned and unaligned trees, ghost records in free queues | Table overflow, key or value crossing free space or the footer, ghosts without the flag, wrong levels, repeated node on a path, unsorted keys, child key below its separator, height 17 |
| Volumes | One and several volumes, each role E10 provides, case-insensitive and case-sensitive personalities | Missing selection with several volumes, empty slot, index mismatch, duplicate UUIDs, every refused feature bit |
| Records | Every kind in each tree, extended fields at their boundaries, names of 1 to 255 bytes and every Unicode class of E4 | Truncated values, overlapping fields, misaligned field data, wrong hash or length, invalid UTF-8, embedded NUL or slash, dot names, wrong record variant, type disagreeing with mode |
| Content | Files of 0, 1, 4095, 4096, 4097 bytes and above 4 GiB, many extents, holes in both forms, preallocated extents past the size, clones and their overwritten branches, every supported compression type with one and many chunks | Gaps, overlaps, unaligned lengths, extent past both the rounded size and the allocated bytes, extent past `N`, crypto ID outside the table, short or long decoded chunks, chunk table overruns |
| Links | Hard links across directories, symbolic links of 1 and 1023 bytes, FIFOs | Mismatched link counts, missing sibling records, directory with two parents, directory cycle, link target of 1024 bytes |
| Verification | Every native fixture verifies; leaked and doubly claimed blocks, free-count mismatches and broken reference counts are found | Each mutation reports its class; a report is never complete after an unsupported object |
| Writer | Empty, small, deep, wide (forcing multi-level trees), large-file and nearly full containers in both personalities; byte identity across source order, budget, scratch and time zone; poison scan of unused bytes | Colliding names under each personality, oversized names and labels, times outside the range, sizes that do not fit |

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
| U1 | Unicode Character Database at the version E4 establishes: `UnicodeData.txt`, `CaseFolding.txt`, `NormalizationTest.txt` | Unicode-3.0 licensed data; generated tables and test vectors |
| Z1 | `lzfse` reference implementation | BSD-3-Clause; decoder files imported with their notice |
| L1 | `apfsprogs` (`apfsck`, `mkapfs`), `apfs-fuse`, `libfsapfs` | GPL-2.0 and LGPL-3.0 external oracles; no code, declarations or identifiers enter the tree |
| G1 | `go-filesystems/apfs` | BSD-3-Clause comparison point for structural questions; no code imported |
| D1 | [Research draft](../fsspecs/fsimg_apfs_spec.md) | Original project research; not authoritative |
