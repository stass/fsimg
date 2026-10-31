# QNX ETFS: on-media format research and fsimg implementation specification

**Edition:** research and engineering draft, 20 October 2026  
**Target:** `stass/fsimg` (`master`), BSD-2-Clause C11 library and command-line filesystem image tool  
**Intended capabilities:** inspect, list, stream file contents, safely extract, verify, and *create* compatible ETFS transaction-stream images, without mounting  
**Primary interoperability targets:** QNX Neutrino 6.5.x and 7.0/7.1 ETFS tools; ETFS on RAM and NAND through normalized transaction exports  
**Not claimed:** an exhaustive vendor on-flash specification, production NAND programming, in-place modification, or validated crash-consistent replay under every historical QNX release

> **Provenance and licensing.** This document is newly authored explanatory text, offset tables, constraints, design proposals, and test recipes based on publicly available vendor manuals, publicly accessible engineering notes, and permissively licensed open-source parsers. It reproduces **no proprietary source/header code or third-party implementation code**. Only interoperability facts (field widths, offsets, limits and identifiers) are transcribed in original tables. QNX utilities are external test oracles, not distributable project dependencies. The Apache-2.0 forensic parser is an *external* oracle, not a module to copy. No GPL/AGPL or proprietary code should be introduced into the BSD-2-Clause `fsimg` tree. All new `fsimg` implementation code and testing logic should be independently authored.

## 0. Status, central recommendation, evidence scheme

**Recommendation:** deliver ETFS in two sharply separated steps: **(1) strict read-only support for portable `mketfs` / `etfsctl` transaction streams**, followed by **(2) a deterministic, initially unregistered writer qualified against `mketfs` and native `fs-etfs-ram` / `etfsctl`**. Treat *raw NAND with spare/OOB bytes* as a separate adapter with board/driver-specific requirements, not as another sector size of the portable stream. ETFS is an append/transaction filesystem: it should share `fsimg`'s bounded I/O and scratch-backed metadata facilities, but it should **not** inherit FAT allocation ownership or QNX4/QNX6 fixed-inode assumptions.

The clearest currently accessible byte-oriented evidence is the Apache-2.0 [Netherlands Forensic Institute `qnxmount` ETFS Kaitai parser][R8a] and its [QNX-generated image test procedure][R8d]. It establishes an independently testable candidate 16-byte transaction trailer and 64-byte metadata record. It is not a complete vendor specification; CRC/ECC, multi-cluster transaction validity, replay, crash rollback, and some status values remain outstanding. **No QNX-native fixtures were executed in preparing this draft; mark byte observations as pending independent confirmation.**

Evidence classes used below:

| Mark | Meaning | Implementation treatment |
|---|---|---|
| **A** | Explicit public QNX behavior/limit, sometimes corroborated by another public source | Safe requirement; still test each platform profile |
| **B** | Concrete byte-layout observation in a permissively licensed parser and its published test procedure | Good initial decoder model; confirm against an independently generated binary fixture before announcing support |
| **C** | Inference, ambiguity, or observed behavior known only for a limited profile | Experiment first; preserve/report unknown values; never invent writer bytes |
| **P** | Proposed `fsimg` policy/API/algorithm, not an on-disk fact | Subject to `fsimg` design review and normal qualification |

All field offsets below are relative to the indicated *record*, not to an enclosing image. `u16` and `u32` mean fixed-width integers; do not map raw bytes to native padded C structures.

**Product lifecycle notice.** QNX SDP 8.0's October 2026 component notices explicitly list ETFS, `etfsctl`, and `fs-etfs-*` as **discontinued**. Qualify with QNX 6.x/7.x environments; do not plan to find ETFS tools in a clean 8.0 installation. [R10]

## 1. Scope and terminology [A/P]

### 1.1 Filesystem identity

ETFS is the **Embedded Transaction Filesystem**. It is a POSIX-like QNX filesystem designed primarily for NAND flash, with variants for RAM/SRAM and NOR. The same term ETFS must not be confused with:

* QNX **IFS**, the startup image filesystem produced by `mkifs`;
* QNX **EFS** (`mkefs`, commonly F3S/FFS3), a different embedded flash format;
* QNX4 (`fs-qnx4`) with directory records and extent chains;
* QNX6 Power-Safe (`fs-qnx6`) with dual snapshots and tree descriptors.

ETFS is **not** based on a fixed superblock, a conventional on-media inode table at an absolute sector, a FAT, or a persistent free-space bitmap. Every user-data or metadata write adds a transaction. On startup the driver uses transactions to reconstruct live logical files/directories and its in-memory state. The exact scan/validation strategy varies with profile. [R1, R3]

### 1.2 Separate concepts that must not be conflated

| Concept | Definition |
|---|---|
| Media page | NAND read/program granule (commonly 512, 2048, or 4096 **data** bytes, with an additional spare/OOB region) |
| ETFS cluster | ETFS logical data granule: typically 1024, 2048, or 4096 bytes, **excluding** portable 16-byte transaction metadata |
| Erase block / ETFS block | Erase granule containing an integral number of ETFS clusters; `block_size` in `mketfs` denotes its data-byte capacity |
| Transaction | A write of contiguous file clusters, tagged with file ID, file offset, count and a sequence identifier; it must not cross an erase-block boundary in NAND layout [R3] |
| Portable transaction stream | Host-readable serialized payload clusters plus generic 16-byte `etfs_trans` record per cluster, from `mketfs` or `etfsctl` [R4, R6] |
| Raw NAND dump | NAND data and potentially spare/OOB areas, with board-specific metadata/ECC layout; **not generally byte-identical** to a portable ETFS stream |
| FID | Logical file identifier; also used to address fixed system metadata files |
| Filetable | Logical system file indexed by FID containing fixed-size metadata/name entries; **not** a conventional root sector |
| Volume slice | Existing `fsimg` bounded I/O interval; may be a portable ETFS stream stored at an offset in another file |

### 1.3 Supported and deferred operations

| `fsimg` operation | Portable stream stage | Raw NAND stage |
|---|---|---|
| `info`, `ls`, `cat`, `extract` | Reader v1 | Reader adapter later |
| Structural `verify` | After replay and metadata validation; report whether integrity CRC is unverified | BSP-aware status/CRC/ECC required |
| `create` | Deterministic normalized stream, native-tested | No v1 support |
| Historical/deleted-file forensic recovery | Optional separately labeled mode later | Optional later |
| Repair, mount, in-place edit, flash erase/program | Not part of `fsimg` | Not part of `fsimg` |

## 2. Geometry, placement, portability, limits [A]

### 2.1 Public limits

From the QNX 7.1 filesystem-limits guide [R2]:

| Property | Documented limit / implication |
|---|---|
| Filename | **91 bytes**, not necessarily Unicode code points |
| Pathname | **256 bytes** excluding mountpoint and trailing NUL |
| File logical size | **2,147,483,647 bytes** (2 GiB minus one), despite a 32-bit size field |
| FIDs | Maximum **32768** logical file identifiers; 15-bit namespace |
| Typical max files | 4096, configurable by `fs-etfs-* -f`, with extension records consuming additional FIDs |
| File names longer than 32 bytes | Two filetable slots/entries, lowering effective usable file count |
| Largest ETFS cluster | 4096 bytes |
| Published theoretical filesystem size | 64 GB |
| Practical maximum NAND filesystem size | 4 GB cited by QNX (startup-scan cost, not a literal field-width boundary) |
| SLC/MLC NAND | Both supported; MLC requires hardware ECC |
| NAND page data bytes | 512, 2048, 4096; newer guidance explicitly includes 4096 |
| Software ECC | Single-bit correction per 256 bytes (hardware and BSP profile still matter) |
| Spare-area ECC | Available for 2048/4096-byte NAND pages, not the old 512-byte page layout |

These are **format/platform** limits, not promises `fsimg` can create every combination within its own memory/work/output quotas. The 32768 limit describes FID capacity, not a guarantee of 32768 independent ordinary files after system and long-name slots.

### 2.2 Common geometries

The physical NAND model documented in the community wiki [R3] and official architecture guide [R1]:

| NAND page data + OOB | ETFS data cluster | Pages per ETFS cluster | Typical spare total for those pages |
|---|---:|---:|---:|
| `512+16` | 1024 B | 2 | 32 B |
| `2048+64` | 2048 B | 1 | 64 B |
| `4096+128` | 4096 B | 1 | 128 B; later support, not available in early wiki revision |

`mketfs` accepts `cluster_size`, `block_size` and `num_blocks`. Defaults mentioned in the documentation are not sufficient to identify a binary image: the builder specifically recommends passing them explicitly. `block_size` is **erase-block size**; it is not the QNX4 512-byte allocation block and not the QNX6 block size. [R4, R5]

Derived arithmetic for a placement-aware image, subject to checked integer overflow:

```
C = ETFS cluster data size in bytes
B = erase-block data size in bytes
K = B / C                             # clusters in one erase block
N = logical media erase-block count
U = C + 16                           # bytes/cluster in portable stream
P = N * K                            # capacity in clusters if fully populated
S = P * U                            # fully padded stream length
```

Required arithmetic checks [P]: `C in {1024, 2048, 4096}`, `B > 0`, `B % C == 0`, `K > 0`, `N > 0` if specified, `U` and all products fit `uint64_t` and host offsets, a view length fits the supplied bounded slice. The permitted `B` values should be **evidence-driven**, not assumed to be exactly 16 KiB or 64 KiB.

**Important:** `mketfs num_blocks=N,0` builds an image that is **not padded out to N media blocks**; its `.counts` metadata is still sized for the target device. Therefore **stream byte length is not by itself a trustworthy device-capacity field**. Never claim `N == bytes / ((C + 16) * K)` for an unpadded stream. [R4, R5]

### 2.3 NAND OOB != generic 16-byte transaction record

QNX explains that `mketfs` serializes each cluster together with a **generic 16-byte `etfs_trans`**. When the target's `etfsctl` writes it, it removes that generic form, and the low-level `devio` layer constructs its own *BSP-specific* on-flash metadata/ECC representation. [R4, R5]

