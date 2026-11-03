# Device nodes and sockets

## Purpose and scope

Character devices, block devices and sockets are entry kinds fsimg reads,
lists, verifies and records, and never creates. This document extends the
[POSIX metadata design](design-metadata.md) with the three kinds and the
one field they add, the device number, and states what each reader, the
CLI, the extractor, the manifest writer and the writer side do with them.
The kinds are read-only by contract: no writer stores them, and the
catalog, the manifest parser and the host scanner refuse them, so the
writer side needs no rule beyond the ones it already has.

## Problem

A POSIX filesystem image routinely holds a `dev` directory. The FreeBSD 1.0
installation CD, an ISO 9660 image with Rock Ridge extensions, lists 71
entries and then fails: its `dev` directory holds character devices, and
under the common entry model a directory containing any kind outside file,
directory, symbolic link and FIFO fails its preparation with
`FSIMG_EUNSUPPORTED` naming the first such entry. One device hides the rest
of the disc from `t`, `x` and `W`. The same rule is written into every
format design that can store a device: ISO 9660 Rock Ridge, UDF, HFS Plus
and APFS. Extracting such an image is a different question: the safe
extractor creates regular files and directories only, and already skips or
refuses symbolic links and FIFOs, so reading these kinds does not change
what reaches the host.

## Where the code lives

* The kinds and the field are in `include/fsimg.h`; `FSIMG_ENTRY_KNOWN`,
  `FSIMG_META_KNOWN` and `fsimg_device_number_fits()`, the field's one
  rule, are in `lib/core/internal.h`.
* The Rock Ridge decoder (`lib/formats/iso9660/rockridge.c`) decodes `PX`
  types and the `PN` entry.
* The catalog (`lib/write/catalog.c`, `entry_shape_valid()`) holds the
  closed relation between kind, size and source for the writable kinds, and
  `fsimg_metadata_validate()` (`lib/core/unicode.c`) the metadata rules. The
  manifest parser (`lib/source/mtree.c`) refuses `type=block`, `type=char`
  and `type=socket`, naming the keyword. The host scanner opens every
  object no-follow and nonblocking and rejects anything but a regular file
  or directory; a socket or driverless device fails its open with `ENXIO`,
  reported as `FSIMG_EUNSUPPORTED`.
* The reader's replay funnel (`lib/read/directory.c`) checks every entry a
  driver replays before publishing it.
* The extractor (`lib/extract/extract.c`) creates files and directories and
  routes every other kind through `skip_unsupported()`. The selector
  (`lib/read/selection.c`) refuses a single `x -O` member that is not a
  file or symbolic link.
* The worker's entry record and its validation are in
  `cmd/fsimg/protocol.c` (`fsimg_wire_entry_frame_valid()`); listings and
  JSON in `cmd/fsimg/display.c`; manifest lines in
  `lib/extract/mtree-writer.c`.

## Design

### Three more kinds, one more field

The three kinds join `fsimg_entry_kind_t`, continuing the bit sequence, with
the same shape as a FIFO: size zero, no content, no source.

| Kind | Value | Size | Content |
| --- | --- | --- | --- |
| `FSIMG_ENTRY_CHAR_DEVICE` | 16 | Zero | None |
| `FSIMG_ENTRY_BLOCK_DEVICE` | 32 | Zero | None |
| `FSIMG_ENTRY_SOCKET` | 64 | Zero | None |

`fsimg_metadata_t` gains one validity bit and the field it governs:

| Bit | Field | Meaning |
| --- | --- | --- |
| `FSIMG_META_DEVICE` | `dev_major`, `dev_minor` | The stored device number as two `uint32_t` words, the format's major and minor as the format defines them |

The field is valid only on a character or block device; a reader sets it
exactly when its format stores a device number for the entry. A socket
never carries it. Like every field, an absent one has no value, so a device
whose format stores no number is simply a device without a number.

A pair of 32-bit words is the representation because every format that
stores a device number stores one of three things, each a pair or smaller:
UDF stores major and minor as two 32-bit identifiers; HFS Plus and APFS
store a 32-bit macOS `dev_t`, whose split is Apple's and fixed; Rock Ridge
stores two 32-bit words that every known writer fills with major and minor.
A single opaque 64-bit value would merge UDF's and Rock Ridge's two words
into one number the user has to split again, and would still need the split
rules to display `major, minor` the way `ls` does. The pair carries the
same information with the decoding stated once per format below. fsimg
never uses the numbers: they are displayed, serialized and recorded, and
nothing derives a host path, a `mknod()` argument or an `ioctl()` from
them.

### Read-only by construction

The writer side rejects these kinds through rules that already exist, so
the read-only contract needs no new mask or option; the one new rule is
the device number field's:

