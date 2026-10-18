# QNX6 Power-Safe reader and writer

## Scope and evidence

`qnx6` denotes the Power-Safe filesystem, not QNX4, IFS, ETFS, or every
filesystem used by a QNX operating system. The reader consumes one immutable
snapshot. The writer constructs a new volume suitable for subsequent use by
QNX; it does not modify an existing volume or implement online copy-on-write.
Readers and writers remain independently registered capabilities.

The compatibility objective includes Porsche 911 PCM 3.1 head-unit HDD volumes,
classic QNX6, Audi MMI, and extended Power-Safe variants wherever their
encodings and interoperability can be established. Classic QNX6 supplies the
fully described common addressing model; it is not the product's permanent
compatibility ceiling. Each variant receives separate read and write gates.

This document is an original description of disk-format facts and fsimg
requirements. It contains no imported source, translated driver routines,
copied C declarations, or copied reference-document prose. Linux is a GPL
read-only reference, not an implementation dependency or code donor. The
[research record](../docs/qnx6-research.md) identifies sources, their licenses,
observations, and conflicting evidence. This is source-informed format
research; it does not claim a separation between researchers who have seen
reference code and implementers who have not.

Evidence has three meanings here:

* **Established layout**: directly corroborated by the cited implementations
  or vendor documentation. This establishes byte interpretation, not that all
  combinations are safe or accepted by QNX.
* **Observation**: a measured property of a named fixture. An observation is
  not a universal format rule or a substitute for differential experiments.
* **fsimg policy**: a deliberate validation, capability, or construction rule.
  It can be stricter than another reader without redefining the disk format.

An unresolved encoding is a compatibility gate, not permission to write zero,
copy a template image, or infer semantics from a field's name. The
[qualification work](../docs/todo-qnx6.md) tracks the evidence needed to close
these gates. Feature advertisement requires that evidence and the acceptance
tests below. The design covers both reading and writing even where writer
qualification requires facts a read-only driver cannot establish.

## Compatibility profiles

| Profile | Reader contract | Writer contract |
| --- | --- | --- |
| Classic, QNX 6.5-compatible | Plain unencrypted volumes; both byte orders; 512, 1024, 2048, or 4096-byte blocks; validated feature markers | New ordinary volumes in the same profile; regular files and directories; deterministic allocation and two consistent snapshots |
| Classic without boot area | Same plain layout with first header at byte zero, after unambiguous validation | Not emitted by the default writer |
| Audi MMI 3G | Separate superblock decoder and checksum policy; explicit variant selection if detection is ambiguous | Separate qualification; never emitted by selecting the classic profile |
| Extended Power-Safe | Identify recognized extensions and reject unsupported semantics explicitly | Separate qualification of each extension and target release |

### Porsche PCM 3.1 HDD compatibility

An independently published PCM 3.1 investigation reports two ordinary-layout
QNX6 volumes with 1024-byte blocks and a QNX4 navigation volume on one HDD.
It also reports QNX6 under partition-type bytes commonly associated with QNX4.
This is evidence about that drive, not every Porsche 911 or PCM generation.
Use validated filesystem structures to identify each bounded volume. Do not
select Linux's MMI layout from a Porsche/Harman product name. [P1]