Consequences:

1. The portable input size is based on `C + 16`, **not** `512+16`, `2048+64`, or `4096+128` unless a particular physical dump coincidentally matches.
2. A NAND extraction produced without OOB/spare bytes may lack the transaction metadata needed for reconstruction.
3. Even a dump with OOB may require controller de-scrambling, ECC handling, page-pair ordering, and partition/bad-block conventions before a transaction record can be interpreted.
4. `etfsctl -r` exports portable, endian-neutral, device-neutral transactions; `-R` retains erased/blank blocks in the export. The word *raw* in `etfsctl` documentation **does not prove** a literal NAND data+OOB dump. [R6]
5. `fsimg` should not offer generic `create --nand` until it has an explicit BSP profile and native qualification on that hardware.

### 2.4 Raw partition split

An ETFS driver can expose `/dev/etfs1` as an optional **raw partition** for e.g. boot images and `/dev/etfs2` as the transactional filesystem partition. The raw area, if present, precedes the ETFS transaction region. It can be zero bytes. The reader must be given only the transactional volume slice (or an explicitly described container/layer) and must never interpret raw boot data as ETFS records. [R6]

## 3. Portable stream record structure [A for 16-byte size; B for exact offsets]

### 3.1 Serialization model

The publicly available Kaitai schema from `qnxmount` describes an ETFS stream as repeated units of `C` data bytes **followed by** a 16-byte transaction record [R8a]:

```
portable ETFS stream, candidate observed layout:

    [ cluster 0 payload : C bytes ][ transaction 0 : 16 bytes ]
    [ cluster 1 payload : C bytes ][ transaction 1 : 16 bytes ]
    ...

unit i base = stream_start + i * (C + 16)
data(i)   = [base, base + C)
trans(i)  = [base + C, base + C + 16)
```

QNX's `mketfs` utility calls the generic record a *header*; that **describes its function**, not necessarily its byte placement in every exported format. The trailing position above is **B: Kaitai-observed**, and must be confirmed bytewise for each target QNX tool generation. `etfsctl -r`, `-R`, and `mketfs` may differ in padding/erased-unit handling. [R4, R8a]

A well-formed **fully padded** candidate stream has `length % (C+16) == 0`. A truncated stream should not have its last short unit silently ignored; return corruption unless a separately specified container envelope accounts for it. A compact export also consists of complete portable units but need not fill the device's logical capacity.

### 3.2 Candidate generic transaction record (`etfs_trans`)

The following offsets are **observed in a permissive parser**, and are not copied from a proprietary system header [R8a]. Field byte order in the examined schema is **little-endian**; that does **not** establish all QNX `+bigendian` or cross-device export behavior.

| Offset | Length | Field | Interpretation | Confidence |
|---:|---:|---|---|---|
| `0x00` | 4 | `fid` | Owning logical file ID | B |
| `0x04` | 4 | `file_cluster` | Cluster index within the logical file, not physical device offset | B |
| `0x08` | 2 | `nclusters` | Claimed length of a contiguous multi-cluster transaction | B; replay semantics C |
| `0x0A` | 1 | `tacode` | Transaction-header/read-status code | B; code semantics partly C |
| `0x0B` | 1 | `dacode` | Data/read-status code | B; code semantics partly C |
| `0x0C` | 4 | `sequence` | Ordering/commit sequence identifier | B; wrap/tie semantics C |
| `0x10` | — | End | **16-byte total** | A/B |

QNX documents logical `FID`, file offset, length, sequence and data-integrity state generally [R1]. The Kaitai layout specifies the concrete widths/positions, but does **not** specify how CRC/ECC are represented once the `devio` layer has normalized a transaction. **Do not assume the 16-byte portable record itself carries a CRC field**: QNX's more general description includes CRC/ECC for the physical transaction as a whole and the BSP may carry them only in OOB. For a portable image, the presence/absence of a verifiable checksum must be reported as a verification-scope property, not faked.

### 3.3 Known and unknown status codes

The `qnxmount` Kaitai parser interprets the **low nibble** of each of the two status bytes approximately as follows; this table is a factual description of the external decoder, **not an assertion that every value is accepted in all native QNX builds** [R8a]:

| `code & 0x0F` | Candidate meaning | Reader policy [P] |
|---:|---|---|
| `0` | No indicated error | Process if whole transaction validates |
| `1` | Corrected ECC condition | Record warning/corrected status; do not apply another correction to portable payload |
| `2` | Erased | Treat as erased candidate, not file content |
| `3` | Unresolved special state | **Unknown**; investigate; do not invent semantics |
| `4` | Not described | Unsupported until observed |
| `5` | Data error | Invalidate this version; test recovery from an older version |
| `6` | Device error | Invalidate this version; record diagnostic |
| `7` | Bad block | Never use as ordinary file data |
| `8..15` | Not described | Unsupported until observed |

The **upper four bits are not characterized** by these sources. Preserve both raw bytes for diagnostics; never equate a masked `0` with a fully proven valid transaction when unknown high bits are present. Determine whether erased units use `fid=0xffffffff`, `fid=0xffff`, all-ones raw pages, status code 2, or a combination **from fixtures**; the external parser only skips some `fid == 0xffff` records and that is not a safe universal rule.

### 3.4 Record/transaction invariants [A/B/P]

A strict stream reader should validate:

* `fid` within the applicable configured namespace, after recognizing explicit erased/bad-block sentinels; reserve `0..5` as system IDs in the observed profile.
* `file_cluster` and `nclusters` can form a valid file-byte interval via checked multiplication by `C`; no unsigned wrap, no interval exceeding the size allowed by the filetable, unless specifically explained as a superseded historical version.
* `sequence` comparison handles neither ties nor wraparound by accident; treat ambiguities as unsupported/corrupt until the native ordering rule is established.
* If the format identifies a multi-cluster transaction with `nclusters > 1`, validate all member clusters, their placement, status and atomicity as one transaction. **One valid looking cluster inside a torn transaction is not sufficient**.
* For a **placement-aware** stream representing physical blocks, the contiguous transaction's member clusters lie in one erase block. For a **relocatable compact export**, be careful not to infer original erase-block locations merely from serialized adjacency.
* Deleted versions and current versions are distinguished by explicit metadata state and transaction sequencing, not simply by the fact that a stale page is still present.

## 4. System FIDs and filetable [B, corroborated in part by QNX public descriptions]

### 4.1 Fixed file IDs

The permissive reader's profile uses these predefined logical FIDs [R8a]:

| FID | Logical object | Purpose |
|---:|---|---|
| `0` | root | Root directory identity |
| `1` | `.filetable` | Persistent metadata/name records, indexed by FID |
| `2` | `.badblks` | Bad erase-block bookkeeping |
| `3` | `.counts` | Read/erase accounting for the target device's blocks |
| `4` | `.lost+found` | Recovery destination |
| `5` | `.reserved` | Reserved internal object (exact meaning/contents C) |
| `6+` | Ordinary entries and filename extensions | User namespace, subject to configured max |

These are **logical file IDs**, not fixed physical flash addresses. Wear leveling and transaction replay may relocate all of their cluster versions. QNX `etfsctl -F` explicitly flushes `.badblks` and `.counts` [R6]. Public engineering notes say `.badblks` occupies one cluster with unused entries set to an all-ones block number; `.counts` stores per-block read/erase counts [R3]. Record layouts for `.badblks`, `.counts`, `.reserved`, and recovery metadata are **not established sufficiently for an interoperable writer in this draft**. Do not fill them with guesses.

### 4.2 Filetable organization

The observed `qnxmount` representation reconstructs FID 1's cluster sequence, then interprets its data as 64-byte records. **Record array index equals FID** in its profile [R8b]. Thus one `C=1024` cluster holds 16 filetable entries, `C=2048` holds 32, and `C=4096` holds 64. A missing/stale cluster of FID 1 is **not** interchangeable with a block of all-zero records; it may disconnect the directory tree. The reader must resolve live FID 1 clusters by transaction ordering before accessing records.

For every FID `f`:

```
filetable_byte_offset(f) = 64 * f
filetable_cluster(f)     = floor((64 * f) / C)
record_offset_in_cluster = (64 * f) % C
```

Overflow checks and the configured FID capacity apply. This is a logical address into FID 1's reconstructed contents, **not** a physical seek in the ETFS image.

### 4.3 Base 64-byte metadata entry

Independent data-layout evidence comes from `qnxmount`'s Apache-2.0 Kaitai schema and its native-generated fixture tests [R8a, R8d]. The decoded layout is:

| Offset | Width | Name | Interpretation |
|---:|---:|---|---|
| `0x00` | `u16` | `extension_fid` | `0` means no long-name extension; other values may point to an additional filetable slot |
| `0x02` | `u16` | `parent_fid` | Directory owning this name; `0xffff` appears for deleted or uninitialized records |
| `0x04` | `u32` | `mode` | POSIX-style file type plus permissions |
| `0x08` | `u32` | `uid` | User ID |
| `0x0C` | `u32` | `gid` | Group ID |
| `0x10` | `u32` | `atime` | Access time, seconds since Unix epoch in candidate parser |
| `0x14` | `u32` | `mtime` | Modification time, seconds |
| `0x18` | `u32` | `ctime` | Status-change time, seconds |
| `0x1C` | `u32` | `size` | Logical file-byte size; directories observed as zero |
| `0x20` | `32 B` | `short_name_bytes` | Prefix of the filename, zero padded/terminated where shorter |
| `0x40` | — | End | **64 bytes total** |

The QNX user-guide file size ceiling is `0x7fffffff` (2,147,483,647) even though this table allocates 32 bits. No fourth creation/birth timestamp has been identified in this record. For `fsimg`, expose `FSIMG_META_ACCESSED`, `FSIMG_META_MODIFIED`, `FSIMG_META_CHANGED`, `FSIMG_META_OWNER`, and `FSIMG_META_MODE` as observed. Do **not** fabricate `FSIMG_META_CREATED`. [R2, R8a, R11]

