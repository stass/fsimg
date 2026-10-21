# QNX ImageFS driver

## Scope

The [ImageFS format design](design-ifs.md) establishes the on-disk facts,
the closed acceptance tables and the writer profile. This document maps them
onto code: the modules of `lib/formats/ifs/`, the driver state behind the
common reader and writer, the algorithms that derive a hierarchy from a flat
path table within bounded memory, planning and emission, and the tests that
qualify them.

The driver implements the common contracts without exceptions: the
[format descriptor and probe](design-formats.md#format-descriptors-and-profiles),
[prepared directories](design-directories.md),
[logical content](design-content.md), [emission](design-emission.md) and
the [POSIX metadata extension](design-metadata.md). Common code gains no
ImageFS-specific branch. The driver opens an ordinary `fsimg_io_t`: a bare
image file, a slice of one, or the body a `qnxboot` or `hbcifs`
[container](design-layers.md#layer-profiles) has peeled. It knows nothing
about startup headers, chunk framing or the
[codecs](design-codecs.md). The QNX4 driver is the structural template; the
two share no code, since they decode different records.

## Modules

| File | Owns |
| --- | --- |
| `ifs.h` | Private types, constants and prototypes shared by the files below |
| `descriptor.c` | `fsimg_ifs_format`: the `ifs` name, its capability record, the probe and the reader and writer operations |
| `records.c` | Byte-order codecs and decode/encode of the header and of every record kind; pure functions without I/O |
| `volume.c` | Header validation, the mountpoint, and the record-table pass |
| `tree.c` | The path sort, the derived hierarchy, synthesized parents, shadowed duplicates and the child groups |
| `names.c` | Path components and the profile |
| `directory.c` | `prepare`, `replay`, `keys` and `key` |
| `read.c` | `fsimg_ifs_reader_ops`: open, close, resolve and volume information |
| `verify.c` | Full verification: the checksum, the references, the walk |
| `write-ifs.h` | Plan rows and the plan object |
| `write-plan.c` | `validate`, `plan`, `free`: settings and the four passes |
| `write-emit.c` | `emit`: the header, the records, payload copies, the trailer |

`volume.c` is the only place that walks the record table, and `tree.c` the
only place that interprets a path as a hierarchy. Every record is validated
once, in the table pass, before any entry exists; replay re-validates the
one record it reads. The writer never reads an image.

## Codecs

`ifs_u16()`, `ifs_u32()` and their `put` counterparts decode and encode
integers from byte buffers in a byte order passed as a value, so one codec
serves both flags. Decoded records are C structures with `_t` typedefs
holding only validated values:

| Type | Fields |
| --- | --- |
| `ifs_header_t` | byte order, `image_size`, `hdr_dir_size`, `dir_offset`, `boot_ino[4]`, `script_ino`, `chain_paddr`, mount flags, `spare_zero` |
| `ifs_record_t` | length, inode word, kind, mode bits, uid, gid, time, path offset and length within the record, and the kind's payload: data offset and length, or `sym_offset` and `sym_size` with the target's offset within the record |

`ifs_record_decode()` takes the record's bytes and length and applies the
format design's [table rule](design-ifs.md#table-rule),
[prefix](design-ifs.md#record-prefix), [kinds](design-ifs.md#kinds) and
[link layout](design-ifs.md#link-layout): the length floor, the kind's
minimum, the path's NUL inside the record, the zero extended-attribute
offset, the nonzero inode word and its flag nibble, the mode's type and
high bits, the FIFO's zero words and the link's offset, size and NUL. Each
check names its class. Encoding is the exact inverse and fills every
padding byte with zero, so an emitted record never contains an unspecified
byte.

## Probe and open

The probe inspects the fixed prefix of `0x58` bytes. The identity signature
is the seven bytes `imagefs` at offset 0; without it the probe makes no
claim (`FSIMG_EFORMAT`). With it the flags byte must be `0x04` or `0x05`,
else `FSIMG_EUNSUPPORTED`; it selects the byte order of the three sizes,
which must satisfy the [header bounds](design-ifs.md#header-bounds) against
the volume length, else `FSIMG_ECORRUPT`. The probe then claims the image
with `FSIMG_OK`. It performs no I/O and allocates nothing; a prefix shorter
than `0x58` bytes is no claim. Exclusivity rests on the identity. FAT and
exFAT carry a jump at offset 0, never `imagefs`. A QNX6 boot area and a
QNX4 loader block are uninterpreted bytes, and no capture of either begins
with `imagefs`. An ImageFS carries no other format's signature at offset
0, but its file data can hold any bytes, so a crafted image whose data
holds a QNX4 root block at offset 512 draws two claims, as would a QNX4
loader block that begins with `imagefs` and plausible sizes; the common
rule then requires a forced format, and no probe scans for magic.

`open` completes recognition in a fixed order; each step fails before the
next reads:

1. **Header.** Decode the fixed header again under the probe's rules, and
   require zero spare words and `dir_offset` and `hdr_dir_size` that are
   multiples of 4, else `FSIMG_EUNSUPPORTED`. `ifs_header_t` holds the
   decoded fields and the volume length, which the
   [volume-length rule](design-ifs.md#volume-layout) lets exceed
   `image_size` on read; the content domain is
   `[dir_offset, image_size - 4)`, so a longer volume's tail can never be
   delivered.
2. **Mountpoint.** Apply the format design's
   [mountpoint rule](design-ifs.md#mountpoint): one bounded read, the NUL
   search with its two failure classes, the `dir_offset` check, and the
   byte rules that decide whether info reports it. Nothing between the
   NUL and `dir_offset` is read.
3. **Table pass.** Walk the records from `dir_offset` to the terminator as
   the [table pass](#the-table-pass) states, validating each one and
   appending its path and table offset to the path stream.
4. **Hierarchy.** Sort the path stream and derive the
   [child groups](#the-derived-hierarchy). Odd paths and non-directory
   parents fail here.

The reader state then holds the header, the mountpoint when valid, the
counters the table pass collected, the group table, the sealed child
stream, and one window for record reads. No record is resident after open.

## The table pass

`ifs_table_walk()` reads `[dir_offset, hdr_dir_size)` sequentially in
windows and applies the table rule at each position `p`:

* at `hdr_dir_size - 4`, the four bytes must be zero and the walk ends;
* a record's length word is read at `p`; the record `[p, p + length)` must
  lie within `[p, hdr_dir_size - 4)` and hold at least 24 bytes, else
  `FSIMG_ECORRUPT`; a length that is not a multiple of 4 is
  `FSIMG_EUNSUPPORTED`;
* the record is decoded with `ifs_record_decode()`;
* the path is checked: a leading slash, an empty component or a `.` or
  `..` component is `FSIMG_EUNSUPPORTED`, a component outside the profile
  is `FSIMG_EUNSUPPORTED`, and a path over 1024 bytes is
  `FSIMG_EUNSUPPORTED`;
* a regular file's range is checked against the
  [range rule](design-ifs.md#range-rule) with 64-bit arithmetic;
* the record's path bytes, table offset, kind and inode word are appended
  to the path stream, whose declared maximum record length is 1024 bytes
  plus the fixed fields;
* the counters advance: records by kind and by inode flag nibble.

The walk charges one work unit per record and the I/O of each window, and
stops at the first failure. It is the only pass over the table; info reuses
its counters, and replay reads single records by offset.

## The derived hierarchy

`tree.c` turns the sealed path stream into one child group per directory,
in three bounded steps over the metadata store:

1. **Path sort.** The common stable external sort orders the path stream by
   the format design's [component order](design-ifs.md#sorted-paths): bytes
   compared with the separator ranked below every other byte, so the
   comparator is one byte loop. Equal paths keep table order. The result is
   a preorder of the tree with every subtree contiguous.
2. **Child pass.** One sequential read of the sorted run emits a child
   record for every entry, each carrying its parent's node index, its
   component bytes, its kind, and its table offset or a synthesized marker.
   Node indexes are assigned in emission order; the root is node 0. The
   pass keeps the last emitted path, at most 1024 bytes, and an
   ancestor stack holding one `(node index, kind)` per component of the
   last emitted path, the root at depth 0, so its depth is at most 512,
   the most a 1024-byte path holds, and no limit of the session bounds it.
   Every emitted node is pushed, whatever its kind, which is what makes
   the parent-kind check hold by construction. For each record, with
   components `c1 .. ck`:
   * **Root.** A record with the empty path (`k = 0`) is the root
     candidate; one whose kind is not directory is `FSIMG_EUNSUPPORTED`,
     entry or shadowed. When the first sorted record is not the empty
     path, the root is synthesized first, as any missing parent.
   * **Duplicate.** If the next record has the same path, this record is
     shadowed: the shadowed counter advances and nothing is emitted or
     pushed. The last of a run of equal paths is the entry.
   * **Ancestors.** Let `m` be the number of leading components this path
     shares with the last emitted path, which is the stack's path; a
     shadowed record never changes it, so the entry after a shadowed run
     meets the stack as the last emitted record left it. The stack is popped to depth
     `m`, whose top is the path's nearest ancestor that has a node. If
     that node's kind is not directory, this path lies below a file, link
     or FIFO: `FSIMG_EUNSUPPORTED`. Since a shadowed record is never
     pushed, a file record that shadows a directory record is the node its
     descendants meet. Components `c(m+1) .. c(k-1)` name parents the
     table omits: each is synthesized in turn, emitting a directory child
     record with the synthesized marker and pushing its node. The path's
     own record is then emitted and pushed with its kind.
   * **References.** The emitted node's inode word is compared with each
     nonzero `boot_ino[i]` and `script_ino`: a match advances that
     reference's count and records the node's kind. Five references at
     most, so this is constant state, and shadowed records never match.
3. **Group sort and table.** The child stream is sorted by parent index,
   stably, so each directory's children are one contiguous group in
   component order. One sequential pass writes a paged array
   `group[node] = (byte offset, count)` for every directory node, and the
   sorted child stream is sealed and retained.

The tree is a tree by construction: every node has exactly one parent, the
path's, so no directory can be reached twice and no cycle claim is needed.
A child record is bounded, explicitly encoded and re-validated at replay;
synthesized records carry no table offset and decode to the format design's
[implicit-directory values](design-ifs.md#implicit-directories).

Scratch holds the path stream, its sorted copy, the child stream, its
sorted copy and the group table, each proportional to the table's size,
and the sorts hold at most three live stores at a time. A table of
`2^32 - 4` bytes or a million records therefore spills under the scratch
budget and never grows RAM; the ancestor stack and the previous path are
the only resident state of the child pass.

## Directories and names

`prepare` reads the directory's group: `group[node]` gives the offset and
count, and the children are read sequentially from the sealed child stream.
The root is node 0 at level 0; deeper levels use the current entry's
locator, which is its node index. Each child's displayed name is its
component, already validated by the table pass; a synthesized child is
admitted with its fixed metadata. Preparation's cost is the directory's
children, never its subtree.

An admitted record carries the displayed name and a bounded payload: the
kind, the node index, and the table offset or the synthesized marker, each
an explicit little-endian field. `keys` emits one key, the exact component
bytes; `key` validates a query component against the same profile and
returns its bytes. Two children of one directory never share a component,
because equal paths collapsed to one entry in the child pass, so the
namespace is unambiguous by construction; the `ambiguous` text, which names
a duplicate ImageFS path, exists for the common contract and is unreachable.

`replay` re-validates the payload, reads the record again at its table
offset through the window, decodes it with `ifs_record_decode()`, requires
its kind and path length to agree with the payload, and fills the entry:
owner, the twelve permission bits and the modification time as UTC seconds.
A synthesized directory fills the entry from constants. For a regular file,
`replay` builds a one-record extent index over the content domain from the
record's range, so the range is checked again by the index; a zero-length
file builds none. For a symbolic link the index holds one record for the
target bytes inside the record, at `offset + 0x1c + sym_offset`.
`reader->initialized` is the file length or the target length, zero for a
directory or FIFO.

## Content

`resolve` is a lookup in the current entry's index, which holds exactly one
extent. Sequential reads advance through it in batches; the index is
released when the walker advances. No content read rereads the table, and
no byte reaches delivery that the table pass and replay did not validate.
Shared ranges cost each entry its own reads; the default extraction output
ceiling bounds a heavily shared image as it bounds any other.

## Volume information

`info` reports the format `ifs`; the profile `little-endian` or
`big-endian`, followed by `bootable` when any reference is nonzero; the
geometry as `volume_bytes = image_size` with a 1-byte sector and allocation
unit, since the format addresses bytes; the mountpoint, when valid, in the
volume information's `mountpoint` field under `FSIMG_VOLUME_MOUNTPOINT`;
no label; the time basis UTC; and `FSIMG_VOLUME_FREE` only after a
completed verification, with zero free bytes.

The boot-specific state and the counters open collects go through the
volume information's [detail table](design-api.md#reader), in this order:
`mount_flags`, `boot_ino_0` to `boot_ino_3`, `script_ino`, `chain_paddr`,
`flags_0`, `flags_8`, `flags_c`, `flags_e`, `records`,
`implicit_directories` and `shadowed`, fourteen of the sixteen the table
holds. Every value is known when open returns, so info discloses no scan.

## Verification

Full verification needs no ownership stream: an ImageFS has no allocation
to reconcile, and the table pass validated every record at open. It runs:

* **Length.** The [volume-length rule](design-ifs.md#volume-layout):
  the volume length must equal `image_size`, else `FSIMG_ECORRUPT` before
  any work. A boot image whose `imagefs_size` disagrees with its body, or a
  bare image with a tail, fails here.
* **Checksum.** One sequential pass over `[0, image_size)` in windows sums
  the 32-bit words in the image's byte order; a nonzero result is
  `FSIMG_ECORRUPT`. The pass charges one work unit per window.
* **References.** For each nonzero `boot_ino[i]` and `script_ino`, the
  child pass's match count over delivered entries must be exactly 1, else
  `FSIMG_ECORRUPT`, and the match must be a regular file, else
  `FSIMG_EUNSUPPORTED`, as the
  [reference rule](design-ifs.md#boot-and-script-references) states.
* **Walk.** The common walker runs once over the derived tree, replaying
  every directory, so every delivered record is decoded a second time and
  every index is built. Payload read-through, when requested, reads each
  file's and link's bytes through common delivery.
* **Shadowed records.** The count is reported as the `shadowed` detail of
  the volume information, and the CLI's `W` result prints it. A shadowed
  record is validated, so it does not make the verification incomplete:
  every H3 boot image carries them and verifies.

The report counts files, directories including the root and the synthesized
ones, logical bytes and bytes read; allocated units are the image's words
and free, bad and padding counts are zero. An invalid mountpoint makes
verification incomplete with `FSIMG_EUNSUPPORTED` before any work, as an
invalid QNX4 label does. An unsupported record, path or name anywhere has
already failed open, so the report is never marked complete after skipping
a record.

## Writer settings

`validate` accepts the common options with these rules: `sector_bytes` zero
or 1; `allocation_unit_bytes` zero or 1; `fat_root_entries` zero; `label`
NULL, which the common capability check already enforces with
`FSIMG_EUNSUPPORTED`, since `max_label_bytes` is 0; `epoch` within
`[0, 4294967295]` in reproducible mode; `volume_bytes` and `min_free_bytes`
zero, any other value `FSIMG_EINVAL`, because the image has no free space
and its size is derived; `volume_serial` ignored, as the profile stores
none. The capability record's `write_settings` names the byte order, which
QNX6 also stores, the mountpoint and the boot settings, typed fields of
`fsimg_write_options_t` that every other format requires at their
defaults:

| Field | CLI setting | Meaning |
| --- | --- | --- |
| `byte_order` | `endian=little` or `big` | Default little; selects the flags byte and the byte order of every field |
| `mountpoint` | `mountpoint=<path>` | `FSIMG_SETTING_MOUNTPOINT`; copied; NULL selects `/`; the empty string is stored as an empty mountpoint; at most 1024 bytes under the profile's byte rules with slashes allowed, else `FSIMG_EINVAL` |
| `boot_programs[4]`, `script` | `boot1=` to `boot4=`, `script=` | `FSIMG_SETTING_BOOT`; copied image paths; governed by the [boot settings rule](design-ifs.md#boot-settings): a path naming no catalog file, or an entry of another kind, is `FSIMG_EINVAL`, and a slot the closed flag table lacks is `FSIMG_EUNSUPPORTED`, refused by `validate` before any collection; the empty table makes that every slot |

The keys are bare, as the [layer design](design-layers.md#layer-settings)
spells every filesystem setting; the epoch is the common `-e`. The format
declares no partition type, so a scheme in an `ifs` create stack is
`FSIMG_EINVAL`.

## Planning

The plan object embeds `fsimg_plan_t`, the byte order, the mountpoint and
`dir_offset`, `hdr_dir_size`, the data end, `image_size`, the fallback
time, the loss count, and one paged array of rows keyed by catalog ID with
row 0 for the root. A row holds the kind, the record's table offset and
length, the data offset and length for a file, the target length for a
link, and the fields already encoded under the metadata policy: uid, gid,
mode and time. The inode word is `ID + 1`, a pure function of the row's
key, so it is not stored. Rows are the only per-entry state; no vector
grows with the image.

Planning runs a fixed sequence of passes over the frozen catalog:

1. **Entries.** For each catalog ID in order: check each component against
   the profile, refusing a violation with `FSIMG_EINVAL`, and the path
   against the 1024-byte limit, failing a longer one as
   `FSIMG_EUNSUPPORTED`, an entry the format cannot represent; validate
   kind and metadata under the
   [metadata policy](design-metadata.md#writer-policy) with the limits of
   the writer profile, where a time outside 0 through 4294967295 takes the
   fallback as a counted optional loss that fails only when the caller
   forbids losses, and a file above `2^32 - 1` bytes fails as
   `FSIMG_EUNSUPPORTED`; accumulate `initialized_bytes`. A time is stored
   as unsigned UTC seconds; an absent one takes the fallback, and in
   reproducible mode every time is the epoch, a present time that differs
   from it being a counted loss. Sibling collisions are exact byte
   collisions, which catalog preparation has already rejected. Each boot
   setting is resolved to its catalog ID and checked under the boot
   settings rule, so a set slot fails here.
2. **Records.** `dir_offset` is `0x58 + mountpoint length + 1` rounded up
   to a multiple of 4. The root record is `0x18 + 1` bytes rounded up; each
   entry's record length is its kind's minimum with the full image path as
   `L` and the target length as `sym_size`, rounded up to a multiple of 4.
   Table offsets are the running sum from `dir_offset`; `hdr_dir_size` is
   the sum plus the terminator.
3. **Layout.** Each regular file in ID order takes the next multiple of 4
   from `hdr_dir_size` as its data offset, zero-length files included; the
   data end rounded up to a multiple of 4, plus the trailer, is
   `image_size`. Every sum is 64-bit; an `image_size` above `2^32 - 4` is
   `FSIMG_ERANGE`.
4. **Summary.** Fill `fsimg_plan_info_t`: `volume_bytes = image_size`,
   `metadata_bytes = hdr_dir_size + 4`, allocated bytes the image, free
   bytes zero, `initialized_bytes`, a 1-byte sector, and the loss count.

Every value in the plan derives from the catalog, the options and these
rules, so byte-identical output does not depend on source order, budgets,
sort fan-in, scratch placement, locale or time zone.

## Emission

The common emission object has already zeroed the sink. The driver keeps a
running 32-bit word sum in the plan's byte order and emits:

1. The header from the plan, the mountpoint and its NUL; the padding to
   `dir_offset` is already zero.
2. For each row in ID order: the record, encoded by `records.c` with zero
   padding, at its table offset; then, for a file with content, the payload
   copy into its one region `(data offset, length)`. The terminator is
   already zero.
3. The trailer at `image_size - 4`: the additive inverse of the running
   sum.

The running sum is one accumulator over byte lanes: a byte at volume
offset `o` adds its value shifted into lane `o mod 4` of a word in the
plan's byte order. Every metadata write feeds its bytes with their
offsets; every payload copy passes the
[copy observer](design-emission.md#emission-object), which reports each
batch's volume offset, so the lane of every byte follows from its offset
alone, whatever the batch sizes and whether the copy skips zero ranges.
Bytes the driver never feeds are the sink's zeros and add nothing, so the
accumulator equals the word sum of the image on the sink. The driver therefore sums the image without
reading the sink or a source, and common code holds no checksum. Every
emitted byte comes from a row, the layout or a profile constant.

## Resource bounds

| Operation | Work and residency |
| --- | --- |
| Probe and open | One `0x58`-byte read, at most 1025 mountpoint bytes, one sequential pass over the table in windows, two external sorts and two sequential passes; scratch for the path and child streams, their sorted copies and the group table, together a small multiple of the table's size |
| Listing | Per directory: one sequential read of its group and the common key sort; per entry: its record read again at replay. No payload is read |
| Content | One extent lookup per request after the index is built at replay |
| Verification | One sequential pass over the image for the checksum and one walk; no sort of its own |
| Planning | One pass over the catalog and two running sums; rows in one paged array |
| Emission | Payload bytes plus metadata proportional to entries and names |

Only the header, the mountpoint, the window, the current record, the
previous path and the ancestor stack of the child pass are resident per
reader, beside the common walk's charged stack of `max_depth + 1` levels
and its path buffer; everything else is charged scratch. Every loop charges
the session and observes cancellation.

## Failure classification

Reader:

| Condition | Status |
| --- | --- |
| No `imagefs` at offset 0; a prefix shorter than `0x58` bytes | `FSIMG_EFORMAT` |
| Recognized but unobserved: a flags byte other than `0x04` or `0x05`; a nonzero spare word; unaligned `dir_offset` or `hdr_dir_size`; a mountpoint over 1024 bytes or a `dir_offset` that is not its NUL rounded up to 4; a record length not a multiple of 4; a zero inode word; a nonzero extended-attribute offset; an unknown flag nibble, type or high mode bits; a nonzero FIFO word; a link layout other than the one tabled, or a target of 0 or 1024 bytes; a path over 1024 bytes, absolute, with an empty, `.` or `..` component, below a non-directory, or with a component outside the profile; a non-directory record with the empty path; a reference to a non-file; an invalid mountpoint under `verify` | `FSIMG_EUNSUPPORTED` |
| A header bound violated; no mountpoint NUL before `dir_offset`; a record crossing the table end, a zero length, a length below 24 or below the kind's minimum, a path without a NUL; a nonzero terminator; a range outside `[hdr_dir_size, image_size - 4)` or overflowing; a reference naming no delivered entry or several; a volume longer than `image_size` or a nonzero checksum under `verify` | `FSIMG_ECORRUPT` |
| Unrepresentable selection component | `FSIMG_EINVAL` |
| Budget, scratch, work or descriptor exhaustion | `FSIMG_ELIMIT` |

Writer:

| Condition | Status |
| --- | --- |
| A setting outside its type; a nonzero `volume_bytes` or `min_free_bytes`; a mountpoint over 1024 bytes or outside the byte rules; a boot setting naming no catalog file or an entry of another kind; a component outside the profile; a scheme in the create stack | `FSIMG_EINVAL` |
| A label, from the common capability check; a boot setting for a slot the flag table lacks; a path over 1024 bytes; a file above `2^32 - 1` bytes; a kind the profile lacks; a time loss when the caller forbids losses | `FSIMG_EUNSUPPORTED` |
| An image above `2^32 - 4` bytes; a non-reproducible fallback time outside `[0, 4294967295]` | `FSIMG_ERANGE` |
| Budget, scratch, work or descriptor exhaustion | `FSIMG_ELIMIT` |

Diagnostics quote at most one bounded, escaped name and never record bytes,
padding or buffer contents.

## Tests

`test/ifs.c` owns a synthetic fixture generator written from the format
design with its own offset tables; it shares no code with
`lib/formats/ifs/`. It builds images in memory from the header through the
mountpoint, every record kind, implicit parents, duplicates, shared ranges,
references, the terminator and the trailer, in both byte orders, and
mutates them for the format design's
[acceptance matrix](design-ifs.md#acceptance-matrix). Every valid fixture
has a paired mutation that must fail with its class, which proves the check
ran. Reading tests compare `i`, `t`, `x -O` and `x` on the
fixtures and on every native capture found through `FSIMG_IFS_NATIVE`, the
captures directory, using the manifest of names, sizes and checksums each
capture records; the `dumpifs -u` copies and the S1 and S2 bodies under
`FSIMG_IFS_IMAGES`, the directory holding the licensed H3 and X1 images,
are read as slices at the startup section's end.

`test/ifs-write.c` creates images through the public API and reads them
back through its own independent checker, written from the format design
like the generator, as well as through the reader and verifier. It compares
bytes across source order, metadata cache size, scratch placement and time
zone, and scans every padding byte for poison. Its `--native` mode compares
a writer image with the E1 capture of the same tree field by field, apart
from the free choices the writer profile lists: inode numbering, record
order, data order and alignment, and the explicit directory records. Its
`--create` mode builds the native qualification images and their
manifests. Fault tests inject allocation, scratch, short-read, short-write,
EOF, source-change and cancellation failures at every stage and check
cleanup.

Scale tests record I/O calls, metadata passes, peak scratch and RSS. The
reader's grow the image to `2^32 - 4` bytes on a sparse fixture, the table
to a million records, which must spill to scratch, one directory's width to
a million files, one path to 1024 bytes, and depth to 512, the most a path
holds. The writer's emit, into a sink that holds only nonzero pages, an
image at `2^32 - 4` bytes and a catalog that spills to scratch, and plan an
image on both sides of the limit.

`test/ifs-integration.sh` runs the independent oracles the build finds and
skips the others explicitly: `lclevy/dumpifs` and `qnxprobe` as comparisons
when their paths are given; the native captures under `FSIMG_IFS_NATIVE`;
the S1 and S2 bodies under `FSIMG_IFS_IMAGES` with slice bounds from the
environment, running only `info` and `verify` and printing counts, never a
member name; and the QNX 6.5.0 harness for the native `dumpifs` list, check
and extract cycle. `fuzz/ifs.c` feeds the probe, open, traversal, content
and verification paths under finite budgets with seeds generated at run
time. Sanitizer runs follow the build documentation.

## Open decisions

* **Directory times.** `mkifs` writes time 0 on every directory record;
  the writer stores the catalog's time, so a tree with directory times
  differs from `mkifs` output outside reproducible mode. The alternative is
  to zero directory times to match. Recommendation: store the catalog's
  time, since one rule covers every kind, `dumpifs` lists whatever is
  stored, and `-R` already makes both producers agree; the native
  qualification cycle decides whether `dumpifs -x` or a mount accepts it.
