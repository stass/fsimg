# UDF driver

## Scope

The [UDF format design](design-udf.md) establishes the on-disk facts, the
closed acceptance tables and the writer profile. This document maps them onto
code: the modules of `lib/formats/udf/`, the driver state behind the common
reader and writer, the algorithms that keep translation, descriptor walking,
ownership, planning and emission bounded, and the tests that qualify them.

The driver implements the common contracts:
[format descriptor and probe](design-formats.md#format-descriptors-and-profiles),
[prepared directories](design-directories.md),
[logical content](design-content.md), [emission](design-emission.md),
[verification helpers](design-verification-helpers.md) and the
[POSIX metadata extension](design-metadata.md). Common code gains no
UDF-specific branch. The additions UDF needs are format-neutral and live in
their owning contracts: zero extents, decoded link targets and the
storage-byte count in [logical content](design-content.md) and the
[verification helpers](design-verification-helpers.md), the link-target read
in [emission](design-emission.md#encoded-link-targets), the 255-byte label
in the [C API](design-api.md#reader), and the format-neutral name of the
[portable name profile](design-directories.md#omitted-names). The QNX4
driver is the structural template; the two share no code.

## Modules

| File | Owns |
| --- | --- |
| `udf.h` | Private types, constants and prototypes shared by the files below |
| `descriptor.c` | `fsimg_udf_format`: the `udf` name, its capability record, the probe and the reader and writer operations |
| `codec.c` | Tag checksum and CRC-16; decode and encode of `extent_ad`, `lb_addr`, `short_ad`, `long_ad`, `regid`, `charspec` and timestamps; every descriptor's fixed fields; pure functions without I/O |
| `cs0.c` | CS0 decoding to WTF-8, CS0 encoding, dstrings, the name classification of the format design's profile table |
| `volume.c` | Probe recognition, block-size selection, anchors, sequence traversal and the prevailing set, the LVID, the file set: the volume model |
| `map.c` | The translators: Type-1, metadata, sparable and virtual maps, their construction at open and run translation |
| `icb.c` | FE and EFE decoding, ICB-tag rules, the extended-attribute area, the record-to-entry mapping and metadata conversion |
| `alloc.c` | The descriptor-list walker: descriptors, AED chains, extent-type mapping, content-index construction, ownership claims |
| `space.c` | Claimed-block bitsets per ownership domain, the exposure bitset, the space-bitmap pager and unallocated-space-entry reconciliation |
| `directory.c` | `prepare`, `replay`, `keys` and `key`; FID decoding over a body cursor |
| `symlink.c` | Path-component decoding and encoding |
| `read.c` | `fsimg_udf_reader_ops`: open, close, resolve and volume information |
| `verify.c` | Full verification: anchors and the reserve sequence, ownership against space management, link counts, unique IDs, integrity, streams, mirrors and sparing copies |
| `write-udf.h` | Plan rows and the plan object |
| `write-plan.c` | `validate`, `plan`, `free`: settings, the entry pass, forms, sizing and layout |
| `write-emit.c` | `emit`: payload copies, EFEs, AEDs, directory and link bodies, space bitmap, file set, sequences, integrity, anchors, recognition |

Three single owners carry the format's hard parts, so each check exists once:

* `map.c` is the only code that turns a `(map index, block)` into a volume
  block. Every descriptor read, list walk and content extent goes through
  it, so range, overflow, clipping and tag-location space are enforced for
  every map type in one place.
* `alloc.c` is the only code that interprets an allocation descriptor or an
  AED. Directories, files, links, streams, the metadata file, the VAT, the
  unallocated space entry and the writer's own descriptor encoding share its
  rules.
* `space.c` is the only code that records ownership. Every claim names its
  domain, so physical and metadata ownership never mix.

## The volume model

`open` builds one immutable `udf_volume_t`; nothing after `open` changes it:

| Field | Meaning |
| --- | --- |
| `block_bytes`, `blocks` | `B` and `N` |
| `nsr`, `revision` | `2` or `3`; the domain revision |
| `profile` | physical, metadata, sparable or virtual |
| `partition` | the prevailing PD's number, access type, start, length and header |
| `maps[2]`, `map_count` | each map's kind, index and translator state |
| `content_domain` | the image range file storage can occupy |
| `fsd`, `root` | the file set's `long_ad` and the root ICB address |
| `integrity` | the prevailing LVID's type, tables, counts, revisions and next unique ID |
| `label`, `label_valid` | the decoded label and whether `info` reports it |
| `volume_set_serial` | the 64-bit detail, when the identifier's first 16 characters are hexadecimal |
| `anchor_main`, `anchor_reserve` | the extents of the opening anchor |

Translator state is constant size except where a map needs a table, which
lives in scratch: a sparing table's sorted entries, the VAT's and the
metadata file's extent indexes.

## Codecs

The shared little-endian [scalar codecs](design-binary-primitives.md) decode
and encode integers in byte buffers; the UDF codecs check each record's span
before any field is read. Decoded records are C structures with `_t`
typedefs holding only validated values: `udf_tag_t`, `udf_extent_ad_t`,
`udf_lb_addr_t`, `udf_ad_t` (type, length, map index, block, unique ID),
`udf_regid_t`, `udf_timestamp_t`, `udf_pvd_t`, `udf_pd_t`, `udf_lvd_t`
(with up to two decoded maps), `udf_lvid_t`, `udf_fsd_t`, `udf_icb_t` (the
fields of an FE or EFE in one form, with the descriptor kind) and `udf_fid_t`.

`udf_tag_check()` takes a block and the expected identifier and location and
returns the descriptor's CRC-covered length; every caller then derives the
descriptor's true size from its own fields. `udf_tag_seal()` is its inverse
for the writer: given the encoded body it computes the CRC over a CRC length
the caller states and then the checksum. `udf_crc16_update()` exists for the
streamed cases, an FID body read in windows and an inline file whose bytes
pass through the copy observer. The CRC is unit-tested against vectors
computed independently of the codec, including the observed images' tags.

Encoding is the exact inverse of decoding and fills every reserved byte with
zero, so an emitted descriptor never contains an unspecified byte.

## Probe and open

The probe inspects 131072 bytes, enough for 48 recognition slots at 2048
bytes and three at 32768. It tries slot strides 2048, 4096, 8192, 16384 and
32768 in that order and scans each as the format design's recognition rule
states; the first stride that yields a sequence decides. A wrong stride
fails on an ordinary layout: at stride 2048 a 4096-byte layout shows a zero
slot at 34816, and at stride 4096 a 2048-byte layout skips its `NSR` slot;
where a layout could satisfy two strides, the earlier one wins and `open`'s
anchor check at block 256 rejects a wrong choice. At strides of 8192 and
more the window holds 12, 6 and 3 slots, enough for a standalone sequence
but not for a bridge's leading descriptors, so such a bridge is not
recognized. Without a sequence the
probe makes no claim (`FSIMG_EFORMAT`); with one at stride 8192 or more it
claims `FSIMG_EUNSUPPORTED`; otherwise it claims the image with `FSIMG_OK`
and records nothing, since `open` derives the candidate block sizes from
the stride again. The probe performs no I/O and allocates nothing.
Exclusivity holds in both directions: FAT and exFAT carry a jump and BPB at
byte 0, QNX4 its root record at 512, QNX6 its superblock at 8192 and ImageFS
its header at 0, and none places `BEA01`, `NSR` and `TEA01` at consecutive
recognition slots; a UDF volume's first 32 KiB carry none of their
signatures unless a writer put a boot record there, such as mkudffs's
whole-disk MBR, whose treatment the layer design decides (research question
Q16).

`open` completes recognition in a fixed order; each step fails before the
next reads:

1. **Geometry.** The candidates are 512, 1024 and 2048 for stride 2048 and
   4096 for stride 4096. For each candidate `B` whose whole blocks number at
   least 257, read block 256 and test its identity: a tag with identifier 2
   and a valid checksum. Exactly one candidate must carry it, and two are
   `FSIMG_ECORRUPT` as contradictory geometries. The block that carries it
   is the opening anchor, so any further fault in it, its CRC, location or
   extents, is `FSIMG_ECORRUPT`. When no candidate carries it, the same
   candidates are tried at `N - 1` and `N - 257`: a valid anchor there is a
   tail-anchored volume, `FSIMG_EUNSUPPORTED`, and none anywhere is
   `FSIMG_ECORRUPT`. `N` is the slice length over `B`, rounded down.
2. **Main sequence.** Traverse it as the format design states, keeping per
   kind only the prevailing descriptor's decoded form; a descriptor with an
   equal sequence number is compared with it field by field and dropped.
   The PVD, LVD and at least one PD are required; the IUVD and USD are
   validated when present.
3. **Logical volume.** Check the LVD's block size, domain, revision against
   the recognition sequence, map table and map set, and select the profile.
   Check the PD each map names, its access type for the profile, its range
   and its disjointness from the anchor at 256, both sequence extents and
   the integrity extent.
4. **Maps.** Build the translators in dependency order (below). The content
   domain is the partition's byte range, widened on a sparable volume to
   cover the spare packets and clipped on a write-once volume to the slice.
5. **Integrity.** Traverse the integrity sequence and keep the prevailing
   LVID.
6. **Claims.** Create the claimed-block bitset of each ownership domain in
   scratch and claim at open what no directory walk reaches: the space
   bitmap's or unallocated space entry's blocks, the file set sequence's
   blocks, and the metadata file's, bitmap file's and VAT's ICBs, AEDs and
   bodies. The mirror's ICB and AEDs are claimed. Its body's coalesced block
   runs are compared with the metadata file's: disjoint under the
   duplicated flag, the body is claimed; equal without it, it is not
   claimed again; any other relation is classified as the format design
   states. A mirror reaching the walk therefore never collides with its
   file.
7. **File set.** Traverse the file set sequence, keep the prevailing FSD,
   and decode the root ICB: an FE or EFE of file type 4. The root's storage
   is claimed later by `prepare`, exactly like every other directory, so no
   directory is claimed twice and none is left unowned.
8. **Label.** Decode the label as the format design states.

`info` then reports the volume bytes `N * B`, `B` as sector and allocation
unit, the partition length as allocation units, the label when valid, the
clean state from the integrity type, `FSIMG_TIME_PER_TIMESTAMP`, the profile
string (`UDF 2.01 physical`, `UDF 2.50 metadata`, and so on), and these
details in this order: `revision`, `min_read_revision`,
`min_write_revision`, `max_write_revision`, `access_type`,
`partition_start`, `partition_blocks`, `lvid_files`, `lvid_directories`,
`next_unique_id`, `volume_set_serial` when present. `FSIMG_VOLUME_FREE` is
set only by a completed verification.

## Translation

A translator turns `(block, count)` in its map's space into one volume run
`(volume block, run)` with `run <= count`, clipped at the first
discontinuity; callers loop. It checks the block against the map's length,
the run's end by checked arithmetic, and the result against the volume; a
write-once partition's declared length can exceed the volume, so for it the
volume check is the one that bounds the run. `udf_map_read()` reads one
block through a translator into a window and checks its tag against the
address it was asked for, in the map's own space; the translated volume
block is used only for the read, so a tag can never be validated against
the wrong space.

| Map | Construction | Translation |
| --- | --- | --- |
| Type 1 | none | `start + block`, one run to the partition's end |
| Sparable | read the first table copy whose tag validates; check its rules; store its used entries sorted by original in a scratch array | binary search for the packet; mapped or `start + block`, clipped at the packet's end |
| Virtual | read the VAT ICB at `N - 1` through the physical map, walk its list into an index over the physical partition, decode the header or trailer by revision | read entry `block` through the index; one block per run |
| Metadata | decode the metadata file's ICB through the physical map, walk its list into an index; the mirror's and bitmap file's likewise | index lookup; a run ends where the extent does; a zero extent is a hole, `FSIMG_ECORRUPT` |

Construction reads only through maps already built: the physical map first,
then the one Type-2 map, whose backing file is decoded through the physical
map. No translator consults itself, so the bootstrap is acyclic by
construction rather than by a check. VAT entries stay on disk: a lookup
reads one entry through a retained 4 KiB window, so a million-entry VAT
costs its index, not its table, in memory.

## The descriptor-list walker

`udf_list_walk()` turns an ICB into the logical extents of its body:

1. Validate the ICB's allocation form and `L_AD` against the format design.
   An inline body yields one extent at the ICB's own volume bytes after the
   extended attributes, never through the index, since it is not block
   aligned.
2. Read descriptors in order from the ICB, then from each AED: before
   reading an AED, translate and read its block in the continuation's map,
   check its tag and length, and apply the walk policy to it. A zero-length
   descriptor ends the list, with the rest of the area zero; a continuation
   ends the area that holds it; an AED without a descriptor of nonzero
   length other than its continuation is `FSIMG_EUNSUPPORTED` and ends the
   walk. The AED count is checked against
   `ceil(information length / B) + 1`, a bound that holds without any
   claim, so even a walk under the shared policy cannot loop.
3. For a descriptor of type 0, translate its blocks run by run and emit a
   data extent per run; for types 1 and 2 emit one zero extent; for types 0
   and 1 apply the walk policy to the storage. Check that every descriptor
   but the last is whole blocks, and after the list that the lengths sum to
   the information length.
4. Charge one work unit per descriptor and the I/O of each AED.

The policy is one of two:

| Policy | Used for | Rule |
| --- | --- | --- |
| Exclusive | every directory as it is entered, the first name of every other object, the open-time objects, and under verification each stream directory and stream, under its owner's first name | each block is claimed in its domain |
| Shared | a later name of a non-directory | no claim; the first name owned the storage |

A claim of a block already claimed is `FSIMG_ECORRUPT` whichever object
came first, so a cross-link, an AED cycle, a directory reached twice and a
descriptor pointing into the space bitmap all fail by one rule. A walk's
cost is bounded by its descriptor count and the AED bound; an AED chain
that returns to a block fails its claim on the object's exclusive walk, and
a shared walk follows a chain an exclusive walk of the same ICB already
accepted. No other walk exists: a symbolic link's body is decoded only at
replay, inside its first name's exclusive walk or a later name's shared one.

Content uses the shared [extent index](design-content.md#extent-index): the
walker appends data extents as byte ranges in volume bytes and zero extents
by length, and the index coalesces and checks them against the content
domain. A body's final extent, data or zero, is appended rounded up to
whole blocks, as the index requires; delivery clips to the logical length.

## Ownership domains and exposure

`space.c` keeps one claimed-block bitset per ownership domain:

| Domain | Units | Holds |
| --- | --- | --- |
| Physical partition | the partition's logical blocks, before sparing | every object of a physical, sparable or virtual volume, and the metadata file's, mirror's and bitmap file's storage |
| Metadata partition | metadata blocks | every object addressed through the metadata map |

A virtual block claims the physical block its VAT entry names, so two
virtual blocks naming one physical block are a cross-link. A sparable claim
is in logical blocks, since sparing relocates packets one for one and the
space bitmap counts logical blocks.

A non-directory may have several names, and only its first may claim its
storage, so the walker must tell a later name, which is legitimate, from a
second object claiming the same blocks, which is a cross-link. The exposure
bitset does that: one bit per ICB block of each domain, set when the
object's first name is replayed. A name whose bit is set walks under the
shared policy; a name whose bit is clear walks under the exclusive one and
sets it. A directory needs no bit, since entering it claims its ICB block
and a second name then fails that claim.

## Directories and names

`prepare` locates the directory: the root from the volume model at level 0,
otherwise the current entry's locator. It walks the directory's list under
the exclusive policy, which claims its ICB, AEDs and body, then reads its
FIDs through a cursor over the body in 4 KiB windows. A FID may cross a
window, block or extent boundary; the cursor assembles the 38-byte header
and the name and streams the CRC over the implementation use and padding,
so no buffer depends on `L_IU`.

For each FID the cursor checks the tag with the location of its first byte,
the version, characteristics, lengths and padding, and skips a deleted FID
after its framing. The first live FID must be the parent FID naming the
directory's parent, which is the directory itself for the root. For every
other live FID, `prepare`:

1. decodes the name and classes it: delivered, omitted, or refused with
   `FSIMG_EUNSUPPORTED` for NUL, slash, `.` and `..`;
2. reads the target ICB through its map and checks its file type: an
   unsupported kind fails the preparation with `FSIMG_EUNSUPPORTED` naming
   the entry, a type that may not be named fails with `FSIMG_ECORRUPT`, and
   the FID's directory bit must agree;
3. counts a subdirectory for the link-count check;
4. admits the child with its component and a 32-byte payload: the target's
   map index and block, its file type, the FID's hidden bit, its unique ID,
   and the directory's own map index and block.

A symbolic link's target is not decoded here, since decoding walks its body
and walks happen at replay; a target fsimg cannot spell fails at replay with
`FSIMG_EUNSUPPORTED` naming the entry.

Every FID is validated before any child is admitted. When the body ends,
the directory's link count must be `1 + subdirectories` under verification.
`keys` emits one key, the component's exact bytes; `key` checks a query
component against the profile and returns its bytes. The `ambiguous` text
names a duplicate UDF file identifier.

`replay` re-validates the payload, reads the ICB again and fills the entry:
kind, size, owner, mode, the four times and the attributes. For a regular
file it sets `reader->initialized` to the information length, since zero
extents, not a tail, carry UDF's unrecorded bytes. For the first name of a
non-directory, marked in the exposure bitset, it walks the list under the
exclusive policy into the content index. A later name of a regular file
builds its index on its first `resolve` under the shared policy. A symbolic
link's body is walked at replay under the policy its name calls for, and
its target is decoded from it into the reader's
[decoded-target buffer](design-content.md#decoded-link-targets); its size is
the target's length. A FIFO has size 0 and is never delivered from.

## Content

`resolve` answers from the current object: a lookup in its index, which
returns a data or zero extent, or the inline extent. Sequential reads
advance the index's cached extent and batch adjacent blocks; a random offset
costs one binary search. The index is released when the walker advances. A
list is walked at most once per name, and no content read rereads its
directory.

## Verification

Full verification runs the common walker once with omitted subtrees
descended, then reconciles what the walk established. Before any walk it
checks what open did not:

* **Anchors.** Examine 256, `N - 257` and `N - 1`. Outside a partition, a
  position whose first 16 bytes form a tag with identifier 2 and a valid
  checksum must be a valid anchor naming the same extents as the opening
  one, and any other content is not an anchor. Inside a partition only a
  block that passes every anchor check, CRC and location included, is an
  anchor, since file content may happen to carry an anchor-shaped tag; such
  an anchor is claimed there before the walk, so a file covering it is a
  cross-link and the space bitmap must mark it allocated. At least two
  anchors are valid, one on a virtual volume.
* **Reserve sequence.** Traverse it like the main one and compare the
  prevailing set field by field on the fields the reader consumes: the LVD's
  block size, domain, label, file set location, integrity extent and maps,
  each PD's number, access type, start, length and header, and the PVD's
  volume sequence fields. Unconsumed fields may differ, as G1's volume set
  identifiers do.
* **Volume space.** The USD's extents, the sparing tables and spare packets
  are disjoint from every partition and structure.
* **Scope.** A freed-space descriptor makes verification incomplete with
  `FSIMG_EUNSUPPORTED` before any work; an invalid label fails it with
  `FSIMG_ECORRUPT`.

The walk then claims, under the exclusive policy, every directory, every
object under its first name, the FSD's system stream directory and its
streams at the start, and each EFE's stream directory and streams under the
object's first name, and for each FID records:

* **Link counts.** A name appends the target's ICB address to a stream, and
  a first replay appends the address with the ICB's link count; the stream
  is sorted externally and merged, so each non-directory's names equal its
  count. Directories were checked at the end of their own body.
* **Unique IDs.** For a target of link count 1, the FID's unique ID equals
  its target's low 32 bits on NSR03 volumes and equals it or is zero on
  NSR02 volumes; a hard-linked target's FIDs are not compared. A running
  maximum of the targets' unique IDs feeds the LVID check.

After the walk:

* **Space.** With a space bitmap, one sequential pass compares the claimed
  set with the bitmap in 4 KiB windows: a claimed block marked free and an
  allocated block not claimed are `FSIMG_ECORRUPT`, and the free bits are
  the free count. With an unallocated space entry, its free extents are
  claimed in a second bitset and the two sets must partition the domain.
  Without either, nothing more is checked and the free count is zero. A
  metadata partition is reconciled the same way against its bitmap file.
* **Integrity.** On a closed LVID: the free and size tables, when not
  `0xffffffff`, equal the free count and the partition length; the file and
  directory counts equal the walk's distinct non-directories and directories
  with the root, streams excluded; the next unique ID exceeds the maximum.
  An open LVID skips these.
* **Copies.** A duplicated metadata mirror's body equals the metadata
  file's block for block; an unflagged mirror shares the file's extents,
  which open checked, so its content is the file's by construction. Every
  sparing table copy validates and equals the one in use.

Payload read-through, when requested, reads each non-directory's
initialized bytes under its first name through the
[common helper](design-verification-helpers.md#read-and-failure-algorithm),
which counts only bytes read from storage; a symbolic link's target was
read and checked when it was decoded and is not read again. The report
counts files, directories including the root, logical bytes, bytes read,
allocated and free blocks and omitted subtree roots; bad blocks are zero.

## Writer settings

`validate` accepts the common options with these rules and resolves the
defaults into them, as the common writer expects of every driver:
`sector_bytes` zero, which it sets to 2048, or 512, 1024, 2048 or 4096;
`allocation_unit_bytes` zero or equal to the sector size; `fat_root_entries`
zero, which the common settings check enforces; `label` NULL, which it sets
to the empty string, or a string encoding to at most 126 bytes after the
compression ID under the name encoding, with no character from U+0001 to
U+001F and no U+007F, the empty string storing empty identifiers;
`volume_bytes` zero or a multiple of the block size in
`[277 * B, 2^32 * B]`; the epoch representable as a UDF timestamp, years 1
to 9999. `volume_serial` is used verbatim. The capability record's
`write_settings` is zero.

## Planning

The plan object embeds `fsimg_plan_t`, the geometry (`B`, `N`, `L`, `S`,
`A`), the first block of the file-body region, the format time, the encoded
dstrings of the label for the LVD, IUVD, PVD and FSD, the volume set
identifier, the non-directory and directory counts, and one paged array of
128-byte rows keyed by catalog ID with row 0 for the root. A row holds:

| Field | Meaning |
| --- | --- |
| kind, form | entry kind; none, inline or blocks |
| body length | the information length: a file's size, a directory's FID bytes, a link's component bytes |
| EFE block | its partition block |
| AED first block, AED count | the chain after the EFE |
| body first block, body blocks | in the first region for a directory or link, the second for a file |
| subdirectories | for the link count |
| encoded metadata | owner, group, permissions, ICB flag bits, the four timestamps and the FID's hidden bit, already converted under the metadata policy |

Unique IDs derive from catalog IDs (`0` for the root, `15 + id` otherwise,
so the LVID's next unique ID is `16 +` the largest catalog ID) and FID
offsets from the child stream, so a row stores neither and no row names
another. Rows are the only per-entry state; no vector grows with `N`.

Planning runs a fixed sequence of passes over the frozen catalog:

1. **Entries.** For each catalog ID in order: check the component's
   encoding length and refuse characters U+0001 to U+001F and U+007F with
   `FSIMG_EINVAL`; add its FID record, `align4(38 + L_FI)` bytes, to the
   parent row's body length and a directory to the parent's subdirectories;
   for a symbolic link, read its target through the
   [link-target read](design-emission.md#encoded-link-targets), encode its
   components, refuse an unrepresentable target and record the encoded
   length; validate the metadata under the
   [metadata policy](design-metadata.md#writer-policy) with the writer
   profile's limits, storing the encoded fields and counting each optional
   loss; count non-directories and directories. A directory's body starts
   with its 40-byte parent FID.
2. **Forms.** For each row: inline when the body length is at most
   `B - 216`, otherwise blocks with `ceil(length / B)` blocks and the
   descriptor and AED counts of the format design. Accumulate `M`, the first
   region's blocks, and `D`, the file bodies' blocks.
3. **Geometry.** The demand is `1 + M + D + ceil(min_free_bytes / B)`
   blocks after the space bitmap. With exact `volume_bytes`, `L = N - 274`
   must hold `S(L)` plus the demand, else `FSIMG_ERANGE`. With automatic
   sizing start at `L` equal to the demand plus one and, while
   `S(L) + demand > L`, set `L = S(L) + demand`; `S` grows by one block per
   `8 * B` blocks, so the loop ends within a few steps at the smallest
   fitting `L`, and `N = L + 274`. `N` above `2^32` is `FSIMG_ERANGE`.
4. **Layout.** Assign partition blocks in the fixed order: the space bitmap,
   the FSD, then each row's EFE, AEDs and first-region body in catalog ID
   order, then each file's body in catalog ID order. The first block left
   free is `A`, and `A <= L` by construction.
5. **Summary.** Fill `fsimg_plan_info_t`: volume, metadata, allocated, free
   and initialized bytes, and the block size. The common
   [writer loss policy](design-metadata-loss.md) installs the loss count
   after every planning step succeeds.

## Emission

The common emission object has already zeroed the sink. The driver emits:

1. For each regular file in catalog ID order: the payload copy into its
   body blocks, or into its EFE's inline area with an observer that streams
   the CRC; then its EFE and AEDs. The observer advances the CRC over the
   zero bytes between the batches it sees and after the last one, since the
   copy reports no zero ranges, so an inline file of zeros has the right
   CRC.
2. For each symbolic link and FIFO in catalog ID order: its EFE, with a
   link's target read through the link-target read and encoded into the
   inline area or its body block.
3. Directory bodies, by one pass over the directory rows in catalog ID order
   merged with the catalog's child stream, which groups children by parent
   in ID order: the parent FID, then each child's FID, written block by
   block from the emission buffer into the body, or gathered into the EFE's
   inline area; then the directory's EFE and AEDs. Each FID's tag location
   is the block its first byte falls in.
4. The space bitmap and the FSD.
5. Both volume descriptor sequences, the integrity sequence, both anchors
   and the recognition sequence.

Every emitted byte comes from a row, the geometry, the catalog's names and
targets or a profile constant.

## Resource bounds

| Operation | Work and residency |
| --- | --- |
| Probe and open | The probe prefix; block 256 per candidate; both sequences' blocks up to their bounds; the integrity and file set sequences; a sparing table, a VAT ICB and list, or the metadata files' lists; scratch for the claimed bitsets, the exposure bitset and the translators' tables |
| Listing | Per directory: its list, one FID pass and the common key sort; per child: one ICB read at preparation and one at replay, and a link's target decode; per object's first name: its list. No payload is read |
| Content | One index lookup per request after the walk at exposure; one VAT entry read per virtual block on a virtual volume |
| Verification | One traversal, one external sort of the link-count records, one pass over the space bitmap or the free-extent set, a comparison of the reserve sequence, and on a metadata volume one pass over the mirror |
| Planning | One pass over the catalog with one link-target read per symbolic link, one forms pass over the rows, a raise-until-fit search of a few steps, one layout pass |
| Emission | Payload bytes plus metadata proportional to entries, names and the bitmap; one pass over the child stream |

Only the volume model, one block window per role, the current ICB and the
walker's cursor are resident per reader, beside the common walk's charged
stack and path buffer and the 1024-byte decoded-target buffer; everything
else is charged scratch. Every loop charges the session and observes
cancellation.

## Failure classification

Reader:

| Condition | Status |
| --- | --- |
| No recognition sequence; a lone `CD001` sequence; an image shorter than the sequence | `FSIMG_EFORMAT` |
| Block size above 4096; anchors only at the tail; a CRC length shorter than a descriptor's fixed part; a revision, map set, access type, strategy, file type, flag, record format, extended-attribute file, multi-volume set or file set outside the profile; an AED holding only a continuation; an unflagged mirror whose extents are disjoint from the metadata file's; a spare packet inside a partition; a virtual volume whose last block holds no FE or EFE tag; a name with NUL, slash, `.` or `..`; a link target above 1024 bytes or with a component fsimg cannot spell; a type-2 timestamp or a zone of ±1440; freed space under `verify` | `FSIMG_EUNSUPPORTED` |
| A tag, CRC, location, length or reserved-byte fault; no anchor anywhere or two geometries; a sequence or chain bound exceeded; overlapping structures; a map, descriptor-list, FID, name, dstring, component or timestamp that contradicts the format; a claim, link-count, unique-ID, space, integrity, mirror or sparing-copy disagreement; an address outside its map or the volume | `FSIMG_ECORRUPT` |
| Unrepresentable selection component | `FSIMG_EINVAL` |
| Budget, scratch, work or descriptor exhaustion | `FSIMG_ELIMIT` |

Writer:

| Condition | Status |
| --- | --- |
| A setting outside its type; a name above 254 bytes after the compression ID or with a character from U+0001 to U+001F or U+007F; a label above 126 such bytes or with such a character | `FSIMG_EINVAL` |
| A link target with an empty or overlong component; a kind the profile lacks, from the common capability check; a metadata loss when the caller forbids losses | `FSIMG_EUNSUPPORTED` |
| Content that does not fit the exact size or `2^32` blocks | `FSIMG_ERANGE` |
| Budget, scratch, work or descriptor exhaustion | `FSIMG_ELIMIT` |

Diagnostics name a map index, a logical block and the resolved volume
offset where they help, quote at most one bounded, escaped name, and never
descriptor bytes or buffer contents.

## Tests

`test/udf.c` owns a synthetic fixture generator written from the format
design with its own offset tables and its own CRC; it shares no code with
`lib/formats/udf/`. It builds volumes in memory at every block size, from the
recognition sequence through anchors, both sequences, integrity sequences,
the four partition profiles, space bitmaps and tables, file sets, FE and
EFE forms, inline, short, long and AED-chained bodies with every extent type,
directories with deleted and crossing FIDs, CS0 names and component
streams, and mutates them for the format design's
[acceptance matrix](design-udf.md#acceptance-matrix). Every valid fixture
has a paired mutation that must fail with its class. Reading tests compare
`info`, `ls`, `cat` and `extract` on the fixtures and on every oracle image
the environment provides, using each image's manifest of names, sizes,
checksums and metadata.

`test/udf-write.c` creates images through the public API and reads them
back through its own independent checker, written from the format design
like the generator, as well as through the reader and verifier. It compares
bytes across source order, metadata cache size, scratch placement and time
zone, scans every unused byte for poison, and injects allocation, scratch,
short-read, short-write, EOF, source-change and cancellation failures at
every stage with cleanup checks. Its `--create` mode builds the native
qualification images and their manifests.

Scale tests record I/O calls, peak scratch and RSS: a `2^32`-block volume on
a sparse fixture whose bitsets stay in scratch, a directory wide enough to
spill, a file of a million descriptors through an AED chain, a VAT of a
million entries, and a deep tree. The writer's emit into a sink that holds
only nonzero pages a volume at the block-count limit and a directory whose
catalog spills.

`test/udf-integration.sh` runs the oracles the build finds and skips the
others explicitly: `mkudffs` and `udfinfo` across media types, revisions and
block sizes, `genisoimage -udf` and pycdlib bridges, 7-Zip listing and
extraction, a Linux read-only and read-write mount where the host permits
one, and NetBSD `fsck_udf -fn`, `makefs -t udf` and mount through the
environment's NetBSD guest. `fuzz/udf.c` feeds the probe, open, every map
type, traversal, content and verification paths under finite budgets with
seeds generated at run time. Sanitizer runs follow the build documentation.
