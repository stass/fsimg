# FAT reader and verifier

The FAT12, FAT16, and FAT32 reader uses the common resource session, bounded
I/O, Unicode codecs, and metadata store. It owns no host descriptors and keeps
no complete allocation table or directory tree in memory. The Microsoft FAT
specification linked from [format contracts](design-formats.md) defines the
disk layout. The author's BSD-2-Clause libarchive reader supplies format
examples, with provenance in [references](../docs/references.md).

## Identification and geometry

Opening reads the 512-byte boot prefix and validates the jump, signature,
sector size, power-of-two sectors per cluster, reserved area, table count,
table capacity, root layout, and declared length against its input slice.
Cluster count determines the variant at the 4,085 and 65,525 boundaries.
The last valid cluster is `MAX = CountOfClusters + 1`; links above MAX and
below the variant's bad-cluster marker are reserved. Geometry excludes the
bad marker and higher values from allocatable cluster identifiers. The
supported profile accepts 512/1024/2048/4096-byte sectors, one or two FATs,
and allocation units through 512 KiB. FAT32 revision zero is supported.
An explicit format must match the validated variant. The FAT probe does not
match an exFAT boot record, and a forced FAT name on one fails BPB
validation. Opening does not enumerate directories or read file payloads.

One bounded table window serves FAT entry decoding, including FAT12 entries
crossing a window boundary. Mirrored tables must agree for every decoded entry;
full verification compares all meaningful entries. FAT32 nonmirrored mode uses
only its declared active table. Reserved entries, dirty/error flags, FSInfo,
and backup boot geometry are checked within their named verification scope.
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
each directory cluster once. Nonroot directories require first-position dot
and dotdot entries with the correct self/parent clusters and no file payload.
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

Verification consumes a reader before ordinary iteration starts. Directory and
file chain walks emit bounded cluster-run ownership records. One external sort
orders physical ranges; overlapping runs are corruption. A single streamed FAT
pass checks entries, mirrored copies, free/bad/reserved values, and allocation
against the sorted ownership stream. Allocated unowned clusters and references
to free/bad clusters fail. FAT32 FSInfo free-count hints are checked when known;
its next-free hint is range-checked rather than treated as allocation authority.
Payload read-through is a separate flag and report scope.

The common reader owns the input borrow, session, entry snapshot, state, and
driver cleanup. Argument/state errors detected before work preserve state.
Parsing, I/O, quota, cancellation, and source-change failures invalidate the
snapshot and release private traversal/index state. EOF is stable. Information
records report header validation, independent read/write capabilities, and
unknown totals with validity bits. Structural success is reported only after
the complete traversal, ownership sort, and allocation-table scan succeed.
