# QNX6 driver

## Scope

The [QNX6 format design](design-qnx6.md) establishes the on-disk facts, the
compatibility profiles and the evidence gates. This document maps them onto
code: the modules of `lib/formats/qnx6/`, the driver state behind the common
reader and writer, the algorithms that keep tree traversal, allocation checks,
planning and emission bounded, and the tests that qualify them. It covers the
classic profile in both byte orders at every classic block size. The bootless
and MMI layouts receive their own header decoder and reservation model under
the same module structure once their gates close; nothing here assumes them.

The driver implements the common contracts without exceptions: the
[format descriptor and probe](design-formats.md#format-descriptors-and-profiles),
[prepared directories](design-directories.md),
[logical content](design-content.md), [emission](design-emission.md), and
the [POSIX metadata extension](design-metadata.md) that gives the entry model
owner, mode, change time, symbolic links and FIFOs. Common code gains no
QNX6-specific branch.

## Modules

| File | Owns |
| --- | --- |
| `qnx6.h` | Private types, constants and prototypes shared by the files below |
| `descriptor.c` | `fsimg_qnx6_format`: the `qnx6` variant, its capability record and the probe |
| `codec.c` | Byte-order aware integer codecs, the superblock CRC, the long-name checksum, and decode/encode of the 512-byte header, 80-byte tree descriptor, 128-byte inode and 32-byte directory slot; pure functions without I/O |
| `header.c` | Header candidates, field validation, pair agreement, snapshot selection, geometry and the system reservation |
| `tree.c` | The bounded block-tree walker: shape arithmetic, index construction, cycle and range checks, allocation and sharing checks |
| `bitmap.c` | The allocation-bitmap pager: self-validation at open and the allocated-bit query |
| `inode.c` | Inode-table access, inode state validation and the inode-to-entry mapping |
| `names.c` | The name profile, inline and long-name decoding, and checksum policy |
| `directory.c` | `prepare`, `replay`, `keys` and `key`; dot entries; exposure tracking |
| `read.c` | `fsimg_qnx6_reader_ops`: open, close, resolve and volume information |
| `verify.c` | Snapshot-aware full verification |
| `write-qnx6.h` | Plan rows and the plan object |
| `write-plan.c` | `validate`, `plan`, `free`: settings, entry pass, sizing, system run and layout |
| `write-emit.c` | `emit`: payload copies, slots, inodes, indirect blocks, bitmap, headers |

`tree.c` is the only place that interprets a block reference. Readers of
system trees, directories, long names and payload all call it, so range,
sentinel, cycle, allocation-bit and sharing checks exist once. The writer uses
the same shape arithmetic in `tree.c` to count and place indirect blocks, so
plan and emission cannot disagree about where a tree's blocks are.

## Codecs

Every disk integer is decoded from bytes with the volume's byte order, which
the header's magic establishes: `qnx6_u16()`, `qnx6_u32()`, `qnx6_u64()` and
their `put` counterparts take the order as a parameter, so neither the reader
nor the writer has host-endian state. The CRC is the MSB-first polynomial
`0x04c11db7` with zero initial register and no reflection or final XOR, over
header bytes `[8, 512)`; the long-name checksum is the halve-add-toggle
recurrence over unsigned bytes with the zero-to-`0xffffffff` substitution.
Both are implemented from the format design's definitions and tested against
its check values before any fixture is read.

Decoded records are C structures with `_t` typedefs that hold only validated
values:

| Type | Fields |
| --- | --- |
| `qnx6_tree_t` | `length` (u64), `ref[16]` (u32), `depth` (u8) |
| `qnx6_header_t` | serial, format and sync times, flags, reserve percent, UUID, block size, inode capacity, free inodes, block count, free blocks, group count, three `qnx6_tree_t` roots |
| `qnx6_inode_t` | size, uid, gid, four times, mode, link count, `qnx6_tree_t`, status |
| `qnx6_slot_t` | inode number, and either an inline name (length, bytes) or a long-name index and checksum |

Encoding is the exact inverse and fills every reserved byte with its profile
value, so an emitted record never contains an unspecified byte.

## Geometry, headers and the reservation

`qnx6_geometry_t` holds the slice length `V`, the data origin `D = 12288`,
the block size `B`, `P = B / 4`, the block count `N`, the second header
position `S2 = floor(V / 4096) * 4096 - 4096`, the byte order, the inode
capacity, and the system reservation `R`. Products and sums use the checked
64-bit helpers; a geometry that fails a check is corrupt, never clamped.

The probe inspects 8704 bytes. A classic magic at byte 8192 in either byte
order is the identity signature; the probe then checks the CRC, the version
word 4, and a block size in the legal set, and claims the image with
`FSIMG_OK`, or `FSIMG_ECORRUPT` when the CRC fails, or `FSIMG_EUNSUPPORTED`
when a header is recognized but outside the classic table. A magic at byte
zero without one at 8192 is a recognized bootless or MMI layout and claims
`FSIMG_EUNSUPPORTED` until those profiles have decoders; a magic at both
positions is ambiguous and also unsupported. The probe performs no I/O and
allocates nothing.

`open` reads both header areas with exact 512-byte reads and validates each
header completely:

* magic, CRC and byte order; version 4; flags other than the mounted bit
  `0x1` in `{0, 0x100}`; reserve at
  most 100; groups 1 through 64; block size in `{512, 1024, 2048, 4096}`;
* `S2 > 12288`, `D + N * B <= S2`, and `N >= 1`;
* inode capacity at least 2 with `128 * capacity` representable, and the
  inode-table length equal to it; bitmap length equal to `ceil(N / 8)`;
  long-name length a multiple of `B`; the fourth tree empty with sentinel
  references, depth 0 and mode 1; extension bytes `[392, 512)` zero;
* every descriptor with mode 1, zero reserved bytes, the minimal depth for
  its length, and depth at most 2 for the inode and bitmap trees.

Any deviation from the native layout that is not a byte-level inconsistency,
such as an oversized bitmap or a deeper system tree, is `FSIMG_EUNSUPPORTED`:
the reservation model below assumes the native layout, so the driver refuses
rather than guesses. The two headers must agree on UUID, format time, block
size, block count, inode capacity, group count and flags other than the
mounted bit, which is snapshot state. The larger serial is
selected; equal serials require byte-identical headers and are otherwise
`FSIMG_ECORRUPT` as an ambiguous tie. Only the selected header's descriptors
are used afterwards.

The reservation `R` is derived, not read: it is the block count of the bitmap
tree plus the block count of the inode tree, both computed from the geometry
by the tree-shape function below. The classic profile places the selected
snapshot's bitmap and inode-table blocks inside `[0, 2R)` and everything else
outside it. The driver therefore works with two extent domains over the data
origin: the system domain `[D, D + 2R * B)` for the bitmap and inode-table
indexes, and the content domain `[D + 2R * B, D + N * B)` for directories,
long names, symbolic links and payload. `reader->content_domain` is the
content domain, so common delivery cannot return a system block as payload
even through a defective driver index, and every index built for an ordinary
object rejects a reference into the reservation at construction. A system
tree reference outside the system domain, or an ordinary reference inside
it, is corruption. `2R <= N` is checked before either domain exists.

## Trees

A tree with `n = ceil(length / B)` content blocks has the minimal depth `L`
with `16 * P^L >= n`, computed by repeated checked multiplication. Level `j`
of its indirect blocks, numbered from 1 at the root's children to `L` at the
leaves' parents, holds `count_j = ceil(n / P^(L + 1 - j))` blocks, and the
tree has `I = sum(count_j)` indirect blocks. Node `(j, i)` covers children
`[i * P, min((i + 1) * P, count_(j + 1)))`, or leaves in the same range at
level `L`. The driver lays every tree it emits out as one range: its indirect
blocks in post-order, then its `n` content blocks in logical order. The
post-order position of node `(j, i)` is

```text
pos(j, i) = sum(m = j..L: min((i + 1) * P^(m - j), count_m)) - 1
          + sum(m = 1..j-1: floor(i / P^(j - m)))
```

which the format design's measured system run (`B = 512`: bitmap indirect
block 0, bitmap leaves 1 through 32, inode indirect blocks 33 through 65, inode
leaves from 66) satisfies, and which tests check against an explicit
post-order enumeration for every depth up to 4. The writer never stores a
per-block pointer vector: an indirect block's contents are computed from
`(first, n, P, L)` when emitted.

