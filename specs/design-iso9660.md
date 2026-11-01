# ISO 9660 reader and writer

## Scope and evidence

This design defines fsimg's support for ISO 9660 volumes: the cooked
2048-byte-sector image that `.iso` files, CD and DVD data tracks, and the
ISO portion of a UDF bridge disc hold. It covers the volume descriptor set,
the primary hierarchy with its directory records and path tables, the
System Use Sharing Protocol (SUSP) and Rock Ridge (RRIP) interpretation of
the primary hierarchy, the Joliet supplementary hierarchy, multi-extent
files, and El Torito boot catalogs. The
[driver design](design-iso9660-driver.md) maps it onto code, and the
[implementation plan](../docs/iso9660-plan.md) orders the work.

The references are ECMA-119 (sixth edition, December 2025) for the volume
and file structure and its Annex C for Joliet, the IEEE P1281 (SUSP) and
P1282 (RRIP) drafts, and the El Torito 1.0 specification. They are factual
references: this design restates their layouts in its own words and copies
no prose. The research draft
[fsimg_iso9660_spec.md](../fsspecs/fsimg_iso9660_spec.md) informed it and is
not normative; the plan lists where this design departs from it.

Every rule below is one of three kinds:

* **Format.** A layout or relationship the references state. A violation
  is `FSIMG_ECORRUPT`.
* **Profile.** A value fsimg accepts from a closed table, because it is
  what qualified producers write and readers accept. An unlisted value is
  `FSIMG_EUNSUPPORTED`, never a guess.
* **Policy.** An fsimg choice where the format leaves one, such as the
  writer's layout or a reader's displayed name. Policy never makes a valid
  image corrupt.

