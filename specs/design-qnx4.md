# QNX4 reader and writer

## Scope and evidence

`qnx4` denotes the QNX4 filesystem: the 512-byte-block, extent-based layout
that QNX 4 introduced and that QNX Neutrino's `fs-qnx4.so` still mounts. It
is not a QNX6 variant and shares no record with the
[Power-Safe design](design-qnx6.md). The reader consumes one volume as it is
on disk; the writer constructs a new volume that native QNX checks, mounts,
modifies and remounts. Neither modifies an existing volume. Readers and
writers remain independently registered capabilities.

The compatibility target is the navigation partition of the Porsche PCM 3.1
HDD, formatted by the head unit's `dinit -hq`, and native QNX 6.5.0 volumes.
The [driver design](design-qnx4-driver.md) maps the rules below onto the
modules of `lib/formats/qnx4/`; the [research record](../docs/qnx4-research.md)
holds the native evidence and the [qualification work](../docs/todo-qnx4.md)
the open gates. The research draft that supplied the questions is not a
source of rules.

This document is an original description of disk-format facts and fsimg
requirements. It contains no imported source, copied declarations or copied
documentation prose. Linux's `qnx4` reader is a GPL read-only comparison
oracle, never a code donor.

Evidence has three meanings here:

* **Established layout**: byte positions and meanings corroborated by vendor
  documentation and by the native captures. It establishes interpretation,
  not that every combination is accepted by QNX.