`mode` is POSIX-style file type and permissions. QNX image fixture tests exercise regular files, directories, symlinks and FIFOs [R8d]. That maps directly to existing `fsimg` entry kinds; other node types require evidence and an explicit unsupported-entry policy. A symlink target is file payload data (it is **not** stored in the short-name field) [R8b]. `atime` often does not change on real ETFS media unless enabled explicitly, so its recorded value should be reported as-is rather than considered live POSIX access history [R7].

### 4.4 Name-extension 64-byte entry

When a name exceeds 32 bytes, an additional **64-byte filetable entry** is used. Candidate extension record:

| Offset | Width | Interpretation |
|---:|---:|---|
| `0x00` | `u16` | `0x8000` candidate extension-record identity |
| `0x02` | `u16` | FID of the *base* name entry, not its containing directory |
| `0x04` | `59 B` | Remaining name bytes following the 32-byte prefix |
| `0x3F` | `u8` | Extension type/terminator field; value `0` observed by Kaitai parser |
| `0x40` | — | End of 64-byte extension entry |

This exactly explains the published **91-byte** per-component limit (`32 + 59`) and the claim that a name longer than 32 bytes costs a second slot. [R2, R8a] Names with exactly 32 bytes should not require an extension under the public boundary; validate this with `mketfs` fixtures.

**Reference checks:** a live base entry's extension FID must be in range and identify a valid extension, its back-reference must equal the base FID, an extension cannot be shared by two unrelated bases, and extension entries do not themselves appear as files or names. A base FID that is its own extension, or an extension chain/cycle, is corrupt. Full names must be assembled from **bounded lengths**; never call `strlen()` on a full 32- or 59-byte field without proof of an in-field terminator. Test whether the 59-byte extension allows exactly 59 non-NUL bytes with `0x3f` acting as delimiter or whether the name limit includes a trailing zero. Until tested, mark the exact 91-byte edge as a writer gate.

### 4.5 Deletion/free record states

The external parser treats `parent_fid == 0xffff` as deleted/not initialized and `extension_fid == 0xffff` as an invalid/unpopulated entry. It treats `extension_fid == 0x8000` as a special extension marker [R8a]. These are useful **candidate sentinels**, but it is not yet shown which combinations are legal during power-failure recovery, how free FIDs are recycled, or whether other marker bits in a 16-bit field carry meaning. Parse raw values losslessly, distinguish **definitely free** from **recognized but unqualified**, and ensure a torn record cannot create a host output path.

### 4.6 Metadata fields, links, and names

* **Case:** assume byte-preserving and byte-comparing names for parser prototype; independently establish native case behavior. Do not use host locale, Unicode normalization, or a UTF-8 requirement merely because the external parser decodes `strz` as UTF-8.
* **Validation:** reject embedded NUL before bounded termination, path separators within components, `.` and `..` as ordinary emitted leaf names, parent cycles, multiple live entries with the same parent/name, dangling parent FIDs, and overly long reconstructed paths. The format may tolerate some recovery states; an optional forensic display should not automatically make them safe to extract.
* **Hard links:** QNX 6.5/7.0 `fs-etfs-ram` utility docs say hard links are unsupported, but the QNX 7.0 `mketfs -h` documentation explicitly offers hardlink importing. This is a **version-sensitive conflict**. Test `link()` and `mketfs -h` on the chosen native release before declaring hardlink support or an on-media link model. Until then, `fsimg` writes independent copies if its metadata-loss policy permits, and does not synthesize link counts. [R7, R5]
* **Directories:** their contents are represented by metadata parent relationships, not data pages listing child names in the FAT/QNX4 manner; the observed directory metadata size is zero. Native ETFS may omit `.`/`..` from directory enumeration, and changing a child may not update parent directory `mtime`. [R7, R8b]

## 5. Transaction replay and logical contents [A for model; C for exact recovery]

### 5.1 Required conceptual model

A live file is a mapping from logical cluster number to a *surviving physical transaction version*. The filetable itself is a file reconstructed by the same mechanism. A transaction that is superseded, failed, or incomplete must not appear as current file bytes. Physical location does not define logical order: QNX explicitly allows transactions to be placed at different flash locations and restored across devices with matching geometry. [R1, R6]

The simplest evidence-based experimental approach is:

1. Decode candidate transaction records from a bounded stream without applying their contents.
2. Reject erased, missing, or known-bad candidates; record raw status codes and physical position.
3. Group by `(FID, logical cluster)` while maintaining *whole transaction* boundaries.
4. Select the newest complete, valid transaction version using an evidence-qualified sequence comparison; invalidate older overlapping ranges.
5. Reassemble the current FID 1 `.filetable`, validate its record topology.
6. Resolve ordinary file cluster maps against live file sizes and metadata.
7. Build parent/name links and yield entries lazily to the common reader.

**Caution:** the `qnxmount` reference chooses the maximum numeric sequence for each `(FID,cluster)`, and arbitrarily prefers the latest scan position for sequence ties; it does not implement full native rollback/CRC/ECC. That is a convenient proof-of-concept, **not sufficient for fsimg's `verify` or hostile images**. [R8b]

### 5.2 A bounded, strict algorithm proposed for `fsimg` [P]

**Pass 1: geometry/open.** Validate image slice and selected container/cluster profile. Scan only metadata records through common positional I/O, charging every record and byte to the session work budget. Store candidate descriptors `(fid, first logical cluster, count, seq, stream/physical offset, raw status, source block, evidence flags)` in `lib/meta/` scratch-backed records. Never hold an entire image or all FIDs in RAM.

**Pass 2: integrity classification.** For transactions whose physical validity is defined, check all member cluster statuses and any available CRC/ECC evidence. Record `GOOD`, `ERASED`, `BAD`, `PARTIAL`, `UNKNOWN`. `UNKNOWN` is not quietly upgraded to `GOOD`. For a normalized transaction stream with no native physical CRC, use a separately named *structural-only* integrity class.

**Pass 3: replay index.** Use `lib/meta/sort.c` to sort descriptors by `(fid, logical_start, sequence_order, physical_tie_break)` and construct a nonoverlapping interval map of **accepted current** data. General case needs an interval sweep because multi-cluster writes can overwrite only part of a previous extent; a simple hash of `(FID,start)` is insufficient. Use external run merging and a scratch-backed extent index. Validate that every requested logical byte range maps either to an explicit hole/zero rule proved from native behavior or to a selected source cluster. Do not invent `0xff`/zero bytes for missing clusters in ordinary reading.

**Pass 4: metadata roots.** Use FID 1's live content, derive active filetable records, validate root/system FIDs, extension back-pointers, parent relationships and all file sizes. Note that malformed metadata may refer to physical data the replay index does not contain: fail closed.

**Pass 5: directory exposure.** Produce a stable index keyed by `(parent_fid, exact name bytes, fid)` and use `fsimg`'s prepared-directory/replay pattern. For every directory, enforce unique names and parent-cycle checks using scratch-backed visited state. Build no unbounded recursion or resident path tree.

**Pass 6: read/verify.** `cat` and extraction resolve only the requested file ranges and issue bounded positional reads of live cluster payloads. `verify` additionally checks all reachable files, orphan/unreachable metadata, extension uniqueness, transaction overlap rules, invalid/superseded versions, and (only when supported) physical CRC/ECC and bad-block bookkeeping.

### 5.3 Unresolved transaction semantics: explicit gates [C]

Until native differential fixtures settle them, treat these as unresolved, rather than specification facts:

* Whether every cluster of a multi-cluster transaction carries identical `nclusters`, only its first cluster carries the count, or some other continuation convention.
* Whether `sequence` increments per transaction, per cluster, per file, or globally; whether wraparound is defined; whether equal sequences can appear on different FIDs or after reclaim.
* How torn writes and partial multi-cluster writes are marked and rolled back; whether multi-cluster atomicity is visible solely through per-cluster headers.
* Whether replacing part of a previously written extent leaves a logical hole, retains old overlapping cluster data, or uses a multi-version merge.
* Whether a freshly created zero-length file has any payload transaction.
* Whether a sparse seek (`write()` beyond EOF) leaves a hole and, if so, whether hole reads return zeros or are materialized.
* Whether `etfsctl -r` elides superseded transactions and whether `-R` preserves their original relative placement.
* Whether metadata operations such as rename/unlink update one filetable cluster atomically, multiple metadata pages via a higher-level protocol, or a reserved recovery record.
* Whether native boot recovery chooses the newest **valid** version on CRC failure or discards an entire dependent operation group.

Document the observed rule from native tests, pin tool/version/fixture and then replace these C gates with precise normative predicates.

### 5.4 Distinguish strict reader from optional salvage

The default fsimg reader must represent one coherent live namespace or reject corruption; it must not quietly resurrect deleted files, repair invalid parents, fill missing clusters with `0xff`, or select an unverified newer version over a valid older one. Forensic recovery may be valuable, but it should be explicitly opt-in and report every uncertain entry. The existing extraction `-k` (skip unsupported entry kinds) should **not** be overloaded to mean "salvage corrupt ETFS".

## 6. Integrity, recovery, wear and physical bad blocks [A/C]

QNX specifies transaction CRC checking, ECC correction on NAND, rejection/rollback of damaged transactions at startup, dynamic/static wear leveling, read-disturb refresh, and garbage collection (copy live clusters, erase abandoned blocks). ETFS is robust because old transactions remain until newer writes are safe, not because every byte on media is valid. [R1, R3]

### 6.1 Integrity verification scopes

| Input view | What can be justified initially | What must remain unclaimed |
|---|---|---|
| `mketfs` portable stream | Fixed-width headers, metadata consistency, referenced payload spans, no uninitialized output, reproducible logical tree | Hardware ECC, physical spare CRC, native post-crash rollback without qualifying fixtures |
| `etfsctl -r` export | Same structural checks; can test against simultaneously exported mounted tree | Original physical erased/bad-block positions, device CRC/ECC |
| `etfsctl -R` export | Adds blank/erased logical slots suitable for recovery tests | Raw NAND OOB bytes unless independently proven |
| NAND+OOB dump | With explicit board profile, status and spare CRC/ECC checks may be possible | Generalized support across arbitrary controller/BSPs |
| NAND data-only dump | May support limited carving where transactions live in-band on a particular device | General successful ETFS recovery when required OOB is missing |