Independent implementations are the evidence. The writer is qualified
against Linux `isofs`, FreeBSD `cd9660`, libarchive (`bsdtar`), `isoinfo`,
`isovfy` and `xorriso` as readers, and the reader against images that
`xorriso`, `genisoimage`, `makefs -t cd9660` and libarchive's ISO writer
create, plus the owner's Porsche PCM 3.1 update disc (H3 in the
[QNX6 research record](../docs/qnx6-research.md#pcm-31-update-disc)).

Out of scope, each with a stated outcome: the ISO 9660:1999 enhanced
descriptor as a view, interleaved files, extended attribute record
interpretation, associated files, multisession and multi-volume sets,
zisofs and other compressed files, Apple and AAIP extensions, raw 2352-byte
sectors and BIN/CUE, High Sierra, UDF, and writing hybrid MBR or GPT system
areas. None of them is ever read as something else.

## Shared owners and representation

The driver follows every common contract:

| Concern | Owner |
| --- | --- |
| Byte ranges, reads and zeroing | Bounded I/O on the volume slice |
| Identity and recognition | The [descriptor and probe](design-formats.md#format-descriptors-and-profiles) |
| Traversal, paths, depth, selection, namespace validation | The [prepared-directory walker](design-directories.md) |
| Delivery of file bytes and link targets | [Logical content](design-content.md) |
| Metadata scratch: streams, arrays, external sort, extent index | [Storage](design-storage.md) |
| Field omission and timestamp losses | The [writer loss policy](design-metadata-loss.md) |
| Payload copies, link-target reads and sink initialization | [Emission](design-emission.md) |
| A partition table or container around the volume | The [layer design](design-layers.md) |

The driver reads and writes a bare volume. An isohybrid image, whose system
area holds an MBR or GPT that makes the same file a partitioned disk, has
two valid readings: the scheme probe and the ISO probe both claim it, so
detection reports an ambiguity under the existing
[rule](design-layers.md#detection-and-forcing), and `-F iso9660/-` reads the
ISO while `--partition` reads a partition. No probe gives one reading
priority.

## Volume layout

### Logical blocks and addresses

The supported profile has one geometry: 2048-byte logical sectors and
2048-byte logical blocks. Every address in the volume is a logical block
number `n`, at byte offset `n * 2048` of the volume, computed in 64 bits.
The volume holds `volume_space_size` blocks, the primary descriptor's
count; the format addresses at most `2^32 - 1` blocks, just under 8 TiB.
Every extent is checked as `start + length <= volume_space_size` in 64-bit
block arithmetic before it is read.

The slice may be longer than the volume, for trailing pad or a container's
rounding; bytes beyond the volume are never read. It may also be shorter,
as rips that lose the final pad blocks are: the readable volume ends at the
slice, every structure the reader uses must lie within it, and one that
does not is a truncated image, `FSIMG_ECORRUPT` when it is referenced. A
missing tail that nothing references is not a finding; info reports its
length.

### The system area

Blocks 0 through 15 are the system area. The format gives them no
structure; the reader never interprets them and never rejects an image for
their contents, which on a hybrid image hold a partition table and boot
code. Info reports whether any of their bytes is nonzero, so a hybrid image
is visible. The writer writes zeros.

### The volume descriptor set

From block 16, consecutive blocks hold volume descriptors, each 2048 bytes:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | Type: 0 boot record, 1 primary, 2 supplementary or enhanced, 3 partition, 255 terminator |
| 1 | 5 | Standard identifier `CD001` |
| 6 | 1 | Descriptor version |
| 7 | 2041 | Type-specific contents |

### Descriptor scan rule

Open reads descriptors from block 16 until the first terminator:

* every descriptor before the terminator carries `CD001`, else
  `FSIMG_ECORRUPT`;
* its type and version are in the [profile table](#reader-profile), else
  `FSIMG_EUNSUPPORTED`;
* the terminator's bytes 7 through 2047 are zero, else `FSIMG_ECORRUPT`;
* at most 64 descriptors precede the terminator, a profile bound: a longer
  set is `FSIMG_EUNSUPPORTED`. Producers write fewer than ten;
* at least one primary descriptor precedes the terminator, else
  `FSIMG_ECORRUPT`;
* a second primary descriptor must be byte-identical to the first, else
  `FSIMG_EUNSUPPORTED`; the first is the one used.

Bytes after the terminator are not interpreted. A UDF bridge disc places
its volume recognition sequence there, which this reader ignores.

## Numeric and time encodings

### Both-byte-order fields

| Name | Size | Encoding |
| --- | ---: | --- |
| `u8` | 1 | Unsigned byte |
| `s8` | 1 | Two's-complement signed byte |
| `u16le`, `u16be` | 2 | One byte order only |
| `u16both` | 4 | Little-endian copy then big-endian copy of one 16-bit value |
| `u32le`, `u32be` | 4 | One byte order only |
| `u32both` | 8 | Little-endian copy then big-endian copy of one 32-bit value |

The two copies of a `both` field must be equal, else `FSIMG_ECORRUPT`; the
format has no checksum over its descriptors, and the duplicate copy is its
consistency check. The writer stores both copies of every such field.

### Recording dates

Directory records and Rock Ridge short timestamps use seven bytes:

| Byte | Field | Range |
| ---: | --- | --- |
| 0 | Years since 1900 | 0 to 255 |
| 1 | Month | 1 to 12 |
| 2 | Day | Valid for the month and year |
| 3 | Hour | 0 to 23 |
| 4 | Minute | 0 to 59 |
| 5 | Second | 0 to 59 |
| 6 | Offset from UTC in 15-minute units, `s8` | -48 to 52 |

Seven zero bytes mean the date is absent. Any other value outside the
ranges is `FSIMG_ECORRUPT`. A present date becomes an `fsimg_timestamp_t`
with basis `FSIMG_TIME_OFFSET`, `offset_minutes = 15 * offset`, seconds of
the UTC instant, and a precision of one second.

### Descriptor dates

Volume descriptors and Rock Ridge long timestamps use seventeen bytes:
sixteen ASCII digits `YYYYMMDDHHMMSScc`, with `cc` hundredths of a second,
then the offset byte of the recording date. Sixteen `0` digits, or sixteen
zero bytes, with a zero offset mean absent. Otherwise every digit must be a
decimal digit and the fields must form a valid date, year 1 through 9999,
else `FSIMG_ECORRUPT`. The precision is ten milliseconds.

## Primary and supplementary descriptors

### The primary descriptor

| Offset | Size | Field | Rule |
| ---: | ---: | --- | --- |
| 7 | 1 | Unused | Not interpreted |
| 8 | 32 | System identifier | Not interpreted |
| 40 | 32 | Volume identifier | The label, space padded |
| 80 | 8 | Volume space size, `u32both` | Nonzero |
| 120 | 4 | Volume set size, `u16both` | 1 |
| 124 | 4 | Volume sequence number, `u16both` | 1 |
| 128 | 4 | Logical block size, `u16both` | 2048 |
| 132 | 8 | Path table size in bytes, `u32both` | See [path tables](#path-tables) |
| 140 | 4 | Type-L path table block, `u32le` | Within the volume |
| 144 | 4 | Optional type-L copy, `u32le` | Zero or within the volume |
| 148 | 4 | Type-M path table block, `u32be` | Within the volume |
| 152 | 4 | Optional type-M copy, `u32be` | Zero or within the volume |
| 156 | 34 | Root directory record | See [dot records](#dot-records) |
| 190 | 128 | Volume set identifier | Not interpreted |
| 318 | 128 | Publisher identifier | Not interpreted |
| 446 | 128 | Data preparer identifier | Not interpreted |
| 574 | 128 | Application identifier | Not interpreted |
| 702 | 37 | Copyright file identifier | Not interpreted |
| 739 | 37 | Abstract file identifier | Not interpreted |
| 776 | 37 | Bibliographic file identifier | Not interpreted |
| 813 | 17 | Volume creation date | Valid or absent |
| 830 | 17 | Volume modification date | Valid or absent |
| 847 | 17 | Volume expiration date | Valid or absent |
| 864 | 17 | Volume effective date | Valid or absent |
| 881 | 1 | File structure version | 1 |
| 883 | 512 | Application use | Not interpreted |

Bytes 72 to 79, 88 to 119, 882 and 1395 to 2047 are reserved. The reader
does not interpret them: producers have written into them, and no reader
depends on them. Full verification checks the [ownership](#ownership) of
every block the descriptor names, not its reserved bytes. The writer
writes zeros there.

### Supplementary descriptors and Joliet

A type-2 descriptor with version 1 is a supplementary descriptor. It has
the primary descriptor's layout with two differences: byte 7 holds volume
flags and bytes 88 to 119 hold escape sequences. Its root record, path
tables and identifiers describe its own hierarchy.

A supplementary descriptor is **Joliet** when its escape sequences begin
with one of these three-byte sequences and the rest of the 32 bytes are
zero:

| Bytes | Joliet level |
| --- | --- |
| `25 2F 40` (`%/@`) | 1 |
| `25 2F 43` (`%/C`) | 2 |
| `25 2F 45` (`%/E`) | 3 |

A Joliet descriptor's volume flags must be zero, its geometry fields equal
the primary descriptor's (volume space size, set size, sequence number and
block size), and its root record satisfies the primary root's rules, else
`FSIMG_ECORRUPT`. Its label is 32 bytes of big-endian UCS-2. A
supplementary descriptor with any other escape sequence is framed and
ignored. Two Joliet descriptors in one set are `FSIMG_EUNSUPPORTED`: no
producer writes them, and choosing one would be a guess.

### Other descriptors

* **Enhanced** (type 2, version 2): framed and ignored; it is not a view.
* **Partition** (type 3): framed and ignored; it names no partition fsimg
  opens.
* **Boot record** (type 0): an [El Torito](#el-torito) boot record when its
  boot system identifier says so; any other boot record is framed and
  ignored.

## Views

An image can carry up to three name trees over the same file data. The
reader exposes exactly one, the **view**, for the whole reader session:

| View | Tree | Names | Kinds and metadata |
| --- | --- | --- | --- |
| `rockridge` | Primary hierarchy with its SUSP entries | Rock Ridge names | Files, directories, symbolic links, FIFOs; owner, mode, Rock Ridge times |
| `joliet` | The Joliet hierarchy | UCS-2 identifiers as UTF-8 | Files and directories; recording dates |
| `primary` | Primary hierarchy without SUSP | ISO identifiers | Files and directories; recording dates |

A view never consults another tree: a Joliet entry gets no POSIX metadata
from a primary record with a similar name, and a Rock Ridge view never
falls back to Joliet when a deep directory turns out corrupt.

### View selection

The read option `iso_view` (CLI `-o view=auto|rockridge|joliet|primary`)
selects the view. A forced view that the image does not offer is
`FSIMG_EUNSUPPORTED` at open. The default `auto` chooses at open:

1. `rockridge` when the [handshake](#the-handshake) at the primary root
   succeeds;
2. otherwise `joliet` when a Joliet descriptor exists;
3. otherwise `primary`.

Rock Ridge comes first because it carries the exact POSIX names, kinds and
metadata, Joliet second because its Unicode names are exact for most
trees, and the primary view, whose names are often projections, last. The
choice reads only the descriptors and the primary root's first dot record
with its continuation areas. A forced `joliet` or `primary` view does not
decode the primary root's system use area, so a damaged handshake cannot
keep a caller from another tree. Corruption found after open is reported
against the selected view; the choice never changes mid-session. Info
names the selected view in the profile and lists which views the image
offers.

## Directory records

### Record layout

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | Record length `LEN_DR` |
| 1 | 1 | Extended attribute record length in blocks |
| 2 | 8 | Extent block, `u32both` |
| 10 | 8 | Data length in bytes, `u32both` |
| 18 | 7 | Recording date |
| 25 | 1 | File flags |
| 26 | 1 | File unit size |
| 27 | 1 | Interleave gap size |
| 28 | 4 | Volume sequence number, `u16both` |
| 32 | 1 | Identifier length `LEN_FI` |
| 33 | `LEN_FI` | Identifier |
| `33 + LEN_FI` | 0 or 1 | A zero pad byte when `LEN_FI` is even |
| then | rest | System use area |

### Framing rule

A directory is the extent `[extent, extent + data_length)`. Its data length
is a nonzero multiple of 2048, else `FSIMG_ECORRUPT`, and the extent lies
within the volume. Within it, at each position `p`:

* a zero byte at `p` ends the records of the block containing `p`: every
  remaining byte of that block must be zero, and the walk continues at the
  next block;
* otherwise `LEN_DR` must be even, at least `34`, at least
  `33 + LEN_FI + pad`, and must end within the block containing `p`; a
  record never crosses a block boundary;
* the record's volume sequence number is 1, else `FSIMG_EUNSUPPORTED`;
* `LEN_FI` is at least 1.

A violation is `FSIMG_ECORRUPT`. The system use area is the bytes from the
pad to `LEN_DR`; its meaning depends on the view.

### Flags

| Bit | Meaning | Rule |
| --- | --- | --- |
| `0x01` | Existence: hidden | Reported as `FSIMG_ATTR_HIDDEN`; the entry is delivered |
| `0x02` | Directory | The extent is a directory |
| `0x04` | Associated file | `FSIMG_EUNSUPPORTED` naming the entry |
| `0x08` | Record format | With an extended attribute record `FSIMG_EUNSUPPORTED`; without one `FSIMG_ECORRUPT` |
| `0x10` | Protection | As record format |
| `0x20`, `0x40` | Reserved | `FSIMG_ECORRUPT` when set |
| `0x80` | Multi-extent: another section follows | See [file sections](#file-sections); `FSIMG_ECORRUPT` on a directory |

A nonzero file unit size or interleave gap marks an interleaved file,
which is `FSIMG_EUNSUPPORTED` naming the entry; the content of an
interleaved file is not contiguous and is never read as if it were.
An associated file and an unsupported kind fail their directory's
preparation, as the [metadata design](design-metadata.md#entry-kinds)
requires for every object kind fsimg does not represent.

### Dot records

Every directory begins with two records whose identifiers are the single
bytes `0x00` (the directory itself) and `0x01` (its parent), in every
hierarchy, Joliet included. Both have the directory flag. The first
record's extent and data length must equal the directory's own. A
one-byte identifier `0x00` or `0x01` in any later record is a misplaced
dot record. The two records are structure, never entries.

The second record names the directory's **physical parent**, the directory
whose record names it in the hierarchy: the walker's parent, except for a
directory a Rock Ridge view reaches through
[relocation](#relocation), whose physical parent is its relocation
directory. The root's physical parent is the root. Three rules make every
hierarchy a tree that a walk reaches each directory of once, without a
record of where it has been:

1. a directory reached by an ordinary record has the walker's parent as
   its physical parent;
2. no child record names the root's extent;
3. no two child records in one directory open the same directory extent.

A directory then has exactly one possible parent and one record in it, so
a cycle or a directory shared between parents fails one of the rules. A
violation is `FSIMG_ECORRUPT`.

The root record in a descriptor is a 34-byte record with `LEN_FI = 1`,
identifier `0x00` and the directory flag, else `FSIMG_ECORRUPT`; its
extent is the root directory, whose first record must agree with it.

### File sections

A file is one record, or a run of consecutive records in one directory
that share the identifier, each but the last carrying the multi-extent
flag. The run is one entry with one logical file:

* the sections' data lengths sum, in 64 bits, to the file's size;
* every section but the last has a data length that is a nonzero multiple
  of 2048;
* every section's extent lies within the volume;
* a run ends with a section without the flag, before a record with
  another identifier and before the end of the directory, else
  `FSIMG_ECORRUPT`;
* each section with data starts at or after the end of the previous
  section with data, a profile rule that every producer meets, else
  `FSIMG_EUNSUPPORTED`, so a file's size never exceeds the volume.

A section of length zero holds no data, has no extent to check and takes
no part in the ascending rule; the writer never emits one. fsimg
admits at most 65535 sections per file, a profile bound above what an
8 TiB volume needs in maximal sections; more is `FSIMG_EUNSUPPORTED`. A
second run with the same identifier is a duplicate name, which namespace
validation rejects.

### Extended attribute records

A nonzero extended attribute length `n` places an `n`-block record at the
start of the extent, before the data, so a section's data starts at block
`extent + n` and the extent `[extent, extent + n + ceil(length / 2048))`
must lie within the volume. The reader skips the record without
interpreting it, so its owner, permission and time fields never become
entry metadata. A directory with an extended attribute record is
`FSIMG_EUNSUPPORTED`. The writer writes none.

## Path tables

Each hierarchy has a type-L path table, little-endian, and a type-M path
table, big-endian, listing its directories:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | Identifier length `LEN_DI` |
| 1 | 1 | Extended attribute record length |
| 2 | 4 | Extent block, `u32le` or `u32be` |
| 6 | 2 | Parent's record number, `u16le` or `u16be` |
| 8 | `LEN_DI` | Directory identifier; the root's is `0x00` |
| then | 0 or 1 | A zero pad byte when `LEN_DI` is odd |

Records are numbered from 1; record 1 is the root and its parent is 1. The
descriptor's path table size is the exact byte count of the records, and
both tables hold exactly that many bytes of whole records. The rules,
checked by full verification:

* both tables decode to the same records in the same order;
* an optional copy, when its address is nonzero, equals its table;
* every record but the root has a parent number below its own number,
  and parent numbers never decrease from one record to the next;
* within one parent, identifiers ascend under the hierarchy's
  [order](#ordering);
* the records are exactly the hierarchy's directories, each with its
  extent, its physical parent's extent through the parent number, and its
  identifier, as the directory records give them.

Listing and extraction never read a path table: the directory records are
the authority for traversal, and the path tables are a redundant index
that verification proves consistent. A hierarchy whose directories need
more than 65535 records is `FSIMG_EUNSUPPORTED` at verification, since
parent numbers are 16 bits.

## Names

### Primary identifiers

A primary identifier is `LEN_FI` bytes. The reader's native checks: no
byte is `0x00` or `0x2f` (`/`), and when a file identifier holds a `;`,
the last one is followed by a decimal version 1 through 32767 without
leading zeros. A violation is `FSIMG_ECORRUPT`. The reader does not
enforce the d-character set or the interchange level's length limits: the
level is not recorded on the volume, and producers write relaxed names
that every reader accepts. A byte the [byte-name profile](#displayed-names-and-keys)
refuses, such as one above `0x7e` that is not UTF-8, omits its entry. The
writer enforces the character set and the limits.

### Joliet identifiers

A Joliet identifier is big-endian UCS-2: its length is even, else
`FSIMG_ECORRUPT`. The reader decodes it as UTF-16BE, accepting surrogate
pairs, which producers write for names outside the Basic Multilingual
Plane. The code unit `0x0000` and the character `/` are `FSIMG_ECORRUPT`.
An unpaired surrogate is encoded in the generalized three-byte form that
UTF-8 forbids for surrogates; the result has no `0x00` or `/` byte, keeps
distinct identifiers distinct, and is not strict UTF-8, so the entry is
[omitted](design-directories.md#omitted-names). The reader accepts
identifiers longer than Annex C's 64 characters, up to the record's
capacity, because producers write them; the writer does not.

### Rock Ridge names

A Rock Ridge name is the concatenation of the record's `NM` entries. The
native checks: the name is 1 through 1024 bytes, holds no `0x00` or `/`,
and is neither `.` nor `..`; an `NM` with the current or parent flag
appears only on the dot records. A violation is `FSIMG_ECORRUPT`; a name
over 1024 bytes is `FSIMG_EUNSUPPORTED`. A record without `NM` takes its
[primary displayed name](#displayed-names-and-keys).

### Displayed names and keys

Every view admits children under the
[byte-name profile](design-directories.md#omitted-names): a component that
is not strict UTF-8, or holds a control byte or `0x7f`, omits its entry
rather than making the image unreadable. A Rock Ridge name written under a
host's 8-bit encoding is the common case.

In the `primary` and `joliet` views the displayed name is the identifier,
as UTF-8 for Joliet, without a final `;1`. A version other than 1 stays
(`NAME.TXT;2`), so two versions never merge, and a trailing `.` stays
(`README.`), because removing it would make a conforming file `README.;1`
and a directory `README` collide. Within one directory, identifiers must
differ once a final `;1` is removed, else `FSIMG_ECORRUPT`. In a
conforming primary hierarchy this always holds, since every file
identifier carries a `.` and a version and no directory identifier
carries either; in Joliet, where a version is not part of a name for any
reader, two such identifiers are the same name. In the `rockridge` view
the displayed name is the Rock Ridge name.

Each entry has one name key, its displayed name's exact bytes. In the
`primary` and `joliet` views `key` removes a final `;1` from a query the
same way, so `README.TXT;1` and `README.TXT` select one entry; a
`rockridge` query is its exact bytes. Keys do not fold case: a primary-view query
must spell `README.TXT`, since folding would make two relaxed names that
differ in case collide. A directory entry also has an **extent key**: a
zero byte, which no name contains, followed by the 32-bit block of the
directory it opens. Namespace validation rejects a repeated key, so a
repeated name and two records opening one directory, rule 3 of the
[dot records](#dot-records), fail through the same check without a
separate structure.

### Ordering

Directory records after the dot records, and path-table records with one
parent, are sorted. For the primary hierarchy, identifiers compare:

1. by the file name, the bytes before the `.` (a directory's whole
   identifier), the shorter padded with spaces (`0x20`), ascending by byte;
2. then by the extension, the bytes between the `.` and the `;`, padded
   with spaces, ascending;
3. then by the version, descending;
4. sections of one file stay in section order.

Joliet compares the same way over 16-bit code units, padding with
`U+0020`. Full verification checks that records never descend under these
rules; listing does not depend on them. ECMA-119 leaves identifiers these
rules find equal, such as a directory `FOO` and a file `FOO.;1`, in either
order, so the reader accepts both orders. The writer, which needs one
order for reproducible bytes, breaks such ties by the identifier's bytes,
ascending, which is policy and binds only the writer.

## System Use Sharing Protocol

### The handshake

SUSP is in use in a hierarchy when the system use area of its root's first
dot record begins with an `SP` entry: length 7, version 1, check bytes
`BE EF`, and a skip count. Without a well-formed `SP` at offset 0, the
hierarchy's system use areas are opaque bytes, never decoded.

Rock Ridge is present when SUSP is in use and the root dot record's entry
stream, continuations included, holds an `ER` entry whose identifier is in
the closed table `RRIP_1991A`, `IEEE_P1282`, `IEEE_1282`, with extension
version 1. Without one, the image does not offer the `rockridge` view. The
profile accepts a skip count of 0 only; a present Rock Ridge with another
skip count is a view fsimg cannot read: forcing it or verifying the image
is `FSIMG_EUNSUPPORTED`, and automatic selection passes it over for the
next view, which info reports. Once `SP` is well formed, a malformed entry
stream at the root is `FSIMG_ECORRUPT` whenever the handshake runs.

### Entry framing and continuation

Each entry is a two-byte signature, a length byte counting the whole entry
and at least 4, a version byte, and its contents. In a record's system use
area and in each continuation area, entries are decoded in order; an entry
must end within its area, else `FSIMG_ECORRUPT`. A zero byte where a
signature would start, or fewer than four bytes left, ends the area. `ST`
ends the stream. `PD` is padding. An unknown signature is skipped by its
length; one whose meaning would change content is not unknown, as the
[Rock Ridge table](#entries) states.

A `CE` entry, 28 bytes, gives a continuation area: block, byte offset and
length, each `u32both`. The area lies within one block, offset plus length
at most 2048, and its length is nonzero; the stream continues there after
the current area ends. An area that crosses a block is
`FSIMG_EUNSUPPORTED`, a profile rule that producers and the kernel readers
keep. A record's stream visits at most 16 continuation areas, beyond which
it is `FSIMG_EUNSUPPORTED`; an area that overlaps one already visited in
the same stream is `FSIMG_ECORRUPT`, so a continuation cycle cannot be
followed. An area holds at most one `CE`. Together the two bounds limit
one record's stream to its system use area and 32 KiB, whatever the image
holds.

## Rock Ridge

### Entries

In the `rockridge` view every record's system use area is decoded as a
SUSP stream. The entries:

| Entry | Length | Contents | Rule |
| --- | --- | --- | --- |
| `PX` | 36 or 44 | Mode, link count, uid, gid, and in the 44-byte form the serial number, each `u32both` | Another length is `FSIMG_ECORRUPT` |
| `PN` | 20 | Device number | Allowed only on a device, which is unsupported |
| `SL` | 5 or more | Flags, then components | [Symbolic links](#symbolic-links) |
| `NM` | 5 or more | Flags, then name bytes | [Rock Ridge names](#rock-ridge-names); bit 0 continues the name |
| `CL` | 12 | Child directory block | [Relocation](#relocation) |
| `PL` | 12 | Parent directory block | [Relocation](#relocation) |
| `RE` | 4 | Relocated-directory marker | [Relocation](#relocation) |
| `TF` | 5 or more | Flags, then times | Below |
| `RR` | 5 | Historical presence flags | Ignored |
| `SF` | Any | Sparse file | `FSIMG_EUNSUPPORTED` naming the entry |
| `ZF` | Any | zisofs compressed file | `FSIMG_EUNSUPPORTED` naming the entry |

Single-instance entries (`PX`, `PN`, `CL`, `PL`, `RE`, `TF`) appearing
twice in one stream are `FSIMG_ECORRUPT`. A known entry with a version
other than 1 is `FSIMG_ECORRUPT`.

`TF` flags select, in this order, creation, modification, access,
attribute change, backup, expiration and effective times; bit 7 selects
17-byte times instead of 7-byte ones. The entry's length must equal 5 plus
the selected count times the width, else `FSIMG_ECORRUPT`. Creation,
modification, access and attribute change become `created`, `modified`,
`accessed` and `changed`; the other three have no field and are decoded
only for validity.

### Kinds and metadata

`PX` mode's type bits give the kind: regular file, directory, symbolic
link or FIFO. A directory type must agree with the directory flag, and the
others must not have it, except on a relocation placeholder: a record
carrying `CL` is a directory in this view whatever its flag, and its `PX`
type, when present, must be directory. A disagreement is
`FSIMG_ECORRUPT`. A character device, block device or socket is
`FSIMG_EUNSUPPORTED` naming the entry. The permission bits `07777` are the
mode, and uid and gid the owner; the link count and serial number are not
interpreted. A record without `PX` takes its kind from the directory flag
and has no owner or mode. Times come from `TF`; a missing modification
time takes the recording date of the same record.

A directory's metadata is its record's in its parent, except the root's,
which comes from its own first dot record. Rock Ridge entries on a file's
later sections are framed and not interpreted; the first section's entries
describe the file.

### Symbolic links

A symbolic link's target is built from its `SL` entries, in order. Each
entry's components are records of a flags byte, a length and that many
bytes:

| Component flag | Meaning |
| --- | --- |
| `0x01` | Continue: the next component joins this one without a `/` |
| `0x02` | The component is `.` |
| `0x04` | The component is `..` |
| `0x08` | The component is the root `/` |
| `0x10`, `0x20` | Volume root and host: `FSIMG_EUNSUPPORTED` |

Components join with `/` between them, except that nothing is added after
a root component, so the components root and `a` give `/a`. The entry
flag's bit 0 says the next `SL` entry continues the target. The target is
1 through 1024 bytes, `max_link_target_bytes`, else `FSIMG_EUNSUPPORTED`;
an empty target, a component that overruns its entry, a flag byte with two
of `0x02`, `0x04` and `0x08`, a `0x00` byte, or a final entry still flagged
to continue is `FSIMG_ECORRUPT`. A symbolic link's size is its target's
length and its content the target, through the
[decoded link target](design-content.md#decoded-link-targets) of logical
content; the data length of its ISO record is not its size. fsimg never
resolves a target.

### Relocation

The primary hierarchy is at most eight levels deep, the root being level
1. A deeper Rock Ridge tree relocates directories: the directory is stored
under a relocation directory, usually named `rr_moved`, and its record
there carries `RE`; its intended parent holds a placeholder record, with
the directory flag clear, carrying `CL` with the relocated directory's
block; and the relocated directory's second dot record carries `PL` with
its intended parent's block.

In the `rockridge` view:

* a record with `CL` is a directory entry opening the extent `CL` names,
  which is not the root's; that directory's first dot record must name
  that extent, and its second dot record must carry `PL` naming the
  walker's parent, the directory holding the placeholder, while that dot
  record's extent names the directory's physical parent;
* a directory reached through an ordinary record must not carry `PL`, so
  every directory has exactly one way in: through `CL` when it carries
  `PL`, through its physical parent otherwise;
* a record with `RE` is not an entry: its directory is reached only
  through its placeholder;
* full verification requires every `RE` directory to be named by exactly
  one `CL`, whose target's physical parent is the directory holding the
  `RE` record, and every `CL` target to be such an `RE` directory.

With the [dot-record rules](#dot-records), these keep the view a tree: a
relocation loop or a directory reached both ways fails a check, and the
walker's depth limit bounds a long chain. The relocation directory itself
is an ordinary directory in the view; no name is special. In the
`primary` view `CL`, `PL` and `RE` do not exist: the placeholder is the
file its record describes, empty as producers write it, and the relocated
directory sits under its physical parent.

## El Torito

### Boot record

An El Torito boot record is a type-0 descriptor whose boot system
identifier, bytes 7 to 38, is `EL TORITO SPECIFICATION` followed by zero
bytes; bytes 39 to 70 are zero and bytes 71 to 74 hold the boot catalog's
block, `u32le`, within the volume. More than one El Torito boot record is
`FSIMG_EUNSUPPORTED`.

### Boot catalog

The boot catalog is a run of 32-byte entries starting at that block:

| Entry | Bytes | Rule |
| --- | --- | --- |
| Validation | 0 header `0x01`; 1 platform; 2-3 zero; 4-27 identifier; 28-29 checksum, `u16le`; 30-31 `55 AA` | The sixteen `u16le` words sum to zero modulo 65536 |
| Initial | 0 indicator `0x88` or `0x00`; 1 media type; 2-3 load segment; 4 system type; 5 zero; 6-7 sector count, `u16le`; 8-11 image block, `u32le`; 12-31 zero | Media type 0 to 4 |
| Section header | 0 `0x90` (more follow) or `0x91` (last); 1 platform; 2-3 entry count, `u16le`; 4-31 identifier | Count at least 1 |
| Section entry | As initial, with selection criteria in 12-31 | An extension flag in bit 5 of byte 1 is `FSIMG_EUNSUPPORTED` |

The validation entry and the initial entry come first. Section headers and
their entries follow until a header marked last has its entries; without
section headers, the boot catalog ends after the initial entry. The
profile bounds a boot catalog to its first block, 64 entries, beyond which
it is `FSIMG_EUNSUPPORTED`. Platform IDs 0 (x86), 1 (PowerPC), 2 (Mac) and
`0xEF` (EFI) are accepted; another is `FSIMG_EUNSUPPORTED`. A failed check
is `FSIMG_ECORRUPT`.

The sector count is in 512-byte virtual sectors and says what firmware
loads, not how long the image is. A boot image's extent therefore covers
at least `ceil(count * 512 / 2048)` blocks and at least one, and lies
within the volume. Boot entries are volume facts, reported by info and
checked by verification, never entries in a tree; a boot catalog or boot
image that is also a file in a hierarchy is that file's ordinary data.

## Ownership

Full verification checks that every block has at most one kind of owner.
The owners are the system area, each descriptor, each path table and its
copies, each directory, each continuation area, the boot catalog, and the
data: file sections, with their extended attribute records, and boot
images. The sharing rules:

* data may overlap data: Joliet and primary records name one file's
  extents, deduplicating producers share identical content, and a boot
  image is usually a file;
* a file whose one section is exactly the boot catalog's extent is the
  boot catalog published as a file, as `boot.cat` is;
* a directory or path table claimed again by the same hierarchy, from
  another walk or another reference, is the same object;
* continuation areas may share a block, but their byte ranges are
  disjoint, even between records;
* any other overlap, such as file data over a directory, a continuation
  area or a descriptor, is `FSIMG_ECORRUPT`.

A zero-length section claims nothing. No block needs an owner: unused
blocks are legal.

## Reader profile

| Value | Accepted | Otherwise |
| --- | --- | --- |
| Descriptor types | 0, 1, 2, 3, 255 | `FSIMG_EUNSUPPORTED` |
| Descriptor versions | 1; 2 for an enhanced type-2 descriptor | `FSIMG_EUNSUPPORTED` |
| Descriptors before the terminator | At most 64 | `FSIMG_EUNSUPPORTED` |
| Primary descriptors | At least one; later ones identical to the first | Missing `FSIMG_ECORRUPT`; differing `FSIMG_EUNSUPPORTED` |
| Joliet descriptors | At most one | `FSIMG_EUNSUPPORTED` |
| Logical block size | 2048 | `FSIMG_EUNSUPPORTED` |
| Volume set size, sequence numbers | 1 | `FSIMG_EUNSUPPORTED` |
| File structure version | 1 | `FSIMG_EUNSUPPORTED` |
| File unit size, interleave gap | 0 | `FSIMG_EUNSUPPORTED` |
| Associated files | None | `FSIMG_EUNSUPPORTED` |
| Extended attribute records | Skipped on files; none on directories | `FSIMG_EUNSUPPORTED` on a directory |
| Sections | At most 65535 per file, ascending without overlap | `FSIMG_EUNSUPPORTED` |
| `SP` skip count with Rock Ridge | 0 | The view is unsupported |
| `ER` identifiers for Rock Ridge | `RRIP_1991A`, `IEEE_P1282`, `IEEE_1282` | The view is absent |
| Continuation areas | Within one block; at most 16 per stream | `FSIMG_EUNSUPPORTED` |
| `PX` lengths | 36, 44 | `FSIMG_ECORRUPT` |
| Rock Ridge kinds | File, directory, symbolic link, FIFO | `FSIMG_EUNSUPPORTED` |
| `SF`, `ZF` | Absent | `FSIMG_EUNSUPPORTED` |
| Name and target lengths | At most 1024 bytes | `FSIMG_EUNSUPPORTED` |
| El Torito platforms | 0, 1, 2, `0xEF` | `FSIMG_EUNSUPPORTED` |
| El Torito boot catalog | One block, 64 entries; no extension entries | `FSIMG_EUNSUPPORTED` |

The reader profile is broader than the writer's: it accepts every level's
names, relaxed identifiers, long Joliet names, multi-extent files, both
`PX` lengths and every `ER` identifier listed, because qualified producers
write them.

## Writer profile

### Layout

The writer has one profile: a primary hierarchy at the selected
interchange level with Rock Ridge entries, a Joliet level 3 hierarchy, one
session, no extended attribute records, no interleaving, and El Torito only
when a boot image is named. Every hierarchy is written for every image:
one profile has one qualification matrix, Rock Ridge carries the exact
names and POSIX metadata, Joliet serves readers without Rock Ridge, and
each reader picks the view it wants. The blocks, in order:

| Blocks | Contents |
| --- | --- |
| 0 to 15 | System area, zero |
| 16 | Primary descriptor |
| 17 | El Torito boot record, when booting |
| next | Joliet descriptor |
| next | Terminator |
| next | Primary type-L table, then type-M table, then Joliet type-L and type-M tables, each starting on a block |
| next | El Torito boot catalog, one block, when booting |
| next | Primary directories in path-table order |
| next | Continuation blocks |
| next | Joliet directories in path-table order |
| next | File data, in catalog ID order, each file's sections contiguous |
| last | Zero blocks up to a requested volume size |

The El Torito specification places the boot record at block 17, which
fixes the order of the descriptors. The volume space size counts every
block, padding included. Every byte the layout does not fill is zero.
Directory extents, path tables, the boot catalog and file data start on
block boundaries, and the descriptors' path table sizes are the exact
unpadded byte counts. An empty file's record names block 0 and claims
nothing.

### Primary names

The level setting chooses the identifier limits:

| Level | File identifier | Directory identifier | Sections |
| --- | --- | --- | --- |
| 1 | Name at most 8 and extension at most 3 characters | At most 8 characters | One |
| 2 | Name plus extension at most 30 characters | At most 31 characters | One |
| 3 (default) | As level 2 | As level 2 | Several |

Characters are the d-characters `A` to `Z`, `0` to `9` and `_`. A file
identifier is `NAME.EXT;1`, with `.` present even when the extension is
empty; a directory identifier has no `.` or version. Each source component
gets its identifier in two steps, deterministic and independent of host
order, locale and budget:

1. **Natural.** A component made only of ASCII letters, digits and `_`,
   plus, for a file, at most one `.`, that fits the level once its letters
   are uppercased, has that uppercased spelling as its natural identifier.
   In one directory, the first child in catalog order keeps a natural
   identifier; a later child with the same one, which differs from it
   only in case, takes an alias.
2. **Alias.** Every other child takes an alias: its component with every
   byte outside letters, digits and `_` replaced by `_`, letters
   uppercased, the extension cut to 5 characters (3 at level 1) and the
   name cut to leave room, followed by `_` and a 7-digit uppercase
   hexadecimal number; at level 1 the name is the `_` and number alone.
   Kept natural identifiers whose name ends in `_` and seven hexadecimal
   digits reserve that number. Aliases take increasing numbers, from 1, in
   catalog order of the children that need them, skipping reserved ones.

An alias can equal no other identifier: two aliases differ in their
numbers, and a natural identifier of the same shape reserved its number.
Seven digits number more children than a directory can hold, since a
directory's records fit in 32-bit lengths. Every file identifier carries a
`.` and a version and no directory identifier does, so identifiers stay
distinct after the reader removes `;1`. As for FAT's
[alias family](design-fat-writer.md), this needs no repeated search.

### Joliet names

A component is its own Joliet identifier when it encodes to UCS-2 without
surrogates, holds no control character and none of `*`, `/`, `:`, `;`,
`?` and `\`, and fits 64 code units as a directory or 62 as a file, which
then adds `;1`, so every identifier holds at most 128 bytes. Otherwise it
takes an alias by the primary rule over code units: unrepresentable
characters become `_`, the name is cut to fit, and `_` and seven
hexadecimal digits follow, from a family whose kept natural identifiers
reserve their numbers. Natural identifiers are source names, which are
unique in a directory, so identifiers stay distinct after the reader
removes `;1`, and the writer's byte tie-break orders `foo;1` and `foo.;1`. Joliet identifiers are case-sensitive, as on every reader but
Windows. Symbolic links and FIFOs are not in the Joliet hierarchy, which
cannot describe them. Annex C lifts the eight-level limit for Joliet, so
the Joliet tree is the logical tree at full depth; the writer does not
apply Annex C's 240-byte path accounting, which no qualified reader
enforces.

Neither projection is a metadata loss: the Rock Ridge names of the primary
hierarchy are the image's exact names, and the projections are the
format's own derived identifiers.

### Rock Ridge entries and continuation

The writer writes Rock Ridge as RRIP 1.10 defines it: the `ER` identifier
`RRIP_1991A` with the specification's description and source texts, and
the 36-byte `PX`. Every Rock Ridge reader accepts that combination, and
the serial number of the 44-byte form has no use without hard links.
Each record's entries, in this order:

| Record | Entries |
| --- | --- |
| Root's first dot record | `SP`, `ER`, `PX`, `TF` |
| Every other dot record | `PX`, `TF`; and `PL` on a relocated directory's second dot record |
| A file, link or FIFO | `PX`, `TF`, `NM`; `SL` for a link |
| A directory in its parent | `PX`, `TF`, `NM`; `RE` when relocated |
| A relocation placeholder | `PX` of the directory, `TF`, `NM`, `CL` |

A Rock Ridge name is at most 255 bytes, the component limit of every
target host, else `FSIMG_EUNSUPPORTED`. `NM` entries carry at most 250
name bytes each, with the continue flag on all but the last; `SL` entries
carry whole components, a component longer than an entry continuing in
the next, at most 255 bytes per entry. Link counts are 1, or 2 plus the
subdirectory count for a directory. `TF` carries modification, and access
and attribute change when the entry has them, in 7-byte form.

A record holds at most 254 bytes. When its entries do not fit, the record
keeps the longest prefix of them that fits with a 28-byte `CE` after it,
and the rest form a chain of continuation fragments: each fragment takes
the longest run of the remaining entries that fits 2048 bytes, keeping 28
bytes for a `CE` to the next fragment when more remain. Entries are never
split, so a 255-byte name and a 1024-byte target, even one made of
slashes, fit in at most three fragments, well inside the reader's
16-area bound. Fragments are placed in record emission order by next fit:
a fragment goes at the current position in the current continuation block
when it fits there, and at the start of the next block otherwise, never
crossing a block. Every entry's size is fixed before any address is known,
since addresses have fixed widths, so sizing is one pass without
iteration. Rock Ridge entries are written on a file's first section only.

### File sections

A file larger than `2^32 - 1` bytes is `FSIMG_EUNSUPPORTED` at levels 1 and
2. At level 3 it is written as sections of `0xFFFFF800` bytes, the largest
whole-block length below `2^32`, and a final section with the rest when
the rest is nonzero, all contiguous and ascending, each a record with the
same identifier in both hierarchies.

### Deep trees and relocation

The writer keeps the primary hierarchy within ECMA-119's bounds: eight
levels and 255-byte paths. A directory is relocated when its physical
level would be 9 or more, or when its physical path, identifiers and
separators, would leave no room for its longest possible child: more than
255 bytes less the separator and the level's longest file identifier.
A relocated directory is written under `RR_MOVED` in the root with `RE`,
at level 3, its subtree continuing below it and relocated again by the
same rule; its logical parent holds a placeholder file record with `CL`,
under the identifier the directory would have had there; and its second
dot record carries `PL`. In `RR_MOVED` every relocated directory takes an
alias, numbered in catalog order, so its identifier's length, and with it
every physical path below it, follows from its name alone, before any
number is assigned. The Joliet hierarchy holds the logical tree and no
relocation directory.

The relocation directory's Rock Ridge name is `.rr_moved` and its
existence flag is set. A root child naturally named `RR_MOVED` takes an
alias. When the source root already holds an empty directory whose name
is `.rr_moved`, as extracting a relocated image yields, it is the
relocation directory and keeps its metadata, so such a tree round-trips;
any other root child of that name, when relocation is needed, is
`FSIMG_EUNSUPPORTED`.

### El Torito

Booting is opt-in through two settings naming regular files of the tree
by image path: `boot` for an x86 BIOS image and `efiboot` for an EFI image.
Each named file must be a non-empty regular file of one section, else
`FSIMG_EINVAL` naming the setting. The writer emits the boot record and
one boot catalog block:

* a validation entry whose platform is x86 when `boot` is given and EFI
  otherwise, with a zero identifier;
* an initial entry, bootable, no emulation, load segment 0 and system type
  0, for the BIOS image when it is given, with the sector count
  `bootload`, default 4, which must not reach past the image's blocks,
  else `FSIMG_EINVAL`; otherwise for the EFI image;
* when both are given, a last section header for platform `0xEF` with one
  entry for the EFI image.

An EFI entry's sector count is the image's length in 512-byte sectors,
rounded up, when that fits 16 bits, and 0 otherwise. A boot image is an
ordinary file of the tree and its entries point at that file's data; the
boot catalog is not a file. The writer never modifies a boot image: it
writes no boot information table, the patch some BIOS loaders expect at
offset 8. Writing a hybrid MBR or GPT into the system area, emulation boot
and boot images outside the tree are out of scope. fsimg checks the boot
catalog's integrity, not that firmware runs the image.

### Settings and sizing

| Setting | CLI | Rule |
| --- | --- | --- |
| `iso_level` | `level=1|2|3` | Default 3 |
| `boot_bios` | `boot=PATH` | Absent by default; an absolute image path |
| `boot_efi` | `efiboot=PATH` | Absent by default; an absolute image path |
| `boot_load_sectors` | `bootload=N` | 1 to 65535, default 4; only with `boot` |
| `label` | `-L` | At most 16 characters of the Basic Multilingual Plane without controls; NULL selects `CDROM`, and the CLI passes its own default |

The label is the Joliet volume identifier exactly, and the primary volume
identifier as its d-character projection: letters uppercased and every
other character outside the set replaced by `_`, space padded. The
capability record's `max_label_bytes` is 32, the Joliet field's bytes.
`sector_bytes` and `allocation_unit_bytes` are zero or 2048.
`min_free_bytes` must be zero, since the format has no free space, else
`FSIMG_EINVAL`. `volume_bytes` zero sizes the image exactly; a nonzero
value must be a multiple of 2048, else `FSIMG_EINVAL`, and at least the
exact size, else `FSIMG_ERANGE`, and pads the volume with zero blocks it
counts. Minimization chooses nothing: the size is already exact. The
descriptor does not declare an exact volume, so a scheme or padding
container may hold an ISO volume.

### Times and defaults

Every record's recording date is the entry's modification time in UTC
with offset 0; `TF` carries the entry's times the same way. A time outside
1900 to 2155, a sub-second part or an offset basis that is not UTC is
encoded under the [loss policy](design-metadata-loss.md) as a counted
optional loss; an absent time takes the fallback. Reproducible mode makes
every time and both volume dates the epoch, and refuses an epoch outside
the 7-byte range with `FSIMG_EINVAL`; otherwise the volume dates and the
fallback are the planning clock. Absent owner and mode take uid and gid 0
and mode `0755` for directories, `0644` for files and `0777` for links
and FIFOs, as for the QNX formats. Expiration and effective dates are
absent. The application identifier is `FSIMG`; the other identifier
fields are spaces.

## Deterministic planning and emission

Every byte of a written image derives from the catalog, the options and
the rules above. Identifiers, sort order, record packing, continuation
placement, path-table numbering and block assignment are pure functions of
catalog order and content, so byte-identical output does not depend on
source order, memory or scratch budgets, sort fan-in, scratch placement,
locale or time zone. Planning computes the whole layout before the sink is
touched; emission writes each structure once and never patches bytes
already written.

## Resource and failure requirements

The reader holds no tree, path table or directory in RAM. Opening reads
the system area, at most 65 descriptor blocks with the terminator, the
boot catalog's block, and the primary root's first block with its at most
16 continuation areas. Preparing a directory reads its extent once and
each continuation area of its records; replay reads a record again through
a one-block window. Verification's claims, path-table records and
relocation matches go to scratch streams sorted externally. The writer's
per-entry state is paged rows and scratch streams. Every loop charges the
session and observes cancellation.

Failure classes:

| Class | Examples |
| --- | --- |
| `FSIMG_EFORMAT` | No `CD001` descriptor at block 16; a volume shorter than 17 blocks |
| `FSIMG_ECORRUPT` | Disagreeing copies of a `both` field; a referenced extent outside the volume or the slice; a record crossing a block; nonzero block tail padding; a dot-record rule; a broken section run; identifiers equal after `;1`; malformed SUSP or Rock Ridge entries; a continuation overlap; a relocation mismatch; inconsistent path tables; unsorted records; an ownership conflict; a bad El Torito checksum |
| `FSIMG_EUNSUPPORTED` | A profile value outside its table; a forced view the image lacks; a device, associated or interleaved file; zisofs or sparse files |
| `FSIMG_EINVAL` | A selection component the view cannot represent; a writer setting outside its rules |
| `FSIMG_ERANGE` | A requested volume smaller than the image; an image beyond `2^32 - 1` blocks |
| `FSIMG_ELIMIT` | Budget, scratch, work, depth or file-descriptor exhaustion |

## Acceptance matrix

Each row has a valid witness and a mutation that must fail with the class
shown, when reading unless the row says at verification.

| Area | Valid witnesses | Mutations and expected classes |
| --- | --- | --- |
| Probe | PVD at block 16; boot record at 16 and PVD at 17; the smallest volume each producer writes | No `CD001` (EFORMAT); 16 blocks (EFORMAT); block size 512 (EUNSUPPORTED); disagreeing halves (ECORRUPT) |
| Descriptor set | PVD, SVD, boot record, partition, enhanced, terminator in any legal order; trailing UDF recognition sequence | No terminator within 64 (EUNSUPPORTED); missing PVD (ECORRUPT); nonzero terminator byte (ECORRUPT); two Joliet SVDs (EUNSUPPORTED); differing second PVD (EUNSUPPORTED) |
| Geometry | Slice longer than the volume; slice missing unreferenced tail blocks | An extent past the volume by one block, and with a length near `2^32` blocks (ECORRUPT); a referenced block past a short slice (ECORRUPT) |
| Records | Records filling a block exactly; empty-tail blocks; 2048-byte boundaries | Odd length, length 33, crossing a block, nonzero tail padding, `LEN_FI` past the record (ECORRUPT); reserved flags (ECORRUPT); interleave (EUNSUPPORTED) |
| Dot records and tree | Root and nested directories | Wrong self or parent extent (ECORRUPT); a child naming the root (ECORRUPT); two records opening one directory (ECORRUPT); a misplaced dot identifier (ECORRUPT) |
| Sections | Single section; two and many sections; final section of 0 and of `2^32 - 1` bytes; a >4 GiB sparse fixture | Missing last section, changed identifier, flag on a directory, unaligned middle section (ECORRUPT); descending or overlapping sections, 65536 sections (EUNSUPPORTED) |
| Extended attributes | One- and two-block records before data | Record past the extent (ECORRUPT); on a directory (EUNSUPPORTED) |
| Path tables | Both tables and copies for each hierarchy; 65535 directories | L and M disagree, wrong parent, unsorted, missing or phantom directory, size disagreement (ECORRUPT at verification) |
| Names | `A.TXT;1`, `A.TXT;2`, `README.;1` beside directory `README`, relaxed lowercase identifiers, long Joliet names, non-BMP Joliet names, Rock Ridge UTF-8 names | `A.TXT` beside `A.TXT;1` (ECORRUPT); version 0 or 32768 (ECORRUPT); odd Joliet length (ECORRUPT); unpaired surrogate (omitted); Latin-1 Rock Ridge name (omitted) |
| Order | Sorted directories and path tables, versions descending, `FOO` and `FOO.;1` in either order | Swapped unequal neighbours (ECORRUPT at verification) |
| SUSP | `SP` alone; `ER` in a continuation; 16 continuation areas; unknown entries | Bad check bytes at root (opaque, view absent); length below 4 or past the area (ECORRUPT); 17 areas, an area crossing a block (EUNSUPPORTED); self-referencing or overlapping continuation (ECORRUPT); nonzero skip (EUNSUPPORTED for the view) |
| Rock Ridge | Every kind; 36- and 44-byte `PX`; 1024-byte name and target; `NM` and `SL` across continuations; short and long `TF` | Type disagreeing with the flag, duplicate `PX`, `TF` length mismatch, dangling `SL` continuation, `NM` with a slash (ECORRUPT); device (EUNSUPPORTED); `ZF`, `SF` (EUNSUPPORTED) |
| Relocation | Depths 9 to 20 through one and several relocations; placeholders with a directory `PX` | `CL` to a non-directory, missing `PL`, `PL` on an ordinary directory, `RE` without `CL`, a loop (ECORRUPT) |
| Joliet | Levels 1 to 3; shared data with the primary hierarchy | Bad escape (not Joliet); geometry differing from the PVD (ECORRUPT); a forced `joliet` view over a damaged primary handshake (reads) |
| El Torito | BIOS only, EFI only, BIOS and EFI, boot image as a file and not | Checksum, signature, media type, count past the boot catalog, image past the volume (ECORRUPT); unknown platform, extension entry, a second block of entries (EUNSUPPORTED) |
| Ownership | Primary and Joliet sharing data; `boot.cat` as a file; deduplicated files | File data over a directory, continuation or descriptor; two records sharing continuation bytes (ECORRUPT) |
| Writer | Every acceptance row's valid witness that the writer profile can express, at each level; reordered sources; fixed sizes; relocation by depth and by path length; a round trip of an extracted relocated tree | Required metadata loss; a level 1 or 2 file above `2^32 - 1` bytes; a 256-byte Rock Ridge name; a boot setting naming a directory or a missing path; `bootload` past the image; a volume size too small |

## Source keys

* ECMA-119 6th edition, sections on the volume descriptor set, primary and
  supplementary descriptors, directory records, path tables, file
  identifiers and their ordering, recording dates, and Annex C (Joliet).
* IEEE P1281 draft 1.12: `SP`, `CE`, `ST`, `PD`, `ER`, `ES`.
* IEEE P1282 drafts 1.10 and 1.12: `PX`, `PN`, `SL`, `NM`, `CL`, `PL`, `RE`,
  `TF`, `SF`.
* El Torito Bootable CD-ROM Format Specification 1.0: the boot record,
  validation entry, default entry, section header and section entry.
* The research draft [fsimg_iso9660_spec.md](../fsspecs/fsimg_iso9660_spec.md)
  for the test families and oracle matrix.
