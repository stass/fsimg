# QNX ImageFS reader and writer

## Scope and evidence

`ifs` denotes the QNX image filesystem, ImageFS: the flat, read-only volume
that `mkifs` builds and that QNX Neutrino's `procnto` mounts from the boot
image. It is one header, one table of records that each carry a full
relative path, the file data, and a four-byte additive checksum. There are
no allocation structures, no directory storage and no in-place
modification. The reader consumes one bare ImageFS as a volume slice; the
writer constructs a new standalone ImageFS that native QNX 6.5.0's
`dumpifs` lists, checks and extracts. Neither modifies an existing
image. Readers and writers remain independently registered capabilities.

The compatibility targets are the ImageFS bodies of the Porsche PCM 3.1
update disc (H3): the eight SH-4 boot images and the `hbcifs`-wrapped
application image, plus the three x86 boot images of the QNX 6.5.0 live CD
(X1) and the output of 6.5.0's native `mkifs`. The
[driver design](design-ifs-driver.md) maps the rules below onto the modules
of `lib/formats/ifs/`; the [research record](../docs/ifs-research.md) holds
the native evidence, and the [IFS plan](../docs/ifs-plan.md) orders the work
and records the owner's decisions. The research draft that supplied the
questions is not a source of rules.

Everything that surrounds an ImageFS belongs to other owners and is only
referenced here. The startup header, boot prefix, startup section, chunk
framing, startup and stored checksums and the boot-image wrap belong to the
`qnxboot` container; the Harman-Becker header and its unframed stream belong
to the `hbcifs` container; both are
[layer profiles](design-layers.md#layer-profiles). The LZO1X and NRV2B
decoders and the LZO1X encoder are the
[project-owned codecs](design-codecs.md). The driver sees an ordinary
`fsimg_io_t` holding a bare ImageFS, whether the image was a file or a
container's peeled body.

This document is an original description of disk-format facts and fsimg
requirements. It contains no imported source, copied declarations or copied
documentation prose. `lclevy/dumpifs`, `qnxprobe` and `qnx_dumpers` are
comparison tools, never code donors.

Evidence has three meanings here:

* **Established layout**: byte positions and meanings corroborated by vendor
  documentation and by the native captures. It establishes interpretation,
  not that every combination is accepted by QNX.
* **Observation**: a property measured on a named capture or survey.
  Observations fix the closed acceptance tables.
* **fsimg policy**: a validation, capability or construction rule chosen
  here. It can be stricter than QNX without redefining the format.

Acceptance tables are closed: a value in an observed tuple is accepted, an
unobserved value is `FSIMG_EUNSUPPORTED`, and a value that contradicts the
format is `FSIMG_ECORRUPT`. The tables widen only from evidence recorded in
the research record. Source keys in brackets are listed at the end.

## Shared owners and representation

The resource context owns memory, scratch, work accounting and cancellation.
Bounded I/O owns every volume-relative byte access. The common source catalog
owns input identities and repeatable reads; the output coordinator owns
publication. The common contracts cover ImageFS without exceptions:
[format descriptors and probes](design-formats.md#format-descriptors-and-profiles),
[prepared directories](design-directories.md),
[logical content and the extent index](design-content.md),
[emission](design-emission.md), [sizing](design-sizing.md) and
[POSIX metadata](design-metadata.md). The entry model carries owner, mode,
the modification time, symbolic links and FIFOs, so common code gains only
the descriptor registration, the writer's mountpoint and boot settings, the
volume information's mountpoint field and detail table, and the emission
copy's observer, each specified in the [C API](design-api.md),
[metadata](design-metadata.md#capability-records) and
[emission](design-emission.md#source-copy) designs and used as the
[driver design](design-ifs-driver.md) states.

Every disk integer is a 32-bit or 16-bit unsigned value in the byte order the
header's flags byte selects, decoded from bytes; native structure layout,
host alignment and host `stat` constants are not disk codecs. Offsets are
bytes from the start of the image, `B`. Offsets and sums use checked
`uint64_t` arithmetic; a value that fails a check is corrupt, never clamped.
All project-defined C types have `_t` typedefs.

## Volume layout

An ImageFS is `image_size` bytes, laid out in this order: [D], [X1]

| Range | Content |
| --- | --- |
| `[0, 0x58)` | Fixed header |
| `[0x58, dir_offset)` | The mountpoint, its NUL, and padding |
| `[dir_offset, hdr_dir_size - 4)` | The record table |
| `[hdr_dir_size - 4, hdr_dir_size)` | The table terminator: four zero bytes |
| `[hdr_dir_size, image_size - 4)` | File data |
| `[image_size - 4, image_size)` | The checksum trailer |

Native nonbootable output ends exactly at `image_size`, and a container's
peeled body is exactly its declared `imagefs_size` bytes, so a well-formed
volume is exactly `image_size` long; the tie between the boot image's
`imagefs_size` and the body's `image_size` is therefore this driver's
check, not the container's. Reading tolerates a longer volume, such as a
slice cut past the image: bytes past
`image_size` are outside the volume, never interpreted and never
delivered, and info reports `image_size` as the volume's bytes. Full
verification requires the volume length to equal `image_size`; a longer
volume contradicts the image's own size and is `FSIMG_ECORRUPT`. A shorter
volume fails the header bounds. This is the one statement of the
volume-length rule; the driver cites it. [X1], [S1], [S2]

### Header

| Offset | Bytes | Field |
| --- | --- | --- |
| `0x00` | 7 | `imagefs`, the identity signature; no terminator |
| `0x07` | 1 | Flags: `0x04` little-endian, `0x05` big-endian |
| `0x08` | 4 | `image_size`: the whole image, trailer included |
| `0x0c` | 4 | `hdr_dir_size`: the end of the record table, terminator included |
| `0x10` | 4 | `dir_offset`: the first record |
| `0x14` | 16 | `boot_ino[4]`: boot-program references, zero when unused |
| `0x24` | 4 | `script_ino`: the startup-script reference, zero when unused |
| `0x28` | 4 | `chain_paddr`: a physical address, zero when unused |
| `0x2c` | 40 | Ten spare words, zero |
| `0x54` | 4 | Mount flags |
| `0x58` | | The mountpoint, NUL-terminated |

The flags byte is decoded before any multi-byte field, since it selects
their byte order. Only `0x04` and `0x05` occur natively; `+bigendian` makes
`mkifs` write `0x05` and a big-endian body, so both orders are read. Any
other value is `FSIMG_EUNSUPPORTED`. The spare words are zero in every
image; a nonzero spare word is `FSIMG_EUNSUPPORTED`. Mount flags and
`chain_paddr` are zero in every image; they are reported, not interpreted,
and any value is accepted. [D], [X1], [S1], [S2]

### Header bounds

These relations hold in every image and are checked by the probe from the
fixed header alone; a violation is `FSIMG_ECORRUPT`: [X1], [S1], [S2]

* `0x58 <= dir_offset`, `dir_offset + 4 <= hdr_dir_size`, since the table
  holds at least its terminator, and `hdr_dir_size + 4 <= image_size`,
  computed in 64 bits so that no bound underflows;
* `image_size` is a multiple of 4, so the image is whole words for the
  checksum;
* `image_size` is at most the slice length.

`dir_offset` and `hdr_dir_size` are multiples of 4 in every image, because
every record and the terminator are whole words; an unaligned value is
`FSIMG_EUNSUPPORTED`. The smallest image is a header, a one-byte
mountpoint, the root record, the terminator and the trailer. [X1]

### Mountpoint

The mountpoint is the NUL-terminated string at `0x58`. `mkifs`'s buildfile
attribute `mount=` sets it, and an empty value stores an empty mountpoint,
as H3's application image has. Without `mount=`, `mkifs` stores `/` in a
bootable image, the value of every boot image surveyed, and an empty
mountpoint in a nonbootable one. In every image
`dir_offset` is the position after the NUL rounded up to a multiple of 4:
`0x5c` for `/`, `0x60` for `/foo`. The bytes between the NUL and
`dir_offset`, at most three, are padding: zero in 6.5.0 output, but one to
three nonzero bytes in every H3 image, so they are never interpreted or
checked. [X1], [S1], [S2]

A mountpoint is at most 1024 bytes before its NUL, the one path limit this
design applies to every stored path string: QNX's path maximum, which
`mkifs` enforces on record paths by refusing 1025 bytes. The reader reads
the smaller of `dir_offset - 0x58` and 1025 bytes from `0x58` and finds the
first NUL: none before `dir_offset` is `FSIMG_ECORRUPT`; none within 1025
bytes when `dir_offset` lies further is `FSIMG_EUNSUPPORTED`; and a NUL
whose position rounded up to a multiple of 4 is not `dir_offset` is
`FSIMG_EUNSUPPORTED`, an unobserved header shape. The reader reports the
mountpoint through the volume information's mountpoint field when its
bytes satisfy the name profile's byte rules below with slashes allowed; an
invalid mountpoint never blocks reading: info omits it, and full
verification reports the volume incomplete with `FSIMG_EUNSUPPORTED`. This
is the one statement of the mountpoint rule; the driver cites it. [X1]

Without a mountpoint setting the writer stores `/`, not the empty
mountpoint a nonbootable `mkifs` image gets without `mount=`. `/` is the
value of every bootable `mkifs` image and of every H3 and X1 boot image,
so one default serves a standalone image and the body an `ifsboot` wrap
packages, and `dumpifs` accepts both values; `mountpoint=` with an empty
value writes the empty mountpoint. [X1], [S1], [S2]

## The record table

### Table rule

The records tile `[dir_offset, hdr_dir_size - 4)` exactly, each starting
where the previous one ends, and the four bytes at `hdr_dir_size - 4` are
zero. This holds in every image. Reading proceeds from `dir_offset`: [X1],
[S1], [S2]

* at `hdr_dir_size - 4` the four bytes must be zero, and the table ends;
* otherwise a record starts here: its length must be at least 24, at least
  its kind's minimum below, and must not reach past `hdr_dir_size - 4`.

A record that crosses the end, a zero length before the end, a length below
24 or below the kind's minimum, a path without a NUL inside the record, or
a nonzero terminator is `FSIMG_ECORRUPT`. A record with a zero inode word
is `FSIMG_EUNSUPPORTED`: no native image has one, and its meaning is
unverified. Every native record length is a multiple
of 4; another length is `FSIMG_EUNSUPPORTED`. The bytes of a record after
its payload's final NUL are padding: zero in 6.5.0 output, nonzero in the
root record of every H3 image, so never interpreted or checked. An empty
table, with the terminator at `dir_offset`, is accepted; its root is
synthesized under the [hierarchy rules](#root). [X1], [S1]

### Record prefix

Every record starts with 24 bytes: [D], [X1]

| Offset | Bytes | Field |
| --- | --- | --- |
| `0x00` | 2 | Record length, padding included |
| `0x02` | 2 | Extended-attribute offset; zero |
| `0x04` | 4 | Inode word: flag nibble in the top four bits, the inode number below |
| `0x08` | 4 | Mode: the type in `0170000`, permissions in `07777` |
| `0x0c` | 4 | Group identifier |
| `0x10` | 4 | User identifier |
| `0x14` | 4 | Modification time, unsigned Unix seconds |

No native image uses extended attributes, and their schema is unknown; a
nonzero offset is `FSIMG_EUNSUPPORTED`. The type selects the kind below;
the permission bits are the entry's mode. Mode bits above `0177777` are
unobserved and `FSIMG_EUNSUPPORTED`. Owner, group and time are reported as
stored across their full 32-bit ranges: `mkifs` accepts every uid, gid and
permission value, and times come from the host file. [X1]

### Kinds

| Type | Kind | Payload after the prefix | Path at | Minimum length |
| --- | --- | --- | --- | --- |
| `040000` | Directory | none | `0x18` | `0x18 + L + 1` |
| `0100000` | Regular file | u32 data offset, u32 length | `0x20` | `0x20 + L + 1` |
| `0120000` | Symbolic link | u16 `sym_offset`, u16 `sym_size` | `0x1c` | `0x1c + L + 1 + sym_size + 1` |
| `010000` | FIFO | u32 zero, u32 zero | `0x20` | `0x20 + L + 1` |

`L` is the path length, the bytes before the first NUL at or after the path
offset; the NUL must lie inside the record. The directory record carries
the path alone; `mkifs` writes it for every `type=dir` entry with mode
`040755` and time 0, `dperms=` setting the mode. A FIFO's two words are
zero in every native FIFO; a nonzero word is `FSIMG_EUNSUPPORTED`. Devices,
named specials and sockets cannot be produced by 6.5.0's `mkifs`
(`type=chr` is refused), so their types, and every other type value, are
`FSIMG_EUNSUPPORTED`. [D], [X1], [S1]

### Link layout

In every symbolic link of every image the target starts at
`path + sym_offset` with `sym_offset = L + 1`, that is, right after the
path's NUL; it is `sym_size` bytes, followed by a NUL, then padding to the
record's end. Relative, absolute, dangling, `..` and non-ASCII targets all
follow it. The reader requires exactly this relationship, so no second
candidate layout is ever tried: `sym_offset` other than
`L + 1`, a NUL within the `sym_size` bytes, since a target [holds no
NUL](design-metadata.md), or a NUL missing after them is
`FSIMG_EUNSUPPORTED`. A record too short for the target and NUL it declares
is below the kind's minimum, which the table rule makes `FSIMG_ECORRUPT`. The target is content, delivered
as bytes, never resolved. `mkifs` accepts targets of up to 1024 bytes and
refuses 4000, so the format's link-target limit, the capability record's
`max_link_target_bytes`, is 1024 bytes, which is also the entry model's
ceiling; `sym_size` 0, or above 1024, is `FSIMG_EUNSUPPORTED`. [X1], [S1]

### Inode word

The top four bits of the inode word are flags the loader interprets for
processed executables; the remaining 28 bits are the inode number, which
`mkifs` assigns from 1 for the root and from 2 in buildfile order. The
nibble takes the values `0x0`, `0x8`, `0xc` and `0xe` natively, on H3
files that `mkifs` processed as ELF; any other nibble is
`FSIMG_EUNSUPPORTED`. The flags are counted for info and never interpreted
for reading: a flagged member is an ordinary file whose bytes are delivered
as stored. The inode number is never an identity: hard links are not
inferred from repeated numbers or shared ranges, and every record is its
own entry. [D], [X1], [S1]

### Boot and script references

`boot_ino[]` and `script_ino` are inode words. In every boot image
`boot_ino[0]` is `0xe0000002` and `script_ino` is 3; each names exactly one
regular-file record by its full 32-bit inode word, flag nibble included. A
nonzero reference is checked by verification under that rule, against the
nodes of the derived tree, entries omitted for their names included, since
a [shadowed record](#duplicates) has no entry and the loader could not open
it by name: a reference that names no entry, or more than one, is
`FSIMG_ECORRUPT`, and a reference whose only match is a shadowed record
names no entry; one that names an entry of another kind is
`FSIMG_EUNSUPPORTED`. A shadowed record that carries the word of a
reference naming exactly one entry does not fail it: info counts the
reference in its `shadowed_references` detail, because a startup that
found references by scanning the table could load that record instead.
The referenced members, `.script`
and the boot programs, are ordinary file entries delivered as bytes.
Nonbootable images carry zeros, which are never checked. [S1], [S2], [X1]

## Data ranges

### Range rule

Every regular file's data lies in `[hdr_dir_size, image_size - 4)`: the
start is at least `hdr_dir_size`, and `start + length`, computed in 64 bits,
is at most `image_size - 4`. A range outside that, into the header, the
table, the trailer or past the image, is `FSIMG_ECORRUPT`. Ranges may
overlap each other: `mkifs` stores identical contents twice and no native
range overlaps, but a producer may share bytes, and each record remains its
own entry with its own content. A zero-length file's offset is not
interpreted; `mkifs` writes the next data offset there. Data is packed
without alignment natively (offsets such as `0xc7`) unless `+page_align`
places a member on a 4096-byte boundary; flagged members start on 4-byte
boundaries in H3. The gaps between data are zero in every image but are
never checked. [X1], [S1], [S2]

Content is delivered as one extent per file through the common extent
index, over the content domain `[dir_offset, image_size - 4)`, which also
holds the symbolic-link targets inside their records. The header and the
trailer therefore cannot be delivered as content under any driver defect.

## The checksum

The trailer makes the sum of all 32-bit words of `[0, image_size)`, in the
image's byte order and including the trailer itself, zero modulo `2^32`.
Every native image satisfies it: the trailer is the additive inverse of the
sum of the preceding words. Verification computes it in one sequential pass;
a nonzero sum is `FSIMG_ECORRUPT`. Open and listing do not compute it, since
it costs the whole image. `dumpifs` checks neither this checksum nor the
boot image's, so there is no native oracle for it; the writer's own trailer
is checked by the reader and by an independent checker. [D], [X1], [E10]

## Hierarchy

### Sorted paths

The table is a flat list of full relative paths with no directory storage.
The hierarchy is derived from the paths: open makes one pass over the table,
putting each record's path and byte offset into the metadata store, and the
common stable external sort orders them by path components. Two paths
compare component by component, each component as bytes; at the first
differing component the shorter or lower bytes sort first, and a path that
is a prefix of another sorts first. This is byte order with the separator
below every other byte. The sorted run is a preorder: every directory
precedes its descendants and each subtree is contiguous, so one pass over
it derives every directory's children, synthesizing the parents the table
omits. Nothing holds the table in RAM, and the order of children is the
sorted order. [X1], [S1]

### Implicit directories

`mkifs` writes a directory record only for a `type=dir` entry; every other
parent is implicit. H3's boot images have one directory record and 16 to 21
implicit directories, X1's one and two. A missing parent is synthesized as a
directory with mode `040755`, uid 0, gid 0 and time 0, the values `mkifs`
gives an explicit directory record. No native reader exposes an implicit
parent's metadata (there is no mount on 6.5.0), so this is fsimg policy
named by observation of explicit records. Synthesized directories are
counted by info. [X1], [S1], [S2]

### Root

The root is the empty path. The first native record is always the root: a
directory with the empty path, inode 1 and mode `040755`; an empty buildfile
produces only it. The reader does not require it: a table without an
empty-path record gets its root synthesized exactly as any other missing
parent, which also covers the empty table. A record with the empty path and
a kind other than directory is `FSIMG_EUNSUPPORTED`, whether it is the
entry or shadowed: the decision is made on the record, and a volume whose
root is not a directory has no tree to deliver, since every other path
would lie [below a non-directory](#paths-below-a-non-directory). The root
is never delivered as an entry, so a root record contributes only its kind
and its inode word, which a boot or script reference may name. [X1]

### Duplicates

`mkifs` writes two records with one path when a buildfile names it twice;
every H3 boot image has one to three paths stored as a link record followed
by a file record, and `dumpifs -x` leaves the later record's bytes. A
duplicate appears as adjacent equal keys in the sorted run; the stable sort
keeps table order among them, so the last record in table order is the
entry and the earlier ones are shadowed. A shadowed record is validated
like any other, counted by info, reported by verification, and never
delivered: it has no entry, no key and no content, and it is not a parent.
A duplicate root follows the same rule. A record below a non-directory is
[shadowed](#paths-below-a-non-directory) the same way. [X1], [S1]

### Paths below a non-directory

`mkifs` accepts a path below a regular file (`d` and `d/x`) or a symbolic
link, and its table order puts such a record before or after the
non-directory's own record depending on the buildfile; six of the eight H3
boot images hold one top-level link with two link records below it.
`procnto` shadows them: booted from such an image, it lists the link
alone, resolves every path below it through the link's target, and never
shows the records stored below it, in every table order tested.
`dumpifs -v` lists them as table rows, and `dumpifs -x` fails on a file
below a file.

The record delivered at a path decides:

* When the last record with a path, the entry, is a regular file,
  symbolic link or FIFO, every record whose path lies below that path is
  shadowed exactly as an earlier duplicate is: validated, counted by info
  in the same `shadowed` count, reported by verification, matched by no
  reference, and never delivered. No parent is synthesized for it, so a
  shadowed subtree adds no implicit directory, and nothing below it is a
  parent. The table order does not matter.
* A shadowed duplicate never decides anything. A directory record followed
  by a link record with the same path leaves the link as the entry and the
  directory and every record below it shadowed; a link record followed by
  a directory record leaves the directory as the entry, a parent like any
  other, with only the link shadowed.
* Shadowing is a property of the derived tree and comes before the
  [profile](#names-and-the-profile): a record below a non-directory is
  shadowed, not omitted, even when the non-directory or an ancestor lies
  outside the profile.

For symbolic links the rule is the observed `procnto` behavior. For files
and FIFOs it is fsimg policy, one rule for every non-directory, since no
path below a file or a FIFO names anything a POSIX lookup could reach.
[E12], [X1], [S1]

### Odd paths

`mkifs` strips a leading `./` and leading slashes but stores `x/../y` and
`emp//ty` verbatim; `dumpifs -x` fails on the empty component. fsimg does
not represent such paths: a stored path that is absolute, has an empty
component, or has a `.` or `..` component makes the image
`FSIMG_EUNSUPPORTED`, not corrupt, because the producer writes them. The
empty path is the root, not an empty component. These cases are found in
the table pass at open. [X1]

## Names and the profile

Path components are compared byte for byte and are case-sensitive, under the
one name profile both QNX formats share: strict UTF-8, no NUL, slash, control
byte or `0x7f`, and neither `.` nor `..`. `mkifs` stores names as given,
UTF-8 and Latin-1 bytes alike, and refuses a space only in its buildfile
parser, so a natively legal component outside the profile is omitted under
the [cross-reader rule](architecture.md#names-and-metadata), as for QNX4
and QNX6. The node that owns the component, explicit or
[implicit](#implicit-directories), is validated like any other and is
neither listed nor selectable. Every record below it is omitted with it,
and every record that shares its path is shadowed or omitted with it.
Extraction of a scope holding the node needs the skip option. The omission
is counted once, as the `omitted` detail of info and as `omitted_entries`
in the operation's result. Verification still checks the omitted records.
A path is at most 1024 bytes: `mkifs` accepts 1024 and refuses 1025, one
component may use the whole length, and depth 200 is accepted, so
depth is bounded by the length alone, at 512 components. A longer path is
`FSIMG_EUNSUPPORTED`. Every name in H3 and X1 is valid UTF-8. The writer
never truncates: a name the profile cannot store fails planning. [X1], [S1],
[S2]

## Reader profile

| Property | Accepted |
| --- | --- |
| Flags | `0x04`, `0x05` |
| Header bounds | the relations above; aligned `dir_offset` and `hdr_dir_size` |
| Mountpoint | 0 to 1024 bytes before its NUL, `dir_offset` at the NUL rounded up to 4, under the byte rules for reporting |
| Spare words | zero |
| Mount flags, `chain_paddr` | any, reported |
| Records | the table rule; lengths multiples of 4; extended-attribute offset zero; nonzero inode word |
| Kinds | directory, regular file, symbolic link, FIFO |
| Inode flag nibble | `0x0`, `0x8`, `0xc`, `0xe` |
| Mode | type as tabled; bits above `0177777` zero |
| Owner and time | the full 32-bit ranges as stored |
| Paths | relative, 0 to 1024 bytes, no empty, `.` or `..` component; a component outside the profile omits its node and is counted, never fatal; a path below a file, link or FIFO entry is shadowed and counted, never fatal |
| Symbolic links | the link layout, targets of 1 to 1024 bytes |
| Ranges | the range rule; overlaps allowed |
| References | nonzero ones name one regular-file record |
| Volume length | at least `image_size`; equal to it under verification |
| Checksum | zero sum, under verification |

The reader capability advertises files, directories, symbolic links and
FIFOs; owner, mode and the modification time; byte addressing (a sector and
allocation unit of 1 byte); a file limit of `2^32 - 1` bytes and a volume
limit of `2^32 - 4` bytes, the largest `image_size` that is a multiple of 4;
a name limit of 1024 bytes; a link-target limit of 1024 bytes; and no
label.

## Writer profile

The writer builds an ImageFS from the catalog in one deterministic layout.
Every layout property is one of two classes: a convention, which a reader
could depend on and which follows `mkifs`, or a free choice, which the
writer makes in its own order: [X1]

| Property | Class | The writer |
| --- | --- | --- |
| Root record first, empty path, inode 1 | Convention | The same |
| Directory records | Convention: only for named directories | One explicit record per source directory, the root included |
| Record padding to 4 bytes; the terminator | Convention | The same |
| `dir_offset` at the mountpoint's NUL rounded up to 4 | Convention | The same |
| Data after the table, trailer last, `image_size` exact | Convention | The same |
| Padding contents | Not a closed rule | Zero everywhere |
| Inode numbering | Free | 1 for the root, then catalog ID order from 2 |
| Record order | Free | The root, then catalog ID order |
| Data order and alignment | Free | Files in catalog ID order, each on a 4-byte boundary |
| Byte order | Setting | Little by default; big on request |

Explicit directory records for every directory are the one departure from
`mkifs`'s default, which writes them only for `type=dir`. They are a
convention `mkifs` itself follows for named directories, `dumpifs` lists
and extracts them, and they are the only way the image carries a
directory's owner, mode and time; an implicit directory would read back
with the synthesized values. The writer stores every kind's metadata by one
rule. [X1]

### Layout

| Range | Content |
| --- | --- |
| `[0, 0x58)` | The header: flags by the byte-order setting, the three sizes, zero references, zero spare words, zero mount flags |
| `[0x58, dir_offset)` | The mountpoint, its NUL, zero padding to a multiple of 4 |
| `[dir_offset, T)` | The root record, then one record per catalog entry in ID order, each padded with zeros to a multiple of 4 |
| `[T, T + 4)` | The terminator; `hdr_dir_size = T + 4` |
| `[hdr_dir_size, E)` | Each regular file's bytes in catalog ID order, each starting on a 4-byte boundary |
| `[E, image_size - 4)` | Zero padding to a multiple of 4 |
| `[image_size - 4, image_size)` | The trailer |

### Records

| Kind | Record |
| --- | --- |
| Directory | Length `0x18 + L + 1` padded; mode `040000` plus the permission bits |
| Regular file | The data offset and the length; mode `0100000` plus the bits |
| Symbolic link | `sym_offset = L + 1`, `sym_size` the target length, the target and its NUL after the path; mode `0120000` plus the bits |
| FIFO | Two zero words; mode `010000` plus the bits |

The inode number of a record is its catalog ID plus 1, which fits the 28
bits by construction: every record is at least 28 bytes and the image at
most `2^32 - 4`, so there are fewer than `2^28` records. Its flag nibble
and the header's references come from the [boot settings](#boot-settings);
without them every nibble and reference is zero and the image is
nonbootable. The extended-attribute offset is zero. Owner, mode and times follow
the [metadata policy](design-metadata.md#writer-policy): uid and gid 0,
mode `0755` for directories, `0644` for files and `0777` for links and
FIFOs when absent, the timestamp fallback for an absent time, and the epoch
for every time in reproducible mode. The catalog's root carries no metadata,
so the root record has mode `040755`, owner 0 and the fallback time, which
`-R` with epoch 0 makes `mkifs -n`'s zero. Owner and mode fit the record
exactly. A time outside 0 through 4294967295 is an optional loss under that
policy, counted and reported, and takes the fallback; it fails only for a
caller that forbids losses. [X1]

### Limits

| Value | Limit | Failure |
| --- | --- | --- |
| Path, from the image root | 1024 bytes | `FSIMG_EUNSUPPORTED` |
| Component | Within the path limit, under the profile | `FSIMG_EINVAL` for a profile violation |
| Symbolic-link target | 1 to 1024 bytes, the format's limit and the common ceiling | Never reached: the ceiling equals the limit, so collection refuses a longer target first |
| Mountpoint | 0 to 1024 bytes under the byte rules, slashes allowed | `FSIMG_EINVAL` |
| File length | `2^32 - 1` bytes | `FSIMG_EUNSUPPORTED` |
| Record length | 16 bits; by construction at most 2080 bytes, `0x1c + 1025 + 1025` rounded up to 4 | never reached |
| Image | `2^32 - 4` bytes | `FSIMG_ERANGE` |

An unrepresentable entry fails planning with `FSIMG_EUNSUPPORTED` and is
never truncated. Hard links are not written: the catalog has no
representation for them. The format has no label, so `max_label_bytes` is
0 and a label setting is refused by the common capability check. [X1]

### Boot settings

A bootable ImageFS names its boot programs and startup script in the
header and flags the records the loader processes. The writer takes these
as typed settings: up to four boot programs and one script, each the image
path of a regular file in the catalog, in the order of `boot_ino[]`. For
each named path the writer resolves the entry and writes that record's full
inode word, flag nibble included, into the header slot; a path that names
no catalog entry or an entry of another kind is `FSIMG_EINVAL`. The flag
nibble a referenced record receives comes from a closed writer table of
`(slot, nibble)` pairs, which the native evidence fills: a slot the table
lacks is `FSIMG_EUNSUPPORTED`. The table is empty, so every boot setting is
refused and every writer image is nonbootable; the
[research record](../docs/ifs-research.md#evidence-questions) names the
experiment that fills it. The writer never reads an image: wrapping the
result in a boot image's startup section, and the template it copies, are
the `qnxboot` container's. [S1], [S2]

### Settings and sizing

The image has no free space and no geometry: its size is exactly the header,
the mountpoint, the records, the terminator, the data and the trailer. A
requested `volume_bytes` or `min_free_bytes` is refused with `FSIMG_EINVAL`,
[sizing](design-sizing.md) has no dimension to search, and the plan reports
the derived size. The settings are the mountpoint, default `/`, the value
every boot image carries; the byte order, default little, big allowed
because `mkifs +bigendian` produces big-endian bodies natively; and the epoch, within 0 to 4294967295, the
range the time field stores. The mountpoint, byte order and boot settings
are filesystem settings, so their keys are bare: `mountpoint=`, `endian=`,
the latter shared with QNX6, `boot1=` to `boot4=` and `script=`; the epoch
is the common `-e`. An ImageFS is
placed in no partition scheme in any observed product, and the
[volume-length rule](#volume-layout) fails one in a slot longer than
`image_size`, so the format declares an
[exact volume](design-layers.md#exact-volumes): the layer design refuses
a scheme, or a container that pads the volume, below `ifs` before any
output, while gzip around it is written. An `ifsboot` or `hbcifs`
container around it is the
[layer design's](design-layers.md#layer-profiles). [X1], [S1], [S2]

## The PCM and live-CD images

The eight H3 boot bodies are little-endian with flags `0x04`, a mountpoint
of `/`, nonzero header padding, 206 to 230 records in the `PCM3_IFS1` family
and 174 in `PCM3_Emergency`, one directory record each, 21 and 16 implicit
directories, 52 and 42 symbolic links, inode nibbles `0x0`, `0x8`, `0xc` and
`0xe`, `boot_ino[0]` `0xe0000002` and `script_ino` 3 each naming one file,
valid UTF-8 names and depth at most 6 and 4. Each has three shadowed
records: in the `PCM3_Emergency` pair, the link records of three paths
stored as a link and then a file; in the `PCM3_IFS1` family, the link
record of one such path and two link records below a top-level link,
which precede it in the table.
The application body `PCM3_IFS2` is 68849668 bytes, flags `0x04`, an empty
mountpoint, no references, 336 records (2 directories, 214 files, 120
links), data starting on a page boundary above `hdr_dir_size`, and no
duplicate. The three X1 bodies have 45 records (1 directory, 40 files, 4
links), 2 implicit directories, zero header padding and the same references
as H3. All twelve bodies open and verify under every rule above, the
shadowed records reported, not refused. [S1], [S2]

Reading a boot image is the `qnxboot` container's peel followed by this
reader on the body; reading `PCM3_IFS2.ifs` is the `hbcifs` container's
spool-backed peel followed by this reader. A `dumpifs -u` copy, whose
startup header stays in place and whose body is decoded, is read as a
slice at the startup section's end.

## Deterministic planning and emission

Planning is a fixed sequence of passes over the frozen catalog: entries,
records, layout, summary. Every planned value derives from the catalog, the
settings and the rules above, so output is byte-identical across source
order, budgets, scratch placement, locale and time zone. Emission writes the
header, the records and the terminator, payload copies, and the trailer
accumulated over every emitted word; every emitted byte comes from a plan
row, the layout or a profile constant, and the common emission object has
zeroed everything else. The [driver design](design-ifs-driver.md) fixes the
passes and the emission order.

## Resource and failure requirements

Keep only the header, the mountpoint, a bounded number of windows, the
current record and the current entry's extent resident. Scratch holds the
path records of the table pass, the sorted runs, the derived child groups,
the per-directory group table, plan rows and the verifier's state, never
copied payload. Opening costs one sequential pass over the table and two
external sorts of its records; preparing a directory costs its children,
not its subtree; a content lookup costs one index search; verification
costs one sequential pass over the image for the checksum and one walk.
Every loop charges the session's work limit and observes cancellation;
scratch exhaustion fails explicitly. A table of `2^32 - 4` bytes and a flat
root of a million files stay within these bounds.

Failures are classified by the common statuses: unrecognized image
(`FSIMG_EFORMAT`), recognized but unobserved (`FSIMG_EUNSUPPORTED`),
contradiction (`FSIMG_ECORRUPT`), invalid setting or selection
(`FSIMG_EINVAL`), content that fits no size (`FSIMG_ERANGE`), exhaustion
(`FSIMG_ELIMIT`), and source change, cancellation and I/O failure as common
code reports them. A failed lookup never becomes end of file or a zero
extent. A partial output is never success. Diagnostics carry at most one
bounded, escaped name and never record bytes, padding or buffer contents.

## Acceptance matrix

Synthetic fixtures come from an original generator with its own offset
tables; native captures are read from a path the environment gives and are
never redistributed. Every passing case is paired with a mutation of the
same fixture that must fail with its class, so a test proves the checked
code ran. The minimum matrix:

| Area | Required cases |
| --- | --- |
| Probe and header | `imagefs` with one byte changed (no claim); flags `0x00` to `0x07` and `0xff`; each bound relation at `x - 1`, `x` and `x + 1`; `image_size` of 0, 4, and 1, 2 and 3 modulo 4; `image_size` one past the slice; a slice one byte and one sector longer than `image_size` (read, tail never delivered, corrupt under verify); unaligned `dir_offset` and `hdr_dir_size`; an image shorter than `0x58` bytes; nonzero spare words; nonzero mount flags and `chain_paddr` (accepted, reported); probe exclusivity against FAT, exFAT, QNX6 and QNX4 fixtures in both directions, and an ImageFS whose file data holds a QNX4 root block at 512 drawing two claims; forced format |
| Mountpoint | empty, `/`, `/foo`, 70, 1024 and 1025 bytes; a NUL exactly at `dir_offset - 1` and none before `dir_offset`; a NUL whose rounding is below `dir_offset` (unsupported); nonzero padding (accepted); invalid UTF-8 (omitted by info, incomplete under verify) |
| Records | lengths 0, 1, 23, 24, below each kind's minimum, exact minimum, and crossing the table end; a length of 25 (unsupported); a missing or nonzero terminator; a zero inode word; a nonzero extended-attribute offset; each unknown type value; mode bits above `0177777`; a path without a NUL; an empty table |
| Inode flags | each accepted nibble; each other nibble; the counts reported; a flagged directory (accepted, counted) |
| Hierarchy | implicit parents at depth 1 and 512; a tree whose record order is reversed; a duplicate path (link then file, file then link, two files, two directories), the last delivered and the earlier counted; a duplicate root; a root record present, absent and last; a file record with the empty path, as the entry and shadowed; a path below a file, below a link and below a FIFO, with the non-directory record before and after its descendants in the table, each shadowed and counted with no parent synthesized for it; a subtree of records at several depths below a link, in mixed table order, and a sibling after it delivered; a duplicate below a link, both records shadowed; a directory record shadowed by a file or a link record with descendants, the descendants shadowed; a link record shadowed by a directory record with descendants, the descendants delivered; a record below a link outside the profile, shadowed and not omitted; a corrupt range in a record below a link, still `FSIMG_ECORRUPT`; a file `d` beside `d-x` and `d/x`; `x/y` then `z` twice, the first `z` shadowed, with `z` delivered at the root; an absolute path; `.` and `..` components; an empty component; `a-b` and `a/b` siblings ordered by component |
| Names | components of 1, 255, 256 and 1024 bytes; a 1025-byte path; non-ASCII UTF-8; a space; names differing only by case; Latin-1 bytes, a control byte and `0x7f` on a file, a link, an explicit directory with descendants and an implicit one, each omitted and counted once with its descendants undelivered and uncounted, verification complete, `info`'s `omitted` detail equal to the count, a reference to an omitted file satisfied and counted in `omitted_references`, extraction of the parent refused without the skip option and omitting with it, and a corrupt range below the omitted directory still `FSIMG_ECORRUPT` |
| Ranges | a range ending exactly at the trailer; one including the trailer; one inside the table; one inside the header; start plus length overflowing 32 bits; shared ranges, including extraction of a heavily shared image under the default output ceiling; zero-length files with offsets 0, inside the table and past the image; sizes 0, 1, 3, 4, 65535, 65536, 65537 and above 16 MiB |
| Symbolic links | `sym_offset` of `L`, `L + 1` and `L + 2`; `sym_size` 0, 1, 1023, 1024 and 1025; a target crossing the record; a missing NUL after the target; relative, absolute, dangling and `..` targets delivered as bytes |
| Metadata | uid and gid 0, 65535 and 4294967295 read as stored; modes `0000`, `0644`, `0755`, `4755` and `7777`; times 0 and 4294967295; FIFO words nonzero |
| References | `boot_ino[0]` and `script_ino` naming one file; naming no record; naming two records; naming only a shadowed record, an earlier duplicate or a record below a link; naming a file whose inode word one or two shadowed records also carry, verified and counted once in `shadowed_references`, and two references to such a word, counted twice; naming a directory and a link; a nonzero `boot_ino[3]`; all zero |
| Checksum | a correct trailer; one flipped byte in the header, the mountpoint, a record, file data, padding and the trailer; a byte-swapped trailer; a big-endian image summed in its order |
| Writer | an empty catalog, which is the root record alone; one file; the name limits; each kind; owner 4294967295 and mode `7777`; a time of 4294967296 as a counted loss and as a refusal when losses are forbidden; a 1024-byte target, and a 1025-byte one refused by collection; mountpoints of 0, 1, 1024 and 1025 bytes; both byte orders; `volume_bytes`, `min_free_bytes`, a label and each boot setting refused; a 1025-byte path refused; an image one word below and at `2^32 - 4` bytes on a sparse sink; byte identity across source order, cache size, scratch path and time zone; a poison scan of every padding byte |
| Failure | Allocation, scratch, short read, short write, premature EOF, source change, cancellation, flush and publication failures at every stage, each followed by a cleanup check |

Independent acceptance is bidirectional: fsimg reads every native capture,
every `dumpifs -u` copy and every S1 and S2 body, comparing names, sizes and
checksums with the capture's manifest; native QNX 6.5.0's `dumpifs` lists,
checks and extracts fsimg output identically to `mkifs` output of the same
tree, and the E11 mount is added if one exists. `lclevy/dumpifs` and
`qnxprobe` are comparisons when their paths are given.

## Source keys

Provenance details are in the [research record](../docs/ifs-research.md#sources).

| Key | Source |
| --- | --- |
| X1 | Native QNX 6.5.0 `mkifs` and `dumpifs` experiments E1 to E7 and E9 to E11: captures `ifs-basic`, `ifs-names`, `ifs-extra`, `ifs-data`, `ifs-e10-sense`, `ifs-e3-extract` |
| E10 | The `dumpifs` sensitivity experiment, capture `ifs-e10-sense` |
| E12 | The `procnto` boot test of records below a symbolic link, capture `e12-links` |
| S1 | PCM 3.1 update disc: the eight boot bodies and `PCM3_IFS2` through the oracle decode, structure-only survey |
| S2 | QNX 6.5.0 live CD: `qnxbase.ifs`, `qnxbasedma.ifs`, `qnxbasesmp.ifs`, structure-only survey |
| D | The vendor documentation of the header and record layout, as the research record's sources list it |

[X1]: ../docs/ifs-research.md#observations
[E10]: ../docs/ifs-research.md#dumpifs-sensitivity-e10
[E12]: ../docs/ifs-research.md#paths-below-a-link-at-boot-e12
[S1]: ../docs/ifs-research.md#s1-h3-boot-images
[S2]: ../docs/ifs-research.md#s2-x1-boot-images
[D]: ../docs/ifs-research.md#sources