Qualification records the actual partition table, mounted-device mapping,
firmware build, and formatter/checker/filesystem-module identities. Script
device lists do not define geometry. PCM-Forge's inventory of native
`mkqnx6fs` and `chkqnx6fs` utilities supplies a lead for obtaining matching
tools; presence and compatibility are checked on the target. A QNX 6.3.2
operating-system label alone does not establish the filesystem profile.
The [PCM-Forge evidence assessment](../docs/qnx6-research.md#pcm-forge-native-evidence-and-conflicting-assumptions)
distinguishes these leads from verified native results. [G1]

A complete replacement HDD requires more than creating one valid QNX6 volume:
the disk composition layer preserves or reconstructs validated partition
bounds, sector geometry, required boot/locator data, and any other filesystem
volumes. QNX4 requires its own reader/writer design. A Porsche compatibility
manifest records PCM hardware/software identity, disk and partition geometry,
per-volume profile and byte order, namespace, supported metadata, links, and
content hashes. The parser never infers filesystem kind from the label QNX 6
on an operating-system build.

Automotive image reproduction also requires the applicable ownership,
permissions, links, and mandatory system directories. The generic
regular-file/directory baseline is not sufficient to claim a replacement
Porsche HDD. Qualification compares a rebuilt image against an authorized
source and exercises it on the matching head unit or representative isolated
system. A successful Linux read or a checksum-only check does not establish
head-unit compatibility. No proprietary firmware, maps, boot code, or personal
drive contents are bundled with fsimg's synthetic tests.

The PCM corpus explicitly checks for unused-reference values inside a live
file's logical range. PCM-Explorer describes these as sparse holes on its
target, but its decoder also zero-fills failed mappings. Native target reads
and marker/extent evidence must distinguish an actual sparse representation
from missing or corrupt storage. A confirmed sparse encoding requires a
qualified decoder for that automotive profile. [P1]

### Profile selection

The classic profile is a compatibility target, not the claim that every volume
created on QNX 6.5 is supported. QNX documents different formatter profiles for
6.5, 6.6, and 7.0. Larger blocks, encryption, extra superblock copies, expandable
volumes, inode integrity checks, ACLs, sparse files, and socket inodes are
distinct features. Their disk markers and payload representations must be
understood before acceptance. OS version strings alone do not identify an
on-disk feature set. [Q2], [Q3], [Q4], [Q5]

The default writer uses little endian, 1024-byte blocks, a boot-area layout,
and one allocation group. Explicit big endian and other classic block sizes
are independently qualified. A group is an allocation-policy subdivision,
not permission to relocate the data origin. Readers do not infer group-local
pointer bases. [Q1]

The driver maintains a closed table of accepted compatibility-marker tuples,
flag meanings, root modes, inode extension encodings, and optional metadata
layouts. Each row names its evidence and fixture profile. Unknown
read-incompatible features fail opening; unknown write-incompatible features
cannot be emitted. An unknown marker is not ignored because Linux ignores it.
The raw words at superblock offsets 28 and 30 are not parsed as decimal QNX
release numbers. Determining their marker semantics is a prerequisite for
populating this table, not a task for heuristic detection.

## Shared owners and representation

The resource context owns memory, scratch, work accounting, and cancellation.
Bounded I/O owns all volume-relative byte access. The common source catalog
owns input identities and repeatable reads; the output coordinator owns
publication. QNX6 code obtains neither ambient host paths nor unbounded file
descriptors. The extractor continues to accept only regular files and
directories under the safe profile.

A QNX6 volume object holds validated geometry, byte order, profile, and one
selected snapshot descriptor. A tree descriptor holds a logical byte length,
sixteen block references, and an indirection depth. The same private tree
decoder serves inode tables, bitmaps, long-name storage, directories, and
ordinary content. It does not introduce a mutable inode cache or a VFS.
Reader state and immutable writer plan rows remain separate.

Offsets and products use checked `uint64_t` arithmetic. Disk integers are
decoded from bytes; native structure layout, alignment, host `char`
signedness, host endianness, and host `stat` constants are not disk codecs.
All project-defined C types have `_t` typedefs.

## Classic volume geometry

The established classic layout has a 512-byte superblock record inside each
4096-byte reserved superblock area. Let:

| Symbol | Unit and meaning |
| --- | --- |
| `H` | Bytes before the first superblock: 8192 normally, zero for the bootless layout |
| `B` | Filesystem block bytes, one of 512, 1024, 2048, 4096 |
| `N` | Number of addressable filesystem blocks from the header |
| `D` | Data-block origin in volume bytes: `H + 4096` |
| `S2` | Second superblock position: `D + N * B` |
| `E` | End of reserved filesystem storage: `S2 + 4096` |

Block reference `p` names the range starting at `D + p * B`. Block zero is a
valid metadata block. Neither the boot area nor either superblock area is
included in `N`. The entire interval `[0, E)` must fit the input slice, and
every block access must fit both `[D, S2)` and the slice. [L1], [L2], [R1]

```text
0                 H             D                         S2          E
| optional boot   | header area | N filesystem blocks      | header area |
                  | 512 bytes  | system + ordinary data   | 512 bytes   |
                  | + padding  |                         | + padding   |
```

For a writer's exact classic partition/slice length `V`, require 512-byte
sector alignment. Set the internal end `E = floor(V / 4096) * 4096` and
`N = (E - 16384) / B`, requiring `E >= 16384`. Metadata and user content
must fit after this calculation. The output still has exactly `V` bytes;
initialize `[E, V)` to zero. These are framing constraints, not a promise
that a 16384-byte filesystem can hold even an empty root. Automatic sizing
accounts for metadata and reserve before rounding its output to 4096 bytes.
For example, QNX's 1028099-sector partition has `V = 526386688`,
`E = 526385152`, and `N = 514032` at `B = 1024`. A reader does not mistake
trailing container bytes for a larger filesystem. Deployment onto a QNX
partition requires matching its exact length, including alignment tail.
Other device-sector profiles require separate qualification. [Q1], [Q2]

There is no arbitrary image-wide magic scan. Classic candidates are the plain
header at byte 8192, the bootless plain header at byte zero, and the MMI header
at byte zero. Verify each in-range candidate and reject competing interpretations.
The plain layout at byte zero is not automatically MMI. The boot sector is
not executed and a partition-type byte is not proof of format validity.
Discovery of a backup from a damaged first header is recovery work outside
the strict profile; do not derive trusted geometry from a bad checksum.

Larger-block formats need different alignment and superblock-area rules.
In particular, the QNX sizing documentation and the NFI parser account for
additional space when `B > 4096`. Extending the legal block-size list while
retaining `D = 12288` is incorrect. The classic formula is not generalized
without a separately qualified geometry decoder. [Q2], [N1]

## Superblock fields and integrity

Offsets below are bytes from the start of a classic superblock. All numeric
fields use the detected filesystem byte order. The magic value is
`0x68191122`: its bytes are `22 11 19 68` in little endian and `68 19 11 22`
in big endian. The identity field is sixteen opaque bytes, not a host-endian
integer. [L1], [N1], [R1]

| Offset | Bytes | Interpretation |
| --- | --- | --- |
| 0 | 4 | Magic |
| 4 | 4 | Superblock CRC |
| 8 | 8 | Snapshot serial |
| 16 | 4 | Volume creation time |
| 20 | 4 | Volume access/update time; precise writer meaning is profile-qualified |
| 24 | 4 | Flags |
| 28 | 2 | First compatibility/version word |
| 30 | 2 | Second compatibility/version word |
| 32 | 16 | Volume identity / label storage |
| 48 | 4 | Block size |
| 52 | 4 | Inode capacity |
| 56 | 4 | Free inode count |
| 60 | 4 | Block count `N` |
| 64 | 4 | Free block count |
| 68 | 4 | Allocation-group parameter |
| 72 | 80 | Inode-table tree descriptor |
| 152 | 80 | Allocation-bitmap tree descriptor |
| 232 | 80 | Long-name tree descriptor |
| 312 | 80 | Additional system tree; semantics require profile evidence |
| 392 | 120 | Extension/reserved region; covered by CRC |

The 392-byte structure described by the Linux header is not the complete
512-byte integrity record. The NFI schema describes a further tree at 392,
two four-byte fields at 472 and 476, and 32 trailing bytes. Its names for
these objects are research clues, not authorization to ignore or synthesize
them. Classic and extended profiles specify which fields are absent, empty,
or meaningful, including the representation of an empty descriptor. [L1], [N1]

The superblock CRC covers exactly bytes `[8, 512)`, in stored byte order. It
uses a 32-bit, most-significant-bit-first CRC with polynomial `0x04c11db7`,
initial register zero, no input/output reflection, and no final XOR. It does
not include the magic, stored CRC, remaining superblock-area padding, or an
appended length. The resulting integer is stored using filesystem byte order.
This is not the default reflected CRC exposed by zlib or the POSIX `cksum`
utility's complete file-checksum convention. [L2], [N2]

Independent arithmetic check values are `0` for empty input and `0x89a1897f`
for the nine ASCII bytes `123456789`. Polynomial long division and an executed
bitwise shift-register calculation independently agree on these values and
both fixture CRCs, as recorded in the research evidence. CRC-32/MPEG-2 uses
an all-ones initial register and has a different check value, `0x0376e6e7`;
it is not this zero-initialized convention. [C1]
The research fixture provides two
complete 504-byte test inputs with stored values. Tests must cover both disk
byte orders and a changed byte near offset 511; checking only decoded fields
misses part of the integrity domain.

fsimg's strict snapshot policy validates both headers before selection:
magic, CRC, byte order,
profile, geometry, counts, tree capacities, and ranges. Stable identity,
block size, block count, inode capacity, layout, and compatible feature
interpretation must agree; serials, free counts, timestamps, flags with
documented snapshot meaning, and tree roots may differ. Reject a profile
transition whose compatibility cannot be established.

Select the larger unsigned 64-bit serial. Equal serials are accepted only for
equivalent snapshot descriptions; conflicting roots or state are ambiguous.
There is no guessed wraparound ordering. The default strict reader requires
both headers to validate and does not silently fall back to an older snapshot
after a checksum or tree failure. Every subsequent metadata read uses the
selected descriptor, including inode-table and long-name roots. [L2]

## Tree descriptors and addressing

Every classic tree descriptor has the following 80-byte representation:

| Offset | Bytes | Interpretation |
| --- | --- | --- |
| 0 | 8 | Logical stream length |
| 8 | 64 | Sixteen 32-bit filesystem block references |
| 72 | 1 | Indirection depth `L` |
| 73 | 1 | Tree mode, interpreted by the accepted profile |
| 74 | 6 | Profile-defined extension/reserved bytes |

An inode embeds the same pointer/depth model at different offsets. Each
indirect block consists of `P = B / 4` references. All branches have the same
declared depth; data occurs only at the leaves. This is not the Unix scheme
of mixing direct, single-indirect, and double-indirect slots in one inode.
At depth zero the sixteen references point to content blocks. At depth `L`,
the logical capacity is `16 * P^L` content blocks. [L1], [L3], [N1]

For logical block `k`, select root slot `floor(k / P^L)`, then the successive
base-`P` digits of `k mod P^L` identify the indirect slots, from highest to
lowest place. For example, at `B = 1024`, `P = 256`, and `L = 1`, logical
block 257 uses root slot 1 and indirect slot 1. Checked division and
multiplication establish these values without shifts by an untrusted count.

The supported depth is 0 through 5, subject to representable capacity and
volume limits. Compute `ceil(length / B)` without overflowing `length + B`.
If a capacity product exceeds the integer domain, use a checked comparison
against the required count or reject it; never wrap and accept. A 64-bit
length does not override the 32-bit reference space or the bounded adapter.

The unused-reference value is `0xffffffff`. Every reference followed for a
required block is different from this sentinel and smaller than `N`. Zero
is not a hole. In the classic nonsparse profile an absent required leaf is
corruption, never an invitation to return zeros. Errors reading indirect
blocks also remain errors. Modern sparse encodings require their own feature
decoder; they are not inferred from this unused sentinel. [L1], [L3], [Q4]

The writer uses the smallest sufficient depth and fills every unused pointer
slot with the sentinel. It pads a partially occupied data block with zero.
Unused pointer slots therefore contain defined nonzero bytes; zeroing an
entire indirect block is not a valid empty-pointer encoding.

A reader never returns bytes beyond an object's logical length. Native QNX
can overallocate storage; valid references beyond the logical end cannot
simply disappear from allocation accounting. The accepted profile specifies
whether such references represent preallocation. Full verification visits
and owns their indirect/data blocks without exposing their contents. Unknown
extra allocation is rejected rather than ignored, and sparse/preallocation
interpretations remain distinct. [Q4]

Every walk has a fixed depth bound and charges pointer visits and I/O to the
session. A repeated ancestor block is a cycle even though the depth bound
would eventually stop it. Repeated blocks in different branches require an
ownership check, not just cycle detection. Keep the current path in bounded
memory and record broader ownership in scratch.

## Inodes and metadata

Inode numbers start at 1; inode 1 is the root directory. Inode `i` resides
at logical byte `(i - 1) * 128` in the selected inode-table tree, not at a
fixed physical disk offset. Validate `1 <= i <= inode_capacity` and complete
128-byte coverage in that stream before reading it. [L1], [L2], [N2]

| Offset | Bytes | Interpretation |
| --- | --- | --- |
| 0 | 8 | Logical content length |
| 8 | 4 | Owner ID |
| 12 | 4 | Group ID |
| 16 | 4 | Additional time field, called `ftime` by the references |
| 20 | 4 | Modification time |
| 24 | 4 | Access time |
| 28 | 4 | Inode change time |
| 32 | 2 | POSIX file type and permission bits |
| 34 | 2 | Extension/link metadata; do not assume it is unused |
| 36 | 64 | Sixteen 32-bit block references |
| 100 | 1 | Indirection depth |
| 101 | 1 | Inode state/status |
| 102 | 2 | Profile-defined metadata |
| 104 | 24 | Profile-defined extension/reserved bytes |

Type comes from the disk mode masked with octal `0170000`: `0100000` is a
regular file, `0040000` a directory, `0120000` a symbolic link, and `0010000`
a FIFO. Permission bits are decoded explicitly. The status byte is not a
file-type substitute: the research fixture has live directories with status
3 as well as reserved directories with status 1. Linux's symbolic names for
these numbers do not establish an exhaustive state machine. [L1], [L2], [F1]

Modification, access, and change times are unsigned 32-bit Unix-epoch seconds
in the classic reader interpretation, with one-second precision. They do not
use the host timezone and are not signed 2038-limited fields. The representable
range is 0 through 4294967295 seconds. `ftime` is not exposed as creation
time without independent confirmation of its meaning. Inode change time is
not file creation time. Required unrepresentable timestamps fail planning;
optional loss follows the common metadata policy. [L2]

The baseline public entry model carries files, directories, and supported
timestamps. It does not thereby gain owner, group, mode, link-count, birth-time,
ACL, or device fidelity. A metadata/API extension must represent each field
and its validity before the driver advertises that fidelity. Unsupported
attributes cannot be mapped to unrelated FAT attribute bits. With no selected
POSIX metadata, writer policy is uid/gid zero, directory permissions 0755,
and regular-file permissions 0644, independent of process umask.

The common source model treats host hardlinks as separate source entries
unless an explicit topology-preserving contract exists. The baseline writer
assigns them separate inodes. An image with multiple ordinary names for one
validated regular inode can be read only under a qualified link encoding;
ownership belongs to that inode once, while output quotas count every emitted
path. Directory sharing and cycles remain errors. Do not infer a special
hardlink inode format from the Linux prose description alone.

Symbolic links have a distinct mode and Linux reads their target bytes from
the inode content tree. They are never followed during fsimg path selection
or extraction. Until the public entry model represents a special kind, an
operation requiring that entry returns unsupported, not a fabricated regular
file or silent omission. Full verification cannot report complete support
after skipping such objects. [L2]

Free inode, deleted inode, root, `.boot`, extension/link fields, and any
integrity-check encoding have explicit profile recipes. Filling fields at 34
or 101 with guessed zeros is prohibited. The fixture's extension word often
matches POSIX link counts, but that observation does not prove its complete
bit layout. Similarly, unknown trailing inode bytes are not necessarily
padding in an extended profile.

## Directories and names

A directory's content is an array of 32-byte slots. Its logical length is
a multiple of 32. Scan the declared stream; an unused slot is not an
end-of-directory marker. Root and parent relationships are validated without
following `.` or `..` as traversal edges. The writer places those two entries
first; root's two references both identify inode 1. [L3], [L4]

| Offset | Short-name slot | Long-name slot |
| --- | --- | --- |
| 0, four bytes | Target inode | Target inode |
| 4, one byte | Name byte length, 1 through 27 | Marker `0xff` |
| 5 | Up to 27 name bytes through offset 31 | Three profile-defined bytes |
| 8, four bytes | Part of inline name | Zero-based long-name stream block index |
| 12, four bytes | Part of inline name | Long-name checksum when enabled |
| 16, sixteen bytes | Part of inline name | Profile-defined tail |

Slots with inode zero or length zero are unused in the reference readers;
fsimg does not dereference their other fields. For a live slot, lengths 28
through 254 are invalid. Inline names are length-delimited, not dependent on
a trailing NUL. A 27-byte inline name consumes the entire name area. [L4], [N1]

A long-name index `j` identifies logical byte `j * B` of the long-name tree.
It is neither a filesystem block reference nor an ordinary inode number.
Read a two-byte length in filesystem byte order followed by that many bytes;
the record must fit both its block and the tree's declared length. The maximum
name is 510 bytes. Writers use this representation for names longer than
27 bytes, allocate one full block per name, and zero its remaining bytes.
No terminator is included in the length or checksum. [L4], [N2]

The Linux long-name checksum is a 32-bit recurrence with initial value zero:
take half the previous unsigned value, add the next character value modulo
`2^32`, then toggle the high bit if the previous value was odd. These are
ordered operations; rotating first and adding later can differ on overflow.
It is not the superblock CRC. For ASCII byte values, check examples are
`a -> 0x00000061`, `ab -> 0x80000092`, and
`abc -> 0x400000ac`. [L4]

The reference uses plain C `char` for character values. Its behavior for
UTF-8 bytes above 127 therefore does not establish a portable signedness rule.
The checksum's enabled marker, byte interpretation for non-ASCII names, and
disabled-field value must be qualified against QNX-generated long names in
both byte orders. Do not accept either checksum indiscriminately or let the
host compiler choose. A qualified checksum mismatch is corruption; Linux's
diagnostic-only handling is not fsimg policy. MMI does not use this checksum
under its Linux profile. [L4], [Q1], [Q8]

The fsimg name profile uses strict UTF-8 components, compared byte-for-byte with case preserved
and case significant. There is no locale-dependent folding or silent Unicode
normalization. Reject embedded NUL, slash, control bytes `0x01` through `0x1f`,
`0x7f`, invalid UTF-8, and ordinary children named `.` or `..`. The 510 limit
counts encoded bytes, not Unicode characters. Host normalization and case
collisions remain the extractor's separate responsibility. [L4], [Q5]

Directories follow the [prepared-directory contract](design-directories.md).
Each entry's namespace key is its exact UTF-8 name bytes, which gives one
canonical interpretation for listing, selection, content reads, and extraction.
Directory owner tracking is paged and separate from the configured depth limit.

## Allocation bitmap and snapshots

Bit value one denotes an allocated block; zero denotes a free block. [Q6]
The classic profile uses the bit numbering observed in F1: bit `p` describes
filesystem block `p`, with byte `floor(p / 8)` and bit `p mod 8` from least
significant to most significant within that byte. F1's root pointers and
individual bitmap bytes establish this correspondence; popcount alone cannot
distinguish bit order. [F1]
The logical bitmap covers at least `ceil(N / 8)` bytes. Its declared length, padding,
and tree allocation are independently bounded. Bits outside the block domain
in its final storage are not counted as free; the classic convention marks
them allocated. [Q6], [L3], [F1]

Inode-table and bitmap storage, including their indirect blocks, occupies a
reserved system region associated with the two snapshots. Ordinary data and
long-name storage also consume allocatable blocks. A writer accounts for all
metadata and both system reservations before offering free space. The reserve
for future copy-on-write operations is distinct from system metadata and
distinct from a source file's logical zeros.

The kernel documentation describes two system halves associated with the two
headers. Do not turn this description into a rule that every selected inode
table block must belong exclusively to one half: fixture F1's newer snapshot
retains an unchanged inode-table block used by the older snapshot. A snapshot
can also stop owning ordinary data still referenced by its predecessor.
Each bitmap describes its own snapshot; applying the active bitmap to every
old-snapshot block reports false corruption. [L3], [F1]

Bootstrap bitmap pages on demand through the geometry-checked, cycle-bounded
tree decoder. Record every bitmap leaf and indirect block read in a paged
pending ledger before consulting it. To validate an encountered block's bit,
resolve just the required bitmap page; add newly encountered bitmap blocks
to the same ledger. Drain this work until all encountered blocks' bits have
been checked. A block is enqueued once, so allocation-bit dependencies can
refer back to an already recorded bitmap block without recursive validation.
Pointer-tree cycles still fail independently. Any clear required bit fails
the operation before exposure.

This work is the dependency closure of accessed pages, not an unconditional
bitmap scan at open. The ledger, pages, pending work, and I/O obey memory,
scratch, work, and cancellation limits; an adversarially large closure fails
at those limits. Cached validated pages remain valid only within the immutable
reader snapshot. Self-reference does not authorize unchecked pointers or
unlimited bootstrap work.

Ordinary selected reads also check the allocation bit of every followed
indirect, inode, directory, long-name, and payload block against the selected
snapshot. The bootstrap exception ends before exposing any entry or content.
Known system reservations cannot be returned as ordinary file payload.
Referenced inodes must have a qualified live state; deleted/free inode data
cannot be surfaced by a stale directory reference. These local checks do not
claim the complete cross-object ownership analysis of full verification.

Full verification is snapshot-aware. It records block ownership by snapshot,
object, logical range, and role, externally sorts the records, and merges
them with streamed allocation bits. Within one snapshot, unrelated objects
cannot share a data block; metadata/data aliases and duplicate indirect
ownership fail. Separate directory references to one supported regular inode
do not create separate owners. Cross-snapshot sharing is not itself an error.

The verification accounting includes reserved system space, reachable metadata,
ordinary objects, and any profile-defined allocation records. Free counts,
inode counts, and unowned allocations are checked only using the qualified
reservation and inode-state model. Neither counting only reachable user files
nor treating every allocated but unreachable block as a leak is sufficient.
An unknown system tree prevents a complete verification claim. CRC success
and successful directory enumeration do not establish allocation correctness.

## Writer planning and emission

### Inputs and deterministic policy

The writer consumes the frozen common catalog. It validates name bytes,
collisions, source lengths, supported kinds, metadata representability,
profile, exact or automatic volume size, byte order, block size, inode
capacity, identity, and requested free reserve before output mutation.

Format settings have typed validation. The settings distinguish filesystem
block size from host or device sector size, inode capacity from live entries,
and a 16-byte volume identity from the common 32-bit serial option. The
snapshot sequence is not the volume identity. A UUID and a textual label are
mutually exclusive where the QNX profile uses the same storage; an absent
label is not silently replaced by FAT's `NO NAME`. Identity byte encoding,
text padding, and any selector flag are qualified as a unit. [Q1]

The deterministic catalog order assigns root inode 1 and, when required by
the qualified profile, `.boot` directory inode 2. If the catalog contains
`/.boot` as a directory, assign that directory inode 2, retain its children
with their ordinary independent inode numbers, and validate the directory's
selected metadata against the profile. If absent,
add a synthetic plan-owned directory with the profile's required metadata;
the frozen source catalog is not mutated. A non-directory at that path,
duplicate entry, or incompatible required metadata fails planning. Compatible
source directories are neither overwritten nor rejected merely for existing.
Remaining IDs follow canonical catalog order. Directory
slots place `.` and `..` before children sorted by raw UTF-8 bytes. Long-name
indexes follow the same traversal order. Reproducible mode fixes all emitted
times and identity bytes; defaults never depend on host byte order, umask,
time zone, iteration order, scratch placement, or memory budget.

Inode capacity covers user and mandatory system-directory inodes plus any
requested spare capacity. Round it to a whole inode-table block and include
the resulting unused inode records in both storage planning and initialization.
Directory length is whole-block rounded capacity for its live slots, with
zeroed unused slots. Normal file length is the exact source length. A zero
length ordinary file needs no content blocks and has depth zero and unused
root pointers. Zero sources are allocated and initialized as ordinary classic
file content; they do not introduce a newer sparse-file encoding.

### Count trees before assigning blocks

For `n` dense content blocks at minimal depth `L`, let `P = B / 4`.
The tree occupies `n` content blocks plus
`sum(ceil(n / P^j), j = 1..L)` indirect blocks. An empty tree occupies zero
blocks. Each division is rounded without addition overflow. The descriptor
itself resides in the inode or superblock, not in another allocated block.

Apply this count separately to every file and directory, the long-name stream,
the inode table, and the bitmap. A long-name stream with `m` names has `m`
content blocks even when each name is much shorter than `B`. Inode-table
content is `ceil(128 * inode_capacity / B)` blocks. Bitmap content is
`ceil(ceil(N / 8) / B)` blocks for the classic nonexpandable candidate.

Two complete system-tree copies provide a simple new-image layout. Their
reservation includes both copies' indirect blocks and the format's required
alignment and allocation-policy overhead. This dense-tree arithmetic is
established by the addressing model; the exact system-region reservation and
placement rules are a separate writer qualification gate. The formula alone
does not prove that QNX can subsequently update the image.

Automatic sizing solves the dependency between `N`, bitmap storage, total
metadata, and free reserve with checked monotone sizing and a finite ceiling.
Choose the smallest aligned candidate satisfying all requirements within the
chosen profile; do not change the requested filesystem family or byte order.
For exact sizing, reject insufficient capacity before emission. Every
iteration either increases the candidate or terminates with a plan/error;
resource limits and cancellation also bound planning work.

Persist assignments as paged rows keyed by catalog ID, plus externally sorted
allocation runs. Assign deterministic contiguous ranges where possible; a
contiguous range still uses QNX6 tree pointers. Fill indirect blocks from the
plan in logical order. Do not allocate a resident vector proportional to
volume blocks, inode capacity, long names, or catalog size.

### Prepare two independently valid snapshots

Both headers describe complete, internally consistent filesystem views.
Use different deterministic serials, 1 and 2, so snapshot choice is
unambiguous. Both views contain the same user namespace and content; ordinary
immutable payloads can be shared. Each view's system descriptors reference
its planned system metadata, and its own bitmap/free counts conform to the
qualified reservation rules. Do not create the second view by copying a
header whose system pointers refer to the wrong reservation.

The precise flags, compatibility words, root modes, inode state/link fields,
unused-inode recipe, mandatory directory metadata, bitmap reservation, and
boot-area locator bytes are part of a versioned writer profile. These values
are derived from controlled QNX formatter experiments and native validation.
One readable fixture or a Linux mount does not supply a complete recipe.
Until the profile has this evidence, writer registration remains disabled
rather than producing plausibly readable images with unsafe free-space data.

### Emit only a validated immutable plan

The common [emission contract](design-emission.md) owns preflight,
initialization, payload copying, flush, and failure marking. The driver emits
in this order:

1. Request one payload copy per regular file over its planned block ranges.
   Then emit directory and long-name blocks, then indirect/system blocks and
   allocation metadata from the plan.
2. Emit both 512-byte superblocks with their complete CRCs after the structures
   they describe. Initialize the remaining header-area and boot-area bytes
   according to the qualified nonbootable profile; do not import a QNX loader.

Every final partial data block, unused inode byte, directory slot, free block,
reserved tail, and volume padding byte has a defined value, either written by
the driver or zero from common initialization.

This ordering helps inspection but is not a live-filesystem atomic update
protocol. fsimg's guarantee comes from private construction and common
publication. It does not promise power-failure-safe in-place edits or replace
QNX's device synchronization requirements. [Q4]

## Audi MMI and extended layouts

The Linux MMI variant starts its first header at byte zero and block zero at
4096. Its common first sixteen bytes contain magic, CRC, and serial. Compared
with the plain header, the principal differing offsets are: [L1], [L5]

| Offset | Bytes | MMI interpretation |
| --- | --- | --- |
| 16 | 12 | Variant-specific bytes |
| 28 | 12 | Variant identity |
| 40 | 4 | Block size |
| 44 / 48 | 4 each | Inode capacity / free inodes |
| 52 / 56 | 4 each | Block count / free blocks |
| 60 | 4 | Variant-specific bytes |
| 64 / 144 / 224 | 80 each | Inode / bitmap / long-name roots |
| 304 | 80 | Additional tree |
| 384 | 128 | Remaining integrity-covered bytes |

The superblock CRC still covers bytes 8 through 511. The second header is at
`4096 + N * B` for the documented classic-size MMI layout. Linux omits
long-name checksum enforcement for this variant. Its ordinary and MMI paths
also choose different winners on equal serials; fsimg instead rejects an
ambiguous tie. MMI byte orders, unknown header fields, system allocation, and
writer recipes require their own fixture matrix. Shared magic does not merge
these profiles.

Extended profiles define, rather than infer, large-block geometry, sector
locators, redundant-header selection, extension trees, inode checksums and
their coverage, ACL ownership, encrypted names/content, expandable-volume
capacity, sparse reference encodings, and special inode kinds. In particular,
all-zero file content and a missing classic pointer are not evidence of a
sparse encoding. A wrapper such as a partition table or trusted-disk container
is handled outside the filesystem driver through an explicitly bounded slice.

## Resource and failure requirements

Keep only selected headers, fixed-size descriptors, a bounded tree path,
charged cache pages, and the current entry resident. Metadata cache keys
include snapshot identity where interpretations differ. Sequential reads
retain a tree cursor and batch adjacent physical ranges. A random content
lookup follows at most `L + 1` content-tree blocks; allocation checking can
add bitmap-tree lookups for each encountered block and bounded bootstrap
dependency work. Cache hits avoid repeated validation reads, but the advertised
cost includes those bitmap misses and the paged ledger. No file read rescans
the entire inode table or repeatedly enumerates its parent directory.

Scratch grows with directory entries, plan rows, and ownership records, not
copied payload. Full verification can require work proportional to inode
capacity and allocation bitmap size; ordinary opening and selected reads do
not imply that scan. All loops, including empty directory slots and unused
inode scans, obey session work limits. A small logical entry count must not
permit unlimited metadata work. Scratch exhaustion fails explicitly without
an unbounded in-memory fallback.

Distinguish unrecognized magic, recognized unsupported profile, corrupt
supported structure, arithmetic/host-range rejection, resource exhaustion,
source change, cancellation, and I/O failure through common errors. A failed
pointer lookup never becomes EOF or a zero extent. Diagnostics contain
bounded, escaped name data and no directory slack, freed inode contents,
uninitialized buffers, or decrypted secrets from unsupported profiles.

## Acceptance and independent validation

Use original BSD-2-Clause synthetic fixture generators for unit tests, with
offset tables and CRC mathematics independently encoded from the production
decoder. Reference sources are not pasted into those generators. Every
imported binary fixture has per-file provenance and redistribution review;
a repository license alone does not establish the rights to its image
contents. The research fixture remains an external research input.

The minimum format matrix includes:

| Area | Required cases |
| --- | --- |
| Geometry/probe | Both endians; all classic blocks; sector-aligned length with a nonempty 4096-alignment tail; bootless and MMI distinctions; forced-format checks; bounded slices; truncated headers; overflow; competing valid interpretations; modern large-block rejection |
| Snapshots | Either header newer; different namespaces in the two views; shared unchanged system block; active/inactive allocation differences; equal conflicting serials; bad CRC in either header; poisoned padding through byte 511 |
| Trees | Zero-length object; valid block zero; 16/17 leaves; each indirect transition; fragmented leaves; maximum depth; overflowed capacity; unused required pointer; out-of-range pointer; ancestor cycle; same-block branches; metadata/data overlap |
| Inodes | First/last ID; zero/out-of-range ID; truncated table; each accepted state; directory with status 3; nonzero link/extension word; unsupported special type and extension; timestamp boundaries |
| Names | 1/27/28/510/511 bytes; multi-byte UTF-8 boundaries; long-name index zero; index vs inode confusion; checksum disabled/enabled; non-ASCII checksum vectors; embedded NUL/slash; invalid UTF-8; duplicate siblings; host collisions |
| Directories | Empty directory; unused slots before live ones; missing/duplicate/wrong dot entries; self/ancestor links; multiple parents; wide directory forced to scratch; unsupported entry without silent omission |
| Allocation | Bitmap self-validation; both system reservations; bit 7/8 boundary; final padding bits; live unallocated block; cat/extract rejection of free payload and indirect blocks without prior verify; allocated preallocation beyond EOF; orphan allocation; free counters; full/near-full images; unused inode initialization |
| Writer | Exact and automatic sizing; inode exhaustion; existing compatible `.boot` tree; synthesized `.boot`; wrong-kind/metadata conflict; both endian profiles; deterministic serials/identity; free reserve; root metadata; zero sources; complete byte initialization |
| Failure | Allocation/scratch failures; short reads/writes; premature EOF; zero progress; source mutation; output/source alias; cancellation; flush/close/publication failure; no successful partial output |

For a nontrivial implementation defect, add and demonstrate a failing
regression before fixing it. Run ASan and UBSan on parser, allocation,
arithmetic, and emitter changes; qualify LSan/MSan on supported hosts. Report
unavailable tools and skipped sanitizers as such.

Independent acceptance is bidirectional:

* Generate controlled images with QNX `mkqnx6fs` or host-side `mkqnx6fsimg`,
  populate them on an isolated QNX test system where necessary, unmount, and
  compare fsimg enumeration/content with a QNX-generated manifest.
* Check fsimg images with the matching QNX `chkqnx6fs` without repair flags,
  then mount disposable copies on the target QNX release, read the complete
  tree, and exercise create, grow, rename, hardlink, unlink, sync, unmount,
  and remount. Compare their resulting snapshots and data with fsimg.
* Use Linux's QNX6 reader as a separate compatibility check for its supported
  subset. Record kernel revision, byte order, variant, and checksum behavior.
  NFI and ReFirmLabs parsers can provide additional comparisons, but their
  unvalidated paths are not correctness authorities.
* Compare byte-identical fsimg output across source order, budget, sort fan-in,
  scratch location, locale, and timezone. Scan all unused bytes for poison
  and verify that unused pointer slots retain their defined sentinel.

Native write-after-create testing is essential: a read-only implementation
cannot prove that reservation, free inode state, or snapshot metadata permits
safe future writes. `chkqnx6fs -s` only displays/validates the selected header;
it is not a complete filesystem check. Ordinary tests remain unprivileged;
native mounts and hostile-kernel tests run only in disposable isolated test
infrastructure. External tools are development dependencies, never runtime
requirements. [Q7]

For PCM 3.1, use matching target-native formatter/checker versions when
available and confirm their options before generating fixtures. Reported
utility names are not proof of executable availability on the target. Native
experiments vary one format property at a time on disposable regular-file
images in isolated infrastructure, record complete commands and tool identity,
and capture image bytes after sync and unmount. The production HDD is an
input reference, not experiment storage. This evidence supplements ordinary
unprivileged synthetic tests. [G1]

Fuzz bounded probes, snapshot choice, tree mapping, inode decoding, directory
slots, long names, and ownership verification. Scale tests separately grow
logical volume size, inode capacity, entries in one directory, fragmentation,
and file size beyond 4 GiB. Record metadata passes, I/O calls, memory, scratch,
logical bytes, transferred bytes, and RSS; a sparse host fixture is not a
full-volume transfer benchmark.

## Source keys

Links and provenance details for these keys are in the
[research record](../docs/qnx6-research.md#sources).

| Key | Source |
| --- | --- |
| Q1 | QNX 6.5 `mkqnx6fs` |
| Q2 | QNX `mkqnx6fsimg`, geometry and compatibility profiles |
| Q3 | QNX 8 `mkqnx6fs`, feature and geometry options |
| Q4 | QNX 8 `fs-qnx6.so`, compatibility, sparse files, synchronization |
| Q5 | QNX Power-Safe user guide, filenames and compatibility |
| Q6 | QNX Power-Safe architecture, bitmap and inode storage |
| Q7 | QNX 6.5 `chkqnx6fs` |
| Q8 | QNX 6.6 `mkqnx6fsimg`, endian and long-name checksum options |
| C1 | CRC RevEng catalogue, contrasting MPEG-2 initialization |
| L1 | Linux v6.12 disk-layout declarations, factual offsets only |
| L2 | Linux v6.12 inode, superblock, and tree reader |
| L3 | Linux v6.12 QNX6 format description |
| L4 | Linux v6.12 directory/name reader |
| L5 | Linux v6.12 MMI superblock reader |
| N1 / N2 | Pinned NFI QNX6 schema / reader |
| R1 | Pinned ReFirmLabs QNX6 extractor |
| F1 | Pinned NFI synthetic test-image observations |
| P1 | Pinned PCM-Explorer Porsche PCM 3.1 format investigation |
| G1 | Pinned PCM-Forge native tool and firmware inventory |

[Q1]: https://www.qnx.com/developers/docs/6.5.0SP1.update/com.qnx.doc.neutrino_utilities/m/mkqnx6fs.html
[Q2]: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mkqnx6fsimg.html
[Q3]: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mkqnx6fs.html
[Q4]: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/f/fs-qnx6.so.html
[Q5]: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.user_guide/topic/fsystems_QNX6_filesystem.html
[Q6]: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.sys_arch/topic/fsys_QNX6_filesystem.html
[Q7]: https://www.qnx.com/developers/docs/6.5.0SP1.update/com.qnx.doc.neutrino_utilities/c/chkqnx6fs.html
[Q8]: https://www.qnx.com/developers/docs/6.6.0_anm11_wf10/com.qnx.doc.neutrino.utilities/topic/m/mkqnx6fsimg.html
[C1]: https://reveng.sourceforge.io/crc-catalogue/17plus.htm#crc.cat.crc-32-mpeg-2
[L1]: https://github.com/torvalds/linux/blob/v6.12/include/linux/qnx6_fs.h
[L2]: https://github.com/torvalds/linux/blob/v6.12/fs/qnx6/inode.c
[L3]: https://github.com/torvalds/linux/blob/v6.12/Documentation/filesystems/qnx6.rst
[L4]: https://github.com/torvalds/linux/blob/v6.12/fs/qnx6/dir.c
[L5]: https://github.com/torvalds/linux/blob/v6.12/fs/qnx6/super_mmi.c
[N1]: https://github.com/NetherlandsForensicInstitute/qnxmount/blob/11c8a7f9ee9b945d584263743f6ea8524e8776d2/qnxmount/qnx6/parser.ksy
[N2]: https://github.com/NetherlandsForensicInstitute/qnxmount/blob/11c8a7f9ee9b945d584263743f6ea8524e8776d2/qnxmount/qnx6/interface.py
[R1]: https://github.com/ReFirmLabs/qnx6-extractor/blob/075434f8c07a3bc85f09394970373fe5e04f17a0/qnx6_extractor/main.py
[F1]: ../docs/qnx6-research.md#fixture-observations
[P1]: https://github.com/dspl1236/PCM-Explorer/blob/a38709a0db90b9a3e70f4542b82553e93ace0f42/docs/QNX6-NOTES.md
[G1]: https://github.com/dspl1236/PCM-Forge/blob/78cc9e76b2a5b7b205022e78912cee043a00603e/research/firmware/PCM31_ISO_ANALYSIS.md
