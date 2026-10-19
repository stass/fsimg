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
| `FSIMG_ENTRY_SYMLINK` | 4 | Target byte length, 1 through 1023 | The target bytes, without a terminator | Provider or host |
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

The existing rules apply unchanged: `required` is a subset of `valid`, unknown
bits are invalid, an absent field has no value, and the catalog preserves every
valid field. A kind is not metadata: it has no validity bit and is always
present. Volume information gains `uuid[16]` with `FSIMG_VOLUME_UUID` for
formats whose identity is a 16-byte UUID; the bytes are in textual order.

## Writer policy

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
before any entry is read again: a failed plan reports no loss, and a tree with
nothing the format lacks is not read again.

When a format stores a field the entry does not carry, the driver applies its
documented default, independent of the process umask, host identity, time zone
or clock: QNX6 uses uid and gid zero, mode `0755` for directories, `0644` for
regular files and `0777` for symbolic links and FIFOs, and derives absent
times from its timestamp fallback. Reproducible mode replaces every stored
time with the epoch, as it already does for the shared timestamps.

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
is empty or longer than 1023 bytes fails the scan with `FSIMG_EUNSUPPORTED`.
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
and all four timestamps; FAT and exFAT readers set nothing new. The safe
extractor keeps its profile: it creates regular files and directories with
private modes, never restores owners, permission bits or change times, and
creates no symbolic links or FIFOs. By default an unsupported kind fails the
extraction with `FSIMG_EUNSUPPORTED` naming the entry. The option
`FSIMG_EXTRACT_SKIP_UNSUPPORTED`, CLI `-k`, instead skips such entries and
reports their count in the context statistics' `skipped_entries`, which the
CLI prints as a warning and as a JSON summary field; an omission is never
silent. Owner, mode and change time, like other fields the extractor does not
restore, are optional losses under the extraction loss policy. Selecting a
symbolic link directly with `cat` writes its target bytes, which is the
content the entry declares.

`ls -j` records carry `kind` as a name (`file`, `directory`, `symlink`,
`fifo`) and the new fields when valid and null otherwise; human listings mark
a symbolic link with `@` and a FIFO with `|` after the quoted name, in the
style of `ls -F`. `info` prints the volume UUID when valid, and `formats`
lists the additional kinds and fields a format advertises.

## Capability records

`fsimg_format_info_t.entry_kinds` and `metadata_fields` use the masks above.
The record also gains `max_label_bytes`, the format's encoded on-disk label
capacity in bytes (11 for FAT, 22 for exFAT's eleven UTF-16 units), zero for a
format without a volume label. Writer construction rejects a non-NULL label
for such a format; the CLI applies its default label only when the value is
nonzero and passes a NULL label otherwise, so a format that refuses labels,
such as QNX6, is created without `-L` and fails explicitly when `-L` is given.