* The catalog's closed kind, size and source relation lists the four
  writable kinds. The three new kinds are not added to it, so
  `fsimg_writer_add()` with one of them fails with `FSIMG_EINVAL`,
  "entry kind, size and source disagree", as any non-storable kind value
  does. A catalog therefore never carries a device or socket, no writer
  receives one, and common planning needs no case for them.
* The field's rule, `FSIMG_META_DEVICE` only on a character or block
  device, is one predicate applied at each boundary an entry crosses: the
  catalog's metadata validation, the reader's replay funnel and the
  parent's frame validation. Since no catalog kind is a device, the catalog
  refuses the field on every entry with `FSIMG_EINVAL`, "a device number is
  valid only on a device", and its record needs no slot for it. A reader
  driver that sets the field on another kind fails the walk with
  `FSIMG_ESTATE`, so a listing, manifest or verification never shows a
  device number on a file.
* `fsimg_format_info_t.entry_kinds` keeps its one meaning for the writable
  kinds, read and written. For the three read-only kinds it means the
  format's reader reports them; `--formats` lists them with the others. No
  writer-side check consults the mask for a kind the catalog cannot hold.
  The format-independent capability test asserts that a format listing a
  device kind also lists `FSIMG_ENTRY_FILE`, and that no format lists
  `FSIMG_META_DEVICE` in `metadata_fields`, since no writer stores it.
* The manifest parser keeps refusing `type=block`, `type=char` and
  `type=socket` with its existing message, "block, char and socket entries
  are not stored". The `device` keyword stays in the host-object class:
  validated as well-formed, never consulted.