If a CLI prints `verify successful`, its machine-readable result must state *structural-only* versus *physical-integrity verified*. If the existing `fsimg` verify output contract cannot express that distinction, either implement an explicit capability/coverage flag or return `FSIMG_EUNSUPPORTED` for a requested full physical-integrity level. Do not conflate a parser walk with a scrub of on-flash ECC.

### 6.2 Bad block system file and wear counts

`.badblks` is described in the QNX engineering wiki as one cluster; unused block entries are all-ones. `.counts` holds read and erase counters for every physical erase block [R3]. The binary element widths, counter order, version, and relationship to the driver's bad-block table need byte-level controlled experiments; the format writer MUST not invent them. The `num_blocks` builder setting controls `.counts` sizing even when `num_pad=0`, which makes target capacity distinct from serialized stream length. [R4, R5]

Native tools provide observable state through `etfsctl -i`: block counts, clusters per block, clean/spare/filthy/inactive pools, read/write/erase counts and error summaries [R6]. Record this report alongside every fixture. Read and erase counters are runtime-mutating fields: byte-for-byte equality of an ETFS image after a native mount is **not** a reasonable universal acceptance condition; compare canonical logical contents and validate the expected changes.

### 6.3 Read/write error and ECC options

`fs-etfs-ram -C 0` disables both CRC/ECC (appropriate for disposable RAM testing); `-C 1` uses CRC without ECC; older 6.5 docs also describe `-C 2` with ECC. Driver versions differ. The `-S` option selects a simpler checksum instead of a polynomial CRC. Published QNX guidance describes BSP callbacks `devio_readtrans`, `devio_readcluster`, and `devio_postcluster` to handle spare-area CRC/ECC, with spare ECC placeholders filled with `0xff` while computing the associated CRC [R2, R7]. These are **hardware integration behaviors**, not an immediately reproducible portable 16-byte checksum layout.

Native tests should enumerate `-C` and `-S` profiles and **identify exactly what changes in stream bytes**, if anything. Do not add a fabricated CRC algorithm to `fsimg` merely to make a format writer appear complete.

## 7. Filename and POSIX metadata model [A/B/P]

### 7.1 Filenames

Names are components, not host paths, and occupy at most 91 data bytes (32 inline + up to 59 extension). Implement names as bounded opaque byte strings until an official encoding rule is confirmed. On extraction, follow the existing `fsimg` host representation and collision policies. If the host cannot create an extracted name, report unsupported rather than truncate/sanitize into a collision. The 256-byte ETFS path limit is a *target/native interoperability constraint* even if an `fsimg` host path can be longer. [R2, R8a, R11]

Test lengths: 0 (invalid except special root), 1, 31, **32**, **33**, 58, 59, 60, 90, **91**, 92 (invalid), and deeply nested paths at 255/256/257 bytes. Include exact-32 and exact-91 names with no in-field NUL; invalid UTF-8 bytes; ASCII case pairs; non-ASCII UTF-8; names containing slash/backslash/colon; and duplicates within one parent. Treat target `.` and `..` as reserved path syntax, not ordinary entries.

### 7.2 File attributes

| Entry kind | Evidence | `fsimg` treatment |
|---|---|---|
| Regular | QNX build tools and test fixtures | Supported |
| Directory | Public QNX docs and filetable test | Supported; derived from `parent_fid` edges |
| Symbolic link | QNX docs and fixture | Supported; payload contains link target |
| FIFO | QNX-generated fixture (`mkfifo`) | Read/write if native writer fixture passes |
| Character/block device | POSIX mode may encode them; fixture tests enumerate categories | Recognition then unsupported unless permitted/reliably encoded in current `fsimg` model |
| Hard link | Conflicting historical `fs-etfs-ram` documentation vs 7.0 `mketfs -h` | Version-profile gate; do not claim until tested |

Store uid/gid and mode from fixed 32-bit fields. Validate mode's type against payload semantics; a directory with arbitrary nonzero payload size or a FIFO carrying file content should be treated as unqualified unless a native fixture proves it legal. The filetable has 32-bit atime/mtime/ctime seconds; preserve signedness and 2038 boundary interpretation as undecided until native fixtures. Expose no invented nanoseconds or birth time. Source creation-time metadata is potentially lost under ETFS; use `fsimg`'s existing explicit loss reporting policy when creating an image. [R8a, R11]

## 8. Byte order, signatures, probe, unsupported profiles [A/C/P]

### 8.1 Endian behavior

The public `mketfs` buildfile has `+bigendian` and `-bigendian` settings, little endian by default [R4, R5]. The `qnxmount` Kaitai schema decodes its fixtures little-endian [R8a]. `etfsctl -r` documents an *endian-neutral* serialization that can move data across different device classes [R6]. This combination DOES NOT prove that a portable image can always be decoded by toggling a single big/little-endian flag at each 16-byte record: metadata fields and export normalization may be mixed or converted.

`fsimg` must test and record each of:

1. `mketfs -bigendian` stream bytes vs `+bigendian` stream bytes, with the same source tree.
2. `etfsctl -r` exports after loading each into matching QNX driver profile.
3. `etfsctl -R` exports and a subsequent re-import.
4. Cross-profile read/write across device type where QNX supports it.

Use explicit byte-order codecs; never cast disk bytes to host-native C structs. Fail unsupported combinations rather than silently choosing little endian.

### 8.2 ETFS has no good generic magic signature

The existing `fsimg` descriptor contract assumes that a **pure bounded-prefix** probe can identify a unique, signature-bearing filesystem before `open()`. ETFS has no known universal superblock/magic at a fixed prefix offset; the logical `.filetable` is itself a replayed transaction file that may be physically far from the beginning. File lengths divisible by `C + 16`, or a coincidental FID of `1`, are **not sufficient identity tests**. [R3, R11]

**Minimal extension proposed to `fsimg`** (rather than invent a fake ETFS magic):

* Add a **format-generic descriptor identity policy**: `prefix_signature` (current behavior) or `validated_forced_open` (new). Common code, not drivers, enforces the policy.
* A `validated_forced_open` driver is eligible only when the caller explicitly selects a format. Its `open()` performs bounded scanning and must prove a self-consistent `.filetable`, root, system-FID, geometry, and transaction model **before** returning `FSIMG_OK`. A failure to find the ETFS identity is `FSIMG_EFORMAT` (not a mysterious partial directory); recognized unsupported variant is `FSIMG_EUNSUPPORTED`.
* Automatic detection for ETFS remains unavailable for exports lacking a uniquely safe prefix signature. Optionally, accept auto detection for a **separately qualified canonical `mketfs` subprofile** with a strong prefix signature. This is a conscious compatibility limit, not a code shortcut.
* The forced-open policy never bypasses corruption, status validation, or resource quotas. It does not branch on `format == "etfs"` in any common fsimg code.

This resolves a genuine mismatch with `specs/design-formats.md`. Trying to make a universal ETFS probe out of `length % (C+16) == 0` would undermine detection exclusivity for all registered formats.

### 8.3 Geometry options for reading [P]

Provide an explicit, evidence-based `-O` profile using fsimg's existing format-specific settings vocabulary, with final spelling resolved by the API/CLI design:

```
layout=etfs-stream       # default supported normalized export
cluster=1024|2048|4096  # mandatory for ambiguous images
block=65536              # optional where available; required for native-geometry verification
endian=le|be|auto        # auto only if both interpretations prove one unambiguous valid identity
```

Avoid pretending `block` is known from stream size. Use the existing bounded `-b`/`-z` slice controls to select an ETFS region inside a larger file. For `layout=nand-oob`, require an independently specified `nand-page`, `nand-oob`, `pages-per-cluster`, `pages-per-erase-block`, device-data arrangement and a named **BSP adapter**; don't allow unspecified guessing.

## 9. `fsimg` architecture integration [P; grounded in current repository]

The `fsimg` project already has the right components: bounded positional I/O and slices; a format descriptor registry with independently optional reader/writer; resource-context budgets; stable scratch-backed records and sorting; lazy reader/content resolution; source catalogs and deterministic output; safe extraction; QNX4/QNX6 native oracles; and Ruby specification-driven testing [R11]. ETFS needs one clear layer for transactions and one for the reconstructed namespace.

### 9.1 Proposed modules

```
lib/formats/etfs/
    etfs.h            private types and contracts
    descriptor.c      "etfs" name, capabilities, recognition policy, reader/writer hooks
    codec.c           fixed-width generic transaction and filetable decoders/encoders
    geometry.c        stream layout, block/cluster checks, physical-view selector
    stream.c          bounded scan and normalization of generic transaction units
    replay.c          status qualification, complete-transaction selection, versioning
    filetable.c       FID 1 resolver, 64-byte base/extension records, system FIDs
    names.c           bounded byte names, extension links, canonical comparison keys
    directory.c       prepared directory keys, enumeration, parent graph validation
    read.c            reader lifecycle, content access, info
    verify.c          reachable graph, integrity coverage, history and block accounting
    write-etfs.h      writer-private plan records
    write-plan.c      settings validation, catalog assignment, geometry, fixed metadata
    write-emit.c      deterministic transaction serialization and source streaming

fuzz/etfs.c

test/etfs.c
test/etfs-write.c
test/etfs-integration.sh.in

test/spec/lib/fsimg_spec/etfs/
test/spec/suite/etfs/

specs/design-etfs.md
specs/design-etfs-driver.md

docs/etfs-research.md
```

Do not create a public `etfs_fs_t` or an in-memory ETFS VFS. The reader and writer should be independent, sharing **only pure codecs and an explicit format profile**. Use the shared resource context, `lib/meta` scratch storage and `lib/io` exclusively. Driver code must not open an unbounded image path, call arbitrary host filesystem syscalls, or secretly hold a RAM-sized filetable. Common code should gain only the new **format-generic** forced-open policy and descriptor registration.

### 9.2 Reader and logical-content contract

