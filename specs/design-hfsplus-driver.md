# HFS Plus driver

## Scope

The [HFS Plus format design](design-hfsplus.md) establishes the on-disk
facts, the closed acceptance tables and the writer profile. This document
maps them onto code: the modules of `lib/formats/hfsplus/`, the driver state
behind the common reader and writer, the algorithms that keep B-tree
access, fork mapping, verification, planning and emission bounded, and the
tests that qualify them.

The driver implements the common contracts with the five additions the
format design lists, each owned by its common specification: the
[extra-data bit](design-metadata.md#metadata-fields),
[decoded content spans](design-content.md#decoded-spans), the 765-byte
label, the [seconds helper's origin](design-metadata-loss.md#unsigned-whole-second-selection)
and the [imported decoders](design-codecs.md#imported-decoders). Common code
gains no HFS-specific branch. The QNX4 driver is the structural template for
the module split, the tests and the gates; the drivers share no code.

## Modules

| File | Owns |
| --- | --- |
| `hfsplus.h` | Private types, constants and prototypes shared by the files below |
| `descriptor.c` | `fsimg_hfsplus_format`: the `hfsplus` and `hfsx` names, their capability records, the probe and the reader operations; the unregistered writer descriptor until native qualification |
| `codec.c` | Big-endian decode and encode of the volume header, fork data, extent records, node descriptor, B-tree header record, catalog keys and records, attribute keys and records, journal info block and header, master directory block, and the little-endian compression header and chunk tables; pure functions without I/O |
| `unicode.c` | UTF-16 and UTF-8 conversion with the `/` and `:` exchange, decomposition, the folded and binary comparators, namespace keys |
| `unicode-tables.h` | The generated decomposition, combining-class and fold tables and their input digests |
| `volume.c` | Open: the wrapper, the header and geometry, the journal policy, the special files, the three trees, the root and the private folders |
| `fork.c` | The fork resolver: inline and overflow extents into an extent index, continuity and domain checks, verification claims |
| `btree.c` | The B-tree reader: node reads and framing, header records, keyed search, the leaf cursor, level iteration for verification |
| `catalog.c` | Catalog record validation, the kind table, thread lookup, hard-link resolution, the record-to-entry mapping |
| `attributes.c` | The attribute scan of one node ID, attribute record validation, the extra-data mark, the compression attribute |
| `compress.c` | The compression header and chunk tables, the decode window and dispatch to zlib, LZVN and LZFSE |
| `directory.c` | `prepare`, `replay`, `keys` and `key`; format-owned root children |
| `read.c` | `fsimg_hfsplus_reader_ops`: open, close, resolve and volume information |
| `verify.c` | Full verification: tree structure, the catalog pass, the merges, the bitmap comparison, the alternate header |
| `write-hfsplus.h` | Plan rows, record streams and the plan object |
| `write-plan.c` | `validate`, `plan`, `free`: settings, the entry pass, the record sort, sizing and layout |
| `write-btree.c` | The bulk B-tree packer that planning and emission both run, so node boundaries cannot disagree |
| `write-emit.c` | `emit`: payload copies, tree nodes, the bitmap and both headers |

`fork.c` is the only place that interprets an extent descriptor or an
overflow record, and `btree.c` the only place that interprets node framing.
Every fork, the catalog's and the bitmap's included, goes through the one
resolver, so domain, continuity, sum, record-count and claim checks exist
once; every tree goes through the one node reader, so offset tables, kinds,
heights and sibling links are validated once.

## Codecs

The shared big-endian [scalar codecs](design-binary-primitives.md) decode
and encode integers in byte buffers; the record codecs check each record's
span before any field is read. Decoded records are C structures with `_t`
typedefs holding only validated values:

| Type | Fields |
| --- | --- |
| `hfsplus_extent_t` | `start`, `count` (u32 blocks) |
| `hfsplus_fork_t` | `logical_size`, `total_blocks`, eight `hfsplus_extent_t` |
| `hfsplus_header_t` | Every header field of the format design's table, the five forks decoded |
| `hfsplus_node_t` | Descriptor fields and the validated record offsets of one node buffer |
| `hfsplus_btree_t` | Header record fields, the tree's node size, comparator and fork index |
| `hfsplus_ckey_t` | `parent`, `units` (count) and a borrowed pointer to the big-endian units in the node buffer |
| `hfsplus_folder_t`, `hfsplus_file_t`, `hfsplus_thread_t` | The record fields; Finder information kept as raw bytes for the type and creator tests |
| `hfsplus_akey_t` | `file_id`, `start_block`, name units and borrowed pointer |
| `hfsplus_attr_t` | Record type, inline size and offset, or fork, or eight extents |
| `hfsplus_jib_t`, `hfsplus_journal_t` | Journal info block and header fields, the header's byte order |
| `hfsplus_mdb_t` | The wrapper fields the format design names |
| `hfsplus_cmpf_t` | Compression type and uncompressed size |

A borrowed key pointer is valid until its node buffer is reused, which the
B-tree reader's cursor discipline below fixes. Encoding is the exact inverse
and fills every reserved byte with zero, so an emitted record never contains
an unspecified byte.

## Probe and open

The probe inspects 1536 bytes. Without the
[identity signature](design-hfsplus.md#probe-identity) it makes no claim
(`FSIMG_EFORMAT`). With it, a header signature is checked for its version
pairing (`FSIMG_EUNSUPPORTED` otherwise), a power-of-two `blockSize` from 512,
a nonzero `totalBlocks` and `totalBlocks * blockSize` within the volume
length (`FSIMG_ECORRUPT` otherwise); a wrapper's master directory block is
claimed on its signatures alone, its fields being checked at open. The probe
then claims the image with `FSIMG_OK`. It performs no I/O and allocates
nothing. A forced `hfsx` on an `H+` volume, or a forced `hfsplus` on an `HX`
volume, contradicts the declared variant and is `FSIMG_EFORMAT`, as the
[architecture](architecture.md#specification-driven-testing) classifies it.

`open` completes recognition in a fixed order; each step fails before the
next reads:

1. **Wrapper.** For a `BD` block, decode the master directory block, compute
   the embedded range with checked arithmetic, require it inside the slice
   and open it as a nested slice; the driver then works on that slice alone,
   whose header at 1024 must show `H+` version 4 and pass the probe's field
   checks. The embedded volume's own boot blocks lie inside the wrapper's
   allocation and are not tested.
2. **Geometry.** Require the slice length `V` to be a multiple of 512;
   decode the header; apply the attribute table; derive `B`, `N`, `r`, `t`
   and the content domain `[r * B, (N - t) * B)` into `hfsplus_geometry_t`.
3. **Extents tree.** Build its fork index from the eight inline extents
   alone, require their sum to equal `totalBlocks`, read node 0 and check
   the header record against the tree parameter table.
4. **Special forks.** Resolve the catalog, bitmap, attributes and startup
   forks through the fork resolver, whose overflow lookups now work. A
   zero attributes or startup fork means absent.
5. **Catalog and attributes trees.** Read and check each header node; select
   the catalog comparator from the signature and `keyCompareType`; require
   `totalNodes * M` to equal the fork's `logicalSize`.
6. **Journal.** With bit 13 set, read the 180-byte info block and the
   44-byte header and apply the reader policy table. Record the journal's
   block range and the info block's block for the root's format-owned files.
7. **Root.** Look up the thread `(2, "")` and require a folder thread with
   parent 1; look up the primary `(1, name)` it names and require a folder
   record with `folderID` 2. The name is the label; a name the
   [display mapping](design-hfsplus.md#display-and-query-mapping) refuses
   leaves the label absent, so `info` omits it and verification reports the
   volume incomplete.

The reader state then holds the geometry, the header, the three tree
descriptors with their fork indexes, the bitmap's fork index, the journal
ranges, the root record, the label when valid, and the private data folder's
node ID once found. Open reads at most the header, the alternate header
under verification only, three header nodes, a few search paths and the two
journal records; it never reads the bitmap, a directory's records or any
payload. `info` reports the geometry (`V` bytes, `N` units of `B`, a 512-byte
sector), the label, the clean flag, the profile, and these details in this
order: `journal` (0 none, 1 clean, 2 uninitialized), `catalog_node_bytes`,
`extents_node_bytes`, `attributes_node_bytes` (0 without the tree),
`files`, `folders`, `next_catalog_id`, `volume_id`, `last_mounted`,
`create_date` (the stored 1904-based local value) and, for a wrapped volume,
`wrapper_offset`. `FSIMG_VOLUME_FREE` is set only by a completed
verification.

## The fork resolver

`hfsplus_fork_index()` turns `(file ID, fork type, fork data)` into a shared
[extent index](design-content.md#extent-index) over the content domain:

1. Validate the fork data against the format design's
   [extent rules](design-hfsplus.md#extent-rules): descriptor order, domain,
   the inline sum not above `totalBlocks`, `logicalSize <= totalBlocks * B`
   in checked arithmetic.
2. Append the used inline extents in order; adjacent extents coalesce.
3. While the appended blocks fall short of `totalBlocks`, search the extents
   tree for the exact key `(file ID, fork type, appended)`; its absence is
   `FSIMG_ECORRUPT`. Advance the leaf cursor from there: each record of the
   same file and fork type must start exactly at the appended count and
   contribute at least one block, and appending stops when the count reaches
   `totalBlocks`. The record after the last must belong to another file or
   fork, so an extra record is caught where it lies. Overshooting
   `totalBlocks` is `FSIMG_ECORRUPT`.
4. Charge one work unit per descriptor and the I/O of each node read.

Every record adds at least one block, so a walk appends at most
`totalBlocks` extents and reads at most that many records, whatever the
records claim. The resolver returns the number of overflow records it
consumed. Under verification it also appends the fork's runs to the
ownership claims, so no other code converts an extent into a claim.

The extents tree itself is resolved by step 2 alone; requiring its inline
sum to equal `totalBlocks` at open is what makes its own resolution
non-recursive.

## The B-tree reader

`btree.c` reads node `n < totalNodes` by resolving fork bytes
`[n * M, (n + 1) * M)` through the tree's fork index, in as many reads as the
node crosses extents, into one of the reader's node buffers, then validates
the descriptor and the offset table against the format design's
[node rules](design-hfsplus.md#nodes-and-records). Callers state the kind and
height they expect, and a mismatch is `FSIMG_ECORRUPT`.

The reader holds a fixed set of buffers of the largest node size among its
trees, charged at open: one for the catalog cursor that a directory scan
keeps, one for catalog searches made while that cursor is live, and one each
for the extents and attributes trees. A search reuses its tree's search
buffer at every level and leaves only the leaf in it. No node is retained
beyond those buffers: a later search rereads its path, so there is no
driver-private cache to keep coherent or to bound.

* **Search** finds the leaf record with the greatest key not above the search
  key, or reports that the key precedes every record. From the root it
  follows, at each index node, the greatest index key not above the search
  key, requiring each child to be below `totalNodes`, of the expected kind,
  and of height exactly one less, so a descent ends within `treeDepth`
  steps. Every catalog search uses the volume's comparator. A search for a
  thread `(id, "")` or a folder's first child lands on the thread unless a
  primary in that range has a name that folds to nothing, which the caller
  then refuses as an equal key, so even a mistaken fold table cannot make
  such a search return the wrong record unnoticed.
* **The leaf cursor** advances through a leaf's records and then along
  `fLink`, requiring each next node to be a leaf whose `bLink` names the node
  just left. Its caller checks key order, which is what ends a scan on a
  looping chain.
* **Level iteration**, used by verification alone, walks one level's sibling
  chain from its leftmost node, found by descending the first index record of
  each level.

## Catalog records and links

`hfsplus_record_validate()` checks a catalog record against the format
design's record and flag tables and maps it to a kind with the kind table.
`hfsplus_thread_lookup()` searches `(id, "")`, requires a thread whose type
matches the expected kind, and returns its parent and name.

`hfsplus_link_kind()` applies the format design's
[stub tests](design-hfsplus.md#file-hard-links) to a file record: a file
stub, a directory stub, or neither. The test compares the record's
`createDate` with two dates: the root folder's, which open already read,
and the private data folder's. The first record carrying either pair of
Finder codes looks the data folder up by one keyed search in the root and
keeps its node ID and date in the reader state; a record without the codes
costs nothing. On a volume without a data folder only the root's date can
match, and a record that matches it is a stub whose indirect node
`hfsplus_link_resolve()` then cannot find.

`hfsplus_link_resolve()` turns a file stub into its indirect node: the
stub's `special` must be nonzero and its forks empty; the indirect node is
found by a keyed search for `iNode` followed by the decimal reference in
the private data folder. It must be a file record that is not itself a
stub, of kind regular file or symbolic link. A missing private folder or
indirect node is `FSIMG_ECORRUPT`. A directory stub is an unsupported kind.

The record-to-entry mapping fills the kind, the size (the data fork's
`logicalSize`, or the compressed size), and the metadata: owner and mode
valid unless the BSD information is zero, the four times as UTC seconds,
and `FSIMG_META_EXTRA_DATA` from the attribute scan and the resource fork.

## Attributes and compression

`hfsplus_attr_scan()` visits the attribute records of one node ID: one
search for `(id, "", 0)` in the attributes tree and a cursor walk while
`fileID` matches. It validates each record, reports whether any
[stored attribute](design-hfsplus.md#stored-attributes-and-the-extra-data-mark)
exists, which excludes a consumed `com.apple.decmpfs` and the format's own
bookkeeping names, and returns the compression attribute's inline value
when present. The scan runs only for a
record with flag `0x0004` or `ownerFlags` `0x20`; an object with no
attributes costs nothing. A compression attribute held in a fork record,
rather than inline, is `FSIMG_EUNSUPPORTED` until observed.

`compress.c` owns the decode window: a 65536-byte output buffer and a
69632-byte input buffer, the format design's stored-chunk bound, plus, on
the first LZFSE file, the LZFSE decoder's scratch. They are charged once per
reader when the first compressed file is resolved and released at close.
zlib streams use the common
[allocation adapter](design-binary-primitives.md), so their state is charged
to the same context. A compressed entry's content state holds the type, the
uncompressed size, the resource fork's index for chunked types and the
chunk table's location; the table entries of chunk `i` and its successor are
read when chunk `i` is first needed, through a small window over the table,
so the table is never held whole, and the chunk is checked against them and
the fork. Resolving offset `o` decodes chunk `floor(o / 65536)` unless it is
the window's current chunk, charges the bytes consumed and produced as
work, and returns the decoded span. A codec error, a chunk that decodes to
the wrong length, or one stored beyond the input bound is `FSIMG_ECORRUPT`.
The decoders consume exactly the stored bytes and produce exactly the
expected bytes; they never grow output past the window. The encoded bytes
come from the resource fork through its fork index or from the attribute's
inline record, so every read of them is bounded by the content domain or
by a validated node buffer.

## Directories and names

`prepare` lists a directory by its node ID `d`:

1. Search for `(d, "")` and require the first record to be `d`'s folder
   thread, naming the parent the walker entered `d` from and the entry's
   stored name. Each folder has one thread, so a folder can be entered from
   one parent only: a second parent, a cycle back to an ancestor, or a
   folder record reusing another folder's ID fails here with
   `FSIMG_ECORRUPT`. The namespace is a tree by construction and needs no
   visited-folder set. The root's thread names parent 1 and the label.
2. Advance the cursor while the parent ID is `d`. Each record must be a file
   or folder primary, strictly ascending under the volume's comparator after
   the previous one; a thread or an equal or smaller key is
   `FSIMG_ECORRUPT`. Key order is what ends a scan along a corrupt sibling
   chain.
3. Validate each record and its name under the format design's name table.
   At the root, recognize the [format-owned children](design-hfsplus.md#private-directories-and-format-owned-files)
   by their exact stored units, validate them and count them, but do not
   admit them; record the private data folder's node ID when it is seen.
4. Classify every other child with `hfsplus_link_kind()` and the kind
   table: a directory stub, a socket or a device fails preparation with
   `FSIMG_EUNSUPPORTED`, so no child of the directory is exposed; a file
   stub is resolved now, and the indirect node's record takes the stub's
   place for everything but the name; a file with `ownerFlags` `0x20` has
   its compression attribute read and its type checked now, so an
   unsupported type fails here as well.
5. Count the children and require the count to equal the folder's
   `valence` and, with flag `0x0010`, the folders among them to equal its
   `folderCount`.

An admitted child carries its displayed name and a payload of at most the
common [64-byte bound](design-directories.md#prepared-directory), little
endian: the effective record's node ID (4), its leaf node number (4) and
record index (2), the stub's node ID or zero (4), the kind and the
extra-data bit (1 each), the compression type (1, zero for none), the size
(8), the owner and group (4 each), the permission bits (2), a flags byte
saying whether owner and mode are valid and whether a folder count is
recorded (1), the four times as stored (16), and for a folder its
`valence` and folder count (4 each), which its own preparation checks: 60
bytes. The effective record is the indirect node's for a stub, so that is
the record the leaf location names. `keys` emits one key, the
[namespace key](design-hfsplus.md#namespace-keys); `key` maps a query
component the same way, and one that is not valid UTF-8 or decomposes past
255 units is `FSIMG_EINVAL`. The ambiguity text names sibling names that are
equivalent under HFS Plus lookup, classified `FSIMG_ECORRUPT`.

`replay` re-validates the payload's ranges and fills the entry from it
alone, so listing a directory reads no node twice. It builds nothing: the
first `resolve` reads the effective record's leaf node again, requires the
record at the stored index to be a file record with the stored node ID,
and builds the fork index from it; for a compressed file it also scans the
node ID's attributes again for the compression attribute.

## Content

A regular file's or symbolic link's content is its data fork, resolved
through its fork index, built on first `resolve` and released when the
walker advances; a sequential read advances one extent cursor and batches
adjacent blocks, and a random offset costs one binary search. A compressed
file's content is decoded spans from the decode window. An indirect node's
fork is read for each stub that names it. `reader->initialized` is the
entry's size; HFS Plus has no uninitialized tail.

## Verification

Full verification establishes what the walk alone does not: that every tree
is well formed, that every catalog record is owned and paired, and that the
bitmap is exactly the storage the volume uses.

Before any work, verification refuses with `FSIMG_EUNSUPPORTED`, leaving
the report incomplete, a volume whose label is absent or whose private
directory folder is not empty, since the driver walks no directory
hard-link target. One keyed search for each private folder settles that
check and gives the catalog pass the data folder's node ID; verification
then runs these passes in order, each bounded. The first contradiction ends it with
`FSIMG_ECORRUPT`, and an unsupported feature found in any pass ends it with
`FSIMG_EUNSUPPORTED`, so a report is never complete after a skip:

1. **Tree structure.** For the extents, catalog and attributes trees in
   that order, level iteration visits every node of every level once. Each
   level's sibling chain is followed from its leftmost node; the index
   records of level `h`, read in order across the level, must name the
   nodes of level `h - 1` in their chain order, each paired with that node's
   first key, so the separator and child checks are a merge of two
   sequential streams. Keys ascend strictly across each level. A paged bitset
   of `totalNodes` bits marks every visited node, together with node 0 and
   the map chain's nodes; a node visited twice is `FSIMG_ECORRUPT`, and the
   set is then compared with the node map, which also yields `freeNodes`.
   `leafRecords`, `firstLeafNode`, `lastLeafNode` and `treeDepth` are checked
   against what the iteration counted.
2. **The catalog pass.** The catalog's leaf level, iterated in step 1, is
   the inventory: every record appears exactly once. For each primary
   record the pass appends `(node ID, type, parent, name)` to an object
   stream; resolves both forks of every file record through the fork
   resolver, which claims their runs and counts the overflow records they
   consume; counts files and folders, and separately the records whose
   parent is the private data folder, which must all be files; and appends
   `(reference, stub ID)` for each file stub to a link stream and
   `(reference, link count)` for each indirect node in the private data
   folder to an inode stream.
   Thread records appear in leaf order sorted by node ID, the order of
   their keys, and are kept as a second stream in that order.
3. **The attributes pass.** The attributes tree's leaf level yields records
   in node-ID order; the pass validates each, resolves fork records with
   their continuation records, claims their runs, and appends the node IDs
   that own records to an attribute stream.
4. **Special storage.** Claim the reserved blocks, the tail blocks, the
   bitmap, the three trees and the startup file, and the bad-block records.
   The journal and its info block are claimed by the catalog pass through the
   two journal files, whose forks the root's preparation compared with the
   info block.
5. **Merges.** Sort the object stream by node ID and merge it with the
   thread stream and the attribute stream in one sequential pass: every
   primary has exactly one thread whose type, parent and name match, every
   thread has its primary, node IDs are unique, every attribute owner is a
   primary, and `nextCatalogID` exceeds the largest ID unless bit 12 is set.
   Sort the link and inode streams by reference and merge them: every stub's
   reference names an indirect node, and every indirect node's link count
   equals its stubs. The overflow records consumed, plus the bad-block
   records, equal the extents tree's `leafRecords`, so no overflow record is
   orphaned. The counted files and folders equal the header's counts.
6. **Reachability.** The common walk runs once, its preparation checking
   each reachable folder's thread, children, `valence` and name order. The
   primaries it reaches, plus the format-owned root children and the
   private data folder's records that step 2 counted, must equal all the
   primaries step 2 counted, so a disconnected folder cycle, whose members
   all have parents, is `FSIMG_ECORRUPT`.
7. **Bitmap.** Seal the ownership claims, an instance of the
   [exclusive run utility](design-verification-helpers.md#exclusive-ownership-run-utility)
   over `[0, N)`, and compare them with the bitmap's first `N` bits in one
   sequential pass in windows: a claimed clear bit and an allocated
   unclaimed bit are `FSIMG_ECORRUPT`; the padding bits must be zero; the
   clear bits must equal `freeBlocks` and set `FSIMG_VOLUME_FREE`.
8. **Alternate header.** Compare the fields the format design names.

HFS Plus storage is exclusive: no two forks share a block, and a hard link
shares an indirect node through the catalog, not extents. So the strict run
utility fits without an exception. Every fork's claims come from one
funnel, the fork resolver, while the catalog and attributes passes reach
every record exactly once; only the reserved head and tail and the
bad-block records, which have no fork data, are claimed directly in step
4. A cross-linked fork fails in step 7 whichever objects own it.

Payload read-through, when requested, reads each delivered entry's content
through common delivery during step 6, decoding compressed files
completely. An indirect node is read once for each stub that names it, as
extraction reads it, so no set of nodes already read is kept, and its bytes
count once per stub.

A wrapped volume is verified as its embedded volume, under the format
design's [wrapper rule](design-hfsplus.md#the-hfs-wrapper), and its report
is complete when every pass succeeds.
The report counts files and directories the walk delivered, the root
included, logical bytes, bytes read, allocated and free blocks, and the
bad blocks as `bad_clusters`.

## Writer settings

`validate` applies the format design's [settings table](design-hfsplus.md#settings-and-sizing):
`allocation_unit_bytes` zero or a power of two from 512 to 65536;
`sector_bytes` zero or 512; `volume_bytes` zero or a multiple of 512 whose
block count is at most `2^32 - 1`; `label` NULL, empty or at most 255 units
after encoding; `epoch` within 1904 to 2040 in reproducible mode;
`volume_uuid` any value. `volume_serial` is ignored, as the format stores no
32-bit serial. The capability record's `write_settings` names
`FSIMG_SETTING_VOLUME_UUID` alone, so the CLI generates a random identifier
unless `-R` is given.

## Planning

The plan object embeds `fsimg_plan_t` and holds the variant, the geometry,
the tree sizes and node counts, the first block of every metadata file, the
allocated count `A`, the format time, one paged array of rows keyed by
catalog ID with row 0 for the root, and the sorted catalog record stream with
the index-level streams the packer derived from it. A row holds the size, the
data fork's first block and block count, the owner, mode and four times
already selected under the metadata policy, the kind, the `valence` and the
folder count; the node ID is `15 + id`, or 2 for the root, and is not
stored. Rows and streams are the only per-entry state; nothing grows with
`N` but the bitmap the emission streams.

Planning runs a fixed sequence of passes over the frozen catalog:

1. **Entries.** For each catalog ID in order: encode the name as the format
   design's [writer names](design-hfsplus.md#writer-names) rule states, or fail
   with `FSIMG_EINVAL`; refuse the reserved root names; check kind and
   metadata through the common policy, selecting owner, mode and times; count
   the entry into its parent's row; accumulate the data fork's blocks for
   every candidate block size, for the sizing search; and append two records
   to the catalog record stream, the primary and the thread. A record is the
   sort key, the parent node ID and the namespace key as big-endian bytes,
   then the catalog ID, the record type and the stored name.
2. **Sort.** Sort the stream externally by sort key. Adjacent equal keys are
   siblings that macOS would treat as one name: planning fails with
   `FSIMG_EINVAL` naming the second. A thread's key is its node ID and the
   empty name, so threads never collide.
3. **Packing.** Run the packer over the sorted stream: leaves in order,
   recording each leaf's first key in the level-1 stream, then each level
   from the previous one, until one node remains. Node counts give the
   catalog's used nodes, and map nodes come from a fixed-point step on
   `totalNodes`. The extents tree, and the attributes tree when the native
   formatter makes one, is its header node. The node sizes are fixed, so
   the node counts depend on the catalog alone and packing runs once; each
   candidate block size only rounds the three forks up to whole allocation
   blocks, a constant-time step.
4. **Geometry.** For the block size, the explicit one or each candidate
   under minimization, find `N`: with an exact `volume_bytes`, `N =
   floor(V / B)` must hold the plan or `FSIMG_ERANGE`; otherwise start at
   the demand without the bitmap and raise `N` to `r + bitmap(N) + demand +
   t` until it holds. Minimization keeps the smallest volume, ties to the
   default order.
5. **Layout.** Assign first blocks in the fixed order of the format design's
   [layout](design-hfsplus.md#layout): the bitmap at `r`, the extents tree,
   the catalog tree, the attributes tree, then each file's and symbolic
   link's data fork in catalog ID order. The last block used is `A - 1`.
6. **Summary.** Fill `fsimg_plan_info_t`: volume, metadata, allocated, free
   and initialized bytes, the 512-byte sector, the block size and `N` as
   `allocation_units`. The common
   [writer loss policy](design-metadata-loss.md) installs the loss count
   after every planning step succeeds.

Every value in the plan derives from the catalog, the options, the pinned
tables and these rules, so byte-identical output does not depend on source
order, budgets, sort fan-in, scratch placement, locale or time zone.

## Emission

The common emission object has already zeroed the sink. The driver emits:

1. For each row in ID order, the payload copy of a file or symbolic link
   into its one region `(first * B, blocks * B)`.
2. The catalog tree: the packer runs again over the sorted stream and the
   level streams, now encoding each node into the transfer buffer: leaf
   records from the stream record and its row, which supplies the node ID,
   the parent, the metadata, the fork extents and the folder counts; index
   records from the level streams; sibling links from the node numbering;
   then the header node with its map and any map nodes. Each node is one
   metadata write at its fork offset.
3. The extents tree's and attributes tree's header nodes.
4. The bitmap: ones for blocks `0` to `A - 1` and for the tail, zeros
   elsewhere and in the padding, written in whole blocks.
5. The alternate header, then the primary header, identical.

Planning and emission run the same packer code, so a node boundary
emission computes cannot differ from the one planning sized. Every emitted
byte comes from a row, a stream record, the geometry or a profile constant.

## Resource bounds

| Operation | Work and residency |
| --- | --- |
| Probe and open | One 1536-byte read; three header nodes; the extents tree's inline index; a few search paths for the special forks' overflow and the root; the journal records |
| Listing | Per directory: one search and one cursor scan of its records, its thread validated, the common key sort; per hard-link stub, one or two keyed searches; per flagged file, one attribute scan. No payload is read |
| Content | Per entry, one leaf reread and one resolver walk on first resolve, then one index lookup per request; per compressed chunk, one table read and one decode |
| Verification | Every node of every tree read once by level iteration plus once by its lower level's merge; the catalog and attributes passes; two external sorts and their merges; one bitmap pass of `ceil(N / 8)` bytes; scratch for the streams, the node bitsets and the ownership runs |
| Planning | One pass over the catalog, one external sort of two records per entry, one packing pass per tree level, the raise-until-fit search |
| Emission | Payload bytes plus one pass over the sorted records and level streams, and the bitmap |

Resident per reader: the geometry, the header, the tree descriptors, the
node buffers, the current entry's record and extent cursor, and the decode
window when a compressed file has been read, beside the common walk's
charged stack and path buffer. Everything else is charged scratch. Every
loop charges the session and observes cancellation.

## Failure classification

Reader:

| Condition | Status |
| --- | --- |
| No identity signature; slice shorter than 1536 bytes; a forced variant that contradicts the signature | `FSIMG_EFORMAT` |
| Signature-version pairing; volume bits outside the table, 29 or 30; tree parameters outside the table; catalog flags outside the table or `0x1000`; sockets, devices and directory hard links; names with `:`, NUL, `.` or `..`; link targets above 1023 bytes or with NUL; compression types outside the table, inline sizes above 65536, compression attributes in fork records; a journal with transactions or on another device; an unobserved attribute record type | `FSIMG_EUNSUPPORTED` |
| Geometry, header, fork, extent, overflow, node framing, key, order, separator, sibling, map, record, thread, kind, name encoding, count, link, attribute, compression, journal, wrapper or bitmap inconsistency | `FSIMG_ECORRUPT` |
| Unrepresentable selection component | `FSIMG_EINVAL` |
| Budget, scratch, work or descriptor exhaustion | `FSIMG_ELIMIT` |

Writer:

| Condition | Status |
| --- | --- |
| Setting outside its rule; a name or label outside 1 to 255 units after encoding; equivalent siblings; a reserved root name | `FSIMG_EINVAL` |
| A file needing more than `2^32 - 1` blocks; a kind the profile lacks or a link target above 1023 bytes, from the common capability check; a time loss when the caller forbids losses | `FSIMG_EUNSUPPORTED` |
| Content that does not fit the exact size or `2^32 - 1` blocks; a non-reproducible format time outside 1904 to 2040 | `FSIMG_ERANGE` |
| Budget, scratch, work or descriptor exhaustion | `FSIMG_ELIMIT` |

Diagnostics quote at most one bounded, escaped name and never node bytes,
attribute values, decoded content or buffer contents.

## Tests

`test/hfsplus.c` owns a synthetic fixture generator written from the format
design with its own offset tables and its own simple B-tree builder; it
shares no code with `lib/formats/hfsplus/`. It builds volumes in memory:
headers, bitmaps, extents, catalog and attributes trees of chosen node sizes
and depths, fragmented forks with overflow records, hard links, private
folders, compressed files of every type with its own small encoders where a
codec allows a stored form, journals and wrappers, and mutates them for the
format design's [acceptance matrix](design-hfsplus.md#acceptance-matrix).
Every valid fixture has a paired mutation that must fail with its class.
Reading tests compare `info`, `t`, `x -O` and `x` on the fixtures and on
every native capture found through `FSIMG_HFSPLUS_NATIVE`, using the
manifest of names, sizes, modes, times and checksums each capture records.

`test/hfsplus-unicode.c` checks the generated tables: the digest of each
table's serialization equals the digest the research record pins from the
native observation, and spot vectors cover each range rule. The macOS
integration script repeats the observation on a fresh native volume and
compares every row, so a host change is noticed.

`test/hfsplus-write.c` creates images through the public API and reads them
back through its own independent checker, written from the format design
like the generator, as well as through the reader and verifier. It compares
bytes across source order, metadata cache size, scratch placement and time
zone, scans every unused byte for poison, and checks the packer at node
boundaries: records that exactly fill a node, one byte more, a non-root
level whose last node holds one record, which native qualification also
checks, and a catalog wide enough for map nodes.
Fault tests inject allocation, scratch, short-read, short-write, EOF,
source-change and cancellation failures at every stage and check cleanup.

`test/hfsplus-integration.sh.in` runs the independent oracles the build
finds and skips the others explicitly: on macOS, `newfs_hfs` and `hdiutil`
captures read by fsimg, and fsimg output attached with `hdiutil`, checked
with `fsck_hfs -n`, mounted, modified, unmounted, checked again and read
back; on Linux, `fsck.hfsplus -n`, `hfsdump` from the pinned hfsfuse build
and the Sleuth Kit's `fls` and `icat` on fsimg output, and fsimg on
`mkfs.hfsplus` volumes. `fuzz/hfsplus.c` feeds the probe, open, traversal,
content, decompression and verification paths under finite budgets with
seeds generated at run time; `fuzz/lzfse.c` feeds the imported decoders
through their wrapper. Sanitizer runs follow the build documentation.
