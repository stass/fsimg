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
| `codec.c` | Fletcher-64 and pure decoders and encoders of the object header, container and volume superblocks, checkpoint map, object map, node header and footer, space-manager records, reaper, reap-list header, filesystem keys and values and extended fields; no I/O |
| `checkpoint.c` | The descriptor-ring scan, candidate completeness, selection, and the ephemeral mapping table |
| `object.c` | `apfs_object_read()`: the one function that turns a physical, virtual or ephemeral reference into a validated object in a caller's window |
| `btree.c` | Node validation, the cursor and the comparators |
| `omap.c` | Object-map lookup at an XID over a cursor |
| `volume.c` | The volume array, volume selection, superblock validation, snapshot selection and the read XID |
| `unicode.c` | Name rules; canonical decomposition with canonical reordering, and case folding, over the generated tables; whether a name holds a code point unassigned in the pinned version or a noncharacter; the comparison form under the volume's rule, the name hash, namespace keys |
| `unicode-tables.h` | The generated decomposition, combining-class and case-folding tables, and their input digests |
| `inode.c` | Inode and extended-field interpretation, the attribute scan with the extra-data mark, record-to-entry mapping, data-stream lookups |
| `directory.c` | `prepare`, `replay`, `keys` and `key`; the parent rule for directories |
| `content.c` | `resolve`: file extents, holes and the dispatch to `compress.c` |
| `compress.c` | The compression types and their rules, inline payloads, the reads of a resource fork through its file extents, the decode window, and calls to the shared framing validator, zlib and the imported decoders |
| `read.c` | `fsimg_apfs_reader_ops`: open, close and volume information |
| `verify.c` | Container-wide verification: claims, reconciliation streams, the bitmap merge and the walk |
| `spaceman.c` | `apfs_fixed_regions()`, the one list of fixed regions that verification claims and the writer lays out; the space manager's read and geometry check; the geometry the native formatter derives from `N`, which the writer plans; the space-manager pager over address, chunk-information and bitmap blocks, for verification |
| `write-apfs.h` | Plan rows, tree profiles and the plan object |
| `write-plan.c` | `validate`, `plan`, `free`: settings, the entry pass, record generation, sizing and layout |
| `write-btree.c` | The bulk B-tree packer that planning and emission both run, so node boundaries cannot disagree |
| `write-trees.c` | One record generator per generated tree and the one loop that feeds it to the packer, which planning runs with no sink and emission with the writing sink |
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

* `lib/core/unicode.[ch]`: `fsimg_utf8_decode()` and `fsimg_utf8_encode()`,
  the strict UTF-8 decoder and the encoder of one code point, between which
  `unicode.c` builds a comparison form.
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
Database 17.0.0, the version macOS applies (E4), with the digests of the pinned
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

* the type table: given a compression type, whether the module knows it
  and, if so, its facts: where its bytes are kept (inline after the
  header, or the resource fork behind a pair table or a word table), its
  codec (none, zlib, LZVN, LZFSE or LZBITMAP) and its raw marker, `0xFF`,
  `0x06`, `0xCC` or none. Under a codec, a payload or chunk that begins
  with the marker is the content after it; under none, every payload and
  chunk of a marked type is the marker and the content, and an unmarked
  type's payload is the content;
* the raw-marker rule: given a type's facts and one payload or chunk with
  its decoded length, it returns the content stored unencoded, or says the
  bytes are the codec's stream, or rejects a missing `0xCC` or unencoded
  content of the wrong length.