The ETFS descriptor returns regular file contents by mapping `(FID, logical byte offset, byte count)` through an independently validated live interval/cluster index to one or more `fsimg_io_read` operations. A file's visible length comes from the qualified filetable record, **not** the maximum cluster seen in the log. Directory iteration is stable over names and FIDs; it must not require a RAM-resident tree of 32768 objects. Symlinks are byte-backed content; FIFOs have no file content. Use the existing `fsimg_reader` entry metadata and common extractor semantics. [R11]

### 9.3 `info` and `verify`

`info` should display confirmed values: cluster data bytes, portable stride, stream length, candidate transaction count, configured/derived erase block size, system FID consistency, live file and extension counts, detected endian/profile, and status-count summary. It should report capacity/free-space counts **only** when independently defensible from a complete placement-aware image and the identified `.counts` schema. A compact `mketfs` stream is not a full flash-volume utilization census.

`verify` is a different operation: check the complete reachable namespace and transaction map with bounded work. Separate *logically consistent*, *replay-qualified*, and *CRC/ECC-verified* evidence levels. Corrupt data must not be treated as safely verified simply because `qnxmount` or `qnxprobe` can list it.

### 9.4 Public capability profile

The descriptor should separately advertise image-reader capability, image-writer capability (once qualified), entry kinds, metadata fields, filename/path maxima, supported layouts, possible endian profiles, and its explicit "requires cluster size / forced format" recognition behavior. Expose writer capabilities only **after** native QNX validation; before that, construct the writer through `fsimg_writer_new_variant()` and an **unregistered** test descriptor exactly as existing QNX writer qualification practice prescribes [R11].

## 10. Image creation specification [P; byte requirements B/C]

### 10.1 Creation target: `mketfs`-compatible stream

The only first-class writer target should be the generic stream consumed by `etfsctl -w`. This is a newly created, immutable output artifact, not a programmed physical NAND chip. `fsimg create` must never attempt to erase/reformat a live ETFS device. The best first target is QNX6.5/7.0 RAM ETFS with no controller-specific CRC/ECC.

The writer must accept a frozen source catalog, validate every name/path/file size, UID/GID/mode and timestamp representability, decide which losses are allowed, plan logical FIDs and all filetable extension entries, and emit a coherent sequence of transactions. Every output byte must be defined or explicitly zero/padding per observed native convention. Use normal `fsimg` safe publication and failure semantics. No mutation, insert/delete history, wear leveling or volume repair is required to create a **clean initial image**.

### 10.2 Proposed target settings

Proposed format settings; do not hard-code unproven defaults:

| Setting | Role | Qualification |
|---|---|---|
| `cluster=1024|2048|4096` | Data bytes per transaction unit | Required |
| `block=<bytes>` | Target erase-block data capacity | Required for native writer |
| `blocks=<N>` | Logical erase-block count for `.counts` | Required |
| `pad_blocks=<n>` | How many blocks to serialize; `0` means compact | Native `num_blocks=N,n` analog |
| `endian=le|be` | Target portable stream and metadata byte order | Require byte-differential fixtures |
| `reproducible` / existing `-R` | Fixed timestamps/FID assignment/stable order | Required |
| `profile=qnx65-ram|qnx7-ram|...` | Closed interoperability profile, if native layouts differ | Explicit, not magic auto switching |
| `reserve`/`spare_blocks` | Capacity policy | Defer unless these change emitted format bytes and have native evidence |

Sample **proposed** CLI (not current implementation):

```sh
fsimg create -R -t etfs -O cluster=1024,block=65536,blocks=16,pad_blocks=0,endian=le \
    -o image.etfs source-directory
fsimg info -t etfs -O cluster=1024 -i image.etfs
fsimg verify -t etfs -O cluster=1024 -i image.etfs
fsimg extract -t etfs -O cluster=1024 -i image.etfs -C extracted-directory
```

The `-O` keys are **suggested**, not established. Fit their final parsing into the current `fsimg` option schema rather than designing a one-off secondary CLI parser.

### 10.3 Deterministic writer plan

1. Validate every tree entry including dangerous paths/links; take the source-consistency snapshot and catalog already supplied by common `fsimg` code.
2. Reserve system FIDs 0–5. Assign ordinary FIDs in stable lexical traversal order; allocate extension FIDs for names 33–91 bytes. Reject namespace exhaustion including extensions.
3. Calculate how many 64-byte filetable records are needed and how many filetable data clusters they occupy. Set every metadata record and unused slot exactly according to a **native-verified initial-state profile**; do not leave unwritten stack bytes.
4. Derive `.counts` from the target block count; generate `.badblks` and `.reserved` only after capture of exact initialization format and legal defaults. Validate spare-block allocation and target capacity.
5. Plan a sequence of data and filetable/system transactions, with each placed on valid cluster boundaries and, for placement-aware padded images, no transaction crossing an erase-block boundary.
6. Choose sequence values, multi-cluster grouping rules, transaction status and checksum semantics solely from a pinned native fixture profile. No synthetic sequence scheme until matching QNX acceptance is proved.
7. Emit payload in bounded transfers directly from source callbacks; emit metadata and transaction records from bounded plan stores, and zero-fill initialized-data tails and intentionally unused image areas only where the QNX writer does so.
8. Finalize through `fsimg`'s output coordinator. A short write, source mutation, quota breach, or failed durable publication remains an explicit failure, never a successful image.

`mketfs -n` makes reproducible comparison practical; compare *logical file hierarchy* and *exact structure* before demanding bytewise identity because QNX output can contain version/initialization differences [R4, R5].

### 10.4 Writer non-goals

No journal replay engine is needed to create a brand-new image; an offline compact **single-version** serialization is enough if native QNX recognizes it. However, the writer **must** still emit system files, initial transaction headers, and sequence/commit semantics exactly as the chosen QNX formatter expects. Simply dumping data pages plus fabricated FID metadata is not interoperability.

## 11. Concrete native oracle commands and test setup [A]

These examples operate on **disposable RAM-backed ETFS**, not real NAND or vehicle devices. The actual temporary device name/mountpoint may differ; discover with `ls /dev/etfs*`. All `-e`, `-f`, and `-w` operations are destructive for their target device. Never run them on production flash.

### 11.1 Host-side `mketfs` image builder

Use licensed QNX SDP 6.x/7.x `mketfs` or `mkxfs -t etfs` where supported [R4, R5]:

```sh
# On a development host with the QNX tools installed:
mkdir -p fixture-root/sub
printf 'ETFS fixture\n' > fixture-root/sub/message.txt
printf '1234567890abcdef' > fixture-root/short

# Make a geometry-explicit buildfile. The path below is an input directory,
# not a special ETFS boot sector. Use an absolute host path if required.
cat > fixture.etfs.bld <<'BLD'
[cluster_size=1k block_size=64k num_blocks=16,0]
fixture-root
BLD

mketfs -n fixture.etfs.bld fixture.etfs
```

A 1 KiB data cluster generates **1040-byte** units in the normalized output; a 2 KiB cluster generates 2064-byte units; 4 KiB generates 4112-byte units. Note: `num_blocks=16,0` sets logical media capacity but compact output size can be far smaller. Verify specific argument forms with the installed `mketfs -?` or vendor docs because the 6.5, 6.6, 7.0, and 7.1 positional arguments differ (6.6 and later may accept a separate directory parameter).

Native QNX 7.1 also offers `cksum=`, `sha256=`, `mtime=`, `drop=`, `+/-include`; these are useful for deterministic *input* fixtures but do not prove new ETFS disk features. Use documented `-h` to construct an explicit version-specific hardlink experiment where available. [R5]

### 11.2 Runtime in-RAM filesystem oracle

The QNX manual shows the baseline:

```sh
# On a disposable QNX 6.x/7.x guest with fs-etfs-ram and etfsctl:
mkdir -p /ram-etfs
fs-etfs-ram -C 0 -e -c 0 -m /ram-etfs
etfsctl -d /dev/etfs2 -i

# If a new image must be written, use only the disposable RAM device:
etfsctl -d /dev/etfs2 -S -e -w fixture.etfs -c

# Observe the mounted content and metadata:
find /ram-etfs -print
ls -la /ram-etfs

# Export live transaction formats:
etfsctl -d /dev/etfs2 -r live.etfs
etfsctl -d /dev/etfs2 -R live-with-erased.etfs
```

Native start examples use `-C 0` (no CRC/ECC) to avoid conflating the normalized stream with NAND's spare-area integrity. The NFI fixture generator instead runs `-C 1` and records a full image via `etfsctl -R` [R8d]. That makes it a good second oracle profile, once both are understood. `etfsctl -i` is the capacity/erase geometry truth source for the QNX instance. The command-line `etfsctl` processes switches left-to-right and should be issued with `-d /dev/etfs2` **first** [R6, R7].

### 11.3 Native mutation and remount verification

Run changes against the mounted disposable RAM filesystem, export before/after, and compare all three views: source host files, current native QNX tree, `fsimg` tree. Important recipes:

```sh
printf 'old' > /ram-etfs/change
# export image A here
printf 'new' > /ram-etfs/change
# export image B here

mkdir -p /ram-etfs/d1 /ram-etfs/d2
mv /ram-etfs/d1 /ram-etfs/d2/moved
rm -f /ram-etfs/change
# export image C here

# When supported on the guest:
mkfifo /ram-etfs/fifo
ln -s d2/moved /ram-etfs/symlink
```

Repeated overwrites and `dd ... seek=... conv=notrunc` are essential to learn *overlapping transaction version* behavior. The NFI [test generator script][R8d] already exercises small/large files, partial overwrites, renames, deletes, nested directories, symbolic links, FIFO, permissions and ownership. [R8d]

### 11.4 `etfsctl` operation matrix

| Command | Purpose | Safe on a test RAM ETFS? |
|---|---|---|
| `etfsctl -d /dev/etfs2 -i` | Report native geometry/counters | Yes, observational |
| `... -r file.etfs` | Export device-neutral active transaction stream | Yes, observational |
| `... -R all.etfs` | Export including erased/blank areas | Yes, observational |
| `... -S -e -w image.etfs -c` | Stop, erase, program, resume | **Destructive**, test only |
| `... -S -f -c` | Format new empty filesystem | **Destructive**, test only |
| `... -D` | Request filetable defragmentation | Mutates underlying media; test only |
| `... -F` | Flush read/erase/bad-block accounting files | Mutates underlying media; test only |
| `... -W raw.etfs` | Import export including blank regions | **Destructive**, test only |

