# QNX4 driver

## Scope

The [QNX4 format design](design-qnx4.md) establishes the on-disk facts, the
closed acceptance tables and the writer profile. This document maps them onto
code: the modules of `lib/formats/qnx4/`, the driver state behind the common
reader and writer, the algorithms that keep extent walking, allocation
checks, planning and emission bounded, and the tests that qualify them.

The driver implements the common contracts without exceptions: the
[format descriptor and probe](design-formats.md#format-descriptors-and-profiles),
[prepared directories](design-directories.md),
[logical content](design-content.md), [emission](design-emission.md) and
the [POSIX metadata extension](design-metadata.md). Common code gains no
QNX4-specific branch. The QNX6 driver is the structural template; the two
share no code, since they decode different records.

## Modules

| File | Owns |
| --- | --- |
| `qnx4.h` | Private types, constants and prototypes shared by the files below |
| `descriptor.c` | `fsimg_qnx4_format`: the `qnx4` name, its capability record and the probe |
| `codec.c` | Little-endian integer codecs and decode/encode of the 64-byte inode record, 64-byte link record, 512-byte extent block and 512-byte long-name record; pure functions without I/O |
| `volume.c` | Geometry from the slice length, the root block, the root directory's fixed slots, discovery of `.bitmap`, `.inodes`, `.longfilenames` and `lost+found` |
| `extent.c` | The extent walker: first extent and chain validation, index construction, ownership claims |
| `bitmap.c` | The bitmap pager: self-check at open and the allocated-bit query |
| `inode.c` | Record positions, link resolution through the `.inodes` locator, record validation and the record-to-entry mapping |
| `names.c` | The name profile, direct, link and long-name decoding, the mangled prefix |
| `directory.c` | `prepare`, `replay`, `keys` and `key`; dot entries; exposure tracking |
| `read.c` | `fsimg_qnx4_reader_ops`: open, close, resolve and volume information |
| `verify.c` | Full verification: ownership, references, bitmap merge, link counts, occupancy |
| `write-qnx4.h` | Plan rows and the plan object |
| `write-plan.c` | `validate`, `plan`, `free`: settings, the entry pass, sizing and layout |
| `write-emit.c` | `emit`: payload copies, slots, records, long names, the bitmap |

`extent.c` is the only place that interprets an extent or an extent block.
The bitmap, `.inodes`, `.longfilenames`, `lost+found`, every directory,
symbolic link and payload go through it, so range, overflow, chain-length,
loop, use-count, covered-sum, signature, first-copy and claim checks exist
once. The writer never needs its chain logic: every object it emits is one
extent.

## Codecs

`qnx4_u16()`, `qnx4_u32()` and their `put` counterparts decode and encode
little-endian integers from byte buffers. Decoded records are C structures
with `_t` typedefs holding only validated values:

| Type | Fields |
| --- | --- |
| `qnx4_extent_t` | `first` (u32 block), `blocks` (u32) |
| `qnx4_inode_t` | name length and 16 bytes, size, first extent, chain block, four times, extent count, mode, uid, gid, links, status, `reserved_zero` (bytes `0x3a` to `0x3d`) and `type` (byte `0x3e`) |
| `qnx4_link_t` | name length and 48 bytes, target block and slot, long-name block, status, and `spare_zero` (bytes `0x39` to `0x3e`) |
| `qnx4_xblock_t` | next, previous, used count, covered sum, sixty `qnx4_extent_t`, `spare_zero`, `signature_ok`, and the first-extent copy |
| `qnx4_longname_t` | directory block, slot, state, name length and 505 bytes, `tail_zero` |

A slot is decoded as an inode record or a link record by its status byte's
link bit, after the free-slot test. Encoding is the exact inverse and fills
every reserved byte with zero, so an emitted record never contains an
unspecified byte.

## Probe and open

The probe inspects 1024 bytes: the loader block and the root block. QNX4 has
no magic number, so the identity signature is root-block record 0: the name
`/` followed by fifteen NUL bytes, a status of `0x01` or `0x21`, and a
directory mode type. Without it the probe makes no claim (`FSIMG_EFORMAT`).
With it the probe checks that the first extent's block is nonzero and that
the extent lies inside the slice, else `FSIMG_ECORRUPT`; and that records 1
to 4 have 16-NUL name fields and the statuses of the format design's table
(`0x11`, with `0x13` allowed on records 2 and 3), else
`FSIMG_EUNSUPPORTED`. It then claims the image with `FSIMG_OK`. The probe performs no I/O and allocates nothing. Exclusivity
holds in both directions: a FAT, exFAT or QNX6 volume carries a jump, a BPB,
a boot area or a QNX6 header at byte 512, never `/` and fifteen NULs; and a
QNX4 root block carries no other format's signature at its offsets.

`open` completes recognition in a fixed order; each step fails before the
next reads:

1. **Geometry.** The slice length must be a multiple of 512; `N` is the
   quotient, `B = ceil(N / 4096)`, and `N` must be at least 43 and at most
   `2^31`. `qnx4_geometry_t` holds `N`, `B`, the slice length and the
   content domain `[(2 + B) * 512, N * 512)`.
2. **Root block.** Decode the five records against the format design's
   root-block table and require the zero ranges. Record 0's first extent
   must start at `3 + B`. Decode the label; an invalid one is recorded as
   absent, so `info` omits it and verification reports the volume
   incomplete.
3. **Bitmap pager.** Create the pager over blocks `3` to `2 + B` from the
   geometry alone and run its bounded self-check (below). No object can be
   walked before this step, because every walk checks allocation bits.
4. **Claims.** Create the claimed-block bitset of `N` bits in scratch and
   claim blocks `1` through `2 + B`.
5. **Root slots.** Walk record 0's extents under the allocated policy, a
   non-claiming read, and validate slots 0 to 5 against the root-slot
   table: the dots' targets, slot 2 a direct `.bitmap` record with extent
   `(3, B)` and size `ceil(N / 8)`, slots 3 to 5 links to `(2, 1)`,
   `(2, 2)` and `(2, 3)`. The root is claimed later by `prepare`, exactly
   like every other directory, so no directory is claimed twice and none
   is left unowned.
6. **`.inodes`.** Walk record 1 under the exclusive policy into the
   `.inodes` index, build the `.inodes` locator (below), read slot 0 and
   require the signature.
7. **`.longfilenames`.** Scan the root's remaining slots for a direct record
   of that name; when present, walk it under the exclusive policy into the
   long-name index and build its locator. A second such record, or a used
   root slot named `.bitmap` or `.inodes`, is `FSIMG_ECORRUPT`.
8. **`lost+found`.** Walk record 4 under the exclusive policy and validate
   its dots as the format design states. A used slot beyond the dots is
   recorded, so verification reports the volume incomplete; it never fails
   open and its content is never exposed.

The reader state then holds the geometry, the root block, the label when
valid, the `.inodes` and long-name indexes and locators, the claimed
bitset, the `.inodes` exposure bitset and one 4 KiB window for the bitmap;
the root and `lost+found` indexes are released after their checks, and
`lost+found`'s blocks stay claimed. `info` reports the geometry (volume
bytes `N * 512`, 512-byte sector and allocation unit, `N` units), the label
when valid and nonempty, the clean flag from record 0's status, and the
profile `long names` or `48-byte names` by the presence of
`.longfilenames`. `FSIMG_VOLUME_FREE` is set only by a completed
verification.

