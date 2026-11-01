# POSIX metadata and entry kinds

## Scope

The common entry model carries regular files, directories, symbolic links and
FIFOs, and the metadata that POSIX filesystems such as QNX6 store for them:
owner, group, permission bits and inode change time, in addition to the
timestamps and generic attributes every format shares. This document defines
that representation once, for the [C API](design-api.md), the
[catalog](design-sources.md), the host scanner, the [safe
extractor](design-security.md#safe-extraction), the [CLI](design-cli.md) and
every format driver. A driver advertises a field or kind only when it can
read and write it with the fidelity stated here; FAT and exFAT advertise none
of the additions and treat them under the existing loss policy.

## Entry kinds

`fsimg_entry_kind_t` is a closed set whose values are bits, so a format's
capability record lists its supported kinds as a mask:

| Kind | Value | Size | Content | Source |
| --- | --- | --- | --- | --- |
| `FSIMG_ENTRY_FILE` | 1 | Logical length | Payload bytes | Required |
| `FSIMG_ENTRY_DIRECTORY` | 2 | Zero | None | None |
| `FSIMG_ENTRY_SYMLINK` | 4 | Target byte length, 1 through 1024, within the format's `max_link_target_bytes` | The target bytes, without a terminator | Provider or host |
| `FSIMG_ENTRY_FIFO` | 8 | Zero | None | None |

A symbolic link's target is ordinary content: a reader delivers it through
`read_at()` and `next_range()`, and a writer receives it through the entry's
source like a file's payload. The target is not validated as a path and is
never resolved by fsimg, but it never contains a NUL byte: collection rejects
the zero source for a link, and a provider's target is checked when it is
read. Every other on-disk object kind, such as a socket or
device node, is unsupported: a reader whose directory contains one fails that
directory's preparation with `FSIMG_EUNSUPPORTED` naming the entry, and a
writer never receives one because the catalog rejects unknown kind values.

## Metadata fields

`fsimg_metadata_t` gains three validity bits and the fields they govern:

| Bit | Field | Meaning |
| --- | --- | --- |
| `FSIMG_META_CHANGED` | `changed` | Inode change time, an `fsimg_timestamp_t` with the common basis rules |
| `FSIMG_META_OWNER` | `uid`, `gid` | Numeric owner and group identifiers, `uint32_t` |
| `FSIMG_META_MODE` | `mode` | Permission bits `07777`: the twelve POSIX permission, setuid, setgid and sticky bits; no file-type bits |

One further bit governs no value:

| Bit | Field | Meaning |
| --- | --- | --- |
| `FSIMG_META_EXTRA_DATA` | None | The entry has stored data the entry model does not carry, such as an HFS Plus resource fork or extended attributes |

A reader sets it where its format design says, and only there; its absence
says nothing for a format that never sets it. It exists so that an
omission is visible per entry: `t --json` shows it, and since the extractor
never restores it, the extraction loss report names it among the entry's
lost fields, beside owner and mode, which safe extraction never restores
either. No format stores it, so no capability record lists it in
`metadata_fields`, and a catalog entry that carries it is an omission under
the common writer policy, like any field a format lacks. Carrying the data
itself needs a common design of its own.

The existing rules apply unchanged: `required` is a subset of `valid`, unknown
bits are invalid, an absent field has no value, and the catalog preserves every
valid field. A kind is not metadata: it has no validity bit and is always
present. Volume information gains `uuid[16]` with `FSIMG_VOLUME_UUID` for
formats whose identity is a 16-byte UUID; the bytes are in textual order.

## Writer policy

The [writer metadata-loss design](design-metadata-loss.md) owns the common
refusal, accounting, callback, and timestamp-helper contracts. Drivers decide
whether an encoding preserves a field; common writer code applies the caller's
loss policy and counts each affected field once.

Planning checks each valid field against the selected driver's capability
record. A field the format stores is encoded exactly or, where the format's
range or precision is narrower, under the common optional-loss policy: a
required field that does not fit fails, an optional one is counted and
reported under `FSIMG_WRITE_ALLOW_LOSS`, which the option defaults and the CLI
leave set, and fails when a caller clears it. A field the format does not
store at all is an omission under the same policy, so a FAT plan with owner
metadata counts one loss per field by default and fails only for a caller that
forbids losses. Unsupported kinds fail planning for every format; a kind is
never optional. Either failure is `FSIMG_EUNSUPPORTED` naming the entry's
path. The writer checks every entry against the capability record before the
driver plans, so no driver receives a kind it does not store and one rule
governs omission for every format. The catalog keeps the unions of collected
kinds, valid fields and required fields, which decide whether the check fails
before any entry is read again: a plan this check fails reports no loss, and a tree with
nothing the format lacks is not read again.

When a format stores a field the entry does not carry, the driver applies its
documented default. Owner and mode defaults are independent of the process
umask and host identity: QNX6 uses uid and gid zero, mode `0755` for directories,
`0644` for regular files and `0777` for symbolic links and FIFOs; APFS uses
uid and gid zero, `0755` for directories and symbolic links, as macOS
creates links, and `0644` for regular files and FIFOs. Time defaults
follow the format's explicit fallback policy, independent of the host time
zone; QNX4, QNX6, ImageFS and APFS use the planning clock outside
reproducible mode.
Reproducible mode replaces every stored time with the epoch. An absent field's
default is not a loss.

## Host import

`add_tree()` records a symbolic link as a `SYMLINK` entry whose source is the
link's target, read with `readlinkat()` relative to the scanned directory and
never followed; the scanner still opens every other object with no-follow
flags and rejects sockets, devices and unknown kinds. The source record for a
link stores the target length and the link's identity and change metadata;
reopening reads the target again into a bounded buffer and compares it with
the recorded length, so a replaced link fails with `FSIMG_ESOURCE_CHANGED`
like a changed file. Each read inspects the link before and after
`readlinkat()`, so the target belongs to the identity compared; a target that
is empty or longer than `FSIMG_SYMLINK_TARGET_MAX`, 1024 bytes, fails the
scan with `FSIMG_EUNSUPPORTED`. The format's own, possibly lower, limit is
applied at planning, as the [capability records](#capability-records) state.
Host FIFOs remain rejected: the scanner has no use for a host FIFO's identity,
and image FIFOs come from the programmatic API.

Owner and mode are imported only when the writer options set
`FSIMG_WRITE_IMPORT_POSIX`; the CLI exposes it as `-p`. The scanner then
records `uid`, `gid` and the opened object's permission bits as valid optional
fields. Without the flag, host trees carry only the shared timestamp and
attribute fields, so creating FAT and exFAT images from ordinary trees needs
no new option.

## Reading, extraction and display

Readers set the fields their format stores. The QNX6 reader sets owner, mode,
and all four timestamps; the HFS Plus reader sets the same fields and, on an
entry with a resource fork or stored attributes, `FSIMG_META_EXTRA_DATA`;
the ISO 9660 reader sets owner, mode and the timestamps Rock Ridge records
in its `rockridge` view and only the recording date in the others; FAT and
exFAT readers set nothing new. The safe
extractor keeps its profile: it creates regular files and directories with
private modes, never restores owners, permission bits or change times, and
creates no symbolic links or FIFOs. By default an unsupported kind fails the
extraction with `FSIMG_EUNSUPPORTED` naming the entry. The option
`FSIMG_EXTRACT_SKIP_UNSUPPORTED`, CLI `-k`, instead skips such entries and
reports their count in the context statistics' `skipped_entries`, which the
CLI prints as a warning and as a JSON summary field; an omission is never
silent. An entry a reader omits for a name outside its profile, under the
[omitted-names contract](design-directories.md#omitted-names), is treated
the same way: without the option it fails the extraction with
`FSIMG_EUNSUPPORTED`, naming the entry in the escaped rendering; with it the
entry is omitted. Omissions have their own count, `omitted_entries`, in the
operation's result and the context statistics, one per omitted subtree
root, which a listing or a verification reports as well, and the CLI prints
as its own warning and JSON field. Owner, mode and change time, like other
fields the extractor does not restore, are optional losses under the
extraction loss policy. Selecting a
symbolic link directly with `x -O` writes its target bytes, which is the
content the entry declares.

`t --json` records carry `kind` as a name (`file`, `directory`, `symlink`,
`fifo`) and the new fields when valid and null otherwise, with
`extra_data` true when that bit is set and null otherwise; human listings mark
a symbolic link with `@` and a FIFO with `|` after the quoted name, in the
style of `ls -F`. Info (`i`) prints the volume UUID when valid, and `--formats`
lists the additional kinds and fields a format advertises.

## Capability records

`fsimg_format_info_t.entry_kinds` and `metadata_fields` use the masks above.
The record also gains `max_label_bytes`, the format's encoded on-disk label
capacity in bytes (11 for FAT, 22 for exFAT's eleven UTF-16 units, 48 for
QNX4, 510 for HFS Plus's 255 UTF-16 units, 255 for an APFS volume name,
126 for UDF's logical volume identifier,
32 for ISO 9660's sixteen UCS-2 Joliet characters), zero for a format without a
volume label. Writer construction rejects a non-NULL label
for such a format; the CLI applies its default label only when the value is
nonzero and passes a NULL label otherwise, so a format that refuses labels,
such as QNX6, is created without `-L` and fails explicitly when `-L` is given.
The same rule covers format-specific write options: `write_settings` is the
mask of `FSIMG_SETTING_*` fields of `fsimg_write_options_t` the format stores
(byte order, volume UUID, inode capacity and reserve percentage, all QNX6's;
QNX4 stores the inode capacity alone; ImageFS stores the byte order and
the mountpoint under `FSIMG_SETTING_MOUNTPOINT`, a copied string field, as
the [ImageFS driver design](design-ifs-driver.md#writer-settings) states;
FAT12 and FAT16 store the fixed root capacity under
`FSIMG_SETTING_ROOT_ENTRIES`; APFS stores the volume UUID and the case
sensitivity under `FSIMG_SETTING_CASE_SENSITIVE`; ISO 9660 stores its
interchange level under `FSIMG_SETTING_ISO_LEVEL` and its El Torito boot
images under `FSIMG_SETTING_ELTORITO`, as the
[ISO driver design](design-iso9660-driver.md#writer-settings) states),
and writer construction rejects a non-default value of any other one with
`FSIMG_EINVAL` before the driver's `validate` runs, so no driver has to
remember another format's settings. The CLI generates a random UUID, like
the serial, without `-R` when the mask names the UUID and none is given.
The [settings catalogue](design-settings.md) associates these mask bits with
their fields and defaults; capability checks and CLI conversion share that
description while format validation retains the value relationships.

`read_settings` names accepted read options in the same `FSIMG_SETTING_*`
namespace: APFS's volume and snapshot and ISO 9660's view. Common reader
validation refuses non-default fields absent from this mask before driver
open, as the [settings catalogue](design-settings.md) specifies.

`max_link_target_bytes` is the longest symbolic-link target the format
stores: 1024 for ImageFS, UDF and ISO 9660, 1023 for QNX4, QNX6 and APFS, and zero for FAT
and exFAT, whose `entry_kinds` lack links. The entry model's ceiling,
`FSIMG_SYMLINK_TARGET_MAX`, is the largest of these, 1024, and bounds the
buffers and the host scan; the
[architecture](architecture.md#names-and-metadata) records the split.
Common writer planning refuses a link whose target is longer than the
selected format's limit with `FSIMG_EUNSUPPORTED`, naming the entry, next to
the check that refuses a kind outside `entry_kinds`, so no driver checks a
target length. A reader classifies a stored target above its format's limit
as its design states for an unobserved value. The format-independent
capability test over every registered record asserts that
`max_link_target_bytes` is zero exactly when `entry_kinds` lacks
`FSIMG_ENTRY_SYMLINK`, so the two never disagree.