`etfsctl -r` exports can be written to another device when **cluster size and erase block/cluster geometry agree**, even if class and native ECC differ [R6]. Use it to compare `fsimg` writer output via native import; not as proof that `fsimg` is a hardware NAND programmer.

## 12. Third-party readers and source/test references

### 12.1 Independent, permissively licensed ETFS parsers

| Project | License | What it covers | Cautions |
|---|---|---|---|
| [NFI qnxmount][R8] | **Apache-2.0** | ETFS, EFS, QNX6 file parsing and FUSE mount; [Kaitai ETFS layout][R8a]; [transaction/filetable interpreter][R8b]; [native image recipe][R8d] and [test cases][R8e] | Linux FUSE; incomplete native replay/status/CRC checking; keeps file contents in memory; use as oracle, **not** as production verifier |
| [Alexis Brignoni qnxprobe][R9] | **MIT** | Standard-library Python identification, directory listing, ZIP extraction, ETFS/EFS and many other formats without mounting | Independent implementation but related reference lineage; repository documents synthetic ETFS qualification, **not a fully proven vehicle NAND dump** |

**Oracle independence caveat:** `qnxprobe` says its ETFS handling drew on `qnxmount`'s published scheme. Comparing the two results is useful, but does not count as two independent derivations of the disk format. The strongest oracle is the QNX vendor mount/metadata and an independently generated `mketfs` image. Preserve the exact revision (commit SHA), fixture-generating commands, licenses and validator versions in `docs/references.md` and test manifests.

