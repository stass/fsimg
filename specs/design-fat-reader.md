# FAT reader and verifier

The FAT12, FAT16, and FAT32 reader uses the common resource session, bounded
I/O, Unicode codecs, and metadata store. It owns no host descriptors and keeps
no complete allocation table or directory tree in memory. The Microsoft FAT
specification linked from [format contracts](design-formats.md) defines the
disk layout. The author's BSD-2-Clause libarchive reader supplies format
examples, with provenance in [references](../docs/references.md).

## Identification and geometry

A FAT volume is a boot record with a legal fixed BPB that either carries
the PC boot signature or whose first FAT echoes its media byte. The legal
fixed BPB has a supported sector size, a power-of-two sectors per cluster,
nonzero reserved sector and table counts, a defined media byte, and no
exFAT signature. The PC boot signature is an x86 jump (`EB ?? 90` or
`E9 ?? ??`) with `0x55AA` at offset 510. The echo is the strict form of
the media check mtools makes: the first FAT, after the reserved sectors the
BPB declares, begins with `BPB_Media` and two bytes of ones. The three
bytes hold `FAT[0]` and `FAT[1]` on FAT12, `FAT[0]` and the low byte of
`FAT[1]` on FAT16, and the low three bytes of `FAT[0]` on FAT32, so an
unsigned FAT12 or FAT16 volume whose `FAT[1]` is a legal end-of-chain value
other than all ones is not identified. mtools also accepts a first
FAT beginning with three zero bytes, which some Atari disks carry; fsimg
does not, since zeros vouch for nothing. The echo identifies FAT disks
written for other machines, which lack `0x55AA` and often the x86 jump:
Atari ST disks begin with a 68000 branch or no jump at all, MSX-DOS disks
with `EB FE 90`, and NEC PC-98 disks use 1024-byte sectors.

Identification, at probe and at open, reads the same prefix: the first
8 KiB of the volume, or the whole volume when it is shorter. Two of the
largest supported sectors hold the start of the first FAT of any volume
with one reserved sector, as on every non-PC floppy. A boot record with
neither the signature nor the echo inside the prefix, including one whose
first FAT lies beyond it, is not FAT (`FSIMG_EFORMAT`), which keeps other
boot records that carry a BPB, such as an MBR or NTFS, from being reported
as corrupt FAT. Once identified, every later check is corruption or an
unsupported feature of a recognized volume, whichever form identified it.
A boot record identified by the signature alone has its media echo checked
at open (below).

FAT's identity checks are the voucher, the PC boot signature or the media
echo, and the reserved entries checked at open, `FAT[0]` holding the media
entry and `FAT[1]` a reserved end-of-chain value. A caller that forces a
FAT name and assumes its identity (`--force`) waives all three: the Atari
ST formatter writes neither voucher and leaves its reserved entries as
garbage, and traversal never reads `FAT[0]` or `FAT[1]`, since no cluster
number below 2 is valid. Everything else is validation and still runs: the
legal BPB, the exFAT exclusion (an exFAT boot record has no legal BPB in
any case), the variant match, every region and table check, the mirror
comparison of the reserved entries and all traversal checks. Open records
the first waived check that failed, in the order voucher, `FAT[0]`,
`FAT[1]`, and full verification then fails with `FSIMG_ECORRUPT` before
walking the volume, for example `fat12 identity was assumed: the boot
record has no PC boot signature and the first FAT does not repeat the
media byte`. An assumed `FAT[1]` holds no clean or error flags worth
decoding, so the volume reports the flagless state FAT12 always reports,
clean and without an I/O error. A volume whose identity checks all pass
reads and verifies exactly as without the flag.