* **Observation**: a property measured on a named capture or on the PCM
  partition. Observations fix the closed acceptance tables.
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
publication. The common contracts cover QNX4 without exceptions:
[format descriptors and probes](design-formats.md#format-descriptors-and-profiles),
[prepared directories](design-directories.md),
[logical content and the extent index](design-content.md),
[emission](design-emission.md), [sizing](design-sizing.md) and
[POSIX metadata](design-metadata.md). The entry model carries owner,
mode, the four timestamps, symbolic links and FIFOs, so common code gains
only the descriptor registration and a volume-information label field of 49
bytes, room for a 48-byte label and its terminator.

Every disk integer is little-endian and decoded from bytes; native structure
layout, host alignment and host `stat` constants are not disk codecs. Block
numbers are 1-based. Offsets and products use checked `uint64_t` arithmetic;
a value that fails a check is corrupt, never clamped. All project-defined C
types have `_t` typedefs.

## Volume layout and geometry

### Blocks and the block count

A volume is `N` blocks of 512 bytes. Block `b` starts at byte `512 * (b - 1)`
of the volume slice. No record stores `N`: the native checker derives it from
the device size, and so does fsimg. `N` is the slice length divided by 512,
and the slice length must be a multiple of 512. A volume embedded in a longer
image is read as a slice given with `-b` and `-z`; without `-z` the slice runs
to the end of the image and `N` follows from that. [X1]

The smallest volume the native formatter accepts is 43 blocks, at which no
block is free; the largest `N` is `2^31`. The reader accepts any `N` from 43
to `2^31` whose bitmap and root structures validate; the writer emits volumes
in the same range. [X1], [Q7]

### Fixed layout

With `B = ceil(N / 4096)` bitmap blocks, the native formatter lays a volume
out as follows, and the reader requires the first four rows at these
positions: [X1]

| Blocks | Content |
| --- | --- |
| 1 | Loader. Not interpreted; a zero loader block mounts and checks clean |
| 2 | Root block: five 64-byte records and the label |
| 3 to `2 + B` | Bitmap, one bit per block |
| `3 + B` onward | Root directory's first extent |
| then | `.inodes`, `lost+found`, and every later object, wherever their extents say |

The default formatter places the root directory at `3 + B` with 8 blocks,
`.inodes` at `11 + B` with 16 blocks and `lost+found` at `27 + B` with 16
blocks. The reader takes those three from their records; only the root
directory's first extent is fixed at `3 + B`, which the native checker also
requires. [X1], [H1]

## The root block

Block 2 holds five inode records in the 64-byte layout below, a 48-byte
label and zeros: [X1]

| Offset | Bytes | Content |
| --- | --- | --- |
| `0x000` | 64 | Record 0: the root directory, name `/` |
| `0x040` | 64 | Record 1: `.inodes`, empty name |
| `0x080` | 64 | Record 2: `.boot`, empty name |
| `0x0c0` | 64 | Record 3: `.altboot`, empty name |
| `0x100` | 64 | Record 4: `lost+found`, empty name |
| `0x140` | 128 | Zero |
| `0x1c0` | 48 | Volume label, NUL-padded; a 48-byte label has no terminator |
| `0x1f0` | 16 | Zero |

### Root-block records

| Record | Name field | Mode | Status | Links | Extent |
| --- | --- | --- | --- | --- | --- |
| 0 root | `/` and fifteen NUL bytes | directory | `0x21` clean, `0x01` not clean | `2 + subdirectories`, `lost+found` included | first extent at `3 + B` |
| 1 `.inodes` | 16 NUL bytes | regular | `0x11` | 1 | the `.inodes` table |
| 2 `.boot` | 16 NUL bytes | regular | `0x11` untouched, `0x13` written | 1 | its content, or none |
| 3 `.altboot` | 16 NUL bytes | regular | `0x11` untouched, `0x13` written | 1 | its content, or none |
| 4 `lost+found` | 16 NUL bytes | directory | `0x11` | 2 | its slots |

The native formatter writes mode `040775` for the root, `0100444` for
`.inodes`, `0100600` for `.boot` and `.altboot` and `040755` for
`lost+found`, owner 0, and the format time in all four time fields. Writing
`.boot` or `.altboot` through the mounted filesystem sets the record's
modified bit. The reader requires the mode type and the status; the
permission bits, owner and times are reported, not constrained. The root
record's name is the probe's identity signature (the driver design states
the probe). Every volume observed has a `lost+found` record; a zero record 4
is unobserved and `FSIMG_EUNSUPPORTED`. [X1], [H1]

### Label

The label is the bytes at `0x1c0` up to the first NUL or to 48 bytes. The
reader reports a nonempty label through the volume information record. A
label is valid when its bytes satisfy the name profile's byte rules below
and the bytes after its first NUL are zero. An invalid label never blocks
reading: `info` omits it, and full verification reports the volume
incomplete with `FSIMG_EUNSUPPORTED`. The writer stores a label of at most
48 bytes under the same byte rules, so `max_label_bytes` is 48, and an empty
label selects the zero field the formatter writes without `-l`. The PCM
partition has a zero label. [X1], [H1]

### Clean flag

Bit `0x20` of the root record's status is the clean flag: set after `dinit`
and after `umount`, clear while mounted. `info` reports it as the volume's
clean state. No other record carried it, and the busy bit the draft asked
about was never observed. [X1], [H1]

## Records

### The 64-byte inode record

A direct directory slot, an `.inodes` slot and a root-block record share one
layout: [Q4], [X1]

| Offset | Bytes | Field |
| --- | --- | --- |
| `0x00` | 16 | Name, NUL-padded; a 16-byte name has no terminator |
| `0x10` | 4 | Size in bytes |
| `0x14` | 4 | First extent: starting block, 0 when there is no extent |
| `0x18` | 4 | First extent: length in blocks |
| `0x1c` | 4 | First extent block of the overflow chain, 0 when the record's extent is the only one |
| `0x20` | 4 | Creation time, seconds since 1970 UTC |
| `0x24` | 4 | Modification time |
| `0x28` | 4 | Access time |
| `0x2c` | 4 | Status-change time |
| `0x30` | 2 | Extent count, the record's own extent included |
| `0x32` | 2 | Mode: POSIX type and permission bits |
| `0x34` | 2 | Owner ID |
| `0x36` | 2 | Group ID |
| `0x38` | 2 | Link count |
| `0x3a` | 4 | Reserved, zero |
| `0x3e` | 1 | Type byte, zero |
| `0x3f` | 1 | Status |

The reserved bytes and the type byte were zero in every native and PCM
record; the reader requires zero in both and the writer writes zero. The
name field of a record in `.inodes` or in root-block records 1 to 4 is 16
NUL bytes; the name lives in the link record. [X1], [H1]

### The 64-byte link record

A slot whose status has the link bit names an inode record elsewhere: [Q5],
[X1]

| Offset | Bytes | Field |
| --- | --- | --- |
| `0x00` | 48 | Name, NUL-padded; a 48-byte name has no terminator |
| `0x30` | 4 | Target record's block |
| `0x34` | 1 | Target record's slot in that block, 0 to 7 |
| `0x35` | 4 | Long-name record's block, 0 for a name of at most 48 bytes |
| `0x39` | 6 | Zero |
| `0x3f` | 1 | Status, `0x08` |

### Status byte

| Bit | Meaning |
| --- | --- |
| `0x01` | Used: the slot is a live inode record |
| `0x02` | Modified: regular-file data has been written through the filesystem |
| `0x04` | Hidden: the slot names a format-owned directory that listing and lookup skip while the checker and the link count include it; only on the root slot naming `lost+found` |
| `0x08` | Link: the slot is a link record |
| `0x10` | Table record: the record lives in `.inodes` or in the root block, not in a directory slot; on every used `.inodes` slot and root-block records 1 to 4 |
| `0x20` | Clean, on the root record only |

A directory slot is free when its status has neither `0x01` nor `0x08`, and
an `.inodes` slot when it lacks `0x01`; every other byte of a free slot is
ignored, because the filesystem leaves stale names and targets in deleted
slots. `0x01` together with `0x08` is `FSIMG_ECORRUPT`. The accepted tuples
are closed, and the modified bit appears only on regular files: [X1], [H1]

| Record | Accepted status |
| --- | --- |
| Direct slot of a regular file | `0x01`, `0x03` |
| Direct slot of a directory, symbolic link or FIFO | `0x01` |
| Used `.inodes` slot of a regular file | `0x11`, `0x13` |
| Used `.inodes` slot of any other kind | `0x11` |
| Root-block records 1 and 4 | `0x11` |
| Root-block records 2 and 3 | `0x11`, `0x13` |
| Root-block record 0 | `0x01`, `0x21` |
| Link record | `0x08` |

The formatter writes `0x01` and the filesystem sets `0x02` when it writes
regular-file data; directories, symbolic links, FIFOs and untouched system
files keep `0x01`. The writer follows that: `0x03` for a regular file with a
direct record, `0x13` for one in `.inodes`, `0x01` and `0x11` for every other
object, `0x13` for a `.boot` or `.altboot` record with content and `0x11`
for an empty one, `0x21` for the root. [X1]

### Kinds, mode, owner and times

The mode's type bits select the entry kind: [X1], [Q6]

| Mode type | Kind | Content | Allocation |
| --- | --- | --- | --- |
| `0100000` | regular file | `size` bytes | exactly `ceil(size / 512)` blocks |
| `0040000` | directory | `size` bytes of 64-byte slots | exactly `size / 512` blocks; `size` a nonzero multiple of 512 |
| `0120000` | symbolic link | the target, `size` bytes | exactly `ceil(size / 512)` blocks |
| `0010000` | FIFO | none | any; the blocks are the FIFO's own and never delivered |

Any other type, including device nodes, is `FSIMG_EUNSUPPORTED`: the
containing directory's preparation fails, and nothing is fabricated or
omitted. The native filesystem keeps no allocation past a regular file's
size, with or without its overallocation option, so the exact-allocation rule
is an observation, not a policy. A FIFO is the one observed exception: native
`mkfifo` records size 5120 and ten blocks it never writes, so the reader
claims and verifies a FIFO's allocation and delivers nothing. The size field
is 32 bits, and the documented file limit is `2^31 - 1` bytes; a larger size
is `FSIMG_EUNSUPPORTED`. [X1], [Q7]

Owner and group are 16 bits. The reader reports them as stored, 65535
included. The four times are unsigned 32-bit UTC seconds with one-second
precision, mapped to the metadata extension's birth, modification, access
and change times; the reader accepts the full 32-bit range. The native
filesystem refuses owner 65535 and `touch` stores 0 through 2147483647, so
those are the writer's ranges. Symbolic links are `0120777` natively; FIFOs
carry their permission bits. The entry's metadata sets `FSIMG_META_OWNER`,
`FSIMG_META_MODE` with the low twelve bits, and all four timestamps. [X1]

## Extents

### Extent rules

An object's storage is an ordered list of extents, each a starting block and
a block count. The first extent lives in the inode record; the rest live in
a chain of 512-byte extent blocks starting at record offset `0x1c`. The
extent count at `0x30` counts them all, the record's own included: [X1],
[Q4]

* the count is 0 exactly when the first extent's block and length are both
  zero; a zero block with a nonzero length, or a nonzero block with a zero
  length, is `FSIMG_ECORRUPT`;
* the chain pointer at `0x1c` is nonzero exactly when the count exceeds 1;
* every extent lies inside `[1, N]` by checked arithmetic;
* the blocks of every extent and every extent block are owned once; an
  object that names a block twice, or that names a block another object
  owns, is `FSIMG_ECORRUPT` (the driver design's
  [extent walker](design-qnx4-driver.md#the-extent-walker) says when a
  listing detects it; full verification always does);
* the sum of the extent lengths is the object's allocation, bound to its
  size by the kind table above.

### The 512-byte extent block

| Offset | Bytes | Field |
| --- | --- | --- |
| `0x000` | 4 | Next extent block, 0 in the last |
| `0x004` | 4 | Previous extent block, 0 in the first; otherwise the block that pointed here |
| `0x008` | 1 | Descriptors used, 1 to 60 |
| `0x009` | 3 | Zero |
| `0x00c` | 4 | Blocks covered: the sum of this block's descriptor lengths |
| `0x010` | 480 | Sixty descriptors of starting block and length; unused ones zero |
| `0x1f0` | 8 | `IamXblk` and a NUL |
| `0x1f8` | 8 | In the first extent block, a copy of the record's first extent; zero in later blocks |

Native chains fill every block but the last: 61 extents need one extent
block, 62 and 121 two, 122 three. The reader requires every block but the
last to hold 60 descriptors; a partially used block followed by another is
`FSIMG_EUNSUPPORTED`. The chain holds exactly `count - 1` descriptors, so a
walk reads at most `ceil((count - 1) / 60)` extent blocks: a loop or a
repeated block cannot run unbounded and shows as an ownership failure or a
count disagreement, which is `FSIMG_ECORRUPT`. A wrong previous pointer, a
covered-blocks sum that disagrees, a zero or oversized use count, nonzero
spare bytes, a nonzero unused descriptor, a wrong signature or a wrong
first-extent copy is `FSIMG_ECORRUPT`. [X1]

The native filesystem fragments a file only under interleaved appends;
sequential writes stay contiguous. Directories grow by whole extents and
`.inodes` by 32-block extents through the same chain. [X1]

## The bitmap

`.bitmap` is a regular file whose content is the allocation bitmap: bit `k`
of the content, least significant bit first within each byte, is block
`k + 1`; a set bit means allocated. Its record is root slot 2, a direct
record with a single extent `(3, B)` and size `ceil(N / 8)`; bits from `N` to
the end of the last used byte are set, and the bytes from the size to the end
of block `2 + B` are `0xff`. The native formatter writes all of that, and the
checker derives the same length from the device size, so `N` is the slice's
block count and nothing else. A `.bitmap` record that differs in position,
extent, size or padding is `FSIMG_ECORRUPT`. [X1], [H1]

Blocks 1 and 2 and the bitmap's own blocks are allocated. Every block an
object references must be allocated, and full verification requires that
every allocated block has exactly one owner and every free block none.

## Directories

### Slots and size

A directory's content is an array of 64-byte slots. Its size is a nonzero
multiple of 512 equal to its allocation, scanned in full: a free slot is not
an end marker. The native filesystem creates and grows directories in whole
extents whose length depends on the volume, 8 blocks on a 131072-block
volume and 2 on a 4096-block one, while the formatter's root directory is 8
blocks at every measured size and `lost+found` 16 (the research record's
[extents](../docs/qnx4-research.md#extents-e4-e5-e6) observation).
The reader accepts any size the rule allows; the writer's own rounding is a
policy stated with the writer profile. [X1], [H1]

A used slot is a direct record or a link record; free slots are skipped. A
direct record's size, extents and metadata are the object's. A link record
names the object's record elsewhere; link resolution below says where.

### Dot entries

Slots 0 and 1 are `.` and `..`, both link records with status `0x08`, zero
long-name pointer and zero spare bytes: [X1]

| Slot | Name field | Target |
| --- | --- | --- |
| 0 `.` | `.`, NUL, `I`, `0x03`, `Q`, `N`, `X` at bytes 2 to 6, zero at 7, a 32-bit value at 8, zeros to 47 | The directory's own record position |
| 1 `..` | `..` and 46 zero bytes | The parent's record position; `(2, 0)` for a child of the root, except `lost+found`'s, which is `(3 + B, 0)` |

The value at byte 8 is the directory's first-extent length in blocks: 8 for
the root and every `mkdir` on the default volume, 2 on a 4096-block volume,
16 for `lost+found`, unchanged when the directory grows. The reader does not
interpret it; the writer writes the first-extent length. The root's two dot
entries both target `(2, 0)`. A parent's record position is its direct slot,
or its `.inodes` slot when the parent has a link name. Dot entries are
validated against the objects the walk reached them from and are never
admitted as entries. A missing, misplaced or wrong dot entry is
`FSIMG_ECORRUPT`. [X1]

### Root slots

The root directory's first slots are fixed: [X1]

| Slot | Record | Target or content |
| --- | --- | --- |
| 0 | `.` | `(2, 0)` |
| 1 | `..` | `(2, 0)` |
| 2 | `.bitmap`, direct record | extent `(3, B)`, size `ceil(N / 8)` |
| 3 | `.inodes`, link | `(2, 1)` |
| 4 | `.boot`, link | `(2, 2)` |
| 5 | `.altboot`, link | `(2, 3)` |
| 6 | the slot that natively names `lost+found`, status `0x04` | free to the reader |
| 7 | free | reused by the filesystem for the next entry |
| 8 | `.longfilenames`, direct record, or free with `-N` | size 0 and no extent on a new volume |

Root slots 3 to 5 are the only links that target block 2, each with exactly
this target, and they are validated by position when the volume is opened.
Every other link record except the dot entries resolves inside `.inodes`. `.longfilenames`
is found by name among the root's direct records; with `-R` it sits in slot
9 behind `.diskroot`. A used root slot beyond the fixed ones that carries the
name `.bitmap`, `.inodes` or a second `.longfilenames` is `FSIMG_ECORRUPT`.
Slot 6 has neither the used nor the link bit, so it is a free slot like any
other: the reader needs no rule for it. [X1], [H1]

## Names and the profile

### Encodings by length

| Name length | Record | Where the object lives |
| --- | --- | --- |
| 1 to 16 bytes | direct record | the slot itself |
| 17 to 48 bytes | link record, long-name pointer 0 | a used `.inodes` slot |
| 49 to 505 bytes | link record with the mangled prefix and a long-name pointer | a used `.inodes` slot |

An object with several names is always in `.inodes`, whatever the length of
its names, so a 1-to-16-byte name can also be a link record. Direct names
are unterminated at 16 bytes and link names at 48; the length is the bytes
before the first NUL or the field's end. A name of 1 to 16 bytes in a link
record is accepted; an empty name in any used record is `FSIMG_ECORRUPT`.
Directories with link names live in `.inodes` too. [X1], [H1]

### Long names

A name of 49 to 505 bytes stores its first 38 bytes, `~`, eight lowercase
hexadecimal digits of the long-name record's block and a zero byte in the
link's 48-byte name field, and that block number at link offset `0x35`. The
block is a physical block inside `.longfilenames`, whose 512-byte records
are: [X1], [Q5]

| Offset | Bytes | Field |
| --- | --- | --- |
| `0x000` | 4 | Block of the directory slot that owns this record |
| `0x004` | 1 | Slot index in that block |
| `0x005` | 1 | State: `0x01` used; any other value is unused and the record's bytes are ignored |
| `0x006` | 505 | The full name, NUL-padded |
| `0x1ff` | 1 | Zero |

A used record's back-reference must be the link's own position, its name
length 49 to 505 with zeros after it, and the link's 48-byte field must
equal the mangled form of that name; a disagreement is `FSIMG_ECORRUPT`. A
link whose pointer is nonzero but whose record names at most 48 bytes is
`FSIMG_ECORRUPT`. Because the mangled prefix carries the record's block, two
names sharing their first 48 bytes never collide. The native filesystem
refuses 506 bytes, grows `.longfilenames` from 0 to 16 blocks at the first
long name and reuses freed records; renaming a long name is unobserved. [X1]

### Profile

One name profile serves both QNX formats: strict UTF-8, no NUL, slash,
control byte or `0x7f`, and neither `.` nor `..`. It applies to the full
name, the long-name record's for a name over 48 bytes, never to the mangled
prefix. Names are compared byte for byte and are case-sensitive. QNX forbids
NUL, slash and the dot names, so a name carrying a NUL before its length, a
slash, or equal to `.` or `..` in a slot beyond the first two is
`FSIMG_ECORRUPT`; a natively legal name outside the profile is
`FSIMG_EUNSUPPORTED`. The PCM partition's names are all ASCII, so the
profile loses nothing there. The writer never truncates: a name the profile
cannot store fails planning. [H1]

## The `.inodes` table

`.inodes` is a regular file of 64-byte inode records, root-block record 1,
linked from root slot 3. Its size is a multiple of 512, so its slot count is
`size / 64`. Slot 0 is the table's signature: the name `IamTHE.inodeFILE`,
the 32-bit value `0x00100000` at offset 16 and zeros elsewhere, on native
volumes and on the PCM partition alike; anything else is `FSIMG_ECORRUPT`. A
used slot has status `0x11` or `0x13` and a 16-NUL name field. A slot whose
status lacks `0x01` is free: a never-used slot is zero, and a slot freed by
`rm` keeps its bytes with status `0x00`, so a free slot's bytes carry no
information and are ignored. The native formatter creates 16 blocks, 128
slots, and grows the table by 32-block extents. [X1], [H1]

## `.longfilenames`

`.longfilenames` is a regular file of the 512-byte records above, a direct
record in the root created empty by the default formatter and omitted with
`-N`. Its size is a multiple of 512. Its blocks are owned by it; a long-name
pointer that leaves its extents is `FSIMG_ECORRUPT`. The PCM partition has
it, empty. [X1], [H1]

## Format-owned and special files

| Name | Where | Exposure |
| --- | --- | --- |
| `.bitmap` | root slot 2 | hidden; validated at open |
| `.inodes` | root slot 3, record `(2, 1)` | hidden; validated at open |
| `.longfilenames` | root direct record | hidden; validated at open |
| `lost+found` | root-block record 4 | hidden; validated at open, blocks owned |
| `.boot`, `.altboot` | root slots 4 and 5, records `(2, 2)` and `(2, 3)` | regular files whose content is the record's |
| `.diskroot`, `.bad_blks` | ordinary root entries | ordinary regular files |

Hidden objects describe the volume, so `ls`, `cat` and `extract` never show
them and the writer refuses a catalog root entry with any of the four hidden
names. `.boot` and `.altboot` are content: a catalog's `/.boot` or
`/.altboot` regular file supplies the matching root-block record, and
without one the record has size 0 and no extent. [X1]

### `lost+found`

Natively `lost+found` is invisible: its root slot has status `0x04`, so
listing and lookup skip it, yet the root's link count counts it, the checker
walks it and the bitmap allocates its blocks. The reader reaches it only
through root-block record 4: a directory record whose first extent and chain
are walked and owned when the volume is opened, whose `.` targets `(2, 4)`
and whose `..` targets `(3 + B, 0)`, the root's own `.` slot, which is what
the formatter writes; a `..` naming the root record `(2, 0)` has never been
observed there and is `FSIMG_EUNSUPPORTED`. Its content is never exposed. Every slot beyond the
dots is free on every observed volume; a used slot there does not block
reading, and full verification reports the volume incomplete with
`FSIMG_EUNSUPPORTED`, since recovered objects have no observed form. [X1],
[H1]

## Identity and links

### Identity is physical

A direct record's identity is its position `(block, slot)`; a linked object's
identity is its `.inodes` position. Dot entries name these positions, so
validation compares positions and never follows a dot entry. Only `.inodes`
objects can have several names: a direct record is reachable from one slot
by construction. [X1]

### Link resolution

Root slots 3 to 5 are validated by position at open against their fixed
targets in block 2. Dot entries name directory positions (below). Every
other link record's target `(block, slot)` must
have a slot index of at most 7, lie inside an `.inodes` extent, not be slot
0, and hold a used record with status `0x11` or `0x13`; a target outside
those bounds, in the loader, root block, bitmap or any other object, or at a
free slot, is `FSIMG_ECORRUPT`. A used `.inodes` record with an unobserved
status is `FSIMG_EUNSUPPORTED`. [X1], [H1]

A file, symbolic link or FIFO in `.inodes` is exposed once per name, and its
storage is walked and owned once, under the first name the walk reaches. A
directory reached from two names, or twice through a cycle, is
`FSIMG_ECORRUPT` because its storage is owned when it is entered. A link to a
record in `.inodes` from a directory in a different subtree is simply another
name.

### Link counts

A directory's link count is `2 + subdirectories`; the root counts
`lost+found`. A file's, symbolic link's or FIFO's is the number of names that
reach it. Hard links turn every name into a link record to one `.inodes`
slot; unlinking back to one name leaves the link record and the slot in
place. Full verification checks both rules for every used slot, hidden ones
included, and `.inodes` occupancy: every used slot but slot 0 is referenced
and every referenced slot is used. [X1], [H1]

## Reader profile

| Property | Accepted |
| --- | --- |
| Block count | 43 to `2^31`, from the slice length |
| Root block | the five records and zero ranges as tabled; an invalid label is omitted, not fatal |
| Root directory | first extent at `3 + B`; slots 0 to 5 as tabled; `.longfilenames` by name |
| Statuses, type byte, reserved and spare bytes | the closed tables |
| Kinds | regular, directory, symbolic link, FIFO |
| Names | 1 to 505 bytes under the profile |
| Owner and times | the full 16-bit and 32-bit ranges as stored |
| Extents | chains of any length within the record's count |
| Allocation | exact for files, directories and links; any for FIFOs |
| `.inodes` | any size that is a multiple of 512; slot 0 signature |
| `lost+found` | present; a used slot in it makes verification incomplete |
| Loader | not interpreted |

The reader capability advertises files, directories, symbolic links and
FIFOs; owner, mode and the four timestamps; a 512-byte sector and
allocation unit; and a 48-byte label.

## Writer profile

The writer reproduces the native formatter's default layout and fills it
from the catalog. Every object, format-owned ones included, is one contiguous
extent, so the writer never emits an extent block: a file of at most
`2^31 - 1` bytes needs at most 4194304 blocks, which a 32-bit extent length
holds. [X1]

### Layout

| Blocks | Object | Size | Mode | Status |
| --- | --- | --- | --- | --- |
| 1 | Loader | | | zero: a zero loader mounts and checks clean |
| 2 | Root block | | root `040775` or the catalog root's mode | `0x21` |
| 3 to `2 + B` | `.bitmap` | `ceil(N / 8)` | `0100444` | `0x01` |
| `3 + B`, `R` blocks | Root directory | `R * 512` | | |
| next, `I` blocks | `.inodes` | `I * 512` | `0100444` | `0x11` |
| next, 16 blocks | `lost+found` | 8192 | `040755`, links 2 | `0x11`; its root slot 6 `0x04` with target `(2, 4)` |
| next, `L` blocks | `.longfilenames` | `L * 512` | `0100444` | `0x01` |
| next | Catalog objects in catalog ID order | | | |

A directory of `s` slots has `max(8, 8 * ceil(s / 64))` blocks: the root's
slots are `max(9, 8 + E)`, where `E` counts the catalog's root entries other
than `/.boot` and `/.altboot`, and a subdirectory's are `2 + children`.
Rounding directories to 4096 bytes is a policy that matches the default
volume's convention; the native requirement is only whole blocks. `I` is
the `.inodes` capacity below and `L = 16 * ceil(long names / 16)`, zero
without long names. An empty catalog gives exactly the formatter's 43-block
minimum at the smallest size, and at any size the layout is byte for byte
the formatter's apart from times and the loader block. [X1]

### Root slots and records

Slots 0 to 6 are the fixed table above with the `lost+found` slot named,
targeted at `(2, 4)` and given status `0x04`; slot 7 is the first of the
`E` root entries or, without one, a zero slot with status `0x02` as the
formatter writes; slot 8 is `.longfilenames`; the remaining root entries
follow in catalog ID order. `/.boot` and `/.altboot` take no root slot of
their own: slots 4 and 5 always link to records `(2, 2)` and `(2, 3)`.
Root-block records 1 and 4 carry their native modes, owner 0, the format
time and a 16-NUL name field. A catalog `/.boot` or `/.altboot` that is not
a regular file is `FSIMG_EUNSUPPORTED`; one that is supplies record 2's or
3's size, extent, mode, owner and times, with mode `0600`, owner 0 and the
format time for absent fields, and status `0x13` when its size is nonzero;
without one the record has size 0, no extent, the native defaults and
status `0x11`. The catalog root's owner, mode and times, when given, become
the root record's. `lost+found` is 16 blocks with `.` targeting `(2, 4)`
and `..` targeting `(3 + B, 0)`.

### `.inodes` capacity

The capacity is `max(128, 1 + indirect)` slots rounded up to a whole block
of 8, where `indirect` is the number of catalog entries that need an
`.inodes` slot: every entry whose name exceeds 16 bytes. The
`inode_capacity` setting (`inodes=`) counts slot 0 and raises the floor, so
the capacity is `max(128, setting, 1 + indirect)` rounded up to
whole blocks. Slots are assigned in catalog ID order from slot 1, and every
`.inodes` record's name field is 16 NUL bytes.

### Entries

| Name length | Record |
| --- | --- |
| 1 to 16 | direct record in the parent |
| 17 to 48 | link record in the parent, inode record in `.inodes` |
| 49 to 505 | link record with the mangled prefix, inode record in `.inodes`, one `.longfilenames` record |

Long-name records are assigned in catalog ID order; each stores the slot's
block and index, state `0x01` and the name, and the link's name field holds
the mangled prefix with its zero byte at 47. A regular file's size and content
are the source's; a symbolic link's are its target bytes; a directory's are
its slots; a FIFO is written with size 0 and no extent, which the native
checker and filesystem accept. Owner, mode and times
follow the [metadata policy](design-metadata.md#writer-policy): uid and gid
0, mode `0755` for directories, `0644` for files and `0777` for links and
FIFOs when absent, and the format time for an absent time. A field outside
the profile's range is an optional loss under that policy, counted and
reported, and fails only for a caller that forbids losses: an owner or group
above 65534 is stored as 0, and a time outside 0 through 2147483647 takes
the format time. A size above `2^31 - 1` or a name outside the profile fails
planning. Hard links are not written: the catalog has no representation for
them.

### Bitmap

Allocation is one prefix: blocks 1 through `A` are in use and none above.
The bitmap is ones for bits `0` to `A - 1`, zeros to bit `N - 1`, ones to
the end of the last used byte and `0xff` to the end of block `2 + B`.

### Settings and sizing

`sector_bytes` and `allocation_unit_bytes` are zero or 512; `volume_bytes`
is zero or a multiple of 512 from `43 * 512` to `2^31 * 512`; the epoch is
within 0 to 2147483647, the range the writer stores; the label is at most 48 bytes; `inode_capacity` is as above.
A volume of `N` blocks holds the plan when
`2 + B(N) + R + I + 16 + L + objects + ceil(min_free_bytes / 512) <= N`,
where `objects` is the catalog objects' blocks. Automatic sizing takes the
smallest such `N`, which exists by raising `N` until the inequality holds,
since `B` grows by one block per 4096 blocks. QNX4 has no other dimension,
so [minimization](design-sizing.md) returns the default volume.

## The PCM navigation partition

Partition 3 of the PCM 3.1 HDD, type `0x4f`, has 102767805 blocks and the
6.5.0 default layout exactly: a 12845976-byte `.bitmap` over 25090 blocks
with set padding bits, the root directory at 25093, `.inodes` at 25101 and
`lost+found` at 25117 with the default lengths, an empty `.longfilenames`, a
zero label and a nonzero loader that differs from 6.5.0's. The root status
is `0x01`: the partition was not cleanly unmounted, and `info` says so. Every
object is a single extent; `.inodes` has 128 slots with 48 used, directories
included; the 76 names are ASCII, direct ones 5 to 16 bytes and link ones 17
to 31; no object has two names; every allocation is exact and reachable
allocation equals the bitmap. The head unit's formatter carries the same
option set as 6.5.0's and no `.inodes` size option. [H1], [H3]

Reading the partition is a slice of `102767805 * 512` bytes at its
partition offset. Reproducing it is the writer at that `volume_bytes` with
an empty label, which the CLI selects with `-L ""` because its default
label `FSIMG` applies to every format that stores one; the loader block and disk composition are outside this
design.

## Deterministic planning and emission

Planning is a fixed sequence of passes over the frozen catalog: entries,
geometry, layout, summary. Every planned value derives from the catalog, the
options and the rules above, so output is byte-identical across source order,
budgets, scratch placement, locale and time zone. Emission writes payload
copies, directory slots, root-block records, the `.inodes` signature and
records, long-name records and the bitmap; every emitted byte comes from a
plan row, the geometry or a profile constant, and the common emission object
has zeroed everything else. The [driver design](design-qnx4-driver.md) fixes
the passes and the emission order.

## Resource and failure requirements

Keep only the geometry, the root block, a bounded number of 4 KiB windows,
the current record and the current extent chain position resident. Scratch
holds the extent indexes, the claimed-block bitset of `N` bits, the
`.inodes` exposure bitset, directory records, plan rows and the verifier's
link targets, never copied payload. A random content lookup costs one index
search after the object's chain is walked once; sequential reads batch
adjacent blocks. No file read rescans `.inodes` or its directory. Full
verification can cost work proportional to `N / 8` bitmap bytes, the
`.inodes` size and the `.longfilenames` size; opening and selected reads do
not imply that. Every loop, including free-slot and unused-record scans,
charges the session's work limit and observes cancellation; scratch
exhaustion fails explicitly.

Failures are classified by the common statuses: unrecognized image
(`FSIMG_EFORMAT`), recognized but unobserved (`FSIMG_EUNSUPPORTED`),
contradiction (`FSIMG_ECORRUPT`), invalid setting or selection
(`FSIMG_EINVAL`), content that fits no size (`FSIMG_ERANGE`), exhaustion
(`FSIMG_ELIMIT`), and source change, cancellation and I/O failure as common
code reports them. A failed lookup never becomes end of file or a zero
extent. A partial output is never success. Diagnostics carry at most one
bounded, escaped name and never slot bytes, stale free-slot contents or
buffer contents.

## Acceptance matrix

Synthetic fixtures come from an original generator with its own offset
tables; native captures are read from a path the environment gives and are
never redistributed. Every passing case is paired with a mutation of the same
fixture that must fail with its class, so a test proves the checked code
ran. The minimum matrix:

| Area | Required cases |
| --- | --- |
| Geometry and probe | Slice length not a multiple of 512; 1, 2 and 3 bitmap blocks; `N` of 43, 1000, 4095, 4096, 4097, 5001, 8192 and 8193; image shorter than 1024 bytes; root directory past the slice; probe exclusivity against FAT, exFAT and QNX6 fixtures in both directions, native QNX6 boot areas and fsimg's zeroed one included; forced format |
| Root block | Each record's status and mode type, `0x13` on records 2 and 3 only; nonzero reserved ranges; label of 0, 1, 47 and 48 bytes; an invalid label and nonzero padding, omitted by `info` and incomplete under `verify`; zero record 4 |
| Bitmap | Wrong position, extent, size; a clear padding bit; a padding byte not `0xff`; a referenced free block; an allocated orphan; a block with two owners |
| Names | 1, 15, 16, 17, 47, 48, 49, 504 and 505 bytes with 16, 48 and 505 unterminated; a 16-byte name in a link record; an empty name in each record kind; duplicates; invalid bytes; a NUL inside, a slash, `.` and `..` as entry names; names differing only by case; a long name whose prefix, zero byte, pointer, back-reference or state disagrees; a pointer outside `.longfilenames` |
| Records | Every status value for each record kind, the modified bit on each kind; used with link; nonzero type, reserved or spare bytes; a nonzero `.inodes` name field; a freed `.inodes` slot with stale bytes; links with slot index 8, into the loader, root block, bitmap, a payload block, outside `.inodes`, to slot 0 or to a free slot; root slots 3 to 5 with wrong targets; a link into block 2 from any other slot |
| Extents | 0, 1, 2, 60, 61, 62, 121 and 122 extents; each extent-block fault: loop, repeated block, wrong previous pointer, 0 or 61 used, nonzero spare, count disagreement, nonzero unused descriptor, bad signature, wrong first-extent copy, partial block followed by another; an extent crossing `N`; zero start with nonzero length; size short of, equal to and beyond allocation for each kind; FIFO with and without blocks |
| Directories | Size not a multiple of 512, not equal to allocation; each dot-entry fault; a 2-block and a 16-block directory; a cycle; a second parent; a hard link listed twice and walked once; an unsupported kind; a wide directory that spills to scratch |
| `.inodes` and `lost+found` | Missing or wrong signature; used slot unreferenced; referenced free slot; a directory in `.inodes`; `lost+found` with `..` at `(3 + B, 0)`, at `(2, 0)` (unsupported) and elsewhere, with a used slot (reads, verifies incomplete), and with record 4 zero |
| Metadata | uid and gid 0, 65534 and 65535 read as stored; modes `0000`, `0644`, `0755`, setuid; times 0, 2147483647 and 4294967295 read as stored |
| Writer | Exact and automatic sizing at every bitmap-block boundary; the empty catalog at 43 blocks; a full volume; `inodes=` below 128, at and above the need; root with 0, 1, 2 and 60 entries; names at every length boundary; `/.boot` as an empty file, a file with content and a directory; reserved root names; FIFOs and symbolic links; owner 65535 and a time of 2147483648 as counted losses and as refusals when losses are forbidden; byte identity across source order, cache size, scratch path and time zone; poison scan of every unused byte |
| Failure | Allocation, scratch, short read, short write, premature EOF, source change, cancellation, flush and publication failures at every stage, each followed by a cleanup check |

Independent acceptance is bidirectional: fsimg reads every native capture and
the PCM partition, comparing names, sizes and checksums with the capture's
manifest; native QNX 6.5.0 runs `chkfsys -m -f -u` without findings on fsimg
output, mounts it, creates, appends until a file fragments, renames, makes and
removes directories, unlinks, creates a long name, syncs, unmounts, checks
again and remounts, and fsimg verifies the result. The Linux `qnx4` reader is
a comparison for its 48-byte-name subset. The writer capability is registered
only after that native record exists.

## Source keys

Provenance details are in the [research record](../docs/qnx4-research.md#sources).

| Key | Source |
| --- | --- |
| X1 | Native QNX 6.5.0 `dinit`, `chkfsys` and `fs-qnx4.so` experiments E1 to E10 |
| H1 | PCM 3.1 HDD partition 3, structure-only survey |
| H3 | PCM 3.1 update disc: head-unit `dinit`, `chkfsys` and `fs-qnx4.so` identities |
| Q4 | QNX 4.25 disk and file recovery guide: layout order, signatures, extent blocks, system files |
| Q5 | QNX 6.4.1 recovery guide: link records, the 48-byte prefix and the long-name record |
| Q6 | QNX 4.25 filesystem manager architecture: extents, embedded inodes, links, symbolic links |
| Q7 | QNX Neutrino 6.4 filesystem limits: file size and volume size |

[X1]: ../docs/qnx4-research.md#observations
[H1]: ../docs/qnx4-research.md#h1-partition-3
[H3]: ../docs/qnx4-research.md#tool-identities
[Q4]: https://www.qnx.com/developers/docs/qnx_4.25_docs/qnx4/user_guide/recovery.html
[Q5]: https://www.qnx.net/developers/docs/6.4.1/neutrino/user_guide/lost_data.html
[Q6]: https://www.qnx.com/developers/docs/qnx_4.25_docs/qnx4/sysarch/fsys.html
[Q7]: https://qdn.qnx.com/developers/docs/6.4.0/neutrino/user_guide/limits.html
