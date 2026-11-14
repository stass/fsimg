# APFS driver

## Scope

The [APFS format design](design-apfs.md) establishes the on-disk facts, the
profiles and the evidence gates, and owns every on-disk value. This document
maps them onto code: the modules of `lib/formats/apfs/`, the driver state
behind the common reader and writer, the algorithms that keep checkpoint
selection, object resolution, tree traversal, verification, planning and
emission bounded, and the tests that qualify them. Where an algorithm needs
a value it links to the format design, whose
[writer values](design-apfs.md#writer-values) hold everything the writer
emits, with the experiment that closes each provisional one.

The driver implements the common contracts without exceptions: the
[format descriptor and probe](design-formats.md#format-descriptors-and-profiles)
with its division of identity from validation,
[prepared directories](design-directories.md),
[logical content](design-content.md) including its zero extents, held
extents and decoded link targets, the
[truncated-volume rule](architecture.md#byte-and-object-boundaries),
[salvage extraction](design-salvage.md), [emission](design-emission.md),
[sizing](design-sizing.md) and the
[POSIX metadata extension](design-metadata.md) with its extra-data bit.
Common code gains no APFS-specific branch. The volume and snapshot read
settings reach the driver through the read options under the capability
record's `read_settings`, as write settings do through `write_settings`.

## Modules

| File | Owns |
| --- | --- |
| `apfs.h` | Private types, constants and prototypes shared by the files below |
| `descriptor.c` | `fsimg_apfs_format`, added to the static list in `lib/formats/format.c`: the `apfs` variant, its capability record and `codecs` operation, the probe, and the block-0 validation the probe and `open` share |
| `codec.c` | Fletcher-64 and pure decoders and encoders of the object header, container and volume superblocks, checkpoint map, object map, node header and footer, space-manager records, reaper, filesystem keys and values and extended fields; no I/O |
| `checkpoint.c` | The descriptor-ring scan, candidate completeness, selection, and the ephemeral mapping table |
| `object.c` | `apfs_object_read()`: the one function that turns a physical, virtual or ephemeral reference into a validated object in a caller's window |
| `btree.c` | Node validation, the cursor and the comparators |
| `omap.c` | Object-map lookup at an XID over a cursor |
| `volume.c` | The volume array, volume selection, superblock validation, snapshot selection and the read XID |
| `unicode.c` | Name rules; UTF-8 decoding to code points, canonical decomposition with canonical reordering, and case folding, over the generated tables; the comparison form under the volume's rule, the name hash, namespace keys |
| `unicode-tables.h` | The generated decomposition, combining-class and case-folding tables, and their input digests |
| `inode.c` | Inode and extended-field decoding, the attribute scan with the extra-data mark, record-to-entry mapping, data-stream lookups |
| `directory.c` | `prepare`, `replay`, `keys` and `key`; the parent rule for directories |
| `content.c` | `resolve`: file extents, holes and the dispatch to `compress.c` |
| `compress.c` | The compression types and their rules, inline payloads, the reads of a resource fork through its file extents, the decode window, and calls to the shared framing validator, zlib and the imported decoders |
| `read.c` | `fsimg_apfs_reader_ops`: open, close and volume information |
| `verify.c` | Container-wide verification: claims, reconciliation streams, the bitmap merge and the walk |
| `spaceman.c` | `apfs_fixed_regions()`, the one list of fixed regions that verification claims and the writer lays out; the space-manager pager over chunk-information blocks and bitmaps, for verification |
| `write-apfs.h` | Plan rows, tree profiles and the plan object |
| `write-plan.c` | `validate`, `plan`, `free`: settings, the entry pass, record generation, sizing and layout |
| `write-btree.c` | The bulk B-tree packer that planning and emission both run, so node boundaries cannot disagree |
| `write-emit.c` | `emit`: payload copies, trees, object maps, superblocks, space manager and checkpoint |

`object.c` is the only place that interprets an object reference and
`btree.c` the only place that interprets a node, so header, checksum,
storage-class, range, ordering and height checks exist once.
`write-btree.c` is the only place that decides where a node ends. Reader and
writer share the record codecs, the name functions, the layout constants of
`apfs.h` and the list of
[fixed regions](design-apfs.md#ephemeral-objects-and-the-space-manager),
and no logic
beyond them: node validation knows nothing of the packer, so reading the
writer's output back checks the packer against an independent statement of
the node rules.

The driver reuses facilities that exist outside it:

* `lib/core/crc32c.[ch]`: `fsimg_crc32c_update()`, the bare register update,
  to which `unicode.c` applies the conventions the format design gives the
  [name hash](design-apfs.md#directory-records-and-names).
* `lib/core/sha256.[ch]`: the sealed profile's digests and the derived
  container UUID.
* `lib/codec/`: the imported LZFSE and LZVN decoders behind the
  [codec design's](design-codecs.md#imported-lzfse-and-lzvn-decoders)
  interface, `fsimg_lzfse_decode()` and `fsimg_lzvn_decode()`, which the HFS
  Plus driver calls too.
* The system zlib, through the common allocation adapter.
* `lib/formats/decmpfs.[ch]`: the framing validator of
  [transparent compression](#compression-framing), shared with the HFS Plus
  driver.

Two things are the driver's own. Fletcher-64 lives in `codec.c`, since no
other format uses it. The Unicode tables are generated into
`unicode-tables.h` by `tools/gen-apfs-tables.rb` from the Unicode Character
Database at the version E4 establishes, with the digests of the pinned
inputs recorded in `docs/references.md`, as the format design
[states](design-apfs.md#directory-records-and-names). HFS Plus owns its
tables the same way and the two are not shared: the formats apply different
Unicode versions and folding rules, so one owner would hold two table sets
behind one name.

### Compression framing

The `com.apple.decmpfs` header and the chunk tables of a resource fork are
one macOS format, which HFS Plus and APFS both store, so one validator owns
them: `lib/formats/decmpfs.[ch]`, beside `format.[ch]`, private to
libfsimg. It is pure. It holds no state, allocates nothing and knows no
fork: it reads the fork's bytes through a callback,
`read(context, offset, buffer, length, error)`, which each driver backs
with its own way to the stored bytes, a fork's extent index in HFS Plus and
file-extent records under the content cursor here. Its memory is the bytes
it has in hand, at most three 8-byte table entries. It offers:

* the header decoder: the magic, the compression type and the uncompressed
  size;
* `open`: given the type's table kind, the fork's length and the file's
  size, it checks the resource header's relations to the fork, or the
  offset table's first word, and that the table holds exactly the chunks
  the size needs and ends inside the fork, and returns the table's
  description: its offset, the base chunk offsets count from, the payload
  interval and the chunk count;
* `chunk`: given that description and a chunk's index, it reads the entry
  with its neighbours and checks that the stored range lies in the payload
  interval, is no longer than the stored bound and does not overlap its
  neighbours, and returns the range.

A rejection is `FSIMG_ECORRUPT` with a message that names the framing and
no format, and a failure of the callback passes through unchanged. The
module owns the five decoders of the framing, for the header, the resource
header, the resource table, a pair-table entry and a word-table entry, with
their types and length constants, and the relation checks of `open` and
`chunk`. Each driver's `compress.c` owns the rest: which types the format
admits, how the stored bytes are reached, the decode window, the dispatch
of a chunk to its decoder with the inline markers, and the work charges.
The [plan](../docs/apfs-plan.md#common-changes) names the HFS Plus
functions the module is built from.

That the APFS framing is the HFS Plus one is provisional (E7), as the
format design [states](design-apfs.md#transparent-compression). If E7 shows
a difference, `compress.c` keeps a validator of its own for the part that
differs and calls the shared one for the rest; a type E7 finds no specimen
of stays unsupported.

## Codecs

Every APFS integer is little-endian and decoded with the shared
[scalar codecs](design-binary-primitives.md) after its span is proved; the
big-endian resource-fork header of a compressed file is decoded by the
shared [framing validator](#compression-framing), so no file of the driver
reads a big-endian field.
Decoded records are C structures with `_t` typedefs holding validated
values only:

| Type | Fields |
| --- | --- |
| `apfs_obj_t` | checksum, OID, XID, kind, storage class, flags, subtype |
| `apfs_nx_t` | block size, block count, the three feature masks, UUID, next OID and XID, both area descriptions and this checkpoint's runs, space manager, object map and reaper OIDs, maximum volume count, volume OID array |
| `apfs_cpm_t` | kind, subtype, byte size, volume OID, ephemeral OID, address |
| `apfs_omap_t` | flags, snapshot count, tree type and OID, snapshot tree type and OID, most recent snapshot |
| `apfs_node_t` | node flags, level, key count, the four locations, and for a root the footer |
| `apfs_spaceman_t` | the geometry counts, the main device's counts and address-array offset, the internal pool's fields, the three free queues |
| `apfs_reaper_t` | the reap IDs, list head, tail and count, flags, and the fields of an object being reaped |
| `apfs_vol_t` | index, the three feature masks, flags, role, UUID, name, tree types and OIDs, counts, group UUID, integrity and sealed-tree OIDs |
| `apfs_inode_t` | inode number, parent, private ID, four times, internal and BSD flags, count, owner, group, mode, uncompressed size, decoded extended fields |
| `apfs_drec_t` | parent, name span, stored hash and length, child, add time, type, sibling ID |
| `apfs_xattr_t` | owner, name span, flags, and the inline data span or the stream reference |
| `apfs_extent_t` | private ID, logical offset, length, physical block, flags, crypto ID |

Encoding is the exact inverse and writes every reserved byte as zero, so an
emitted object contains no unspecified byte. The Fletcher-64 function is
tested against blocks captured in E1 before any fixture is read, as the
format design [requires](design-apfs.md#object-header-and-checksum).

The compression codecs are reused as they stand. A stored chunk is at most
`FSIMG_LZFSE_INPUT_MAX` bytes and a decoded one at most
`FSIMG_LZFSE_OUTPUT_MAX`, the bounds of the codec interface, which the
driver applies to every codec's chunk. The LZFSE decoder's scratch,
`fsimg_lzfse_scratch_bytes()` bytes, is charged once per reader, when the
first LZFSE file is resolved, and kept until close; zlib's inflate state is
charged through the allocation adapter in the same way. Both imported
decoders return `FSIMG_EUNSUPPORTED` on a big-endian host and the driver
passes that through, so the types they serve are unsupported there and the
`codecs` operation leaves them out, as the format design's
[capability record](design-apfs.md#information-and-capabilities) states.

## Open

`open` runs a fixed sequence; each step validates everything it uses before
the next starts:

1. **Block 0.** One read of 4096 bytes, then `B` bytes once the block size
   is known. The `NXSB` magic is checked as the one identity check; the
   header's kind and OID, the checksum, the geometry, the features and the
   areas are validated as the format design states, by the function the
   probe calls.
2. **Checkpoint.** `checkpoint.c` reads the descriptor ring once with one
   block window. For each slot it records, in a paged array of fixed-size
   slot records in metadata scratch, the block's kind (superblock, valid
   mapping block or other), XID, last-map flag and its superblock run
   fields. Candidates are sorted by XID, which rejects duplicates, and
   judged from the largest down against the slot records alone, never
   rereading a block; the first whose run is whole is selected, and a larger
   one whose run is not sets `clean` to false. The selected run's mapping
   blocks are then read again, their mappings copied into a paged array
   sorted by OID, which rejects duplicate OIDs, and their block runs checked
   against the data run, its wrap and each other; each mapped object is
   read once and checked. A failure after
   selection is `FSIMG_ECORRUPT`, with no fallback to an older checkpoint.
   Selection costs the `D` descriptor blocks, one reread of the selected
   run, and one data run.
3. **Space manager.** The ephemeral object is read through the mapping
   table; its main-device free count gives `free_bytes`.
4. **Container object map.** The physical object and its tree root.
5. **Volume.** Each nonzero slot of the volume array is counted; the
   selected slot, or the only one, is resolved at the checkpoint XID through
   the container map, and its superblock validated, including features and
   the refusal tables. A missing or ambiguous selection fails here.
6. **Volume object map.** The live volume object map, a physical object
   checked against the checkpoint XID.
7. **Snapshot.** With `snapshot=`, the snapshot tree is searched for the
   name record, the metadata record is read, and the snapshot's volume
   superblock replaces the live one, under the header rule E9 fixes; the
   lookup XID for the volume's virtual objects becomes the snapshot's.
8. **Volume trees.** The filesystem tree root is resolved through the live
   volume object map at the lookup XID; in the sealed profile, the integrity
   object and the sealed file-extent tree root are read too.
9. **Root.** Inodes 2 and 3 are looked up and must be directories with
   parent 1.
10. **Information.** The volume information and its details are filled from
    the decoded objects. The volume's length is `N * B` with the selected
    superblock's `N`, whatever the slice holds.

`reader->content_domain` is `[B, N * B)`, the container without block 0.
APFS has no separate data region, so the domain does little beyond bounding
content to the container; ownership verification is what detects a file
extent that names metadata.

### Identity

The [identity check](design-apfs.md#probe-and-identity) is block 0's magic
and nothing else, and step 1 makes it through `fsimg_reader_identity()`. A
failed check fails the open, unless the caller assumes the identity
(`FSIMG_READ_ASSUME_IDENTITY`): the reader core then records the check and
full verification refuses the volume, naming it. No other check goes through
that function, so an assumed identity waives nothing else. In particular a
descriptor block is a checkpoint candidate only with its own magic, which is
validation.

`descriptor.c` holds one function that validates block 0's bytes after the
magic. The probe calls it on the prefix it is given and step 1 on the block
it read, so `open` makes every validation check the probe makes, as the
[format contract](design-formats.md#format-descriptors-and-profiles)
requires of an open that runs without a probe, and the two cannot drift
apart.

### Truncated containers

The driver has no truncation check: it follows the format design's
[rule](design-apfs.md#truncated-containers) through three properties of the
steps above.

* **One funnel.** Every byte is read through `fsimg_reader_bytes()`. A block
  below `N` and past the slice fails there, `FSIMG_ECORRUPT`, "image is
  truncated", in whichever step reaches it. The driver's range checks
  compare addresses with `N` alone.
* **Selection judges only blocks it has read.** The ring pass reads all `D`
  slots and the selected checkpoint's data run, so a cut through either
  fails the open. "Not whole" is a verdict on the contents of blocks that
  are present, which is what a checkpoint interrupted by a crash looks like;
  a missing block never earns it, so truncation cannot select older state.
* **Nothing is allocated from a declared size.** The slot-record array and
  the mapping array grow by blocks read. An object of several blocks is
  streamed through one window, as [objects](#objects) states. The chunk
  structures, whose number follows `N`, are read only by verification, which
  the reader core refuses on a truncated container before the driver's
  `verify` runs.

Open, listing and extraction therefore succeed when everything they touch
lies in the slice, and the reader core derives `missing_bytes` from the
length step 10 reports.

## Objects

`apfs_object_read(reader, ref, window)` takes a reference
`(storage class, OID, XID, kind, subtype, map)` and a window of `B` bytes:

* physical: the OID is the block, which must lie in `[1, N)`;
* virtual: `omap.c` resolves the OID at the reference's XID through the
  named map's cursor, and the object's XID must equal the found key's;
* ephemeral: binary search of the paged mapping array.

It reads the block with one exact read, verifies the checksum, and checks
kind, subtype, storage class, OID and XID. One object can span several
blocks, the space manager. Its mapped size is first checked against the size
the [geometry implies](design-apfs.md#ephemeral-objects-and-the-space-manager)
for its structure and address array; its blocks then pass through the same
window one at a time, the checksum running over them, and its fixed
structure, which lies in the first block, is decoded from there. The address
array is read later, in windows, by the verification pager. No buffer is
sized from a mapped size. A failure names the object's role, never its
bytes.

## B-trees

A cursor belongs to one tree: its root reference, storage class, comparator
and record codec. It holds the current root-to-leaf path, one window and one
table-of-contents position per level, plus the separator bounds each level
was entered under. Windows are allocated as the path deepens and kept for the
session; a tree of height `h` holds at most `h` windows, and the format
design bounds `h` by 16, a taller tree being `FSIMG_EUNSUPPORTED`.

`btree.c` offers three operations:

* `seek(cursor, key, mode)`: position on the last record at or below the
  key, or the first at or above it. The search starts from the deepest
  level whose separator bounds contain the key, so consecutive nearby seeks
  read only the nodes that change; otherwise it descends from the root,
  validating each node as it is loaded.
* `next(cursor)`: advance to the following record, climbing and descending
  as needed; the bounds of every node entered are checked against its
  parent's separators.
* `record(cursor)`: borrow the current key and value spans, valid until the
  cursor next moves.

Node validation is the format design's
[list](design-apfs.md#validation), run once per load. A repeated
node on the current path is a cycle, `FSIMG_ECORRUPT`. Each node load
charges one work unit and its read. A seek is bounded by the height. A scan
visits each node once: every node's keys must lie within the separator
range its parent gives it, and the ranges of a parent's children are
disjoint and increasing, so a node referenced from two places, or a
subtree repeated under two separators, fails the range check before its
records are used, and a scan is bounded by the tree's distinct nodes.

The reader keeps these cursors: the container object map (open only), the
volume object map, a filesystem lookup cursor for inode and attribute
lookups, a filesystem scan cursor for directory records, a content cursor for
file extents, and, in the sealed profile, the sealed file-extent tree.
Verification adds its own scan cursors. Because every cursor's windows are
charged when first needed, a small tree costs one window.

The cursor only reads. The writer builds trees with its own
[packer](#the-packer), and a tree the packer emits passes this validation
because the write tests read it back, not because the two share arithmetic.

## Names and keys

`unicode.c` validates a name and derives its comparison form by the volume's
[rule](design-apfs.md#directory-records-and-names): the canonical caseless
form, canonical decomposition alone, or the bytes. Decomposition, reordering
and folding read the generated tables. The form is produced as code
points into a bounded buffer: the generator computes the largest expansion
of any code point and fails the build above the format design's bound, and
the buffer holds that expansion of the longest name the reader accepts. The
hash is `fsimg_crc32c_update()` over the code points, under the conventions
the format design gives. The namespace key is the form as UTF-8, bounded by
the same expansion, so it fits the common key buffer of
`FSIMG_DIRECTORY_KEY_BYTES`, the 4096 bytes the
[directory design](design-directories.md#namespace) sizes for the longest
APFS name.

`key` converts a selection query component with the same function after
checking it is valid UTF-8 without slash or NUL; an invalid component is
`FSIMG_EINVAL`. The `ambiguous` text describes two APFS names that the
volume treats as equal, without quoting them, as the common contract
requires.

## Directories

`prepare` takes inode 2 at level 0 and otherwise the current entry's inode,
which `replay` proved is a directory. It seeks the scan cursor to the first
directory record of that inode and iterates while the owner and kind match.
For each record it checks the key variant, the name rules, the stored length
and hash, and the value: the child inode at least 16 (or in the volume-group
space) and a type in the format design's table. It admits the child with a
17-byte payload: the child inode (u64), the record type (u8) and the sibling
ID or zero (u64), little-endian. Admission charges the entry; the walker's
key sort then rejects equivalent names. A record `prepare` refuses fails the
directory, and the refusal names the child with
`fsimg_directory_name_child()`, so the error's path is the child's and no
name enters a message; a child whose name cannot be rendered is left
unnamed, as that function defines.

`prepare` reads no inode. Everything about the child beyond its directory
record is `replay`'s to check, so a damaged inode costs its own entry and
not the listing that names it.

`replay` re-validates the payload and looks the child inode up through the
lookup cursor. An inode's attribute records follow its inode record under
the same owner, so `replay` continues over them with `next` and learns the
rest of the entry in the one pass:

* **Inode.** It is validated, its mode type must match the record type, and
  a device's number comes from the device number extended field when
  present, as the format design's [inode table](design-apfs.md#inodes)
  states.
* **Directory.** A directory child's recorded parent must be the directory
  that holds its record, and its name extended field must equal the
  record's name. These are the driver's cycle and sharing rules, which need
  no claimed-object set because a cycle would give a directory two parents
  and a second record would give it a second name.
* **Attributes.** Each attribute record's key and value header are
  validated. The three names the driver consumes, each only on the inode it
  serves, are those of the format design's
  [attribute rule](design-apfs.md#extended-attributes). Any other attribute
  sets `FSIMG_META_EXTRA_DATA` on the entry, and since the bit is derived
  from the inode's own records, every name of a hard-linked inode carries
  it.
* **Symbolic link.** The target is the inline value of the link's
  attribute. While the record is current, `replay` checks it against the
  [target rule](design-apfs.md#symbolic-links), its length and its single
  final NUL, and declares exactly the bytes it checked with
  `fsimg_reader_set_link_target()`.
* **Compressed file.** `replay` validates the `com.apple.decmpfs` header
  against the inode and records the type, the uncompressed size and, for a
  resource-fork type, the fork attribute's data-stream reference. An inline
  payload stays in its record, where `resolve` finds it again by name.

The entry snapshot is then built:

| Kind | Size | Content state |
| --- | --- | --- |
| File | Data-stream size, or the compression header's size | Private ID and size; or the compression state above |
| Symbolic link | Target length | None in the driver: the target is in the reader's buffer |
| Directory, FIFO, device, socket | Zero | None |

For a file, `reader->initialized` is the entry's size. For a link,
`fsimg_reader_set_link_target()` copies the target into the reader's one
buffer of `FSIMG_SYMLINK_TARGET_MAX` bytes and sets the entry's size and
initialized length; delivery serves the link from that buffer and never
calls `resolve` for it, as the
[content design](design-content.md#decoded-link-targets) states. The driver
holds no link buffer, and a bad target is refused at `replay`, so a listing,
which reads no content, already reports it.

Inode lookups at replay are random tree descents; their cost is measured by
the scale tests, and a wide directory whose lookups dominate is the trigger
for sorting a directory's child inodes once per preparation instead.

## Content

`resolve` answers an offset `o` of the current file with one extent. It is
never called for a symbolic link.

* **Uncompressed file.** The content cursor seeks the last file-extent
  record of the private ID at or below `o`. If the record covers `o`, the
  result is a mapped extent over its blocks, or a zero extent when its
  physical block is zero. If it ends at or before `o`, the next record must
  not exist, and the result is a zero extent to the size; otherwise the gap
  is `FSIMG_ECORRUPT`. Sequential reads continue with `next`, which checks
  that each record starts where the previous one ended, so a forward read
  validates the whole tiling once and needs no descent per extent.
* **Compressed file.** Chunk `c = o / 65536` is decoded into a 64 KiB
  window, unless it is the chunk already there, and the result is a held
  extent over that chunk clipped to the size. An inline type has one chunk,
  decoded from its attribute record. For a resource-fork type the first
  `resolve` of the file validates the fork's
  [framing](design-apfs.md#transparent-compression) with the shared
  validator's `open`, which checks that the table's entry count covers the
  size and that the table ends inside the fork. Entries are checked one
  chunk at a time, as HFS Plus does: decoding chunk `c` calls `chunk`,
  which reads entry `c` with its neighbours, holds at most three table
  entries, 24 bytes, whatever the table's length, and checks the entry's
  order and overlap against its neighbours and its bounds against the
  payload interval. That is every property the decode of chunk `c` relies
  on. Structural verification checks every entry without decoding, as the
  [verification](#verification) section states. The
  table is never held: each chunk's stored range comes from `chunk` when the
  chunk is decoded, and is read, through the
  fork's file extents and the same lookup as an uncompressed file's, into a
  second buffer of `FSIMG_LZFSE_INPUT_MAX` bytes, the bound on every stored
  chunk.
  A chunk decodes to exactly its length or fails, and the bytes consumed and
  produced are charged as work.

Mapped extents are checked against `[1, N)` before they are returned;
delivery then clips them to the request and the initialized length and reads
only inside the content domain. The driver checks the same range for the
stored bytes of a chunk, which delivery never sees. A held window is valid
until the next `resolve` or advance, as the content design requires.

### Salvage

The driver takes part in [salvage extraction](design-salvage.md) with no
mechanism of its own. `replay` reads no extent, so the common plan, which
resolves a file from its first byte, meets damage exactly where a read
would, and what survives follows from `resolve`:

* **Uncompressed file.** The prefix ends at the first offset whose lookup
  the decoder rejects: an invalid node on the content cursor's path, a
  malformed file-extent record, a gap or overlap in the tiling, an extent
  outside `[1, N)`, a refused flag or crypto ID. An extent that runs past
  the slice ends the prefix at the slice's end, byte for byte.
* **Shared extents.** No lookup reads the extent-reference tree, so damage
  there costs reading nothing and verification reports it. A clone, or any
  file whose extents another file shares, is resolved through its own
  records and keeps its own prefix, whatever became of the other owner.
* **Compressed file.** The plan decodes the file chunk by chunk. Its prefix
  ends on the 64 KiB boundary before the first chunk that a damaged table
  entry, a fork extent or the decoder rejects, and no later chunk is
  recovered, as on HFS Plus. A fork's framing and an inline payload are
  checked by the first `resolve`, so damage there leaves no recoverable
  bytes. Copying
  the prefix decodes its chunks again, which charges their work a second
  time.
* **Entries.** A rejection in `replay`, of a missing or invalid inode, an
  attribute record, a compression header or a link target, costs that one
  entry, reported under its own name, which the walker holds from the
  directory record. A rejection in `prepare` costs that directory's
  listing. The steps of [open](#open) stay terminal.

`prepare`, `replay` and `resolve` report a decoder's rejection as
`FSIMG_ECORRUPT` or `FSIMG_EUNSUPPORTED` under `FSIMG_OP_READER`, the read
funnel's truncation among them. I/O, budget, scratch and cancellation
failures keep their own operations and stay terminal, as the salvage
[boundary](design-salvage.md#failure-boundary-and-limits) requires.

## Verification

`verify.c` runs once over the container and then once through the common
walker. A [truncated container](#truncated-containers) reaches neither: the
reader core fails its verification first.

**Container pass.** It re-reads the selected checkpoint's descriptor and data
runs, the space manager, the reaper and the container object map, and appends
claims `(first block, count, class, owner)` to a claim stream: the
[fixed regions](design-apfs.md#ephemeral-objects-and-the-space-manager),
one claim each, as `apfs_fixed_regions()` lists them from the decoded
superblock and space manager; the block of each object map itself, the
container's and each volume's, and every node of its tree; every live
object version each object map maps, at its size, tombstones excluded;
every physical tree node; a nonzero keylocker range, as one range each;
every main free-queue entry; and the space manager's chunk-information,
address and chunk bitmap blocks that lie outside the internal pool. The
writer's layout calls the same `apfs_fixed_regions()` on the fields it
plans, so the regions verification claims and the regions the writer
allocates are one list. The reaper contributes no claim: it must be
[idle](design-apfs.md#reaper), or the report is incomplete. The
space-manager blocks inside the pool and the internal-pool free queue's
entries go to a second, smaller claim stream for the pool's own domain,
merged against the pool's bitmap, so no block is claimed in both.
For each volume it scans the object map, the snapshot tree, every
extent-reference tree (the live one and each snapshot's), and the
filesystem tree with dedicated cursors, and in the sealed profile the
sealed file-extent tree. Each physical-extent record appends a data claim,
owner `(volume, tree)`; the filesystem scan, or the sealed file-extent
scan, appends a file-extent record `(block, count, volume, private ID)` to
a third stream.

**Record reconciliation.** The filesystem scan visits each object's records
together because they share the owner prefix, so most checks are local and
need no storage: the inode record (kind 3) is followed by its attributes
(4), sibling links (5), the data-stream ID (6) and extents (8) of a stream
whose private ID is the inode number, and, for a directory, its directory
records (9), whose count must equal the child count; extents tile their
stream; attributes and extended fields are well formed; counts by kind
accumulate for the superblock. Facts that join different owners go to
streams that are sorted once:

* name references `(child, parent, type)`, joined with the inodes in scan
  order to check existence, kind, a directory's single parent and each
  file's link count;
* sibling maps `(sibling ID, inode)`, joined with the sibling links;
* stream uses `(private ID)` from inodes and stream-backed attributes,
  joined with the data-stream ID records' counts;
* collision records `(parent, comparison form, record)` for every
  directory record, sorted by parent and comparison form, so equivalent
  names are found whatever their stored hashes say, including when the
  hashes are unchecked or wrong, and in volumes the common walker does not
  visit; adjacent equal forms are a collision, so an adversarial directory
  costs a sort, not quadratic comparisons.

**Chunk tables.** For each compressed file of a resource-fork type in the
selected volume, structural verification runs the shared validator's
`open` and then `chunk` for every entry in ascending order, reading the
table through the fork's file extents and decoding no payload, as HFS Plus
checks its tables in `content_check()`. It holds three table entries and
charges one unit of work per entry, so a damaged later entry fails `W`
without payload read-through.

**Allocation merge.** The claim stream is sorted by first block. The merge
walks it with `spaceman.c`'s pager, which visits chunk-information blocks in
address order and each chunk's bitmap block in 4 KiB windows. For each block
it checks the format design's rules: metadata claims never overlap any other
claim; data claims overlap only data claims of other trees of the same
volume; a block is allocated exactly when claimed; each chunk's free count
and the device's free count equal the clear bits. The file-extent stream,
sorted by block, is merged with the live tree's physical-extent records:
every file extent lies inside one, and a second sort of
`(record, stream)` pairs checks that each record's count equals the
distinct live streams sharing it. The pool's claim stream is merged the
same way against the pool's bitmap. The two checks the format design holds
[provisional](design-apfs.md#verification), the bitmap bits past `N` and
each volume's allocated block count, are not made until their experiments
give the rule.

**Walk.** The common walker visits the selected volume once. Every
delivered entry is counted through `fsimg_verify_account()`, the one place
entries are counted, so the report follows the common
[definitions](design-verification-helpers.md#initialized-payload-read-through):
`files` counts every non-directory, symbolic links, FIFOs, devices and
sockets included, and each name of a hard-linked inode; `directories` includes the root;
`logical_bytes` adds each non-directory's size, a link's being its target
length. With payload read-through the driver calls `fsimg_verify_payload()`
for each regular file and symbolic link, and `payload_bytes_read` counts
only what delivery read from mapped extents, as the content design's
[stored bytes](design-content.md#stored-bytes) define. Holes and zero tails,
decoded chunks and link targets add nothing, and neither do a compressed
file's stored chunks, which are backing reads of `resolve`.

An inode with several names is read once, at the first name the walk
reaches. Only an inode whose link count is above one can repeat, and
reconciliation has by then proved that count equal to its names. The
filesystem scan therefore appends each such inode number, in the ascending
order it meets them, to a paged array in charged scratch with one mark per
element; the walk finds an inode there by binary search, reads it if it is
unmarked and marks it. Every name still adds to `files` and
`logical_bytes`. Without the marks, many names of one large file would
multiply its reads with no limit but `max_work`, since verification
produces no output.

This bounds names, not bytes. Clones are distinct inodes whose extents name
the same blocks, whether they still share one data stream or have diverged,
and each is read through its own records, so shared blocks are read once
per inode that maps them, within the session's work and time limits, as the
[EROFS driver](design-erofs-driver.md#verification) reads shared chunks.
Reading them once would take a mark per block of every shared extent and a
read path beside the common helper.

The report also carries the container's allocated and free blocks, and a
completed verification sets `FSIMG_VOLUME_FREE` from the counted free
blocks. A refused feature in any volume, an unknown optional or
read-only-compatible feature bit, or a reaper that is not idle makes the
report incomplete with `FSIMG_EUNSUPPORTED`; the walk is never reported
complete after skipping an object.

## Writer settings

`validate` accepts the common options under the format design's
[settings](design-apfs.md#settings-and-identities): `sector_bytes` and
`allocation_unit_bytes` zero or 4096; `label` NULL, selecting the
[default label](design-apfs.md#writer-values), or valid UTF-8 of at most
255 bytes; `fat_root_entries`, `byte_order`,
`inode_capacity`, `reserve_percent` and `mountpoint` at their defaults, which
the common settings check enforces; `volume_uuid` nonzero unless
`FSIMG_WRITE_REPRODUCIBLE` is set; `epoch` inside the writer's time range.
`volume_serial` is ignored, since the CLI generates one for every format.

`volume_bytes` is zero or a multiple of 4096 of at least the writer's
minimum container size. That minimum is the native formatter's, a
[writer value](design-apfs.md#writer-values), and can be larger than the
1 MiB below which the reader refuses a container: the two bounds answer
different questions and `validate` uses only the writer's.

The capability record's `write_settings` names `FSIMG_SETTING_VOLUME_UUID`
and `FSIMG_SETTING_CASE_SENSITIVE`, the latter governing the
`case_sensitive` field, CLI `case=insensitive|sensitive`. It does not name
`FSIMG_SETTING_PLACEMENT`, so a non-empty [placement](design-placement.md)
list fails the common applicability check before `validate` runs.

## Planning

The writer rests on one construction. Every tree is produced by one bulk
packer, which planning runs to count nodes and emission runs to write them,
over the same records in the same order. Node boundaries, node counts and
completion order, and with them every OID and block number, therefore cannot
differ between the plan and the image. Record lengths depend on the catalog
alone, because block numbers and OIDs have fixed widths, as the format
design's [layout](design-apfs.md#layout) states, so every tree is counted
before any block is placed.

### The packer

`write-btree.c` takes a tree's profile and its records in key order, and
keeps one open node per level.

* **Capacity.** A node is one 4096-byte block: a 56-byte header, then the
  table of contents, the keys after it and the values filling from the end;
  a root also ends in its 40-byte footer
  ([node layout](design-apfs.md#node-layout)). Every node is packed against
  the root's capacity, `C = 4096 - 56 - 40`. A level's only node can then
  always be the root, so the packer never splits a top node that fits a node
  but not a root, and never holds a node back to learn which it is. The cost
  is 40 unused bytes in every other node, under one percent.
* **Fit.** A node holds `n` records whose keys take `K` bytes and whose
  values take `V` when `toc(n) + K + V <= C`. `toc(n)` is the table's length
  for `n` entries, four bytes each in a fixed-size tree and eight otherwise,
  with the spare capacity the tree's profile gives it. A key or value counts
  its length rounded up to 8 bytes unless the tree is unaligned. The
  profile's table capacity, alignment, flags and fill are
  [writer values](design-apfs.md#writer-values).
* **Records.** A leaf record is the stream's key and value. An index record
  is the child's first key, whole, with the child's 8-byte OID as its value:
  the virtual OID in the filesystem tree, the block number in a physical
  tree. An object map's records are a fixed 16-byte key and 16-byte value,
  the extent-reference tree's an 8-byte key and 20-byte value, and the
  filesystem tree's vary.
* **Fill.** A record joins its level's open node while the node still fits
  it, and otherwise closes that node and opens the next. Closing a node
  gives it the next completion index and encodes it. A node closed because
  it is full adds its index record to the level above, opening that level
  if there is none. When the records end, the open nodes close from the
  leaves up: one with a level above it adds its index record there, and the
  one with none is the root. A level exists only once the level below has
  filled a node and gone on, so a root that is not a leaf has at least two
  children. A tree with no records is one root leaf with no keys.
* **Last nodes.** The last node of a level holds what remains, as few as one
  record, which the reader's validation accepts. That `fsck_apfs` accepts
  such a node is a provisional [writer value](design-apfs.md#writer-values)
  (E15). If it does not, the last node of a level takes its predecessor's
  last record, at the cost of holding one closed node per level.
* **Progress.** The largest record the writer emits is bounded by its name
  and link-target limits. A static assertion proves that two such records
  fit an empty node, so a record always fits a fresh node and the fallback
  above always has room.

The packer writes through a sink. Planning's sink counts the nodes of each
level; emission's writes each node as it closes. Both number nodes by
completion: the `k`th node closed is node `k` of its tree, children before
their parent and the root last. The root's footer statistics are totals the
packer accumulates.

### The plan

The plan object embeds `fsimg_plan_t` and holds the geometry, the table of
fixed regions and the allocation ranges, the volume flags, the format time,
the UUIDs, each tree's node counts and first run index, the ephemeral table,
and one paged array of rows keyed by catalog ID, row 0 being the root. A row
holds the parent's row, the child count, the content length, the data
blocks of the files before it in catalog order, the first data block, and
the inode fields already encoded under the metadata policy. Rows,
the sorted record stream and the collision stream are the only per-entry
state.

The format time is fixed before the first pass, from the host clock or, in
reproducible mode, the epoch, as the format design's
[settings](design-apfs.md#settings-and-identities) state. No later step
reads a clock.

### Passes

Planning runs a fixed sequence of passes over the frozen catalog:

1. **Entries.** For each catalog ID in order: check the name's length and
   rules; compute its comparison form and hash; append
   `(parent, form, ID)` to the collision stream; count the entry into its
   parent's row, which gives each directory the child count its inode
   stores, and into the per-kind totals of the volume superblock; validate
   kind and metadata representability under the
   [metadata policy](design-metadata.md#writer-policy); accumulate data
   blocks `ceil(size / 4096)`, recording in each file's row the total
   before it, and `initialized_bytes`. An entry's inode
   number and private ID follow from its catalog ID by the rule in the
   [writer values](design-apfs.md#writer-values) and are not stored. The
   same rule gives the volume's next object identifier, which counts inode
   numbers and is unrelated to the container's next OID that layout sets.
2. **Collisions.** Sort the collision stream; two equal `(parent, form)`
   records fail with `FSIMG_EINVAL`. An error carries one path, so it
   names one entry, the later of the two in the sorted stream, by its
   catalog path, as [failure classification](#failure-classification)
   gives for every writer refusal.
3. **Records.** For a symbolic link, first read its target through the
   common [link-target operation](design-emission.md#encoded-link-targets),
   `fsimg_writer_link_target()`, and encode the bytes in its inline
   attribute. The catalog bounds the target length; the read supplies its
   bytes and validates the source. Append every filesystem record each
   entry needs, and the native records of the empty volume, to a record
   stream, with the fields the [writer values](design-apfs.md#writer-values)
   give them. A nonempty file's run is tiled by file-extent records of at
   most the [extent cap](design-apfs.md#writer-values): `ceil(r / cap)` of
   them for a rounded size `r`, consecutive in key order, each of the cap's
   length but the last. The count follows from the size alone, and is one
   for every file no longer than the cap. The split is record generation
   only: the run stays contiguous, the file keeps one physical-extent
   record, and neither the packer nor the layout sees it.
   Every record is encoded in full except the physical block of a file
   extent, a fixed-width field emission fills from the row. Sort the stream
   with the filesystem comparator. The sorted stream holds the final encoded
   lengths and planned target bytes.
4. **Packing.** Run the packer with the counting sink over each tree:
   * the filesystem tree, over the sorted stream;
   * the volume object map, over one record per filesystem node, generated
     from a counter in completion order, which is OID order;
   * the extent-reference tree, over one record per nonempty file; only the
     number of records matters here, since their size is fixed;
   * the snapshot tree, which is empty;
   * the container object map, over its one record, the volume superblock.
5. **Size.** `fits(N)` is the arithmetic of step 6 without its
   assignments, and step 6 runs the same code, so sizing and layout cannot
   disagree. It fills the region list for `N` and walks the regions in
   ascending order, carrying the number of run blocks placed below the
   region at hand. Metadata blocks are placed one by one and continue past
   a region. Where a region falls among file data, one binary search of the
   rows, by the data blocks before each file, finds the file that would
   straddle it; that file and those after it resume past the region. `N`
   fits when the last run ends at or
   below `N` and `N` less the allocation ranges leaves the requested free
   space. A probe writes no row and allocates nothing: its cost is
   proportional to the region count, with at most one row search per
   region.

   With exact `volume_bytes`, `N = volume_bytes / 4096`, and an `N` that
   does not fit fails with `FSIMG_ERANGE`. With automatic sizing, `N` is
   the first block count on the native size step that fits in the search
   the format design's [layout](design-apfs.md#layout) states. It
   starts at the lower bound, which is block 0, the run's metadata blocks
   from step 4, the data blocks from step 1 and the requested free space,
   raised to the minimum container and rounded up to the step. A failed
   probe returns its shortfall, the blocks by which the run, displacement
   gaps included, ends past `N` plus any free space missing, and the next
   probe adds it, rounded up to the step and at least one step. Each probe
   is charged as work, and the
   scan fails with `FSIMG_ERANGE` naming the
   [probe cap](design-apfs.md#writer-values) when it reaches it. The
   maximum volume count follows from `N`.
6. **Layout.** Assign every block and identifier, in this order:
   * **Regions.** `apfs_fixed_regions()` fills the table of fixed regions
     from the superblock and space-manager fields planned for `N`, as it
     does for verification from decoded fields. `run_block(i)` is the `i`th
     block outside them in ascending order, a pure function of the table.
   * **Metadata.** Each metadata object has a run index and sits at
     `run_block` of it. The indexes follow the format design's order: the
     container object map's own block and then its tree, the volume
     superblock, the volume object map's own block and then its tree, the
     filesystem tree, the extent-reference tree, the snapshot tree. Node
     `k` of a tree has the tree's first index plus `k`.
   * **Files.** Each nonempty file, in catalog order, takes one contiguous
     run: it starts at the block after the previous assignment, or, when a
     fixed region would interrupt it there, after that region. Its row
     records the first block, and file runs ascend with the catalog ID.
   * **Allocation ranges.** The fixed regions, the metadata blocks and the
     file runs, merged where they touch, form a short sorted list of
     disjoint ranges: at most two per fixed region and one more. It is the
     only description of allocation the space manager is built from.
   * **Identifiers.** OIDs are assigned in the order the
     [writer values](design-apfs.md#writer-values) give; filesystem node `k`
     takes the first node OID plus `k`. The container's next OID follows
     the last.
   * **Ephemeral table.** The space manager, whose size the geometry gives,
     and the reaper are listed in OID order with
     kind, subtype, byte size and their slots in the checkpoint data area,
     assigned in that order from the first slot.
   * **Internal pool.** Count the chunk-information blocks, the address
     blocks and one bitmap block per chunk that an allocation range
     touches, and assign them pool blocks in the order the
     [writer values](design-apfs.md#writer-values) give. They fit the pool
     by the native size the format design gives it; planning checks that
     they do.
7. **Summary.** Fill `fsimg_plan_info_t` with the volume, metadata,
   allocated, free and initialized bytes, the block size and count. The
   common [loss policy](design-metadata-loss.md) installs the loss count
   after every step succeeds.

Every value derives from the catalog, the options and these rules, and the
format time is the epoch in reproducible mode, so byte-identical output does
not depend on source order, budgets, sort fan-in, scratch placement, locale,
time zone or the clock.

## Emission

The common emission object has already zeroed the sink, so a block the
driver never writes, an unused ring slot or a free block, holds zero. A
block the driver does write is encoded in a buffer first. The packer and
the space-manager encoder zero each block buffer before filling it, so
every unused field, the free space within a node and the tail of a partly
filled chunk-information block hold zero too, and the poison scan checks
both. Each object goes out with
`fsimg_emit_write()` at its block, its checksum computed as it is encoded.
The order below is the order of work; block numbers were fixed by layout,
and the bytes do not depend on the order.

1. **Payload.** For each regular-file row in ID order: the common payload
   copy, `fsimg_emit_copy()`, over its run. An empty file uses a zero-length
   copy with no regions, so its source is still opened and validated.
2. **Filesystem tree.** The sorted record stream passes through the packer
   a second time, with the writing sink, each file extent's physical block
   filled from its row's first block plus the extent's offset in the run.
   Before each symbolic-link attribute is packed, its
   target is read again by catalog ID through the common link-target
   operation and compared byte for byte with the planned attribute. A
   difference fails with `FSIMG_ESOURCE_CHANGED`; no unchecked planned
   target reaches the output. Leaves are written as they close and index
   nodes as their children complete, the root last, with its footer.
3. **Other trees.** The volume object map's tree, from a counter over the
   filesystem nodes: record `k` maps the first node OID plus `k` to that
   node's block. The extent-reference tree, from the rows of the nonempty
   files in catalog order, which is block order and so key order. The empty
   snapshot tree, the volume object map, the volume superblock, and the
   container object map's tree and object.
4. **Space manager.** One cursor walks the allocation ranges and the
   chunks together, both ascending. For each chunk it counts the allocated
   blocks inside it, which gives the entry's free count, and when that
   count is not zero it writes the chunk's bitmap block at its pool block:
   whole bytes of ones and zeros for each range and gap and the partial
   bytes at their edges. What the bits past `N` hold, and the entry of a
   chunk with no allocated block, which gets no bitmap block, are writer
   values. The entries fill the
   chunk-information blocks in chunk order, each written when it is full
   and the last as far as it is filled, over its zeroed buffer,
   and when the geometry calls for address blocks they list the
   chunk-information blocks the same way. The pool's own bitmap marks
   exactly the pool blocks taken. Last comes the space manager object:
   its address array names the blocks just written, and its device free
   count is the sum of the entries' free counts, which is `N` less the
   allocation ranges.
5. **Checkpoint.** The space manager, whose free queues are empty in the
   zero-OID form, and the idle reaper go to the slots the ephemeral table
   gives them in the checkpoint data area. The mapping block is encoded from the same table,
   one mapping per object in table order, so the mappings and the objects
   cannot disagree. The container superblock follows it in the descriptor
   ring, and block 0 receives the copy.

The fields of every object in steps 3 to 5, and the slots of the two rings,
are [writer values](design-apfs.md#writer-values). Every emitted byte comes
from a row, the geometry, the sorted records or one of those values.

## Resource bounds

| Operation | Work and residency |
| --- | --- |
| Probe and open | Block 0, one pass over the descriptor ring, one reread of the selected run, the selected checkpoint's mapped objects (one data run), the container and volume map paths, the volume superblocks, the root inodes; the slot-record array and the paged mapping array |
| Listing | Per directory: one scan of its records and the common key sort; per entry: one inode lookup and one scan of that inode's attribute records, which follow it. No file payload is read |
| Content | Per sequential read: one cursor advance per extent; per random offset: one tree descent; per compressed chunk: one decode of at most 64 KiB, and for an inline payload one attribute lookup; per resource-fork file: one framing check at the first read, and one table entry with its neighbours per decoded chunk |
| Verification | One scan of every tree of every volume; the main and pool claim, file-extent, name-reference, sibling-map, stream-use and collision streams, each sorted once, and the `(record, stream)` pairs; one pass over the chunk structures, bitmaps and pool bitmap; one walk of the selected volume, reading each inode's content once; the array of multiply linked inodes |
| Planning | One catalog pass, two external sorts (collisions and records), one counting run of the packer per tree, a size scan of at most the probe cap's probes, each costing at most one row search per fixed region, one layout pass |
| Emission | Payload bytes, one replay of the sorted records, and metadata proportional to entries and container size |

Resident state per reader is the geometry, the selected superblocks, the
cursors' windows (at most 16 per cursor), and, once a compressed file has
been resolved, the 64 KiB decode window, the stored-chunk buffer of
`FSIMG_LZFSE_INPUT_MAX` bytes and the codec's state: the LZFSE decoder's
scratch, or zlib's inflate state through the common allocation adapter. The
link-target buffer is the reader core's. Resident state per writer is one
node buffer per level of the tree being packed and the allocation ranges.
Everything proportional to entries, records or blocks is charged scratch.
Every loop charges the session and observes cancellation.

## Failure classification

The format design's [table](design-apfs.md#failure-classification) governs.
Within the driver, a failed exact read is `FSIMG_EIO`; a read past the slice
is the funnel's `FSIMG_ECORRUPT`, "image is truncated"; a step that runs out
of budget or scratch is `FSIMG_ELIMIT`; a lookup that misses a required
object is `FSIMG_ECORRUPT` naming the object's role. A codec failure is
`FSIMG_ECORRUPT` for malformed input, `FSIMG_EUNSUPPORTED` from an imported
decoder on a big-endian host, and `FSIMG_ELIMIT` for exhausted resources,
never a silent short read.

`prepare`, `replay` and `resolve` report a decoder's rejection of the image
under `FSIMG_OP_READER`, so [salvage](#salvage) steps over exactly those
and every other failure stays terminal. The checks of `open` are terminal
under salvage too.

A name reaches a diagnostic only through the error's path. A refusal in
`prepare` that concerns one child names it with
`fsimg_directory_name_child()`; a refusal in `replay` is reported against
the entry the walker is replaying; a writer's refusal names the catalog
entry. No message quotes a name, node bytes, attribute data or a buffer.

## Tests

Every valid case below is paired with a mutation or an injected fault that
must fail with its class, so a passing case shows that the check ran and not
only that nothing objected.

`test/apfs-gen.[ch]` is a synthetic container generator written from the
format design with its own offset tables, Fletcher-64, node packing and
CRC-32C; it shares no code with `lib/formats/apfs/`, and `test/apfs.c`, the
salvage test, the contract profile and the fuzz seeds all build their
containers with it. Its names use a small alphabet whose
comparison forms it computes itself, plus the E4 vectors with their recorded
hashes, so it needs no normalizer of its own. It builds containers in memory
at every legal block size, with chosen checkpoint rings, object maps,
multi-level trees, volumes, records and extents, and mutates them.
`test/apfs.c` runs the format design's
[acceptance matrix](design-apfs.md#acceptance-and-independent-validation)
over it. Beyond that matrix come the common contracts the driver joins, the
salvage row in `test/apfs-salvage.c` and the others in `test/apfs.c`:

| Contract | Passing case | Paired failure |
| --- | --- | --- |
| Truncation | A container cut past everything a listing and an extraction touch opens, lists, extracts and reports the missing bytes | The same container cut through the descriptor ring, the selected data run, a tree node and a file extent fails `FSIMG_ECORRUPT`, "image is truncated", where each is reached. A cut that removes the newest checkpoint fails the open, while the same checkpoint torn in place, its blocks present and invalid, opens the older one with `clean` false. Full verification fails on every cut |
| Assumed identity | A container whose block-0 magic is rewritten and whose checksum, which covers the magic, is repaired opens and lists under `FSIMG_READ_ASSUME_IDENTITY` | Without the flag it is `FSIMG_EFORMAT`; with it, the magic rewritten without the repair fails the checksum, each other validation mutation of block 0 still fails with its class, a descriptor block without its magic is not a candidate, and full verification fails naming the assumed check |
| Link target | A link's target is listed and extracted with `resolve` instrumented to fail for a link, at 1 and 1023 bytes | A target with an interior NUL, without its final NUL or of 1024 bytes fails in a listing, which reads no content |
| Extra data | An inode with one unconsumed attribute sets `FSIMG_META_EXTRA_DATA` on each of its names | A link, and a compressed file of each storage, holding only the attributes the driver consumes, do not set it; `com.apple.decmpfs` on an inode that is not a compressed file does |
| Salvage | A file whose third extent record is damaged is extracted as a `.partial` of its first two extents; a compressed file with a bad chunk keeps the chunks before it; a damaged inode costs its entry and a damaged directory record its listing, each reported, with the siblings extracted | Each fixture fails as a whole without salvage; an injected read fault, scratch failure or cancellation during salvage is terminal, not a loss; damage in the extent-reference tree changes no extracted byte and fails verification |
| Verification counts | A file with three names adds three to `files` and three sizes to `logical_bytes` and its size once to `payload_bytes_read`; a hole, a compressed file and a link add nothing to that count | The same file with a wrong link count fails before any payload read; a read fault injected in one of its blocks fails the payload scope once, not once per name |
| Chunk table | A resource-fork file of many chunks, of each table kind, reads from a random offset | A count that does not cover the size fails the read of the file's first byte; an entry outside the payload fails the read of its own chunk while the chunks before it read correctly; an entry out of order or overlapping a neighbour fails at the first chunk whose neighbour check sees it, which can be the chunk before it; full verification fails the file in every case |
| Collisions | A two-volume container whose unselected volume holds names that differ only in case on a case-sensitive volume verifies | The same names in a case-insensitive unselected volume, stored with different hashes, fail `W` as a collision |
| Empty-container allocation | The generator's empty container in the native layout, its bitmap ring outside the pool and both object maps in the run, verifies complete, and every allocated block is one the claims name; the E1 empty captures do the same in the integration test | With the bitmap bit of one block of each fixed region, and of each object map's own block, cleared in turn, verification fails naming a claimed block marked free |

`test/apfs-unicode.c` checks the generated tables: each table's digest
equals the one `docs/references.md` records for the pinned inputs, and the
comparison forms of the `NormalizationTest.txt` vectors and of the E4
vectors match.

`test/apfs-write.c` creates containers through the public API and reads them
back through the reader and verifier and through an independent checker
written from the format design, sharing no code with the driver. It compares
bytes across source order, metadata cache size, scratch placement and time
zone, scans every unused byte for poison, and with `--native` compares an
empty container's structures with the E1 capture of the same size field by
field, apart from UUIDs, times and the formatting identifier. It checks the
packer at its boundaries: records that exactly fill a node and one byte
more, a level whose last node holds one record, a root with two children,
and trees of one, two and three levels; and the layout at its own: a file
that would straddle a fixed region, and the sizes on either side of each
change in the fixed regions, where the automatic size must fit and equal
the first fit of the specified probe sequence, which the test recomputes
independently. The scan is
also run alone over a predicate that never fits, which must fail
`FSIMG_ERANGE` at the cap. The extent split is tested on the record
generator alone,
at the cap, one block above it and at twice the cap, since no fixture
holds such a file. The poison scan runs with the sink and, through
`fsimg_test_poison_allocations()`, the driver's buffers poisoned. A
non-empty placement list, a size below the writer's minimum and each
refused setting fail before any output.

`test/driver-contracts.c` carries an `apfs` coverage profile, a
`contract_fs_profile_t` under the
[contract suite](design-driver-contract-tests.md). Its reader fixture comes
from the independent generator. Its creation options and source tree drive
the writer and round-trip scenarios in a build whose writer capability is
registered, which the format design gates on native qualification. The
[specification harness](design-spec-testing.md) has an `apfs` suite,
`test/spec/lib/fsimg_spec/apfs.rb` with `test/spec/lib/fsimg_spec/apfs/`
and `test/spec/suite/apfs/`: layouts, a model that builds the writer's
one-volume profile, rules named from the format design's container,
checkpoint, object-map, node, volume and record sections, and adapters for
`fsck_apfs -n` and `apfsck`. It is written from the format design,
independently of the driver and of `test/apfs-gen.[ch]`, and the
[plan](../docs/apfs-plan.md#work-packages) orders it after the writer's
native qualification, since the model reproduces the qualified profile.

`test/apfs-integration.sh.in` runs the reader's oracles the host has and
skips the others explicitly: each native capture against its manifest and,
on macOS, `fsck_apfs -n`; `apfsck`, `apfs-fuse` and `fsapfsinfo` on copied
images wherever `build-aux/build-apfs-oracles.sh` built them; and each
third-party specimen against its recorded result. Captures come from the
directory `FSIMG_APFS_NATIVE` names and specimens from the one
`FSIMG_APFS_CORPUS` names, and neither is redistributed.
`tools/apfs/qualify.rb` runs the writer's native qualification on macOS:
`fsck_apfs -n`, `hdiutil attach` with a manifest comparison, and the
write-after-create cycle.

`fuzz/apfs.c` feeds the probe, checkpoint selection, object maps, nodes,
records, content and verification under finite budgets, seeded
from the generator, with the generator's checksum repair so mutations reach
past the checksums; `fuzz/lzfse.c` feeds the imported decoders. Fault
tests inject allocation, scratch, short-read, EOF, source-change and
cancellation failures at every stage of reading, planning and emission and
check cleanup. Scale tests grow the container size, one directory's width,
a file's extent count, the tree heights, the names of one inode and a file
beyond 4 GiB separately, recording I/O calls, node loads and peak scratch.