**Concrete external reader commands** (from the projects' own usage documentation; check their `--help` for version differences):

```sh
# Linux + FUSE, with qnxmount installed in a Python environment:
mkdir -p /tmp/etfs-readonly
python3 -m qnxmount etfs -s 1024 image.etfs /tmp/etfs-readonly
# In another terminal, inspect only; then unmount with the platform's FUSE command.

# Cross-platform, pure-Python read/list/extract with a qnxprobe checkout:
python3 qnxprobe.py --self-test
python3 qnxprobe.py --list --depth 8 image.etfs
python3 qnxprobe.py --extract image-contents.zip image.etfs
```

Neither reader is a native writer. The first needs a suitable Linux FUSE installation; the second reads without a mount but has an ETFS recognition heuristic that may reject legitimate unusual/corrupt variants. Use its ZIP contents as a supplementary comparison, not as an independent native format authority. [R8, R9]

Specific files:

* [qnxmount ETFS Kaitai schema — filetable and generic transaction records][R8a].
* [qnxmount stream reader / replay / metadata-to-path mapping][R8b].
* [qnxmount FUSE implementation][R8c].
* [qnxmount ETFS fixture generator script][R8d].
* [qnxmount ETFS test suite][R8e].
* [qnxmount ETFS fixture directory][R8f] (contains QNX-generated binaries and comparison tarballs; **link** to them, do not vendor without provenance review).
* [qnxprobe entry point and documentation][R9]; [qnxprobe ETFS source discovery via README][R9a].

### 12.2 Related but **not** ETFS oracles

* Linux **QNX4** and **QNX6** filesystem drivers do not read ETFS and cannot qualify it.
* [`dissect.qnxfs`](https://github.com/fox-it/dissect.qnxfs) (AGPL-3.0) targets QNX4/QNX6, not ETFS; **do not incorporate source**.
* [`qnx_dumpers`](https://github.com/jtang613/qnx_dumpers) targets other QNX embedded-image / EFS concerns; not a substitute for a transactional ETFS reader.
* `mkefs` produces EFS, not ETFS; `mkifs` produces IFS, not ETFS; `mkqnx6fs`/`dinit` create other filesystems. All give false-positive confidence if used as ETFS formatters.

### 12.3 Vendor materials

* [QNX architecture: ETFS layout and transaction semantics][R1].
* [QNX 7.1 file limits and NAND/OOB ECC constraints][R2].
* [QNX community ETFS engineering notes][R3].
* [QNX 6.5 `mketfs` manual][R4] (portable 16-byte records; buildfile attributes).
* [QNX 7.1 `mketfs` manual][R5] (newer host tool, includes `mkxfs -t etfs`, per-file attributes).
* [QNX 7.0 `etfsctl` manual][R6] (read/write, erased slots, runtime observability).
* [QNX `fs-etfs-ram` manual][R7] (runtime settings, RAM testing and limitations).
* [QNX SDP 8.0 discontinuation notice][R10].

Avoid treating unlicensed online mirrors of QNX SDK header files as reusable source. A reference parser that points to such a header does **not** grant the right to copy that header into `fsimg`. Use original explanations/independently observed byte facts instead.

## 13. Test matrix and acceptance rules

### 13.1 Image/geometry variants

| Test profile | `C` | Erase block | Endian | Source tool | Required outcomes |
|---|---:|---:|---|---|---|
| `native65-ram1` | 1024 | 16 KiB | LE | QNX6.5 `mketfs`, `fs-etfs-ram -C 0` | Read & extract; writer QNX mount |
| `native65-ram2` | 1024 | 64 KiB | LE | Same | Read, update replay, write |
| `native7-ram` | 1024 | 64 KiB | LE | QNX7.x tools | Byte profile and round trip |
| `nand2k-normalized` | 2048 | Device-native | LE | Native `etfsctl -r` from qualified BSP | Read normalized stream; compare mounted contents |
| `nand4k-normalized` | 4096 | Device-native | LE | Native 7.x build/export | Read normalized stream; compare mounted contents |
| `bigendian-native` | 1024 or 2048 | Qualified | BE | `mketfs +bigendian` + matching guest | Endian handling; no false LE recognition |
| `erased-full-export` | 1024 | Qualified | Qualified | `etfsctl -R` | Ignore actual erased slots; verify current tree |
| `compact-export` | 1024 | Qualified | Qualified | `num_blocks=N,0` / `-r` | Do not infer physical N from output size |
| `nand-oob-boardA` | Native | Native | BSP-specific | Real full OOB dump under rights/permission | **Deferred** until board-specific metadata/ECC proved |

A format-reader compatibility claim requires: one independently created image, one external-tool read of `fsimg`'s logical result, and at least one negative/malformed case for the same geometry. A writer claim requires QNX accepting the image as a **fresh** volume, reading every file/metadata field, then mutating files and remounting/reading again. Keep `etfsctl` output and SHA-256s alongside commands and tool versions.

### 13.2 Live transaction histories

Required state transitions, each saved as a separate `etfsctl -r` and `-R` fixture:

1. Empty formatted volume and empty directory tree; confirm system FIDs/slots.
2. New zero-length file, 1-byte file, `C-1`, `C`, `C+1` and `3*C+17` payloads.
3. Two same-FID writes to exact same logical cluster, including same-size vs different-size overwrites.
4. Partial overwrite in middle of a multi-cluster file, interleaved by writes to another FID.
5. Extending and truncating a file, then rewriting the shorter tail; observe stale clusters beyond EOF.
6. Rename in place and across parents, directory rename, moving directory across depth.
7. Delete then recreate the *same name*; delete and reuse a FID if possible.
8. Rapid repeated metadata changes: `chmod`, `chown`, timestamp changes, symlink creation and removal.
9. Filetable growth beyond one cluster, beyond an erase block, followed by defrag request.
10. Long-name extension creation/deletion/reuse; exact 32, 33 and 91 byte names.
11. Large file with intentional fragmentation (`-c 0` vs larger cache), multiple `nclusters` values.
12. A file written to near media capacity followed by reclaim, spare-block consumption and `.counts`/`.badblks` flush.
13. Mount without unmount, export while stopped, restart/remount, and compare current tree.
14. Atime updates on/off; parent mtime behavior; `-S` checksum mode where supported.
15. Big-endian writer and endian-neutral export.
16. Hard-link creation under the exact installed QNX version; mark unsupported where kernel rejects it.

### 13.3 Illegal/truncated/corruption cases

Corruption test cases should mutate a **single established field or relationship** and record expected `FSIMG_ECORRUPT` / `FSIMG_EUNSUPPORTED` / `FSIMG_EFORMAT` rather than accepting unspecified crashes:

* Wrong `(C+16)` stream stride, last 1–15 bytes truncated, overlarge slice size, offset multiplication overflow.
* Unknown status nibble and high nibble, erased transaction mislabeled live, bad-block code on data.
* FID above max; invalid reserved FID patterns; filetable absent or latest metadata incomplete.
* `file_cluster * C` overflow, `nclusters == 0` where invalid, transaction crossing erase-block boundary in a placement-aware profile.
* Partial/torn multi-cluster transaction, equal sequence conflicting data, sequence wrap ambiguity.
* Corrupt parent ID, missing parent, parent cycle, duplicate names, extension pointing to itself or wrong base, duplicate extension reference.
* Name without terminating NUL at exactly 32/59 bytes; invalid extension last marker; 92-byte name attempts; path exceeds 256.
* Inconsistent filetable size vs number of reachable records; missing FID 1 data; file size exceeds native limit; data reference beyond input.
* Symlink target containing embedded NUL if host extraction policy disallows it; directory stored as file; FIFO with unexpected payload.
* Image with fake `FID=1` fragments but no coherent filetable; a FAT/exFAT/QNX4/QNX6 image with an accidentally divisible length; verify **no false auto-detection**.
* Memory quota, scratch quota, open-descriptor quota, work quota, cancellation, short reads and I/O errors injected during each stage.

**Important:** the expected verdict for an uncertain native status byte is *unsupported* until the semantic rule is established; don't assert all unknown codes are invalid on media for every QNX revision.

## 14. Ruby spec-driven fuzzing integration [P]

The repo's existing `test/spec/` harness (Ruby 3.2+, Minitest, PropCheck and BinData as development-only dependencies) is well suited for ETFS [R11]. Add ETFS rule keys such as:

| Rule ID | Predicate | Witnesses |
|---|---|---|
| `etfs.stream.unit_stride` | Portable records occupy complete `C+16` units | `C=1024,2048,4096`; short unit |
| `etfs.geom.cluster` | Qualified ETFS cluster profile | three accepted sizes; 512, 8192 rejected |
| `etfs.geom.block_relation` | `block_size % cluster_size == 0` | `16k/1k`, `64k/1k`, deliberately fractional |
| `etfs.trans.record_width` | Transaction metadata exactly 16 bytes | one-byte truncation at every offset |
| `etfs.trans.fid_range` | File ID valid or explicitly recognized sentinel | 0,1,5,6,32767,32768,0xffff,0xffffffff |
| `etfs.trans.cluster_span` | Logical extent within bounds | 0,1,0xffff, overflow and end-of-file |
| `etfs.trans.status` | Accepted/corrupt/unknown states distinguished | every nibble 0..15 and high-bit mutations |
| `etfs.trans.replay` | Newest *valid* complete version selected | two/three overlapping writes |
| `etfs.trans.atomic` | Torn multi-cluster write is not partially applied | corruption of each member |
| `etfs.ftable.slot_size` | 64-byte logical records | 1 byte short, cross-cluster boundary |
| `etfs.ftable.system_fids` | Root/FID 1 and reserved topology | absent/duplicate/out of range |
| `etfs.ftable.extension` | Forward/back pointers and marker agree | 32 vs 33 vs 91 byte names; cycle |
| `etfs.names.component` | Names are unique, bounded, unambiguous byte strings | NUL/slash/collision and long names |
| `etfs.graph.parent` | Every live object has a valid acyclic parent chain | self-cycle, 2-cycle, orphan |
| `etfs.file.logical_size` | Payload map covers exactly supported logical contents | sparse/truncate/missing/stale tails |
| `etfs.detect.exclusivity` | ETFS probe does not steal other formats | FAT/exFAT/QNX4/QNX6 collisions |
| `etfs.verify.level` | Integrity claims match available data | generic stream vs raw OOB |
| `etfs.writer.native` | QNX mount/read/mutate/remount passes | small/long names, all entry kinds |

Use a **separate declarative layout model** for raw 16-byte metadata and 64-byte filetable records. The rule builder should generate both valid and targeted-invalid synthetic candidate images and compare their classification with an independently generated QNX fixture. Have C unit tests for pure codec invariants, `fuzz/etfs.c` for raw untrusted bytes and transaction histories, and external integration shell scripts for native `mketfs`/`etfsctl` tests. Tests must not require privileged mounts on ordinary Linux/macOS/FreeBSD CI. The native QNX comparison is a separate explicitly reported qualification gate; never treat a skipped guest test as passing.

## 15. Native reverse-engineering capture protocol for unresolved fields [P]

For each unresolved behavior, create the **smallest possible differential native fixture** instead of guessing:

1. Build or mount an empty QNX ETFS in a disposable VM and export `before.etfs` with both `-r` and `-R`.
2. Perform exactly one operation (e.g., create a 33-byte name, write 1 KB at offset 1 KB, truncate to zero, or create a symlink).
3. Export `after.etfs` from the stopped guest; retain the `etfsctl -i` report and a TAR/archive of the mounted tree with numeric metadata.
4. Decode only generic transaction records and rank `(FID,cluster,sequence)`; produce a machine-readable delta of fields/headers and 64-byte filetable records. Avoid visually diffing entire megabyte volumes.
5. Execute fsimg parser and NFI `qnxmount` on both. Compare metadata, file sizes, and SHA-256 of every regular-file content with the mounted native QNX tree.
6. Power cycle/remount the same disposable guest and re-export if recovery semantics are involved.
7. Record the result in a versioned evidence manifest with tool/guest release, `cluster_size`, `block_size`, `num_blocks`, endian profile, `-C`/`-S` settings, binary SHA-256s, source recipe and tested rule IDs.

Suggested capture tools: `mketfs`, `fs-etfs-ram`, `etfsctl`, QNX `stat`, `find`, `ls -l`, `dd`, `sha256`/`cksum`, `tar`; Python 3 host decoder scripts **authored for the project** (BSD-2-Clause); `qnxmount` and `qnxprobe` only as independent external readers. `fsimg`'s `tools/qnx/vm/` environment and `tools/qnx/guest/` collection scripts are already available, but verify the QNX6.5 installation actually includes ETFS utilities. No hardware programmer or proprietary header is required for the first stage.

## 16. Acceptance gates, project sequencing, and scope control [P]

### Stage E0 — evidence and fixture generation

* Pin QNX6.5/7.x `mketfs` version(s) and both permissive parser revisions.
* Capture empty/simple/overwritten/long-name normalized streams and native TAR metadata.
* Prove exact `C+16` unit offsets and 64-byte filetable fields independently of NFI source.
* Establish whether `+bigendian` changes filetable and record codecs, plus how the normalized export expresses it.
* Inventory known status values and `nclusters` meanings.

**Gate:** a published fixture/evidence table mapping each byte-level rule to one QNX-native example. If this cannot be done, do not advertise full reader support.

### Stage E1 — portable ETFS reader

* Add `etfs` format descriptor with safe **forced format** recognition policy.
* Implement bounded record scanning, FID 1 reconstruction, filetable names/parent tree and basic file contents.
* Support `info`, `ls`, `cat`, `extract` for qualified streams; map existing POSIX kinds.
* Unit and fuzz coverage for resource bounds, truncation, fake headers and graph cycles.

**Gate:** all files and metadata in `qnxmount`'s QNX-generated TAR match; native QNX-generated fixtures also compare, with memory quota and sanitizer tests. Ambiguous replay profiles remain unsupported.

### Stage E2 — transaction-aware full structural verification

* Resolve overlapping multi-cluster updates, truncations and stale records; verify filetable reachability, extension uniqueness and directory DAG.
* Add an integrity evidence-level report, distinguishing structural-only portable exports from ECC-proved physical images.
* Test interrupted/invalid version behavior against controlled native exports; no silent fill or salvage.

**Gate:** `fsimg verify` correctly accepts all qualified native histories and rejects targeted corruption at the declared verdict stage.

### Stage E3 — deterministic portable stream writer

* Keep writer descriptor **unregistered** until native qualification.
* Match exact QNX initial system-FID records, `.counts` sizing, `.badblks` initialization, image padding, sequence and status conventions.
* Generate clean single-version images from bounded source catalogs with no full-image staging.
* Include 1/2/4 KiB cluster geometry and endian profiles only after each is proved.

**Gate:** native QNX `etfsctl -w` accepts writer images; QNX mounts, recursively reads, checks metadata, modifies and remounts the volume, while `fsimg` and third-party parsers independently agree. Then register writer capabilities.

### Stage E4 — raw NAND and forensic recovery (separate project)

* BSP-specific data+OOB decoding for named hardware profiles, including page order, partition offset, bad-block/OOB metadata, CRC and ECC correction.
* Disclose limits for incomplete OOB, missing pages, bad blocks and unqualified controller profiles.
* Optional explicit forensic salvage/deleted-data extraction **must never weaken ordinary `verify` or safe extraction**.

**Gate:** tested hardware/reference dumps with a named BSP, native comparison, bit corruption injection and evidence/coverage report. Do not generalize one OOB format to all NAND devices.

### Relative effort and risk

Reading *clean normalized streams* is modest by the standards of `fsimg`'s QNX4/QNX6 drivers. **Correct historical replay and crash recovery are the main algorithmic risk**; physical raw NAND/OOB is the main hardware/documentation risk; native interoperable creation is gated primarily by initial metadata and sequence/status conventions. Avoid spending weeks on a speculative ECC implementation before the QNX RAM oracle validates the portable writer.

## 17. Specific open questions that must remain explicit

These are not optional niceties; each affects correctness or native writer interoperability:

| ID | Unresolved question | Exact experiment |
|---|---|---|
| Q1 | Trailer byte order under `+bigendian` and `etfsctl` portable normalization? | Same source under `+/-bigendian`, compare decoded 16-byte records |
| Q2 | Exact 16-byte record placement for `mketfs` vs `etfsctl -r/-R`? | Locate FID 1 and known source payload at `C+16` strides |
| Q3 | What constitutes a valid/erased transaction? High-bit status meanings, sentinel FIDs? | Empty erased image, `-R`, `-C 0/1/2`, checksum options |
| Q4 | Multi-cluster `nclusters` tagging and tail rules? | One large native `write()` with cache on/off, compare every header |
| Q5 | Sequence namespace, ties, rollover, relocation? | Interleaved files, updates, reclaim/defrag, compare headers |
| Q6 | Rollback on torn transactions? | Fault-injected RAM/devio or staged log truncation, remount |
| Q7 | Sparse seeks/hole fill and missing cluster meaning? | Native `dd seek` to new file vs to existing |
| Q8 | Filetable unused/deleted slots and extension encoding at 32/91 bytes? | Empty and boundary filename differential fixtures |
| Q9 | `.counts` binary fields and init by `num_blocks`? | `num_blocks=N,0` vs `N,N`, inspect FID 3 before/after `-F` |
| Q10 | `.badblks`, `.reserved` and `.lost+found` bytes on clean/mutated image? | Empty formatted image + native injected error/recovery |
| Q11 | Three timestamps: signedness and native time range? | UTC-boundary and 2038/2106 tests where guest permits |
| Q12 | Hardlinks and link-count representation on 7.0/7.1? | `mketfs -h` vs QNX `ln`, compare FID records |
| Q13 | Metadata update atomicity across filetable clusters? | Rename/move long-name entry whose base/extension lie in different clusters |
| Q14 | CRC representation/coverage in normalized streams? | Differential `-C`, `-S`, and one-bit payload alteration |
| Q15 | Raw NAND OOB layout for a specified BSP? | Obtain permitted full data+spare dump, compare `etfsctl -r`, inspect devio documentation |
| Q16 | Whether 91-byte field contains a terminator or full 91 bytes? | Precisely 90/91-byte native names, inspect record tail |
| Q17 | What data is required for true `verify` on a portable stream? | Compare native intentional CRC failure with normalized export flags |
| Q18 | Auto-detection uniqueness without a prefix marker? | Adversarial fake ETFS headers in other formats, relocate FID 1 far from prefix |

## 18. Conclusion

ETFS support fits `fsimg`'s read/create design, but its **transaction reconstruction** and **portable-vs-NAND distinction** make it more similar to a log-structured filesystem than either currently supported QNX format. The 16-byte generic transaction record, 64-byte filetable entry, fixed system-FID namespace, 32+59-byte name split and QNX `mketfs` / `etfsctl` toolchain give a strong starting point. They are enough for an **experimental clean-stream reader**, not yet enough to assert complete power-failure replay semantics, deep integrity verification or a production-compatible writer.

The highest-value next action is to extend the existing QNX VM fixture tooling to generate a **native-created ETFS image plus a mounted-tree oracle**, then prove `nclusters`, replay and filetable initialization against it. Keep all byte rules, negative tests and provenance pinned. The proper new shared `fsimg` capability is a safe **full-validation forced-open** policy for truly signatureless formats; the remainder should stay inside the ETFS descriptor and driver.

---

## References and source ledger

**Public vendor documentation and engineering behavior**

* <a id="R1"></a>**[R1]** QNX 7.1 System Architecture, [Embedded transaction filesystem](https://qnx.com/developers/docs/7.1/com.qnx.doc.neutrino.sys_arch/topic/fsys_ETFS.html). Source for transaction model, ordering, recovery, media classes and reliability.
* <a id="R2"></a>**[R2]** QNX 7.1 User Guide, [ETFS limits](https://qnx.com/developers/docs/7.1/com.qnx.doc.neutrino.user_guide/topic/limits_ETFS.html). Source for 91-byte names, FIDs, file limits, NAND sizes and spare ECC.
* <a id="R3"></a>**[R3]** QNX Community, [ETFS Design and Implementation Notes](https://community.qnx.com/sf/wiki/do/viewHtml/projects.filesystems/wiki/ETFS). Transaction log, in-memory structure, flash cluster organization, reclaim, badblock and counter system files.
* <a id="R4"></a>**[R4]** QNX 6.5.0 SP1, [`mketfs` manual](https://www.qnx.com/developers/docs/6.5.0SP1.update/com.qnx.doc.neutrino_utilities/m/mketfs.html). Generic 16-byte record, `num_blocks`, byte order, buildfiles and read/write portability.
* <a id="R5"></a>**[R5]** QNX 7.1, [`mketfs` manual](https://qnx.com/developers/docs/7.1/com.qnx.doc.neutrino.utilities/topic/m/mketfs.html). Newer CLI/buildfile, `mkxfs -t etfs` and deterministic input attrs. Also [QNX 7.0 documentation](https://www.get.qnx.com/developers/docs/7.0.0/com.qnx.doc.neutrino.utilities/topic/m/mketfs.html) for `-h` hardlink import.
* <a id="R6"></a>**[R6]** QNX 7.0, [`etfsctl` manual](https://get.qnx.com/developers/docs/7.0.0/com.qnx.doc.neutrino.utilities/topic/e/etfsctl.html). Device split, portable `-r`, all-block `-R`, import/format/erase, stats and safety requirements.
* <a id="R7"></a>**[R7]** QNX 6.5, [`fs-etfs-ram` manual](https://qnx.com/developers/docs/6.5.0SP1.update/com.qnx.doc.neutrino_utilities/f/fs-etfs-ram.html). RAM oracle, cache/checksum modes, POSIX limitations. Also [7.0 version](https://qnx.com/developers/docs/7.0.0/com.qnx.doc.neutrino.utilities/topic/f/fs-etfs-ram.html).
* <a id="R10"></a>**[R10]** QNX 8.0 [filesystem discontinuation notice](https://www.qnx.com/developers/docs/relnotes8.0/com.qnx.doc.discontinuation_notice/topic/filesystems.html), 2026 edition. ETFS + utilities discontinued.

**Permissive open-source implementations, fixtures, and existing fsimg code**

* <a id="R8"></a>**[R8]** Netherlands Forensic Institute, [qnxmount](https://github.com/NetherlandsForensicInstitute/qnxmount) — Apache-2.0, read-only FUSE/parser project.
* <a id="R8a"></a>**[R8a]** [ETFS Kaitai record layout](https://github.com/NetherlandsForensicInstitute/qnxmount/blob/main/qnxmount/etfs/parser.ksy) — supports byte-offset tables above.
* <a id="R8b"></a>**[R8b]** [ETFS parsed transactions and filetable code](https://github.com/NetherlandsForensicInstitute/qnxmount/blob/main/qnxmount/etfs/interface.py) — an external behavioral comparison only.
* <a id="R8c"></a>**[R8c]** [ETFS FUSE adapter](https://github.com/NetherlandsForensicInstitute/qnxmount/blob/main/qnxmount/etfs/mount.py).
* <a id="R8d"></a>**[R8d]** [Native fixture generation script](https://github.com/NetherlandsForensicInstitute/qnxmount/blob/main/tests/qnx_etfs/test_data/make_test_fs.sh) — includes `mketfs`, `fs-etfs-ram`, `etfsctl` and diverse file operations.
* <a id="R8e"></a>**[R8e]** [ETFS TAR comparison tests](https://github.com/NetherlandsForensicInstitute/qnxmount/blob/main/tests/qnx_etfs/test_qnx_etfs.py).
* <a id="R8f"></a>**[R8f]** [ETFS fixture-data directory](https://github.com/NetherlandsForensicInstitute/qnxmount/tree/main/tests/qnx_etfs/test_data), including [`etfs.bld`](https://github.com/NetherlandsForensicInstitute/qnxmount/blob/main/tests/qnx_etfs/test_data/etfs.bld).
* <a id="R9"></a>**[R9]** Alexis Brignoni, [qnxprobe](https://github.com/abrignoni/qnxprobe) — MIT, Python standard-library parser/extractor.
* <a id="R9a"></a>**[R9a]** [qnxprobe README](https://github.com/abrignoni/qnxprobe/blob/main/README.md), support/known fixture and provenance discussion.
* <a id="R11"></a>**[R11]** **Private user repository (access via connected GitHub):** [fsimg README](https://github.com/stass/fsimg/blob/master/README.md), [format contracts](https://github.com/stass/fsimg/blob/master/specs/design-formats.md), [architecture](https://github.com/stass/fsimg/blob/master/specs/architecture.md), [QNX4 driver design](https://github.com/stass/fsimg/blob/master/specs/design-qnx4-driver.md), [QNX6 driver design](https://github.com/stass/fsimg/blob/master/specs/design-qnx6-driver.md), [storage](https://github.com/stass/fsimg/blob/master/specs/design-storage.md), [Ruby spec-testing design](https://github.com/stass/fsimg/blob/master/specs/design-spec-testing.md), [public header](https://github.com/stass/fsimg/blob/master/include/fsimg.h), [QNX VM/test tooling](https://github.com/stass/fsimg/tree/master/tools/qnx), [reference ledger](https://github.com/stass/fsimg/blob/master/docs/references.md), [license/provenance guidelines](https://github.com/stass/fsimg/blob/master/CLAUDE.md).

**License classification:** QNX reference manuals are public behavioral documentation, *not* source code with an open-source reuse grant. QNX vendor programs and any QNX SDK source remain proprietary unless independently licensed otherwise; use only as externally installed tools. `qnxmount` is Apache-2.0 and `qnxprobe` is MIT; permissible to examine/use as independent oracles, but this proposal requires independently authored BSD-2-Clause code and retains no copied parser source. `dissect.qnxfs` is AGPL-3.0 and is not imported; it is not an ETFS oracle.

<!-- Linked evidence shortcuts used throughout the specification. -->
[R1]: https://qnx.com/developers/docs/7.1/com.qnx.doc.neutrino.sys_arch/topic/fsys_ETFS.html
[R2]: https://qnx.com/developers/docs/7.1/com.qnx.doc.neutrino.user_guide/topic/limits_ETFS.html
[R3]: https://community.qnx.com/sf/wiki/do/viewHtml/projects.filesystems/wiki/ETFS
[R4]: https://www.qnx.com/developers/docs/6.5.0SP1.update/com.qnx.doc.neutrino_utilities/m/mketfs.html
[R5]: https://qnx.com/developers/docs/7.1/com.qnx.doc.neutrino.utilities/topic/m/mketfs.html
[R6]: https://get.qnx.com/developers/docs/7.0.0/com.qnx.doc.neutrino.utilities/topic/e/etfsctl.html
[R7]: https://qnx.com/developers/docs/6.5.0SP1.update/com.qnx.doc.neutrino_utilities/f/fs-etfs-ram.html
[R8]: https://github.com/NetherlandsForensicInstitute/qnxmount
[R8a]: https://github.com/NetherlandsForensicInstitute/qnxmount/blob/main/qnxmount/etfs/parser.ksy
[R8b]: https://github.com/NetherlandsForensicInstitute/qnxmount/blob/main/qnxmount/etfs/interface.py
[R8c]: https://github.com/NetherlandsForensicInstitute/qnxmount/blob/main/qnxmount/etfs/mount.py
[R8d]: https://github.com/NetherlandsForensicInstitute/qnxmount/blob/main/tests/qnx_etfs/test_data/make_test_fs.sh
[R8e]: https://github.com/NetherlandsForensicInstitute/qnxmount/blob/main/tests/qnx_etfs/test_qnx_etfs.py
[R8f]: https://github.com/NetherlandsForensicInstitute/qnxmount/tree/main/tests/qnx_etfs/test_data
[R9]: https://github.com/abrignoni/qnxprobe
[R9a]: https://github.com/abrignoni/qnxprobe/blob/main/README.md
[R10]: https://www.qnx.com/developers/docs/relnotes8.0/com.qnx.doc.discontinuation_notice/topic/filesystems.html
[R11]: https://github.com/stass/fsimg/blob/master/README.md