The reader's walker, `qnx6_index_build()`, turns a descriptor into a shared
[extent index](design-content.md#extent-index) over one of the two domains:

1. Validate the descriptor: depth at most 5, `n` at most `16 * P^L` by checked
   comparison, and `length` consistent with the caller's object rule.
2. Walk the required slots only: root slots `[0, ceil(n / P^L))`, and in each
   indirect block the slots that cover logical blocks below `n`. Every required
   reference is not the sentinel and below `N`; every slot beyond the required
   range, in the root and in the last partially used indirect block of every
   level, is the sentinel. A valid reference beyond the logical end is
   unqualified preallocation and `FSIMG_EUNSUPPORTED`.
3. For each visited block, apply the policy the caller selects (below), and
   during verification record it as an ownership run. Append leaves to the
   index in logical order; adjacent leaves coalesce, so a contiguous object
   has one extent.
4. Charge one work unit per reference and the I/O of each indirect block.

The policy is one of three, a closed set:

| Policy | Used for | Rule |
| --- | --- | --- |
| Bootstrap | The bitmap tree at open | Record every block; the bitmap does not exist yet |
| Allocated | The inode table at open, a later name of an inode, verification's second walks of the system trees | Check the allocation bit |
| Exclusive | The long-name tree at open, every directory as it is entered, the first name of a file, symbolic link or FIFO | Check the allocation bit and claim the block |

Outside the system trees one committed snapshot uses every block once:
directories, long names, indirect blocks and file data never share storage,
and no tree names one block twice. A paged bitset of `N` bits, created at
open, holds the claimed blocks; claiming a block already claimed is
`FSIMG_ECORRUPT`, whichever object came first. A hard link's later names
walk a tree its first name already claimed, so they only recheck allocation.
The system trees are not claimed; their blocks lie in the system domain,
which no other object can reach, and verification's ownership merge checks
them against each other.

The walker keeps the current path of at most five indirect block numbers; a
block that equals one of its ancestors is a cycle and `FSIMG_ECORRUPT`.
Indirect blocks are read through 4 KiB windows, never buffers sized from the
descriptor. The depth bound, the required-slot bound and the work charge
together bound a walk by the object's declared length, so a hostile
descriptor cannot make the walker visit more than `n + I` blocks. Because
every block of an exclusive walk is claimed once, the exclusive walks of one
reader visit at most `N` blocks in total.

## Allocation bitmap

The bitmap index is built first, before any allocation bit can be consulted,
with the policy "record every visited block". After the index exists the
recorded blocks, indirect and leaf, are checked one by one through it: each
must have its allocation bit set. This closes the self-reference without a
pending ledger: the set of blocks whose bits the check needs is exactly the
set the index covers, and resolving a bit through the index reads bitmap
pages without visiting any further block. The visited-block record is a
sealed stream in scratch, released after the check. The cost is the bitmap
tree's indirect blocks, at most a few thousand at any classic geometry, plus
one page read per recorded block in the pathological case of a scattered
bitmap tree, all charged to the session.

`qnx6_block_allocated()` answers whether block `p < N` is allocated by
resolving page `floor(p / 32768)` of the bitmap through the index into a
4 KiB window retained between calls, and testing bit `p mod 8` of byte
`floor(p / 8)`, least significant bit first. Bits at or past `N` within the
last bitmap block must be set; they are checked once at open and never count
as free. Every later walk, including the inode-table and long-name indexes
built at open, checks the bit of every block it visits before using it.

## Inodes

`qnx6_inode_read()` validates `1 <= i <= capacity`, reads the 128 bytes at
logical offset `(i - 1) * 128` of the inode-table index, decodes them and
validates the record: status bit 0 set, no status bits above bit 1, bytes
102 through 127 zero, a known type in the mode word, a representable size for
the type, and a tree descriptor whose required leaves fit `N`. A record that
fails is corruption when a directory slot referenced it, since a live slot
never names a free or malformed inode in a committed snapshot.

| Mode type | Entry kind | Content |
| --- | --- | --- |
| `0100000` | `FSIMG_ENTRY_FILE` | Payload of `size` bytes, `initialized == size` |
| `0040000` | `FSIMG_ENTRY_DIRECTORY` | Slots, `size` a positive multiple of 32 |
| `0120000` | `FSIMG_ENTRY_SYMLINK` | Target of `size` bytes, 1 to 1023; zero is corrupt, longer is unsupported |
| `0010000` | `FSIMG_ENTRY_FIFO` | None; `size` must be zero |
| other | unsupported | The containing directory's preparation fails |

The entry's metadata sets `FSIMG_META_OWNER`, `FSIMG_META_MODE` with the low
twelve mode bits, and all four timestamps as UTC seconds with one-second
precision: `created` from `ftime`, `modified`, `accessed` and `changed` from
their fields. Attributes are absent.

## Directories and names

`prepare` builds the extent index of the directory's content under the
exclusive policy and scans the slots in 4 KiB windows. The root is inode 1
at level 0; deeper levels use the current entry's inode. The long-name
tree's blocks are claimed at open, so a directory whose storage overlaps a
long-name block, an object walked before it, or itself through a cycle fails
with `FSIMG_ECORRUPT` when it is entered. Entering the same directory again
from a second parent fails the same way, which also rejects hardlinked
directories.

Slots 0 and 1 must be `.` naming the directory's own inode and `..` naming
its parent's inode, both 1 for the root; they are validated and not admitted.
Every other slot with a zero inode or zero name length is unused and is not
decoded further. A live slot names an inode that `qnx6_inode_read()` accepts;
the slot's name is the inline bytes, or the long-name record at logical byte
`index * B` of the long-name index, whose two-byte length must be 28 through
510 and fit both its block and the stream. A long-name slot's bytes 5 through
7 and 16 through 31 are zero natively; anything else there is an unqualified
extension. With flag `0x100` the stored
checksum must equal the recurrence over the name bytes; without it the field
must be zero. QNX itself forbids NUL, slash and the dot names, so they are
corruption. The decoded name is handed to preparation, which applies the
QNX-family profile the descriptor declares (strict UTF-8, no NUL, slash,
control byte or `0x7f`, neither `.` nor `..`) and assigns the child's class
under the [omitted-names contract](design-directories.md#omitted-names);
the driver chooses no class, and one foreign name never fails the directory
or the image.

An admitted record carries its class, its component as the
[contract](design-directories.md#omitted-names) stores it per class, and an
8-byte payload: the
inode number and the parent directory's inode number, each a 32-bit
little-endian value. `keys` emits one key, the exact name bytes;
`key` validates a query component against the same profile and returns its
bytes, so selection and namespace validation compare identical keys. The
`ambiguous` text names a duplicate QNX6 directory name.

`replay` re-validates the payload, reads the inode again and fills the entry.
A paged bitset of one bit per inode records the inodes whose first name was
replayed. For the first name of a file, symbolic link or FIFO, `replay`
builds the content index under the exclusive policy and then sets the bit,
so the tree is claimed once the name is exposed. A later name of the same
inode builds its index on its first `resolve`, under the allocated policy,
so listing many names of one file walks its tree once. Validation is
therefore order dependent: storage shared by two objects fails when the
second of them is walked, and an operation that never walks one of them,
such as a selective read that skips a directory, does not detect it. Full
verification walks everything. `reader->initialized` is the inode's size. A
directory's index is built on descent; a FIFO's tree is empty.

## Content

`resolve` is a lookup in the current file's index, which it first builds for
a later name of an inode. Sequential reads therefore advance one extent
cursor and batch adjacent blocks; a random offset costs one binary search.
The index is released when the walker advances. A name's tree is walked at
most once, and no content read rereads its parent directory.

## Verification

Full verification runs the common walker once with the ownership policy
enabled, then reconciles. During the walk the driver appends:

* an ownership run for every block visited by every index it builds, as
  `(first, count, owner)` with owner identity the inode number; blocks
  visited consecutively extend a run. The system trees and the long-name
  tree use reserved owner identities above the inode space; the verifier
  walks them once more as it starts, since the indexes built at open hold
  only leaves;
* a reference record for every live slot, including `.` and `..`, naming the
  referenced inode.

Both streams are sealed and externally sorted. Exactly one walk records
each tree: the verifier's walks of the system and long-name trees, each
directory's as it is entered, and each inode's under its first name. A
later name's walk never records, and payload read-through reads an inode's
bytes under its first name only, since a later name has the same bytes.
Any two overlapping runs are therefore a block with two owners, or one used
twice by one tree, and fail. The listing walk's claims reject most such images
first; the merge also covers the system trees, which are not claimed. The
merge then streams the bitmap in 4 KiB pages and, for each block: below
`2R`, the bit is set and any owner is a system tree; from `2R`, the bit is
set exactly when an owner exists; at or past `N`, the bit is set. The free
count is the number of clear bits and must equal the selected header's free
block count.

The reference stream, sorted by inode, yields each referenced inode's name
count; it must equal the inode's link count, which for a directory is
`2 + subdirectories` by construction of the `.` and `..` references. A
sequential pass over the inode table then checks that every inode with status
bit 0 set was referenced, that unreferenced inodes have bit 0 clear, and that
the header's free inode count equals the capacity minus the referenced count.
Payload read-through, when requested, reads each inode's initialized bytes
through common delivery during the walk. The report counts files, directories
including the root, logical bytes, bytes read, allocated and free blocks;
bad blocks are always zero, since the format has no bad-block marking. A
completed verification sets `FSIMG_VOLUME_FREE` from the counted free blocks.

An unsupported inode type or extension encountered anywhere makes the
verification incomplete with `FSIMG_EUNSUPPORTED`; the report is never marked
complete after skipping an object for its kind. An omitted name is not such
a skip: verification descends into an omitted directory and replays an
omitted inode under the
[omitted-names contract](design-directories.md#omitted-names), so its
ownership runs and its slots' reference records are appended exactly as a
delivered inode's are and the bitmap, link-count and inode-table
reconciliation covers it; the omitted subtree roots are counted in
`omitted_entries`, the report's files and directories exclude the omitted
subtrees, and the report is complete.

## Writer settings

`validate` accepts the common options with these rules: `sector_bytes` zero
or 512; `allocation_unit_bytes` zero, selecting 1024, or a classic block size,
which is the filesystem block size; `fat_root_entries` zero; `label` NULL,
which the common capability check already enforces with
`FSIMG_EUNSUPPORTED`, since the profile has no label; `epoch` within
`[0, 4294967295]` in reproducible mode; `volume_bytes` zero or a multiple of
512 of at least `16384 + 64 * B`. `volume_serial` is the identity of the
formats that have a serial; the profile stores none and ignores it, since the
CLI generates one for every format. The QNX6-specific fields of
`fsimg_write_options_t` are typed, and the capability record's
`write_settings` names all four, so every other format requires their
defaults:

| Field | CLI setting | Meaning |
| --- | --- | --- |
| `byte_order` | `endian=little` or `big` | Default little; selects the on-disk byte order of every field |
| `volume_uuid[16]` | `uuid=<RFC 4122 text>` | Default all zero, used verbatim in every mode like the FAT serial; written at header offset 32 in textual byte order. Without `-R` and `uuid=` the CLI generates a random version 4 UUID |
| `inode_capacity` | `inodes=<count>` | Zero selects the native ladder raised to the inodes the catalog needs; otherwise the exact capacity, rounded up to a whole inode-table block, which must hold the catalog |
| `reserve_percent` | `reserve=<percent>` | Default 3; the header's reserved-block percentage, at most 100 |

The emitted flags word is `0x100`: long-name checksums are always written.

## Planning

The plan object embeds `fsimg_plan_t`, the geometry, the shape of the three
system trees, the catalog ID of `.boot`, the first block of the long-name
stream, the allocated block count `A`, the used inode count and the format
time, and one paged array of 64-byte rows keyed by catalog ID, with row 0
for the root and one extra row for a synthesized `.boot` directory. A row
holds the content length, the first block of the object's range, the inode
number, its slot index in the parent and, for a directory, the slots
assigned so far, its long-name index or a sentinel, and the inode fields
already encoded under the metadata policy: owner, the four times, mode, link
count and status. The object's shape (content and indirect block counts and
depth) is a function of its length, so the row does not store it. Rows are
the only per-entry state; no vector grows with volume size, inode capacity or
name count.

Planning runs a fixed sequence of passes over the frozen catalog:

1. **Entries.** For each catalog ID in order: check the name length and
   profile; assign the inode number, 1 for the root, 2 for a catalog `/.boot`
   directory or the synthesized one, and the next free number for every
   other entry; compute the content block count, `ceil(size / B)` for files
   and symbolic links and the whole-block slot capacity for directories;
   count children and subdirectories into the parent's row to fix slot
   positions and link counts; assign the next long-name index when the name
   exceeds 27 bytes; validate kind and metadata representability under the
   [metadata policy](design-metadata.md#writer-policy); accumulate the totals
   of content and indirect blocks, for each candidate block size when
   minimization searches an open one, and `initialized_bytes`. The layout
   pass sets a directory's length from its slot count once the block size
   is chosen. A name outside the
   profile fails with `FSIMG_EINVAL`. A catalog `/.boot` that is not a
   directory fails with `FSIMG_EUNSUPPORTED`; a catalog `/.boot` directory
   keeps its children and takes the profile mode `0700`, and another
   requested mode is an optional loss. Without one, `.boot` is synthesized
   and its slot placed among the root's children in byte order. Owner
   defaults to zero and the mode to `0755` for directories, `0644` for files
   and `0777` for links and FIFOs. A time is stored as unsigned UTC seconds;
   an absent one, or one outside 32 bits, takes the format time, and in
   reproducible mode every time is the epoch. A present time that is not a
   whole-second UTC instant in range (or, when reproducible, the epoch) is a
   loss under the policy. Sibling collisions are exact byte collisions,
   which catalog preparation has already rejected, so the driver keeps no
   key stream.
2. **Geometry.** With exact `volume_bytes`, `E = floor(V / 4096) * 4096` and
   `N = (E - 16384) / B`. With automatic sizing, binary search the smallest
   `N` that is a multiple of 64 blocks and satisfies `fits(N)`:
   `2R(N) + objects + longnames + ceil(N * reserve / 100) +
   ceil(min_free / B) <= N`, where `R(N)` uses the inode capacity selected
   for `N` (the native ladder of one inode per 8, 32, 64 or 128 blocks,
   raised to the catalog's inode count, or the requested capacity) and the
   bitmap size `ceil(ceil(N / 8) / B)`. Raising `N` by 64 blocks adds 64
   blocks against at most fourteen system blocks and `ceil(64 * reserve /
   100)` reserve blocks at any block size, so the predicate is monotone over
   that step for reserves up to 76 percent, and the search is bounded by the
   32-bit reference space. With a larger reserve the search still returns a
   fitting `N`, not necessarily the smallest, and near the depth-2 limit of
   the system trees it can refuse a request a smaller `N` would hold. The inode and bitmap trees must
   have depth at most 2, which the native formatter also requires. Under the
   [minimization option](design-sizing.md) the search also covers the open
   block sizes and the exact inode capacity, and refines `N` to the
   alignment step within the last 64-block window.
3. **Layout.** Assign block ranges in a fixed order: the system run `[0, 2R)`
   as the format design's post-order recipe; the root directory and `.boot`
   from `2R`; every entry in catalog ID order; then the long-name stream. Each
   object's range is its indirect blocks followed by its content, so a file's
   payload is one emission region, and a directory's length is its whole
   blocks. The sum must equal `A`, and `A` plus the
   reserve must not exceed `N`.
4. **Summary.** Fill `fsimg_plan_info_t`: volume, metadata, allocated, free and
   initialized bytes, the sector and block sizes, the block count, and the
   inode capacity with its free inodes. The common
   [writer loss policy](design-metadata-loss.md) installs the loss count after
   every planning step succeeds.

Every value in the plan derives from the catalog, the options and these
rules, so byte-identical output does not depend on source order, budgets,
sort fan-in, scratch placement, locale or time zone.

## Emission

The common emission object has already zeroed the sink. The driver emits:

1. For each row in ID order: the payload copy for a file or symbolic link
   over its content range; the `.` and `..` slots of a directory; the entry's
   slot in its parent, inline or as a long-name slot; and the long-name record
   when it has one. Slots are 32-byte writes at `D + (parent.first +
   parent.indirect) * B + slot * 32`.
2. Indirect blocks of every object with depth above zero, generated from the
   row and written in batches of whole blocks from the emission buffer.
3. The inode table in inode order, batching consecutive 128-byte records in
   the emission buffer: unused records stay zero, root and `.boot` carry
   status 1 and their profile modes, every other inode status 3, link counts
   from the rows, and times from the metadata policy.
4. The bitmap leaves: ones for `[0, A)`, zeros to `N`, ones to the end of the
   last leaf; the bitmap and inode-tree indirect blocks; the long-name tree's
   indirect blocks. The twin region stays zero.
5. Both 512-byte headers, byte-identical with serial 1, format and sync times
   equal to the epoch in reproducible mode and otherwise the host's UTC clock
   at planning, flags `0x100`, the native formatter's default group count
   for the planned bitmap and inode-table leaf counts and `N` (the
   [format design](design-qnx6.md) gives the rule), the free counts, the
   three descriptors and the empty fourth tree, and the CRC, at 8192 and
   `S2`. The count describes no on-disk structure of the classic profile;
   the running filesystem uses it to spread later allocations, so storing
   the native value gives the native allocator the group count it would use
   on a natively formatted volume of the same geometry. The boot area and the
   rest of both header areas stay zero, as do bytes from `E` to `V`.

Every emitted byte comes from a row, the geometry or a profile constant.

## Resource bounds

| Operation | Work and residency |
| --- | --- |
| Probe and open | Two 512-byte reads, the indirect blocks of the three system trees, the bitmap self-check pages, and inode 1; scratch for three indexes, a bitset of `N` bits and one of an inode count's bits |
| Listing | Per directory: its tree walk, one slot pass and the common key sort; per inode's first name: its tree walk. No file payload is read |
| Content | One extent lookup per request after the walk at exposure |
| Verification | One traversal, two external sorts of streams proportional to blocks visited and slots, one bitmap pass of `ceil(N / 8)` bytes and one inode-table pass of `128 * capacity` bytes |
| Planning | One pass over the catalog, a bounded binary search, one layout pass; rows in one paged array |
| Emission | Payload bytes plus metadata proportional to entries, names, indirect blocks, the inode table and the bitmap |

Only the geometry, the selected header, two 4 KiB windows, the current
inode and the bounded tree path are resident per reader; everything else is
charged scratch. Every loop charges the session and observes cancellation.

## Failure classification

| Condition | Status |
| --- | --- |
| No classic magic at 8192 | `FSIMG_EFORMAT` |
| Recognized but unqualified: byte-zero layouts, flags outside the table, nonminimal or deep system trees, oversized bitmap, extension bytes, unknown inode type or status bits, nonzero inode tail, preallocation past the logical end, link target above 1023 bytes | `FSIMG_EUNSUPPORTED` |
| CRC, range, sentinel, free-bit, sharing, dot-entry, name-checksum, link-count, tie or counter inconsistency | `FSIMG_ECORRUPT` |
| Setting outside its type, name outside the profile, unrepresentable selection component | `FSIMG_EINVAL` |
| Label for this profile | `FSIMG_EUNSUPPORTED`, from the common capability check |
| Content and reserve do not fit the exact or any automatic size; required metadata, kind or link-target length that the profile cannot store | `FSIMG_ERANGE` or `FSIMG_EUNSUPPORTED` as the common policy states |
| Budget, scratch, work or descriptor exhaustion | `FSIMG_ELIMIT` |

Diagnostics quote at most one bounded, escaped name and never directory slack,
free inode bytes or buffer contents.

## Tests

`test/qnx6.c` owns a synthetic fixture generator written from the format
design with its own offset tables, CRC and checksum arithmetic; it shares no
code with `lib/formats/qnx6/`. It builds volumes in memory for both byte
orders and every classic block size, with a chosen snapshot pair, system run,
inode table, directories, long names and files, and mutates them for the
format design's [acceptance matrix](design-qnx6.md#acceptance-and-independent-validation).
`test/qnx6-write.c` creates images through the public API and reads them
back through its own independent checker, written from the format design
like the generator and sharing no code with the driver, as well as through
the reader and verifier. It compares bytes across source order, metadata
cache size, scratch placement and time zone, and scans every unused byte and
pointer slot. Its `--native` mode compares an empty volume with a native
`mkqnx6fs` capture of the same geometry: the headers and allocated blocks
match byte for byte apart from the CRC and sync time. Fault tests inject allocation,
scratch, short-read, short-write, EOF, source-change and cancellation failures
at every stage and check cleanup. A scale test grows `N`, the inode capacity,
one directory's width, a file's depth and a file beyond 4 GiB separately,
recording I/O calls, metadata passes and peak scratch.

`test/qnx6-integration.sh` runs the independent oracles that the build finds
and skips the others explicitly: the Linux kernel reader on a Linux host,
the NFI parser as a comparison only, and the QNX 6.5.0 harness for the native
`chkqnx6fs`, mount, write, sync and remount cycle that gates writer
registration. The research fixture F1 is read from a path given by the
environment and is never redistributed. `fuzz/qnx6.c` feeds the probe, open,
traversal, content and verification paths under finite budgets. Sanitizer
runs follow the build documentation.