Opening validates the sector size, power-of-two sectors per cluster,
reserved area, table count, table capacity, root layout, and declared length
against its input slice.
The reserved area, the tables and the root region must end within the
declared volume, and a data region of at least one cluster must follow them.
A nonzero `BPB_TotSec16` is the sector count and a nonzero `BPB_FATSz16` the
table size; the 32-bit field at offset 32 or 36 is consulted only when its
16-bit counterpart is zero. Boot sectors from before DOS 3.31 keep boot code
at those offsets, so a nonzero value there beside a nonzero 16-bit field is
ignored rather than treated as a conflict; Linux and mtools read the fields
the same way. A FAT32 volume counts its sectors in `BPB_TotSec32`, and a
nonzero `BPB_TotSec16` cannot describe enough sectors for one.
Cluster count determines the variant at the 4,085 and 65,525 boundaries.
The last valid cluster is `MAX = CountOfClusters + 1`; links above MAX and
below the variant's bad-cluster marker are reserved. A cluster count whose
MAX would reach the bad marker is corrupt, so no allocatable cluster
identifier is the bad marker or a higher value. The
supported profile accepts 512/1024/2048/4096-byte sectors, one or two FATs,
and allocation units through 512 KiB. Any nonzero FAT count is legal; once
the region geometry is valid, a count above two is a recognized volume
reported as unsupported. FAT32 revision zero is supported.
On FAT32, opening range-checks the structure pointers: `BPB_FSInfo` names a
reserved-area sector after the boot sector, and a nonzero `BPB_BkBootSec` a
sector of the reserved area. `0xFFFF` in either means the structure is
absent. The structures they locate are not read at open. Opening refuses as
corrupt a FAT32 `BPB_ExtFlags` with a reserved bit (4-6 or 8-15) set, or one
that disables mirroring and selects an active table beyond the table count.
An explicit format must match the validated variant. The FAT probe does not
match an exFAT boot record, and a forced FAT name on one fails BPB
validation. Opening does not enumerate directories or read file payloads.

One bounded table window serves FAT entry decoding, including FAT12 entries
crossing a window boundary. Mirrored tables must agree for every decoded entry;
full verification compares all meaningful entries. FAT32 nonmirrored mode uses
only its declared active table. Opening requires the active table's first
entry to echo the BPB media byte before it compares any mirrored entry, so a
non-FAT boot sector whose fields happen to form a legal BPB fails as a media
descriptor mismatch rather than as disagreeing mirrors. Reserved entries,
dirty/error flags, FSInfo contents, and the backup boot record are checked
within their named verification scope.
Mirrored mode ignores the inactive selector nibble. Full verification also
requires the declared backup BPB bytes to match the primary copy, including
ignored fields: readability of the active volume and consistency of its recovery
copy are separate checks.
Unused table padding is not interpreted as allocation entries.

## Names and metadata

Short names use CP437, mapping table version 2.00 from the Unicode Consortium.
The initial 0x05 escape denotes 0xe5. Base/extension padding and NT lowercase
flags are decoded explicitly. Long names use validated UTF-16, at most 255
code units, with descending ordinals, one last-slot flag, matching checksums,
zero type/cluster fields, and canonical terminator/0xffff padding. Malformed
live long-name sequences are corruption rather than a silent alias fallback.
Deleted records discard an incomplete sequence.

The comparison profile is `unicode-17-simple-upper`: Unicode 17.0 simple
uppercase scalar mapping, without normalization or multicharacter expansion.
Supplementary scalars are decoded before mapping. Both long names and aliases
use this same key. Thus an entry can have two equivalent keys, but a key may
not belong to two entries. Forbidden FAT characters, empty names, dot/dotdot,
and leading/trailing spaces or trailing periods are rejected for ordinary entries. Short-name
padding is removed before validation. Unicode data retains its Unicode-3.0
license and file-level provenance; the library has no Unicode runtime dependency.

Timestamp decoding uses calendar arithmetic independent of the host locale or
timezone. Zero dates denote absent metadata. Nonzero dates/times must be valid,
including leap days. Creation has 10 ms representation, modification 2 seconds,
and access one day. All are wall-clock values with unknown UTC offset.

