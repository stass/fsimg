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

These are the author's reported measurements; no Porsche drive image is
available in this research corpus. The notes do not establish every 911 model
year, PCM firmware variant, HDD geometry, or compatibility with the Linux
MMI-specific layout. The referenced reader supplies no formatter or native
write-after-create evidence. Porsche reproduction therefore requires a
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
`chkqnx6fs` there on a Cayenne PCM 3.1. This makes target-native formatting
and checking a specific lead instead of assuming that an unrelated QNX SDK
must supply the oracle. Availability and command semantics still require
confirmation on the actual 911 or a matching isolated target.

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

After matching-tool availability is established, controlled experiments on
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
the target disk's partition count, sizes, or filesystem kinds. Record its
actual partition table and mounted-device mapping; do not turn either count
into the Porsche image-layout recipe.

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

The unresolved writer gates remain: exact compatibility flags and modes,
unused/deleted inode state, link/extension fields, system reservation and
group rules, identity encoding, and non-ASCII long-name checksums. Native
tools could resolve them through measured output; inventory names and a
heuristic reader do not resolve them directly.
