# QNX6 evidence and provenance

The [QNX6 design](../specs/design-qnx6.md) is the implementation contract.
This record explains the factual evidence behind it and the limits of that
evidence. Unfinished implementation and experiments belong in
[QNX6 qualification work](todo-qnx6.md).

## Research boundary

Reference implementations are read to identify interoperability facts, not
to acquire implementation code. No Linux/GPL source, translated routines,
source comments, declarations, or third-party parser code is incorporated
into fsimg. Downloaded references and the external fixture are kept outside
the repository. The resulting prose, validation model, equations, and tables
are independently written. Source consultation is disclosed; this is not a
claim that the author has never seen GPL code.

Code licensing and fixture provenance are separate questions. Even permissive
reference code requires per-file provenance review before reuse; none is
reused here. A parser repository's license does not automatically license
firmware or user data embedded in its sample images. No mirrored proprietary
SDK header is used as an authority, including the SDK mirror linked from the
NFI schema. No OEM map, firmware, or personal HDD image is imported.

QNX 6.5.0 native binaries are run as external test tools and inspected by
disassembly only to establish on-disk interoperability facts: field
meanings, accepted values and the decisions a native implementation makes.
No QNX code, loader bytes or translated routines enter fsimg, and no symbol
tables are imported; individual function names are cited only to locate
evidence. The ISO, extracted binaries and generated images stay outside the
repository; native images include the proprietary QNX boot loader in their
first 8192 bytes and are not redistributable fixtures as captured.

## Sources

Vendor references describe user-visible behavior and compatibility, rather
than a complete public byte-level standard. Linux sources are pinned to the
v6.12 release; the other implementations are pinned to the revisions below.