## Directory preparation

FAT directories follow the [prepared-directory contract](design-directories.md);
the common walker owns traversal, paths, limits, and selection. The driver
decodes long-name sequences and short entries. Each entry record's payload is
the original 32-byte short entry, and its displayed name is the validated long
name or decoded alias. Every entry contributes two namespace keys, its long-name
key and its alias key, under the case-equivalence rules above. Equal keys from
different entries, such as a long name equal to another entry's alias, make the
directory ambiguous.

Directory chains are fully walked even after an end marker so allocated tails
cannot hide cycles or shared directory clusters. A paged exact bitset claims
each directory cluster once.

Dot and dotdot entries, the short entries named `.` and `..`, are structural
records and never children. The walker derives each directory's self and parent
clusters from traversal, so reading consumes nothing from them: it skips them
wherever they occur without examining their other fields, and reads a directory
that lacks them. Producers differ here. macOS marks them hidden, KolibriOS
sets case flags in their NT reserved byte, and damaged volumes omit or
misplace them. A dot record directly after a live long-name sequence orphans
that sequence and is corruption.
Full verification, like `fsck.fat`, requires nonroot directories to begin with
dot and dotdot entries with the correct self/parent clusters and no file
payload, and rejects a dot record anywhere else, including the root. Their
attribute byte must have the directory bit; the read-only, hidden, system and
archive bits are ignored, while a volume-label bit, a long-name attribute or a
reserved bit is corruption. Verification also ignores their NT reserved byte,
whose case flags have no name to apply to.
The dotdot entry uses cluster zero when its parent is the root, in every FAT
variant, including FAT32.
Volume labels occur only at root and have no allocation. The root is implicit.
Directory byte length is limited to 2 MiB; depth and path limits are independent.

## Current-file content

Common [logical content](design-content.md) delivery owns reads and ranges;
the FAT driver resolves offsets. Its initialized length equals the logical
length, so FAT content ranges are DATA. A forward cursor retains the chain
position across resolutions, bounds steps by geometry and the required file
allocation, and uses a constant-space cycle detector. Adjacent clusters form one
extent. The final required cluster must end its chain; neither missing nor
excess allocation is accepted. Empty files require cluster zero. Allocation is
validated as content resolution walks the chain, and completely during
verification.

A backward or nonsequential request builds the shared extent index in one
forward chain pass, after which binary search resolves offsets without a
chain restart.

## Structural verification and lifecycle

The [verification-helper design](design-verification-helpers.md) owns the
exclusive-run stream and cursor shared with exFAT. FAT retains its single
fused chain walk for file validation, ownership claims, and optional physical
payload reads; sharing does not add a second walk or a mandatory extent index.

Verification consumes a reader before ordinary iteration starts. Directory and
file chain walks emit bounded cluster-run ownership records. One external sort
orders physical ranges; overlapping runs are corruption. A single streamed FAT
pass checks entries, mirrored copies, free/bad/reserved values, and allocation
against the sorted ownership stream. Allocated unowned clusters and references
to free/bad clusters fail. FAT32 FSInfo free-count hints are checked when known;
its next-free hint is range-checked rather than treated as allocation authority.
With the extended boot signature, the boot-sector label must equal the root
volume-label entry, both compared as decoded text without trailing spaces. An
absent root label counts as `NO NAME`, the boot label formatters write for an
unlabeled volume, so any other boot label without a root entry, including a
blank one, is a disagreement, as `fsck.fat` reports it.
Payload read-through is a separate flag and report scope.

The common reader owns the input borrow, session, entry snapshot, state, and
driver cleanup. Argument/state errors detected before work preserve state.
Parsing, I/O, quota, cancellation, and source-change failures invalidate the
snapshot and release private traversal/index state. EOF is stable. Information
records report header validation, independent read/write capabilities, and
unknown totals with validity bits. Structural success is reported only after
the complete traversal, ownership sort, and allocation-table scan succeed.