* The host scanner keeps its behaviour from the
  [source-root profile](design-security.md#source-root-profile): a device or
  socket is rejected after or during its open as `FSIMG_EUNSUPPORTED`, with
  the open's effects bounded by the content directory's authority as that
  profile states. Image devices come from images alone.

### Readers

Each reader whose format stores devices or sockets reports a well-formed one
as an entry of the kind with the mode, owner and times it reports for a
FIFO. The structural checks the format applies to a FIFO apply unchanged:
no content, forks or allocation claimed as the format design says. A
malformed one keeps its corruption class. `W` verifies a well-formed device
or socket as valid, and the verification report counts it among `files`
with zero logical bytes, like a FIFO.

| Format | Kinds | Device number |
| --- | --- | --- |
| ISO 9660, `rockridge` view | `PX` types `0020000` character, `0060000` block, `0140000` socket | `PN`: `dev_major` is the high word, `dev_minor` the low word, as stored. RRIP names the words the high and low halves of a 64-bit `dev_t`; mkisofs and its descendants write major and minor, and a system with a 32-bit `dev_t` writes zero and its packed number, which fsimg reports as major 0 and the packed value without guessing the recording system's split. A device without `PN`, or `PN` on another kind, is `FSIMG_ECORRUPT`; RRIP requires `PN` on exactly the devices |
| UDF | File types 7 character, 6 block, 10 socket | The device specification extended attribute (type 12, subtype 1): after its 12-byte header, an implementation-use length (u32), the major (u32) and minor (u32) identifiers, then the implementation-use bytes the length covers, all within the attribute. Absent on a device, present on another kind, or misframed: `FSIMG_ECORRUPT` |
| HFS Plus | `fileMode` types `020000` character, `060000` block, `0140000` socket | `special` on a device is the macOS `dev_t`: `dev_major` is bits 24 to 31, `dev_minor` bits 0 to 23. On a socket `special` is not interpreted. Both forks are empty, else `FSIMG_ECORRUPT`, as for a FIFO |
| APFS | Mode types and directory-record types 2 character, 6 block, 12 socket | The device number extended field (type 14, 4 bytes), the macOS `dev_t` split as for HFS Plus. On another kind it is `FSIMG_ECORRUPT`; a device without it is reported without a number, since Apple's format reference does not require the field |
| QNX4, QNX6, ImageFS | None | The on-disk type bits can name a device, but QNX keeps devices in the process manager's namespace, no native tool records one in these filesystems, the inodes have no device-number field, and fsimg holds no observation of such a record. The types stay `FSIMG_EUNSUPPORTED`, as their designs state |
| FAT, exFAT | None | The formats cannot express them |

The `joliet` and `primary` ISO views carry files and directories only, as
today: a device's record is a zero-length file there, as a FIFO's is.

### Command line and worker protocol

The default `t` listing follows `ls -F`: a socket is marked `=` after the
quoted name, a device is not marked, as `ls -F` marks neither. The `-v`
listing's kind character is `c`, `b` or `s`, and its size column shows
`major, minor` (for example `4, 2`) right-aligned when `FSIMG_META_DEVICE`
is valid and `-` otherwise, so a socket and a device without a number show
`-` like a directory or FIFO. `--json` entry records carry `kind` as
`char_device`, `block_device` or `socket`, and `dev_major` and `dev_minor`
as numbers when the field is valid and `null` otherwise, beside `uid`,
`gid` and `mode`. `--formats` names the kinds in the human and JSON
capability output in `FSIMG_ENTRY_*` order.

The worker-to-parent entry record gains two 8-byte fields, `DEV_MAJOR` and
`DEV_MINOR`, after `MODE`, so `FSIMG_WIRE_ENTRY_BYTES` grows by 16. The
parent's frame validation admits the three kinds with a zero length and no
content, permits `FSIMG_META_DEVICE` in `valid` only on a character or
block device, and requires both words to fit 32 bits; a record breaking any
of these is an invalid frame, which fails the job as a protocol error, as
an unknown kind does today. `x -O` sends content entries only, so the
content-only check already excludes them.

### Extraction and the manifest

The extractor's rule is unchanged: it creates regular files and directories
and nothing else, under every profile. A device or socket fails the
extraction with `FSIMG_EUNSUPPORTED` naming the entry, or under
`FSIMG_EXTRACT_SKIP_UNSUPPORTED` (`-k`) is skipped, counted in
`skipped_entries`, named in the verbose listing and recorded in the
manifest, exactly as a symbolic link or FIFO is. The skip count, its
warning and its JSON summary field are the existing ones; no new count,
option or report appears. `x -O` of a device or socket fails as for a
directory or FIFO: `FSIMG_EINVAL`, "selected image path has the wrong entry
kind", naming the member, because the kind has no content.

`x --mtree` records the entry in mtree(5) syntax:

```text
./dev/null type=char mode=0666 uid=0 gid=0 time=946684800.000000000 device=native,1,3
./dev/sd0 type=block mode=0640 uid=0 gid=5 time=946684800.000000000 device=native,4,0
./var/run/log.sock type=socket mode=0666 uid=0 gid=0 time=946684800.000000000
```

`type` is `char`, `block` or `socket`, the mtree(5) names. `device` is
written when `FSIMG_META_DEVICE` is valid, in the keyword's
`format,major,minor` form with the format `native`, the form bsdtar writes
and FreeBSD, NetBSD and macOS `mtree(8)` read; the numbers are the entry's
words in decimal. A device without a number has no `device` keyword, as a
field the entry does not carry is left out. A socket never has one.
Reading such a manifest back with `c @manifest` fails at the `type`
keyword, as the writer-side rule states, which keeps the manifest an exact
record of a tree fsimg cannot recreate.

### Security

The device number is untrusted image data with one use, display. It is
bounded to two 32-bit words at the reader, checked again by the parent's
frame validation, and formatted as decimal text; it never reaches
`mknod()`, `ioctl()`, a host path or a filesystem query. The extractor's
file-and-directory-only contract is untouched, so an image can name any
device number without gaining any authority on the host. A reader's
structural checks on a device or socket are the FIFO's: a stored size,
fork or allocation outside the format's rule is `FSIMG_ECORRUPT`, so the
kinds add no new way to make a reader deliver or claim bytes.

## Refusals and error results

| Case | Result |
| --- | --- |
| A well-formed device or socket in any supported view | Reported; valid under `W` |
| Rock Ridge device without `PN`, or `PN` on a non-device | `FSIMG_ECORRUPT` |
| UDF device without the device specification attribute, or the attribute on a non-device or misframed | `FSIMG_ECORRUPT` |
| HFS Plus device or socket with a non-empty fork | `FSIMG_ECORRUPT` |
| APFS device number field on a non-device | `FSIMG_ECORRUPT` |
| QNX4, QNX6, ImageFS type bits naming a device or socket | `FSIMG_EUNSUPPORTED`, the directory's preparation fails naming the entry, unchanged |
| `fsimg_writer_add()` with a device or socket kind | `FSIMG_EINVAL`, the catalog's kind, size and source relation |
| `fsimg_writer_add()` with `FSIMG_META_DEVICE` valid | `FSIMG_EINVAL`, the catalog's metadata validation |
| A reader driver reporting `FSIMG_META_DEVICE` on another kind | `FSIMG_ESTATE` from the replay funnel |
| Manifest `type=char`, `type=block`, `type=socket` | `FSIMG_EUNSUPPORTED` naming the keyword, unchanged |
| Host source tree holding a device or socket | `FSIMG_EUNSUPPORTED` from the scanner, unchanged |
| Extraction reaching a device or socket | `FSIMG_EUNSUPPORTED` naming the entry; with `-k` skipped and counted |
| `x -O` naming a device or socket | `FSIMG_EINVAL`, wrong entry kind, naming the member |
| Entry frame with a device field on another kind, a word above 32 bits, or a nonzero length | Invalid frame; protocol failure |

## Tests

Each passing case has a seeded failure that proves the check ran.

* `test/posix.c`: the capability test over every registered record asserts
  the device-kind and `metadata_fields` relations above; `fsimg_writer_add()` of
  each new kind fails with `FSIMG_EINVAL` for every writer, and the seeded
  failure accepts one; so does a file carrying `FSIMG_META_DEVICE`.
* `test/iso9660.c` and `test/iso9660-faults.c`: a synthesized Rock Ridge
  image with a character device, a block device and a socket beside the
  other kinds lists all of them with kind, owner, mode and times; a `PN`
  with high word 4 and low word 2 reads `4, 2`; one with high word 0 and
  low word `0x0402` reads `0, 1026`; one with both words `0xffffffff`
  reads both at `4294967295`; a device without `PN` and a `PN` on a file
  are `FSIMG_ECORRUPT`; a `PN` of another length is `FSIMG_ECORRUPT`; the
  socket has no device field; `W` passes on the valid image and fails on
  each mutation. The oracle corpus adds an xorriso-built disc with a
  `dev` directory mapping existing host nodes by `stat` alone and a
  socket from its private source tree, compared with the independent
  listing for kind and with the mapped node's host `rdev`, as libisofs
  stores it in `PN` (high and low 32 bits), for the device number.
* The UDF, HFS Plus and APFS synthesizers, when those readers exist, add
  the equivalent witnesses from each table row and mutations: the attribute
  absent, on a file and misframed; a non-empty fork on a device; the
  extended field on a file; a device without the field reporting no number.
* `test/qnx4.c`, `test/qnx6.c`, `test/ifs.c`: a device type in the mode bits
  still fails preparation with `FSIMG_EUNSUPPORTED`.
* `test/protocol.c`: entry frames of each new kind validate with zero
  length; a frame with a nonzero length, with `FSIMG_META_DEVICE` on a
  socket or a file, or with a word above `UINT32_MAX` is rejected.
* `test/cli/`: `t` marks a socket `=` and a device not at all; `t -v` shows
  `c`, `b`, `s` and `4, 2` or `-` in the size column; `t --json` carries
  the kind names and the device words or nulls; `--formats` lists the kinds
  for ISO 9660 and not for FAT; `x` without `-k` fails naming the device
  and `x -k` skips it, counts it, names it under `-v` and records it in the
  manifest with `type=char` and `device=native,4,2`; `x -O` of the device
  fails with the wrong-kind message; `c @manifest` of the written manifest
  fails at `type=char`.
* `test/mtree.c`: the writer's device lines parse back as well-formed mtree
  up to the refused `type` keyword, so the syntax is the one the parser
  knows.

## Decisions

* **Read-only kinds, stated as a contract.** No writer stores them because
  nothing fsimg creates should carry a device number: a device node in a
  new image is an authority statement about a target system, not content,
  and fsimg has no evidence base for any format's native device layout
  beyond reading it. Keeping the catalog's closed relation as the one
  refusal point removes the kinds from every writer and planner at once.
* **A major and minor pair, decoded per format.** Chosen over a raw opaque
  value because three of the four formats store a pair or a value with a
  fixed, format-owned split, and because the pair is what every listing
  tool shows. The one ambiguity, a Rock Ridge `PN` whose high word is
  zero, is reported as stored rather than guessed: the recording system's
  packing is not in the image.
* **No marker for devices in the default listing.** `ls -F` marks sockets
  `=` and devices not at all; inventing a marker would mean one more thing
  for users to learn for a kind they see in `-v` and JSON anyway.
* **No extraction option to create them.** Creating a device node or a
  socket on the host needs privilege or a bound socket and gives an image
  a say over host device access, against the safe profile. A caller who
  needs them has the manifest.
* **A zero-high-word `PN` is not split.** The Linux and FreeBSD Rock Ridge
  readers treat a zero high word as a packed 32-bit `dev_t` and split the
  low word as `low >> 8` and `low & 0xff`, so the FreeBSD 1.0 CD's devices
  would list as `4, 2` rather than `0, 1026`. That rule is silently wrong
  for a disc mkisofs writes on a modern FreeBSD system, where every device
  has major 0 and a minor that can exceed 255. A guess that is sometimes
  wrong with no sign of it is worse for a display-only value than a number
  the user has to interpret, so fsimg reports both words as stored.
* **An APFS device without the device field has no number.** Apple's
  reference does not require the field, so the inode is reported as a
  device with `FSIMG_META_DEVICE` clear rather than as corruption. Native
  evidence that macOS always writes the field would tighten this in the
  APFS design.