| Key | Primary source | Evidence used and qualification |
| --- | --- | --- |
| Q1 | [QNX 6.5 mkqnx6fs](https://www.qnx.com/developers/docs/6.5.0SP1.update/com.qnx.doc.neutrino_utilities/m/mkqnx6fs.html) | Classic blocks, byte order, geometry, allocation groups, reserve, identity options, long-name checksum compatibility with QNX 6.4 |
| Q2 | [QNX mkqnx6fsimg](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mkqnx6fsimg.html) | Explicit compatibility profiles, host image generation, sizes, inode capacity, mandatory directory accounting |
| Q3 | [QNX 8 mkqnx6fs](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/m/mkqnx6fs.html) | Expanded block-size range, inode checksums, expandable volume options; not a binary encoding specification |
| Q4 | [QNX fs-qnx6.so](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/f/fs-qnx6.so.html) | Compatibility markers, sparse behavior, inode checks, copies, synchronization and COW requirements |
| Q5 | [QNX Power-Safe user guide](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.user_guide/topic/fsystems_QNX6_filesystem.html) | UTF-8 and forbidden bytes; sparse and socket compatibility |
| Q6 | [QNX Power-Safe architecture](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.sys_arch/topic/fsys_QNX6_filesystem.html) | Allocation-bit meaning, system file storage, expandable capacity |
| Q7 | [QNX chkqnx6fs](https://www.qnx.com/developers/docs/6.5.0SP1.update/com.qnx.doc.neutrino_utilities/c/chkqnx6fs.html) | Native check passes, no-repair default, header-only inspection distinction |
| Q8 | [QNX 6.6 mkqnx6fsimg](https://www.qnx.com/developers/docs/6.6.0_anm11_wf10/com.qnx.doc.neutrino.utilities/topic/m/mkqnx6fsimg.html) | Explicit byte-order and long-name-checksum controls for differential experiments |
| L1 | [Linux disk layout](https://github.com/torvalds/linux/blob/v6.12/include/linux/qnx6_fs.h) | GPL-2.0 SPDX; field widths/order and numeric constants only |
| L2 | [Linux inode/superblock reader](https://github.com/torvalds/linux/blob/v6.12/fs/qnx6/inode.c) | GPL-2.0-only; geometry, CRC coverage, selection, addressing, time and symlink interpretation |
| L3 | [Linux format description](https://github.com/torvalds/linux/blob/v6.12/Documentation/filesystems/qnx6.rst) | Descriptive corroboration of tree and system-area model; not a writer authority |
| L4 | [Linux directory reader](https://github.com/torvalds/linux/blob/v6.12/fs/qnx6/dir.c) | GPL-2.0; slot sizes, long-name reference units, checksum behavior |
| L5 | [Linux MMI reader](https://github.com/torvalds/linux/blob/v6.12/fs/qnx6/super_mmi.c) | GPL-2.0; distinct header offsets and MMI interpretation |
| N1 | [NFI schema](https://github.com/NetherlandsForensicInstitute/qnxmount/blob/11c8a7f9ee9b945d584263743f6ea8524e8776d2/qnxmount/qnx6/parser.ksy) | Layout cross-check, extended header fields, sector locators |
| N2 | [NFI reader](https://github.com/NetherlandsForensicInstitute/qnxmount/blob/11c8a7f9ee9b945d584263743f6ea8524e8776d2/qnxmount/qnx6/interface.py) | Independent code path for tree addressing and non-reflected CRC |
| R1 | [ReFirmLabs extractor](https://github.com/ReFirmLabs/qnx6-extractor/blob/075434f8c07a3bc85f09394970373fe5e04f17a0/qnx6_extractor/main.py) | Layout and forensic-reader comparison; checksum and bitmap paths are not accepted as oracles |
| P1 | [PCM-Explorer format notes](https://github.com/dspl1236/PCM-Explorer/blob/a38709a0db90b9a3e70f4542b82553e93ace0f42/docs/QNX6-NOTES.md) | Author-reported PCM 3.1 observations, mixed QNX6/QNX4 HDD, classic block origin and inode census |
| P2 | [PCM-Explorer reader](https://github.com/dspl1236/PCM-Explorer/blob/a38709a0db90b9a3e70f4542b82553e93ace0f42/pcmexplorer/core.py) | Automotive comparison; permissive missing-block handling is not a sparse-format specification |
| C1 | [CRC RevEng catalogue](https://reveng.sourceforge.io/crc-catalogue/17plus.htm#crc.cat.crc-32-mpeg-2) | MPEG-2 uses an all-ones initial register, distinguishing its check value from QNX6's zero-initialized CRC |
| G1 | [PCM-Forge firmware inventory](https://github.com/dspl1236/PCM-Forge/blob/78cc9e76b2a5b7b205022e78912cee043a00603e/research/firmware/PCM31_ISO_ANALYSIS.md) | Reported native formatter/checker availability, firmware build identity, and a script's partition-device names |
| G2 | [PCM-Forge system observations](https://github.com/dspl1236/PCM-Forge/blob/78cc9e76b2a5b7b205022e78912cee043a00603e/research/PCM31_SYSTEM_INFO.md) | Author-reported checker on a PCM 3.1 HDD; not a measurement of the target 911 |
| G3 | [PCM-Forge system collector](https://github.com/dspl1236/PCM-Forge/blob/78cc9e76b2a5b7b205022e78912cee043a00603e/modules/sysinfo/scripts/sysinfo_dump.sh) | Concrete locations for firmware, kernel, process, mount, and tool inventory; script inspection only |
| G4 | [PCM-Forge HDD reader](https://github.com/dspl1236/PCM-Forge/blob/78cc9e76b2a5b7b205022e78912cee043a00603e/tools/hdd_tool.py) | Heuristic salvage implementation; incorrect ordinary block origin and incomplete integrity checks exclude it as a format oracle |
| X1 | QNX 6.5.0 x86 live ISO `qnx-6.5.0-x86-201007091524.iso` (SHA-256 `81a4f541...7dbe2`) under QEMU 11.1.2 | Controlled `mkqnx6fs`/`fs-qnx6.so`/`chkqnx6fs` experiments on a 64 MiB raw disk pre-filled with `0xaa`; see [native QNX 6.5.0 evidence](#native-qnx-650-evidence) |
| X2 | Disassembly of the X1 `fs-qnx6.so` (SHA-256 `87079603...fc58c8`), `mkqnx6fs` (`39a4cfb8...9d642`) and `chkqnx6fs` (`b991f6f2...7e`); unstripped i386 ELF | Decision logic behind X1 observations: header acceptance, feature classes, checksum, name validation, protected inodes |
| H1 | Owner-supplied image of one Porsche PCM 3.1 HDD, partly reformatted during a recovery attempt | Partition table, filesystem kinds and QNX6 structure only; see [measured PCM 3.1 HDD](#measured-pcm-31-hdd) |
| H2 | Disassembly of the head unit's `mkqnx6fs` (SHA-256 `09f998f6...22d05`) and `chkqnx6fs` (`e225fb41...1c5fd`) from H1's partition 1, build `632-1395` of 2009; unstripped SH-4 little-endian ELF | Head-unit formatter defaults and boot loader, the format-version-3 header; see [head-unit formatter and checker](#head-unit-formatter-and-checker) |
| H3 | Porsche PCM 3.1 update disc image, volume `PCM_20150721` (SHA-256 `d19c34fb...c2b021`), variant `PCM31RDW400`: `PCM3_IFS1.ifs` (`d4dfda29...aa539`) and `PCM3_Emergency.ifs` (`784e553d...12fa1`), LZO-compressed SH-4 boot images | Head-unit `fs-qnx6.so` (`f952c318...fc6ed`, build `632-1395`), device mapping, stock partition sectors and recovery format commands; see [PCM 3.1 update disc](#pcm-31-update-disc) |

NFI's repository declares [Apache-2.0](https://github.com/NetherlandsForensicInstitute/qnxmount/blob/11c8a7f9ee9b945d584263743f6ea8524e8776d2/LICENSE).
ReFirmLabs declares [MIT](https://github.com/ReFirmLabs/qnx6-extractor/blob/075434f8c07a3bc85f09394970373fe5e04f17a0/LICENSE),
with a Mathew Evans author attribution in the parser. PCM-Explorer declares
[MIT](https://github.com/dspl1236/PCM-Explorer/blob/a38709a0db90b9a3e70f4542b82553e93ace0f42/LICENSE).
Their declarations are recorded as provenance, not as a claim that their
format observations are independent of Linux or that their implementations
satisfy fsimg's safety requirements.

PCM-Forge declares [MIT](https://github.com/dspl1236/PCM-Forge/blob/78cc9e76b2a5b7b205022e78912cee043a00603e/LICENSE).
The inspected material is documentation and source code kept in temporary
research storage. No PCM tool is executed, no vehicle is contacted, and no
vendor executable is imported. The project license is not a redistribution
license for the proprietary utilities its documentation inventories.

## Reference disagreements and consequences

| Evidence | Consequence for fsimg |
| --- | --- |
| Linux describes a 512-byte CRC domain but its plain C layout ends at 392 bytes | Validate the complete raw record, including the extension region |
| Linux's name checksum uses plain `char`; it only logs a mismatch | Determine non-ASCII byte interpretation from native fixtures and enforce the qualified checksum |
| ReFirmLabs computes a reflected zlib CRC and reads its stored value with a fixed big-endian format | Do not adopt that integrity path; Linux, NFI, and fixture CRCs corroborate the non-reflected, zero-initialized convention |
| ReFirmLabs' bitmap loop omits one bit position and excludes block zero in some pointer paths | Treat it as a corroborating layout reference, not a free-space oracle |
| NFI fixes parsing to little endian and buffers whole objects on some paths | It does not establish big-endian behavior or fsimg's resource bounds |
| NFI's schema includes metadata beyond Linux's four roots | Unknown extra trees must not disappear from verification or writer accounting |
| Linux's prose assigns file-type implications to inode status | Use POSIX mode for type; measured live directories also carry status 3 |
| Linux prose simplifies each snapshot to one system half | Preserve valid shared unchanged metadata; validate allocation separately per snapshot |
| Linux's plain and MMI readers resolve equal serials differently | Treat differing equal-serial views as ambiguous rather than inheriting an accidental tie policy |
| PCM-Explorer turns absent mappings into zero-filled content | This is insufficient evidence for an automotive sparse encoding; QNX documents a distinct sparse compatibility feature |

Reference-reader success is useful evidence about that reader's behavior.
It does not establish safe handling of malformed images, complete allocation
verification, or the ability of QNX to modify an emitted volume.

## Fixture observations

F1 is NFI's externally hosted
[test_image.bin](https://github.com/NetherlandsForensicInstitute/qnxmount/blob/11c8a7f9ee9b945d584263743f6ea8524e8776d2/tests/qnx6/test_data/test_image.bin).
Its associated [generation script](https://github.com/NetherlandsForensicInstitute/qnxmount/blob/11c8a7f9ee9b945d584263743f6ea8524e8776d2/tests/qnx6/test_data/make_test_fs.sh)
uses QNX's formatter and filesystem operations. The precise installed QNX
release and formatter version are not recorded by that script. The script
and binary are independent evidence artifacts; file sizes in the script
must not be assumed to describe the checked-in binary exactly.

The binary is 409600 bytes, SHA-256
`7dc3b4f771d9c74af6df0643124c18450357cdb9b01c892d48613b11dc3a3554`.
The following are direct byte-level observations from a temporary download,
using an independently written positional decoder and bitwise CRC evaluation:

| Property | First snapshot | Second snapshot |
| --- | --- | --- |
| Header byte offset | 8192 | 405504 |
| Serial | 1 | 2 |
| Stored and independently calculated CRC | `0xad95bfb3` | `0x7d719362` |
| Block size / block count | 1024 / 384 | 1024 / 384 |
| Inode capacity / free count | 48 / 46 | 48 / 14 |
| Free blocks | 368 | 339 |
| Flags | `0x100` | `0x101` |
| Compatibility words | 4, 3 | 4, 3 |
| Allocation-group parameter | 1 | 1 |
| Inode-table length | 6144 | 6144 |
| Inode-table content references | 1, 2, 3, 4, 5, 6 | 8, 9, 10, 11, 12, 6 |
| Bitmap logical bytes / content reference | 48 / 0 | 48 / 7 |
| Long-name stream length | 0 | 0 |
| Root-directory content block | 14 | 383 |

The geometry reconciles exactly:
`8192 + 4096 + 384 * 1024 + 4096 = 409600`.
The bitmap's least-significant-bit-first interpretation gives 16 allocated
blocks in the first view and 45 in the second, matching both free counts.
Both views mark blocks 0 through 13 allocated. The second clears block 14,
which still holds the first view's root directory. Block 6 remains referenced
by both inode-table descriptors. Thus cross-snapshot sharing and differing
allocation state are observable, not hypothetical edge cases.

The second bitmap's byte 1 is `0xbf`: bit 6 is clear, corresponding to old
root block 14. Byte 47 is `0x80`: bit 7 is set, corresponding to its live
root block 383. These block-specific observations establish the LSB-first
interpretation; equal popcounts would not distinguish reversed bit order.

Two original arithmetic methods agree on the CRC: a byte-wise, MSB-first
shift register initialized to zero, and polynomial long division of the input
bitstring multiplied by `x^32` by polynomial `0x104c11db7` over GF(2).
Both produce `0x89a1897f` for ASCII `123456789` and match both stored fixture
CRCs in the table. Changing the register initialization to `0xffffffff`
produces `0x0376e6e7`, the MPEG-2 check value. No third-party checksum routine
is used in these calculations.

All four classic root descriptors have mode byte 1. The fourth is empty with
unused references set to `0xffffffff`; bytes 392 through 511 are zero. This
does not prove that a zeroed 80-byte descriptor and an empty sentinel-filled
descriptor are interchangeable.

The first view has root and `.boot` directory inodes with status 1. The second
also has ordinary directory inodes with status 3, alongside regular files,
symlinks, and a FIFO with status 3. The word at inode offset 34 is nonzero:
regular files commonly have 1, leaf directories 2, and the populated root 6.
This correlates with link counts but does not establish a universal mask,
overflow rule, or the meaning of every bit. It specifically rules out blindly
zeroing the word or classifying all status-3 inodes as regular files.

These observations validate layout, geometry, CRC, and some accounting
relationships. They do not qualify an fsimg writer, long-name checksums,
big-endian encoding, indirect trees, MMI, sparse files, or the exact system
reservation algorithm. No QNX native validator or read/write mount is run as
part of this byte-level research.

## Porsche evidence

P1 describes one PCM 3.1 drive with two classic-layout QNX6 partitions and
a QNX4 navigation partition. Its QNX6 headers occur 8192 bytes into each
partition, with 1024-byte blocks and the ordinary 12288-byte data origin.
The notes report different active header positions in the two partitions.
They also report partition identifiers `0x4d`, `0x4e`, and `0x4f`, despite
their mixed filesystem contents. This supports structural probing and exact
slice boundaries instead of a partition-type-to-filesystem shortcut.

These are the author's reported measurements; the drive measured here is
described under [H1](#measured-pcm-31-hdd). The notes do not establish every
911 model year, PCM firmware variant, HDD geometry, or compatibility with the
Linux MMI-specific layout. The referenced reader supplies no formatter or
native write-after-create evidence. Porsche reproduction therefore requires a
device-specific compatibility manifest and both filesystem and disk-level
qualification. The QNX6 specification alone cannot promise a bootable,
complete replacement HDD.

## PCM-Forge: native evidence and conflicting assumptions

The pinned PCM-Forge material adds a concrete route to native experiments,
but supplies neither a QNX6 formatter implementation nor the unknown writer
field encodings in its inspected filesystem sources.

### Native tools and firmware identity

G1 reports `mkqnx6fs`, `chkqnx6fs`, `fdisk`, `dinit`, `df`, and `du` in the
PCM's `/mnt/data/tools/` inventory. G2 separately reports a 44 KB
`chkqnx6fs` there on a Cayenne PCM 3.1. H1's partition 1 holds `mkqnx6fs`,
`chkqnx6fs`, `fdisk` and `dinit` in `/tools`, and its 44433-byte
`chkqnx6fs` agrees with G2's size; see
[head-unit formatter and checker](#head-unit-formatter-and-checker).
Running them, and their mounted path on the head unit, require an SH-4
QNX target.

G1 identifies a QNX 6.3.2 PSP3 firmware build. That operating-system label does
not specify the separate filesystem module's on-disk profile. Capture the
formatter, checker, and filesystem-module identities together with hardware
and firmware identity; generic QNX 6.5 compatibility is not a substitute.

G3 identifies useful evidence locations: firmware version files beneath
`/mnt/ifs1/HBproject` or `/HBproject`, `pidin` kernel/process information,
`mount` output, `df`, and device/tool listings. Its tool-name loop includes
`dinit` but omits `mkqnx6fs` and `chkqnx6fs`. The full script also lists
`/mnt/data/tools/`, which can establish their file presence when that listing
succeeds. Confirm executable identity and options explicitly in a
filesystem-focused collector.

The script also exports unrelated personal persistence data. fsimg
qualification collects version, geometry, metadata, and synthetic-fixture
evidence without that broader export.

On an SH-4 QNX target with these tools, controlled experiments on
disposable regular-file images can vary geometry, inode capacity, groups,
names, links, and source data one factor at a time. Native check results and
post-sync, unmounted bytes then address the marker, state, reservation, and
checksum gates. Creation/mutation experiments use an isolated target and
disposable storage, never the working HDD's partitions. No native formatting
or checker result is supplied by this source inspection itself.

### Partition inventory needs direct measurement

G1 derives five device names, `hd0t77` through `hd0t81`, from `parkHDD.sh`.
P1 describes three partitions on its measured drive. A script can enumerate
possible devices without proving that all exist. Neither observation fixes
the target disk's partition count, sizes, or filesystem kinds. H1 records one
drive's partition table and the [update disc](#pcm-31-update-disc) the
mounted-device mapping; do not turn either script count into the Porsche
image-layout recipe.

QNX names a partition device after its partition type in decimal, so
`hd0t77` through `hd0t81` denote types `0x4d` through `0x51`. These are
exactly the five partition types measured on H1; see the
[PCM measurements](#measured-pcm-31-hdd).

### The HDD reader does not establish a proprietary layout

G4's `resolve_inode` searches candidate inode regions instead of traversing
the inode-table tree. Its comments attribute the difficulty to scrambled
inode numbers and a divergent Harman format. P1 explicitly rejects that
interpretation and explains the ordinary tree-based mapping. The code itself
provides a concrete reason for disagreement: `read_file` translates block
references from the partition origin, omitting the classic 12288-byte data
origin, and excludes valid block zero.

G4 also reads only 256 header bytes without validating the 512-byte CRC
domain, supports direct file blocks only, and reconstructs directory contents
by scanning rather than selecting one validated snapshot. These behaviors
make it a salvage aid, not evidence for changing fsimg's established layout
or a validator for emitted images. The spec retains uniform tree traversal,
correct slice-relative block translation, and snapshot-bound enumeration.

The native QNX 6.5.0 evidence below resolves the classic profile's header,
inode, name and system-area questions that the earlier sources left open.
The [PCM 3.1 update disc](#pcm-31-update-disc) supplies the head unit's
device mapping, partition sectors, format commands and filesystem module.

## Native QNX 6.5.0 evidence

Most X1 experiments formatted the whole 67108864-byte disk (`V`), giving `N =
(V - 16384) / B` and superblocks at 8192 and `D + N * B`; the `-n` experiment
and a 5033164800-byte second disk for deep trees and a mid-write power cut are
noted where used. Each capture records the guest commands and native tool
output and keeps the raw image; the torn-header record names the exact
host-side byte change. All captures, including those after native create,
write, delete and remount cycles, check clean. The matrix covered B = 512,
1024, 2048 and 4096, both byte orders and long-name checksums enabled and
disabled; populated volumes used B = 1024 in both byte orders and B = 512 and
4096 in little endian.

### Superblock

| Field | Native value and meaning | Evidence |
| --- | --- | --- |
| 8, serial | A fresh format writes serial 1 to both headers; they are byte-identical | X1 |
| 16, 20 | Both equal the format time on a fresh volume | X1 |
| 24, flags | `0x100` = long-name checksums enabled (the `mkqnx6fs` default); `0` disabled. `0x1` is set in the driver's in-memory copy during a read-write mount and never reached disk, including in a header committed while mounted (power-cut capture). Any bit in `0xfe00` refuses a read-write mount ("unsupported r/w features"); any bit in `0xff0000` refuses mounting ("incompatable features") | X1, X2 |
| 28 | On-disk format version, always 4 (`chkqnx6fs`: "Ondisk format: v4"). Any other value refuses mounting | X1, X2 |
| 30 | Reserved-block percentage (`-r`, default 3); the driver rejects values above 100 and reserves `N * w30 / 100` blocks (`floor(N / 100) * w30` when `N > 0x28f5c27`) | X1, X2 |
| 32 | The `-u` UUID's sixteen bytes in textual order, independent of byte order | X1 |
| 68 | Allocation-group count; `sblksanity` rejects values above 64 | X1, X2 |
| 312 | Empty descriptor: length 0, sixteen `0xffffffff` references, depth 0, mode 1 | X1 |
| 392..511 | All zero; the descriptor-shaped bytes at 392 hold zero references, not the unused sentinel | X1 |
| 512..4095 | Zero in both header areas | X1 |

Every tree descriptor written by the formatter has mode byte 1 and six zero
extension bytes. The driver selects the header with the strictly greater
64-bit serial and the first header on a tie. When both are loaded it
requires equal magic, creation time, inode count, block count, block size
and group count; the UUID is not compared. Single-header sanity requires
the volume to fit the device, free inodes below the inode count, free
blocks below `N`, groups at most 64 and the reserve at most 100 percent.
Block sizes outside 512 through 4096, or below the device sector size, are
refused.

`mkqnx6fs` computes `N = floor((V - 16384) / 4096) * 4096 / B`. Its default
inode count is one inode per 8 blocks for `N <= 0xfffff`, per 32 up to
`0x7fffff`, per 64 below `0x8000000` and per 128 above, rounded up to fill
the last inode-table block (8192 at B = 1024 on this disk, 153600 at
`N = 9830368`, B = 512). Its default group count is
`min(ladder(m), cap)`: `m` is the smaller of the bitmap and inode-table
leaf-block counts, `ladder(m)` is 1, 4, 8, 16 or 32 for `m` at most 3, 7,
15, 31 or above, and `cap` is 4, 8, 16 or 32 by the same `N` thresholds
unless `-T` selects a usage table. This reproduces every measured volume:
4, 4, 1 and 1 groups at B = 512, 1024, 2048 and 4096 on the 64 MiB disk, 1
for `-n 16384` at B = 1024, 16 at `N = 9830368` and 8 on each H1 QNX6
volume. `main` requires twice the
bitmap tree, twice the inode tree and two directory blocks to fit in `N`.

`nindirect` sizes each system tree from its leaf count `L` with
`P = B / 4`: no indirect blocks for `L <= 16`, `ceil(L / P)` for
`L <= 16 * P`, and `ceil(L / P) + ceil(L / (P * P))` for `L <= 16 * P * P`;
anything larger fails with "filesystem too large", so system trees never
exceed depth 2. At B = 512, `-i 1048576` (exactly a full depth-2 inode
tree) succeeds and `-i 1048577` fails.

### System area and snapshots

A fresh format allocates one contiguous run from block 0: the bitmap
blocks, the inode-tree indirect blocks, the inode-table leaves, then an
equal-size twin region, then the root and `.boot` directory blocks. With
`R` system blocks, every system block `k` has a fixed twin `k + R`. The
formatter zero-fills the whole run, points both headers at the primary
half and marks both halves allocated. For example, at B = 1024, `R = 1036`
(8 bitmap, 4 indirect, 1024 leaves) and the used count is
`2 * 1036 + 2 = 2074`; the same relation holds at every block size.
Within the run, the bitmap tree comes first (indirect block, then leaves),
then the inode tree's indirect blocks from the level nearest the leaves
upward, then the inode-table leaves, each level in ascending logical order.
At B = 512 that is bitmap indirect 0, bitmap leaves 1-32, inode lower
indirects 33-64, inode top indirect 65 and leaves 66-4160 (`R = 4161`).

A native snapshot rewrites a changed system block in its other twin
location and writes only the alternate header with the next serial. After
one session, header 0 still describes the empty serial-1 volume and header 1
describes serial 2, for instance bitmap block 0 moved to `0x40c`
and inode indirect block 8 to `0x414`. The next session wrote serial 3 to
header 0. Ordinary data and directory blocks outside the system run are
allocated normally. Populate/delete/remount sessions at B = 512, 1024 and
4096 confirm the twin rule at every tree level: in both resulting headers
(serials 2 and 3) every bitmap and inode-tree block, indirect or leaf, is at
its fresh-format position `k` or at exactly `k + R`, and nowhere else.
A B = 512 volume with `N = 9830368` and 1048576 inodes, giving a depth-2
bitmap tree with 19 lower indirect blocks and a depth-2 inode tree with 16
top-level indirect blocks, confirms the rule at every level after a 150 MB
file (file depth 3) was written, files deleted and the volume remounted.

The run order is a post-order of each tree's indirect blocks followed by its
leaves, bitmap tree first: every indirect block follows all of its child
indirect blocks, siblings in logical order. With 16 top-level inode
indirect blocks, each directly follows its 128 children. The order matches
every fresh capture, covering bitmap depths 0, 1 and 2 and inode depths 1
and 2.

### Header placement on the slice

`qnx6fs_vfs_relearn` sets the header offsets before reading either header:
8192 for the first and `(V & ~0xfff) - 4096` for the second, where `V` is
the device or partition length, with the data origin at 12288; the mount
fails unless that second offset exceeds 12288. `sblksanity` then requires only
`12288 + N * B` to fit below the second header. Formatting the 64 MiB disk
with `mkqnx6fs -n 16384 -b 1024` put the second header at the last 4096
bytes of the disk, not at `12288 + N * B`; the volume checked clean, mounted,
accepted a write and committed serial 2 to that end-of-disk header. The
bytes between `12288 + N * B` and the second header kept the pre-fill
pattern: the formatter does not write them.

### Power loss and header validation

A file written and committed with `sync`, followed by a 200000-byte file
and a directory written without a commit and a hard kill of the emulator,
left header 1 at serial 2 describing only the committed file and header 0
at serial 1 describing the empty volume. After rebooting, native
`chkqnx6fs` passed and the mount showed the committed file with its
content and nothing else. The uncommitted data never reached the disk, so
the test shows loss of uncommitted work and survival of committed work, not
that pre-commit writes avoid committed blocks. In the live serial-2
bitmap, the serial-1 root directory block `0x818` is free.

The loader marks each header unreadable (I/O error), invalid (bad magic or
CRC) or valid, and fails the mount if either header is invalid; only an
unreadable header is skipped. Natively, flipping bit 0 of byte `0x2100`
(inside the older, serial-1 header's CRC domain) made both read-write and
read-only mounts fail with "Corrupted file system detected", and `chkqnx6fs`,
with or without `-f`, refused the volume with "bad sblk chksum". The commit
path increments the 64-bit serial, computes the CRC over bytes 8 through 511,
writes the slot not holding the committed snapshot and toggles the
current-slot bit; the two io-blk calls around that write are unnamed indirect
calls, interpreted as cache flushes because the driver refuses devices that
lack cache synchronization.

A second power-cut test deferred automatic snapshots (`snapshot=3600`) on a
freshly created, all-zero 5033164800-byte disk at B = 1024. A 300000000-byte
file was written and committed with `sync`; it was then deleted and a
600000000-byte file was written in the background for 25 seconds before the
emulator was killed. 140703 blocks (144 MB) of the new file had reached the
disk, every one free in the committed snapshot's bitmap, and all 292969 blocks
of the deleted file still held their committed contents; they were the only
blocks on the disk holding the old file's pattern. An earlier run on a reused
disk gave the same counts. The committed header (serial 2) was the newest
valid one; after rebooting, `chkqnx6fs` passed before and after mounting, the
deleted file was present and complete, and the new file was absent.

The bitmap's set bits are exactly the allocated blocks. Bits past `N` in its
last block are set (`0xff` bytes). The formatter does not write any block
outside the system run, so unused data blocks keep stale device contents.

### Inodes

| Inode | Mode | Offset 34 | Status 101 | Content |
| --- | --- | --- | --- | --- |
| 1, root | `040775` | 3 when empty (link count) | 1 | One block: `.`, `..`, `.boot`, rest zero slots |
| 2, `.boot` | `040700` | 2 | 1 | One block: `.`, `..` |
| Never used | | | | 128 zero bytes |

The inode table is allocated and zeroed in full at format time. Offset 34 is
the POSIX link count: a second hard link increments it, directories count
`2 + subdirectories`, and a hard link is simply a second directory slot
naming the same inode. Offsets 102 through 127 were zero in every inode.
Times are Unix seconds; offset 16 (`ftime`) is the creation time. Writing a
file at `t`, appending at `t + 3` and changing mode at `t + 6` produced
ftime `t`, mtime `t + 3` and ctime `t + 6`; a later read did not update
atime.

The status byte is a bit set. Bit 0 means allocated: inode walks test it and
freeing clears it. Bit 1 is set when the driver creates an inode and when it
writes regular-file data; rewriting a formatter-created directory leaves it
clear. Formatter-created inodes have status 1, inodes created
through the filesystem 3, and deleted inodes 2. A deleted inode keeps its
other fields; its directory slot keeps the name and has inode number 0.

The driver refuses to remove inode number 2 (EACCES) regardless of its name,
so a compatible writer reserves inode 2 for `.boot`. A volume whose boot
area is all zero mounts, checks clean and accepts native writes.

Files with no content have depth 0 and sixteen unused references. The
driver uses the smallest sufficient depth: a 17-block file at B = 1024 has
depth 1. Native QNX does not zero the bytes after a file's logical end in
its last data block; symbolic-link targets are stored as file content with
the remainder of the block zeroed. New directories are spread over
allocation groups (their inodes and blocks start new groups), and indirect
blocks are allocated downward from the end of a group.

### Names

`q6_validfilename` accepts 1 to 510 bytes, rejects NUL and `/` (EINVAL) and
validates each character with the C library's `mblen`, returning EILSEQ for an
invalid sequence. Under the native locale this is UTF-8: native creation of a
name containing `ff` failed with "Illegal byte sequence", and a name beginning
with byte `80` was not created. Control bytes are not rejected. Names up to 27
bytes are inline with zero padding; longer names use the long-name marker, one
long-name block per name with the record's remainder zeroed, and zero in slot
bytes 5 through 7 and 16 through 31.

The long-name checksum is the documented recurrence over *unsigned* bytes
(`movzbl`). Native vectors: 14 copies of `c3 a9` give `0x55554083` (the
signed interpretation would give `0x55555ea3`), and ten copies of
`e4 b8 ad` give `0xfffffbaf`. Both byte orders store the same numeric value
in filesystem byte order. With checksums disabled the stored value is 0. A
computed value of zero, or an empty name, is stored as `0xffffffff`.

### fsimg writer output

The writer's layout departs from a native volume in two ways: both headers
carry serial 1 over a populated tree, and every object occupies one
contiguous run after the system run in catalog order. QNX 6.5.0 (X1) accepts
both. The header's allocation-group count is the native formatter's default
for the geometry. Eight 67108864-byte volumes, one at every block size in
each byte order, holding the `test-qnx6-write --create` fixture tree
(inline, 27-, 28- and 510-byte names, a UTF-8 name, a symbolic link, a FIFO,
a depth-1 file of 300000 bytes, a 70001-byte zero file, empty files and
directories, a synthesized `.boot` sorted among the root's names and a file
named `.boot` in a subdirectory) were written onto `/dev/hd0` by the guest
itself and run through `tools/qnx/guest/qualify.sh`: `chkqnx6fs -vv`,
mount, read every file, create files and a directory with a long name, grow
the 300000-byte file by 3 MB, append, hard-link, symlink, rename an entry to
a long name, delete files and a directory, sync, unmount, `chkqnx6fs`,
remount, read again, unmount and `chkqnx6fs` again. Every check passed and
the script exited 0; both byte orders give the same counts:

| Captures | B | Groups | Blocks used before, after | Inodes |
| --- | --- | --- | --- | --- |
| `writer-b512-{le,be}-groups-qualify` | 512 | 4 | 9076, 14982 of 131040 | 18, 20 of 16380 |
| `writer-b1024-{le,be}-groups-qualify` | 1024 | 4 | 2456, 5397 of 65520 | 18, 20 of 8192 |
| `writer-b2048-{le,be}-groups-qualify` | 2048 | 1 | 719, 2188 of 32760 | 18, 20 of 4096 |
| `writer-b4096-{le,be}-groups-qualify` | 4096 | 1 | 242, 976 of 16380 | 18, 20 of 2048 |

Five further volumes with a group count of one at every block size
(`writer-b512-le-qualify`, `writer-b1024-le-qualify`,
`writer-b1024-be-qualify`, `writer-b4096-be-qualify`, and
`writer-b2048-be-uuid-qualify` with the UUID
`11111111-2222-3333-4444-555555555555`) pass the same cycle, so QNX does not
depend on the count. The natively modified images verify with fsimg,
payload included. The captures are in `~/qnx65-vm/writer-captures/` with
their records, which name the SHA-256 of the image loaded. An empty fsimg
volume given a native empty capture's size, block size, byte order, UUID,
reserve and format time as its epoch matches that capture's headers and
allocated blocks byte for byte, apart from the CRC and sync time, at every
block size in both byte orders (`test-qnx6-write --native`).

## Measured PCM 3.1 HDD

H1 is an image of one owner's Porsche PCM 3.1 HDD, supplied for analysis
after the owner's recovery attempt reformatted some partitions with QNX
6.5.0 `mkqnx6fs`. Only structure is recorded here: no file contents,
names, identities or personal timestamps. The image stays outside the
repository.

The disk is 100030242816 bytes of 512-byte sectors with an MBR (`55aa`)
and 63-sector alignment:

| Partition | Type | First sector | Sectors | Content |
| --- | --- | --- | --- | --- |
| 1, active | `0x4d` | 63 | 4208967 | QNX6, original |
| 2 | `0x4e` | 4209030 | 2104515 | QNX6, owner reformat, empty |
| 3 | `0x4f` | 6313545 | 102767805 | QNX4, original |
| 5, logical | `0x50` | 109081413 | 2104452 | QNX6, original, empty |
| 6, logical | `0x51` | 111185928 | 84180537 | QNX6, owner reformat, empty |

The extended partition (`0x05`) starts at sector 109081350 and chains two
EBRs. Five data partitions confirm a G1-style device list rather than P1's
three volumes, for this drive.

Provenance follows from two independent marks. The head unit writes format
times from an unset clock: 7 seconds at header offset 16 for partition 1,
24 for partition 5 and 11 for the QNX4 root entry of partition 3.
Partitions 2 and 6 instead carry wall-clock format times, share one volume
identity and contain the QNX 6.5.0 build of the boot loader, which marks
them as the owner's QNX 6.5.0 reformat. Partition 5 carries the loader
embedded in the head unit's own formatter (see below), and its unset-clock
time ties it to the head unit, so by inference the head unit's formatter
made it. The owner reports that partition 5 was not reformatted during the
owner's recovery attempt, so it is the head unit's original format.

Every QNX6 volume is little endian with B = 1024, flags `0x100`, version
word 4, reserve 3, an 8192-byte boot area holding a QNX v1.2b loader, empty
tree 312 and zero bytes 392-4095 in both headers; the partition tails past
`E` read as zero. Each loader stores three 32-bit sector numbers at boot
offsets 4, 8 and 12: the partition's first LBA, the first header's sector
within the partition (16) and the second header's sector (`S2 / 512`, the
slice-end position). The head unit's loader is a different build from QNX
6.5.0's: about 2010 bytes differ and its code is 2598 bytes long against
2585. Apart from the loader, the volumes match the QNX 6.5.0
recipe:

* **Head-unit format (partition 5).** Both headers are byte-identical with
  serial 1; the system run starts at block 0 with `R = 4259`, followed by a
  zeroed twin region, all allocated; the bitmap's padding is `0xff`; root
  is `040775` with link count 3 and `.boot` is inode 2, `040700`, both
  status 1. Its inode count, 32888 for `N = 1052208`, follows the 6.5.0
  one-per-32-blocks default, and its 8 groups equal the 6.5.0 group rule.
  The owner's QNX 6.5.0 reformat of partition 2 (`N = 1052240`) has the same
  structure.
* **Original populated volume (partition 1).** Serial 8293 and 8294, so
  the two headers are adjacent snapshots. `N = 2104464`, filling its
  partition, with 65768 inodes (one per 32 blocks) and 8 groups (the 6.5.0
  group rule) and
  `R = 8514`; the newer header's bitmap root is block `0x2142 = R`, the twin
  of block 0. 69 live inodes (4 directories, 65 regular files) are all
  uid/gid 0, with status 1 for root and `.boot` and 3 otherwise; link counts
  are POSIX values; offsets 102-127 are zero. All 38 long names carry
  checksums that match the unsigned recurrence, every name is valid UTF-8
  without control bytes, and unused directory slots are all zero.
* **Owner reformat with a short filesystem (partition 6).** The 43.1 GB
  partition holds a filesystem of `N = 1052240`. Its first header is at
  8192 and its second at the partition's last 4096 bytes, not at
  `12288 + N * B`; both are valid and identical, and its loader records
  the slice-end sector. This matches the native `mkqnx6fs -n` placement
  described above; that `-n` was used is an inference, since its `N`
  equals partition 2's. The partition beyond the filesystem reads as
  zero, which in a sparse image may only mean those sectors were never
  written.

The QNX4 volume's root entry is a directory `040775` owned by uid 0 with
link count 4. Its structure is outside the QNX6 design and awaits a QNX4
design.

The head unit's formatter therefore produces the QNX 6.5.0 superblock,
system-area, inode and directory structure for empty volumes of this size,
with its own loader build, and the original populated volume obeys every
QNX 6.5.0 structural rule measured. What H1 cannot show: the original
contents of partitions 2 and 6 and behavior on other PCM 3.1 drives or
software builds. The [update disc](#pcm-31-update-disc) supplies the
mounted-device mapping and the head unit's format commands.

### Head-unit formatter and checker

Partition 1 holds the head unit's own `mkqnx6fs` and `chkqnx6fs` in
`/tools`, unstripped SH-4 builds marked `VERSION=632-1395` and dated
2009-03-19 (H2). They were disassembled, not run; no SH-4 QNX target was
available. A raw scan of the whole image finds no copy of the head unit's
filesystem module `fs-qnx6.so`.

The formatter has the same filesystem-construction functions as QNX 6.5.0's,
which adds device trim; the remaining differences are statically linked C
library, UUID and compiler helpers. Its sizing and header logic matches
6.5.0 in every inspected respect:

* `default_inodes` uses one inode per 8, 32, 64 or 128 blocks at the
  thresholds `0xfffff`, `0x7fffff` and `0x7ffffff`. `default_groups`
  applies the 6.5.0 group rule described under
  [superblock](#superblock): the ladder 1, 4, 8, 16 or 32 at 3, 7, 15 and
  31 leaf blocks, capped at 4, 8, 16 or 32 by the same thresholds.
  `default_blocks` subtracts 16384 bytes and rounds down to 4096, and
  `nindirect` refuses system trees deeper than 2.
* The `-T` usage table holds the same values as 6.5.0's:

  | Usage | Block size | Blocks per inode | Groups | Reserve |
  | --- | --- | --- | --- | --- |
  | `desktop` | 1024 | 8 | 32 | 5 % |
  | `media` | 2048 | 512 | 8 | 1 % |
  | `runtime` | 1024 | 32 | 16 | 3 % |

* `main` writes the magic, serial 1, format version 4 at offset 28 and a
  reserve of 3 percent unless `-r` or a usage table sets it. The usage text
  is identical to 6.5.0's, including `-e` for byte order and
  `-o lfncksum`.

The formatter embeds an 8192-byte boot area. Partitions 1 and 5 equal it
except for the three sector fields at offsets 4, 8 and 12; partitions 2 and
6 differ in about 2010 bytes, as the QNX 6.5.0 loader does. Partition 5's
unset-clock format time ties it to the head unit, and its loader is
consistent with the head unit's formatter; both together are the basis for
attributing partition 5 to that formatter, an inference.

The checker also recognizes format version 3, which QNX 6.5.0's tools do
not handle, and ties it to the layout without a boot area. Before reading
headers, `nobootloader` reads sector 0 and sets a global flag unless the
sector starts with `0xeb`, has `0x90` at byte 2 and ends with `55 aa`. The
flag selects the first header's position, byte 0 when set and 8192 when
clear, and the accepted version: with a boot sector the header must have
4 in the word at offset 28; without one it must have 3 in the 16-bit word
at offset 24, where version 4 has its flags. A header that is neither is
not refused by this step when the flag is clear. By inference, version 3 is
this checker's bootless layout, and it accepts no bootless version-4
volume. The Linux MMI layout (L1, L5) has the version-3 field offsets, a
header at byte 0 and data at 4096, so, also by inference, MMI volumes are
version-3 QNX6; Linux treats bytes 16 through 27, which hold the version-3
times, version and flags, as spare.

`sbv3v4conversion` turns a version-3 header into version 4 in memory. In
its native-byte-order branch:

* bytes 40 through 387 move to 48 through 395, so block size and every
  later field sit 8 bytes earlier in version 3;
* bytes 28 through 39 move to 36 through 47, and bytes 32 through 35 stay;
* the version word at 28 becomes 4 and the reserve word at 30 becomes 3;
* the 16-bit word at offset 26 becomes the 32-bit flags word at 24.

The foreign-byte-order branch was not traced end to end. Whether the
identity bytes at 32 through 47 keep their meaning after conversion was not
established.

## PCM 3.1 update disc

H3 is a Porsche field-update disc: a plain ISO 9660 image carrying 14
release variants. The `PCM31RDW400` variant was examined. Its flash
payloads include `PCM3_IFS1.ifs`, the main QNX boot image, and
`PCM3_Emergency.ifs`, the recovery image. Both are QNX image filesystems
(startup magic `eb 7e ff 00`, machine SH) whose image filesystem follows the
startup code as LZO1X chunks, each prefixed by a 16-bit big-endian length.
The variant's hard-disk module installs files below `mnt/data/tools/`;
its `mkqnx6fs` and `chkqnx6fs` are byte-identical to H1's. The `_MOPF` and
`_MOPF_9x1` boot images carry byte-identical `fs-qnx6.so` and `hddmounter`.

### Device mapping

The main boot image's `hddmounter` mounts the HDD partitions in the order
79, 78, 80, 81, 77:

| Device | H1 partition | Mount point | Filesystems tried |
| --- | --- | --- | --- |
| `/dev/hd0t77` | 1 | `/mnt/data` | QNX6, then QNX4 |
| `/dev/hd0t78` | 2 | `/mnt/share` | QNX6, then QNX4 |
| `/dev/hd0t79` | 3 | `/mnt/nav` | QNX4, then QNX6 |
| `/dev/hd0t80` | 5 | `/mnt/pv-cache` | QNX6 |
| `/dev/hd0t81` | 6 | `/mnt/media` | QNX6 |

The recovery image carries a different `hddmounter` build.

### Recovery partitioning and formatting

The recovery image's `pcm3EmergencyApp` partitions and formats the HDD. It
writes stock 512-byte partition sectors with `partition_write`, for 40 GB
and 100 GB drives. H1's MBR equals the stock 100 GB `.mopf` sector except
for the 4-byte disk signature at offsets 440 through 443, which is zero in
the stock sector, and both of H1's EBRs, at sectors 109081350 and
111185865, equal the stock ones. The stock MBR has no boot code. The image
also carries a `partition_write_disk_signature` tool; no caller was found.

The app then formats each partition, in order:

| Device | Command |
| --- | --- |
| `/dev/hd0t77` | `/proc/boot/mkqnx6fs -q /dev/hd0t77` |
| `/dev/hd0t78` | `/proc/boot/mkqnx6fs -q /dev/hd0t78` |
| `/dev/hd0t79` | `dinit -hq /dev/hd0t79` (QNX4) |
| `/dev/hd0t80` | `/proc/boot/mkqnx6fs -q /dev/hd0t80` |
| `/dev/hd0t81` | `/proc/boot/mkqnx6fs -q /dev/hd0t81 -u11111111-2222-3333-4444-555555555555` |

Every QNX6 partition takes the formatter's defaults, and the media
partition a fixed UUID; the app's banner names a "Jukebox partition UUID".
When `hd0t78` or `hd0t77` fails to mount, the app reformats it with the same
command; for `hd0t80` and `hd0t81` it only reports the failure. H1's
partition 6 carries a different identity, consistent with the owner's
reformat. Whether the factory used this procedure is not shown by the disc.

The `mkqnx6fs` these commands run is the recovery image's own: a stripped
84230-byte binary (`91376d2c...a84de`) of the same `632-1395` build and
date as H2's, with a byte-identical boot area. H2's findings carry over to
it by inference. QNX 6.5.0's `mkqnx6fs -u` stores the sixteen UUID bytes in
textual order at header offset 32, for both byte orders, and without `-u`
writes a random version-4 UUID (X1); the head unit's formatters use the same
UUID library, so they are expected, not measured, to match.

### Head-unit filesystem module

The main boot image's `fs-qnx6.so` (`f952c318...fc6ed`) is build `632-1395`
of 2009, the same build as H2's tools. It is stripped but keeps its dynamic
symbols. It chooses the header layout by position, not by a boot-sector
test or the version word:

* `qnx6fs_vfs_relearn` places the first header at 8192, the data origin at
  12288 and the second header at the partition's last whole 4096-byte unit,
  and calls `q6_superblock_load`. When that returns 302, the driver's
  corrupt-volume result, it retries with the first header at byte 0 and the
  data origin at 4096, the Linux MMI geometry, keeping the second header at
  the slice end, so a byte-0 volume mounts only if its second header is
  there.
* `q6_superblock_load` reads both headers. A header whose read fails is
  skipped; one that reads but fails its magic or CRC makes the load return
  302, which therefore also triggers the byte-0 retry. Of two valid headers
  the larger serial wins.
* With the first header at 8192, the selected header is copied into memory
  and the word at offset 28 must be 4, or the mount fails with error 275;
  anything but 4 is refused there, including version 3. Only when the first
  header could not be read does the load also test the selected header for
  3 in the 16-bit word at offset 24 and return 302, sending the driver to
  the byte-0 layout; why the test depends on that status was not
  established.
* With the first header at byte 0, every header is converted from version 3
  to version 4 in memory, with the checker's moves, before any version test,
  and converted back to version 3 before it is written; its CRC is computed
  over bytes 8 through 511 of the version-3 form. A version-4 header at
  byte 0 is therefore misread rather than refused.
* The single-header checks equal QNX 6.5.0's: `D + N * B` must not exceed
  the second header's position, free inodes must be below the inode count
  and free blocks below `N`, at most 64 groups and a reserve of at most 100.
  The flag classes are the same: a bit in `0xff0000` refuses mounting and a
  bit in `0xfe00` a read-write mount.
* Two valid headers must agree on magic and format time. With the first
  header at 8192 they must also agree on block size, block count, inode
  count and group count, as in QNX 6.5.0; at byte 0 those are not compared.

QNX 6.5.0's driver tries only the header at 8192 and requires version 4,
so the head unit reads and writes bootless version-3 volumes, the MMI
layout by inference, and QNX 6.5.0 refuses them. The foreign-byte-order
branch of the conversion stores the 16-bit flags at offset 26 as a 32-bit
word in the wrong byte order in both directions; by inference a
big-endian version-3 volume with flag `0x100` reads as `0x10000`, in the
unmountable class. The head unit is little endian, so this does not affect
its own volumes.