## The extent walker

`qnx4_index_build()` turns an inode record into a shared
[extent index](design-content.md#extent-index) over the content domain, or
over the bitmap domain `[1024, (2 + B) * 512)` for `.bitmap` alone:

1. Validate the first extent and the count against the format design's
   extent rules: count 0 exactly when the extent is empty, a chain pointer
   exactly when the count exceeds 1.
2. Append the first extent, then follow the chain. Before reading an extent
   block, check that its number lies in `[1, N]` and apply the walk policy
   to it; then read it through a 512-byte window and require its previous
   pointer to be the block just left (0 for the first), its used count 1
   to 60 and 60 in every
   block but the last, its spare bytes and unused descriptors zero, its
   covered sum equal to its descriptor lengths, its signature, and its
   first-extent copy equal to the record's first extent in the first block
   and zero in later ones. The chain ends when the descriptors appended
   reach `count - 1`; a next pointer there must be zero, and a zero next
   pointer earlier is a short chain. Either is `FSIMG_ECORRUPT`.
3. For each extent, apply the walk policy below, and during verification
   record an ownership run for it and for each extent block when the walk
   is the object's recording walk. Adjacent extents coalesce, so a
   contiguous object has one index record.
4. After the chain, check the allocation against the size by the kind
   table: exact for regular files, directories and symbolic links, any for
   a FIFO.
5. Charge one work unit per descriptor and the I/O of each extent block.

A walk reads at most `ceil((count - 1) / 60)` extent blocks and appends at
most `count` extents, so a hostile record bounds its own cost, and a loop
shows as a claim failure or as a count disagreement at the chain's end.

The policy is one of two:

| Policy | Used for | Rule |
| --- | --- | --- |
| Allocated | The root's slots at open; a later name of an `.inodes` object; verification's second walks of the system objects | Each block's bit is set |
| Exclusive | `.inodes`, `.longfilenames` and `lost+found` at open, every directory as it is entered, the first name of a file, symbolic link or FIFO | Each block's bit is set and the block is claimed |

A volume uses every block once, so the first walk of each object claims its
extents and its extent blocks in the claimed bitset; claiming a block already
claimed is `FSIMG_ECORRUPT` whichever object came first. The loader, root
block and bitmap are claimed at open, so an extent into them fails the same
way, and the content domain rejects them at index construction as well.

## The bitmap pager

The bitmap's position is fixed by the geometry, so the pager exists before
any record is trusted. `qnx4_block_allocated()` answers whether block `b`
in `[1, N]` is allocated by reading the 4 KiB window containing byte
`floor((b - 1) / 8)` of the bitmap bytes at volume offset
`1024 + floor((b - 1) / 8)`, retained between calls, and testing bit
`(b - 1) mod 8`, least significant first. The self-check at open is bounded
to two windows: in the first window, the bits of blocks `1` through
`min(2 + B, 32768)` are set; in the window holding the last bitmap block,
bits `N` through `8 * ceil(N / 8) - 1` are set and the bytes from
`ceil(N / 8)` to the end of block `2 + B` are `0xff`. Any clear bit or
other byte there is `FSIMG_ECORRUPT`. That the whole bitmap range through
`2 + B` is allocated is checked by verification's merge, where the bitmap's
own run is an owner. Padding bits never count as free.

## Identity and link resolution

A record position is `(block, slot)` packed as `block * 8 + slot` in a
`uint64_t`, the owner identity in verification and the key of every
reference. `qnx4_record_read()` reads a position's 64 bytes through a
window and decodes them; root-block positions `(2, 0)` to `(2, 4)` come from
the decoded root block.

The `.inodes` locator maps a physical block to its logical offset in the
`.inodes` index: the index's extents sorted by image offset, in scratch,
searched by binary search, built once at open from the chain's at most
`count` extents. The long-name locator is the same over `.longfilenames`.
`qnx4_link_resolve()` takes a link record and returns the target's position
and decoded record: the slot index is at most 7, the block lies in an
`.inodes` extent by the locator, the logical slot is not 0, and the record's
status is `0x11` or `0x13`; a free slot, a link record there or a position
outside `.inodes` is `FSIMG_ECORRUPT`, and a used slot with another status
is `FSIMG_EUNSUPPORTED`. It never accepts block 2: the three root links into
the root block are validated by position at open, and the root's `prepare`
admits slots 4 and 5 with the targets open validated instead of resolving
them.

`qnx4_inode_validate()` applies the closed tables to any inode record: the
status for its kind of position and mode, zero reserved bytes and type byte,
a 16-NUL name field in `.inodes` and the root block, a known mode type, a
size within `2^31 - 1`, and the count-and-extent consistency the walker
rechecks. The record-to-entry mapping is the format design's kind table;
the entry's metadata carries owner, the low twelve mode bits and the four
times as UTC seconds.

## Directories and names

`prepare` builds the directory's index under the exclusive policy and scans
its slots in 4 KiB windows. The root is record `(2, 0)` at level 0; deeper
levels use the current entry's locator. Entering a directory claims its
storage, so a directory reached from a second parent, through a cycle or
through two link names fails with `FSIMG_ECORRUPT` at entry, with no
separate rule for hardlinked directories.

Slots 0 and 1 must be the dot entries, validated against the directory's
own position and its parent's position; the root's both name `(2, 0)`, and
`lost+found`'s `..` may name `(3 + B, 0)`. They are never admitted. Every
later slot whose status has neither the used nor the link bit is free and
is not decoded further. A used slot is validated as a direct record, or as
a link record whose target resolves; at the root, slots 2 and 3 and the
`.longfilenames` record are skipped after their validation at open, and
slots 4 and 5 are admitted with their fixed targets. The name is then
decoded: the direct field's 1 to 16 bytes, the link field's 1 to 48 bytes,
or, with a nonzero long-name pointer, the long-name record found through the
long-name locator, whose back-reference, state, length and mangled prefix
with its zero byte must agree. The full name is checked against the fsimg
profile; an empty name, a NUL before the length, a slash, or `.` or `..` in
a slot beyond the first two is `FSIMG_ECORRUPT`.

An admitted record carries the displayed name and a 16-byte payload: the
object's position and the directory's own position, each a 64-bit
little-endian value. `keys` emits one key, the exact name bytes; `key`
validates a query component against the same profile and returns its bytes.
The `ambiguous` text names a duplicate QNX4 directory name.

`replay` re-validates the payload, reads the record again and fills the
entry. A paged bitset of one bit per `.inodes` slot records the objects
whose first name was replayed. For the first name of a file, symbolic link
or FIFO, `replay` builds the content index under the exclusive policy and
then sets the bit; a later name builds its index on its first `resolve`
under the allocated policy. A direct record has one name and no bit.
`reader->initialized` is the record's size, zero for a FIFO. A FIFO's index
is built and claimed but never delivered from.

## Content

`resolve` is a lookup in the current object's index. Sequential reads
advance one extent cursor and batch adjacent blocks; a random offset costs
one binary search. The index is released when the walker advances. A chain
is walked at most once per name, and no content read rereads its directory
or `.inodes`.

## Verification

Full verification runs the common walker once with the ownership policy
enabled, then reconciles. During the walk the driver appends:

* an ownership run `(first, count, owner)` for every extent and extent block
  of each object's recording walk, under the object's position, with
  reserved owners above the position space for the loader, the root block
  and the bitmap. As in QNX6, exactly one walk records each object: the
  verifier's walks of `.inodes`, `.longfilenames` and `lost+found` as it
  starts (open recorded nothing), each directory's as it is entered, and
  each other object's under its first name; a later name's walk never
  records;
* a reference record for every used slot, whether or not the entry is
  exposed, so the hidden root records' link counts are checked like every
  other: a name slot under the object's position; for every directory one
  under its own position for `.` and one under its parent's for `..`,
  where the root's `..` counts under the root and `lost+found`'s under the
  root as well; and root-block record 4 as `lost+found`'s one name;
* a long-name reference for every link with a long-name pointer, under the
  record's block.

Each object's extents are recorded by exactly one walk, so any two
overlapping runs are a block with two owners and fail. The runs are sorted
externally and merged with the bitmap in 4 KiB pages: every block in
`[1, N]` has its bit set exactly when it has an owner, and the bits past `N`
are set. The free count is the number of clear bits and sets
`FSIMG_VOLUME_FREE`.

The reference stream, sorted by position, gives every object's reference
count, which must equal its link count: `2 + subdirectories` for a
directory, by construction of the dot references, and the name count for
every other object. A sequential pass over `.inodes` then checks that every
used slot but 0 was referenced; a referenced free slot already failed at
resolution, and a free slot's bytes are not examined. A sequential pass
over `.longfilenames` checks that
every record with state `0x01` was referenced exactly once. Payload
read-through, when requested, reads each object's initialized bytes through
common delivery under its first name. The report counts files, directories
including the root, logical bytes, bytes read, allocated and free blocks;
bad blocks are zero, since `.bad_blks` is an ordinary file.

An unsupported kind, name or status anywhere, a used slot in `lost+found`
or an invalid label makes the verification incomplete with
`FSIMG_EUNSUPPORTED`; the report is never marked complete after skipping an
object.

## Writer settings

`validate` accepts the common options with these rules: `sector_bytes` and
`allocation_unit_bytes` zero or 512; `fat_root_entries` zero; `label` NULL
or at most 48 bytes under the profile's byte rules, where NULL and the empty
string both select the zero field; `epoch` within `[0, 4294967295]` in
reproducible mode; `volume_bytes` zero or a multiple of 512 in
`[43 * 512, 2^31 * 512]`. `volume_serial` is ignored, as the profile stores
none. The capability record's `write_settings` names `inode_capacity`
alone:

| Field | CLI setting | Meaning |
| --- | --- | --- |
| `inode_capacity` | `inodes=<count>` | The `.inodes` slot count, slot 0 included: the capacity is `max(128, count, 1 + indirect)` rounded up to a whole block of 8 slots, so the table is never below the native 16 blocks and always holds the catalog |

## Planning

The plan object embeds `fsimg_plan_t`, the geometry, the first blocks of
the root directory, `.inodes`, `lost+found`, `.longfilenames` and the first
catalog object, the `.inodes` capacity and used count, the long-name count,
the catalog IDs of `/.boot` and `/.altboot` or sentinels, the allocated
block count `A`, the format time, and one paged array of 64-byte rows keyed
by catalog ID with row 0 for the root. A row holds the size, the first block
of the object's one extent, its slot index in the parent and, for a
directory, the slots assigned so far, its `.inodes` slot or a sentinel, its
long-name record index or a sentinel, and the record fields already encoded
under the metadata policy: owner, the four times, mode, link count and
status. The parent's first block, the `.inodes` block of a slot and the
block of a long-name record all derive from these and the layout, so a row
stores no pointer. Rows are the only per-entry state; no vector grows with
`N`.

Planning runs a fixed sequence of passes over the frozen catalog:

1. **Entries.** For each catalog ID in order: check the name length and
   profile, and refuse a root entry named `.bitmap`, `.inodes`,
   `.longfilenames` or `lost+found` with `FSIMG_EINVAL`; choose a direct
   record for 1 to 16 bytes, else assign the next `.inodes` slot from 1 and,
   above 48 bytes, the next long-name record; count the entry into its
   parent's slots and, for a directory, into the parent's link count; take
   `/.boot` and `/.altboot` out of the root's slots when they are regular
   files and fail them as `FSIMG_EUNSUPPORTED` otherwise; validate kind and
   metadata under the [metadata policy](design-metadata.md#writer-policy)
   with the limits of the writer profile, where an owner or group above
   65534 is stored as 0 and a time outside 0 through 2147483647 takes the
   format time, each a counted optional loss that fails only when the
   caller forbids losses, while a size above `2^31 - 1` fails as
   `FSIMG_EUNSUPPORTED`; accumulate the blocks of every object and
   `initialized_bytes`. A time is stored as unsigned UTC seconds; an absent
   one takes the format time, and in reproducible mode every time is the
   epoch. Sibling collisions are exact byte collisions, which catalog
   preparation has already rejected.
2. **Geometry.** Directory sizes follow the writer profile's rounding; the
   `.inodes` capacity is the setting rule above; `.longfilenames` is
   `16 * ceil(long names / 16)` blocks. The plan's block demand is
   `D = R + I + 16 + L + objects + ceil(min_free_bytes / 512)`, the
   loader, root block and bitmap excluded. A volume fits when
   `2 + B(N) + D <= N`. With exact `volume_bytes`, `N` is given and must
   fit, else `FSIMG_ERANGE`. With automatic sizing the search raises `N`
   until it fits: start at `N = 3 + D`, and while `2 + B(N) + D > N` set
   `N = 2 + B(N) + D`; each step adds the bitmap's growth, one block per
   4096, so the loop ends within a few steps at the smallest fitting `N`.
   An empty catalog ends at 43. `N` above `2^31` is `FSIMG_ERANGE`.
3. **Layout.** Assign first blocks in the fixed order: the root directory
   at `3 + B`, `.inodes`, `lost+found`, `.longfilenames`, then every entry
   in catalog ID order, each one extent. The last block used is `A`, which
   fits by construction.
4. **Summary.** Fill `fsimg_plan_info_t`: volume, metadata, allocated, free
   and initialized bytes, the 512-byte sector and block, and the loss count.

Every value in the plan derives from the catalog, the options and these
rules, so byte-identical output does not depend on source order, budgets,
sort fan-in, scratch placement, locale or time zone.

## Emission

The common emission object has already zeroed the sink. The driver emits:

1. For each row in ID order: the payload copy for a file or symbolic link
   over its extent; the `.` and `..` slots of a directory, with the first
   extent length at byte 8 of `.`; the entry's slot in its parent, a direct
   record, or a link record plus the inode record at its `.inodes` slot;
   and its long-name record when it has one. Slots are 64-byte writes at
   `(parent.first - 1) * 512 + slot * 64`.
2. The root block: record 0 from row 0 with status `0x21`, records 1 and 4
   with their profile values, records 2 and 3 from the `/.boot` and
   `/.altboot` rows or their empty defaults, every one with a 16-NUL name
   field, the label, zeros elsewhere.
3. The root's fixed slots 0 to 6, slot 7 when no entry took it, slot 8, and
   `.inodes` slot 0; `lost+found`'s two dot slots, `..` targeting
   `(3 + B, 0)`.
4. The bitmap: ones for bits `0` to `A - 1`, zeros to bit `N - 1`, ones to
   the end of the last used byte and `0xff` bytes to the end of block
   `2 + B`, written in whole blocks from the emission buffer.

Every emitted byte comes from a row, the geometry or a profile constant.
Format-owned records carry the format time in all four fields.

## Resource bounds

| Operation | Work and residency |
| --- | --- |
| Probe and open | Two 512-byte reads, the bitmap self-check's two windows, the chains of the root directory, `.inodes`, `.longfilenames` and `lost+found`; scratch for the indexes and locators, the claimed bitset of `N` bits, which every open creates, `info` included, and the exposure bitset of the `.inodes` slot count |
| Listing | Per directory: its chain, one slot pass and the common key sort; per object's first name: its chain. No payload is read |
| Content | One extent lookup per request after the walk at exposure |
| Verification | One traversal, two external sorts of streams proportional to extents and names, one bitmap pass of `ceil(N / 8)` bytes, one pass over `.inodes` and one over `.longfilenames` |
| Planning | One pass over the catalog, the raise-until-fit size search of a few steps, one layout pass; rows in one paged array |
| Emission | Payload bytes plus metadata proportional to entries, names and the bitmap |

Only the geometry, the root block, the bitmap window, one record window, the
current record and the chain cursor are resident per reader; everything else
is charged scratch. Every loop charges the session and observes
cancellation.

## Failure classification

Reader:

| Condition | Status |
| --- | --- |
| No root signature in block 2; image shorter than 1024 bytes | `FSIMG_EFORMAT` |
| Recognized but unobserved: a status, type byte, reserved or spare byte, `.inodes` status or name field, zero root-block record 4, extent-block fill, kind, size above `2^31 - 1`, or name outside the profile; a used slot in `lost+found` or an invalid label under `verify` | `FSIMG_EUNSUPPORTED` |
| Slice length not a multiple of 512; `N` outside `[43, 2^31]`; root extent not at `3 + B`; bitmap position, size or padding; used with link; empty name, NUL, slash or dot name; range, chain, count, signature, first-copy, claim, dot-entry, link-target, long-name, link-count, occupancy or free-bit inconsistency | `FSIMG_ECORRUPT` |
| Unrepresentable selection component | `FSIMG_EINVAL` |
| Budget, scratch, work or descriptor exhaustion | `FSIMG_ELIMIT` |

Writer:

| Condition | Status |
| --- | --- |
| Setting outside its type, name outside the profile, reserved root name | `FSIMG_EINVAL` |
| Catalog `/.boot` or `/.altboot` of another kind; a size above `2^31 - 1`; a kind the profile lacks; an owner or time loss when the caller forbids losses | `FSIMG_EUNSUPPORTED` |
| Content does not fit the exact size or `2^31` blocks | `FSIMG_ERANGE` |
| Budget, scratch, work or descriptor exhaustion | `FSIMG_ELIMIT` |

Diagnostics quote at most one bounded, escaped name and never slot bytes,
stale free-slot contents or buffer contents.

## Tests

`test/qnx4.c` owns a synthetic fixture generator written from the format
design with its own offset tables; it shares no code with
`lib/formats/qnx4/`. It builds volumes in memory from the loader and root
block through bitmaps with chosen padding, root and nested directories,
`.inodes`, direct and link records, extent-block chains and long-name
records, and mutates them for the format design's
[acceptance matrix](design-qnx4.md#acceptance-matrix). Every valid fixture
has a paired mutation that must fail with its class, which proves the check
ran. Reading tests compare `info`, `ls`, `cat` and `extract` on the fixtures
and on every native capture found through `FSIMG_QNX4_NATIVE`, using the
manifest of names, sizes and checksums each capture records.

`test/qnx4-write.c` creates images through the public API and reads them
back through its own independent checker, written from the format design
like the generator, as well as through the reader and verifier. It compares
bytes across source order, metadata cache size, scratch placement and time
zone, and scans every unused byte for poison. Its `--native` mode compares an
empty volume with the native `dinit` capture of the same geometry: every
block but the loader matches byte for byte apart from times. Its `--create`
mode builds the native qualification tree. Fault tests inject allocation,
scratch, short-read, short-write, EOF, source-change and cancellation
failures at every stage and check cleanup. Scale tests grow `N` to near
`2^31` on a sparse fixture with the bitmap in scratch, one directory's width
to spilling, one file to about `10^5` extents and a tree's depth, recording
I/O calls, metadata passes, peak scratch and RSS.

`test/qnx4-integration.sh` runs the independent oracles the build finds and
skips the others explicitly: the Linux `qnx4` reader on a Linux host, as a
comparison for 48-byte names; qnxprobe as a comparison when its path is
given; the native captures; the PCM partition through an image path and
slice bounds from the environment, running only `info` and `verify` and
printing counts; and the QNX 6.5.0 harness for the native `chkfsys`, mount,
modify, sync, unmount and remount cycle that gates writer registration.
`fuzz/qnx4.c` feeds the probe, open, traversal, content and verification
paths under finite budgets with seeds generated at run time. Sanitizer runs
follow the build documentation.