A rejection is `FSIMG_ECORRUPT` with a message that names the framing and
no format, and a failure of the callback passes through unchanged. The
module owns the five decoders of the framing, for the header, the resource
header, the resource table, a pair-table entry and a word-table entry, with
their types and length constants, the relation checks of `open` and
`chunk`, the type table, the one statement of every type's facts, and the
raw-marker rule. Each driver's `compress.c` owns the rest: an explicit list
of the types the format admits, so a row added to the table admits nothing
by itself, how the stored bytes are reached, the decode window, the copy of
unencoded content or the call of the codec's decoder, and the work
charges.
The [plan](../docs/apfs-plan.md#common-changes) names the HFS Plus
functions the module is built from.

The APFS framing is the HFS Plus one field for field (E7): the
header, the zlib type's resource header, resource table and pairs, and the
offset words of types 8 and 12, with the relations that `open` and
`chunk` check, so APFS uses the shared validator unchanged and keeps no validator
of its own. Type 10 uses the word table of types 8 and 12. The type table
holds what E7 found, which the HFS Plus reader shares: an LZFSE payload or
chunk stored uncompressed begins with the raw marker `0xFF`, as zlib's
does, while a compressed one begins with `bvx2` or `bvxn`, and a type 9
payload or type 10 chunk is the marker `0xCC` and the bytes, as the
[format design](design-apfs.md#transparent-compression) states. What APFS
adds is admission: it lists types 3, 4, 7 to 12, leaving out 1, of which
no specimen exists, and 13 and 14, which have no decoder, while HFS Plus
also lists type 1 for its documented meaning. The
storage around the framing is APFS's own: the `com.apple.decmpfs` value
may be held in its record or in a stream, a resource fork that supplies
content is always a stream, and both streams are read through the content
cursor; a size-zero resource-fork attribute beside an inline type is
accepted in either form and supplies nothing.

## Codecs

Every APFS integer is little-endian and decoded with the shared
[scalar codecs](design-binary-primitives.md) after its span is proved; the
big-endian resource-fork header of a compressed file is decoded by the
shared [framing validator](#compression-framing), so no file of the driver
reads a big-endian field.
Decoded records are C structures with `_t` typedefs. A decoder judges a
record's shape: it proves every byte it reads inside its span and that the
fields framing the record, such as a count of entries, a node's areas and
table-of-contents entries or a state buffer's size, agree with that span,
and a rejected record leaves its output untouched. Values are the
callers': magic, block size and geometry are `descriptor.c`'s and
`checkpoint.c`'s; kind, subtype, storage class, OID and XID are
`object.c`'s; key order, levels, node flags against the tree, alignment and
record lengths are `btree.c`'s; object-map flags and values are `omap.c`'s;
and the offsets of the space manager's arrays are proved by the pager in
`spaceman.c`, which reads them in windows. The records:

| Type | Fields |
| --- | --- |
| `apfs_obj_t` | checksum, OID, XID, kind, flags (the storage class among them), subtype |
| `apfs_nx_t` | block size, block count, the three feature masks, UUID, next OID and XID, both area descriptions and this checkpoint's runs, space manager, object map and reaper OIDs, maximum volume count, volume OID array |
| `apfs_cpm_block_t`, `apfs_cpm_t` | a mapping block's flags and count; a mapping's kind, flags, subtype, byte size, volume OID, ephemeral OID, address |
| `apfs_omap_t` | flags, snapshot count, tree type and OID, snapshot tree type and OID, most recent snapshot, pending revert range |
| `apfs_omap_key_t`, `apfs_omap_val_t` | OID and XID; flags, size, address |
| `apfs_node_t` | node flags, level, key count, the four locations, the areas they imply, and for a root the footer (`apfs_btree_info_t`) |
| `apfs_toc_t`, `apfs_entry_t` | a table-of-contents entry as stored; its key and value spans in the node, or a ghost |
| `apfs_spaceman_t` | the geometry counts, each device's counts and address-array offset, the internal pool's fields and bitmap ring, the three free queues, version and structure size |
| `apfs_cib_t`, `apfs_chunk_info_t` | a chunk-information or address block's index and count; a chunk's XID, first block, block and free counts, bitmap block |
| `apfs_fq_key_t` | a free-queue record's XID and first block |
| `apfs_reaper_t` | the reap IDs, list head, tail and object count, flags, and the fields of an object being reaped |
| `apfs_reap_list_t` | next OID, entry count, first and last entry indexes |
| `apfs_vol_t` | index, the three feature masks, flags, role, UUID, name, tree types and OIDs, counts, group UUID, integrity and sealed-tree OIDs |
| `apfs_jkey_t` | a filesystem-tree key's header: owner OID (60 bits) and record kind (4 bits) |
| `apfs_xfields_t`, `apfs_xfield_t` | an extended-field blob's span, field count and used data; one field's type, flags, size and data span |
| `apfs_dstream_t`, `apfs_dir_stats_t` | a data stream's size, allocated bytes, default crypto ID and byte counters; directory statistics' child count, total size, chained key and generation |
| `apfs_inode_val_t` | parent, private ID, four signed times, internal and BSD flags, count, protection class, write generation, owner, group, mode, uncompressed size, extended fields |
| `apfs_drec_key_t`, `apfs_drec_val_t` | parent, name span, stored hash (hashed keys); child, add time, flags with the type, extended fields (the sibling ID among them) |
| `apfs_name_key_t`, `apfs_xattr_val_t` | an attribute's or snapshot name's owner and name span; an attribute's flags, stored data length, and the inline data span or the stream OID and data stream |
| `apfs_sibling_key_t`, `apfs_sibling_val_t` | inode and sibling ID; parent and name span. A sibling map's value is an inode number, a data-stream ID's a reference count |
| `apfs_extent_key_t`, `apfs_extent_val_t` | private ID and logical offset; length (56 bits), flags (8 bits), physical block, crypto ID |
| `apfs_pext_val_t` | a physical extent's length in blocks (60 bits), kind (4 bits), owner, signed reference count; its key is a bare header naming the first block |
| `apfs_snap_meta_val_t` | a snapshot's extent-reference tree OID and type, superblock OID, signed creation and change times, inode number, flags, name span. A snapshot name's value is its XID |
| `apfs_file_info_key_t`, `apfs_file_info_val_t` | private ID, block address (56 bits), info type (8 bits); hashed length, hash size, hash span |

Filesystem-tree keys and values are decoded separately, as the tree stores
them, so a search key is built without a value; `inode.c`, `directory.c` and
`content.c` join a record's key and value into the entry they serve. A
borrowed name or data span points into the decoded bytes. A key's header
selects its record's decoders; a key that is the header alone is decoded by
one function that requires exactly that length and the expected kind, so no
caller has to remember the length. A crypto-state record is identified by
its kind and never decoded. The split of checks:

| Owner | Checks |
| --- | --- |
| `codec.c` | fixed parts; each key's kind; a stored name ending the record exactly with its NUL; an attribute's single layout flag (inline or stream) and the data length it implies; a file-information hash ending the value; an extended-field blob whose headers and data fill it exactly, each field's data zero-padded to 8 bytes, the used count equal to the padded data (E2, E3) and no type repeated |
| `inode.c` | inode kinds, mode and type agreement, internal flags, extended-field types and their sizes, the name field, the device number |
| `directory.c` | name validity and length limits, the stored hash, the sibling ID's rules |
| `content.c` | attribute flags beyond the layout flag, the inline bound, extent flags, crypto IDs, extent tiling |
| `verify.c` | physical-extent kinds, owners and reference counts |
| `volume.c` | snapshot metadata and names |

Encoding is the exact inverse and writes every reserved byte as zero, so an
emitted object contains no unspecified byte; the space manager's data-zone
state, which nothing reads, is not decoded and is encoded as zero, its
value in every E1 capture. The Fletcher-64 function is
tested against blocks captured in E1, whose stored checksums the format
design's [arithmetic](design-apfs.md#object-header-and-checksum)
reproduces.

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
   areas are validated as the format design states, by the checks the
   probe shares.
2. **Checkpoint.** `checkpoint.c` reads the descriptor ring once with one
   block window. For each slot it records, in a paged array of fixed-size
   slot records in metadata scratch, the block's kind (superblock, valid
   mapping block or other), XID, last-map flag, its `N`, data base and data
   count, whose geometry the pass validates, and its superblock run
   fields. Candidates are sorted by XID, which rejects duplicates, and
   judged from the largest down against the slot records alone, never
   rereading a block; the first whose run is whole is selected, and a larger
   one whose run is not sets `clean` to false. A block 0 newer than the
   selected checkpoint fails the open. The selected run's mapping
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
7. **Snapshot.** With `snapshot=`, the snapshot metadata tree is searched for the
   name record, the metadata record is read, and the snapshot's volume
   superblock, a physical object whose header OID is its address and
   whose XID is the snapshot's, replaces the live one; the
   lookup XID for the volume's virtual objects becomes the snapshot's.
   The copy names no object map (E9), so the live volume object map
   stays the one searched; the copy is checked by its own rules in the
   [format design](design-apfs.md#snapshots), not by step 5's.
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
magic, given the bytes it holds and the volume length; it verifies the
checksum when the bytes hold the whole block. Step 1 calls it on the block
it read, which is the whole block, and the probe on its 4096-byte prefix,
which holds the whole block only when `B` is 4096, so `open` makes every
validation check the probe makes, as the
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
`(storage class, OID, XID, kind, subtype, map, headerless)` and a window of
`B` bytes; `headerless` is set only by the cursor of a sealed volume's
filesystem tree:

* physical: the OID is the block, which must lie in `[1, N)`;
* virtual: `omap.c` resolves the OID at the reference's XID through the
  named map's cursor, and the object's XID must equal the found key's.
  The found value's no-header flag must equal `headerless`, or the read is
  `FSIMG_ECORRUPT`;
* ephemeral: binary search of the paged mapping array.

It reads the block with one exact read, verifies the checksum, and checks
kind, subtype, storage class, OID and XID. A headerless node skips these
checks; it is accepted only through its digest, which `btree.c` checks
before the node is used. One object can span several
blocks, the space manager. Its reader, `apfs_spaceman_read()`, decodes the
fixed structure from the first block, unverified, only to learn the size
the [geometry implies](design-apfs.md#ephemeral-objects-and-the-space-manager)
for its structure and arrays, and passes that size in the reference, so a
mapped size that differs fails before the read; its blocks then pass
through the same
window one at a time, the checksum running over them, and its fixed
structure, which lies in the first block, is decoded from there. The arrays
after it can run into later blocks (E12: from 2520 to 6736 at 128 TiB), so
the pool ring's XID, bitmap-index and free-next arrays and the address
array are read later, by offset and in windows, by the verification pager.
No buffer is
sized from a mapped size. A failure names the object's role, never its
bytes.

## B-trees

A cursor belongs to one tree: its root reference, storage class, comparator
and record codec, and for a sealed volume's filesystem tree the hash
length and the expected root digest. In such a tree `btree.c` adds the
root's OID to each index value's child OID, an overflow being
`FSIMG_ECORRUPT`, reads the child as a
headerless reference, and compares its digest with the parent's value, or
the root's with the integrity object's root hash, before the
[validation checks](design-apfs.md#validation) run; a mismatch is
`FSIMG_ECORRUPT`. It holds the current root-to-leaf path, one window and one
table-of-contents position per level, plus the separator bounds each level
was entered under. Windows are allocated as the path deepens and kept for the
session; a tree of height `h` holds at most `h` windows, and the format
design bounds `h` by 16, a taller tree being `FSIMG_EUNSUPPORTED`.

`btree.c` offers three operations:

* `seek(cursor, key, mode)`: position on the last record at or below the
  key, or the first at or above it, so among records the comparator calls
  equal to the key on the last or the first. The search starts from the
  deepest level whose separator bounds strictly contain the key, so
  consecutive nearby seeks read only the nodes that change; otherwise it
  descends from the root, validating each node as it is loaded. Descending
  for the first record at or above the key takes the child before the first
  separator not below it, because where a tie-break is undecided that child
  can end in records equal to the key.
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
records are used, and a scan is bounded by the tree's distinct nodes. The
exception is an undecided class, below.

A comparator is a weak order whose classes of equal keys are decided,
holding at most one key, or undecided, where the format design's
[comparators](design-apfs.md#comparators) leave the tie-break open. The
profile names the undecided classes; their keys may lie in either order,
within a node and across nodes, and no key lies out of order; equal keys
of one such class are their consumer's to refuse. Two children's ranges can
then share keys of one undecided class, so a subtree holding only such keys
can lie under two separators, and nested, the repetition multiplies. The
cursor counts the nodes it loads from each seek or first and refuses, as
`FSIMG_ECORRUPT`, a walk loading more than the slice has blocks plus the
height, which no valid tree reaches; a scan is thus bounded by the image's
size, and the records repeated belong to the one class.

The filesystem tree has a profile per directory-record key variant. In the
hashed profile every class is decided. In the plain profile a plain
directory record orders by its owner and kind alone, since no weak order
on the names can agree with every order the format leaves open; the
records of one directory form one undecided class, a name lookup scans the
directory, and a record repeated as above is refused by the walker's key
sort as an equivalent name. The profile's entry check judges every key, in
index and leaf nodes, with its kind's decoder, so the comparator orders only
well-formed keys; values are judged by the modules that decode them, so a
damaged value costs its own record and not the node that holds it.

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
hash is `fsimg_crc32c_update()` from `FSIMG_CRC32C_SEED` over the code points
as little-endian 32-bit units, without the final complement, masked to its low
22 bits, as the format design gives. The namespace key is the form as
UTF-8, bounded by the same expansion, so it fits the common key buffer of
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
and hash, a mismatch on a name holding a code point unassigned in Unicode
17.0.0 or a noncharacter being `FSIMG_EUNSUPPORTED` and any other
`FSIMG_ECORRUPT`, and the value: the child inode at least 16 (or in the volume-group
space) and a type in the format design's table. It admits the child with a
25-byte payload: the child inode (u64), the record type (u8), the sibling
ID or zero (u64) and the directory holding the record (u64), little-endian;
replay needs the last for the parent rule, since the walker may prepare
other directories between a directory's preparation and its children's
replays. Admission charges the entry; the walker's
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
  no claimed-object set because a cycle through the root would give a
  directory two parents and a second record would give it a second name.
  A cycle the root cannot reach is found by
  [verification's walk](#verification), which must reach every directory.
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
  resource-fork type, the fork attribute's data-stream reference. A header
  held in a stream is read here: the value's length is checked against the
  common [bounds](design-apfs.md#transparent-compression), its 16 bytes
  are read through the content cursor, and a resource-fork type's length
  is then checked to be exactly 16. This is the one read `replay` makes
  outside the trees. An inline payload held in its record stays there,
  where `resolve` finds it again by name; one held in a stream is recorded
  by its data-stream reference, as a fork is.

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
  decoded from its attribute record, or its payload, the stream's bytes
  after the header and at most `FSIMG_LZFSE_INPUT_MAX` of them, is read
  into the stored-chunk buffer. For a resource-fork type the first
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
  produced are charged as work. A file of a type `compress.c` does not
  admit, type 1 or LZBITMAP's 13 and 14, is listed with its header's size.
  The first `resolve` refuses it with `FSIMG_EUNSUPPORTED`. An empty one,
  which no `resolve` reaches, therefore reads as empty, and under salvage a
  nonempty one keeps its entry with no bytes.

Mapped extents are checked against `[1, N)` before they are returned;
delivery then clips them to the request and the initialized length and reads
only inside the content domain. The driver checks the same range for the
stored bytes of a chunk, which delivery never sees. A held window is valid
until the next `resolve` or advance, as the content design requires.

### Salvage

The driver takes part in [salvage extraction](design-salvage.md) with no
salvage state of its own. `replay` reads no extent but a streamed compression
header's, so the common plan, which
resolves a file from its first byte, meets damage exactly where a read
would, and what survives follows from `resolve`:

* **Uncompressed file.** The prefix ends at the first offset whose lookup
  the decoder rejects: an invalid node on the content cursor's path, a
  malformed file-extent record, a gap or overlap in the tiling, an extent
  outside `[1, N)`, a refused flag or crypto ID. An extent that runs past
  the slice ends the prefix at the slice's end, byte for byte. The record
  after the extent that reaches the size is checked with it, since no
  later lookup reads it: a record of the stream starting below the size
  contradicts delivered bytes and rejects that extent's lookup, while a
  damaged next node, a malformed next record or a record past the size
  judges no delivered byte and costs the file nothing; the walk or
  verification reports it where it meets it. The size is the stream's,
  not the prefix the plan shortens the file to, so copying the prefix
  meets the records the plan met.
* **Shared extents.** No lookup reads the extent-reference tree, so damage
  there costs reading nothing and verification reports it. A clone, or any
  file whose extents another file shares, is resolved through its own
  records and keeps its own prefix, whatever became of the other owner.
* **Compressed file.** The plan decodes the file chunk by chunk. Its prefix
  ends on the 64 KiB boundary before the first chunk that a damaged table
  entry, a fork extent or the decoder rejects, and no later chunk is
  recovered, as on HFS Plus. A fork's framing and an inline payload are
  checked by the first `resolve`, so damage there leaves no recoverable
  bytes; a damaged streamed header is a rejection in `replay` and costs
  the entry. Copying
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

**Container pass.** It starts from the checkpoint open selected and checked,
its superblock, mappings and space manager, and re-reads the reaper with its
reap-list objects (through
`apfs_object_read()`, to decide whether it is idle) and the container object
map, and appends
claims `(first block, count, class, owner)` to a claim stream: the
[fixed regions](design-apfs.md#ephemeral-objects-and-the-space-manager),
one claim each, as `apfs_fixed_regions()` lists them from the selected
superblock and the space manager the checkpoint load checked, the copy the
pager walks; the block of each object map itself, the
container's and each volume's, and every node of its tree; every live
object version each object map maps, at its size, tombstones excluded;
every physical tree node; each snapshot's volume superblock; a nonzero
keylocker range, as one range each;
every main free-queue entry; and the space manager's chunk-information,
address and chunk bitmap blocks that lie outside the internal pool. The
writer's layout calls the same `apfs_fixed_regions()` on the fields it
plans, so the regions verification claims and the regions the writer
allocates are one list. The reaper contributes no claim: it must be
[idle](design-apfs.md#reaper), or the report is incomplete. The
space-manager blocks inside the pool and the internal-pool free queue's
entries go to a second, smaller claim stream for the pool's own domain,
merged against the pool's bitmap, so no block is claimed in both.
For each volume it scans the object map with the object map's snapshot
tree, the snapshot metadata tree, each snapshot's volume superblock, every
extent-reference tree (the live one and each snapshot's), and the
filesystem tree with dedicated cursors, and in the sealed profile the
sealed file-extent tree. Each tree is walked from its first record to its
end, and the nodes the walk loads and the records it visits must equal its
footer's node and key counts, since a walk of a valid tree loads each node
once. Each physical-extent record appends a data claim,
owner `(volume, tree)`; the filesystem scan, or the sealed file-extent
scan, appends a file-extent record `(block, count, volume, stream ID)` for
each mapped extent to a third stream; hole extents, whose physical block is
zero, have no physical-extent record and append nothing.

**Record reconciliation.** The filesystem scan visits each object's records
together because they share the owner prefix, so most checks are local and
need no storage: the inode record (kind 3) is followed by its attributes
(4), sibling links (5) and, for a directory, its directory records (9),
whose count must equal the child count; a stream's data-stream ID (6) and
extents (8) lie together under its stream ID, which a clone or a written
clone branch can make differ from the inode number, so extents tile their
stream wherever it
lies; attributes and extended fields are well formed; counts by kind
accumulate for the superblock. Facts that join different owners go to
streams that are sorted once:

* name references `(child, parent, type, has sibling ID)`, joined with
  the inodes in scan order to check existence, kind, a directory's single
  parent, each file's link count, and that an inode's names carry sibling
  IDs exactly when it has sibling links, one per name;
* sibling links `(sibling ID, inode, parent, name)`, joined with the same
  facts of every directory record that carries a sibling ID, so each link
  equals the record it describes and every such record has its link, and
  with the sibling maps `(sibling ID, inode)`. A join rather than a lookup
  per link keeps the cost a sort: a plain-key directory's names take no
  part in the key order, and a lookup in one would scan the directory;
* stream uses `(stream ID, kind, size, allocated bytes)`, from each inode's
  data-stream field and each stream attribute, joined with the streams
  found by the scan, `(stream ID, data-stream ID count or none, end of the
  last extent)`: an inode stream's count equals its inode uses, an
  attribute stream has one use and no data-stream ID record, as native
  volumes write it, every stream found has a use, and no stream's extents
  end past the larger of a user's size rounded up and its allocated bytes;
* collision records `(parent, comparison form, record)` for every
  directory record, sorted by parent and comparison form, so equivalent
  names are found whatever their stored hashes say, including when the
  hashes are wrong, and in volumes the common walker does not visit;
  adjacent equal forms are a collision, so an adversarial directory costs
  a sort, not quadratic comparisons.

**Chunk tables.** For each compressed file of a resource-fork type in the
selected volume, as the common walk makes it current, structural
verification runs the shared validator's `open` and then `chunk` for every
entry in ascending order, reading the table through the fork's file
extents and decoding no payload, as HFS Plus checks its tables in
`content_check()`; LZBITMAP's type 14 is checked too, since the check needs
no decoder. The walk from the root never makes current a file whose
inode's parent field is the private directory, so no entry of such a
file's table is read: when the record scan finds a compressed one with a
resource fork, the report is incomplete. It holds three table entries and
charges one unit of work per entry, so a damaged later entry fails `W`
without payload read-through.

**Allocation merge.** The claim stream is sorted by first block. The merge
walks it with `spaceman.c`'s pager, which visits the chunks in ascending
order, each address and chunk-information block once as its first chunk is
reached, and reads each chunk's bitmap block, one block at a time. The
container pass walks a pager of its own, reading no bitmap, for the claims
of those blocks. For each block
it checks the format design's rules: metadata claims never overlap any other
claim; data claims overlap only data claims of other trees of the same
volume; a block is allocated exactly when claimed; each chunk's free count
and the device's free count equal the clear bits. The file extents are
joined with the physical-extent records of every extent-reference tree of
the volume, the live one and each snapshot's, in one sweep: each record
contributes an opening at its first block and a closing at its end, both
carrying its signed reference count, and each file extent a start and an
end, all keyed `(volume, block)` and sorted once, openings and starts
before ends and closings at one block. The sweep keeps two totals, the
signed sum of the reference counts of the records open and the number of
file extents started and not ended, and wherever the block advances, the
span just passed must have them equal. So a mapped block outside every
record fails, as does a block whose summed count no stream matches, and a
block only snapshots keep has a summed count of zero, never a negative
one. Records of one tree never overlap, so over a span each tree adds one
record's count or none. The join costs the sort of twice the
extents and records together, however many records one extent spans, and
each boundary the sweep reads is charged to `max_work`. Clones that share
one data stream contribute one stream. The pool's claim stream is merged the
same way against the pool's bitmap. The main bitmap's bits past `N` are
not checked ([verification](design-apfs.md#verification)).
The check the format design holds
[provisional](design-apfs.md#verification), the allocated block count of a
volume that has snapshots, is not made until its experiment gives the rule;
every other volume's allocated block count, a sealed volume's included, is
checked against the blocks the format design lists there.

**Walk.** The common walker visits the selected volume once, in its live
view: verification ignores `snapshot=`, since its scans, claims and counts
are the live volume's. Every
directory but the root and the private directory has exactly one parent,
so the directories the walk reaches from the root form a tree, and a cycle
of directories the root cannot reach exists exactly when the walk reaches
fewer directories than the filesystem scan counted, which the superblock's
count equals; such a shortfall is `FSIMG_ECORRUPT` naming both counts. A
directory record in the private directory that names a directory
(provisional, E16) makes the report incomplete instead, and the count is not
judged in that volume, since the walk from the root cannot reach it. The
walk descends into omitted directories, so a directory counts whether or
not it is delivered. A volume verification does not walk has no such
check: finding a cycle there would take repeated joins of each
directory's parent chain, about the logarithm of the deepest path in
sorts. Every
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
complete after skipping an object. So do structures whose blocks no claim
describes, or whose rules no capture shows: a nonzero EFI jumpstart; a
volume's sealed file-extent tree or integrity object in a profile that
does not read them; and a snapshot with nonzero flags, an object-map
snapshot record with a nonzero flag, or a snapshot of a dataless-snapshot
volume. Such
blocks cannot be told from damage, so an incomplete container pass makes
no allocation merge; every structure is still read, and damage found
outranks the incomplete report. The extent-reference and snapshot trees
are subtypes 15 and 16 (E3, `apfs-newfs-268435456-e3-ci`).

## Writer settings

`validate` accepts the common options under the format design's
[settings](design-apfs.md#settings-and-identities): `sector_bytes` and
`allocation_unit_bytes` zero or 4096; `label` NULL or empty, selecting the
[default label](design-apfs.md#writer-values), or valid UTF-8 of at most
255 bytes; `fat_root_entries`, `byte_order`,
`inode_capacity`, `reserve_percent` and `mountpoint` at their defaults, which
the common settings check enforces; `volume_uuid` nonzero unless
`FSIMG_WRITE_REPRODUCIBLE` is set; under it, `epoch` inside the writer's
time range.
`volume_serial` is ignored, since the CLI generates one for every format.

`volume_bytes` is zero or a multiple of 4096 of at least the writer's
minimum container size. That minimum, 1024 blocks, is a
[writer value](design-apfs.md#writer-values), and can be larger than the
1 MiB below which the reader refuses a container: the two bounds answer
different questions and `validate` uses only the writer's. A
`volume_bytes` above the writer's largest container, 34359738368 blocks
(128 TiB), fails with `FSIMG_ERANGE`.

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
  for `n` entries, four bytes each in a fixed-size tree and eight otherwise:
  `n` rounded up to a multiple of 8 entries and at least 8, and in a
  fixed-size tree at least the most records of the node's kind, leaf or
  index, that `C` holds with their entries, also rounded up. A key or
  value counts its length rounded up to 8 bytes unless the tree is
  unaligned. The table sizing and the profile's alignment, flags and fill
  are [writer values](design-apfs.md#writer-values).
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
  record, which the reader's validation and `fsck_apfs` accept at the leaf
  and index levels ([writer values](design-apfs.md#writer-values), E15).
* **Progress.** The largest record the writer emits, leaf or index (the
  longest key and its 8-byte child OID), is bounded by its name and
  link-target limits. A static assertion proves that such a record fits an
  empty node, so a record always fits a fresh node.
  The packer admits a record only when it fits an empty node and two
  index records carrying its key fit one, so every index node closed full
  has at least two children, only a level's last node may have one, and
  the height grows with the logarithm of the record count.

The packer writes through a sink. Planning passes none and takes the node
counts of each level the packer returns; emission's sink writes each node
as it closes. Both number nodes by
completion: the `k`th node closed is node `k` of its tree, children before
their parent and the root last. The root's footer statistics are totals the
packer accumulates.

### The plan

The plan object embeds `fsimg_plan_t` and holds the geometry, the table of
fixed regions, the run's first block and the allocated block count, the
volume flags, the format time, the UUIDs, each tree's node counts and first run index, the ephemeral table,
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
   disagree. It fills the region list for `N` and checks that the regions,
   in ascending block order, form one gapless prefix from block 0, as the native formatter
   places them; a list that does not is an internal error
   (`FSIMG_ESTATE`), never a run around a region. The run starts after the
   prefix and holds the metadata blocks and then the file data,
   contiguous, so it ends at the prefix plus the metadata blocks from
   step 4 plus the data blocks from step 1, and the allocation is the
   blocks below that end. `N` fits when the end plus the requested free
   space is at most `N`. A probe writes no row and allocates nothing: its
   cost is proportional to the region count.

   With exact `volume_bytes`, `N = volume_bytes / 4096`, and an `N` that
   does not fit fails with `FSIMG_ERANGE`. With automatic sizing, `N` is
   the first block count on the native size step that fits in the search
   the format design's [layout](design-apfs.md#layout) states. It
   starts at the lower bound, which is block 0, the run's metadata blocks
   from step 4, the data blocks from step 1 and the requested free space,
   raised to the minimum container and rounded up to the step. A failed
   probe returns its shortfall: the smallest count holding the probe's
   layout is the run's end plus the requested free space, and the
   shortfall is what that count exceeds `N` by. The next probe adds it,
   rounded up to the step and at least one
   step. Each probe
   is charged as work, and the
   scan fails with `FSIMG_ERANGE` naming the
   [probe cap](design-apfs.md#writer-values) when it reaches it, or the
   writer's largest container, 34359738368 blocks, when a probe passes it.
   The maximum volume count follows from `N`.
6. **Layout.** Assign every block and identifier, in this order:
   * **Regions.** `apfs_fixed_regions()` fills the table of fixed regions
     from the superblock and space-manager fields planned for `N`, as it
     does for verification from decoded fields, and the writer's geometry
     step checks that they form the prefix step 5 requires. `run_block(i)`
     is the block after the prefix plus `i`.
   * **Metadata.** Each metadata object has a run index and sits at
     `run_block` of it. The indexes follow the format design's order: the
     container object map's own block and then its tree, the volume
     superblock, the volume object map's own block and then its tree, the
     filesystem tree, the extent-reference tree, the snapshot tree. Node
     `k` of a tree has the tree's first index plus `k`.
   * **Files.** Each nonempty file, in catalog order, takes one contiguous
     run starting at the block after the previous one, the first after the
     metadata blocks: a file whose data starts at offset `b` of the data
     blocks starts at the run's first block plus the metadata blocks plus
     `b`. Its row records the first block, and file runs ascend with the
     catalog ID.
   * **Allocation.** The fixed regions, the metadata blocks and the file
     runs are one range from block 0 to the run's end, the only
     description of allocation the space manager is built from.
   * **Identifiers.** OIDs are assigned in the order the
     [writer values](design-apfs.md#writer-values) give; filesystem node `k`
     takes the first node OID plus `k`. The container's next OID follows
     the last.
   * **Ephemeral table.** The space manager, whose size the geometry gives,
     and the reaper are listed in OID order with
     kind, subtype, byte size and their slots in the checkpoint data area,
     assigned in that order from the first slot.
   * **Internal pool.** Count the chunk-information blocks, the address
     blocks and one bitmap block per chunk that the allocation
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
and the bytes do not depend on the order. Every tree, the filesystem tree
included, has one record generator that both phases call: it reads the
plan, and the only values it encodes that layout changes are fixed-width
addresses, as is the file-extent block emission completes in place, so
planning's counting run and emission's writing run see records of the same
lengths in the same order. The packer places node boundaries from record
lengths and count alone and never compares keys, so planning's addresses
need not be final. A tree's node count
therefore equals the planned one by construction; emission still refuses a
node past it before writing it and a shortfall after the root, since every
block the tree occupies was placed from that count. Either is an internal
error, `FSIMG_ESTATE`.

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
   target reaches the output. The writer emits extended attributes only for
   symbolic links, so an attribute record on any other row is an internal
   error; a writer that adds attributes changes this check with them. Leaves are written as they close and index
   nodes as their children complete, the root last, with its footer.
3. **Other trees.** The volume object map's tree, from a counter over the
   filesystem nodes: record `k` maps the first node OID plus `k` to that
   node's block. The extent-reference tree, from the rows of the nonempty
   files in catalog order, which is block order and so key order. The empty
   snapshot tree, the volume object map, the volume superblock, and the
   container object map's tree and object.
4. **Space manager.** One cursor walks the chunks in ascending order
   against the allocated range. For each chunk it counts the allocated
   blocks inside it, which gives the entry's free count, and when that
   count is not zero it writes the chunk's bitmap block at its pool block:
   whole bytes of ones up to the range's end, then zeros, and the partial
   byte at the edge. What the bits past `N` hold, and the entry of a
   chunk with no allocated block, which gets no bitmap block, are writer
   values. The entries fill the
   chunk-information blocks in chunk order, each written when it is full
   and the last as far as it is filled, over its zeroed buffer,
   and when the geometry calls for address blocks they list the
   chunk-information blocks the same way. The pool's own bitmap marks
   exactly the pool blocks taken. Last comes the space manager object:
   its address array names the blocks just written, and its device free
   count is the sum of the entries' free counts, which is `N` less the
   allocated blocks.
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
| Probe and open | Block 0, one pass over the descriptor ring, one reread of the selected run, the selected checkpoint's mapped objects (one data run), the container and volume map paths, the volume superblocks; the slot-record array and the paged mapping array |
| Listing | The root inode, which the level-0 `prepare` checks; per directory: one scan of its records and the common key sort; per entry: one inode lookup and one scan of that inode's attribute records, which follow it. No file payload is read |
| Content | Per sequential read: one cursor advance per extent; per random offset: one tree descent; per compressed chunk: one decode of at most 64 KiB, and for an inline payload one attribute lookup; per resource-fork file: one framing check at the first read, and one table entry with its neighbours per decoded chunk |
| Verification | One scan of every tree of every volume; the main and pool claim, file-extent, name-reference, sibling-map, stream-use and collision streams and the extent boundaries, each sorted once; two passes over the chunk structures (claims, then the merge) and one over the bitmaps and pool bitmap; one walk of the selected volume, reading each inode's content once; the array of multiply linked inodes |
| Planning | One catalog pass, two external sorts (collisions and records), one counting run of the packer per tree, the extent-reference tree's over one pass of the rows, a size scan of at most the probe cap's probes, each costing constant work per fixed region, one layout pass |
| Emission | Payload bytes, one replay of the sorted records, and metadata proportional to entries and container size |

Resident state per reader is the geometry, the selected superblocks, the
cursors' windows (at most 16 per cursor), and, once a compressed file has
been resolved, the 64 KiB decode window, the stored-chunk buffer of
`FSIMG_LZFSE_INPUT_MAX` bytes and the codec's state: the LZFSE decoder's
scratch, or zlib's inflate state through the common allocation adapter. The
link-target buffer is the reader core's. Resident state per writer is one
node buffer per level of the tree being packed.
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
containers with it. Its names use a small alphabet whose comparison forms
it computes itself, plus the E4 vectors with their recorded hashes
(`test/fixtures/apfs/name-hashes.txt`), so it needs no normalizer of its
own. It builds containers in memory at every legal block size, with
chosen checkpoint rings, object maps, multi-level trees, volumes, records
and extents, and mutates them.
`test/apfs.c` runs the format design's
[acceptance matrix](design-apfs.md#acceptance-and-independent-validation)
over it. Beyond that matrix come the common contracts the driver joins, the
salvage row in `test/apfs-salvage.c` and the others in `test/apfs.c`:

| Contract | Passing case | Paired failure |
| --- | --- | --- |
| Truncation | A container cut past everything a listing and an extraction touch opens, lists, extracts and reports the missing bytes | The same container cut through the descriptor ring, the selected data run, a tree node and a file extent fails `FSIMG_ECORRUPT`, "image is truncated", where each is reached. A cut that removes the newest checkpoint fails the open, while the same checkpoint torn in place, its superblock valid and a mapping block invalid, opens the next older checkpoint with `clean` false when block 0 is no newer than that checkpoint, and fails when block 0 is newer. Full verification fails on every cut |
| Assumed identity | A container whose block-0 magic is rewritten and whose checksum, which covers the magic, is repaired opens and lists under `FSIMG_READ_ASSUME_IDENTITY` | Without the flag it is `FSIMG_EFORMAT`; with it, the magic rewritten without the repair fails the checksum, each other validation mutation of block 0 still fails with its class, a descriptor block without its magic is not a candidate, and full verification fails naming the assumed check |
| Link target | A link's target is listed and extracted with `resolve` instrumented to fail for a link, at 1 and 1023 bytes | A target with an interior NUL, without its final NUL or of 1024 bytes fails in a listing, which reads no content |
| Extra data | An inode with one unconsumed attribute sets `FSIMG_META_EXTRA_DATA` on each of its names | A link, and a compressed file of each storage, holding only the attributes the driver consumes, do not set it, nor does `ditto`'s size-zero resource-fork attribute beside an inline type; `com.apple.decmpfs` on an inode that is not a compressed file does |
| Salvage | A file whose third extent record is damaged is extracted as a `.partial` of its first two extents; a compressed file with a bad chunk keeps the chunks before it; a damaged inode costs its entry and a damaged directory record its listing, each reported, with the siblings extracted | Each fixture fails as a whole without salvage; an injected read fault, scratch failure or cancellation during salvage is terminal, not a loss; damage in the extent-reference tree changes no extracted byte and fails verification |
| Verification counts | A file with three names adds three to `files` and three sizes to `logical_bytes` and its size once to `payload_bytes_read`; a hole, a compressed file and a link add nothing to that count | The same file with a wrong link count fails before any payload read; a read fault injected in one of its blocks fails the payload scope once, not once per name |
| Chunk table | A resource-fork file of many chunks, of each table kind, reads from a random offset | A count that does not cover the size fails the read of the file's first byte; an entry outside the payload fails the read of its own chunk while the chunks before it read correctly; an entry out of order or overlapping a neighbour fails at the first chunk whose neighbour check sees it, which can be the chunk before it; full verification fails the file in every case |
| Collisions | A two-volume container whose unselected volume holds names that differ only in case on a case-sensitive volume verifies | The same names in a case-insensitive unselected volume, each stored with its correct and therefore shared hash, fail `W` as a collision |
| Empty-container allocation | The generator's empty container in the native layout, its bitmap ring outside the pool and both object maps in the run, verifies complete, and every allocated block is one the claims name; the E1 empty captures do the same in the integration test | With the bitmap bit of one block of each fixed region, and of each object map's own block, cleared in turn, verification fails naming a claimed block marked free |

`test/apfs-unicode.c` checks the generated tables: each table's digest
equals the one `docs/references.md` records for the pinned inputs, and the
comparison forms of the `NormalizationTest.txt` vectors match, and so do
the hashes of the E4 vectors in `test/fixtures/apfs/name-hashes.txt`.

`test/apfs-write.c` creates containers through the public API and reads them
back through the reader and verifier and through an independent checker
written from the format design, sharing no code with the driver. It compares
bytes across source order, metadata cache size, scratch placement and time
zone, scans every unused byte for poison, and with `--native` compares an
empty container's fields with the
[writer values](design-apfs.md#writer-values): a value a row cites from E1
must equal the E1 capture of the same size, in the form the row names, and a
value built by a construction or set by policy, such as the volume slot, the
next OID, the counters or the XIDs, must equal its row's construction.
UUIDs, times and the formatting identifier are exempt. It checks the
packer at its boundaries: records that exactly fill a node and one byte
more, a level whose last node holds one record, a root with two children,
and trees of one, two and three levels; and the layout at its own: the
run contiguous after the fixed-region prefix, and the sizes on either side
of each change in the fixed regions, where the automatic size must fit and equal
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
