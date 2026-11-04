# ISO 9660 driver

## Scope

The [ISO 9660 format design](design-iso9660.md) establishes the on-disk
facts, the views, the closed acceptance tables and the writer profile.
This document maps them onto code: the modules of `lib/formats/iso9660/`,
the driver state behind the common reader and writer, the bounded
algorithms for directories, Rock Ridge continuation, relocation and
verification, planning and emission, and the tests that qualify them.

The driver implements the common contracts:
the [format descriptor and probe](design-formats.md#format-descriptors-and-profiles),
[prepared directories](design-directories.md),
[logical content](design-content.md), [emission](design-emission.md) and
the [POSIX metadata extension](design-metadata.md). Three common
mechanisms serve it and are owned by those designs, not by this driver:
content delivered from a
[decoded link target](design-content.md#decoded-link-targets),
[several walks in one verification](design-directories.md#several-walks),
and the writer's [link-target read](design-emission.md#encoded-link-targets).
Common code has no ISO-specific branch. The ImageFS and QNX4 drivers are
the structural templates; they share no code with this one.

## Modules

| File | Owns |
| --- | --- |
| `iso9660.h` | Private types, constants, profile bounds and prototypes shared by the files below |
| `descriptor.c` | `fsimg_iso9660_format`: the `iso9660` name, its capability record, the probe, the reader and writer operations |
| `codec.c` | Both-byte-order fields, recording and descriptor dates, and decode/encode of descriptors, directory records and path-table records; pure functions without I/O |
| `volume.c` | The descriptor scan, Joliet identification, the system-area check, view selection, the content domain, the block window |
| `susp.c` | The SUSP cursor: entry framing, continuation areas, the visited-area set, the `SP` and `ER` handshake |
| `rockridge.c` | `PX`, `PN`, `NM`, `SL`, `TF`, `CL`, `PL`, `RE` and the refused entries into one decoded record; link-target assembly |
| `names.c` | Primary and Joliet identifier checks, UTF-16BE decoding, displayed names, keys, and the two identifier orders |
| `directory.c` | `prepare` and `replay` for the three views: framing, dot records, section runs, relocation, the payload and the content index |
| `read.c` | `fsimg_iso9660_reader_ops`: open and close, volume information, resolve |
| `eltorito.c` | The boot record and boot catalog: decode and validation for reading, encode for writing |
| `verify.c` | The walk plan, ownership claims and their sweep, path tables, order, relocation matching |
| `write-iso9660.h` | Plan rows and the plan object |
| `write-names.c` | Natural identifiers, collision groups, alias reservations and assignment, for both hierarchies |
| `write-susp.c` | Rock Ridge entry encoding and the record and fragment split, shared by sizing and emission |
| `write-plan.c` | `validate`, `plan`, `free`: settings and the planning passes |
| `write-emit.c` | `emit`: descriptors, path tables, boot catalog, directories, continuation fragments, payload copies |

`codec.c` and `eltorito.c` are the only places that know field offsets,
and the writer encodes every structure through the same functions the
reader decodes with, so a structure the writer emits is one the reader's
checks accept. `susp.c` is the only code that follows a continuation, and
`directory.c` the only code that walks directory records.

## Codecs

`iso_u16both()` and `iso_u32both()` decode a both-byte-order field and
fail with `FSIMG_ECORRUPT` when its copies differ; their `put` forms write
both copies. The date codecs convert between the 7- and 17-byte forms and
`fsimg_timestamp_t` through the common calendar functions, never through
host `mktime` or a time zone. Extent checks compute `start + length` in
`uint64_t` block counts against the volume and the readable end before any
multiplication by 2048. Decoded structures are C types with `_t` typedefs
holding validated values only:

| Type | Fields |
| --- | --- |
| `iso_descriptor_t` | Type, version, block, and for a primary or supplementary descriptor the volume space size, path table size and blocks, root record, label bytes, dates and whether one was malformed, Joliet level |
| `iso_record_t` | Length, extended attribute blocks, extent, data length, date, flags, identifier offset and length, system use offset and length |
| `iso_path_record_t` | Identifier length and bytes, extent, parent number |
| `iso_rr_t` | Presence bits and values of `PX`, `TF`, `CL`, `PL`, `RE`; the name length; the link-target length; the refused-entry class |
| `iso_boot_t` | Boot catalog block, entry count, and per entry the platform, boot indicator, media type, load segment, system type, sector count and image block |

`iso_record_decode()` applies the [framing rule](design-iso9660.md#framing-rule)
and the [flag table](design-iso9660.md#flags) to bytes already bounded to
one block; each failure names its class. Encoding is the exact inverse and
fills every padding byte with zero.

## Probe and open

The probe reads the descriptor's fixed prefix of 34816 bytes, through block
16. Common detection takes the maximum prefix declared by all descriptors,
131072 bytes for UDF. The identity signature is `CD001` at offset 32769
with a type of 0, 1, 2, 3 or 255 at 32768; without it, or with a shorter
volume, the probe makes no claim (`FSIMG_EFORMAT`). High Sierra (`CDROM` at
offset 9), a UDF recognition sequence (`BEA01`) and CD-i never carry it.
Under an [assumed identity](design-formats.md#format-descriptors-and-profiles)
open skips `CD001` at block 16, the only identity check, and records it; the
type at block 16 is then validation, an unknown one `FSIMG_EUNSUPPORTED`, and
every later descriptor still needs `CD001`.
With the identity the probe claims the image; when block 16 is the primary
descriptor it also decodes the logical block size and the volume space
size, failing with `FSIMG_ECORRUPT` for disagreeing copies or a zero size
and with `FSIMG_EUNSUPPORTED` for a block size other than 2048. Any other
first descriptor is claimed with `FSIMG_OK`, and open finds the primary
descriptor. The probe performs no I/O and allocates nothing. A volume
whose block 16 holds `CD001` by chance, inside another format's data,
draws two claims and needs a forced format; no probe scans for a
signature elsewhere.

`open` completes recognition in a fixed order; each step fails before the
next reads:

1. **Descriptors.** Read blocks from 16 one at a time under the
   [scan rule](design-iso9660.md#descriptor-scan-rule), at most 65 with
   the terminator, decoding each and keeping the first primary
   descriptor, the Joliet descriptor, the El Torito boot record and the
   counts. A later primary descriptor is compared with the first byte for
   byte.
2. **Geometry.** Check the primary descriptor's fields, the Joliet
   descriptor's agreement with them, the root records, and the path-table
   blocks and sizes against the volume. The readable end is the smaller of
   the volume and the slice's whole blocks, and its shortfall is recorded
   for info. The content domain is `[(terminator + 1) * 2048, readable
   end)`.
3. **System area.** Sixteen one-block reads, stopping at the first
   nonzero byte, record whether any byte is
   nonzero, for info. Nothing is validated.
4. **El Torito.** When a boot record exists, decode and validate the boot
   catalog's one block under the [boot-catalog rule](design-iso9660.md#boot-catalog).
5. **View.** For `auto` or a forced `rockridge`, read the primary root's
   first block and run the [handshake](design-iso9660.md#the-handshake)
   through the SUSP cursor. Select the view under the
   [selection rule](design-iso9660.md#view-selection) and the `iso_view`
   read option, which the reader's copy of the decoding options supplies.

The reader state then holds the decoded descriptors, the selected view and
its root, whether the handshake ran and what it found, the boot summary,
and one block window for directory reads and one for continuation reads.
Open reads at most
16 + 65 + 1 + 1 blocks and the root's at most 16 continuation blocks.

## The SUSP cursor

`susp.c` iterates one record's entry stream without allocating per entry.
The cursor holds the current area, the record's system use bytes or a
continuation area, and a fixed array of the areas entered in this stream
as `(block, offset, length)`: the system use area and at most 16
continuation areas. `iso_susp_next()` returns one
entry's signature, version and a bounded view of its contents, or the end
of the stream:

* a signature position with fewer than four bytes left, or a zero byte,
  ends the area; `ST` ends the stream;
* an entry's length below 4 or past its area is `FSIMG_ECORRUPT`;
* `CE` is consumed by the cursor, not returned: its area must be nonempty,
  start in a block within the readable volume and overlap no area entered
  (`FSIMG_ECORRUPT`), then end within that block and find the stream with
  fewer than 16 continuation areas entered (`FSIMG_EUNSUPPORTED`); it
  becomes the next area when the current one ends;
* `PD` is skipped by its length; every other entry is returned, and the
  decoder skips one it does not know.

Every continuation area is one read of at most one block, served from the
continuation window, which holds the last continuation block read, so the
common case of many records continuing into one block reads it once. The
cursor charges one work unit per area, and an area holds at most 2048
bytes, so one record's stream decodes at most its system use area and
32 KiB; a directory's decoding cost is bounded by its record count times
that, and every record is charged when admitted.

`rockridge.c` consumes a whole stream into an `iso_rr_t`, which owns the
name and link target it assembles in its 1024-byte buffers, so nothing
else can overwrite them while preparation or replay uses them; a dot
record's stream assembles neither. It applies the
[entry table](design-iso9660.md#entries) and the
[link rule](design-iso9660.md#symbolic-links). A refused entry, a type
outside the seven, `ZF` or an over-long name or target, is recorded as a
class rather than
failing at once, so every corruption check runs first and preparation
names the entry when it refuses it.

## Directories and names

### Preparation

`prepare` takes the directory's extent and data length from the root
record at level 0, otherwise from the current entry's locator, and:

1. reads the extent block by block through the directory window, applying
   the framing rule to every record and the [dot-record rules](design-iso9660.md#dot-records)
   to the first two: the first names this directory; the second names the
   walker's parent, or, for a directory the locator marks as reached
   through `CL`, carries `PL` naming the walker's parent; a directory
   reached otherwise must not carry `PL`;
2. groups records into section runs, holding only the current run's
   identifier, section count and previous section end;
3. decodes, per the view, each run's name and kind: in `primary` and
   `joliet` from the identifier and flags, in `rockridge` from the first
   section's SUSP stream;
4. refuses a child record, or a `CL` target, that names the root's extent;
5. admits each run with `fsimg_directory_admit()`, except a `rockridge`
   run carrying `RE`, which is not an entry.

A directory child's admission carries its extent key, so namespace
validation's external sort rejects two records opening one directory
together with repeated names. With the dot-record rules this makes the
view a tree by construction: no bitset or record of visited directories
exists, and none is needed. Under verification, preparation also compares
each run's identifier with the previous one under the hierarchy's
[order](design-iso9660.md#ordering) and emits the [claims](#verification)
of the directory, its records' sections and its continuation areas. A
run's refusal (associated, interleaved, a type outside the seven, `ZF`,
`SF`) fails the
preparation with `FSIMG_EUNSUPPORTED` naming the entry, after its name is
known.

The admitted payload, little-endian with explicit widths, holds: the
directory's extent and data length; the block and in-block offset of the
run's first record; the section count; the kind; a reached-through-`CL`
flag; and for a directory the extent and data length of the directory it
opens, which for a placeholder is the `CL` target's. Twenty-six bytes, so
the record fits `FSIMG_DIRECTORY_PAYLOAD_BYTES`. Preparation's cost is the
directory's records and their continuation areas, never its subtree.

A `CL` placeholder's target is read at preparation: its first block's two
dot records give the target's data length and its `PL`, which must name
this directory. Its physical parent, the second dot record's extent, is
checked by verification's relocation match.

### Names and keys

`names.c` derives the displayed component and the keys of the
[name rules](design-iso9660.md#displayed-names-and-keys). A primary
identifier is used as its bytes. A Joliet identifier is decoded to UTF-8
in the name buffer, an unpaired surrogate written in the generalized
three-byte form so the portable name profile omits the entry. A Rock Ridge
name is copied from its `NM` entries. `keys` returns the name key and, for
a directory, the extent key, a zero byte followed by the opened extent's
block in big-endian order; `key` validates a query component under the
profile, `FSIMG_EINVAL` outside it, removes a final `;1` in the `primary`
and `joliet` views, and returns its bytes. A query can never produce an
extent key, since it holds no zero byte. The `ambiguous` text is
`duplicate ISO 9660 name or directory in one directory`.

### Replay

`replay` re-validates the payload, reads the run's first record again at
its block and offset through the directory window, decodes it, and
requires its flags, its displayed name and, in `rockridge`, its stream's
name to agree with the record the walker replays, else `FSIMG_ECORRUPT`.
Consecutive replays usually hit the window's block, so a directory costs
about one read per block at replay as at preparation. It then fills the
entry: kind, size, the hidden attribute, and the times, owner and mode the
view provides. For a multi-section file it reads the following records of
the run from the same directory extent, checking each again.

For a regular file with content, `replay` builds the current content
index: one extent-index record per nonzero section, `(logical offset,
(extent + extended attribute blocks) * 2048, rounded length)`, where the
length is rounded up to 2048 bytes as the
[extent index](design-content.md#extent-index) requires. Non-final sections
are whole blocks, so only the final section can gain padding. Logical
offsets advance by the data length and common delivery clips the final
extent to the file size, so no padding is exposed. Records are appended in
order within the content domain, which rejects a section outside it. A
one-section file builds one record; the index is released when the walker
advances. `reader->initialized` is the file size. For a symbolic link,
`replay` sets the entry's kind first, then assembles the target from the
`SL` entries and passes it to `fsimg_reader_set_link_target()`, the
[decoded-target](design-content.md#decoded-link-targets) declaration, which
checks that kind, copies the target into the reader's buffer and sets the
size. A directory or FIFO
has no content.

## Content

`resolve` is a lookup in the current entry's index: the cached last
extent for sequential reads and binary search otherwise. It returns the
section containing the offset, so a read crossing a section boundary is
two extents that common delivery joins in one call. No content read
rereads a directory, and no byte reaches delivery outside a section that
preparation and replay validated. A symbolic link never reaches `resolve`:
common delivery copies its decoded target. Shared extents cost each entry
its own reads; the default extraction output ceiling bounds a heavily
shared image as it bounds any other.

## Volume information

`info` reports the format `iso9660`; the profile, the selected view's
name: `rockridge`, `joliet` or `primary`; the geometry as `volume_bytes =
volume_space_size * 2048` with 2048-byte sectors and allocation units and
`allocation_units = volume_space_size`; the label of the view's
descriptor, the primary volume identifier without trailing spaces or NUL
bytes for
`rockridge` and `primary` and the decoded Joliet identifier for `joliet`,
reported under `FSIMG_VOLUME_LABEL` only when it is not empty and is printable by the one rule the
[descriptor text fields](design-iso9660.md#descriptor-text-and-dates) follow;
the time basis `FSIMG_TIME_OFFSET`; and never `FSIMG_VOLUME_FREE` or
`FSIMG_VOLUME_UUID`, since the format has neither free space nor an
identity.

The details, in this order: `rockridge` (0 absent, 1 present, 2 present
with an unsupported skip count, `unknown` as 3 when a forced view did not
run the handshake), `joliet_level` (0 to 3), `enhanced` (1 when an
enhanced descriptor exists), `descriptors` (the count before the
terminator), `terminator_block`, `root_block` and `path_table_bytes` of the
selected view's hierarchy, `system_area_used` (1 when a byte of blocks 0 to
15 is nonzero), `boot_catalog_block` (0 without El Torito), `boot_entries`,
`boot_x86` and `boot_efi` (entries per platform). Every value is known when
open returns.

The named texts, under the keys and rules of the format design's
[descriptor text and dates](design-iso9660.md#descriptor-text-and-dates),
in table order: `volume_set`, `publisher`, `preparer`, `application`,
`copyright_file`, `abstract_file`, `biblio_file`, `created`, `modified`,
`expires` and `effective`. Each is filled from the selected view's
descriptor, so the three views report the primary descriptor's bytes or the
Joliet descriptor's units as they report the label; a field that is empty,
not printable or an absent date is skipped rather than reported empty,
exactly as an invalid label leaves `FSIMG_VOLUME_LABEL` clear, so
`text_count` is the number of fields the descriptor carries legibly. The
decoded descriptor keeps the seven fields' raw bytes and the four decoded
dates from open, since open already reads and checks them, and no block is
read again for information. The eleven values fit the information record's
text table, whose entry holds the longest UTF-8 decoding of a 128-byte
UCS-2 field, and the build checks that they fit.

## Verification

Full verification checks the whole volume and every view the image offers.
It runs:

1. **Handshake.** When open did not run it, because a `joliet` or
   `primary` view was forced, verification runs it first, so the offered
   views are known and whether the volume uses SUSP; an `SP` with a
   nonzero skip count makes verification `FSIMG_EUNSUPPORTED`, with or
   without Rock Ridge, since the cursor reads no stream behind skipped
   bytes.
2. **Descriptor dates.** A primary or Joliet descriptor date that open
   read as absent because it is
   [malformed](design-iso9660.md#descriptor-dates) makes verification
   `FSIMG_ECORRUPT`; open records it in the decoded descriptor.
3. **Header claims.** The system area, each descriptor block, each path
   table and its copies, the boot catalog and the part of each boot
   image's extent within the volume go into the claim stream.
4. **Walks.** The common walker runs once per offered view, with the
   driver's walk view set before each start and a finished walker released
   between them, under the [several-walks rule](design-directories.md#several-walks).
   The selected view is first and counted: `fsimg_verify_account()`
   accounts its entries. The others follow, uncounted, with their root out
   of omission scope: `rockridge`, `primary` and `joliet` in that order,
   skipping the selected one. Payload read-through, when requested, reads
   the files and links of every walk, since shared extents are read per
   reference. Each hierarchy's physical walk, `primary` or `joliet`,
   reads every record of the hierarchy and emits its claims and
   path-table records; the `primary` walk also claims each record's
   continuation areas when the volume uses SUSP, which covers every area
   the reader follows, the handshake's included whether or not Rock Ridge
   is present, and checks the
   [order](design-iso9660.md#ordering) of each directory's records as it
   prepares it. The `rockridge` walk is not physical: it skips every
   record carrying `RE`, and with it the relocation directory that
   mkisofs and xorriso mark with `RE`; it emits the `CL` half of the
   relocation records.
   Each record is reported once, when its directory is prepared, never
   when replay or a placeholder's check reads it again.
   Each walk admits and charges its entries, so verifying an image with
   three views charges the entry limit three times.
5. **Claims.** Each claim is `(start byte, end byte, class, hierarchy)`:
   directories, path tables, the boot catalog, descriptors and data
   rounded to whole blocks, continuation areas exact. The stream is sorted
   externally by start, end and class, and one sweep applies the
   [sharing rules](design-iso9660.md#ownership) with constant state: the
   furthest end of any data claim seen, and the last metadata claim. A
   metadata claim conflicts with an earlier data claim still open and with
   any open metadata claim: each physical walk prepares a directory once
   and the header claims each path table once, so no structure is claimed
   twice. A data claim conflicts with an open metadata claim unless both
   are the identical boot-catalog interval, which sorts just before the
   data claims identical to it, so the exception is a comparison with the
   previous metadata claim. Each sweep step charges the session.
6. **Path tables.** For each hierarchy, both tables are decoded in
   lockstep, record by record, requiring equal records, the
   [ordering rules](design-iso9660.md#path-tables) and the exact size;
   optional copies are compared byte for byte. Each record's parent extent
   is found through a paged array indexed by record number. The records
   `(extent, parent extent, identifier)` and those the walk emitted for
   every physical directory record, `(opened extent, the containing
   directory's extent, identifier)`, admitted or not, are sorted by extent
   and must be equal.
7. **Relocation.** The `rockridge` walk emits the extent of each
   directory it enters through `CL`. The `primary` walk, which frames
   every record's SUSP stream when the volume uses SUSP, emits the
   extent of each directory whose second dot record carries `PL`, having
   checked that record names the directory's physical parent. Sorted, the
   two lists must be equal; neither repeats an extent, since each walk
   prepares a directory once. `RE` takes no part: mkisofs and xorriso mark
   their relocation directory itself with `RE` and no `CL` names it.
8. **El Torito.** Open validated the boot catalog; its extents are in the
   claim stream.

The report counts the selected view's delivered files and directories,
the root included, its logical bytes, and the bytes read in every walk;
allocated units are the blocks some claim covers; free and bad counts are
zero; the omitted subtree roots are the counted walk's. A complete report
means every offered view was walked and passed its checks, and every
check above held. An unsupported value in what a walk reads fails
verification. Rock Ridge entries are interpreted only where the
`rockridge` view reads them: in a subtree `RE` hides from it, the records
are checked in the primary hierarchy and their SUSP streams framed, but
their Rock Ridge entries are not decoded, so a malformed or refused entry
there does not fail verification.

## Writer settings

`validate` accepts the common options with these rules: `sector_bytes`
and `allocation_unit_bytes` zero or 2048; `volume_bytes` zero or a
multiple of 2048; `min_free_bytes` zero; `fat_root_entries`, `byte_order`,
`inode_capacity`, `reserve_percent`, `volume_uuid` and `mountpoint` at
their defaults, which the common settings check enforces since the record
lacks their bits; `volume_serial` ignored; the label within the
[label rule](design-iso9660.md#settings-and-sizing); an epoch within 1900
to 2155 in reproducible mode. The format-specific fields, behind
`FSIMG_SETTING_ISO_LEVEL`, `FSIMG_SETTING_ELTORITO` and
`FSIMG_SETTING_ISO_DESCRIPTOR`:

| Field | CLI setting | Meaning |
| --- | --- | --- |
| `iso_level` | `level=1|2|3` | Zero selects 3 |
| `boot_bios` | `boot=PATH` | Copied; NULL for none; an absolute image path within the path limit |
| `boot_efi` | `efiboot=PATH` | As `boot_bios` |
| `boot_load_sectors` | `bootload=N` | Zero selects 4; at most 65535; nonzero only with `boot_bios` |
| `iso_volume_set`, `iso_publisher`, `iso_preparer`, `iso_application` | `volset=TEXT`, `publisher=TEXT`, `preparer=TEXT`, `application=TEXT` | Copied; NULL selects the default; at most 64 a-characters, the empty string included |
| `iso_copyright`, `iso_abstract`, `iso_biblio` | `copyright=TEXT`, `abstract=TEXT`, `biblio=TEXT` | Copied; NULL selects spaces; at most 18 file identifier characters, the empty string included |
| `iso_created`, `iso_modified`, `iso_expires`, `iso_effective`, each with its `_set` flag | `created=SECONDS`, `modified=SECONDS`, `expires=SECONDS`, `effective=SECONDS` | Unix seconds in the `-e` grammar; a clear flag with a nonzero value is `FSIMG_EINVAL`; within year 1 through 9999 |

The keys are bare filesystem keys, as the [layer design](design-layers.md#layer-settings)
spells them. `validate` checks each descriptor text against its class and
capacity and each present date against the digit date's range, naming the
key in `error.path`, under the format design's
[descriptor text rules](design-iso9660.md#descriptor-text-and-dates); the
checks are one function per class over the closed field table, not one
per field. The seven texts are pointers like the mountpoint and boot paths:
copied at construction, bounded by their capacities, with NULL and `""`
distinct because an empty field is a value. The CLI bounds each text by its
capacity and a terminator, and the worker job carries each in a field of that
size with a presence bit, and each date as eight bytes with a presence bit,
so a transported empty text or zero date stays distinct from an absent one.
Whether a boot path names a suitable file is checked by
planning against the frozen catalog. The capability record's
`read_settings` names `FSIMG_SETTING_ISO_VIEW`, so the common reader
accepts a non-default `iso_view` for this format and refuses it for every
other, as the [settings catalogue](design-settings.md) states.

## Planning

The plan object embeds `fsimg_plan_t`, the level, the label projections,
the primary descriptor's encoded text fields and four dates, which
validation and the fallback settle before any pass runs and emission copies
into the descriptor, the boot summary, the layout's block numbers, each hierarchy's name and
path streams, and one paged array of rows
keyed by catalog ID, row 0 the root. One extra row is reserved at catalog
count plus one for a generated relocation container; an adopted container
uses its existing catalog ID. A driver-owned lookup supplies that synthetic
entry to physical streams, while every source operation stays within the
frozen catalog's ID range. A row holds the kind, the size, the
section count, the encoded times, owner and mode, the logical and
physical depth and path length, the physical parent for a relocated
directory, and the results of later passes: each hierarchy's directory
block, length and path-table number, the file's first data block, the
encoded length of its Rock Ridge entries kept in the record, the number
and lengths of its continuation fragments, the first fragment's relative
position, and a link's encoded `SL` length and hash. Continuation fields
belong to a record plan: a relocated directory has separate plans for its
physical `RE` record and logical `CL` placeholder. The name stream's
closed record roles distinguish ordinary files, directories, placeholders
and relocated records, so directory spelling does not imply a physical
directory flag or path-table descent. Names are not in rows; they live in
two sorted streams.
Planning runs these passes over the frozen catalog:

1. **Entries.** In ID order: check each component against the 255-byte
   Rock Ridge bound, `FSIMG_EUNSUPPORTED` above it; validate kind and
   metadata under the [metadata policy](design-metadata.md#writer-policy)
   with the [times and defaults](design-iso9660.md#times-and-defaults);
   refuse a file above `2^32 - 1` bytes at levels 1 and 2; count each
   directory's subdirectories for its link count; accumulate
   `initialized_bytes`.
2. **Primary names.** `write-names.c` emits each eligible child's natural
   identifier with its logical parent and ID, sorts externally by parent
   and identifier, keeps the first of each equal group, and emits the
   reservations of kept identifiers in the alias family, sorted by parent
   and number. The children needing an alias, sorted by parent and ID,
   are merged with the reservations to assign numbers. Every child's
   final identifier then goes to the primary stream of `(physical parent,
   identifier, ID)`, sorted by parent and the
   [primary order](design-iso9660.md#ordering) with the writer's byte
   tie-break, and each directory's row records its group: the frame
   offset of its first child and the count. A relocated directory's
   placeholder keeps the identifier this pass gives it.
3. **Relocation.** One pass in catalog ID order, which visits a parent
   before its children, computes each directory's physical level and path
   length from its physical parent's and the length of its identifier,
   relocating it under the
   [relocation rule](design-iso9660.md#deep-trees-and-relocation). A
   relocated directory's identifier in `RR_MOVED` is always an alias,
   whose length follows from its name and the level alone, so the pass
   needs no number. It adds the relocation directory, adopting an empty
   source `.rr_moved` in the root when one exists, numbers the relocated
   directories' aliases in `RR_MOVED` in ID order and moves their stream
   records under it.
4. **Joliet names.** Pass 2's rules over UTF-16 code units, without links
   and FIFOs, over the logical tree, giving the Joliet stream and its
   group table.
5. **Path tables.** For each hierarchy, a breadth-first walk numbers the
   directories into a path stream of `(ID, parent number, identifier)`:
   the root is record 1, and visiting record `i` appends its child
   directories from its group in stream order, which is the path table's
   order within one parent. The path stream is the walk's own queue, read
   behind the records the walk appends, and afterwards lists the
   directories in path-table order for sizing, layout and emission. The
   pass sums the table size and refuses more than 65535 directories with
   `FSIMG_EUNSUPPORTED`.
6. **Sizing.** For each hierarchy, in path-table order, each directory's
   records are measured and packed into blocks, and each directory's
   length stored in its row. In the primary hierarchy each record's Rock
   Ridge entries are encoded, a symbolic link's target read through the
   [link-target read](design-emission.md#encoded-link-targets), and split
   between the record and its fragment chain with `write-susp.c`; the
   fragments are placed by next fit in the continuation region, which
   follows path-table order because emission writes directories in it.
   The row keeps the record's encoded length, its fragments' lengths and
   the first one's position, and for a link the length and FNV-1a hash of
   its `SL` entries, so emission recognizes a target that changed.
   Lengths depend only on entry sizes, never on addresses, so the pass
   runs once.
7. **Layout.** Assign blocks in the [layout order](design-iso9660.md#layout):
   descriptors, path tables, the boot catalog, primary directories by
   number, the continuation region, Joliet directories by number, then
   each file's sections in ID order. Every sum is 64-bit; a volume above
   `2^32 - 1` blocks is `FSIMG_ERANGE`. A requested `volume_bytes` below the
   result is `FSIMG_ERANGE`; above it, the difference is zero padding.
8. **Summary.** Fill `fsimg_plan_info_t`: `volume_bytes`, the blocks in
   `allocation_units`, `metadata_bytes` for descriptors, path tables,
   directories, the continuation region and the boot catalog,
   `allocated_bytes` for metadata and file blocks, `initialized_bytes`,
   free bytes zero, and 2048-byte sectors and units. The common
   [writer loss policy](design-metadata-loss.md) installs the loss count
   after every pass succeeds.

## Emission

The common emission object has already zeroed the sink. The driver emits:

1. The primary descriptor, the boot record, the Joliet descriptor and the
   terminator, each encoded by `codec.c` and `eltorito.c`.
2. Each hierarchy's type-L and type-M path tables, from its path stream
   and the directories' rows.
3. The boot catalog block.
4. The primary directories in number order: the two dot records, then the
   directory's group from the primary stream, each child's records with
   its Rock Ridge entries rebuilt from its row, its name and, for a link,
   its target read again through the link-target read. Each fragment of
   the record's chain is written at its planned position as its record
   is. An encoded length that differs from the row's is a changed link
   target, `FSIMG_ESOURCE_CHANGED`, or a corrupt plan, `FSIMG_ECORRUPT`.
5. The Joliet directories in number order.
6. For each regular file, one common payload copy into its region: its
   sections are contiguous, so the region is `(first block * 2048, size)`.
   An empty file uses a zero-length copy with no regions, preserving source
   opening and validation even when nothing is written.

Every emitted byte comes from a row, a stream, the layout or a profile
constant, and every structure is written once.

## Resource bounds

| Operation | Work and residency |
| --- | --- |
| Probe and open | A 34816-byte driver prefix within the shared detection buffer (131072 bytes for UDF); the system area, at most 67 blocks and the root's at most 16 continuation blocks |
| Listing | Per directory: its blocks once at preparation and about once at replay, its continuation areas, and the common key sort; no payload |
| Content | One index lookup per request after replay builds the section index |
| Verification | One walk per offered view, the claim sort and sweep, the path-table pass and sort per hierarchy, the relocation sort |
| Planning | One pass over the catalog, the name sorts per hierarchy, the relocation, sizing and numbering passes; rows in one paged array |
| Emission | Payload bytes plus metadata proportional to entries and names; each link target read once more |

Resident per reader: the decoded descriptors, the boot summary and two
block windows, beside the common walk's stack, path buffer and
decoded-target buffer; preparation and replay hold the cursor's 17-area
array and one decoded record with its 1024-byte name and target buffers
on the stack. The extent index, claim streams and
path-table arrays are charged scratch. Every loop charges the session and
observes cancellation.

## Failure classification

Reader:

| Condition | Status |
| --- | --- |
| No identity at block 16; a volume shorter than 34816 bytes | `FSIMG_EFORMAT` |
| A [profile](design-iso9660.md#reader-profile) value outside its table; a forced view the image lacks; an associated, interleaved, sparse or zisofs entry; a name or target over 1024 bytes | `FSIMG_EUNSUPPORTED` |
| Disagreeing field copies; a referenced structure outside the volume or the slice; a framing, dot-record, tree, section, name, SUSP, Rock Ridge, relocation, boot-catalog or date violation; a repeated name or directory; under verification, an order, path-table, ownership or relocation-match violation | `FSIMG_ECORRUPT` |
| A selection component outside the portable name profile; a non-default read setting the opened format does not declare | `FSIMG_EINVAL` |
| Budget, scratch, work, depth or file-descriptor exhaustion | `FSIMG_ELIMIT` |

Writer:

| Condition | Status |
| --- | --- |
| A setting outside its rules; a label outside its rule; a descriptor text field with a character outside its class or longer than its capacity in the Joliet descriptor (64 or 18 characters); a volume date outside year 1 through 9999, or nonzero without its presence flag; a boot path that names no regular non-empty one-section file; `bootload` past its image | `FSIMG_EINVAL` |
| A component over 255 bytes; a link target over 1023 bytes or with a component over 255 bytes; a file above `2^32 - 1` bytes at levels 1 and 2; more than 65535 directories in a hierarchy; a root child named `.rr_moved` that is not an empty directory when relocation is needed; a kind the profile lacks; a required metadata loss | `FSIMG_EUNSUPPORTED` |
| A requested volume below the image; a volume above `2^32 - 1` blocks | `FSIMG_ERANGE` |
| A plan row or stream record read back outside its limits; an encoded length that differs from the plan without a changed source | `FSIMG_ECORRUPT` |
| A link target reread at emission whose encoded length or hash differs from the plan's | `FSIMG_ESOURCE_CHANGED` |
| Budget, scratch, work or file-descriptor exhaustion | `FSIMG_ELIMIT` |

Diagnostics quote at most one bounded, rendered name and never record
bytes or buffer contents.

## Tests

`test/iso9660.c` owns a synthetic image generator written from the format
design with its own offset tables; it shares no code with
`lib/formats/iso9660/`. It builds images in memory: descriptor sets in
every legal order, primary and Joliet hierarchies, multi-section files,
extended attribute records, SUSP streams with continuation chains, every
Rock Ridge entry, relocation, and El Torito boot catalogs, and it mutates
them for the format design's [acceptance matrix](design-iso9660.md#acceptance-matrix).
Every valid fixture has a paired mutation that must fail with its class,
which proves the check ran. Reading tests compare `i`, `t`, `x -O` and `x`
in each view on the fixtures and on the external corpus.

`test/iso9660-write.c` creates images through the public API and reads
them back through its own independent checker, written from the format
design like the generator, as well as through the reader and verifier in
all three views. It compares bytes across source order, metadata cache
size, scratch placement and time zone, scans every padding byte for
poison, and pins the alias, fragment-chain and relocation rules with
trees built to hit each boundary. Fault tests inject allocation, scratch,
short-read, short-write, EOF, source-change and cancellation failures at
every stage and check cleanup.

The descriptor text and dates are tested at each layer, every passing case
beside a seeded failure that proves the check ran:

* **Codec.** Each class's encoder and decoder on the closed field table:
  a text of 64 characters (18 for a file identifier), an empty text, a
  date at year 1 and at 9999, each decoded back equal; against a
  65-character text, a lowercase letter, a `/`
  in a file identifier and a date at year 0 and 10000 refused; a decoded
  field's bytes compared with the test's own offset table, so an offset
  or width copied wrongly fails.
* **Writer.** Every setting given, the image read back in all three
  views: `rockridge` and `primary` report the primary text, `joliet`
  reports the same texts, and all three the same dates; an image with
  no setting byte-identical to one with the defaults given explicitly, and
  its descriptor's text and date bytes equal to a stored vector of
  `FSIMG`, spaces and absent dates; `application=` empty yields spaces in both descriptors; a 65-character a-character text and a 19-character file identifier refused; a
  date given in reproducible
  mode is stored, and a date left out is the epoch; each refusal names its
  key in `error.path`.
* **Reader.** Synthetic descriptors with a control byte, a byte above
  `0x7e`, lowercase text, a leading `_`, an unpaired surrogate in the
  Joliet descriptor, a date with a nonzero offset and hundredths, and a
  date whose UTC year leaves the range: open and full verification
  succeed, and exactly the reportable fields appear; a non-digit, month
  13, year 0 or zero-byte hundredths in any of the four dates of either
  descriptor opens, lists and extracts with the date absent, and full
  verification is `FSIMG_ECORRUPT`.
  External corpus images report their producers' application identifiers,
  compared with `xorriso -pvd_info`.
* **CLI.** Each key accepted and transported through the worker job, with
  an empty value and a zero date distinct from omission; a duplicate key,
  a value over its width, a character outside its class and a signed
  seconds value with a plus sign or suffix refused naming the key; `i`
  prints the texts in table order in the human form and under `texts` in
  JSON, with the CLI's ordinary escaping, checked against a synthetic
  image whose field holds `"` and a backslash, since a reported field is
  printable and those are the characters the escaping touches.
* **Specification harness.** Input rules `create.descriptor_text` and
  `create.descriptor_dates`, written from the format design with their
  own class tables, with a valid witness and a violating mutation each;
  the `xorriso -pvd_info` and `isoinfo -d` oracles compare every text and
  date the recipe sets with the created image, and record them on reading
  cases.

Scale tests record I/O calls, metadata passes, peak scratch and RSS: a
directory of a million entries, which must spill; a Rock Ridge tree 256
levels deep through relocation; a file of 4 TiB as sparse sections; a
volume of `2^32 - 1` blocks with a small tree; a directory whose every
record continues into one continuation block; and on the writer, a
catalog that spills and a plan on both sides of the block limit.

`test/iso9660-integration.sh` runs the independent oracles the build finds
and skips the others explicitly: `xorriso`, `genisoimage`, `makefs -t
cd9660` and libarchive as producers read by fsimg; `isoinfo`, `isovfy`,
`xorriso -indev`, `bsdtar`, `7z` and, on hosts that allow it, the Linux and
FreeBSD kernel readers as consumers of fsimg output; `xorriso
-report_el_torito` for boot catalogs; and QEMU with SeaBIOS and OVMF for
the boot smoke tests. The owner's H3 disc, given by path in the
environment, is read with `i`, `t` and `W`, printing counts only.
`fuzz/iso9660.c` feeds the probe, open, every view's traversal, content and
verification under finite budgets, and `fuzz/iso9660-susp.c` feeds the SUSP
cursor and Rock Ridge decoding over a synthetic block map. Sanitizer runs
follow the build documentation.
